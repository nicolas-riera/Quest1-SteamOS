// quest1_vrlink: part of libquest1_v4l2.so, for Steam Link VR's headset client (vrlink: a PC's SteamVR on the headset).
//
// vrlink tells the PC what the headset and its controllers are with the properties SteamVR gives it here (tracking
// system, model, serial, render model, input profile, controller type...), and the PC's driver_vrlink sets up its
// devices from them. Two things went wrong with what SteamVR gives it on this headset (2026-11-11):
//  - vrlink only ships default bindings for Knuckles and Steam Frame controllers, so SteamVR gives the Touch
//    controllers the Knuckles ones and shows vrlink a simulated Valve Index ("Index", LHR-FFFFFFF0, Knuckles
//    controllers). The PC then does not know the headset model and never activates it.
//  - our own identity (tracking system "quest1", {quest1} input profile) means nothing to the PC either.
// So vrlink, and no other process, reads the identity Steam Link's Android app gives a Quest 1 once patched to run
// on it (github.com/MisterAnderson91/quest1-steamlink-patcher, MIT: its hmd_config.json "vr_monterey" entry and
// controller_config.json Quest 1 Touch names): an "Oculus Quest" with Quest 1 Touch controllers, which the PC
// drives with SteamVR's own Oculus profiles and render models. Nothing of Valve's is changed: the properties are
// answered in vrlink's process, through its IVRSystem.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h> // program_invocation_short_name
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPORT __attribute__((visibility("default")))

// IVRSystem_026's methods, counted in its vtable (openvr.h; float at 23 confirmed in vrlink's own calls)
enum { SLOT_ARRAY = 27, SLOT_STRING = 28, NSLOTS = 512 };
enum { HMD, LEFT, RIGHT, OTHER };

typedef uint32_t (*get_string_t)(void *self, uint32_t dev, int prop, char *buf, uint32_t size, int *err);
typedef uint32_t (*get_array_t)(void *self, uint32_t dev, int prop, uint32_t tag, void *buf, uint32_t size, int *err);

static const struct
{
	int prop;
	const char *v[3]; // HMD, left, right
} idents[] = {
    {1000 /* TrackingSystemName */, {"oculus", "oculus", "oculus"}},
    {1001 /* ModelNumber */, {"Oculus Quest", "Oculus Quest (Left Controller)", "Oculus Quest (Right Controller)"}},
    {1002 /* SerialNumber */, {"VRLINKHMDQUEST1", "VRLINKQ1_Controller_Left", "VRLINKQ1_Controller_Right"}},
    {1003 /* RenderModelName */, {"generic_hmd", "oculus_quest_controller_left", "oculus_quest_controller_right"}},
    {1005 /* ManufacturerName */, {"Oculus", "Oculus", "Oculus"}},
    {1035 /* ResourceRoot */, {"vrlink", "vrlink", "vrlink"}},
    {1036 /* RegisteredDeviceType */,
     {"oculus/VRLINKHMDQUEST1", "oculus/VRLINKQ1_Controller_Left", "oculus/VRLINKQ1_Controller_Right"}},
    {1037 /* InputProfilePath */,
     {"{oculus}/input/rift_profile.json", "{oculus}/input/touch_profile.json", "{oculus}/input/touch_profile.json"}},
    {7000 /* ControllerType */, {"rift", "oculus_touch", "oculus_touch"}},
};

static get_string_t real_string;
static get_array_t real_array;
// a copy of vrclient's vtable with two methods replaced, and the two words before it (offset to top and type
// info, which RTTI reads); the class behind IVRSystem has more methods than the interface, so the copy goes on
// to the end of the mapping or NSLOTS
static void *vtable_copy[2 + NSLOTS];
static void **const vtable = vtable_copy + 2;
static signed char roles[64]; // role + 1 of each device index, 0 when not known yet

static int role_of(void *self, uint32_t dev)
{
	if (dev == 0)
		return HMD;
	if (dev >= sizeof(roles) || !roles[dev]) {
		// the hands from the model SteamVR shows (ours, or the simulated "Knuckles Left")
		char model[128] = "";
		int err = 0;
		real_string(self, dev, 1001, model, sizeof(model), &err);
		int r = err ? OTHER : strstr(model, "Left") ? LEFT : strstr(model, "Right") ? RIGHT : OTHER;
		if (dev >= sizeof(roles))
			return r;
		roles[dev] = (signed char)(r + 1);
	}
	return roles[dev] - 1;
}

