// IVRCameraComponent_003 for driver_quest1 (see camera.h and docs/passthrough.md).
//
// How vrserver drives the component (SteamVR 2.17 linuxarm64 vrserver, CVRServerCameraHost; addresses
// in docs/passthrough.md):
//  1. When a client enables the camera: GetCameraFrameBufferingRequirements(&queue, &size); vrserver
//     creates `queue` shared-memory buffers "VR_TrackedCameraStream_<n>" of `size` bytes each and
//     hands their (vrserver-side) addresses to SetCameraFrameBuffering(queue, ptrs, size), then calls
//     SetCameraVideoSinkCallback(host). StartVideoStream / StopVideoStream / Pause / Resume follow the
//     clients' requests.
//  2. The driver writes a frame into a free buffer and calls OnCameraVideoSinkCallback(). vrserver
//     then calls GetVideoStreamFrame() until it returns nullptr. For each frame it reads
//     m_nBufferIndex (+20) and, if m_flFrameCaptureTime_ServerRelative (+88) != 0, stores
//     ticks(now + that) in m_nFrameCaptureTicks_ServerAbsolute (+96); it then queues the buffer index
//     for its clients (if the queue is full it calls ReleaseVideoStreamFrame at once).
//  3. When clients are done with a buffer, vrserver calls ReleaseVideoStreamFrame(<address of buffer
//     n>): the frame header must therefore live at the start of the buffer, and GetVideoStreamFrame
//     returns that address. The pixels follow the header (QUEST1_CAMERA_IMAGE_OFFSET, default
//     sizeof(CameraVideoStreamFrame_t) = 208; not verified on a consumer yet).
//
// The composed frame: the selected cameras (default calibration Ids 0 and 2, the bottom pair that
// looks forward-down) turned upright by quarter turns (computed from DeviceFromCamera in the factory
// calibration), stacked vertically (EVRTrackedCameraFrameLayout_Stereo | VerticalLayout: left on top)
// or a single camera (Mono). Grayscale, brightened by a gamma/auto-gain LUT (the frames come with the
// SLAM exposure), delivered as RGBX32 (default), RGB24, NV12 or YUYV16.

#include "camera.h"
#include "quest1_camera_ring.h"

#include <fcntl.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace vr;

namespace {

void CLog(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	VRDriverLog()->Log(buf);
}

int64_t MonoNs()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

const char *Env(const char *name, const char *def = nullptr)
{
	const char *v = getenv(name);
	return v && *v ? v : def;
}

// --- Minimal JSON reader (enough for camera_calibration_v2.json) ------------------------------------------

struct Json
{
	enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
	double num = 0;
	std::string str;
	std::vector<Json> arr;
	std::map<std::string, Json> obj;

	const Json *Get(const char *key) const
	{
		if (type != Obj)
			return nullptr;
		auto it = obj.find(key);
		return it == obj.end() ? nullptr : &it->second;
	}
	bool Numbers(double *out, size_t n) const
	{
		if (type != Arr || arr.size() < n)
			return false;
		for (size_t i = 0; i < n; i++) {
			if (arr[i].type != Num)
				return false;
			out[i] = arr[i].num;
		}
		return true;
	}
};

struct JsonParser
{
	const char *p, *end;
	int depth = 0;

	void Ws()
	{
		while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
			p++;
	}
	bool String(std::string &out)
	{
		if (p >= end || *p != '"')
			return false;
		p++;
		while (p < end && *p != '"') {
			if (*p == '\\' && p + 1 < end) {
				p++;
				char c = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p; // \uXXXX kept as 'u' + digits
				out += c;
			} else {
				out += *p;
			}
			p++;
		}
		if (p >= end)
			return false;
		p++;
		return true;
	}
	bool Value(Json &v)
	{
		if (++depth > 32)
			return false;
		Ws();
		if (p >= end)
			return false;
		bool ok = true;
		if (*p == '{') {
			v.type = Json::Obj;
			p++;
			Ws();
			if (p < end && *p == '}') {
				p++;
			} else {
				for (;;) {
					Ws();
					std::string key;
					if (!String(key))
						return false;
					Ws();
					if (p >= end || *p != ':')
						return false;
					p++;
					if (!Value(v.obj[key]))
						return false;
					Ws();
					if (p < end && *p == ',') {
						p++;
						continue;
					}
					if (p < end && *p == '}') {
						p++;
						break;
					}
					return false;
				}
			}
		} else if (*p == '[') {
			v.type = Json::Arr;
			p++;
			Ws();
			if (p < end && *p == ']') {
				p++;
			} else {
				for (;;) {
					v.arr.emplace_back();
					if (!Value(v.arr.back()))
						return false;
					Ws();
					if (p < end && *p == ',') {
						p++;
						continue;
					}
					if (p < end && *p == ']') {
						p++;
						break;
					}
					return false;
				}
			}
		} else if (*p == '"') {
			v.type = Json::Str;
			ok = String(v.str);
		} else if (!strncmp(p, "true", std::min<size_t>(4, end - p)) && end - p >= 4) {
			v.type = Json::Bool, v.num = 1, p += 4;
		} else if (!strncmp(p, "false", std::min<size_t>(5, end - p)) && end - p >= 5) {
			v.type = Json::Bool, p += 5;
		} else if (!strncmp(p, "null", std::min<size_t>(4, end - p)) && end - p >= 4) {
			p += 4;
		} else {
			std::string tmp(p, std::min<size_t>(64, end - p));
			char *e = nullptr;
			v.type = Json::Num;
			v.num = strtod(tmp.c_str(), &e);
			if (e == tmp.c_str())
				return false;
			p += e - tmp.c_str();
		}
		depth--;
		return ok;
	}
};

bool ParseJsonFile(const char *path, Json &out)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	std::string s;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && s.size() < (1u << 20))
		s.append(buf, n);
	fclose(f);
	JsonParser jp{s.data(), s.data() + s.size()};
	return jp.Value(out);
}

// --- Camera model ------------------------------------------------------------------------------------------

//! One tracking camera: factory calibration (raw sensor orientation, OpenCV axes: x right, y down,
//! z forward) and the quarter turns that make its image upright.
struct CamModel
{
	int id = 0;
	bool calibrated = false;
	int w = 640, h = 480;                  // raw image
	double f = 191, cx = 319.5, cy = 239.5; // PinholeSymmetric
	double k[6] = {0.29, 0, -0.15, 0.09, -0.02, 0}; // Fisheye62 radial (typical values when uncalibrated)
	double p[2] = {0, 0};                  // Fisheye62 tangential
	double R[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}; // DeviceFromCamera rotation (raw camera axes)
	double t[3] = {0, 0, 0};
	int rot = 3; // quarter turns clockwise applied to the raw image (3 = 90° counter-clockwise)

