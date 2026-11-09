// quest1_rpspectator: a Remote Play stream from the headset is a spectator view (the wearer's VR view, as on the
// Frame), so the PC watching it must not drive the headset. LD_PRELOAD'ed into the Steam client (steam-q1).
//
// While q1apps shows the VR view on the streamed screen (it creates /tmp/quest1-spectator for the session, see
// src/q1desktop/q1apps.cpp and docs/desktop.md "Remote Play spectator"):
//  - Steam's injection of the PC's mouse and keyboard (XTestFake* from steamui.so and steamclient.so) is dropped,
//    so they do not reach the apps under the laser or Steam's own UI. Releases still go through: a key or button
//    pressed before the session never stays stuck.
//  - The cursor image Steam draws into the stream (XFixesGetCursorImage) comes back fully transparent, so the
//    app's cursor under the laser does not show on the PC.
//  - The PC's gamepads, which the stream forwards to the headset, get no virtual gamepad there (Steam Input makes
//    one uinput device, "Steam Virtual Gamepad" 28de:11ff, per controller it drives). A gamepad plugged into the
//    headset is an input device here before Steam wraps it; the PC's are not. So during a session Steam may open
//    /dev/uinput only while the headset has more gamepads of its own than Steam holds uinput devices.
//    q1apps makes the trigger up to a second after the video stream starts, and the PC's controllers come with
//    the session's start, so a session also counts from the moment Steam's video stream thread exists.
// Outside a session everything goes through untouched. The state is looked up at most every 250 ms.
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))
#define SPECT_TRIGGER "/tmp/quest1-spectator"

static int spectating(void)
{
	static __thread long long next;
	static __thread int on;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
	long long now = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
	if (now >= next) {
		on = access(SPECT_TRIGGER, F_OK) == 0;
		next = now + 250;
	}
	return on;
}

// Steam's video stream thread (CGameStreamVideoStage..., comm cut to 15 characters) runs in this process: rare
// (only when /dev/uinput is opened), so no cache
static int streaming(void)
{
	DIR *d = opendir("/proc/self/task");
	int found = 0;
	for (struct dirent *e; d && !found && (e = readdir(d));) {
		if (e->d_name[0] == '.')
			continue;
		char path[300], comm[32] = "";
		snprintf(path, sizeof(path), "/proc/self/task/%s/comm", e->d_name);
		FILE *f = fopen(path, "re");
		if (f) {
			found = fgets(comm, sizeof(comm), f) && !strncmp(comm, "CGameStreamVide", 15);
			fclose(f);
		}
	}
	if (d)
		closedir(d);
	return found;
}

// the real function: steamui.so is dlopen'ed with its libraries in its own scope, where RTLD_NEXT does not look
static void *real_sym(const char *lib, const char *name)
{
	void *f = dlsym(RTLD_NEXT, name);
	if (!f) {
		void *h = dlopen(lib, RTLD_NOW | RTLD_NOLOAD);
		if (!h)
			h = dlopen(lib, RTLD_NOW);
		if (h)
			f = dlsym(h, name);
	}
	return f;
}

