// Head-locked camera view (crude passthrough) as a SteamVR overlay: route (b) of docs/passthrough.md.
// Reads the camera ring (/dev/shm/quest1-cameras, see ../steamvr-quest1/quest1_camera_ring.h), puts the two
// bottom tracking cameras (ids 0 and 2) side by side, upright, brightened, and draws them in a quad that follows
// the HMD. Shown only while /tmp/quest1-passthrough exists (touch it to show, rm to hide).
// Usage: camoverlay   (as the steamos user, with SteamVR running). Env: PT_DIST (m, 1.0), PT_WIDTH (m, 1.6)
#include <openvr.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../steamvr-quest1/quest1_camera_ring.h"

using namespace vr;

static const char *kRing = Q1CAM_RING_DEFAULT_PATH;
static const char *kFlag = "/tmp/quest1-passthrough";

static void Heartbeat()
{
	std::string p = std::string(kRing) + ".want";
	int fd = open(p.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0666);
	if (fd >= 0) {
		futimens(fd, nullptr);
		close(fd);
	}
}

int main()
{
	EVRInitError err = VRInitError_None;
	VR_Init(&err, VRApplication_Overlay);
	if (err != VRInitError_None) {
		fprintf(stderr, "VR_Init: %s\n", VR_GetVRInitErrorAsEnglishDescription(err));
		return 1;
	}
	VROverlayHandle_t ov;
	VROverlay()->CreateOverlay("quest1.passthrough", "Passthrough", &ov);
	float dist = getenv("PT_DIST") ? atof(getenv("PT_DIST")) : 1.0f;
	VROverlay()->SetOverlayWidthInMeters(ov, getenv("PT_WIDTH") ? atof(getenv("PT_WIDTH")) : 1.6f);
	HmdMatrix34_t m = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, -dist}}};
	VROverlay()->SetOverlayTransformTrackedDeviceRelative(ov, k_unTrackedDeviceIndex_Hmd, &m);
	VROverlay()->SetOverlayFlag(ov, VROverlayFlags_SortWithNonSceneOverlays, true);
	VROverlay()->SetOverlaySortOrder(ov, 100);
	VROverlay()->SetOverlayAlpha(ov, 1.0f);

	const q1cam_ring_header *hdr = nullptr;
	size_t mapSize = 0;
	uint32_t last = 0;
	bool shown = false;
	// output: two 480x640 upright images side by side
	const int W = 480, H = 640, OW = W * 2;
	std::vector<uint8_t> out(OW * H * 4, 255);
	uint8_t lut[256];
	for (int i = 0; i < 256; i++)
		lut[i] = (uint8_t)std::min(255.0, 255.0 * pow(i / 255.0, 0.55) * 1.4);
	for (;;) {
		bool want = access(kFlag, F_OK) == 0;
		if (want != shown) {
			shown = want;
			if (want)
				VROverlay()->ShowOverlay(ov);
			else
				VROverlay()->HideOverlay(ov);
		}
		if (!want) {
			usleep(200000);
			continue;
		}
		Heartbeat();
		if (!hdr) {
			int fd = open(kRing, O_RDONLY | O_CLOEXEC);
			struct stat st;
			if (fd >= 0 && !fstat(fd, &st) && st.st_size >= (off_t)sizeof(q1cam_ring_header)) {
				void *p = mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
				if (p != MAP_FAILED) {
					const q1cam_ring_header *h = (const q1cam_ring_header *)p;
					if (h->magic == Q1CAM_RING_MAGIC && h->version == Q1CAM_RING_VERSION) {
						hdr = h;
						mapSize = st.st_size;
					} else {
						munmap(p, st.st_size);
					}
				}
			}
			if (fd >= 0)
				close(fd);
			if (!hdr) {
				usleep(500000);
				continue;
			}
		}
		uint32_t n = __atomic_load_n(&hdr->write_count, __ATOMIC_ACQUIRE);
		if (n == last || n == 0) {
			usleep(8000);
			continue;
		}
		last = n;
		uint32_t si = (n - 1) % hdr->slot_count;
		const q1cam_ring_slot &s = hdr->slot[si];
		uint32_t seq0 = __atomic_load_n(&s.seq, __ATOMIC_ACQUIRE);
		if ((seq0 & 1) || (s.frame_type != Q1CAM_FRAME_HEADSET && s.frame_type != 0))
			continue;
		const uint8_t *base = (const uint8_t *)hdr + hdr->header_size + (size_t)si * hdr->slot_bytes;
		const int ids[2] = {0, 2};
		for (int e = 0; e < 2; e++)
			for (uint32_t c = 0; c < s.ncams && c < Q1CAM_MAX_CAMS; c++) {
				const q1cam_ring_cam &rc = s.cam[c];
				if (!rc.present || (int)rc.cam_id != ids[e] || rc.width != 640 || rc.height != 480)
					continue;
				const uint8_t *src = base + rc.offset;
				// quarter turn (3 x 90 deg clockwise, as the driver's upright rotation): out(xr, yr) = raw(w-1-yr, xr)
				for (int yr = 0; yr < H; yr++)
					for (int xr = 0; xr < W; xr++) {
						uint8_t v = lut[src[(size_t)xr * rc.stride + (639 - yr)]];
						uint8_t *d = &out[((size_t)yr * OW + e * W + xr) * 4];
						d[0] = d[1] = d[2] = v;
						d[3] = 255;
					}
			}
		if (__atomic_load_n(&s.seq, __ATOMIC_ACQUIRE) != seq0)
			continue;
		VROverlay()->SetOverlayRaw(ov, out.data(), OW, H, 4);
	}
}
