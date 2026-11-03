// Frame-style passthrough provider (docs/passthrough.md, "Route (a) design"): feeds SteamVR's own roomview
// (vrcompositor CTrackedCamera) from the camera ring written by libq1camtap, through the shared state
// VR_CameraPassthroughState. A separate OpenVR client (Overlay type, so that it has the compositor connection the
// resource manager needs); started only while SteamVR runs.
//
// Layout (reversed from vrcompositor 0x15f310/0x15f048 and vrclient CVRCameraPassthroughInternal):
//   state (0x6200 bytes): two camera-type blocks at +0x14 and +0x20e8 (0x20d4 bytes each). In a block b:
//     b+0 u32 generation, b+4 u8 "type enabled", b+8 u32 record count N, b+0x40 u8 "ready",
//     camera c (0 = left, 1 = right): records at b + 0x44 + c*0x1040 + k*0x104 {u32 state; u8 desc[248]; u64 sharedHandle},
//     b+0x20c4 + 8*c: u64 spline-distortion shared handle.
//   record state: 0 free, 1 frame ready (provider), 2 taken by the compositor (it frees older ones).
//   The frame images are shared Vulkan images from IVRIPCResourceManagerClient::NewSharedVulkanImage (the blob has no
//   dma-buf import, but its opaque fd is a dma-buf: ReceiveSharedFd + mmap gives CPU access; the pixel layout of that
//   memory (tiling) still has to be confirmed live).
// The roomview data is enabled (block byte +4) only once a frame is in place, and disabled again on exit or when the
// camera ring goes stale: never leave it enabled without frames (vrcompositor's watchdog aborts).
//
// Pixels: vrcompositor's roomview fragment shader reads the frame as YUV in an RGBA8 image (R = Cr, G = Y, B = Cb, chroma
// centred on 0.5019) and converts it to linear RGB; the memory is NV12 (see WriteYuv).
//
// Env: PT_BLOCK (0|1, default 0), PT_N (records per camera, default 4), PT_HZ (default 30), PT_PATTERN=1|2 (synthetic
// gradient or checkerboard instead of camera frames), PT_DUMP=1 (log record states once a second), PT_RING (ring path).
// No frame content is logged or stored.
#include <openvr.h>

#include <dirent.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../steamvr-quest1/q1json.h"
#include "../steamvr-quest1/quest1_camera_ring.h"

using namespace vr;

static const size_t kStateSize = 0x6200;
static const size_t kBlock[2] = {0x14, 0x20e8};
static const size_t kRecStride = 0x104, kCamStride = 0x1040, kRec0 = 0x44, kSpline = 0x20c4;
static const int kW = 640, kH = 480; // camera image size
static volatile sig_atomic_t g_quit;

static int64_t NowNs()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void OnSig(int) { g_quit = 1; }

static bool Running(const char *comm)
{
	bool found = false;
	if (DIR *d = opendir("/proc")) {
		while (dirent *de = readdir(d)) {
			char path[64], name[32] = "";
			snprintf(path, sizeof(path), "/proc/%s/comm", de->d_name);
			if (FILE *f = fopen(path, "r")) {
				if (fscanf(f, "%31s", name) == 1 && !strcmp(name, comm))
					found = true;
				fclose(f);
			}
			if (found)
				break;
		}
		closedir(d);
	}
	return found;
}

