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
	double p[2] = {0, 0};                  // Fisheye62 tangential
	double kb[4] = {0.29, 0, -0.15, 0.09}; // the four coefficients the roomview shader takes, fitted to all six (FitKB)
	double m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

//! The roomview shader's fisheye model is Kannala-Brandt with four radial terms (theta^3..theta^9); Meta's Fisheye62 has six
//! (to theta^13). Dropping the last two is not small off-axis: 16 px at 60 deg and 82 px at 70 deg on camera 0 (5 and 27 on
//! camera 2), and the bottom cameras look 40 deg down, so the view ahead and above sits there: one eye's image was lower than
//! the other's. Least-squares fit of the four over theta in [0, 1.4] instead (under 0.4 px).
static void FitKB(const double k[6], double kb[4])
{
	double ata[4][5] = {};
	for (int n = 0; n <= 400; n++) {
		const double th = 1.4 * n / 400, t2 = th * th;
		double full = 0, p = th, a[4];
		for (int i = 0; i < 6; i++) {
			p *= t2;
			full += k[i] * p;
			if (i < 4)
				a[i] = p;
		}
		for (int r = 0; r < 4; r++) {
			for (int c = 0; c < 4; c++)
				ata[r][c] += a[r] * a[c];
			ata[r][4] += a[r] * full;
		}
	}
	for (int c = 0; c < 4; c++) { // Gauss-Jordan, partial pivoting
		int piv = c;
		for (int r = c + 1; r < 4; r++)
			if (fabs(ata[r][c]) > fabs(ata[piv][c]))
				piv = r;
		for (int j = 0; j < 5; j++)
			std::swap(ata[c][j], ata[piv][j]);
		for (int r = 0; r < 4; r++)
			if (r != c) {
				const double m = ata[r][c] / ata[c][c];
				for (int j = c; j < 5; j++)
					ata[r][j] -= m * ata[c][j];
			}
	}
	for (int i = 0; i < 4; i++)
		kb[i] = ata[i][4] / ata[i][i];
}

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
			c.p[0] = dist[6], c.p[1] = dist[7];
			FitKB(c.k, c.kb);
			c.ok = true;
		}
		break;
	}
	return c;
}

//! Fisheye62 projection of a camera-frame direction (OpenCV axes) to a raw pixel. False behind or beyond ~92 degrees.
static bool Project62(const Calib &c, const double d[3], double &px, double &py)
{
	double r = sqrt(d[0] * d[0] + d[1] * d[1]);
	double th = atan2(r, d[2]);
	if (th > 1.6)
		return false;
	double t2 = th * th, poly = 1, tp = 1;
	for (int i = 0; i < 6; i++)
		tp *= t2, poly += c.k[i] * tp;
	double thd = th * poly;
	double u = r > 1e-12 ? thd * d[0] / r : 0, v = r > 1e-12 ? thd * d[1] / r : 0;
	double r2 = u * u + v * v;
	double ut = u + 2 * c.p[0] * u * v + c.p[1] * (r2 + 2 * u * u);
	double vt = v + 2 * c.p[1] * u * v + c.p[0] * (r2 + 2 * v * v);
	px = c.f * ut + c.cx, py = c.f * vt + c.cy;
	return true;
}

/*
 * The composite camera. vrcompositor's roomview takes one camera image per eye, and the bottom cameras look ~40 degrees
 * down: everything above their field of view (half of the view ahead, user 2026-11-08: "the whole top half stretched")
 * was the edge of their image, clamped and stretched. Meta covers it with the top cameras. So each eye gets a virtual
 * camera at its bottom camera's position, looking straight ahead (equidistant fisheye, f = kVirtF, ~140 x 104 degrees),
 * whose image is filled from the bottom and the top camera of that side through a table built once: each pixel's ray
 * meets the roomview's sphere (kSphereM, the driver's mesh radius) and that point is looked up in both cameras (bilinear),
 * blended over the band where both see it. The bottom camera's part does not depend on the depth (the virtual camera
 * sits at its centre); the top camera's does, through its ~6 cm offset. The top camera's gain follows the bottom one's
 * over the shared band (the two expose separately).
 */
static const double kSphereM = 3.0;
//! The rendered part of the virtual image: within these angles of straight ahead (the panels show ~50 degrees each way;
//! the rest is margin for the head's turn between capture and display). The other pixels stay black.
static const double kMaxAzDeg = 70, kMaxElDeg = 62;

