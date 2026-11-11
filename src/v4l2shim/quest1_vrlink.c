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
//
// The headset's model starts with "Deckard" (the Steam Frame's code name): the PC's driver_vrlink takes a model
// starting with "steam frame" or "deckard" (any case) for a Steam Frame, and only then is the PC's SteamVR in the
// Frame's dashboard mode (SteamVRClientUnified: no dashboard of its own, its panels go to the headset's dashboard;
// "ThirdPartyClient" otherwise) and uses the Frame's stream profile (2026-11-11). The model is the only thing it
// looks at for that, and it is also the name the PC shows.
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h> // program_invocation_short_name
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

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
    {1001 /* ModelNumber */, {"Deckard (Oculus Quest)", "Oculus Quest (Left Controller)", "Oculus Quest (Right Controller)"}},
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
	fprintf(stderr, "quest1_v4l2: vrlink: identity of a \"Deckard (Oculus Quest)\" with Quest 1 Touch controllers "
	        "(SteamVR shows %s)\n", model);
}

// vrlink makes its network and decoding threads SCHED_FIFO 80 (as on the Frame). Steam's processes may only go up to
// RLIMIT_RTPRIO (steam.service: 40), so that failed and they stayed normal threads: they ran late while the headset
// was busy, the PC read that as a bad link ("Bad link event", ~10-70 ms) and held its encoder back, half the frames
// (2026-11-11). Here vrlink's requests are lowered to RT_MAX, below Meta's tracking and Monado (48 and up).
#define RT_MAX 40

static bool is_vrlink(void)
{
	static int v = -1;
	if (v < 0)
		v = !strcmp(program_invocation_short_name, "vrlink");
	return v;
}

EXPORT int pthread_setschedparam(pthread_t t, int policy, const struct sched_param *sp)
{
	static int (*real)(pthread_t, int, const struct sched_param *);
	if (!real)
		*(void **)&real = dlsym(RTLD_NEXT, "pthread_setschedparam");
	struct sched_param p;
	if (is_vrlink() && (policy == SCHED_FIFO || policy == SCHED_RR) && sp->sched_priority > RT_MAX) {
		p = *sp;
		p.sched_priority = RT_MAX;
		sp = &p;
	}
	return real(t, policy, sp);
}

EXPORT int sched_setscheduler(pid_t pid, int policy, const struct sched_param *sp)
{
	static int (*real)(pid_t, int, const struct sched_param *);
	if (!real)
		*(void **)&real = dlsym(RTLD_NEXT, "sched_setscheduler");
	struct sched_param p;
	int base = policy & ~SCHED_RESET_ON_FORK;
	if (sp && is_vrlink() && (base == SCHED_FIFO || base == SCHED_RR) && sp->sched_priority > RT_MAX) {
		p = *sp;
		p.sched_priority = RT_MAX;
		sp = &p;
	}
	return real(pid, policy, sp);
}

// Three of vrlink's threads ask for no priority: its main thread (it takes each decoded frame and submits it to
// vrcompositor), EventsThread (the link's messages) and data-loop.0 (its PipeWire sound). With the headset busy
// (Meta's tracking, SteamVR, Monado: load ~10) they waited for a CPU longer than they ran (169, 274 and 173 ms a
// second, 2026-11-11). They are made realtime here too, below vrlink's own (RT_MAX), as they come up.
static const struct
{
	const char *name;
	int prio;
} rt_threads[] = {{"vrlinkrunthread", RT_MAX - 2}, {"EventsThread", RT_MAX - 2}, {"data-loop.0", RT_MAX - 4}};

static void *rt_watch(void *arg)
{
	(void)arg;
	for (;;) {
		DIR *d = opendir("/proc/self/task");
		struct dirent *de;
		while (d && (de = readdir(d))) {
			pid_t tid = (pid_t)atoi(de->d_name);
			char path[64], comm[32] = "";
			snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
			FILE *f = tid > 0 ? fopen(path, "r") : NULL;
			if (!f)
				continue;
			if (fgets(comm, sizeof(comm), f))
				comm[strcspn(comm, "\n")] = 0;
			fclose(f);
			for (size_t i = 0; i < sizeof(rt_threads) / sizeof(rt_threads[0]); i++) {
				if (strcmp(comm, rt_threads[i].name) || sched_getscheduler(tid) != SCHED_OTHER)
					continue;
				struct sched_param p = {.sched_priority = rt_threads[i].prio};
				if (syscall(SYS_sched_setscheduler, tid, SCHED_FIFO, &p) == 0)
					fprintf(stderr, "quest1_v4l2: vrlink: %s realtime (SCHED_FIFO %d)\n", comm, p.sched_priority);
				else
					fprintf(stderr, "quest1_v4l2: vrlink: %s realtime: %s\n", comm, strerror(errno));
			}
		}
		if (d)
			closedir(d);
		sleep(1);
	}
	return NULL;
}

__attribute__((constructor)) static void rt_watch_start(void)
{
	pthread_t t;
	if (is_vrlink() && pthread_create(&t, NULL, rt_watch, NULL) == 0) {
		pthread_setname_np(t, "q1rtwatch");
		pthread_detach(t);
	}
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
