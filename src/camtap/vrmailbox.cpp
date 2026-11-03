// vrmailbox <mailbox> '<json>' [wait_s]: send one message to a SteamVR mailbox through vrclient's IVRMailbox_001,
// the channel SteamVR's own UI uses (its web pages go through vrserver's web socket, which needs a token).
// vrcompositor's system layer listens on "vrcompositor_systemlayer" (CSystemLayer::RegisterMailboxHandlers), e.g.
//   vrmailbox vrcompositor_systemlayer '{"type":"set_camera_roomview_enabled_forsceneapps","enabled":true}'
// Messages sent back to us are printed for wait_s seconds (default 1).
// IVRMailbox_001 is not in the public SDK: the layout below is the one Proton's vrclient bindings use.
#include <openvr.h>

#include <cstdio>
#include <cstdlib>
#include <unistd.h>

using namespace vr;

class IVRMailbox_001
{
public:
	virtual int RegisterMailbox(const char *name, uint64_t *handle) = 0;
	virtual int UndoRegisterMailbox(uint64_t handle) = 0;
	virtual int SendMessage(uint64_t handle, const char *address, const char *message) = 0;
	virtual int ReadMessage(uint64_t handle, char *buffer, uint32_t size, uint32_t *bytes) = 0;
};

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <mailbox> '<json>' [wait_s]\n", argv[0]);
		return 2;
	}
	const double wait = argc > 3 ? atof(argv[3]) : 1.0;
	EVRInitError e = VRInitError_None;
	VR_Init(&e, VRApplication_Utility);
	if (e != VRInitError_None) {
		fprintf(stderr, "VR_Init: %d\n", (int)e);
		return 1;
	}
	auto *mb = (IVRMailbox_001 *)VR_GetGenericInterface("IVRMailbox_001", &e);
	if (!mb) {
		fprintf(stderr, "no IVRMailbox_001: %d\n", (int)e);
		VR_Shutdown();
		return 1;
	}
	uint64_t h = 0;
	int r = mb->RegisterMailbox("q1_vrmailbox", &h);
	printf("register: %d\n", r);
	r = mb->SendMessage(h, argv[1], argv[2]);
	printf("send: %d\n", r);
	for (int i = 0; i < (int)(wait * 20); i++) {
		static char buf[65536];
		uint32_t n = 0;
		while (mb->ReadMessage(h, buf, sizeof(buf) - 1, &n) == 0 && n) {
			buf[n < sizeof(buf) ? n : sizeof(buf) - 1] = 0;
			printf("reply: %s\n", buf);
			n = 0;
		}
		usleep(50000);
	}
	mb->UndoRegisterMailbox(h);
	VR_Shutdown();
	return 0;
}