static Calib VirtualCamera(const Calib &bottom)
{
	Calib v;
	v.ok = bottom.ok;
	// the bottom camera's own pixel density (its f): a finer virtual image only interpolates (266 px/rad was 1.4 times
	// the cameras' and covered 52 degrees up and down: the bottom of the view was the image's stretched edge)
	v.f = bottom.f, v.cx = (kW - 1) / 2.0, v.cy = (kH - 1) / 2.0;
	for (double &k : v.k)
		k = 0;
	v.kb[0] = 1e-6, v.kb[1] = v.kb[2] = v.kb[3] = 0; // equidistant; all four zero would make the shader a pinhole
	// DeviceFromCamera: OpenCV axes (x right, y down, z forward) = device (x, -y, -z), at the bottom camera's centre
	const double m[16] = {1, 0, 0, bottom.m[3], 0, -1, 0, bottom.m[7], 0, 0, -1, bottom.m[11], 0, 0, 0, 1};
	memcpy(v.m, m, sizeof(m));
	return v;
}

//! A bilinear sample of a raw camera image (stride kW): its 2x2's top-left pixel and the four weights (sum 256).
struct Tap
{
	uint32_t off;
	uint8_t w[4]; // (x, y), (x + 1, y), (x, y + 1), (x + 1, y + 1); w[0] is 256 - the others, may wrap to 0 for 256
	uint16_t w0;
	uint16_t pad;
};
struct OneTap
{
	uint32_t out;
	Tap t;
};
struct TwoTaps
{
	uint32_t out;
	Tap b, t;
	uint16_t wTop; // the top camera's share, 0..256
	uint16_t pad;
};
//! One eye's composite, as three lists (no per-pixel branch): pixels from the bottom camera only, the top only, both.
struct Composite
{
	std::vector<OneTap> bottom, top;
	std::vector<TwoTaps> both;
	size_t empty = 0; //!< pixels in the window that no camera sees (black)
};

static inline int Sample(const uint8_t *img, const Tap &t)
{
	const uint8_t *p = img + t.off;
	return (p[0] * t.w0 + p[1] * t.w[1] + p[kW] * t.w[2] + p[kW + 1] * t.w[3] + 128) >> 8;
}

/*
 * The composite camera. vrcompositor's roomview takes one camera image per eye, and the bottom cameras look ~40 degrees
 * down: everything above their field of view (half of the view ahead, user 2026-11-08: "the whole top half stretched")
 * was the edge of their image, clamped and stretched. Meta covers it with the top cameras. So each eye gets a virtual
 * camera at its bottom camera's position, looking straight ahead (equidistant fisheye, the bottom camera's f), whose image
 * is filled from the bottom and the top camera of that side through tables built once: each pixel's ray meets the
 * roomview's sphere (kSphereM, the driver's mesh radius) and that point is looked up in both cameras (bilinear), blended
 * over the band where both see it. The bottom camera's part does not depend on the depth (the virtual camera sits at its
 * centre); the top camera's does, through its ~6 cm offset. The top camera's gain follows the bottom one's over the shared
 * band (the two expose separately). Cost (composite_bench, big core): 8.6 ms an eye with one table entry per pixel and
 * a branch per pixel at f = 266, 2026-11-08 morning; now see docs/passthrough.md.
 */
