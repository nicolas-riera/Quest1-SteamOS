// gamepad: the Touch controllers, read through OpenXR actions, as a virtual Xbox 360 pad (uinput)
// so the Steam client (Big Picture) can be driven from inside the headset.
//
// Touch -> Xbox: sticks -> sticks, triggers -> LT/RT, grips -> LB/RB, A/B/X/Y -> A/B/X/Y,
// stick clicks -> L3/R3, left menu -> Start, right Oculus button -> Guide (Steam button).
// The device claims the Xbox 360 USB ids so Steam and SDL map it without configuration.
#include "gamepad.h"

#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

enum {
	A_STICK,
	A_TRIGGER,
	A_GRIP,
	A_STICK_CLICK,
	A_AX, // A on the right hand, X on the left
	A_BY, // B / Y
	A_MENU,
	A_COUNT
};

static const struct {
	const char *name;
	XrActionType type;
	const char *path[2]; // left, right (NULL = none)
} actions[A_COUNT] = {
    [A_STICK] = {"stick", XR_ACTION_TYPE_VECTOR2F_INPUT,
                 {"/user/hand/left/input/thumbstick", "/user/hand/right/input/thumbstick"}},
    [A_TRIGGER] = {"trigger", XR_ACTION_TYPE_FLOAT_INPUT,
                   {"/user/hand/left/input/trigger/value", "/user/hand/right/input/trigger/value"}},
    [A_GRIP] = {"grip", XR_ACTION_TYPE_FLOAT_INPUT,
                {"/user/hand/left/input/squeeze/value", "/user/hand/right/input/squeeze/value"}},
    [A_STICK_CLICK] = {"stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT,
                       {"/user/hand/left/input/thumbstick/click", "/user/hand/right/input/thumbstick/click"}},
    [A_AX] = {"ax", XR_ACTION_TYPE_BOOLEAN_INPUT, {"/user/hand/left/input/x/click", "/user/hand/right/input/a/click"}},
    [A_BY] = {"by", XR_ACTION_TYPE_BOOLEAN_INPUT, {"/user/hand/left/input/y/click", "/user/hand/right/input/b/click"}},
    [A_MENU] = {"menu", XR_ACTION_TYPE_BOOLEAN_INPUT,
                {"/user/hand/left/input/menu/click", "/user/hand/right/input/system/click"}},
};

static XrActionSet set;
static XrAction action[A_COUNT];
static XrPath hand[2];
static int uinput_fd = -1;

// last values sent, to only emit changes
static int last_key[KEY_MAX + 1];
static int last_abs[ABS_MAX + 1];

static void emit(int type, int code, int value)
{
	struct input_event ev = {0};
	ev.type = type;
	ev.code = code;
	ev.value = value;
	if (write(uinput_fd, &ev, sizeof(ev)) < 0) {
		// a full queue drops an event; the next frame resends the state
	}
}

static void set_key(int code, int pressed)
{
	if (last_key[code] != pressed) {
		emit(EV_KEY, code, pressed);
		last_key[code] = pressed;
	}
}

static void set_abs(int code, int value)
{
	if (last_abs[code] != value) {
		emit(EV_ABS, code, value);
		last_abs[code] = value;
	}
}