	int RotW() const { return rot & 1 ? h : w; }
	int RotH() const { return rot & 1 ? w : h; }

	//! rotated-camera direction -> raw-camera direction (inverse of the image rotation)
	void RotToRaw(const double in[3], double out[3]) const
	{
		double x = in[0], y = in[1];
		switch (rot & 3) {
		case 0: out[0] = x, out[1] = y; break;
		case 1: out[0] = y, out[1] = -x; break;  // rotated (xr, yr) = (-y, x)
		case 2: out[0] = -x, out[1] = -y; break;
		case 3: out[0] = -y, out[1] = x; break;  // rotated (xr, yr) = (y, -x)
		}
		out[2] = in[2];
	}
	//! raw pixel -> rotated pixel (continuous coordinates, pixel centres at +0.5)
	void RawToRotPx(double x, double y, double &xr, double &yr) const
	{
		switch (rot & 3) {
		case 0: xr = x, yr = y; break;
		case 1: xr = h - y, yr = x; break;
		case 2: xr = w - x, yr = h - y; break;
		case 3: xr = y, yr = w - x; break;
		}
	}
	//! principal point in the rotated image
	void RotCenter(double &rcx, double &rcy) const { RawToRotPx(cx + 0.5, cy + 0.5, rcx, rcy), rcx -= 0.5, rcy -= 0.5; }

	//! Fisheye62 projection of a raw-camera direction to a raw pixel. False behind the camera.
	bool Project(const double d[3], double &px, double &py) const
	{
		double r = sqrt(d[0] * d[0] + d[1] * d[1]);
		double th = atan2(r, d[2]);
		if (th > 1.6) // ~92°: outside any useful field of view
			return false;
		double t2 = th * th, poly = 1, tp = 1;
		for (int i = 0; i < 6; i++) {
			tp *= t2;
			poly += k[i] * tp;
		}
		double thd = th * poly;
		double u = r > 1e-12 ? thd * d[0] / r : 0, v = r > 1e-12 ? thd * d[1] / r : 0;
		double r2 = u * u + v * v;
		double ut = u + 2 * p[0] * u * v + p[1] * (r2 + 2 * u * u);
		double vt = v + 2 * p[1] * u * v + p[0] * (r2 + 2 * v * v);
		px = f * ut + cx;
		py = f * vt + cy;
		return true;
	}

	//! HeadFromCamera with OpenVR camera axes (x right, y up, looking down -z) of the rotated image.
	HmdMatrix34_t HeadFromCamera() const
	{
		// columns: rotated camera axes expressed in raw camera axes, then in the device frame
		HmdMatrix34_t m{};
		const double axes[3][3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, -1}}; // OpenVR camera axes in OpenCV terms
		for (int c = 0; c < 3; c++) {
			double raw[3];
			RotToRaw(axes[c], raw);
			for (int r = 0; r < 3; r++)
				m.m[r][c] = (float)(R[r][0] * raw[0] + R[r][1] * raw[1] + R[r][2] * raw[2]);
		}
		for (int r = 0; r < 3; r++)
			m.m[r][3] = (float)t[r];
		return m;
	}
};

//! Chooses the quarter turn whose image "up" points closest to the device's +y (up).
int UprightRotation(const CamModel &c)
{
	int best = 0;
	double bestUp = -2;
	CamModel tmp = c;
	for (int r = 0; r < 4; r++) {
		tmp.rot = r;
		double up[3] = {0, -1, 0}, raw[3];
		tmp.RotToRaw(up, raw);
		double devUpY = c.R[1][0] * raw[0] + c.R[1][1] * raw[1] + c.R[1][2] * raw[2];
		if (devUpY > bestUp)
			bestUp = devUpY, best = r;
	}
	return best;
}

void LoadCalibration(std::vector<CamModel> &cams)
{
	const char *paths[] = {Env("QUEST1_CAMERA_CALIB"), "/persist/calibration/camera_calibration_v2.json",
	                       "/run/q1track/calibration/camera_calibration_v2.json"};
	Json root;
	const char *used = nullptr;
	for (const char *path : paths)
		if (path && ParseJsonFile(path, root)) {
			used = path;
			break;
		}
	const Json *list = used ? root.Get("CameraCalibration") : nullptr;
	if (!list || list->type != Json::Arr) {
		CLog("quest1 camera: no factory calibration readable, using defaults\n");
		return;
	}
	for (CamModel &c : cams) {
		for (const Json &e : list->arr) {
			const Json *id = e.Get("Id");
			int eid = !id ? -1 : id->type == Json::Num ? (int)id->num : id->type == Json::Str ? atoi(id->str.c_str()) : -1;
			if (eid != c.id)
				continue;
			double size[2], m[16], proj[3], dist[8];
			const Json *pj = e.Get("Projection"), *dj = e.Get("Distortion");
			const Json *pc = pj ? pj->Get("Coefficients") : nullptr, *dc = dj ? dj->Get("Coefficients") : nullptr;
			const Json *sz = e.Get("ImageSize"), *dfc = e.Get("DeviceFromCamera");
			if (!sz || !dfc || !pc || !dc || !sz->Numbers(size, 2) || !dfc->Numbers(m, 16) || !pc->Numbers(proj, 3) ||
			    !dc->Numbers(dist, 8))
				break;
			c.w = (int)size[0], c.h = (int)size[1];
			c.f = proj[0], c.cx = proj[1], c.cy = proj[2];
			for (int i = 0; i < 6; i++)
				c.k[i] = dist[i];
			c.p[0] = dist[6], c.p[1] = dist[7];
			for (int r = 0; r < 3; r++) {
				for (int col = 0; col < 3; col++)
					c.R[r][col] = m[r * 4 + col];
				c.t[r] = m[r * 4 + 3];
			}
			c.calibrated = true;
			c.rot = UprightRotation(c);
			break;
		}
		CLog("quest1 camera: cam %d %s, %dx%d f %.1f, image turned %d x 90 deg clockwise\n", c.id,
		     c.calibrated ? "calibrated" : "NOT in the calibration file", c.w, c.h, c.f, c.rot);
	}
}

// --- Frame sources -----------------------------------------------------------------------------------------

struct SrcImage
{
	const uint8_t *data = nullptr;
	int w = 0, h = 0, stride = 0;
	float exposure_s = 0, gain = 0;
};

