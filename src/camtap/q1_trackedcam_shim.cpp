// libq1trackedcam.so: serves SteamVR's dashboard camera test (Settings > Camera > test) from the camera ring.
//
// Why: the dashboard's camera test (vrwebhelper: StartCameraTest / GetCameraTestFrame) only uses the public
// IVRTrackedCamera_006: AcquireVideoStreamingService, GetCameraFrameSize and GetVideoStreamFrameBuffer. In SteamVR 2.17
// the frames of that path are read from a "VR_TrackedCameraExternalService" shared segment that nothing in the shipped
// binaries ever fills (docs/passthrough.md, "The camera test"): the call returns InvalidHandle. So this library answers
// those three calls itself, from /dev/shm/quest1-cameras (src/steamvr-quest1/quest1_camera_ring.h), and forwards every
// other call to the real interface. Nothing of Valve's is patched or copied.
//
// How: LD_PRELOAD into vrwebhelper (device/holo-steamvr.sh adds it to vrwebhelper.sh when /etc/quest1-trackedcam-shim
// exists; off by default). It interposes VR_GetGenericInterface and wraps the object returned for "IVRTrackedCamera_006".
// The picture: the four cameras in their raw orientation, 2x2 (0 1 / 2 3, as the camera ring numbers them), each
// 640x480 averaged down 5x5 to 128x96: 256x192 RGBX, brightened by an auto gain and a gamma. 256 columns because the
// dashboard (vrwebhelper's GetCameraTestFrame) PNG-encodes a frame of up to 256 columns as is, and a wider one by
// keeping every (width / 256)-th column of the first 256 x step only: 480 columns came out cropped to their first 256
// and stretched, and encoding the 480x1280 frame ten times a second made the whole dashboard panel lag (user,
// 2026-11-17).
//
// Env: QUEST1_TRACKEDCAM_SHIM=1 (required, set by vrwebhelper.sh), QUEST1_TRACKEDCAM_RING (default ring path),
// QUEST1_TRACKEDCAM_GAMMA (default 0.6).
#include "../steamvr-quest1/quest1_camera_ring.h"

#include <openvr.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

using namespace vr;

namespace {

constexpr int kDown = 5;                                    // 640x480 -> 128x96
constexpr int kSubW = 640 / kDown, kSubH = 480 / kDown;     // one camera
constexpr int kFrameW = kSubW * 2, kFrameH = kSubH * 2;     // 2x2: 256x192
constexpr TrackedCameraHandle_t kHandle = 0x51c1;

int64_t MonoNs()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void Log(const char *msg)
{
	fprintf(stderr, "[q1trackedcam] %s\n", msg);
}

//! Reads the newest frame set of the ring and composes the test picture.
class RingPicture
{
public:
	//! Fills out (kFrameW * kFrameH RGBX) with the newest frame set; false when there is none (yet).
	bool Get(uint8_t *out, uint32_t *sequence, int64_t *captureNs)
	{
		std::lock_guard<std::mutex> lock(mutex);
		Heartbeat();
		if (!hdr && !Map())
			return false;
		uint32_t n = __atomic_load_n(&hdr->write_count, __ATOMIC_ACQUIRE);
		if (n != lastCount) {
			lastCount = n;
			lastChange = MonoNs();
		} else if (MonoNs() - lastChange > 2000000000LL) { // producer stopped or restarted: remap next time
			Unmap();
			return false;
		}
		if (!n)
			return false;
		if (n != composedCount) {
			if (!Compose(n))
				return false;
			composedCount = n;
		}
		memcpy(out, picture.data(), picture.size());
		*sequence = composedCount;
		*captureNs = captureNsLast;
		return true;
	}

private:
	//! libq1camtap copies frames only while this file's mtime is less than 3 s old
	void Heartbeat()
	{
		int64_t now = MonoNs();
		if (now - lastBeat < 1000000000LL)
			return;
		lastBeat = now;
		std::string p = path() + ".want";
		int fd = open(p.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0666);
		if (fd >= 0) {
			futimens(fd, nullptr);
			close(fd);
		}
	}
	static std::string path()
	{
		const char *p = getenv("QUEST1_TRACKEDCAM_RING");
		return p && *p ? p : Q1CAM_RING_DEFAULT_PATH;
	}
	bool Map()
	{
		int64_t now = MonoNs();
		if (now - lastTry < 500000000LL)
			return false;
		lastTry = now;
		int fd = open(path().c_str(), O_RDONLY | O_CLOEXEC);
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
		    (uint64_t)h->header_size + (uint64_t)h->slot_count * h->slot_bytes > (uint64_t)st.st_size) {
			munmap(m, st.st_size);
			return false;
		}
		hdr = h;
		mapSize = st.st_size;
		lastCount = h->write_count;
		lastChange = now;
		composedCount = 0;
		return true;
	}
	void Unmap()
	{
		if (hdr)
			munmap((void *)hdr, mapSize);
		hdr = nullptr;
	}

