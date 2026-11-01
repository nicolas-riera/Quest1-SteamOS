/* frameid: LD_PRELOAD shim that decides, per call site, what Steam's Plat_IsSteamFrame_DoNotUse()
 * (libtier0_s.so) answers. Steam's own implementation is just "-steamframe" / "-deckard" on the command
 * line; this shim exists because the Steam client uses the same predicate for several things (the
 * steamui config ON_FRAME = Frame OOBE / Frame UI, the VR UI transport, ...). See docs/frame-mode.md.
 *
 * FRAMEID_LOG=<file>                    log each distinct caller (module:offset, process, answer) once
 * FRAMEID_FALSE=mod:hexoff|mod:*[,...] callers (module basename : offset in module) that get 0
 * FRAMEID_FILE=<file>                   file whose content (same syntax) replaces FRAMEID_FALSE, re-read per call
 * FRAMEID_DEFAULT=0|1                   answer for the other callers (default 1)
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *base(const char *p)
{
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}

static void *seen[512];
static int nseen;

static unsigned char Plat_IsSteamFrame_DoNotUse_impl(void *ret) __attribute__((noinline));

static unsigned char Plat_IsSteamFrame_DoNotUse_impl(void *ret)
{
	Dl_info di;
	char key[256] = "?";
	int answer = 1;
	const char *def = getenv("FRAMEID_DEFAULT");
	if (def && *def == '0')
		answer = 0;
	if (dladdr(ret, &di) && di.dli_fname)
		snprintf(key, sizeof key, "%s:%lx", base(di.dli_fname), (unsigned long)((char *)ret - (char *)di.dli_fbase));
	const char *list = getenv("FRAMEID_FALSE");
	char filebuf[1024];
	const char *lf = getenv("FRAMEID_FILE");
	if (lf) { /* live override while experimenting: file content replaces FRAMEID_FALSE */
		int fd = open(lf, O_RDONLY);
		if (fd >= 0) {
			ssize_t n = read(fd, filebuf, sizeof filebuf - 1);
			close(fd);
			if (n > 0) {
				filebuf[n] = 0;
				while (n > 0 && (filebuf[n - 1] == 10 || filebuf[n - 1] == 32))
					filebuf[--n] = 0;
				list = filebuf;
			}
		}
	}
	if (list) {
		/* tokens are "module:hexoff" or "module:*" (every caller in that module) */
		char mod[128] = "";
		const char *colon = strchr(key, ':');
		if (colon && (size_t)(colon - key) < sizeof mod) {
			memcpy(mod, key, colon - key);
			mod[colon - key] = 0;
		}
		size_t kl = strlen(key), ml = strlen(mod);
		for (const char *p = list; *p;) {
			const char *e = strchr(p, ',');
			size_t tl = e ? (size_t)(e - p) : strlen(p);
			if ((tl == kl && !strncmp(p, key, tl)) ||
			    (tl == ml + 2 && !strncmp(p, mod, ml) && p[ml] == ':' && p[ml + 1] == '*')) {
				answer = 0;
				break;
			}
			p += tl + (e ? 1 : 0);
		}
	}
	const char *log = getenv("FRAMEID_LOG");
	if (log) {
		int i;
		for (i = 0; i < nseen; i++)
			if (seen[i] == ret)
				break;
		if (i == nseen && nseen < 512) {
			seen[nseen++] = ret;
			int fd = open(log, O_WRONLY | O_APPEND | O_CREAT, 0644);
			if (fd >= 0) {
				char line[512], exe[128] = "?";
				ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
				if (n > 0)
					exe[n] = 0;
				int l = snprintf(line, sizeof line, "%s pid %d %s -> %d\n", key, getpid(), base(exe), answer);
				if (write(fd, line, l) < 0) {}
				close(fd);
			}
		}
	}
	return (unsigned char)answer;
}

/* the real symbol returns bool in w0; the wrapper keeps the caller's return address */
unsigned char Plat_IsSteamFrame_DoNotUse(void)
{
	return Plat_IsSteamFrame_DoNotUse_impl(__builtin_return_address(0));
}