static Composite BuildComposite(const Calib &v, const Calib cam[2])
{
	Composite out;
	const double feather = 40; // px from a camera's image edge over which its weight rises
	const double deg = 180 / M_PI;
	for (int y = 0; y < kH; y++)
		for (int x = 0; x < kW; x++) {
			double u = (x - v.cx) / v.f, w = (y - v.cy) / v.f, th = sqrt(u * u + w * w);
			double dc[3] = {th > 1e-9 ? sin(th) * u / th : 0, th > 1e-9 ? sin(th) * w / th : 0, cos(th)};
			double d[3], o[3] = {v.m[3], v.m[7], v.m[11]};
			for (int r = 0; r < 3; r++)
				d[r] = v.m[r * 4] * dc[0] + v.m[r * 4 + 1] * dc[1] + v.m[r * 4 + 2] * dc[2];
			if (fabs(atan2(d[0], -d[2])) * deg > kMaxAzDeg || fabs(asin(std::max(-1.0, std::min(1.0, d[1])))) * deg > kMaxElDeg ||
			    th > M_PI / 2)
				continue; // never on the panels: stays black
			// |o + s d| = R, s > 0
			double bb = o[0] * d[0] + o[1] * d[1] + o[2] * d[2], cc = o[0] * o[0] + o[1] * o[1] + o[2] * o[2] - kSphereM * kSphereM;
			double sdist = -bb + sqrt(std::max(0.0, bb * bb - cc));
			double P[3] = {o[0] + sdist * d[0], o[1] + sdist * d[1], o[2] + sdist * d[2]};
			Tap tap[2];
			bool have[2] = {false, false};
			double wgt[2] = {0, 0};
			for (int c = 0; c < 2; c++) {
				const Calib &k = cam[c];
				double q[3] = {P[0] - k.m[3], P[1] - k.m[7], P[2] - k.m[11]}, dcam[3], px, py;
				for (int i = 0; i < 3; i++) // DeviceFromCamera^T
					dcam[i] = k.m[i] * q[0] + k.m[4 + i] * q[1] + k.m[8 + i] * q[2];
				if (dcam[2] <= 0 || !Project62(k, dcam, px, py))
					continue;
				double sx = px - 0.5, sy = py - 0.5; // pixel centres at integers
				if (sx < 0 || sy < 0 || sx >= kW - 1 || sy >= kH - 1)
					continue;
				int ix = (int)sx, iy = (int)sy;
				double fx = sx - ix, fy = sy - iy;
				int w1 = (int)std::lround(fx * (1 - fy) * 256), w2 = (int)std::lround((1 - fx) * fy * 256),
				    w3 = (int)std::lround(fx * fy * 256);
				w3 = std::min(w3, 256 - w1 - w2); // the roundings may sum past 256
				tap[c].off = (uint32_t)(iy * kW + ix);
				tap[c].w0 = (uint16_t)(256 - w1 - w2 - w3);
				tap[c].w[0] = 0, tap[c].w[1] = (uint8_t)w1, tap[c].w[2] = (uint8_t)w2, tap[c].w[3] = (uint8_t)w3;
				tap[c].pad = 0;
				have[c] = true;
				wgt[c] = std::min(1.0, std::min({sx, sy, kW - 1 - sx, kH - 1 - sy}) / feather);
			}
			const uint32_t i = (uint32_t)(y * kW + x);
			if (have[0] && have[1]) {
				int wTop = (int)std::lround(256 * wgt[1] * (1 - wgt[0]) / std::max(1e-6, wgt[1] * (1 - wgt[0]) + wgt[0]));
				if (wTop <= 0)
					out.bottom.push_back({i, tap[0]});
				else if (wTop >= 256)
					out.top.push_back({i, tap[1]});
				else
					out.both.push_back({i, tap[0], tap[1], (uint16_t)wTop, 0});
			} else if (have[0])
				out.bottom.push_back({i, tap[0]});
			else if (have[1])
				out.top.push_back({i, tap[1]});
			else
				out.empty++;
		}
	return out;
}

//! One eye's composite image from its bottom and top camera (stride kW); topGain (8.8 fixed point) scales the top camera.
//! Only the pixels some camera sees are written: `out` (ordinary memory, copied to the shared image in one pass: scattered
//! writes there are slow) keeps the black it was filled with everywhere else.
static void RenderComposite(const Composite &cp, const uint8_t *bottom, const uint8_t *top, int topGain,
                            const uint32_t grayPx[256], uint32_t *out, double &sumB, double &sumT, int &n)
{
	sumB = sumT = 0, n = 0;
	for (const OneTap &e : cp.bottom)
		out[e.out] = grayPx[Sample(bottom, e.t)];
	if (!top) { // no top image: the bottom camera where it sees, black elsewhere
		for (const OneTap &e : cp.top)
			out[e.out] = grayPx[0];
		for (const TwoTaps &e : cp.both)
			out[e.out] = grayPx[Sample(bottom, e.b)];
		return;
	}
	for (const OneTap &e : cp.top)
		out[e.out] = grayPx[std::min(255, (Sample(top, e.t) * topGain) >> 8)];
	long sb = 0, st = 0;
	for (size_t k = 0; k < cp.both.size(); k++) {
		const TwoTaps &e = cp.both[k];
		int vb = Sample(bottom, e.b), vt = Sample(top, e.t);
		if ((k & 7) == 0)
			sb += vb, st += vt, n++;
		vt = std::min(255, (vt * topGain) >> 8);
		out[e.out] = grayPx[(vb * (256 - e.wTop) + vt * e.wTop + 128) >> 8];
	}
	sumB = (double)sb, sumT = (double)st;
}