class FrameSource
{
public:
	virtual ~FrameSource() = default;
	//! Waits up to timeoutMs for a frameset newer than the last one; fills one image per wanted
	//! camera id (data null when missing). The images stay valid until Done().
	virtual bool Acquire(const std::vector<int> &ids, std::vector<SrcImage> &out, int64_t &captureNs, int timeoutMs) = 0;
	//! False if the frameset changed under the reader (drop the frame).
	virtual bool Done() = 0;
};

class RingSource : public FrameSource
{
public:
	explicit RingSource(std::string path) : path(std::move(path)) {}
	~RingSource() override { Unmap(); }

	bool Acquire(const std::vector<int> &ids, std::vector<SrcImage> &out, int64_t &captureNs, int timeoutMs) override
	{
		Heartbeat();
		if (!hdr && !Map()) {
			usleep(std::min(timeoutMs, 500) * 1000);
			return false;
		}
		uint32_t n = __atomic_load_n(&hdr->write_count, __ATOMIC_ACQUIRE);
		if (n == lastCount) {
			timespec ts{timeoutMs / 1000, (timeoutMs % 1000) * 1000000L};
			syscall(SYS_futex, &hdr->write_count, FUTEX_WAIT, n, &ts, nullptr, 0);
			n = __atomic_load_n(&hdr->write_count, __ATOMIC_ACQUIRE);
			if (n == lastCount) {
				CheckStale();
				return false;
			}
		}
		lastCount = n;
		lastChange = MonoNs();
		slotIndex = (n - 1) % hdr->slot_count;
		const q1cam_ring_slot &s = hdr->slot[slotIndex];
		seq0 = __atomic_load_n(&s.seq, __ATOMIC_ACQUIRE);
		if (seq0 & 1)
			return false;
		if (s.frame_type != Q1CAM_FRAME_HEADSET && s.frame_type != 0)
			return false;
		const uint8_t *base = (const uint8_t *)hdr + hdr->header_size + (size_t)slotIndex * hdr->slot_bytes;
		out.assign(ids.size(), SrcImage{});
		for (size_t i = 0; i < ids.size(); i++)
			for (uint32_t c = 0; c < s.ncams && c < Q1CAM_MAX_CAMS; c++) {
				const q1cam_ring_cam &rc = s.cam[c];
				if (!rc.present || (int)rc.cam_id != ids[i])
					continue;
				if ((uint64_t)rc.offset + (uint64_t)rc.stride * rc.height > hdr->slot_bytes)
					continue;
				out[i] = SrcImage{base + rc.offset, (int)rc.width, (int)rc.height, (int)rc.stride, rc.exposure_s, rc.gain};
			}
		captureNs = s.capture_mono_ns ? s.capture_mono_ns : s.arrival_mono_ns;
		return true;
	}
	bool Done() override
	{
		if (!hdr)
			return false;
		uint32_t s1 = __atomic_load_n(&hdr->slot[slotIndex].seq, __ATOMIC_ACQUIRE);
		return s1 == seq0;
	}

private:
	//! libq1camtap (the producer inside Meta's trackingservice) copies frames only while this file's
	//! mtime is less than 3 s old: touch it once a second while we are reading
	void Heartbeat()
	{
		int64_t now = MonoNs();
		if (now - lastBeat < 1000000000LL)
			return;
		lastBeat = now;
		std::string p = path + ".want";
		int fd = open(p.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0666);
		if (fd >= 0) {
			futimens(fd, nullptr);
			close(fd);
		}
	}
	bool Map()
	{
		int64_t now = MonoNs();
		if (now - lastTry < 1000000000LL)
			return false;
		lastTry = now;
		int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			return false;
		struct stat st;
		if (fstat(fd, &st) || st.st_size < (off_t)sizeof(q1cam_ring_header)) {
			close(fd);
			return false;
		}
		void *m = mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
		close(fd);
		if (m == MAP_FAILED)
			return false;
		const q1cam_ring_header *h = (const q1cam_ring_header *)m;
		if (h->magic != Q1CAM_RING_MAGIC || h->version != Q1CAM_RING_VERSION || h->slot_count == 0 ||
		    h->slot_count > Q1CAM_RING_SLOTS || h->header_size < sizeof(q1cam_ring_header) ||
		    q1cam_ring_file_size(h->slot_bytes) - sizeof(q1cam_ring_header) + h->header_size > (uint64_t)st.st_size) {
			munmap(m, st.st_size);
			return false;
		}
		hdr = h;
		mapSize = st.st_size;
		lastCount = h->write_count;
		lastChange = now;
		CLog("quest1 camera: ring %s mapped (producer pid %d)\n", path.c_str(), h->producer_pid);
		return true;
	}
	void Unmap()
	{
		if (hdr)
			munmap((void *)hdr, mapSize);
		hdr = nullptr;
	}
	//! a producer restart recreates the file: remap when nothing arrived for 2 s
	void CheckStale()
	{
		if (MonoNs() - lastChange > 2000000000LL) {
			Unmap();
			lastTry = 0;
		}
	}

	std::string path;
	const q1cam_ring_header *hdr = nullptr;
	size_t mapSize = 0;
	uint32_t lastCount = 0, slotIndex = 0, seq0 = 0;
	int64_t lastTry = 0, lastChange = 0, lastBeat = 0;
};

//! Synthetic frames (30 Hz): a moving gradient with a per-camera bar pattern, to test the plumbing
//! without cameras.
class TestSource : public FrameSource
{
public:
	bool Acquire(const std::vector<int> &ids, std::vector<SrcImage> &out, int64_t &captureNs, int timeoutMs) override
	{
		int64_t now = MonoNs();
		int64_t next = last + 33333333;
		if (next > now) {
			int64_t wait = std::min<int64_t>(next - now, (int64_t)timeoutMs * 1000000);
			timespec ts{(time_t)(wait / 1000000000), (long)(wait % 1000000000)};
			nanosleep(&ts, nullptr);
			if (MonoNs() < next)
				return false;
		}
		last = MonoNs();
		frame++;
		imgs.resize(ids.size());
		out.assign(ids.size(), SrcImage{});
		for (size_t i = 0; i < ids.size(); i++) {
			std::vector<uint8_t> &im = imgs[i];
			im.resize(640 * 480);
			for (int y = 0; y < 480; y++)
				for (int x = 0; x < 640; x++) {
					uint8_t v = (uint8_t)((x + y + frame * 4) & 0xff) / 2;
					if (y < 40 && x / 40 <= ids[i]) // camera id as bars, top-left of the raw image
						v = 255;
					if (x < 8 || y < 8) // raw image's left and top edges
						v = 200;
					im[y * 640 + x] = v;
				}
			out[i] = SrcImage{im.data(), 640, 480, 640, 0.004f, 2.0f};
		}
		captureNs = last - 20000000;
		return true;
	}
	bool Done() override { return true; }

private:
	int64_t last = 0;
	uint32_t frame = 0;
	std::vector<std::vector<uint8_t>> imgs;
};