static uint8_t *MapState()
{
	DIR *d = opendir("/dev/shm");
	if (!d)
		return nullptr;
	uint8_t *out = nullptr;
	while (dirent *de = readdir(d)) {
		if (!strstr(de->d_name, "-Shm_"))
			continue;
		std::string p = std::string("/dev/shm/") + de->d_name;
		struct stat st;
		if (stat(p.c_str(), &st) || (size_t)st.st_size != kStateSize)
			continue;
		int fd = open(p.c_str(), O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		void *m = mmap(nullptr, kStateSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		close(fd);
		if (m != MAP_FAILED) {
			out = (uint8_t *)m;
			printf("ptprovider: state %s\n", p.c_str());
			break;
		}
	}
	closedir(d);
	return out;
}

//! A camera's factory calibration (camera_calibration_v2.json, Meta's Fisheye62 model on the raw 640x480 image): focal
//! length and principal point in pixels, the radial coefficients of theta^2..theta^12 (the roomview shader takes the
//! first four: Kannala-Brandt), DeviceFromCamera (OpenCV camera axes in the device frame).
struct Calib
{
	bool ok = false;
	double f = 191, cx = 319.5, cy = 239.5;
	double k[6] = {0.29, 0, -0.15, 0.09, -0.02, 0};
	double m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

static Calib LoadCalib(int id)
{
	Calib c;
	const char *paths[] = {getenv("PT_CALIB"), "/persist/calibration/camera_calibration_v2.json",
	                       "/run/q1track/calibration/camera_calibration_v2.json"};
	q1json::Json root;
	for (const char *path : paths)
		if (path && q1json::ParseJsonFile(path, root))
			break;
	const q1json::Json *list = root.Get("CameraCalibration");
	if (!list || list->type != q1json::Json::Arr)
		return c;
	for (const q1json::Json &e : list->arr) {
		const q1json::Json *jid = e.Get("Id");
		int eid = !jid ? -1 : jid->type == q1json::Json::Num ? (int)jid->num : jid->type == q1json::Json::Str ? atoi(jid->str.c_str()) : -1;
		if (eid != id)
			continue;
		const q1json::Json *pj = e.Get("Projection"), *dj = e.Get("Distortion"), *dfc = e.Get("DeviceFromCamera");
		const q1json::Json *pc = pj ? pj->Get("Coefficients") : nullptr, *dc = dj ? dj->Get("Coefficients") : nullptr;
		double proj[3], dist[8];
		if (pc && dc && dfc && pc->Numbers(proj, 3) && dc->Numbers(dist, 8) && dfc->Numbers(c.m, 16)) {
			c.f = proj[0], c.cx = proj[1], c.cy = proj[2];
			for (int i = 0; i < 6; i++)
				c.k[i] = dist[i];
			c.ok = true;
		}
		break;
	}
	return c;
}

//! The per-frame record descriptor (248 bytes) vrcompositor copies (0x15f4bc) into its camera state (0x15f748..0x15f8b4):
//!   +8 double capture time (s); +16..+28 float fx, fy, cx, cy (pixels: the shader divides by the block's image size);
//!   +32..+44 float k1..k4 (Kannala-Brandt fisheye; all zero = pinhole); +224 u8 "spline" (adds the spline distortion
//!   image, model 2); +228.. spline scale/offset; +48, +96, +144 three 3x4 float matrices (poses, below).
static void FillDesc(uint8_t *desc, double captureS, const Calib &cal, const HmdMatrix34_t &head)
{
	memset(desc, 0, 248);
	memcpy(desc + 8, &captureS, sizeof(captureS));
	float in[8] = {(float)cal.f, (float)cal.f, (float)(cal.cx + 0.5), (float)(cal.cy + 0.5),
	               (float)cal.k[0], (float)cal.k[1], (float)cal.k[2], (float)cal.k[3]};
	memcpy(desc + 16, in, sizeof(in));
	// +48: the camera's pose, Meta's DeviceFromCamera (OpenCV camera axes in the head frame): vrcompositor (0x15f660) inverts it
	// into the shader's _m3, which projects head-space points (the depth mesh's frame) into the camera.
	// +96: the same pose: vrcompositor (0x160a80) draws each eye with the camera nearest to it, comparing these with the eye
	// positions (when params byte 1, state[3], is set: SetShow).
	// +144: the head pose at capture (raw tracking space, like the depth mesh)
	float m1[12];
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 4; c++)
			m1[r * 4 + c] = (float)cal.m[r * 4 + c];
	memcpy(desc + 48, m1, sizeof(m1));
	memcpy(desc + 96, m1, sizeof(m1));
	memcpy(desc + 144, &head.m[0][0], sizeof(head.m));
	// +204..+216: the valid image area in texture coordinates (u min, v min, u max, v max; the fragment shader's bounds,
	// vrcompositor 0x15f754 -> uniforms _m15/_m16): outside it the roomview is black.
	// Unbounded: where the bottom cameras do not see (above the horizon), the image's edge is stretched (the sampler clamps
	// to the edge) instead of black, as Meta's passthrough does
	const float bounds[4] = {-100, -100, 101, 101};
	memcpy(desc + 204, bounds, sizeof(bounds));
}

//! The "params" of IVRCameraPassthroughInternal (GetParams/SetParams, vrclient 0x1a49d8/0x1a4bf8) are 5 bytes at state+2: the
//! compositor shows the roomview while state[2] is set (GetParams(nullptr) returns it, vrcompositor 0x15dda0, called per frame at
//! 0x9ee6c) and reads state[4] as the "wanted" byte of its snapshot (+830, compared with the running state +837). On the
//! Steam Frame the driver sets them (passthrough on/off); here the provider does while frames flow. state[3] (snapshot +829)
//! gives each eye its own camera in the 3D roomview (0x161b98: without it both eyes take the left eye's camera).
static void SetShow(uint8_t *state, bool on)
{
	state[2] = on;
	state[3] = on;
	state[4] = on;
}

struct Img
{
	SharedTextureHandle_t h = 0;
	uint8_t *map = nullptr;
	size_t len = 0;
};

//! The frame images as vrcompositor's roomview shader reads them: RGBA8 holding Y'CbCr (R = Cr, G = Y, B = Cb), linear
//! rows of kW * 4 bytes (the compat layer makes these images linear; its log gives the row pitch). The gray cameras get
//! neutral chroma; `red` marks pattern areas.
template <class Luma, class Red>
static void WriteYuv(Img &im, Luma luma, Red red)
{
	for (int j = 0; j < kH; j++)
		for (int i = 0; i < kW; i++) {
			uint8_t *p = im.map + ((size_t)j * kW + i) * 4;
			bool r = red(i, j);
			p[0] = r ? 240 : 128, p[1] = luma(i, j), p[2] = r ? 90 : 128, p[3] = 255;
		}
}

int main()
{
	signal(SIGTERM, OnSig);
	signal(SIGINT, OnSig);
	const int blk = getenv("PT_BLOCK") ? atoi(getenv("PT_BLOCK")) & 1 : 0;
	const int N = getenv("PT_N") ? std::max(2, std::min(16, atoi(getenv("PT_N")))) : 4;
	const int hz = getenv("PT_HZ") ? std::max(1, atoi(getenv("PT_HZ"))) : 30;
	const int pattern = getenv("PT_PATTERN") ? atoi(getenv("PT_PATTERN")) : 0; // 1 gradient, 2 checkerboard
	const bool dump = getenv("PT_DUMP") != nullptr;
	const char *ringPath = getenv("PT_RING") ? getenv("PT_RING") : Q1CAM_RING_DEFAULT_PATH;
	const Calib calib[2] = {LoadCalib(0), LoadCalib(2)}; // the bottom pair (left, right)
	// The sensors' values are linear in light; the roomview shader decodes its Y' as sRGB (full-range BT.601, then the sRGB
	// curve): encode them so that the light reaches the display linear, as Meta's passthrough does (its gamma steps cancel out)
	uint8_t toSrgb[256];
	for (int v = 0; v < 256; v++) {
		double l = v / 255.0, e = l <= 0.0031308 ? 12.92 * l : 1.055 * pow(l, 1 / 2.4) - 0.055;
		toSrgb[v] = (uint8_t)std::lround(std::min(1.0, e) * 255);
	}
	uint32_t grayPx[256]; // the frame image's RGBA8 pixel (R = Cr, G = Y', B = Cb, A) for each sensor value: neutral chroma
	for (int v = 0; v < 256; v++)
		grayPx[v] = 0xff000000u | 128u << 16 | (uint32_t)toSrgb[v] << 8 | 128u;
	for (int c = 0; c < 2; c++)
		printf("ptprovider: camera %d %s: f %.1f centre %.1f %.1f k %.3f %.3f %.3f %.3f\n", c ? 2 : 0,
		       calib[c].ok ? "calibrated" : "NOT calibrated (defaults)", calib[c].f, calib[c].cx, calib[c].cy, calib[c].k[0],
		       calib[c].k[1], calib[c].k[2], calib[c].k[3]);

	// an Overlay-type connection starts vrserver and the dashboard when SteamVR is down: wait for it
	while (!g_quit && !(Running("vrserver") && Running("vrcompositor")))
		sleep(3);
	if (g_quit)
		return 0;
	sleep(10);
	EVRInitError err = VRInitError_None;
	VR_Init(&err, VRApplication_Overlay);
	if (err != VRInitError_None) {
		fprintf(stderr, "ptprovider: VR_Init: %s\n", VR_GetVRInitErrorAsEnglishDescription(err));
		return 1;
	}
	EVRInitError e2;
	auto *rm = (IVRIPCResourceManagerClient *)VR_GetGenericInterface(IVRIPCResourceManagerClient_Version, &e2);
	if (!rm) {
		fprintf(stderr, "ptprovider: no resource manager (%d)\n", (int)e2);
		return 1;
	}
	uint8_t *st = nullptr;
	for (int i = 0; i < 60 && !st && !g_quit; i++) {
		st = MapState();
		if (!st)
			sleep(1);
	}
	if (!st) {
		fprintf(stderr, "ptprovider: VR_CameraPassthroughState not found\n");
		return 1;
	}
	uint8_t *b = st + kBlock[blk];

	// frame images: N per camera (left = block camera 0, right = camera 1), plus one spline image per camera
	std::vector<Img> img[2];
	Img spline[2];
	auto make = [&](Img &im, uint32_t w, uint32_t h, uint32_t vkFormat) -> bool {
		// 0x80 (input attachment): the compat layer's mark for linear tiling (the frames are written by the CPU; optimal
		// RGBA8 images are UBWC-compressed on the Adreno 540)
		if (!rm->NewSharedVulkanImage(vkFormat, w, h, false, false, false, 1, 1, 0, vkFormat == 37 ? 0x80 : 0, &im.h) || !im.h)
			return false;
		uint64_t ipc = 0;
		int fd = -1;
		if (!rm->RefResource(im.h, &ipc) || !rm->ReceiveSharedFd(ipc, &fd))
			return false;
		im.len = (size_t)w * h * 4;
		void *m = mmap(nullptr, im.len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		close(fd);
		if (m == MAP_FAILED)
			return false;
		im.map = (uint8_t *)m;
		return true;
	};
	for (int c = 0; c < 2; c++) {
		img[c].resize(N);
		for (Img &im : img[c])
			if (!make(im, kW, kH, 37)) {
				fprintf(stderr, "ptprovider: frame image allocation failed\n");
				return 1;
			}
		if (!make(spline[c], 64, 1, 109 /* VK_FORMAT_R32G32B32A32_SFLOAT */) && !make(spline[c], 64, 1, 37)) {
			fprintf(stderr, "ptprovider: spline image allocation failed\n");
			return 1;
		}
		// spline distortion: identity mapping placeholder (format to be confirmed live)
		float *f = (float *)spline[c].map;
		for (int i = 0; i < 64 && (size_t)(i + 1) * 16 <= spline[c].len; i++)
			f[i * 4 + 0] = i / 63.0f, f[i * 4 + 1] = i / 63.0f, f[i * 4 + 2] = 0, f[i * 4 + 3] = 1;
	}

	// publish the pool (everything except the enabled byte), then frames, then enable
	uint32_t zero = 0, nrec = (uint32_t)N, gen = 1;
	memcpy(b + 8, &nrec, 4);
	const uint32_t size[2] = {kW, kH}; // the image size the shader divides the projected pixel by (vrcompositor 0x15f504)
	memcpy(b + 12, size, sizeof(size));
	for (int c = 0; c < 2; c++) {
		for (int k = 0; k < N; k++) {
			uint8_t *rec = b + kRec0 + c * kCamStride + k * kRecStride;
			memcpy(rec, &zero, 4);
			memcpy(rec + 4 + 248, &img[c][k].h, 8);
		}
		memcpy(b + kSpline + 8 * c, &spline[c].h, 8);
	}
	memcpy(b, &gen, 4);
	b[0x40] = 1;
	printf("ptprovider: pool ready, block %d, %d images/camera\n", blk, N);
	fflush(stdout);

	const q1cam_ring_header *hdr = nullptr;
	uint32_t last = 0;
	int64_t lastFrame = 0, lastBeat = 0, lastDump = 0, lastIdle = 0;
	int slot[2] = {0, 0};
	auto Rec = [&](int c, int k) { return (uint32_t *)(b + kRec0 + c * kCamStride + k * kRecStride); };
	const int64_t period = 1000000000LL / hz;
	int64_t next = NowNs();
	while (!g_quit) {
		int64_t now = NowNs();
		if (now - lastBeat > 1000000000LL) { // keep the producer (libq1camtap) awake
			lastBeat = now;
			std::string p = std::string(ringPath) + ".want";
			int fd = open(p.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0666);
			if (fd >= 0) {
				futimens(fd, nullptr);
				close(fd);
			}
		}
		if (!pattern && !hdr) {
			int fd = open(ringPath, O_RDONLY | O_CLOEXEC);
			struct stat s2;
			if (fd >= 0 && !fstat(fd, &s2) && s2.st_size >= (off_t)sizeof(q1cam_ring_header)) {
				void *m = mmap(nullptr, s2.st_size, PROT_READ, MAP_SHARED, fd, 0);
				if (m != MAP_FAILED) {
					const q1cam_ring_header *h = (const q1cam_ring_header *)m;
					if (h->magic == Q1CAM_RING_MAGIC && h->version == Q1CAM_RING_VERSION)
						hdr = h;
					else
						munmap(m, s2.st_size);
				}
			}
			if (fd >= 0)
				close(fd);
		}
		// The records to write this frame into: free ones (the compositor takes them while it shows the roomview). When none is
		// free it is not reading (roomview off): take a ready one back, but only twice a second (those frames are not shown,
		// they only must not be stale when it reads again). A record it has taken (2) is left alone.
		bool canWrite;
		{
			int target[2] = {-1, -1};
			for (int c = 0; c < 2; c++)
				for (int i = 0; i < N && target[c] < 0; i++)
					if (__atomic_load_n(Rec(c, (slot[c] + i) % N), __ATOMIC_ACQUIRE) == 0)
						target[c] = (slot[c] + i) % N;
			if ((target[0] < 0 || target[1] < 0) && now - lastIdle >= 500000000LL) {
				lastIdle = now;
				for (int c = 0; c < 2; c++)
					for (int i = 0; i < N && target[c] < 0; i++) {
						uint32_t ready = 1;
						if (__atomic_compare_exchange_n(Rec(c, (slot[c] + i) % N), &ready, 0u, false, __ATOMIC_ACQ_REL,
						                                __ATOMIC_ACQUIRE))
							target[c] = (slot[c] + i) % N;
					}
			}
			canWrite = target[0] >= 0 && target[1] >= 0;
			if (canWrite)
				slot[0] = target[0], slot[1] = target[1];
		}
		bool fresh = false;
		double capS = now * 1e-9;
		if (canWrite && pattern) {
			fresh = true;
			for (int c = 0; c < 2; c++) {
				Img &im = img[c][slot[c]];
				if (pattern >= 3) { // the whole buffer filled with one RGBA value (3: 255,0,0,255; 4: 0,0,255,255)
					for (size_t o = 0; o + 4 <= im.len; o += 4)
						im.map[o] = pattern == 3 ? 255 : 0, im.map[o + 1] = 0, im.map[o + 2] = pattern == 3 ? 0 : 255,
						im.map[o + 3] = 255;
				} else if (pattern == 2) // 32 px checkerboard, the top left corner red, to check the layout
					WriteYuv(im, [](int x, int y) { return (uint8_t)(((x / 32 + y / 32) & 1) ? 235 : 40); },
					          [](int x, int y) { return x < 64 && y < 64; });
				else
					WriteYuv(im, [&](int x, int) { return (uint8_t)(x * 255 / kW + (now / 20000000)); },
					          [](int, int) { return false; });
			}
			lastFrame = now;
		} else if (canWrite && hdr) {
			uint32_t n = __atomic_load_n(&hdr->write_count, __ATOMIC_ACQUIRE);
			if (n && n != last) {
				last = n;
				uint32_t si = (n - 1) % hdr->slot_count;
				const q1cam_ring_slot &s = hdr->slot[si];
				uint32_t seq0 = __atomic_load_n(&s.seq, __ATOMIC_ACQUIRE);
				if (!(seq0 & 1) && (s.frame_type == Q1CAM_FRAME_HEADSET || s.frame_type == 0)) {
					const uint8_t *base = (const uint8_t *)hdr + hdr->header_size + (size_t)si * hdr->slot_bytes;
					const int ids[2] = {0, 2};
					int got = 0;
					for (int c = 0; c < 2; c++)
						for (uint32_t ci = 0; ci < s.ncams && ci < Q1CAM_MAX_CAMS; ci++) {
							const q1cam_ring_cam &rc = s.cam[ci];
							if (!rc.present || (int)rc.cam_id != ids[c] || rc.width != (uint32_t)kW || rc.height != (uint32_t)kH)
								continue;
							const uint8_t *src = base + rc.offset;
							Img &im = img[c][slot[c]];
							for (int y = 0; y < kH; y++) {
								const uint8_t *row = src + (size_t)y * rc.stride;
								uint32_t *out = (uint32_t *)im.map + (size_t)y * kW;
								for (int x = 0; x < kW; x++)
									out[x] = grayPx[row[x]];
							}
							got++;
						}
					if (got == 2 && __atomic_load_n(&s.seq, __ATOMIC_ACQUIRE) == seq0) {
						fresh = true;
						lastFrame = now;
						capS = (s.capture_mono_ns ? s.capture_mono_ns : s.arrival_mono_ns) * 1e-9;
					}
				}
			}
		}
		if (fresh) {
			// the head pose at capture (+144: vrcompositor reprojects the frame from it to the head pose it displays with):
			// the frames reach us 25-35 ms after their mid-exposure, and a pose of the wrong time shakes the view as the head
			// turns. SteamVR extrapolates back from the latest pose.
			TrackedDevicePose_t hp = {};
			VRSystem()->GetDeviceToAbsoluteTrackingPose(TrackingUniverseRawAndUncalibrated, (float)(capS - NowNs() * 1e-9),
			                                            &hp, 1);
			HmdMatrix34_t head = hp.bPoseIsValid ? hp.mDeviceToAbsoluteTracking
			                                     : HmdMatrix34_t{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
			for (int c = 0; c < 2; c++) {
				FillDesc((uint8_t *)Rec(c, slot[c]) + 4, capS, calib[c], head);
				__atomic_store_n(Rec(c, slot[c]), 1u, __ATOMIC_RELEASE); // frame ready
				slot[c] = (slot[c] + 1) % N;
			}
			if (!b[4]) {
				b[4] = 1; // enable only now that a frame is in place
				SetShow(st, true);
				printf("ptprovider: roomview data enabled\n");
				fflush(stdout);
			}
		}
		// no frame for 2 s: disable so that the compositor does not wait for frames
		if (b[4] && now - lastFrame > 2000000000LL) {
			b[4] = 0;
			SetShow(st, false);
			printf("ptprovider: no frames for 2 s, disabled\n");
			fflush(stdout);
		}
		if (dump && now - lastDump > 1000000000LL) {
			lastDump = now;
			int cnt[3] = {0, 0, 0};
			for (int c = 0; c < 2; c++)
				for (int k = 0; k < N; k++)
					cnt[std::min<uint32_t>(2, *(uint32_t *)(b + kRec0 + c * kCamStride + k * kRecStride))]++;
			TrackedDevicePose_t hp = {};
			VRSystem()->GetDeviceToAbsoluteTrackingPose(TrackingUniverseRawAndUncalibrated, 0, &hp, 1);
			printf("ptprovider: records free %d ready %d taken %d, enabled %d, head (raw) %.2f %.2f %.2f%s\n", cnt[0], cnt[1],
			       cnt[2], b[4], hp.mDeviceToAbsoluteTracking.m[0][3], hp.mDeviceToAbsoluteTracking.m[1][3],
			       hp.mDeviceToAbsoluteTracking.m[2][3], hp.bPoseIsValid ? "" : " (invalid)");
			fflush(stdout);
		}
		next += period;
		int64_t wait = next - NowNs();
		if (hdr && !pattern && canWrite) {
			// wake as soon as the producer completes a frame set (its futex word), at the latest after a period
			if (__atomic_load_n(&hdr->write_count, __ATOMIC_ACQUIRE) == last) {
				timespec ts = {0, (long)std::clamp<int64_t>(wait, 1000000, period)};
				syscall(SYS_futex, (void *)&hdr->write_count, FUTEX_WAIT, last, &ts, nullptr, 0);
			}
			next = NowNs();
		} else if (wait > 0)
			usleep((useconds_t)(wait / 1000));
		else
			next = NowNs();
	}
	b[4] = 0; // never leave the roomview enabled without a provider
	SetShow(st, false);
	b[0x40] = 0;
	for (int c = 0; c < 2; c++)
		for (int k = 0; k < N; k++)
			*(uint32_t *)(b + kRec0 + c * kCamStride + k * kRecStride) = 0;
	VR_Shutdown();
	return 0;
}