static int uinput_create(void)
{
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		perror("xscreen: /dev/uinput");
		return -1;
	}
	static const int keys[] = {BTN_A,      BTN_B,      BTN_X,      BTN_Y,     BTN_TL,    BTN_TR,
	                           BTN_SELECT, BTN_START,  BTN_MODE,   BTN_THUMBL, BTN_THUMBR};
	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
		ioctl(fd, UI_SET_KEYBIT, keys[i]);
	ioctl(fd, UI_SET_EVBIT, EV_ABS);

	struct uinput_abs_setup abs[] = {
	    {ABS_X, {0, -32768, 32767, 16, 128, 0}},  {ABS_Y, {0, -32768, 32767, 16, 128, 0}},
	    {ABS_RX, {0, -32768, 32767, 16, 128, 0}}, {ABS_RY, {0, -32768, 32767, 16, 128, 0}},
	    {ABS_Z, {0, 0, 255, 0, 0, 0}},            {ABS_RZ, {0, 0, 255, 0, 0, 0}},
	    {ABS_HAT0X, {0, -1, 1, 0, 0, 0}},         {ABS_HAT0Y, {0, -1, 1, 0, 0, 0}},
	};
	// the Quest kernel is 4.4: no UI_DEV_SETUP / UI_ABS_SETUP (4.5+), so describe the device with
	// the legacy uinput_user_dev write
	struct uinput_user_dev dev = {0};
	for (unsigned i = 0; i < sizeof(abs) / sizeof(abs[0]); i++) {
		ioctl(fd, UI_SET_ABSBIT, abs[i].code);
		dev.absmin[abs[i].code] = abs[i].absinfo.minimum;
		dev.absmax[abs[i].code] = abs[i].absinfo.maximum;
		dev.absfuzz[abs[i].code] = abs[i].absinfo.fuzz;
		dev.absflat[abs[i].code] = abs[i].absinfo.flat;
	}

	dev.id.bustype = BUS_USB;
	dev.id.vendor = 0x045e; // Microsoft
	dev.id.product = 0x028e; // Xbox 360 controller
	dev.id.version = 0x0110;
	snprintf(dev.name, sizeof(dev.name), "Microsoft X-Box 360 pad");
	if (write(fd, &dev, sizeof(dev)) != sizeof(dev) || ioctl(fd, UI_DEV_CREATE) < 0) {
		perror("xscreen: uinput device");
		close(fd);
		return -1;
	}
	return fd;
}

bool gamepad_init(XrInstance instance, XrSession session)
{
	XrActionSetCreateInfo sci = {XR_TYPE_ACTION_SET_CREATE_INFO};
	strcpy(sci.actionSetName, "steam");
	strcpy(sci.localizedActionSetName, "Steam gamepad");
	if (XR_FAILED(xrCreateActionSet(instance, &sci, &set)))
		return false;
	xrStringToPath(instance, "/user/hand/left", &hand[0]);
	xrStringToPath(instance, "/user/hand/right", &hand[1]);

	XrActionSuggestedBinding bindings[A_COUNT * 2];
	uint32_t nb = 0;
	for (int a = 0; a < A_COUNT; a++) {
		XrActionCreateInfo ci = {XR_TYPE_ACTION_CREATE_INFO};
		strcpy(ci.actionName, actions[a].name);
		strcpy(ci.localizedActionName, actions[a].name);
		ci.actionType = actions[a].type;
		ci.countSubactionPaths = 2;
		ci.subactionPaths = hand;
		if (XR_FAILED(xrCreateAction(set, &ci, &action[a])))
			return false;
		for (int h = 0; h < 2; h++) {
			if (!actions[a].path[h])
				continue;
			bindings[nb].action = action[a];
			xrStringToPath(instance, actions[a].path[h], &bindings[nb].binding);
			nb++;
		}
	}
	XrInteractionProfileSuggestedBinding sb = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
	xrStringToPath(instance, "/interaction_profiles/oculus/touch_controller", &sb.interactionProfile);
	sb.suggestedBindings = bindings;
	sb.countSuggestedBindings = nb;
	if (XR_FAILED(xrSuggestInteractionProfileBindings(instance, &sb)))
		return false;
	XrSessionActionSetsAttachInfo ai = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
	ai.countActionSets = 1;
	ai.actionSets = &set;
	if (XR_FAILED(xrAttachSessionActionSets(session, &ai)))
		return false;

	uinput_fd = uinput_create();
	if (uinput_fd >= 0)
		printf("xscreen: Touch controllers -> virtual Xbox 360 pad\n");
	return uinput_fd >= 0;
}