//! Test pattern 5: each pixel of the image as the shader's model (FillDesc: KB fisheye, the record's pose) places it, in
//! device directions: lines every 10 degrees of azimuth and elevation, the horizon and straight ahead in red, above the
//! horizon lighter. With the head level, the red cross must sit on the virtual world's horizon, straight ahead.
static void DirectionGrid(const Calib &cal, std::vector<uint8_t> &luma, std::vector<uint8_t> &red)
{
	luma.assign((size_t)kW * kH, 0), red.assign((size_t)kW * kH, 0);
	const double deg = 180 / M_PI;
	for (int y = 0; y < kH; y++)
		for (int x = 0; x < kW; x++) {
			double u = (x - cal.cx) / cal.f, v = (y - cal.cy) / cal.f, thd = sqrt(u * u + v * v), th = thd;
			for (int it = 0; it < 20; it++) { // th (1 + k1 th^2 + ...) = thd
				double t2 = th * th, poly = 1 + t2 * (cal.kb[0] + t2 * (cal.kb[1] + t2 * (cal.kb[2] + t2 * cal.kb[3])));
				double dpoly = 1 + t2 * (3 * cal.kb[0] + t2 * (5 * cal.kb[1] + t2 * (7 * cal.kb[2] + t2 * 9 * cal.kb[3])));
				th -= (th * poly - thd) / dpoly;
			}
			double s = thd > 1e-9 ? sin(th) / thd : 1, dc[3] = {u * s, v * s, cos(th)}, d[3];
			for (int r = 0; r < 3; r++)
				d[r] = cal.m[r * 4] * dc[0] + cal.m[r * 4 + 1] * dc[1] + cal.m[r * 4 + 2] * dc[2];
			double az = atan2(d[0], -d[2]) * deg, el = asin(std::max(-1.0, std::min(1.0, d[1]))) * deg;
			double fe = fabs(el - 10 * std::round(el / 10)), fa = fabs(az - 10 * std::round(az / 10));
			size_t i = (size_t)y * kW + x;
			luma[i] = el > 0 ? 110 : 50;
			if (fe < 0.5 || fa < 0.5)
				luma[i] = 235;
			if ((fabs(el) < 0.8 && fabs(az) < 60) || (fabs(az) < 0.8 && fabs(el) < 60))
				red[i] = 1, luma[i] = 150;
		}
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
	               (float)cal.kb[0], (float)cal.kb[1], (float)cal.kb[2], (float)cal.kb[3]};
	memcpy(desc + 16, in, sizeof(in));
	// +48: the camera's pose in raw tracking space at the capture (head pose at capture * Meta's DeviceFromCamera, OpenCV
	// camera axes): it becomes the shader's _m3, which projects the mesh's points (raw space: driver_quest1 publishes the mesh
	// with its rawFromMeshPose) into the camera.
	// +96: DeviceFromCamera alone (head frame): vrcompositor (0x160a80) draws each eye with the camera nearest to it,
	// comparing these with the eye positions (when params byte 1, state[3], is set: SetShow).
	// +144: identity. vrcompositor turns the mesh by X = (+144) * P^-1 (P: the head pose it renders with) into its _m1 (head
	// -> eye * X): with the identity that is P^-1, raw space -> the head at display, exact for any P. See
	// docs/passthrough.md, "The view 35 degrees too low" and "The mesh in tracking space".
	float m1[12];
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 4; c++)
			m1[r * 4 + c] = (float)cal.m[r * 4 + c];
	memcpy(desc + 96, m1, sizeof(m1));
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 4; c++)
			m1[r * 4 + c] = (float)(head.m[r][0] * cal.m[c] + head.m[r][1] * cal.m[4 + c] + head.m[r][2] * cal.m[8 + c] +
			                        (c == 3 ? head.m[r][3] : 0));
	memcpy(desc + 48, m1, sizeof(m1));
	const float identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
	memcpy(desc + 144, identity, sizeof(identity));
	// +204..+216: the valid image area in texture coordinates (u min, v min, u max, v max; the fragment shader's bounds,
	// vrcompositor 0x15f754 -> uniforms _m15/_m16): outside it the roomview is black.
	// Unbounded: where the bottom cameras do not see (above the horizon), the image's edge is stretched (the sampler clamps
	// to the edge) instead of black, as Meta's passthrough does
	const float bounds[4] = {-100, -100, 101, 101};
	memcpy(desc + 204, bounds, sizeof(bounds));
}

