// quest1_vkredirect: LD_PRELOAD helper that sends dlopen("…/libvulkan.so.1") to quest1_vkshim.
//
// Chromium's ANGLE opens the Vulkan loader from its own directory first (the Steam runtime ships
// a Khronos libvulkan.so.1 next to libGLESv2.so, which finds no ICD here), so LD_LIBRARY_PATH alone
// does not reach the shim. Anything whose basename is libvulkan.so[.1] is redirected, except the
// hybris library the shim itself opens.
//   QUEST1_VK_REDIRECT  target (default /usr/local/lib/quest1-vk-steam/libvulkan.so.1)
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

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
