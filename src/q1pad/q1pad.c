// SPDX-License-Identifier: MIT
/*
 * q1pad: the Touch controllers (q1trackd's shared memory, q1track_shm.h) as a virtual Xbox 360
 * pad (uinput), so the Steam client sees a gamepad like it does on the Steam Frame, whose
 * controllers are a Steam Input device. Without one, Steam's VR UI widgets (the dashboard "+"
 * menu's Desktop entry, ...) ignore laser-pointer clicks. SteamVR keeps reading the controllers
 * through its own driver; this pad is only for Steam Input.
 *
 * Touch -> Xbox: right A/B -> South/East, left X/Y -> West/North, sticks -> sticks, index
 * triggers -> LT/RT, grips -> LB/RB, stick clicks -> L3/R3, left menu -> Start. The Oculus
 * button is not mapped (the system keeps it).
 *
 * The device claims the Xbox 360 USB ids so Steam and SDL map it without configuration. The
 * Quest kernel is 4.4: no UI_DEV_SETUP / UI_ABS_SETUP (4.5+), hence the legacy uinput_user_dev.
 */
#include <fcntl.h>
#include <linux/uinput.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "../q1track/q1track_shm.h"

// OSSDK::Input::v1::Button bits (docs/tracking-ipc.md 3.4)
#define B_MENU_LEFT (1u << 2) // "back" on the left controller, bit 1 (home) on the right one is Oculus
#define B_STICK (1u << 3)
#define B_AX (1u << 8)
#define B_BY (1u << 10)

static int fd = -1;
static int last_key[KEY_MAX + 1], last_abs[ABS_MAX + 1];

static void emit(int type, int code, int value)
{
	struct input_event ev = {0};
	ev.type = type;
	ev.code = code;
	ev.value = value;
	if (write(fd, &ev, sizeof(ev)) < 0) {
		// a full queue drops an event; the state is resent whenever it changes again
	}
}

static int dirty;

static void key(int code, int on)
{
	if (last_key[code] != on) {
		emit(EV_KEY, code, on);
		last_key[code] = on;
		dirty = 1;
	}
}

static void axis(int code, int v)
{
	if (last_abs[code] != v) {
		emit(EV_ABS, code, v);
		last_abs[code] = v;
		dirty = 1;
	}
}

static int uinput_create(void)
{
	fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		perror("q1pad: /dev/uinput");
		return -1;
	}
	static const int keys[] = {BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST,   BTN_TL,    BTN_TR,
	                           BTN_SELECT, BTN_START, BTN_MODE,  BTN_THUMBL, BTN_THUMBR};
	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
		ioctl(fd, UI_SET_KEYBIT, keys[i]);
	ioctl(fd, UI_SET_EVBIT, EV_ABS);

	struct uinput_user_dev dev;
	memset(&dev, 0, sizeof(dev));
	static const int sticks[] = {ABS_X, ABS_Y, ABS_RX, ABS_RY};
	static const int trig[] = {ABS_Z, ABS_RZ};
	static const int hat[] = {ABS_HAT0X, ABS_HAT0Y};
	for (int i = 0; i < 4; i++) {
		ioctl(fd, UI_SET_ABSBIT, sticks[i]);
		dev.absmin[sticks[i]] = -32768;
		dev.absmax[sticks[i]] = 32767;
		dev.absfuzz[sticks[i]] = 16;
		dev.absflat[sticks[i]] = 128;
	}
	for (int i = 0; i < 2; i++) {
		ioctl(fd, UI_SET_ABSBIT, trig[i]);
		dev.absmax[trig[i]] = 255;
		ioctl(fd, UI_SET_ABSBIT, hat[i]);
		dev.absmin[hat[i]] = -1;
		dev.absmax[hat[i]] = 1;
	}
	dev.id.bustype = BUS_USB;
	dev.id.vendor = 0x045e; // Microsoft
	dev.id.product = 0x028e; // Xbox 360 controller
	dev.id.version = 0x0110;
	snprintf(dev.name, sizeof(dev.name), "Microsoft X-Box 360 pad");
	if (write(fd, &dev, sizeof(dev)) != sizeof(dev) || ioctl(fd, UI_DEV_CREATE) < 0) {
		perror("q1pad: uinput device");
		return -1;
	}
	return 0;
}

static int stick16(float v)
{
	if (fabsf(v) < 0.08f) // q1trackd's stick is a touch position: a small dead zone keeps it still
		return 0;
	v = v > 1 ? 1 : v < -1 ? -1 : v;
	return (int)(v * 32767);
}

static int trig8(float v) { return (int)((v < 0 ? 0 : v > 1 ? 1 : v) * 255); }

static int64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int main(void)
{
	if (uinput_create() < 0)
		return 1;
	struct q1track_shm *m = NULL;
	for (;;) {
		if (!m) { // q1trackd may start later (or restart): map when the file is there
			int sfd = open(Q1TRACK_SHM_PATH, O_RDONLY | O_CLOEXEC);
			if (sfd >= 0) {
				m = mmap(NULL, sizeof(*m), PROT_READ, MAP_SHARED, sfd, 0);
				close(sfd);
				if (m == MAP_FAILED)
					m = NULL;
			}
		}
		struct q1track_controller c[2];
		memset(c, 0, sizeof(c));
		if (m && m->magic == Q1TRACK_MAGIC && m->version == Q1TRACK_VERSION) {
			uint32_t s0, s1 = 0;
			int ok = 0;
			for (int i = 0; i < 5 && !ok; i++) { // seqlock read
				s0 = m->seq;
				memcpy(c, (const void *)m->controller, sizeof(c));
				s1 = m->seq;
				ok = !(s0 & 1) && s0 == s1;
			}
			if (!ok || now_ns() - m->update_ns > 1000000000LL)
				memset(c, 0, sizeof(c)); // q1trackd stalled: release everything
			for (int i = 0; i < 2; i++)
				if (!c[i].connected)
					memset(&c[i], 0, sizeof(c[i]));
		}
		const struct q1track_controller *l = &c[0], *r = &c[1];
		key(BTN_SOUTH, !!(r->buttons & B_AX));
		key(BTN_EAST, !!(r->buttons & B_BY));
		key(BTN_WEST, !!(l->buttons & B_AX));
		key(BTN_NORTH, !!(l->buttons & B_BY));
		// the grips are analog on Touch; click at half squeeze
		key(BTN_TL, l->grip > 0.5f);
		key(BTN_TR, r->grip > 0.5f);
		key(BTN_START, !!(l->buttons & B_MENU_LEFT));
		key(BTN_THUMBL, !!(l->buttons & B_STICK));
		key(BTN_THUMBR, !!(r->buttons & B_STICK));
		axis(ABS_X, stick16(l->stick_x));
		axis(ABS_Y, -stick16(l->stick_y)); // Xbox: +Y is down
		axis(ABS_RX, stick16(r->stick_x));
		axis(ABS_RY, -stick16(r->stick_y));
		axis(ABS_Z, trig8(l->trigger));
		axis(ABS_RZ, trig8(r->trigger));
		if (dirty) {
			emit(EV_SYN, SYN_REPORT, 0);
			dirty = 0;
		}
		usleep(8000); // 125 Hz
	}
}
