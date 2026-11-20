// q1handviz: shows Meta's tracked hands in SteamVR (docs/hand-tracking.md): a joint skeleton overlay on each tracked
// hand and, while the palm faces the user, the small Oculus (right) / Menu (left) mark near it, as Meta's OS draws for
// the system gesture. Reads what driver_quest1 publishes (handviz_shm.h) and puts overlays on the hand's tracked
// device. Textures go through SetOverlayFromFile (the path that does not leak, camoverlay.cpp). Nothing is drawn
// without a tracked hand. Run as steamos with SteamVR running.
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <openvr.h>

#include "../steamvr-quest1/handviz_shm.h"

using namespace vr;

namespace {
const int kSkel = 192, kMark = 96;
// parent of each of Meta's 24 joints: wrist 0, forearm 1, thumb 2-5, index 6-8, middle 9-11, ring 12-14, pinky 15-18,
// tips 19-23 (docs/hand-tracking.md)
const int kParent[24] = {-1, 0, 0, 2, 3, 4, 0, 6, 7, 0, 9, 10, 0, 12, 13, 0, 15, 16, 17, 5, 8, 11, 14, 18};

struct V3
{
	double x, y, z;
};
V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 Norm(V3 a)
{
	double n = sqrt(Dot(a, a));
	return n > 1e-9 ? V3{a.x / n, a.y / n, a.z / n} : V3{0, 1, 0};
}
V3 QRot(const float *q, V3 v) // q = w x y z
{
	V3 u{q[1], q[2], q[3]};
	V3 c = Cross(u, v), cc = Cross(u, c);
	return {v.x + 2 * (q[0] * c.x + cc.x), v.y + 2 * (q[0] * c.y + cc.y), v.z + 2 * (q[0] * c.z + cc.z)};
}

struct Img
{
	int w, h;
	std::vector<uint8_t> px; // RGBA
	Img(int w, int h) : w(w), h(h), px(w * h * 4, 0) {}
	void Disc(int cx, int cy, int r, uint8_t R, uint8_t G, uint8_t B)
	{
		for (int y = cy - r; y <= cy + r; y++)
			for (int x = cx - r; x <= cx + r; x++)
				if (x >= 0 && y >= 0 && x < w && y < h && (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) {
					uint8_t *p = &px[(y * w + x) * 4];
					p[0] = R, p[1] = G, p[2] = B, p[3] = 255;
				}
	}
	void Line(int x0, int y0, int x1, int y1, int r, uint8_t R, uint8_t G, uint8_t B)
	{
		int n = std::max(abs(x1 - x0), abs(y1 - y0)) + 1;
		for (int i = 0; i <= n; i++)
			Disc(x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, r, R, G, B);
	}
};

void WriteTga(const Img &im, const char *tmp, const char *dst)
{
	FILE *f = fopen(tmp, "wb");
	if (!f)
		return;
	uint8_t hd[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, (uint8_t)(im.w & 255), (uint8_t)(im.w >> 8),
	                  (uint8_t)(im.h & 255), (uint8_t)(im.h >> 8), 32, 8};
	fwrite(hd, 1, 18, f);
	std::vector<uint8_t> row(im.w * 4);
	for (int y = im.h - 1; y >= 0; y--) { // bottom-up BGRA
		for (int x = 0; x < im.w; x++) {
			const uint8_t *p = &im.px[(y * im.w + x) * 4];
			row[x * 4] = p[2], row[x * 4 + 1] = p[1], row[x * 4 + 2] = p[0], row[x * 4 + 3] = p[3];
		}
		fwrite(row.data(), 1, row.size(), f);
	}
	fclose(f);
	rename(tmp, dst);
}

//! overlay transform (device-relative) of a quad in the skeleton frame: centre c, axes x, y, z (columns)
HmdMatrix34_t Place(const q1handviz_hand &h, V3 c, V3 x, V3 y, V3 z)
{
	V3 ax = QRot(h.rootRot, x), ay = QRot(h.rootRot, y), az = QRot(h.rootRot, z), t = QRot(h.rootRot, c);
	HmdMatrix34_t m = {{{(float)ax.x, (float)ay.x, (float)az.x, (float)(t.x + h.rootPos[0])},
	                    {(float)ax.y, (float)ay.y, (float)az.y, (float)(t.y + h.rootPos[1])},
	                    {(float)ax.z, (float)ay.z, (float)az.z, (float)(t.z + h.rootPos[2])}}};
	return m;
}

struct HandOv
{
	VROverlayHandle_t skel = k_ulOverlayHandleInvalid, mark = k_ulOverlayHandleInvalid;
	bool skelShown = false, markShown = false;
	int lastPinch = -1;
};
} // namespace

int main()
{
	// One VR_Init per process, once SteamVR had time to come up (as q1-perfhud); on failure the process ends and systemd
	// starts it again. Connected first, this tool created its overlays before the keep-alive vrcmd and became the client
	// that starts vrcompositor, whose thread in vrserver then spins (docs/performance.md, 2026-11-20).
	sleep(10);
	EVRInitError err = VRInitError_None;
	VR_Init(&err, VRApplication_Background);
	if (err != VRInitError_None) {
		fprintf(stderr, "VR_Init: %s\n", VR_GetVRInitErrorAsEnglishDescription(err));
		return 1;
	}
	HandOv ov[2];
	for (int h = 0; h < 2; h++) {
		char name[48];
		snprintf(name, sizeof(name), "quest1.handviz.skel.%d", h);
		VROverlay()->CreateOverlay(name, "Hand skeleton", &ov[h].skel);
		snprintf(name, sizeof(name), "quest1.handviz.mark.%d", h);
		VROverlay()->CreateOverlay(name, "System gesture mark", &ov[h].mark);
		VROverlay()->SetOverlayWidthInMeters(ov[h].skel, 0.26f);
		VROverlay()->SetOverlayWidthInMeters(ov[h].mark, 0.045f);
		for (VROverlayHandle_t o : {ov[h].skel, ov[h].mark}) {
			VROverlay()->SetOverlayFlag(o, VROverlayFlags_SortWithNonSceneOverlays, true);
			VROverlay()->SetOverlaySortOrder(o, 80);
		}
		VROverlay()->SetOverlayAlpha(ov[h].skel, 0.85f);
	}
	q1handviz_shm *m = nullptr;
	double last[2] = {0, 0};
	// No OpenVR round trip while no hand is tracked (a check every 0.5 s), and a hand's device index asked once a second
	// (it was two calls every 50 ms, hands or not).
	TrackedDeviceIndex_t devCache[2] = {k_unTrackedDeviceIndexInvalid, k_unTrackedDeviceIndexInvalid};
	double devAsked[2] = {0, 0};
	bool idle = true;
	for (;;) {
		usleep(idle ? 500000 : 1000000 / 20);
		if (!m) {
			int fd = open(Q1HANDVIZ_PATH, O_RDONLY | O_CLOEXEC);
			struct stat st;
			if (fd >= 0 && fstat(fd, &st) == 0 && (size_t)st.st_size >= sizeof(q1handviz_shm)) {
				void *p = mmap(nullptr, sizeof(q1handviz_shm), PROT_READ, MAP_SHARED, fd, 0);
				m = p == MAP_FAILED ? nullptr : (q1handviz_shm *)p;
			}
			if (fd >= 0)
				close(fd);
			if (!m)
				continue;
		}
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		double now = ts.tv_sec + ts.tv_nsec * 1e-9;
		bool fresh = now - m->updateNs * 1e-9 < 0.5 && m->magic == Q1HANDVIZ_MAGIC;
		idle = !fresh || (!m->hand[0].tracked && !m->hand[1].tracked);
		for (int h = 0; h < 2; h++) {
			q1handviz_hand c;
			uint32_t a;
			int tries = 0;
			do {
				a = m->seq;
				memcpy(&c, (const void *)&m->hand[h], sizeof(c));
			} while (((a & 1) || a != m->seq) && ++tries < 5);
			TrackedDeviceIndex_t dev = k_unTrackedDeviceIndexInvalid;
			if (fresh && c.tracked) {
				if (now - devAsked[h] > 1.0 || devCache[h] == k_unTrackedDeviceIndexInvalid) {
					devAsked[h] = now;
					devCache[h] = VRSystem()->GetTrackedDeviceIndexForControllerRole(
					    h ? TrackedControllerRole_RightHand : TrackedControllerRole_LeftHand);
				}
				dev = devCache[h];
			}
			bool on = dev != k_unTrackedDeviceIndexInvalid;
			HandOv &o = ov[h];
			if (!on) {
				if (o.skelShown)
					VROverlay()->HideOverlay(o.skel);
				if (o.markShown)
					VROverlay()->HideOverlay(o.mark);
				o.skelShown = o.markShown = false;
				continue;
			}
			V3 n = Norm({c.palmDev[0], c.palmDev[1], c.palmDev[2]});
			V3 J[24];
			for (int j = 0; j < 24; j++)
				J[j] = {c.joints[j][0], c.joints[j][1], c.joints[j][2]};
			V3 f = Sub(J[9], J[0]);
			double d = Dot(f, n);
			f = Norm({f.x - d * n.x, f.y - d * n.y, f.z - d * n.z});
			V3 x = Cross(f, n);
			V3 ctr{0.5 * (J[0].x + J[9].x), 0.5 * (J[0].y + J[9].y), 0.5 * (J[0].z + J[9].z)};
			if (now - last[h] > 1.0 / 15) { // the skeleton picture, seen from the palm side
				last[h] = now;
				Img im(kSkel, kSkel);
				const double span = 0.26; // meters across the picture
				auto pt = [&](V3 p, int *px, int *py) {
					V3 r = Sub(p, ctr);
					*px = (int)(kSkel / 2 + Dot(r, x) / span * kSkel);
					*py = (int)(kSkel / 2 - Dot(r, f) / span * kSkel);
				};
				for (int j = 2; j < 24; j++) {
					int x0, y0, x1, y1;
					pt(J[kParent[j]], &x0, &y0);
					pt(J[j], &x1, &y1);
					im.Line(x0, y0, x1, y1, 2, 120, 220, 255);
				}
				for (int j = 0; j < 24; j++) {
					int px, py;
					pt(J[j], &px, &py);
					bool tip = c.pinch && (j == 19 || j == 20);
					im.Disc(px, py, j >= 19 ? 5 : 4, 255, 255, tip ? 80 : 255);
				}
				const char *dst = h ? "/tmp/q1handviz_r.tga" : "/tmp/q1handviz_l.tga";
				WriteTga(im, "/tmp/q1handviz.tmp", dst);
				VROverlay()->SetOverlayFromFile(o.skel, dst);
			}
			HmdMatrix34_t t = Place(c, ctr, x, f, n);
			VROverlay()->SetOverlayTransformTrackedDeviceRelative(o.skel, dev, &t);
			if (!o.skelShown) {
				VROverlay()->ShowOverlay(o.skel);
				o.skelShown = true;
			}
			// the system gesture's mark: right Oculus ring, left Menu bars, near the palm while it faces the user
			if (c.palmFacing) {
				if (o.lastPinch != (int)c.pinch) {
					o.lastPinch = c.pinch;
					Img im(kMark, kMark);
					uint8_t g = c.pinch ? 255 : 200, b = c.pinch ? 120 : 255;
					if (h) {
						for (int a2 = 0; a2 < 360; a2 += 2)
							im.Disc(kMark / 2 + (int)(36 * cos(a2 * M_PI / 180)),
							        kMark / 2 + (int)(24 * sin(a2 * M_PI / 180)), 5, g, g, b);
					} else {
						for (int i = 0; i < 3; i++)
							im.Line(kMark / 4, kMark / 2 - 22 + i * 22, 3 * kMark / 4, kMark / 2 - 22 + i * 22, 5, g,
							        g, b);
					}
					const char *dst = h ? "/tmp/q1handviz_mr.tga" : "/tmp/q1handviz_ml.tga";
					WriteTga(im, "/tmp/q1handviz.tmp", dst);
					VROverlay()->SetOverlayFromFile(o.mark, dst);
				}
				V3 mc{ctr.x + n.x * 0.07, ctr.y + n.y * 0.07, ctr.z + n.z * 0.07};
				HmdMatrix34_t mt = Place(c, mc, x, f, n);
				VROverlay()->SetOverlayTransformTrackedDeviceRelative(o.mark, dev, &mt);
				if (!o.markShown) {
					VROverlay()->ShowOverlay(o.mark);
					o.markShown = true;
				}
			} else if (o.markShown) {
				VROverlay()->HideOverlay(o.mark);
				o.markShown = false;
			}
		}
	}
}