#define REAL_IN(lib, name) \
	static __typeof__(name) *real_##name; \
	if (!real_##name) \
		real_##name = (__typeof__(name) *)real_sym(lib, #name)
#define REAL(name) REAL_IN("libXtst.so.6", name)

EXPORT int XTestFakeKeyEvent(Display *d, unsigned int keycode, Bool is_press, unsigned long delay)
{
	REAL(XTestFakeKeyEvent);
	if (is_press && spectating())
		return 1;
	return real_XTestFakeKeyEvent ? real_XTestFakeKeyEvent(d, keycode, is_press, delay) : 0;
}

EXPORT int XTestFakeButtonEvent(Display *d, unsigned int button, Bool is_press, unsigned long delay)
{
	REAL(XTestFakeButtonEvent);
	if (is_press && spectating())
		return 1;
	return real_XTestFakeButtonEvent ? real_XTestFakeButtonEvent(d, button, is_press, delay) : 0;
}

EXPORT int XTestFakeMotionEvent(Display *d, int screen, int x, int y, unsigned long delay)
{
	REAL(XTestFakeMotionEvent);
	if (spectating())
		return 1;
	return real_XTestFakeMotionEvent ? real_XTestFakeMotionEvent(d, screen, x, y, delay) : 0;
}

EXPORT int XTestFakeRelativeMotionEvent(Display *d, int dx, int dy, unsigned long delay)
{
	REAL(XTestFakeRelativeMotionEvent);
	if (spectating())
		return 1;
	return real_XTestFakeRelativeMotionEvent ? real_XTestFakeRelativeMotionEvent(d, dx, dy, delay) : 0;
}

EXPORT XFixesCursorImage *XFixesGetCursorImage(Display *d)
{
	REAL_IN("libXfixes.so.3", XFixesGetCursorImage);
	XFixesCursorImage *c = real_XFixesGetCursorImage ? real_XFixesGetCursorImage(d) : NULL;
	if (c && c->pixels && spectating())
		for (unsigned i = 0; i < (unsigned)c->width * c->height; i++)
			c->pixels[i] = 0; // ARGB, alpha 0
	return c;
}

static int read_line(const char *path, char *buf, size_t n)
{
	FILE *f = fopen(path, "re");
	if (!f)
		return 0;
	int ok = fgets(buf, (int)n, f) != NULL;
	fclose(f);
	return ok;
}

// the headset's own gamepads: input devices with BTN_GAMEPAD (0x130), Steam's virtual ones (28de:11ff) left out
static int local_gamepads(void)
{
	DIR *d = opendir("/sys/class/input");
	int n = 0;
	for (struct dirent *e; d && (e = readdir(d));) {
		if (strncmp(e->d_name, "event", 5))
			continue;
		char path[300], vendor[16] = "", product[16] = "", key[1024] = "";
		snprintf(path, sizeof(path), "/sys/class/input/%s/device/id/vendor", e->d_name);
		read_line(path, vendor, sizeof(vendor));
		snprintf(path, sizeof(path), "/sys/class/input/%s/device/id/product", e->d_name);
		read_line(path, product, sizeof(product));
		if (!strncmp(vendor, "28de", 4) && !strncmp(product, "11ff", 4))
			continue;
		snprintf(path, sizeof(path), "/sys/class/input/%s/device/capabilities/key", e->d_name);
		if (!read_line(path, key, sizeof(key)))
			continue;
		// longs in hex, the highest first: bit 0x130 is bit 48 of the fifth from the end
		char *w[64];
		int nw = 0;
		for (char *t = strtok(key, " \n"); t && nw < 64; t = strtok(NULL, " \n"))
			w[nw++] = t;
		if (nw >= 5 && (strtoull(w[nw - 5], NULL, 16) >> 48) & 1)
			n++;
	}
	if (d)
		closedir(d);
	return n;
}

// the uinput devices this process holds
static int held_uinputs(void)
{
	DIR *d = opendir("/proc/self/fd");
	int n = 0;
	for (struct dirent *e; d && (e = readdir(d));) {
		char path[300], target[64];
		snprintf(path, sizeof(path), "/proc/self/fd/%s", e->d_name);
		ssize_t l = readlink(path, target, sizeof(target) - 1);
		if (l > 0) {
			target[l] = 0;
			n += !strcmp(target, "/dev/uinput");
		}
	}
	if (d)
		closedir(d);
	return n;
}

// /dev/uinput during a session: a virtual gamepad for a gamepad of the headset's, none for the PC's
static int uinput_refused(const char *path)
{
	if (!path || strcmp(path, "/dev/uinput") || (!spectating() && !streaming()))
		return 0;
	int local = local_gamepads(), held = held_uinputs();
	if (local > held) {
		fprintf(stderr, "quest1_rpspectator: Remote Play session: virtual gamepad allowed (%d gamepads here, %d "
		                "virtual)\n", local, held);
		return 0;
	}
	fprintf(stderr, "quest1_rpspectator: Remote Play session: no virtual gamepad for the PC's controller (%d "
	                "gamepads here, %d virtual)\n", local, held);
	errno = EACCES;
	return 1;
}

typedef int open_fn(const char *, int, ...);
typedef int openat_fn(int, const char *, int, ...);

static mode_t open_mode(int flags, va_list ap)
{
	return (flags & (O_CREAT | O_TMPFILE)) ? va_arg(ap, mode_t) : 0;
}

EXPORT int open(const char *path, int flags, ...)
{
	static open_fn *real;
	va_list ap;
	va_start(ap, flags);
	mode_t mode = open_mode(flags, ap);
	va_end(ap);
	if (uinput_refused(path))
		return -1;
	if (!real)
		real = (open_fn *)dlsym(RTLD_NEXT, "open");
	return real(path, flags, mode);
}

EXPORT int open64(const char *path, int flags, ...)
{
	static open_fn *real;
	va_list ap;
	va_start(ap, flags);
	mode_t mode = open_mode(flags, ap);
	va_end(ap);
	if (uinput_refused(path))
		return -1;
	if (!real)
		real = (open_fn *)dlsym(RTLD_NEXT, "open64");
	return real(path, flags, mode);
}

EXPORT int openat(int dirfd, const char *path, int flags, ...)
{
	static openat_fn *real;
	va_list ap;
	va_start(ap, flags);
	mode_t mode = open_mode(flags, ap);
	va_end(ap);
	if (uinput_refused(path))
		return -1;
	if (!real)
		real = (openat_fn *)dlsym(RTLD_NEXT, "openat");
	return real(dirfd, path, flags, mode);
}

EXPORT int openat64(int dirfd, const char *path, int flags, ...)
{
	static openat_fn *real;
	va_list ap;
	va_start(ap, flags);
	mode_t mode = open_mode(flags, ap);
	va_end(ap);
	if (uinput_refused(path))
		return -1;
	if (!real)
		real = (openat_fn *)dlsym(RTLD_NEXT, "openat64");
	return real(dirfd, path, flags, mode);
}
