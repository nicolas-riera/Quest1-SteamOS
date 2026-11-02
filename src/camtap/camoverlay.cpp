// Head-locked camera view (crude passthrough) as a SteamVR overlay: route (b) of docs/passthrough.md.
// Reads the camera ring (/dev/shm/quest1-cameras, see ../steamvr-quest1/quest1_camera_ring.h), puts the two
// bottom tracking cameras (ids 0 and 2) side by side, upright, brightened, and draws them in a quad that follows
// the HMD. Shown only while /tmp/quest1-passthrough exists (touch it to show, rm to hide).
// Usage: camoverlay (as steamos, SteamVR running; retried by systemd). Env: PT_DIST (m, 1.0), PT_WIDTH (m, 1.6), PT_HZ (8), PT_RAW=1 (SetOverlayRaw: leaks memory blocks on this stack, frozen after ~200 frames), PT_DEBUG
#include <openvr.h>

#include <dirent.h>
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
	// VRApplication_Overlay would make vrclient start vrserver and the dashboard inside this service's cgroup:
	// wait until SteamVR runs, and connect as a Background app (which starts nothing)
	for (;;) {
		bool server = false, comp = false;
		if (DIR *d = opendir("/proc")) {
			while (dirent *de = readdir(d)) {
				char path[64], name[32] = "";
				snprintf(path, sizeof(path), "/proc/%s/comm", de->d_name);
				if (FILE *f = fopen(path, "r")) {
					if (fscanf(f, "%31s", name) == 1) {
						server |= !strcmp(name, "vrserver");
						comp |= !strcmp(name, "vrcompositor");
					}
					fclose(f);
				}
			}
			closedir(d);
		}
		if (server && comp)
			break;
		sleep(3);
	}
	sleep(10);
	EVRInitError err = VRInitError_None;
	VR_Init(&err, VRApplication_Background);
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

	const int64_t pushNs = 1000000000LL / (getenv("PT_HZ") ? atoi(getenv("PT_HZ")) : 8);
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
		static int64_t lastPush = 0;
		timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		int64_t now = ts.tv_sec * 1000000000LL + ts.tv_nsec;
		if (now - lastPush < pushNs)
			continue;
		lastPush = now;
		if (!getenv("PT_RAW")) { // 32-bit uncompressed TGA (bottom-up BGRA) through SetOverlayFromFile
			static std::vector<uint8_t> tga;
			if (tga.empty()) {
				tga.assign(18 + (size_t)OW * H * 4, 0);
				tga[2] = 2;
				tga[12] = OW & 255, tga[13] = OW >> 8, tga[14] = H & 255, tga[15] = H >> 8;
				tga[16] = 32, tga[17] = 8; // 8 alpha bits, bottom-left origin
			}
			for (int y = 0; y < H; y++)
				for (int x = 0; x < OW; x++) {
					const uint8_t *sp = &out[((size_t)y * OW + x) * 4];
					uint8_t *dp = &tga[18 + ((size_t)(H - 1 - y) * OW + x) * 4];
					dp[0] = sp[2], dp[1] = sp[1], dp[2] = sp[0], dp[3] = 255;
				}
			static const char *tmp = "/dev/shm/q1-passthrough.tga.tmp", *dst = "/dev/shm/q1-passthrough.tga";
			if (FILE *f = fopen(tmp, "wb")) {
				fwrite(tga.data(), 1, tga.size(), f);
				fclose(f);
				rename(tmp, dst);
			}
			VROverlay()->SetOverlayFromFile(ov, dst);
		} else
			VROverlay()->SetOverlayRaw(ov, out.data(), OW, H, 4);
		static int pushes = 0;
		if (getenv("PT_DEBUG") && ++pushes % 10 == 0) {
			int fds = 0;
			if (DIR *d = opendir("/proc/self/fd")) {
				while (readdir(d))
					fds++;
				closedir(d);
			}
			fprintf(stderr, "pushes %d fds %d\n", pushes, fds);
		}
	}
}
