// q1marker: a world-locked overlay with a random pattern, 1.5 m ahead of the head and a bit low (test tool: a local
// reference next to the stream's picture in the same snapshot). Runs until killed.
#include <openvr.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

int main(int argc, char **argv)
{
	vr::EVRInitError err = vr::VRInitError_None;
	vr::VR_Init(&err, vr::VRApplication_Overlay);
	if (err != vr::VRInitError_None) {
		fprintf(stderr, "q1marker: VR_Init: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(err));
		return 1;
	}
	vr::IVROverlay *ov = vr::VROverlay();
	vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
	if (ov->CreateOverlay("q1.marker", "q1marker", &h) != vr::VROverlayError_None)
		return 1;
	enum { W = 128, H = 64 };
	static unsigned char px[W * H * 4];
	srand(1);
	for (int by = 0; by < H / 4; by++)
		for (int bx = 0; bx < W / 4; bx++) {
			unsigned char v = rand() & 1 ? 230 : 20;
			for (int y = 0; y < 4; y++)
				for (int x = 0; x < 4; x++) {
					unsigned char *p = px + ((by * 4 + y) * W + bx * 4 + x) * 4;
					p[0] = p[1] = p[2] = v;
					p[3] = 255;
				}
		}
	ov->SetOverlayRaw(h, px, W, H, 4);
	ov->SetOverlayWidthInMeters(h, 0.8f);
	vr::TrackedDevicePose_t pose;
	vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &pose, 1);
	const vr::HmdMatrix34_t &m = pose.mDeviceToAbsoluteTracking;
	float fx = -m.m[0][2], fz = -m.m[2][2], n = std::sqrt(fx * fx + fz * fz);
	fx /= n, fz /= n;
	float yaw = std::atan2(-fx, -fz); // the overlay faces the head
	float c = std::cos(yaw), s = std::sin(yaw);
	vr::HmdMatrix34_t t = {{{c, 0, s, m.m[0][3] + 1.5f * fx}, {0, 1, 0, m.m[1][3] - (argc > 1 ? (float)atof(argv[1]) : 0.45f)}, {-s, 0, c, m.m[2][3] + 1.5f * fz}}};
	ov->SetOverlayTransformAbsolute(h, vr::TrackingUniverseStanding, &t);
	ov->ShowOverlay(h);
	printf("q1marker: shown\n");
	fflush(stdout);
	for (;;)
		sleep(1);
}