	bool Compose(uint32_t n)
	{
		uint32_t slotIndex = (n - 1) % hdr->slot_count;
		const q1cam_ring_slot &s = hdr->slot[slotIndex];
		uint32_t seq0 = __atomic_load_n(&s.seq, __ATOMIC_ACQUIRE);
		if ((seq0 & 1) || (s.frame_type != Q1CAM_FRAME_HEADSET && s.frame_type != 0))
			return false;
		const uint8_t *base = (const uint8_t *)hdr + hdr->header_size + (size_t)slotIndex * hdr->slot_bytes;
		const q1cam_ring_cam *cams[4] = {nullptr, nullptr, nullptr, nullptr};
		for (int i = 0; i < 4; i++)
			for (uint32_t c = 0; c < s.ncams && c < Q1CAM_MAX_CAMS; c++) {
				const q1cam_ring_cam &rc = s.cam[c];
				if (rc.present && (int)rc.cam_id == i && rc.width == 640 && rc.height == 480 &&
				    (uint64_t)rc.offset + (uint64_t)rc.stride * rc.height <= hdr->slot_bytes)
					cams[i] = &rc;
			}
		if (!cams[0] && !cams[1] && !cams[2] && !cams[3])
			return false;
		picture.resize((size_t)kFrameW * kFrameH * 4);
		// each camera averaged down kDown x kDown (sums of 25 pixels), and the mean for the auto gain from those
		static uint16_t sums[4][kSubH][kSubW];
		uint64_t total = 0, cnt = 0;
		for (int i = 0; i < 4; i++) {
			if (!cams[i])
				continue;
			const uint8_t *src = base + cams[i]->offset;
			size_t stride = cams[i]->stride;
			for (int y = 0; y < kSubH; y++)
				for (int x = 0; x < kSubW; x++) {
					unsigned t = 0;
					for (int dy = 0; dy < kDown; dy++) {
						const uint8_t *r = src + (size_t)(y * kDown + dy) * stride + x * kDown;
						for (int dx = 0; dx < kDown; dx++)
							t += r[dx];
					}
					sums[i][y][x] = (uint16_t)t;
					total += t;
				}
			cnt += (uint64_t)kSubW * kSubH * kDown * kDown;
		}
		// auto gain: bring the mean to about 100 (the frames come with the SLAM exposure)
		float mean = cnt ? (float)total / (float)cnt : 100.f;
		float gain = std::clamp(100.f / std::max(mean, 1.f), 0.5f, 8.f);
		float gamma = (float)atof(getenv("QUEST1_TRACKEDCAM_GAMMA") ? getenv("QUEST1_TRACKEDCAM_GAMMA") : "0.6");
		if (!(gamma > 0.1f && gamma < 4.f))
			gamma = 0.6f;
		uint8_t lut[256];
		for (int v = 0; v < 256; v++)
			lut[v] = (uint8_t)std::lround(255.f * std::pow(std::min(1.f, v * gain / 255.f), gamma));
		for (int i = 0; i < 4; i++)
			for (int y = 0; y < kSubH; y++) {
				uint8_t *row = picture.data() + ((size_t)((i / 2) * kSubH + y) * kFrameW + (i % 2) * kSubW) * 4;
				for (int x = 0; x < kSubW; x++) {
					uint8_t v = cams[i] ? lut[(sums[i][y][x] + kDown * kDown / 2) / (kDown * kDown)] : 0;
					row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = v;
					row[x * 4 + 3] = 255;
				}
			}
		captureNsLast = s.capture_mono_ns ? s.capture_mono_ns : s.arrival_mono_ns;
		return __atomic_load_n(&hdr->slot[slotIndex].seq, __ATOMIC_ACQUIRE) == seq0; // torn: try again next call
	}

