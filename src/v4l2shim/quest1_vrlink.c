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
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "quest1_v4l2.h"
#include "../steamvr-quest1/quest1_hmd_pose.h"
#include "../vklayer-steamvr/quest1_views.h"

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

// vrlink forwards a controller's buttons and axes to the PC only for the Frame's controllers: each frame it reads
// the hand's Prop_ControllerType_String into a 1024-byte buffer, and anything but "frame_controller" stops there
// ("Unsupported Prop_ControllerType_String", vrlink.bin 0x563b0). It then reads each control by its Frame path
// (input_path() below). The identity it sends to the PC reads the same property into 128 bytes, and must stay
// "oculus_touch" (the PC's SteamVR then gives games the Touch controllers). So only that 1024-byte read is told
// "frame_controller".
#define INPUT_TYPE_BUF 1024

static uint32_t string_prop(void *self, uint32_t dev, int prop, char *buf, uint32_t size, int *err)
{
	int role = role_of(self, dev);
	if (prop == 7000 && size == INPUT_TYPE_BUF && (role == LEFT || role == RIGHT)) {
		static const char ft[] = "frame_controller";
		if (err)
			*err = buf ? 0 : 3;
		if (buf)
			memcpy(buf, ft, sizeof(ft));
		return sizeof(ft);
	}
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

// Copies an object's vtable into copy (2 + NSLOTS words: offset to top and type info, which RTTI reads, then the
// methods; the class behind an interface has more methods than the interface, so the copy goes on to the end of the
// mapping or NSLOTS) and returns the copy's methods, or NULL when the vtable does not have more than min_slots.
static void **copy_vtable(void *obj, void **copy, size_t min_slots)
{
	void **vt = *(void ***)obj;
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
	if (slots <= min_slots || a < start + 2 * sizeof(void *))
		return NULL;
	memcpy(copy, vt - 2, (2 + slots) * sizeof(void *));
	return copy + 2;
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
	if (!copy_vtable(sys, vtable_copy, SLOT_STRING)) {
		fprintf(stderr, "quest1_v4l2: vrlink: IVRSystem's vtable not found in the mappings: identity left alone\n");
		return;
	}
	real_string = s;
	real_array = (get_array_t)vt[SLOT_ARRAY];
	vtable[SLOT_STRING] = (void *)string_prop;
	vtable[SLOT_ARRAY] = (void *)array_prop;
	*obj = vtable;
	fprintf(stderr, "quest1_v4l2: vrlink: identity of a \"Deckard (Oculus Quest)\" with Quest 1 Touch controllers "
	        "(SteamVR shows %s)\n", model);
}

// vrlink reads a controller's controls by their paths (IVRPaths: StringToHandle once, then ReadPathBatch each frame;
// vrlink.bin 0x54a18 for the buttons, 0x55840 for the axes): the Frame controller's, right a, b, x, y, menu, left
// dpad_down, dpad_right, dpad_left, dpad_up, view, both thumbstick, grip, bumper, trigger, system, thumbrest. It sends
// them to the PC in slots, in that order, and the PC's driver_vrlink fills the controller it shows games from the
// same slots: for our "oculus_touch" identity, in the Touch's own order (Windows driver_vrlink.dll 0x180a16385;
// 0x180a16dcb for the Frame's). Each path vrlink asks for is replaced here by our Touch control that the PC reads
// from that slot (2026-11-16: before this, Frame names did not exist on our controllers and the trigger, in the
// Frame's trigger slot, moved the stick on the PC). Paths not listed (bumper, thumbstick click, menu...) fill slots
// the PC's Touch does not read, and are left as they are.
static const struct
{
	const char *frame, *touch;
} input_paths[] = {
    // buttons: a, b (right) and x, y (left); the left menu button; the stick's click
    {"/user/hand/right/input/a/click", "/user/hand/right/input/a/click"},
    {"/user/hand/right/input/b/click", "/user/hand/right/input/b/click"},
    {"/user/hand/right/input/y/click", "/user/hand/right/input/joystick/click"},
    {"/user/hand/left/input/dpad_down/click", "/user/hand/left/input/x/click"},
    {"/user/hand/left/input/dpad_right/click", "/user/hand/left/input/y/click"},
    {"/user/hand/left/input/dpad_left/click", "/user/hand/left/input/system/click"},
    {"/user/hand/left/input/dpad_up/click", "/user/hand/left/input/joystick/click"},
    // touches: the buttons', the trigger's, the stick's, the thumb rest's
    {"/user/hand/right/input/a/touch", "/user/hand/right/input/a/touch"},
    {"/user/hand/right/input/b/touch", "/user/hand/right/input/b/touch"},
    {"/user/hand/right/input/x/touch", "/user/hand/right/input/trigger/touch"},
    {"/user/hand/right/input/y/touch", "/user/hand/right/input/joystick/touch"},
    {"/user/hand/right/input/menu/touch", "/user/hand/right/input/thumbrest/touch"},
    {"/user/hand/left/input/dpad_down/touch", "/user/hand/left/input/x/touch"},
    {"/user/hand/left/input/dpad_right/touch", "/user/hand/left/input/y/touch"},
    {"/user/hand/left/input/dpad_left/touch", "/user/hand/left/input/trigger/touch"},
    {"/user/hand/left/input/dpad_up/touch", "/user/hand/left/input/joystick/touch"},
    {"/user/hand/left/input/view/touch", "/user/hand/left/input/thumbrest/touch"},
    // axes: the Frame's stick x, stick y and trigger slots are the Touch's trigger, stick x and stick y
    {"/user/hand/right/input/thumbstick/x", "/user/hand/right/input/trigger/value"},
    {"/user/hand/right/input/thumbstick/y", "/user/hand/right/input/joystick/x"},
    {"/user/hand/right/input/trigger/value", "/user/hand/right/input/joystick/y"},
    {"/user/hand/left/input/thumbstick/x", "/user/hand/left/input/trigger/value"},
    {"/user/hand/left/input/thumbstick/y", "/user/hand/left/input/joystick/x"},
    {"/user/hand/left/input/trigger/value", "/user/hand/left/input/joystick/y"},
};

// IVRPaths_002's StringToHandle, counted in its vtable (openvr.h: ReadPathBatch, WritePathBatch, StringToHandle)
enum { SLOT_PATH_HANDLE = 2 };
typedef int (*path_handle_t)(void *self, uint64_t *handle, const char *path);
static path_handle_t real_path_handle;
static void *paths_vtable_copy[2 + NSLOTS];

static int input_path(void *self, uint64_t *handle, const char *path)
{
	for (unsigned i = 0; path && i < sizeof(input_paths) / sizeof(input_paths[0]); i++)
		if (!strcmp(path, input_paths[i].frame)) {
			int r = real_path_handle(self, handle, input_paths[i].touch);
			// vrlink asks again for some paths every frame: logged once each
			static bool logged[sizeof(input_paths) / sizeof(input_paths[0])];
			if (!logged[i]) {
				logged[i] = true;
				fprintf(stderr, "quest1_v4l2: vrlink: %s read from %s (error %d)\n", path, input_paths[i].touch, r);
			}
			return r;
		}
	return real_path_handle(self, handle, path);
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

// The views shared with vrcompositor's layer (quest1_views.h), made as vrlink starts.
static struct q1_views *views_shm;
static pthread_once_t views_once = PTHREAD_ONCE_INIT;

static void views_open_once(void)
{
	int fd = open(Q1_VIEWS_PATH, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	void *m = fd >= 0 && ftruncate(fd, sizeof(*views_shm)) == 0
	              ? mmap(NULL, sizeof(*views_shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
	if (fd >= 0)
		close(fd);
	views_shm = m == MAP_FAILED ? NULL : m;
	fprintf(stderr, "quest1_v4l2: vrlink: views for vrcompositor in %s: %s\n", Q1_VIEWS_PATH,
	        views_shm ? "ok" : strerror(errno));
}

static struct q1_views *views_open(void)
{
	pthread_once(&views_once, views_open_once);
	return views_shm;
}

static void *rt_watch(void *arg)
{
	(void)arg;
	for (;;) {
		struct q1_views *v = views_open();
		if (v) { // vrlink runs, also while it submits nothing (the headset's dashboard open: seconds)
			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			__atomic_store_n(&v->alive_ns, (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec, __ATOMIC_RELAXED);
		}
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

// vrlink submits each frame as a texture set: the 16 frame buffers it shared with vrcompositor and the index of the
// one to show (IVRClientInternal_XXX slot 40, vrlink.bin 0x340fc). vrclient gives vrcompositor the set's first
// texture as the eye texture, with the index (SubmitTextureSetInfo); vrcompositor then copies texture[index] into
// texture[0] with a method that does nothing in its Vulkan device (vrcompositor 0x7c9e0 -> 0x2c00c8, a bare ret), and
// shows texture[0]. So the headset showed vrlink's buffer 0 only, as the decoder wrote over it every 9 frames: a few
// pictures a second, each late (2026-11-12; vrlink itself picked a new frame every 1/72 s). Here the frame to show
// goes first in the set, which is then submitted with index 0: the same 16 buffers, two of them swapped.
enum { SLOT_SUBMIT_SET = 40, SET_MAX = 16 };
typedef uint32_t (*submit_set_t)(void *self, int32_t index, uint32_t count, const uint64_t *handles, const void *eye_l,
                                 const void *eye_r, const void *bounds_l, const void *bounds_r, uint32_t flags,
                                 double seconds);
static submit_set_t real_submit_set;
static void *client_vtable_copy[2 + NSLOTS];

// After an eye's VRTextureWithPoseAndDepth_t (144 bytes): two floats, the inset's place in the atlas, the view count
enum { EYE_INSET = 152, EYE_VIEWS = 168, SUBMIT_TEXTURE_WITH_DEPTH = 0x10 };

//! The insets' places, for vrcompositor's layer (quest1_views.h): written when they change.
static void publish_views(const void *const eye[2], const void *const bounds[2], uint32_t flags)
{
	struct q1_views n = {0, 2, {{0}}, {{0}}, 0};
	for (int e = 0; e < 2; e++) {
		uint32_t views;
		if (!(flags & SUBMIT_TEXTURE_WITH_DEPTH) || !eye[e] || !bounds[e])
			return;
		memcpy(&views, (const char *)eye[e] + EYE_VIEWS, sizeof(views));
		memcpy(n.wide[e], bounds[e], sizeof(n.wide[e]));
		memcpy(n.inset[e], (const char *)eye[e] + EYE_INSET, sizeof(n.inset[e]));
		const float *w = n.wide[e], *r = n.inset[e];
		if (views != 2 || !(r[0] >= w[0] && r[0] < r[2] && r[2] <= w[2] && r[1] >= w[1] && r[1] < r[3] && r[3] <= w[3]))
			n.eyes = 0;
	}
	struct q1_views *v = views_open();
	if (!v || (v->eyes == n.eyes && !memcmp(v->wide, n.wide, sizeof(n.wide)) && !memcmp(v->inset, n.inset, sizeof(n.inset))))
		return;
	uint32_t seq = __atomic_load_n(&v->seq, __ATOMIC_RELAXED);
	__atomic_store_n(&v->seq, seq | 1, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	v->eyes = n.eyes;
	memcpy(v->wide, n.wide, sizeof(n.wide));
	memcpy(v->inset, n.inset, sizeof(n.inset));
	__atomic_store_n(&v->seq, (seq | 1) + 1, __ATOMIC_RELEASE);
}

// What the user sees as a black flash, logged (journal, "quest1_v4l2: vrlink:") with the time of day, so that a flash
// seen worn can be matched: no new picture from vrlink for more than 100 ms (vrcompositor shows the last one again,
// re-projected: a freeze, not black), an empty submit passed on (no picture for 200 ms: vrcompositor draws no stream),
// a black picture (the whole decoded atlas black: the PC sent black) and a green one (no chroma). A run of black or
// green pictures is one line, with its length. Nothing of it was seen on the table under load (2026-11-17: desktop
// mode, 500 MB taken, load 19). Worn the same day: runs of 19-69 black pictures as VRChat started (its own black).
static void flash_log(const char *fmt, ...)
{
	struct timespec rt;
	clock_gettime(CLOCK_REALTIME, &rt);
	struct tm tm;
	localtime_r(&rt.tv_sec, &tm);
	char msg[160];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	fprintf(stderr, "quest1_v4l2: vrlink: %02d:%02d:%02d.%03ld %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
	        rt.tv_nsec / 1000000, msg);
}

static void flash_watch(int32_t index, bool picture, bool passed_empty, uint64_t now)
{
	static uint64_t last_picture, gap_logged;
	static unsigned black_run, black_total, empty_run, gaps_unlogged, green_run;
	if (passed_empty) {
		if (!empty_run++) {
			if (last_picture)
				flash_log("empty submits passed on: no stream picture for %.0f ms", (now - last_picture) / 1e6);
			else
				flash_log("empty submits passed on: no stream picture yet");
		}
		return;
	}
	if (!picture)
		return;
	if (empty_run) {
		flash_log("empty submits ended after %u", empty_run);
		empty_run = 0;
	}
	if (last_picture && now - last_picture > 100000000ull && now - last_picture < 10000000000ull) {
		// at most a line a second (a stream's start runs at ~10 pictures a second for a few seconds)
		if (now - gap_logged < 1000000000ull) {
			gaps_unlogged++;
		} else {
			flash_log("no new picture for %.0f ms (%u more since the last such line)", (now - last_picture) / 1e6,
			          gaps_unlogged);
			gap_logged = now;
			gaps_unlogged = 0;
		}
	}
	last_picture = now;
	// black: every sample at most 24 (video black is 16; Aurora, a dark scene, has means of 29-53 and bright stars)
	unsigned m[4], peak, uv[2];
	bool sampled = index >= 0 && q1dec_client_frame_sample((unsigned)index, m, &peak, uv);
	bool black = sampled && peak <= 24;
	// green: what a picture with no chroma (U and V near 0 instead of 128 for grey) looks like, the mean chroma of the
	// whole atlas far below grey on both axes. (The green stream of 2026-11-17 was vrcompositor's simplified shader,
	// holo-frame-mode.sh; this tells a decoder or PC fault from the headset's drawing.)
	bool green = sampled && uv[0] < 80 && uv[1] < 80;
	if (green) {
		if (!green_run++)
			flash_log("green picture (no chroma): U %u V %u, luma %u %u %u %u", uv[0], uv[1], m[0], m[1], m[2], m[3]);
	} else if (green_run) {
		flash_log("green pictures ended after %u", green_run);
		green_run = 0;
	}
	if (black) {
		if (!black_run++)
			flash_log("black picture from the PC");
	} else if (black_run) {
		black_total += black_run;
		flash_log("black pictures ended after %u (%u since vrlink started)", black_run, black_total);
		black_run = 0;
	}
}

static uint32_t submit_set(void *self, int32_t index, uint32_t count, const uint64_t *handles, const void *eye_l,
                           const void *eye_r, const void *bounds_l, const void *bounds_r, uint32_t flags,
                           double seconds)
{
	uint64_t set[SET_MAX];
	const int32_t frame = index; // the decoder's frame buffer
	if (handles && index > 0 && (uint32_t)index < count && count <= SET_MAX) {
		memcpy(set, handles, count * sizeof(set[0]));
		set[0] = handles[index];
		set[index] = handles[0];
		handles = set;
		index = 0;
	}
	publish_views((const void *const[2]){eye_l, eye_r}, (const void *const[2]){bounds_l, bounds_r}, flags);
	// An empty submit (no eyes, no textures) shows nothing: vrcompositor draws black for that frame. vrlink sends one
	// when it has no picture ready in time, which happens when the headset is loaded (a stream's start: a dozen in the
	// first 20 s, 2026-11-12), and the user saw the stream flash black. Between pictures it is left out, so that
	// vrcompositor shows the last picture again; a run of them (loading, the stream's end) still goes through.
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
	static uint64_t last_picture;
	bool picture = eye_l && eye_r && count;
	if (picture)
		last_picture = now;
	else if (!eye_l && !eye_r && !count && last_picture && now - last_picture < 200000000ull) {
		return 0; // VRCompositorError_None
	}
	flash_watch(frame, picture, !picture && !eye_l && !eye_r && !count, now);
	return real_submit_set(self, index, count, handles, eye_l, eye_r, bounds_l, bounds_r, flags, seconds);
}

// vrlink's HMD pose (quest1_hmd_pose.h). vrlink reads SteamVR's raw driver pose of a device through
// IVRClientInternal_XXX slot 12 (vrclient copies it from vrserver's shared memory; vrlink.bin 0x56044, from its
// EventsThread), predicts it itself and sends it to the PC. The copy is a CLOCK_MONOTONIC time in ns then
// DriverPose_t (offsets checked against what driver_quest1 published, 2026-11-12). The time is vrserver's: when the
// pose came, plus a lead of its own (35 ms; ~39 from the driver's call), which is measured (raw_pose_lead) and kept, so
// that vrlink reads driver_quest1's pose of the head now as it would read a pose SteamVR had just been given.
enum {
	SLOT_RAW_POSE = 12,
	RAW_TIME = 0, RAW_TIME_OFFSET = 8, RAW_POSITION = 0x80, RAW_VELOCITY = 0x98, RAW_ROTATION = 0xc8,
	RAW_ANGULAR = 0xe8,
};
typedef uint64_t (*raw_pose_t)(void *self, uint32_t dev, void *out);
static raw_pose_t real_raw_pose;

static struct q1_hmd_pose *hmd_pose_open(uint64_t now)
{
	static struct q1_hmd_pose *p;
	static uint64_t tried;
	if (p || (tried && now - tried < 1000000000ull))
		return p;
	tried = now;
	int fd = open(Q1_HMD_POSE_PATH, O_RDWR | O_CLOEXEC);
	void *m = fd >= 0 ? mmap(NULL, sizeof(*p), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
	if (fd >= 0)
		close(fd);
	p = m == MAP_FAILED ? NULL : m;
	if (p)
		fprintf(stderr, "quest1_v4l2: vrlink: the head's pose from driver_quest1 (%s)\n", Q1_HMD_POSE_PATH);
	return p;
}

//! vrserver's lead: its time of the pose it gave last minus when driver_quest1 gave it. A pose given a moment ago
//! that vrserver has not taken yet reads a frame short: such readings are left out.
static bool raw_pose_lead(int64_t sample, int64_t *lead)
{
	static int64_t est;
	static unsigned off;
	if (sample <= 0 || sample > 100000000) {
		*lead = est;
		return est != 0;
	}
	if (!est || off > 50) {
		est = sample;
		off = 0;
	} else if (sample > est - 2000000 && sample < est + 2000000) {
		est += (sample - est) / 16;
		off = 0;
	} else {
		off++;
	}
	*lead = est;
	return true;
}

static uint64_t raw_pose(void *self, uint32_t dev, void *out)
{
	uint64_t r = real_raw_pose(self, dev, out);
	if (dev != 0 || !out)
		return r;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	pthread_mutex_lock(&lock);
	struct q1_hmd_pose *shm = hmd_pose_open(now), p;
	if (!shm) {
		pthread_mutex_unlock(&lock);
		return r;
	}
	__atomic_store_n(&shm->reader_ns, (int64_t)now, __ATOMIC_RELAXED);
	unsigned char *o = out;
	int64_t vr_time, lead = 0;
	memcpy(&vr_time, o + RAW_TIME, sizeof(vr_time));
	bool have_lead = raw_pose_lead(vr_time - __atomic_load_n(&shm->publish_ns, __ATOMIC_ACQUIRE), &lead);
	uint32_t seq;
	int tries = 0;
	do {
		seq = __atomic_load_n(&shm->seq, __ATOMIC_ACQUIRE);
		memcpy(&p, shm, sizeof(p));
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
	} while (((seq & 1) || seq != __atomic_load_n(&shm->seq, __ATOMIC_RELAXED)) && ++tries < 100);
	double q[4];
	memcpy(q, o + RAW_ROTATION, sizeof(q));
	double qn = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
	// the driver's pose, fresh, and a copy laid out as expected (a unit quaternion where the rotation goes)
	if (have_lead && tries < 100 && p.valid && now - (uint64_t)p.time_ns < 50000000ull && qn > 0.99 && qn < 1.01) {
		int64_t t = p.time_ns + lead;
		double zero = 0;
		memcpy(o + RAW_TIME, &t, sizeof(t));
		memcpy(o + RAW_TIME_OFFSET, &zero, sizeof(zero));
		memcpy(o + RAW_POSITION, p.pos, sizeof(p.pos));
		memcpy(o + RAW_VELOCITY, p.vel, sizeof(p.vel));
		memcpy(o + RAW_ROTATION, p.q, sizeof(p.q));
		// The angular velocity goes as measured. It was low-passed (40 ms) from 2026-11-13 while vrcompositor did not
		// correct the stream's picture for fast changes of its pose, so the rate's noise, times the PC's ~90 ms
		// prediction, shook the stream. vrcompositor corrects it exactly since 1efbd78 (2026-11-16), and the low-pass
		// only delayed the rate: the PC's prediction overshot as the head stopped and the picture "bounced" (user,
		// 2026-11-17). On the user's recorded head (85 s, 92 ms ahead) the low-pass made the prediction error 4.6 deg
		// rms against 2.9 without, for 10 % less frame-to-frame shake (docs/streaming.md item 15).
		memcpy(o + RAW_ANGULAR, p.ang, sizeof(p.ang));
	}
	pthread_mutex_unlock(&lock);
	return r;
}

static void patch_client(void *client)
{
	void ***obj = client;
	if (*obj == client_vtable_copy + 2)
		return;
	void **vt = *obj;
	void **copy = copy_vtable(client, client_vtable_copy, SLOT_SUBMIT_SET);
	if (!copy) {
		fprintf(stderr, "quest1_v4l2: vrlink: IVRClientInternal's vtable not found in the mappings: frames left alone\n");
		return;
	}
	real_submit_set = (submit_set_t)vt[SLOT_SUBMIT_SET];
	copy[SLOT_SUBMIT_SET] = (void *)submit_set;
	real_raw_pose = (raw_pose_t)vt[SLOT_RAW_POSE];
	copy[SLOT_RAW_POSE] = (void *)raw_pose;
	*obj = copy;
	fprintf(stderr, "quest1_v4l2: vrlink: each frame goes first in its texture set; the head's pose from driver_quest1\n");
}

static void patch_paths(void *paths)
{
	void ***obj = paths;
	if (*obj == paths_vtable_copy + 2)
		return;
	void **vt = *obj;
	void **copy = copy_vtable(paths, paths_vtable_copy, SLOT_PATH_HANDLE);
	if (!copy) {
		fprintf(stderr, "quest1_v4l2: vrlink: IVRPaths's vtable not found in the mappings: controls left alone\n");
		return;
	}
	real_path_handle = (path_handle_t)vt[SLOT_PATH_HANDLE];
	copy[SLOT_PATH_HANDLE] = (void *)input_path;
	*obj = copy;
	fprintf(stderr, "quest1_v4l2: vrlink: the Frame controller's controls read from the Touch's\n");
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
	if (p && version && is_vrlink()) {
		pthread_mutex_lock(&lock);
		if (!strcmp(version, "IVRSystem_026"))
			patch(p);
		else if (!strcmp(version, "IVRClientInternal_XXX"))
			patch_client(p);
		else if (!strcmp(version, "IVRPaths_002"))
			patch_paths(p);
		pthread_mutex_unlock(&lock);
	}
	return p;
}
