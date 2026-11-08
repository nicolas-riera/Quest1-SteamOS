// quest1_vkredirect: LD_PRELOAD helper of the Steam client (steam-q1) that sends dlopen("…/libvulkan.so.1") to
// quest1_vkshim, and runs Steam's UI processes behind SteamVR.
//
// Chromium's ANGLE opens the Vulkan loader from its own directory first (the Steam runtime ships
// a Khronos libvulkan.so.1 next to libGLESv2.so, which finds no ICD here), so LD_LIBRARY_PATH alone
// does not reach the shim. Anything whose basename is libvulkan.so[.1] is redirected, except the
// hybris library the shim itself opens.
//   QUEST1_VK_REDIRECT  target (default /usr/local/lib/quest1-vk-steam/libvulkan.so.1)
//
// Steam's UI (steamwebhelper: the browser, its renderers, GPU and utility processes) gets nice 10 and the lowest
// best-effort I/O priority, so that its work (an animation repaints the whole VR UI) waits behind SteamVR, Monado
// and Meta's tracking threads (nice 0) instead of sharing the CPUs with them. Its GPU work already runs at LOW
// global priority (quest1_vkshim, quest1_compat). The kernel's cgroup v2 has no cpu or io controller (4.4: memory
// and pids only), hence the per-process priorities. The games Steam starts are children of the steam client, not of
// steamwebhelper: they keep normal priorities.
//   QUEST1_STEAM_UI_NICE  the nice value (default 10; 0 leaves the priorities alone)
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

__attribute__((constructor)) static void steam_ui_priority(void)
{
	if (strcmp(program_invocation_short_name, "steamwebhelper") != 0)
		return;
	const char *e = getenv("QUEST1_STEAM_UI_NICE");
	int nice = e && *e ? atoi(e) : 10;
	if (nice <= 0)
		return;
	if (getpriority(PRIO_PROCESS, 0) < nice) // the children inherit it: set once, in the first process
		setpriority(PRIO_PROCESS, 0, nice);
	// IOPRIO_PRIO_VALUE(IOPRIO_CLASS_BE, 7) for this process (IOPRIO_WHO_PROCESS); inherited as well
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