// --- The component ---------------------------------------------------------------------------------------

class Quest1Camera : public IVRCameraComponent
{
public:
	bool Init(const char *mode)
	{
		testMode = !strcmp(mode, "test");
		std::vector<int> ids;
		const char *idsStr = Env("QUEST1_CAMERA_IDS", "0,2");
		for (const char *s = idsStr; *s;) {
			char *e;
			long v = strtol(s, &e, 10);
			if (e == s)
				break;
			if (v >= 0 && v < 4 && ids.size() < 2)
				ids.push_back((int)v);
			s = *e ? e + 1 : e;
		}
		if (ids.empty())
			ids = {0, 2};
		cams.resize(ids.size());
		for (size_t i = 0; i < ids.size(); i++)
			cams[i].id = ids[i];
		LoadCalibration(cams);
		if (const char *rs = Env("QUEST1_CAMERA_ROT")) { // "r0,r1": override, quarter turns clockwise
			size_t i = 0;
			for (const char *s = rs; *s && i < cams.size(); i++) {
				char *e;
				cams[i].rot = (int)strtol(s, &e, 10) & 3;
				s = *e ? e + 1 : e;
			}
		}
		// all sub-images must have the same size for the stereo layout
		for (CamModel &c : cams)
			if (c.RotW() != cams[0].RotW() || c.RotH() != cams[0].RotH()) {
				CLog("quest1 camera: cameras %d and %d have different orientations, using one camera\n", cams[0].id, c.id);
				cams.resize(1);
				break;
			}
		subW = cams[0].RotW();
		subH = cams[0].RotH();
		frameW = subW;
		frameH = subH * (int)cams.size();

		std::string fmt = Env("QUEST1_CAMERA_FORMAT", "rgbx");
		format = fmt == "rgb" ? CVS_FORMAT_RGB24 : fmt == "nv12" ? CVS_FORMAT_NV12 : fmt == "yuyv" ? CVS_FORMAT_YUYV16 : CVS_FORMAT_RGBX32;
		gamma = (float)atof(Env("QUEST1_CAMERA_GAMMA", "0.6"));
		if (!(gamma > 0.1f && gamma < 4.0f))
			gamma = 0.6f;
		autoGain = atoi(Env("QUEST1_CAMERA_AUTOGAIN", "1")) != 0;
		hfov = (float)atof(Env("QUEST1_CAMERA_HFOV", "100"));
		if (!(hfov > 30 && hfov < 170))
			hfov = 100;
		imageOffset = (uint32_t)atoi(Env("QUEST1_CAMERA_IMAGE_OFFSET", "0"));
		if (imageOffset < sizeof(CameraVideoStreamFrame_t))
			imageOffset = sizeof(CameraVideoStreamFrame_t);
		queueSize = std::clamp(atoi(Env("QUEST1_CAMERA_QUEUE", "4")), 2, 8);
		ringPath = Env("QUEST1_CAMERA_RING", Q1CAM_RING_DEFAULT_PATH);
		CLog("quest1 camera: %s source, %zu camera(s), frame %dx%d %s, image offset %u\n",
		     testMode ? "test" : ringPath.c_str(), cams.size(), frameW, frameH, fmt.c_str(), imageOffset);
		return true;
	}

	void Attach(PropertyContainerHandle_t c, std::function<DriverPose_t()> pose)
	{
		headPose = std::move(pose);
		container = c;
		if (c == k_ulInvalidPropertyContainer)
			return;
		CVRPropertyHelpers *p = VRProperties();
		p->SetBoolProperty(c, Prop_HasCamera_Bool, true);
		p->SetBoolProperty(c, Prop_HasCameraComponent_Bool, true);
		p->SetBoolProperty(c, Prop_AllowCameraToggle_Bool, true);
		p->SetBoolProperty(c, Prop_CameraSupportsCompatibilityModes_Bool, false);
		p->SetInt32Property(c, Prop_NumCameras_Int32, (int32_t)cams.size());
		p->SetInt32Property(c, Prop_CameraFrameLayout_Int32,
		                    cams.size() > 1 ? (EVRTrackedCameraFrameLayout_Stereo | EVRTrackedCameraFrameLayout_VerticalLayout)
		                                    : EVRTrackedCameraFrameLayout_Mono);
		p->SetInt32Property(c, Prop_CameraStreamFormat_Int32, format);
		p->SetStringProperty(c, Prop_CameraFirmwareDescription_String, "Quest 1 OV7251 tracking cameras");
		std::vector<HmdMatrix34_t> xf;
		std::vector<int32_t> fn;
		std::vector<double> coef;
		std::vector<HmdVector4_t> wb;
		for (const CamModel &cm : cams) {
			xf.push_back(cm.HeadFromCamera());
			fn.push_back(VRDistortionFunctionType_FTheta);
			for (int i = 0; i < 6; i++)
				coef.push_back(cm.k[i]);
			coef.push_back(cm.p[0]);
			coef.push_back(cm.p[1]);
			wb.push_back(HmdVector4_t{{1, 1, 1, 1}});
		}
		p->SetProperty(c, Prop_CameraToHeadTransform_Matrix34, &xf[0], sizeof(HmdMatrix34_t), k_unHmdMatrix34PropertyTag);
		p->SetProperty(c, Prop_CameraToHeadTransforms_Matrix34_Array, xf.data(), (uint32_t)(xf.size() * sizeof(HmdMatrix34_t)),
		               k_unHmdMatrix34PropertyTag);
		p->SetProperty(c, Prop_CameraDistortionFunction_Int32_Array, fn.data(), (uint32_t)(fn.size() * sizeof(int32_t)),
		               k_unInt32PropertyTag);
		p->SetProperty(c, Prop_CameraDistortionCoefficients_Float_Array, coef.data(), (uint32_t)(coef.size() * sizeof(double)),
		               k_unDoublePropertyTag);
		p->SetProperty(c, Prop_CameraWhiteBalance_Vector4_Array, wb.data(), (uint32_t)(wb.size() * sizeof(HmdVector4_t)),
		               k_unHmdVector4PropertyTag);
	}

	void Detach()
	{
		StopVideoStream();
		std::lock_guard<std::mutex> lock(mutex);
		container = k_ulInvalidPropertyContainer;
		headPose = nullptr;
	}

