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
// Env: PT_BLOCK (0|1, default 0), PT_N (records per camera, default 4), PT_HZ (default 30), PT_PATTERN=1|2 (synthetic
// gradient or checkerboard instead of camera frames), PT_DUMP=1 (log record states once a second), PT_RING (ring path).
// No frame content is logged or stored.
#include <openvr.h>

#include <dirent.h>
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

//! the per-frame descriptor the compositor copies (248 bytes). Field meanings are partly guessed (see the doc): known is
//! +8 a double (capture time, s), +32..+44 four floats tested for "set", +48 and +144 3x4 float matrices, +220 a float,
//! +224 a byte. Matrices are identity (head-locked, camera at the head origin) until tuned live.
static void FillDesc(uint8_t *desc, double captureS, int cam)
{
	memset(desc, 0, 248);
	memcpy(desc + 8, &captureS, sizeof(captureS));
	// pinhole-ish normalized intrinsics of the 640x480 image (focal 191 px, centre of the image)
	float k[4] = {191.0f / kW, 191.0f / kH, 0.5f, 0.5f};
	memcpy(desc + 32, k, sizeof(k));
	for (int off : {48, 144}) {
		float m[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
		m[3] = cam ? 0.032f : -0.032f; // half the IPD, a placeholder
		memcpy(desc + off, m, sizeof(m));
	}
	desc[224] = 1;
}

//! The "params" of IVRCameraPassthroughInternal (GetParams/SetParams, vrclient 0x1a49d8/0x1a4bf8) are 5 bytes at state+2: the
//! compositor shows the roomview while state[2] is set (GetParams(nullptr) returns it, vrcompositor 0x15dda0, called per frame at
//! 0x9ee6c) and reads state[4] as the "wanted" byte of its snapshot (+830, compared with the running state +837). On the
//! Steam Frame the driver sets them (passthrough on/off); here the provider does while frames flow.
static void SetShow(uint8_t *state, bool on)
{
	state[2] = on;
	state[4] = on;
}

struct Img
{
	SharedTextureHandle_t h = 0;
	uint8_t *map = nullptr;
	size_t len = 0;
};

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
		if (!rm->NewSharedVulkanImage(vkFormat, w, h, false, false, false, 1, 1, 0, 0, &im.h) || !im.h)
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
	int64_t lastFrame = 0, lastBeat = 0, lastDump = 0;
	int slot[2] = {0, 0};
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
		bool fresh = false;
		double capS = now * 1e-9;
		if (pattern) {
			fresh = true;
			for (int c = 0; c < 2; c++) {
				Img &im = img[c][slot[c]];
				for (int y = 0; y < kH; y++)
					for (int x = 0; x < kW; x++) {
						uint8_t *p = im.map + ((size_t)y * kW + x) * 4;
						if (pattern == 2) { // 32 px checkerboard, a corner marked, to check the memory layout
							uint8_t v = ((x / 32 + y / 32) & 1) ? 255 : 32;
							p[0] = x < 64 && y < 64 ? 255 : v, p[1] = v, p[2] = c ? v : 0;
						} else {
							p[0] = (uint8_t)(x * 255 / kW + (now / 20000000)), p[1] = (uint8_t)(y * 255 / kH);
							p[2] = c ? 255 : 0;
						}
						p[3] = 255;
					}
			}
			lastFrame = now;
		} else if (hdr) {
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
							for (int y = 0; y < kH; y++)
								for (int x = 0; x < kW; x++) {
									uint8_t v = src[(size_t)y * rc.stride + x];
									uint8_t *p = im.map + ((size_t)y * kW + x) * 4;
									p[0] = p[1] = p[2] = v, p[3] = 255;
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
			for (int c = 0; c < 2; c++) {
				uint8_t *rec = b + kRec0 + c * kCamStride + slot[c] * kRecStride;
				FillDesc(rec + 4, capS, c);
				__atomic_store_n((uint32_t *)rec, 1u, __ATOMIC_RELEASE); // frame ready
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
			printf("ptprovider: records free %d ready %d taken %d, enabled %d\n", cnt[0], cnt[1], cnt[2], b[4]);
			fflush(stdout);
		}
		next += period;
		int64_t wait = next - NowNs();
		if (wait > 0)
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
