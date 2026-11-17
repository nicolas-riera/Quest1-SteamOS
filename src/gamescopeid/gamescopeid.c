/* gamescopeid: LD_PRELOAD shim for Steam's Plat_IsGamescope() (libtier0_s.so), which is just
 * getenv("GAMESCOPE_WAYLAND_DISPLAY") != "". The Steam UI's quick menu shows its Performance tab inside VR only
 * when the web UI's IN_GAMESCOPE is true (steamui.js, QuickAccess tabs: `(!IsAnyVRWindow || IN_GAMESCOPE)`), and
 * steamui.so computes IN_GAMESCOPE from that one call (the "&IN_GAMESCOPE=true" config parameter). Setting the
 * variable for the whole client is not an option: the 40-odd other callers then take gamescope paths (Remote Play's
 * capture reads gamescope's PipeWire node instead of the ScreenCast portal, wayland init, X atoms...). So only the
 * call sites listed here answer yes, every other caller gets libtier0's real answer.
 *
 * GAMESCOPEID_TRUE=mod:hexoff[,...]  call sites (module basename : return address offset in module) that get 1
 *                                     default: steamui.so:1009604 (client build of 2026-11; the offset moves with each
 *                                     client update: GAMESCOPEID_LOG shows the callers, docs/frame-features.md)
 * GAMESCOPEID_LOG=<file>             log each distinct caller (module:offset, process, answer) once
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_TRUE "steamui.so:1009604"

static void *seen[256];
static int nseen;

static unsigned char impl(void *ret) __attribute__((noinline));
static unsigned char impl(void *ret)
{
	Dl_info di;
	char key[256] = "?";
	if (dladdr(ret, &di) && di.dli_fname) {
		const char *s = strrchr(di.dli_fname, '/');
		snprintf(key, sizeof key, "%s:%lx", s ? s + 1 : di.dli_fname, (unsigned long)((char *)ret - (char *)di.dli_fbase));
	}
	const char *list = getenv("GAMESCOPEID_TRUE");
	if (!list)
		list = DEFAULT_TRUE;
	int answer = 0;
	size_t kl = strlen(key);
	for (const char *p = list; *p;) {
		const char *e = strchr(p, ',');
		size_t tl = e ? (size_t)(e - p) : strlen(p);
		if (tl == kl && !strncmp(p, key, tl))
			answer = 1;
		p += tl + (e ? 1 : 0);
	}
	if (!answer) {
		static unsigned char (*real)(void);
		if (!real)
			real = (unsigned char (*)(void))dlsym(RTLD_NEXT, "Plat_IsGamescope");
		answer = real ? real() : 0;
	}
	const char *log = getenv("GAMESCOPEID_LOG");
	if (log) {
		int i;
		for (i = 0; i < nseen; i++)
			if (seen[i] == ret)
				break;
		if (i == nseen && nseen < 256) {
			seen[nseen++] = ret;
			int fd = open(log, O_WRONLY | O_APPEND | O_CREAT, 0644);
			if (fd >= 0) {
				char line[512], exe[128] = "?";
				ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
				if (n > 0)
					exe[n] = 0;
				int l = snprintf(line, sizeof line, "%s pid %d %s -> %d\n", key, getpid(), strrchr(exe, '/') ? strrchr(exe, '/') + 1 : exe, answer);
				if (write(fd, line, l) < 0) {}
				close(fd);
			}
		}
	}
	return (unsigned char)answer;
}

unsigned char Plat_IsGamescope(void)
{
	return impl(__builtin_return_address(0));
}