	// --- IVRCameraComponent ---
	bool GetCameraFrameDimensions(ECameraVideoStreamFormat fmt, uint32_t *w, uint32_t *h) override
	{
		if (!Supported(fmt))
			return false;
		if (w)
			*w = frameW;
		if (h)
			*h = frameH;
		return true;
	}
	bool GetCameraFrameBufferingRequirements(int *queue, uint32_t *size) override
	{
		CLog("quest1 camera: GetCameraFrameBufferingRequirements\n");
		if (queue)
			*queue = queueSize;
		if (size)
			*size = imageOffset + ImageBytes(CVS_FORMAT_RGBX32); // largest format: a format change needs no realloc
		return true;
	}
	bool SetCameraFrameBuffering(int count, void **ptrs, uint32_t size) override
	{
		bool wasStreaming = streaming;
		StopThread();
		std::lock_guard<std::mutex> lock(mutex);
		buffers.clear();
		if (count <= 0 || !ptrs || size < imageOffset + ImageBytes(format)) {
			CLog("quest1 camera: rejected frame buffering (%d buffers of %u bytes)\n", count, size);
			return false;
		}
		for (int i = 0; i < count; i++)
			buffers.push_back(Buffer{(uint8_t *)ptrs[i], Free, 0});
		bufferSize = size;
		CLog("quest1 camera: %d frame buffers of %u bytes\n", count, size);
		if (wasStreaming)
			StartThread();
		return true;
	}
	bool SetCameraVideoStreamFormat(ECameraVideoStreamFormat fmt) override
	{
		if (!Supported(fmt) || (bufferSize && bufferSize < imageOffset + ImageBytes(fmt)))
			return false;
		format = fmt;
		return true;
	}
	ECameraVideoStreamFormat GetCameraVideoStreamFormat() override { return format; }
	bool StartVideoStream() override
	{
		CLog("quest1 camera: StartVideoStream\n");
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (buffers.empty()) {
				CLog("quest1 camera: StartVideoStream without frame buffers\n");
				return false;
			}
		}
		paused = false;
		if (!streaming) {
			streamStartNs = MonoNs();
			frameSequence = 0;
			StartThread();
		}
		return true;
	}
	void StopVideoStream() override
	{
		StopThread();
		std::lock_guard<std::mutex> lock(mutex);
		for (Buffer &b : buffers)
			b.state = Free;
	}
	bool IsVideoStreamActive(bool *pPaused, float *elapsed) override
	{
		if (pPaused)
			*pPaused = paused;
		if (elapsed)
			*elapsed = streaming ? (float)((MonoNs() - streamStartNs) * 1e-9) : 0.0f;
		return streaming;
	}
	const CameraVideoStreamFrame_t *GetVideoStreamFrame() override
	{
		std::lock_guard<std::mutex> lock(mutex);
		Buffer *oldest = nullptr;
		for (Buffer &b : buffers)
			if (b.state == Ready && (!oldest || b.order < oldest->order))
				oldest = &b;
		if (!oldest)
			return nullptr;
		oldest->state = Held;
		return (const CameraVideoStreamFrame_t *)oldest->ptr;
	}
	void ReleaseVideoStreamFrame(const CameraVideoStreamFrame_t *frame) override
	{
		std::lock_guard<std::mutex> lock(mutex);
		for (Buffer &b : buffers)
			if ((const void *)b.ptr == (const void *)frame) {
				b.state = Free;
				return;
			}
	}
	bool SetAutoExposure(bool) override { return true; } // exposure belongs to the tracking stack
	bool PauseVideoStream() override
	{
		paused = true;
		return true;
	}
	bool ResumeVideoStream() override
	{
		paused = false;
		return true;
	}
	bool GetCameraDistortion(uint32_t cam, float inU, float inV, float *outU, float *outV) override
	{
		if (cam >= cams.size() || !outU || !outV)
			return false;
		// (inU, inV): the camera's undistorted (pinhole, QUEST1_CAMERA_HFOV wide) image, 0..1;
		// (outU, outV): the same ray in the distorted (delivered) sub-image, 0..1.
		const CamModel &c = cams[cam];
		double fu = UndistortedFocal();
		double dirR[3] = {(inU * subW - subW * 0.5) / fu, (inV * subH - subH * 0.5) / fu, 1.0}, dir[3], px, py, xr, yr;
		c.RotToRaw(dirR, dir);
		if (!c.Project(dir, px, py))
			return false;
		c.RawToRotPx(px + 0.5, py + 0.5, xr, yr);
		*outU = (float)(xr / subW);
		*outV = (float)(yr / subH);
		return true;
	}
	bool GetCameraProjection(uint32_t cam, EVRTrackedCameraFrameType type, float zNear, float zFar, HmdMatrix44_t *out) override
	{
		if (cam >= cams.size() || !out || zFar <= zNear)
			return false;
		double f, cx, cy;
		Pinhole(cams[cam], type, f, cx, cy);
		// tangents of the image edges (image y down: row 0 is "up")
		double l = -(cx + 0.5) / f, r = (subW - cx - 0.5) / f, t = (cy + 0.5) / f, b = -(subH - cy - 0.5) / f;
		HmdMatrix44_t m{};
		m.m[0][0] = (float)(2 / (r - l));
		m.m[0][2] = (float)((r + l) / (r - l));
		m.m[1][1] = (float)(2 / (t - b));
		m.m[1][2] = (float)((t + b) / (t - b));
		m.m[2][2] = -(zFar + zNear) / (zFar - zNear);
		m.m[2][3] = -2 * zFar * zNear / (zFar - zNear);
		m.m[3][2] = -1;
		*out = m;
		return true;
	}
	bool SetFrameRate(int, int) override { return true; } // the tracking stack sets the camera rate
	bool SetCameraVideoSinkCallback(ICameraVideoSinkCallback *cb) override
	{
		sink.store(cb);
		return true;
	}
	bool GetCameraCompatibilityMode(ECameraCompatibilityMode *mode) override
	{
		if (mode)
			*mode = CAMERA_COMPAT_MODE_BULK_DEFAULT;
		return true;
	}
	bool SetCameraCompatibilityMode(ECameraCompatibilityMode mode) override { return mode == CAMERA_COMPAT_MODE_BULK_DEFAULT; }
	bool GetCameraFrameBounds(EVRTrackedCameraFrameType, uint32_t *l, uint32_t *t, uint32_t *w, uint32_t *h) override
	{
		if (l)
			*l = 0;
		if (t)
			*t = 0;
		if (w)
			*w = frameW;
		if (h)
			*h = frameH;
		return true;
	}
	bool GetCameraIntrinsics(uint32_t cam, EVRTrackedCameraFrameType type, HmdVector2_t *focal, HmdVector2_t *center,
	                         EVRDistortionFunctionType *dtype, double coeffs[k_unMaxDistortionFunctionParameters]) override
	{
		if (cam >= cams.size())
			return false;
		double f, cx, cy;
		Pinhole(cams[cam], type, f, cx, cy);
		if (focal)
			*focal = HmdVector2_t{{(float)f, (float)f}};
		if (center)
			*center = HmdVector2_t{{(float)cx, (float)cy}};
		bool distorted = type == VRTrackedCameraFrameType_Distorted;
		if (dtype)
			*dtype = distorted ? VRDistortionFunctionType_FTheta : VRDistortionFunctionType_None;
		if (coeffs) {
			for (uint32_t i = 0; i < k_unMaxDistortionFunctionParameters; i++)
				coeffs[i] = 0;
			if (distorted) {
				for (int i = 0; i < 6; i++)
					coeffs[i] = cams[cam].k[i];
				coeffs[6] = cams[cam].p[0];
				coeffs[7] = cams[cam].p[1];
			}
		}
		return true;
	}

