// Test client: reads frames through SteamVR's IVRTrackedCamera (served by driver_quest1's camera
// component) and stores a few as PPM files. Usage: trackedcam_test <out-dir> [frames]
// Build: see build_test.sh (cross, OpenVR SDK headers, libopenvr_api.so loaded at run time).
#include <openvr.h>

#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <vector>

using namespace vr;

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "/tmp";
	int want = argc > 2 ? atoi(argv[2]) : 3;
	EVRInitError err = VRInitError_None;
	IVRSystem *sys = VR_Init(&err, (EVRApplicationType)(getenv("PT_APPTYPE") ? atoi(getenv("PT_APPTYPE")) : VRApplication_Background));
	if (!sys) {
		fprintf(stderr, "VR_Init: %s\n", VR_GetVRInitErrorAsEnglishDescription(err));
		return 1;
	}
	if (getenv("PT_TOGGLE")) {
		EVRSettingsError se;
		VRSettings()->SetBool(k_pch_Camera_Section, k_pch_Camera_EnableCamera_Bool, false, &se);
		usleep(500000);
		VRSettings()->SetBool(k_pch_Camera_Section, k_pch_Camera_EnableCamera_Bool, true, &se);
		printf("toggled camera.enableCamera (err %d)\n", (int)se);
		usleep(1000000);
	}
	if (getenv("PT_BLEND")) { // the compositor only draws the roomview when this is set (vrcompositor 0x147fd4 -> 0x63920 -> byte +3421)
		EVRSettingsError se;
		VRSettings()->SetBool("camera", "forceSceneAlphaBlending", atoi(getenv("PT_BLEND")) != 0, &se);
	}
	if (getenv("PT_ROOMVIEW")) { // toggle camera.roomView (the compositor's passthrough view)
		EVRSettingsError se;
		int mode = atoi(getenv("PT_ROOMVIEW")); // 1 = 2D, 2 = 3D (depth mesh)
		VRSettings()->SetInt32("camera", "roomView", 0, &se);
		usleep(500000);
		VRSettings()->SetInt32("camera", "roomView", mode > 0 ? mode : 1, &se);
		printf("toggled camera.roomView (err %d)\n", (int)se);
		usleep(1000000);
	}
	IVRTrackedCamera *tc = VRTrackedCamera();
	bool has = false;
	EVRTrackedCameraError e = tc->HasCamera(k_unTrackedDeviceIndex_Hmd, &has);
	printf("HasCamera: %d (err %d)\n", has, e);
	uint32_t w = 0, h = 0, sz = 0;
	EVRTrackedCameraFrameType ft = VRTrackedCameraFrameType_Undistorted;
	for (int t = 0; t < 4; t++) {
		ft = (EVRTrackedCameraFrameType)t;
		e = tc->GetCameraFrameSize(k_unTrackedDeviceIndex_Hmd, ft, &w, &h, &sz);
		printf("frame type %d: size %ux%u bytes %u err %d (%s)\n", t, w, h, sz,
		       e, tc->GetCameraErrorNameFromEnum(e));
		if (e == VRTrackedCameraError_None)
			break;
	}
	TrackedCameraHandle_t hd = INVALID_TRACKED_CAMERA_HANDLE;
	e = tc->AcquireVideoStreamingService(k_unTrackedDeviceIndex_Hmd, &hd);
	printf("Acquire: err %d (%s) handle %llu\n", e, tc->GetCameraErrorNameFromEnum(e), (unsigned long long)hd);
	if (e != VRTrackedCameraError_None)
		return 2;
	if (const char *hold = getenv("PT_HOLD")) { // keep the streaming service (and so the compositor's roomview) up for N seconds
		sleep(atoi(hold));
		tc->ReleaseVideoStreamingService(hd);
		VR_Shutdown();
		return 0;
	}
	std::vector<uint8_t> buf(sz);
	uint32_t lastSeq = 0;
	int got = 0;
	for (int i = 0; i < 300 && got < want; i++) {
		CameraVideoStreamFrameHeader_t fh{};
		e = tc->GetVideoStreamFrameBuffer(hd, ft, buf.data(), sz, &fh, sizeof(fh));
		if (e == VRTrackedCameraError_None && fh.nFrameSequence != lastSeq && fh.nWidth) {
			lastSeq = fh.nFrameSequence;
			char p[512];
			snprintf(p, sizeof(p), "%s/frame%d.ppm", dir, got);
			if (FILE *f = fopen(p, "wb")) {
				fprintf(f, "P6\n%u %u\n255\n", fh.nWidth, fh.nHeight);
				uint32_t bpp = fh.nBytesPerPixel ? fh.nBytesPerPixel : 4;
				for (uint32_t k = 0; k < fh.nWidth * fh.nHeight; k++)
					fwrite(&buf[k * bpp], 1, 3, f);
				fclose(f);
			}
			printf("frame %d: seq %u %ux%u bpp %u -> %s\n", got, fh.nFrameSequence, fh.nWidth, fh.nHeight,
			       fh.nBytesPerPixel, p);
			got++;
		} else if (i % 20 == 0) {
			printf("poll %d: err %d (%s)\n", i, e, tc->GetCameraErrorNameFromEnum(e));
		}
		usleep(100000);
	}
	tc->ReleaseVideoStreamingService(hd);
	VR_Shutdown();
	return got ? 0 : 3;
}
