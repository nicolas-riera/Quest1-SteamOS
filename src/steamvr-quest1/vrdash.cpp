// vrdash [show|hide]: open or close the SteamVR dashboard without a controller (test tool).
// Build: see build.sh. Run as the SteamVR user with steamvr-q1 (LD_LIBRARY_PATH to libopenvr_api.so).
#include <openvr.h>

#include <cstdio>
#include <cstring>

int main(int argc, char **argv)
{
	bool hide = argc > 1 && strcmp(argv[1], "hide") == 0;
	vr::EVRInitError err = vr::VRInitError_None;
	vr::VR_Init(&err, vr::VRApplication_Overlay);
	if (err != vr::VRInitError_None) {
		fprintf(stderr, "vrdash: VR_Init: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(err));
		return 1;
	}
	vr::IVROverlay *ov = vr::VROverlay();
	printf("vrdash: dashboard visible: %d\n", ov->IsDashboardVisible());
	if (hide) {
		// the dashboard closes when a scene app asks for focus or on the system button; an overlay
		// app can only toggle it through the same call with a null key
		if (ov->IsDashboardVisible())
			ov->ShowDashboard(nullptr);
	} else {
		ov->ShowDashboard("");
	}
	printf("vrdash: dashboard visible: %d\n", ov->IsDashboardVisible());
	vr::VR_Shutdown();
	return 0;
}