private:
	enum BufState { Free, Ready, Held };
	struct Buffer
	{
		uint8_t *ptr;
		BufState state;
		uint64_t order;
	};

	static bool Supported(ECameraVideoStreamFormat f)
	{
		return f == CVS_FORMAT_RGBX32 || f == CVS_FORMAT_RGB24 || f == CVS_FORMAT_NV12 || f == CVS_FORMAT_YUYV16;
	}
	uint32_t ImageBytes(ECameraVideoStreamFormat f) const
	{
		uint32_t px = (uint32_t)frameW * frameH;
		switch (f) {
		case CVS_FORMAT_RGBX32: return px * 4;
		case CVS_FORMAT_RGB24: return px * 3;
		case CVS_FORMAT_NV12: return px * 3 / 2;
		case CVS_FORMAT_YUYV16: return px * 2;
		default: return px * 4;
		}
	}
	double UndistortedFocal() const { return subW * 0.5 / tan(hfov * 0.5 * M_PI / 180.0); }
	void Pinhole(const CamModel &c, EVRTrackedCameraFrameType type, double &f, double &cx, double &cy) const
	{
		if (type == VRTrackedCameraFrameType_Distorted) {
			f = c.f;
			c.RotCenter(cx, cy);
		} else {
			f = UndistortedFocal();
			cx = subW * 0.5 - 0.5;
			cy = subH * 0.5 - 0.5;
		}
	}

	void StartThread()
	{
		if (streaming)
			return;
		streaming = true;
		worker = std::thread([this] { Run(); });
	}
	void StopThread()
	{
		streaming = false;
		if (worker.joinable())
			worker.join();
	}

	void Run()
	{
		pthread_setname_np(pthread_self(), "quest1 camera");
		std::unique_ptr<FrameSource> src(testMode ? (FrameSource *)new TestSource() : new RingSource(ringPath));
		std::vector<int> ids;
		for (const CamModel &c : cams)
			ids.push_back(c.id);
		std::vector<SrcImage> imgs;
		std::vector<uint8_t> gray((size_t)frameW * frameH);
		int64_t lastCapture = 0;
		uint32_t delivered = 0;
		while (streaming) {
			int64_t captureNs = 0;
			if (!src->Acquire(ids, imgs, captureNs, 100))
				continue;
			if (paused) {
				src->Done();
				continue;
			}
			float expo = 0;
			for (size_t i = 0; i < cams.size(); i++) {
				Compose(cams[i], imgs[i], gray.data() + (size_t)i * subW * subH);
				if (imgs[i].data)
					expo = std::max(expo, imgs[i].exposure_s);
			}
			if (!src->Done())
				continue; // torn read: the producer reused the slot
			ApplyLut(gray);

			Buffer *b = TakeBuffer();
			if (!b)
				continue; // every buffer is held by vrserver's clients: drop
			CameraVideoStreamFrame_t *fr = (CameraVideoStreamFrame_t *)b->ptr;
			uint8_t *img = b->ptr + imageOffset;
			Convert(gray, img);
			memset(fr, 0, sizeof(*fr));
			int64_t now = MonoNs();
			fr->m_nStreamFormat = format;
			fr->m_nWidth = frameW;
			fr->m_nHeight = frameH;
			fr->m_nImageDataSize = ImageBytes(format);
			fr->m_nFrameSequence = frameSequence++;
			fr->m_nBufferIndex = (uint32_t)(b - buffers.data());
			fr->m_nBufferCount = (uint32_t)buffers.size();
			fr->m_nExposureTime = (uint32_t)(expo * 1e6f);
			fr->m_nISPFrameTimeStamp = (uint32_t)(captureNs / 1000);
			fr->m_flFrameElapsedTime = (captureNs - streamStartNs) * 1e-9;
			if (lastCapture && captureNs > lastCapture)
				deliveryRate = deliveryRate * 0.9 + 0.1 * (1e9 / (double)(captureNs - lastCapture));
			lastCapture = captureNs;
			fr->m_flFrameDeliveryRate = deliveryRate;
			fr->m_flFrameCaptureTime_DriverAbsolute = captureNs * 1e-9;
			double rel = (captureNs - now) * 1e-9; // negative: how long ago the frame was exposed
			fr->m_flFrameCaptureTime_ServerRelative = rel < -1e-6 ? rel : -1e-6;
			fr->m_RawTrackedDevicePose = Pose();
			fr->m_pImageData = (uint64_t)(uintptr_t)img;
			{
				std::lock_guard<std::mutex> lock(mutex);
				b->state = Ready;
				b->order = ++orderCounter;
			}
			if (ICameraVideoSinkCallback *cb = sink.load())
				cb->OnCameraVideoSinkCallback();
			if (++delivered == 1 || delivered % 900 == 0)
				CLog("quest1 camera: %u frames delivered (%.1f Hz, captured %.1f ms before delivery)\n", delivered,
				     deliveryRate, -rel * 1e3);
		}
	}

	//! A free buffer, or else the oldest one not yet handed to vrserver (its frame is dropped).
	Buffer *TakeBuffer()
	{
		std::lock_guard<std::mutex> lock(mutex);
		Buffer *pick = nullptr;
		for (Buffer &b : buffers)
			if (b.state == Free) {
				pick = &b;
				break;
			}
		if (!pick)
			for (Buffer &b : buffers)
				if (b.state == Ready && (!pick || b.order < pick->order))
					pick = &b;
		if (pick)
			pick->state = Held; // ours while we write it
		return pick;
	}

	//! raw camera image -> upright sub-image (subW x subH, 8 bit)
	void Compose(const CamModel &c, const SrcImage &s, uint8_t *dst) const
	{
		if (!s.data || s.w != c.w || s.h != c.h) {
			memset(dst, 0, (size_t)subW * subH);
			return;
		}
		const int W = s.w, H = s.h, st = s.stride;
		switch (c.rot & 3) {
		case 0:
			for (int y = 0; y < H; y++)
				memcpy(dst + (size_t)y * subW, s.data + (size_t)y * st, W);
			break;
		case 2:
			for (int y = 0; y < H; y++) {
				const uint8_t *src = s.data + (size_t)(H - 1 - y) * st;
				uint8_t *d = dst + (size_t)y * subW;
				for (int x = 0; x < W; x++)
					d[x] = src[W - 1 - x];
			}
			break;
		default:
			// tiles keep both the source rows and the destination rows in cache
			for (int ty = 0; ty < subH; ty += 32)
				for (int tx = 0; tx < subW; tx += 32)
					for (int yr = ty; yr < std::min(ty + 32, subH); yr++) {
						uint8_t *d = dst + (size_t)yr * subW;
						for (int xr = tx; xr < std::min(tx + 32, subW); xr++) {
							// rot 1: (xr, yr) = (H-1-y, x); rot 3: (xr, yr) = (y, W-1-x)
							int x = (c.rot & 3) == 1 ? yr : W - 1 - yr;
							int y = (c.rot & 3) == 1 ? H - 1 - xr : xr;
							d[xr] = s.data[(size_t)y * st + x];
						}
					}
			break;
		}
	}

	void ApplyLut(std::vector<uint8_t> &g)
	{
		if (autoGain) {
			uint64_t sum = 0, n = 0;
			for (size_t i = 0; i < g.size(); i += 61)
				sum += g[i], n++;
			double mean = n ? (double)sum / n : 0;
			// target mean ~ 0.35 after the gamma curve
			double want = 255.0 * pow(0.35, 1.0 / gamma);
			double g1 = mean > 0.5 ? std::clamp(want / mean, 1.0, 16.0) : 16.0;
			gain = gain * 0.8 + g1 * 0.2;
		} else {
			gain = 1.0;
		}
		uint8_t lut[256];
		for (int i = 0; i < 256; i++) {
			double v = std::min(1.0, i * gain / 255.0);
			lut[i] = (uint8_t)std::lround(255.0 * pow(v, gamma));
		}
		for (uint8_t &v : g)
			v = lut[v];
	}

	void Convert(const std::vector<uint8_t> &g, uint8_t *out) const
	{
		const size_t n = g.size();
		switch (format) {
		case CVS_FORMAT_RGBX32: {
			uint32_t *o = (uint32_t *)out;
			for (size_t i = 0; i < n; i++)
				o[i] = 0xff000000u | g[i] * 0x010101u;
			break;
		}
		case CVS_FORMAT_RGB24:
			for (size_t i = 0; i < n; i++)
				out[3 * i] = out[3 * i + 1] = out[3 * i + 2] = g[i];
			break;
		case CVS_FORMAT_NV12:
			memcpy(out, g.data(), n);
			memset(out + n, 128, n / 2);
			break;
		case CVS_FORMAT_YUYV16:
			for (size_t i = 0; i < n; i += 2) {
				out[2 * i] = g[i];
				out[2 * i + 1] = 128;
				out[2 * i + 2] = g[i + 1];
				out[2 * i + 3] = 128;
			}
			break;
		default:
			break;
		}
	}

	TrackedDevicePose_t Pose()
	{
		TrackedDevicePose_t tp{};
		std::function<DriverPose_t()> fn;
		{
			std::lock_guard<std::mutex> lock(mutex);
			fn = headPose;
		}
		if (!fn)
			return tp;
		DriverPose_t p = fn();
		if (!p.poseIsValid)
			return tp;
		// world <- driver <- device <- head, as vrserver composes a DriverPose_t
		auto rot = [](const HmdQuaternion_t &q, const double v[3], double o[3]) {
			double w = q.w, x = q.x, y = q.y, z = q.z;
			double tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
			o[0] = v[0] + w * tx + (y * tz - z * ty);
			o[1] = v[1] + w * ty + (z * tx - x * tz);
			o[2] = v[2] + w * tz + (x * ty - y * tx);
		};
		auto mul = [](const HmdQuaternion_t &a, const HmdQuaternion_t &b) {
			return HmdQuaternion_t{a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
			                       a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
		};
		double a[3], b[3];
		rot(p.qRotation, p.vecDriverFromHeadTranslation, a);
		for (int i = 0; i < 3; i++)
			a[i] += p.vecPosition[i];
		rot(p.qWorldFromDriverRotation, a, b);
		for (int i = 0; i < 3; i++)
			b[i] += p.vecWorldFromDriverTranslation[i];
		HmdQuaternion_t q = mul(mul(p.qWorldFromDriverRotation, p.qRotation), p.qDriverFromHeadRotation);
		double w = q.w, x = q.x, y = q.y, z = q.z;
		float m[3][3] = {{(float)(1 - 2 * (y * y + z * z)), (float)(2 * (x * y - z * w)), (float)(2 * (x * z + y * w))},
		                 {(float)(2 * (x * y + z * w)), (float)(1 - 2 * (x * x + z * z)), (float)(2 * (y * z - x * w))},
		                 {(float)(2 * (x * z - y * w)), (float)(2 * (y * z + x * w)), (float)(1 - 2 * (x * x + y * y))}};
		for (int r = 0; r < 3; r++) {
			for (int c = 0; c < 3; c++)
				tp.mDeviceToAbsoluteTracking.m[r][c] = m[r][c];
			tp.mDeviceToAbsoluteTracking.m[r][3] = (float)b[r];
		}
		double vin[3] = {p.vecVelocity[0], p.vecVelocity[1], p.vecVelocity[2]}, vout[3];
		rot(p.qWorldFromDriverRotation, vin, vout);
		tp.vVelocity = HmdVector3_t{{(float)vout[0], (float)vout[1], (float)vout[2]}};
		double ain[3] = {p.vecAngularVelocity[0], p.vecAngularVelocity[1], p.vecAngularVelocity[2]}, aout[3];
		rot(mul(p.qWorldFromDriverRotation, p.qRotation), ain, aout); // angular velocity is device-local
		tp.vAngularVelocity = HmdVector3_t{{(float)aout[0], (float)aout[1], (float)aout[2]}};
		tp.eTrackingResult = p.result;
		tp.bPoseIsValid = true;
		tp.bDeviceIsConnected = p.deviceIsConnected;
		return tp;
	}

	std::vector<CamModel> cams;
	int subW = 480, subH = 640, frameW = 480, frameH = 1280;
	ECameraVideoStreamFormat format = CVS_FORMAT_RGBX32;
	float gamma = 0.6f, hfov = 100;
	bool autoGain = true, testMode = false;
	double gain = 1.0, deliveryRate = 30.0;
	uint32_t imageOffset = sizeof(CameraVideoStreamFrame_t), bufferSize = 0;
	int queueSize = 4;
	std::string ringPath;

	std::mutex mutex; // buffers, container, headPose
	std::vector<Buffer> buffers;
	uint64_t orderCounter = 0;
	PropertyContainerHandle_t container = k_ulInvalidPropertyContainer;
	std::function<DriverPose_t()> headPose;

	std::atomic<ICameraVideoSinkCallback *> sink{nullptr};
	std::atomic<bool> streaming{false}, paused{false};
	std::thread worker;
	int64_t streamStartNs = 0;
	uint32_t frameSequence = 0;
};