static uint32_t string_prop(void *self, uint32_t dev, int prop, char *buf, uint32_t size, int *err)
{
	int role = role_of(self, dev);
	for (unsigned i = 0; role != OTHER && i < sizeof(idents) / sizeof(idents[0]); i++) {
		if (idents[i].prop != prop)
			continue;
		const char *v = idents[i].v[role];
		uint32_t n = (uint32_t)strlen(v) + 1;
		if (err)
			*err = buf && size < n ? 3 /* TrackedProp_BufferTooSmall */ : 0;
		if (buf && size >= n)
			memcpy(buf, v, n);
		return n;
	}
	return real_string(self, dev, prop, buf, size, err);
}

// logged, to see what else vrlink asks for (the display rates, ...)
static uint32_t array_prop(void *self, uint32_t dev, int prop, uint32_t tag, void *buf, uint32_t size, int *err)
{
	uint32_t n = real_array(self, dev, prop, tag, buf, size, err);
	static int logged;
	if (logged++ < 24)
		fprintf(stderr, "quest1_v4l2: vrlink: array property %d of device %u (tag %u): %u bytes, error %d\n", prop, dev,
		        tag, n, err ? *err : 0);
	return n;
}

static void patch(void *sys)
{
	void ***obj = sys;
	if (*obj == vtable)
		return;
	void **vt = *obj;
	get_string_t s = (get_string_t)vt[SLOT_STRING];
	// the slot is the string getter: the HMD's model comes back (anything else and nothing is changed)
	char model[128] = "";
	int err = -1;
	uint32_t n = s(sys, 0, 1001, model, sizeof(model), &err);
	if (err != 0 || n < 2 || n > sizeof(model) || strlen(model) + 1 != n) {
		fprintf(stderr, "quest1_v4l2: vrlink: IVRSystem not as expected (%u, error %d): identity left alone\n", n, err);
		return;
	}
	// how far the vtable's mapping goes
	uintptr_t start = 0, end = 0, a = (uintptr_t)vt;
	FILE *maps = fopen("/proc/self/maps", "r");
	char line[512];
	while (maps && fgets(line, sizeof(line), maps)) {
		unsigned long lo, hi;
		if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 && a >= lo && a < hi) {
			start = lo;
			end = hi;
			break;
		}
	}
	if (maps)
		fclose(maps);
	size_t slots = end ? (end - a) / sizeof(void *) : 0;
	if (slots > NSLOTS)
		slots = NSLOTS;
	if (slots <= SLOT_STRING || a < start + 2 * sizeof(void *)) {
		fprintf(stderr, "quest1_v4l2: vrlink: IVRSystem's vtable not found in the mappings: identity left alone\n");
		return;
	}
	memcpy(vtable_copy, vt - 2, (2 + slots) * sizeof(void *));
	real_string = s;
	real_array = (get_array_t)vt[SLOT_ARRAY];
	vtable[SLOT_STRING] = (void *)string_prop;
	vtable[SLOT_ARRAY] = (void *)array_prop;
	*obj = vtable;
	fprintf(stderr, "quest1_v4l2: vrlink: identity of an Oculus Quest with Quest 1 Touch controllers (SteamVR shows %s)\n",
	        model);
}

EXPORT void *VR_GetGenericInterface(const char *version, int *err)
{
	static void *(*real)(const char *, int *);
	if (!real)
		*(void **)&real = dlsym(RTLD_NEXT, "VR_GetGenericInterface");
	if (!real) {
		if (err)
			*err = 105; // VRInitError_Init_InterfaceNotFound
		return NULL;
	}
	void *p = real(version, err);
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	if (p && version && !strcmp(version, "IVRSystem_026") && !strcmp(program_invocation_short_name, "vrlink")) {
		pthread_mutex_lock(&lock);
		patch(p);
		pthread_mutex_unlock(&lock);
	}
	return p;
}