//! The head pose (raw tracking space) at a frame set's capture, from driver_quest1 (quest1_camera_ring.h, q1cam_poses). It
//! wakes on the same frame set as we do: wait up to 15 ms for it.
static bool CapturePose(int64_t capNs, HmdMatrix34_t &out)
{
	static const q1cam_poses *poses = nullptr;
	if (!poses) {
		int fd = open(Q1CAM_POSES_PATH, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			return false;
		void *m = mmap(nullptr, sizeof(q1cam_poses), PROT_READ, MAP_SHARED, fd, 0);
		close(fd);
		if (m == MAP_FAILED)
			return false;
		poses = (const q1cam_poses *)m;
	}
	if (!capNs || poses->magic != Q1CAM_POSES_MAGIC)
		return false;
	for (int tries = 0; tries < 30; tries++) {
		for (const q1cam_pose_entry &e : poses->entry) {
			uint32_t seq = __atomic_load_n(&e.seq, __ATOMIC_ACQUIRE);
			if ((seq & 1) || e.capture_mono_ns != capNs)
				continue;
			memcpy(&out.m[0][0], (const void *)e.raw_from_head, sizeof(out.m));
			if (__atomic_load_n(&e.seq, __ATOMIC_ACQUIRE) == seq)
				return true;
		}
		usleep(500);
	}
	return false;
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
	const Calib bottomCal[2] = {LoadCalib(0), LoadCalib(2)}, topCal[2] = {LoadCalib(1), LoadCalib(3)};
	// PT_COMPOSITE=0: the bottom cameras alone, as before 2026-11-08
	const bool composite = !getenv("PT_COMPOSITE") || atoi(getenv("PT_COMPOSITE")) != 0;
	const bool useComp[2] = {composite && bottomCal[0].ok && topCal[0].ok, composite && bottomCal[1].ok && topCal[1].ok};
	const Calib calib[2] = {useComp[0] ? VirtualCamera(bottomCal[0]) : bottomCal[0],
	                        useComp[1] ? VirtualCamera(bottomCal[1]) : bottomCal[1]};
	Composite lut[2];
	int topGain[2] = {256, 256};
	for (int c = 0; c < 2; c++)
		if (useComp[c]) {
			const Calib pair[2] = {bottomCal[c], topCal[c]};
			lut[c] = BuildComposite(calib[c], pair);
			printf("ptprovider: eye %d: composite of cameras %d and %d, f %.1f: %zu pixels from the bottom one, %zu from the top "
			       "one, %zu blended, %zu seen by neither\n",
			       c, c ? 2 : 0, c ? 3 : 1, calib[c].f, lut[c].bottom.size(), lut[c].top.size(), lut[c].both.size(),
			       lut[c].empty);
		}
	std::vector<uint8_t> gridLuma[2], gridRed[2];
	if (pattern == 5)
		for (int c = 0; c < 2; c++)
			DirectionGrid(calib[c], gridLuma[c], gridRed[c]);
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
		printf("ptprovider: camera %d %s: f %.1f centre %.1f %.1f, fitted KB %.4f %.4f %.4f %.4f\n", c ? 2 : 0,
		       calib[c].ok ? "calibrated" : "NOT calibrated (defaults)", calib[c].f, calib[c].cx, calib[c].cy, calib[c].kb[0],
		       calib[c].kb[1], calib[c].kb[2], calib[c].kb[3]);

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
	int64_t lastFrame = 0, lastBeat = 0, lastDump = 0, lastIdle = 0, lastWarn = 0;
	int noCapturePose = 0;
	int slot[2] = {0, 0};
	int readyIdx[2] = {-1, -1}; // the one ready record per camera (-1: none)
	int64_t lastTake = 0;       // when the compositor last took a frame of ours
	int64_t stallSince = 0, lastStallLog = 0;
	auto Rec = [&](int c, int k) { return (uint32_t *)(b + kRec0 + c * kCamStride + k * kRecStride); };
	// each eye's composite in ordinary memory (black where no camera sees, written once)
	std::vector<uint32_t> compImg[2] = {std::vector<uint32_t>((size_t)kW * kH, grayPx[0]),
	                                    std::vector<uint32_t>((size_t)kW * kH, grayPx[0])};
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
		// The records to write this frame into. At most one record per camera is ready (1) at a time, the newest frame: the
		// compositor takes any ready record, and with several it took old ones (up to 1.7 s old, 336 times a minute while it
		// ran slow: the view jumped back). A record it has taken (2) is left alone; it frees older taken ones itself.
		// While it takes nothing (roomview hidden) the ready frame is only replaced twice a second, to keep the CPU idle.
		bool canWrite;
		{
			bool reading = now - lastTake < 1000000000LL;
			int target[2] = {-1, -1};
			if (reading || now - lastIdle >= 500000000LL) {
				for (int c = 0; c < 2; c++)
					for (int i = 0; i < N && target[c] < 0; i++)
						if ((slot[c] + i) % N != readyIdx[c] && __atomic_load_n(Rec(c, (slot[c] + i) % N), __ATOMIC_ACQUIRE) == 0)
							target[c] = (slot[c] + i) % N;
				bool reclaimed[2] = {false, false};
				for (int c = 0; c < 2; c++) {
					uint32_t ready = 1;
					if (target[c] < 0 && readyIdx[c] >= 0 &&
					    __atomic_compare_exchange_n(Rec(c, readyIdx[c]), &ready, 0u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
						target[c] = readyIdx[c]; // no free record: the untaken ready one gets the newer frame
						reclaimed[c] = true;
					}
				}
				if (target[0] < 0 || target[1] < 0) {
					// one camera has nowhere to write: keep the other camera's ready frame (freeing it left that eye
					// with nothing ready while the compositor held every record of the other: the view froze)
					for (int c = 0; c < 2; c++)
						if (reclaimed[c])
							__atomic_store_n(Rec(c, readyIdx[c]), 1u, __ATOMIC_RELEASE);
					target[0] = target[1] = -1;
				} else {
					for (int c = 0; c < 2; c++)
						if (reclaimed[c])
							readyIdx[c] = -1;
				}
			}
			canWrite = target[0] >= 0 && target[1] >= 0;
			if (reading && !canWrite) {
				if (!stallSince)
					stallSince = now;
				else if (now - stallSince > 500000000LL && now - lastStallLog > 2000000000LL) {
					lastStallLog = now;
					char sts[64];
					int o = 0;
					for (int c = 0; c < 2; c++)
						for (int k = 0; k < N && o < 60; k++)
							o += snprintf(sts + o, sizeof(sts) - o, "%u%s", __atomic_load_n(Rec(c, k), __ATOMIC_ACQUIRE),
							              k == N - 1 && c == 0 ? " / " : "");
					printf("ptprovider: no record to write for %.1f s (states %s, ready %d %d)\n", (now - stallSince) * 1e-9, sts,
					       readyIdx[0], readyIdx[1]);
					fflush(stdout);
				}
			} else
				stallSince = 0;
			if (canWrite) {
				slot[0] = target[0], slot[1] = target[1];
				if (!reading)
					lastIdle = now;
			}
		}
		bool fresh = false;
		double capS = now * 1e-9;
		int64_t capNs = 0;
		if (canWrite && pattern) {
			fresh = true;
			for (int c = 0; c < 2; c++) {
				Img &im = img[c][slot[c]];
				if (pattern == 5)
					WriteYuv(im, [&](int x, int y) { return gridLuma[c][(size_t)y * kW + x]; },
					         [&](int x, int y) { return gridRed[c][(size_t)y * kW + x] != 0; });
				else if (pattern >= 3) { // the whole buffer filled with one RGBA value (3: 255,0,0,255; 4: 0,0,255,255)
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
				// the newest headset-exposure frame set since the last one (a controller-exposure set often follows it)
				uint32_t si = (n - 1) % hdr->slot_count;
				for (uint32_t k = n; k != n - std::min<uint32_t>(n - last, hdr->slot_count); k--) {
					const q1cam_ring_slot &c = hdr->slot[(k - 1) % hdr->slot_count];
					if (c.frame_type == Q1CAM_FRAME_HEADSET || c.frame_type == 0) {
						si = (k - 1) % hdr->slot_count;
						break;
					}
				}
				last = n;
				const q1cam_ring_slot &s = hdr->slot[si];
				uint32_t seq0 = __atomic_load_n(&s.seq, __ATOMIC_ACQUIRE);
				if (!(seq0 & 1) && (s.frame_type == Q1CAM_FRAME_HEADSET || s.frame_type == 0)) {
					const uint8_t *base = (const uint8_t *)hdr + hdr->header_size + (size_t)si * hdr->slot_bytes;
					auto camImage = [&](int id, int &stride) -> const uint8_t * {
						for (uint32_t ci = 0; ci < s.ncams && ci < Q1CAM_MAX_CAMS; ci++) {
							const q1cam_ring_cam &rc = s.cam[ci];
							if (rc.present && (int)rc.cam_id == id && rc.width == (uint32_t)kW && rc.height == (uint32_t)kH) {
								stride = (int)rc.stride;
								return base + rc.offset;
							}
						}
						return nullptr;
					};
					int got = 0;
					for (int c = 0; c < 2; c++) {
						int sb = 0, stt = 0;
						const uint8_t *src = camImage(c ? 2 : 0, sb), *top = useComp[c] ? camImage(c ? 3 : 1, stt) : nullptr;
						if (!src)
							continue;
						Img &im = img[c][slot[c]];
						if (useComp[c] && sb == kW && (!top || stt == sb)) {
							double sumB, sumT;
							int n;
							RenderComposite(lut[c], src, top, topGain[c], grayPx, compImg[c].data(), sumB, sumT, n);
							memcpy(im.map, compImg[c].data(), (size_t)kW * kH * 4);
							if (n > 200 && sumT > n * 4.0) { // follow the bottom camera's brightness, slowly
								int want = (int)std::lround(256.0 * sumB / sumT);
								want = std::max(128, std::min(1024, want));
								topGain[c] += (want - topGain[c]) / 8;
							}
						} else {
							for (int y = 0; y < kH; y++) {
								const uint8_t *row = src + (size_t)y * sb;
								uint32_t *out = (uint32_t *)im.map + (size_t)y * kW;
								for (int x = 0; x < kW; x++)
									out[x] = grayPx[row[x]];
							}
						}
						got++;
					}
					if (got == 2 && __atomic_load_n(&s.seq, __ATOMIC_ACQUIRE) == seq0) {
						fresh = true;
						lastFrame = now;
						capNs = s.capture_mono_ns ? s.capture_mono_ns : s.arrival_mono_ns;
						capS = capNs * 1e-9;
					}
				}
			}
		}
		// the head pose at capture (it places the camera in raw space, +48: vrcompositor reprojects the frame from there to the
		// head pose it displays with): the frames reach us 25-35 ms after their mid-exposure, and a pose of the wrong time
		// shakes the view as the head turns.
		// driver_quest1 publishes it (Monado's pose at the capture time). A camera frame without it is dropped (the previous
		// one stays, reprojected): SteamVR's own HMD pose is predicted to the display time and made the view jump.
		HmdMatrix34_t head;
		if (fresh && capNs && !CapturePose(capNs, head)) {
			noCapturePose++;
			fresh = false;
		}
		if (fresh) {
			if (!capNs) { // test patterns
				TrackedDevicePose_t hp = {};
				VRSystem()->GetDeviceToAbsoluteTrackingPose(TrackingUniverseRawAndUncalibrated, 0, &hp, 1);
				head = hp.bPoseIsValid ? hp.mDeviceToAbsoluteTracking : HmdMatrix34_t{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
			}
			for (int c = 0; c < 2; c++) {
				FillDesc((uint8_t *)Rec(c, slot[c]) + 4, capS, calib[c], head);
				__atomic_store_n(Rec(c, slot[c]), 1u, __ATOMIC_RELEASE); // frame ready
				// the previous ready frame is now stale: free it, unless the compositor took it meanwhile
				uint32_t ready = 1;
				if (readyIdx[c] >= 0 && readyIdx[c] != slot[c] &&
				    !__atomic_compare_exchange_n(Rec(c, readyIdx[c]), &ready, 0u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
					lastTake = now; // it was taken: the compositor is reading
				readyIdx[c] = slot[c];
				slot[c] = (slot[c] + 1) % N;
			}
			if (!b[4]) {
				b[4] = 1; // enable only now that a frame is in place
				SetShow(st, true);
				printf("ptprovider: roomview data enabled\n");
				fflush(stdout);
			}
		}
		if (noCapturePose && now - lastWarn > 10000000000LL) {
			lastWarn = now;
			printf("ptprovider: %d camera frames dropped: no capture pose\n", noCapturePose);
			fflush(stdout);
			noCapturePose = 0;
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