// --- vrcompositor's camera gate ---------------------------------------------------------------------------
//
// vrcompositor answers IVRTrackedCamera::AcquireVideoStreamingService itself (CTrackedCamera, "Failed to
// start RoomView since there's no camera enabled") and refuses unless
//  1. the "camera enabled" byte of the shared state VR_CameraPassthroughState (a 25088-byte /dev/shm file
//     made by vrserver, byte 24 = first byte after the 20-byte header + 4) is set: on the Steam Frame the
//     frame_hmd driver sets it, nothing in vrserver does for other HMDs (see docs/passthrough.md), and
//  2. it connects to the block queues /xrservice/roomview3d/meshData and /meshLayout (the Frame's depth
//     mesh producer): created here (IVRBlockQueue, server-only) and left empty.
// Both are set up from the HMD's Activate while the camera is enabled (QUEST1_CAMERA).

//! IVRBlockQueue_004 (vrserver has no _005 class and asserts on unknown versions): virtual slots in capi order
//! (Create, Connect, Destroy), called through the vtable by hand (a pure abstract class here would get its calls
//! "devirtualized" to __cxa_pure_virtual by the compiler). Create has no flags argument in _004 (the extra one is
//! ignored).
struct BlockQueueIface
{
	void **vtbl;
	int Create(uint64_t *handle, char *path, uint32_t dataSize, uint32_t headerSize, uint32_t blockCount, uint32_t flags)
	{
		return ((int (*)(void *, uint64_t *, char *, uint32_t, uint32_t, uint32_t, uint32_t))vtbl[0])(this, handle, path, dataSize,
		                                                                                                headerSize, blockCount, flags);
	}
	int Destroy(uint64_t handle) { return ((int (*)(void *, uint64_t))vtbl[2])(this, handle); }
};