static float get_float(XrSession s, int a, int h)
{
	XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
	gi.action = action[a];
	gi.subactionPath = hand[h];
	XrActionStateFloat st = {XR_TYPE_ACTION_STATE_FLOAT};
	return XR_SUCCEEDED(xrGetActionStateFloat(s, &gi, &st)) && st.isActive ? st.currentState : 0.0f;
}

static int get_bool(XrSession s, int a, int h)
{
	XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
	gi.action = action[a];
	gi.subactionPath = hand[h];
	XrActionStateBoolean st = {XR_TYPE_ACTION_STATE_BOOLEAN};
	return XR_SUCCEEDED(xrGetActionStateBoolean(s, &gi, &st)) && st.isActive && st.currentState;
}

static XrVector2f get_vec2(XrSession s, int a, int h)
{
	XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
	gi.action = action[a];
	gi.subactionPath = hand[h];
	XrActionStateVector2f st = {XR_TYPE_ACTION_STATE_VECTOR2F};
	XrVector2f zero = {0, 0};
	return XR_SUCCEEDED(xrGetActionStateVector2f(s, &gi, &st)) && st.isActive ? st.currentState : zero;
}

static int stick_axis(float v)
{
	int x = (int)(v * 32767.0f);
	return x < -32768 ? -32768 : x > 32767 ? 32767 : x;
}

void gamepad_update(XrSession session, bool focused)
{
	if (uinput_fd < 0)
		return;
	if (focused) {
		XrActiveActionSet active = {set, XR_NULL_PATH};
		XrActionsSyncInfo si = {XR_TYPE_ACTIONS_SYNC_INFO};
		si.countActiveActionSets = 1;
		si.activeActionSets = &active;
		focused = XR_SUCCEEDED(xrSyncActions(session, &si));
	}
	if (!focused) {
		// release everything when the session loses input focus
		for (int k = 0; k <= KEY_MAX; k++)
			if (last_key[k])
				set_key(k, 0);
		for (int a = 0; a <= ABS_MAX; a++)
			if (last_abs[a])
				set_abs(a, 0);
		emit(EV_SYN, SYN_REPORT, 0);
		return;
	}

	XrVector2f ls = get_vec2(session, A_STICK, 0), rs = get_vec2(session, A_STICK, 1);
	set_abs(ABS_X, stick_axis(ls.x));
	set_abs(ABS_Y, stick_axis(-ls.y)); // evdev: +y is down
	set_abs(ABS_RX, stick_axis(rs.x));
	set_abs(ABS_RY, stick_axis(-rs.y));
	set_abs(ABS_Z, (int)(get_float(session, A_TRIGGER, 0) * 255.0f));
	set_abs(ABS_RZ, (int)(get_float(session, A_TRIGGER, 1) * 255.0f));
	set_key(BTN_TL, get_float(session, A_GRIP, 0) > 0.5f);
	set_key(BTN_TR, get_float(session, A_GRIP, 1) > 0.5f);
	set_key(BTN_X, get_bool(session, A_AX, 0));
	set_key(BTN_A, get_bool(session, A_AX, 1));
	set_key(BTN_Y, get_bool(session, A_BY, 0));
	set_key(BTN_B, get_bool(session, A_BY, 1));
	set_key(BTN_THUMBL, get_bool(session, A_STICK_CLICK, 0));
	set_key(BTN_THUMBR, get_bool(session, A_STICK_CLICK, 1));
	set_key(BTN_START, get_bool(session, A_MENU, 0));
	set_key(BTN_MODE, get_bool(session, A_MENU, 1));
	emit(EV_SYN, SYN_REPORT, 0);
}

void gamepad_close(void)
{
	if (uinput_fd >= 0) {
		ioctl(uinput_fd, UI_DEV_DESTROY);
		close(uinput_fd);
		uinput_fd = -1;
	}
}
