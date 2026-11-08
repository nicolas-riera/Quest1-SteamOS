// quest1_vkredirect: LD_PRELOAD helper of the Steam client (steam-q1) that sends dlopen("…/libvulkan.so.1") to
// quest1_vkshim, and gives Steam's UI processes the lowest I/O priority.
//
// Chromium's ANGLE opens the Vulkan loader from its own directory first (the Steam runtime ships
// a Khronos libvulkan.so.1 next to libGLESv2.so, which finds no ICD here), so LD_LIBRARY_PATH alone
// does not reach the shim. Anything whose basename is libvulkan.so[.1] is redirected, except the
// hybris library the shim itself opens.
//   QUEST1_VK_REDIRECT  target (default /usr/local/lib/quest1-vk-steam/libvulkan.so.1)
//
// Steam's UI (steamwebhelper: the browser, its renderers, GPU and utility processes) gets the lowest best-effort I/O
// priority, so that its disk work (shader and HTML caches) waits behind SteamVR's and the tracking's. The kernel's
// cgroup v2 has no io controller (4.4: memory and pids only), hence the per-process priority. The games Steam starts
// are children of the steam client, not of steamwebhelper: they keep normal priorities. Its CPU priority stays
// normal: at nice 10 its threads waited for a CPU longer than they ran (2.4 s against 1.4 s in 20 s of browsing,
// 2026-11-08) and Steam was unusable, while SteamVR's timing threads are real-time and don't need it lower.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

__attribute__((constructor)) static void steam_ui_priority(void)
{
	if (strcmp(program_invocation_short_name, "steamwebhelper") != 0)
		return;
	// IOPRIO_PRIO_VALUE(IOPRIO_CLASS_BE, 7) for this process (IOPRIO_WHO_PROCESS); its children inherit it
	syscall(SYS_ioprio_set, 1, 0, (2 << 13) | 7);
}

__attribute__((visibility("default"))) void *dlopen(const char *file, int mode)
{
	static void *(*real)(const char *, int);
	if (!real)
		real = (void *(*)(const char *, int))dlsym(RTLD_NEXT, "dlopen");
	if (file) {
		const char *base = strrchr(file, '/');
		base = base ? base + 1 : file;
		const char *target = getenv("QUEST1_VK_REDIRECT");
		if (!target || !*target)
			target = "/usr/local/lib/quest1-vk-steam/libvulkan.so.1";
		if ((strcmp(base, "libvulkan.so.1") == 0 || strcmp(base, "libvulkan.so") == 0) &&
		    strncmp(file, "/opt/hybris/", 12) != 0 && strcmp(file, target) != 0)
			file = target;
	}
	return real(file, mode);
}
