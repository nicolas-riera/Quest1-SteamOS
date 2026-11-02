// Feasibility probe for the Frame-style passthrough provider (docs/passthrough.md): what can an OpenVR client do with
// the compositor's resource manager on this stack? Prints results only, never any pixel data.
//  1. is IVRCameraPassthroughInternal_001 obtainable by a client?
//  2. GetDmabufFormats / GetDmabufModifiers (needs VK_EXT_external_memory_dma_buf in vrcompositor: expected to fail here)
//  3. NewSharedVulkanImage (OPAQUE_FD, mappable) -> RefResource -> ReceiveSharedFd -> can the fd be mmap'ed?
// Usage: ptres_test   (as steamos, SteamVR running). Build: build_probe.sh
#include <openvr.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace vr;

int main()
{
	EVRInitError err = VRInitError_None;
	VR_Init(&err, getenv("PT_OVERLAY") ? VRApplication_Overlay : VRApplication_Background);
	if (err != VRInitError_None) {
		printf("VR_Init: %s\n", VR_GetVRInitErrorAsEnglishDescription(err));
		return 1;
	}
	EVRInitError e2 = VRInitError_None;
	void *pt = VR_GetGenericInterface("IVRCameraPassthroughInternal_001", &e2);
	printf("IVRCameraPassthroughInternal_001: %p (err %d)\n", pt, (int)e2);
	auto *rm = (IVRIPCResourceManagerClient *)VR_GetGenericInterface(IVRIPCResourceManagerClient_Version, &e2);
	printf("IVRIPCResourceManagerClient_003: %p (err %d)\n", (void *)rm, (int)e2);
	if (!rm)
		return 2;
	uint32_t nf = 0;
	bool ok = rm->GetDmabufFormats(&nf, nullptr);
	printf("GetDmabufFormats: %d, count %u\n", ok, nf);
	if (ok && nf) {
		uint32_t f[64] = {0};
		uint32_t n = nf > 64 ? 64 : nf;
		rm->GetDmabufFormats(&n, f);
		for (uint32_t i = 0; i < n; i++)
			printf("  format 0x%08x\n", f[i]);
	}
	SharedTextureHandle_t h = 0;
	for (int combo = 0; combo < 4; combo++) {
		bool renderable = combo & 1, mappable = combo & 2;
		SharedTextureHandle_t t = 0;
		bool r = rm->NewSharedVulkanImage(37 /*VK_FORMAT_R8G8B8A8_UNORM*/, 640, 480, renderable, mappable, false, 1, 1, 0, 0, &t);
		printf("NewSharedVulkanImage(renderable %d mappable %d): %d handle 0x%llx\n", renderable, mappable, r,
		       (unsigned long long)t);
		if (r && !h)
			h = t;
		else if (r)
			rm->UnrefResource(t);
	}
	ok = h != 0;
	SharedTextureHandle_t b = 0;
	printf("NewSharedVulkanBuffer(1MB): %d\n", rm->NewSharedVulkanBuffer(1 << 20, 0x3, &b));
	if (ok) {
		uint64_t ipc = 0;
		ok = rm->RefResource(h, &ipc);
		printf("RefResource: %d ipc 0x%llx\n", ok, (unsigned long long)ipc);
		int fd = -1;
		if (ok && rm->ReceiveSharedFd(ipc, &fd)) {
			struct stat st;
			int sr = fstat(fd, &st);
			printf("fd %d: fstat %d size %lld mode 0%o\n", fd, sr, (long long)st.st_size, st.st_mode);
			char lnk[64], tgt[256] = "";
			snprintf(lnk, sizeof(lnk), "/proc/self/fd/%d", fd);
			ssize_t n = readlink(lnk, tgt, sizeof(tgt) - 1);
			if (n > 0)
				tgt[n] = 0;
			printf("  target: %s\n", tgt);
			size_t len = st.st_size > 0 ? (size_t)st.st_size : 640 * 480 * 4;
			void *m = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
			printf("  mmap rw %zu: %s\n", len, m == MAP_FAILED ? strerror(errno) : "ok");
			if (m != MAP_FAILED)
				munmap(m, len);
			close(fd);
		} else {
			printf("ReceiveSharedFd failed\n");
		}
		rm->UnrefResource(h);
	}
	VR_Shutdown();
	return 0;
}
