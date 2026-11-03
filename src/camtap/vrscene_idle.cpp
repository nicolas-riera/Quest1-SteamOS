// vrscene_idle [seconds]: an OpenVR Scene application that only waits for poses (submits nothing), to test what
// vrcompositor does while a scene app is connected (its roomview "for scene apps" path needs one).
#include <openvr.h>

#include <cstdio>
#include <cstdlib>

using namespace vr;

int main(int argc, char **argv)
{
	const int secs = argc > 1 ? atoi(argv[1]) : 60;
	EVRInitError e = VRInitError_None;
	VR_Init(&e, VRApplication_Scene);
	if (e != VRInitError_None) {
		fprintf(stderr, "VR_Init: %d\n", (int)e);
		return 1;
	}
	TrackedDevicePose_t poses[k_unMaxTrackedDeviceCount];
	for (int i = 0; i < secs * 72; i++)
		VRCompositor()->WaitGetPoses(poses, k_unMaxTrackedDeviceCount, nullptr, 0);
	VR_Shutdown();
	return 0;
}