	std::mutex mutex;
	const q1cam_ring_header *hdr = nullptr;
	size_t mapSize = 0;
	uint32_t lastCount = 0, composedCount = 0;
	int64_t lastTry = 0, lastChange = 0, lastBeat = 0, captureNsLast = 0;
	std::vector<uint8_t> picture;
};

RingPicture g_picture;

//! The wrapper: the three calls of the camera test come from the ring, everything else goes to the real object.
class TrackedCameraShim : public IVRTrackedCamera
{
public:
	explicit TrackedCameraShim(IVRTrackedCamera *real) : real(real) {}

	const char *GetCameraErrorNameFromEnum(EVRTrackedCameraError e) override { return real->GetCameraErrorNameFromEnum(e); }
	EVRTrackedCameraError HasCamera(TrackedDeviceIndex_t d, bool *has) override { return real->HasCamera(d, has); }
	EVRTrackedCameraError GetCameraFrameSize(TrackedDeviceIndex_t d, EVRTrackedCameraFrameType t, uint32_t *w, uint32_t *h,
	                                         uint32_t *bytes) override
	{
		if (d != k_unTrackedDeviceIndex_Hmd)
			return real->GetCameraFrameSize(d, t, w, h, bytes);
		if (w)
			*w = kFrameW;
		if (h)
			*h = kFrameH;
		if (bytes)
			*bytes = kFrameW * kFrameH * 4;
		return VRTrackedCameraError_None;
	}
	EVRTrackedCameraError GetCameraIntrinsics(TrackedDeviceIndex_t d, uint32_t c, EVRTrackedCameraFrameType t, HmdVector2_t *f,
	                                          HmdVector2_t *ctr) override
	{
		return real->GetCameraIntrinsics(d, c, t, f, ctr);
	}
	EVRTrackedCameraError GetCameraProjection(TrackedDeviceIndex_t d, uint32_t c, EVRTrackedCameraFrameType t, float zn,
	                                          float zf, HmdMatrix44_t *p) override
	{
		return real->GetCameraProjection(d, c, t, zn, zf, p);
	}
	EVRTrackedCameraError AcquireVideoStreamingService(TrackedDeviceIndex_t d, TrackedCameraHandle_t *h) override
	{
		if (d != k_unTrackedDeviceIndex_Hmd)
			return real->AcquireVideoStreamingService(d, h);
		if (!h)
			return VRTrackedCameraError_InvalidArgument;
		*h = kHandle;
		return VRTrackedCameraError_None;
	}
	EVRTrackedCameraError ReleaseVideoStreamingService(TrackedCameraHandle_t h) override
	{
		return h == kHandle ? VRTrackedCameraError_None : real->ReleaseVideoStreamingService(h);
	}
	EVRTrackedCameraError GetVideoStreamFrameBuffer(TrackedCameraHandle_t h, EVRTrackedCameraFrameType t, void *buf,
	                                                uint32_t bufSize, CameraVideoStreamFrameHeader_t *hdr,
	                                                uint32_t hdrSize) override
	{
		if (h != kHandle)
			return real->GetVideoStreamFrameBuffer(h, t, buf, bufSize, hdr, hdrSize);
		if (hdr && hdrSize != sizeof(CameraVideoStreamFrameHeader_t))
			return VRTrackedCameraError_InvalidFrameHeaderVersion;
		if (buf && bufSize < (uint32_t)(kFrameW * kFrameH * 4))
			return VRTrackedCameraError_InvalidFrameBufferSize;
		std::vector<uint8_t> tmp;
		uint8_t *dst = (uint8_t *)buf;
		if (!buf) {
			tmp.resize((size_t)kFrameW * kFrameH * 4);
			dst = tmp.data();
		}
		uint32_t seq = 0;
		int64_t ns = 0;
		if (!g_picture.Get(dst, &seq, &ns))
			return VRTrackedCameraError_NoFrameAvailable;
		if (hdr) {
			memset(hdr, 0, sizeof(*hdr));
			hdr->eFrameType = t;
			hdr->nWidth = kFrameW;
			hdr->nHeight = kFrameH;
			hdr->nBytesPerPixel = 4;
			hdr->nFrameSequence = seq;
			hdr->ulFrameExposureTime = (uint64_t)ns;
		}
		return VRTrackedCameraError_None;
	}
	EVRTrackedCameraError GetVideoStreamTextureSize(TrackedDeviceIndex_t d, EVRTrackedCameraFrameType t, VRTextureBounds_t *b,
	                                                uint32_t *w, uint32_t *h) override
	{
		return real->GetVideoStreamTextureSize(d, t, b, w, h);
	}
	EVRTrackedCameraError GetVideoStreamTextureD3D11(TrackedCameraHandle_t h, EVRTrackedCameraFrameType t, void *dev, void **view,
	                                                 CameraVideoStreamFrameHeader_t *hdr, uint32_t hdrSize) override
	{
		return real->GetVideoStreamTextureD3D11(h, t, dev, view, hdr, hdrSize);
	}
	EVRTrackedCameraError GetVideoStreamTextureGL(TrackedCameraHandle_t h, EVRTrackedCameraFrameType t, glUInt_t *id,
	                                              CameraVideoStreamFrameHeader_t *hdr, uint32_t hdrSize) override
	{
		return real->GetVideoStreamTextureGL(h, t, id, hdr, hdrSize);
	}
	EVRTrackedCameraError ReleaseVideoStreamTextureGL(TrackedCameraHandle_t h, glUInt_t id) override
	{
		return real->ReleaseVideoStreamTextureGL(h, id);
	}
	void SetCameraTrackingSpace(ETrackingUniverseOrigin u) override { real->SetCameraTrackingSpace(u); }
	ETrackingUniverseOrigin GetCameraTrackingSpace() override { return real->GetCameraTrackingSpace(); }

private:
	IVRTrackedCamera *real;
};

} // namespace

extern "C" __attribute__((visibility("default"))) void *VR_GetGenericInterface(const char *pchInterfaceVersion, EVRInitError *peError)
{
	using Fn = void *(*)(const char *, EVRInitError *);
	static Fn next = (Fn)dlsym(RTLD_NEXT, "VR_GetGenericInterface");
	static std::atomic<TrackedCameraShim *> wrapper{nullptr};
	if (!next)
		return nullptr;
	void *real = next(pchInterfaceVersion, peError);
	const char *on = getenv("QUEST1_TRACKEDCAM_SHIM");
	if (real && on && !strcmp(on, "1") && pchInterfaceVersion && !strcmp(pchInterfaceVersion, IVRTrackedCamera_Version)) {
		TrackedCameraShim *w = wrapper.load();
		if (!w) {
			// one wrapper per process (the real object is a singleton too)
			auto *n = new TrackedCameraShim((IVRTrackedCamera *)real);
			if (wrapper.compare_exchange_strong(w, n))
				Log("IVRTrackedCamera_006 wrapped: the camera test is served from the camera ring");
			else
				delete n;
			w = wrapper.load();
		}
		return w;
	}
	return real;
}