class CompositorGate
{
public:
	void Start()
	{
		if (running.exchange(true))
			return;
		CreateQueues();
		thread = std::thread([this] { Run(); });
	}
	void Stop()
	{
		if (!running.exchange(false))
			return;
		if (thread.joinable())
			thread.join();
		if (table)
			for (uint64_t h : queues)
				table->Destroy(h);
		queues.clear();
	}

private:
	void CreateQueues()
	{
		EVRInitError err = VRInitError_None;
		CLog("quest1 camera: getting IVRBlockQueue\n");
		table = (BlockQueueIface *)VRDriverContext()->GetGenericInterface("IVRBlockQueue_004", &err);
		if (!table) {
			CLog("quest1 camera: no IVRBlockQueue (%d), the compositor will not open the camera\n", (int)err);
			return;
		}
		CLog("quest1 camera: IVRBlockQueue %p\n", (void *)table);
		const uint32_t dataSize = (uint32_t)atoi(Env("QUEST1_CAMERA_MESH_BYTES", "1048576"));
		for (const char *name : {"/xrservice/roomview3d/meshData", "/xrservice/roomview3d/meshLayout"}) {
			char path[64];
			snprintf(path, sizeof(path), "%s", name);
			uint64_t h = 0;
			int e = table->Create(&h, path, dataSize, 64, 3, 0);
			CLog("quest1 camera: block queue %s: error %d\n", name, e);
			if (!e)
				queues.push_back(h);
		}
	}

	//! maps the 25088-byte VR_CameraPassthroughState file and keeps its "camera enabled" byte set
	void Run()
	{
		uint8_t *state = nullptr;
		size_t size = 25088;
		while (running) {
			if (!state) {
				if (DIR *d = opendir("/dev/shm")) {
					while (dirent *de = readdir(d)) {
						if (strncmp(de->d_name, "u", 1) || !strstr(de->d_name, "-Shm_"))
							continue;
						std::string path = std::string("/dev/shm/") + de->d_name;
						struct stat st;
						if (stat(path.c_str(), &st) || (size_t)st.st_size != size)
							continue;
						int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
						if (fd < 0)
							continue;
						void *m = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
						close(fd);
						if (m != MAP_FAILED) {
							state = (uint8_t *)m;
							CLog("quest1 camera: camera passthrough state %s\n", path.c_str());
							break;
						}
					}
					closedir(d);
				}
			}
			if (state)
				state[24] = 1;
			for (int i = 0; i < 10 && running; i++)
				usleep(100000);
		}
		if (state)
			munmap(state, size);
	}

	std::atomic<bool> running{false};
	std::thread thread;
	BlockQueueIface *table = nullptr;
	std::vector<uint64_t> queues;
};

CompositorGate g_gate;

Quest1Camera *g_camera = nullptr;
std::once_flag g_once;

//! "ring", "test" or nullptr (off)
const char *Mode()
{
	static char mode[16];
	const char *m = Env("QUEST1_CAMERA");
	if (!m) {
		if (FILE *f = fopen("/etc/quest1-camera", "r")) {
			if (fscanf(f, "%15s", mode) == 1)
				m = mode;
			fclose(f);
		}
	}
	if (!m || (strcmp(m, "ring") && strcmp(m, "test") && strcmp(m, "1")))
		return nullptr;
	return !strcmp(m, "1") ? "ring" : m;
}

Quest1Camera *Get()
{
	std::call_once(g_once, [] {
		const char *mode = Mode();
		if (!mode)
			return;
		auto *c = new Quest1Camera();
		if (c->Init(mode))
			g_camera = c;
		else
			delete c;
	});
	return g_camera;
}

} // namespace

void Quest1CameraAttach(PropertyContainerHandle_t container, std::function<DriverPose_t()> headPose)
{
	if (Quest1Camera *c = Get()) {
		c->Attach(container, std::move(headPose));
		if (container != k_ulInvalidPropertyContainer && atoi(Env("QUEST1_CAMERA_GATE", "1")))
			g_gate.Start();
	}
}

IVRCameraComponent *Quest1CameraComponent()
{
	return Get();
}

void Quest1CameraDetach()
{
	g_gate.Stop();
	if (g_camera)
		g_camera->Detach();
}
