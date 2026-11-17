// SPDX-License-Identifier: MIT
/*
 * q1handsd: Meta's hand tracking (trackingservice's HandTrackingGlue, docs/hand-tracking.md) for glibc programs.
 * A bionic executable like q1trackd (same build, same environment: see src/q1track/build.sh and
 * device/holo-android-tracking.sh): it loads Meta's libossdk.oculus.so from the read-only system mount, reads the
 * hand state (IHands v6) and publishes it, with the head pose of the same instant, in Q1HANDS_SHM_PATH
 * (q1hands_shm.h), where driver_quest1 reads it. It also decides when the system tracks hands: Quest 1 has no
 * camera mux mode with hands and controllers together ("default" has the controller constellation frames,
 * "HandTracking" the hand frames), so like Meta's OS it switches the hand_tracking_enabled preference.
 *
 * Usage: q1handsd serve [hz]          publish at hz (default 90) and manage the mode (/etc/quest1-hands)
 *        q1handsd dump [seconds]      print the hand state
 *        q1handsd skeleton <out> [lib]  write Meta's bind-pose hand skeleton (needs the runtime's libraries in
 *                                     /data/local/tmp/q1vrapi: device/q1-vrapi-extract.sh)
 *
 * /etc/quest1-hands: "off" (default: never touch the preference), "auto" (hands when no controller is in use),
 * "hands" (hands only).
 */
#include <dlfcn.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "q1hands_shm.h"

/* ---- bionic process start (crtbegin_dynamic without the NDK), as in q1trackd.c ---- */
struct structors {
	void (**preinit_array)(void);
	void (**init_array)(void);
	void (**fini_array)(void);
};
extern void __libc_init(void *raw_args, void (*onexit)(void), int (*slingshot)(int, char **, char **),
			const struct structors *structors) __attribute__((noreturn));
int main(int argc, char **argv);
static int main3(int argc, char **argv, char **envp) { return main(argc, argv); }
static const struct structors no_structors; /* the linker runs .init_array; no .fini_array */
__attribute__((used)) static void start_c(void *raw_args) { __libc_init(raw_args, NULL, main3, &no_structors); }
__asm__(".text\n.global _start\n.type _start, %function\n_start:\n\tmov x0, sp\n\tb start_c\n");

/* ---- libossdk (docs/tracking-ipc.md 2.4): own declarations ---- */
struct big_state { _Alignas(16) uint8_t b[0x100]; };
struct hands_state { _Alignas(16) uint8_t b[0x1000]; };
struct dev_array { void *data; uint64_t count; uint8_t rest[48]; };
typedef struct big_state (*get_state_ptr_fn)(void *self, const int64_t *t);
typedef struct hands_state (*hands_get_state_fn)(void *self, const int64_t *t);
typedef struct big_state (*cin_get_buttons_fn)(void *self, uint64_t id);
typedef uint64_t (*is_in_hand_fn)(void *self, uint64_t id);
typedef uint64_t (*u64_fn)(void *self);
typedef uint64_t (*hub_changed_fn)(void *self, uint64_t *token);
typedef struct dev_array (*hub_enum_fn)(void *self);

static void *head, *hands, *hub, *ctrls, *cinput;
static volatile sig_atomic_t stop;
static void on_signal(int s) { stop = 1; }

static int64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void sleep_until(int64_t t)
{
	struct timespec ts = {t / 1000000000, t % 1000000000};
	clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
}

static int load_ossdk(void)
{
	void *lib = dlopen("libossdk.oculus.so", RTLD_NOW);
	if (!lib) {
		fprintf(stderr, "q1handsd: dlopen libossdk.oculus.so: %s\n", dlerror());
		return -1;
	}
	void *ndk = dlopen("libbinder_ndk.so", RTLD_NOW);
	void (*start_pool)(void) = ndk ? (void (*)(void))dlsym(ndk, "ABinderProcess_startThreadPool") : NULL;
	if (start_pool)
		start_pool();
	void *(*create)(long);
	if ((create = (void *(*)(long))dlsym(lib, "createHeadTracker")))
		head = create(8);
	if ((create = (void *(*)(long))dlsym(lib, "createHands")))
		hands = create(6);
	if ((create = (void *(*)(long))dlsym(lib, "createInputHub")))
		hub = create(1);
	if ((create = (void *(*)(long))dlsym(lib, "createControllers")))
		ctrls = create(6);
	if ((create = (void *(*)(long))dlsym(lib, "createControllerInput")))
		cinput = create(1);
	fprintf(stderr, "q1handsd: head %p hands %p hub %p controllers %p input %p\n", head, hands, hub, ctrls, cinput);
	return hands ? 0 : -1;
}

/* ---- the hand state: HandTrackingPoseData v6 (docs/tracking-ipc.md 3.5), two records of 0x248 bytes ---- */
#define HAND_STRIDE 0x248

static void get_head(int64_t t, struct q1hands_shm *m)
{
	m->head_valid = 0;
	if (!head)
		return;
	void **vt = *(void ***)head;
	struct big_state s = ((get_state_ptr_fn)vt[0])(head, &t);
	if (!s.b[0xA0] || !s.b[0x00])
		return;
	m->head_valid = 1;
	memcpy(m->head_rot, s.b + 0x10, 16);
	memcpy(m->head_pos, s.b + 0x20, 12);
}

static void parse_hand(const uint8_t *h, struct q1hands_hand *o)
{
	memset(o, 0, sizeof(*o));
	o->valid = 1;
	memcpy(&o->status, h + 0x00, 4);
	memcpy(&o->sample_ns, h + 0x08, 8);
	memcpy(&o->output_ns, h + 0x10, 8);
	memcpy(o->root_rot, h + 0x18, 16);
	memcpy(o->root_pos, h + 0x28, 12);
	memcpy(o->bone_rot, h + 0x8C, sizeof(o->bone_rot));
	memcpy(o->confidence, h + 0x1C0, 20);
	memcpy(o->pinch_occlusion, h + 0x1D4, 20);
	memcpy(&o->pinch_confidence, h + 0x1E8, 4);
	memcpy(&o->version, h + 0x1F0, 4);
	int any = 0;
	for (int i = 0; i < 5; i++)
		any |= o->confidence[i] >= 0.0f;
	float n = o->root_rot[0] * o->root_rot[0] + o->root_rot[1] * o->root_rot[1] + o->root_rot[2] * o->root_rot[2] +
		  o->root_rot[3] * o->root_rot[3];
	o->tracked = any && n > 0.9f && n < 1.1f;
}

/* libossdk's Optional flag after the state is not reliable (reads 1 with hands off, then the data is stack
 * garbage): the state is read only while getHandTrackingEnabled() says the system tracks hands */
static void get_hands(int64_t t, struct q1hands_shm *m)
{
	void **vt = *(void ***)hands;
	m->hands_enabled = (uint32_t)(((u64_fn)vt[3])(hands) & 1);
	if (!m->hands_enabled) {
		memset(m->hand, 0, sizeof(m->hand));
		memset(m->raw, 0, sizeof(m->raw));
		return;
	}
	uint64_t sc = ((u64_fn)vt[2])(hands); /* Optional<float>: the float in the low 32 bits, has_value in byte 4 */
	float scale = 0;
	if ((sc >> 32) & 1) {
		uint32_t bits = (uint32_t)sc;
		memcpy(&scale, &bits, 4);
	}
	m->hand_scale = scale;
	static struct hands_state s;
	memset(&s, 0, sizeof(s));
	s = ((hands_get_state_fn)vt[0])(hands, &t);
	memcpy(m->raw, s.b, sizeof(m->raw));
	parse_hand(s.b, &m->hand[0]);
	parse_hand(s.b + HAND_STRIDE, &m->hand[1]);
}

/* ---- controllers: only whether they are in use ---- */
#define MAXDEV 4
static struct { uint64_t id; uint32_t hand; } devs[MAXDEV];
static int ndevs;

static void refresh_devices(void)
{
	static uint64_t token;
	if (!hub)
		return;
	void **vt = *(void ***)hub;
	if (!((hub_changed_fn)vt[0])(hub, &token) && ndevs)
		return;
	struct dev_array a = ((hub_enum_fn)vt[1])(hub);
	ndevs = 0;
	for (uint64_t i = 0; a.data && i < a.count && ndevs < MAXDEV; i++) {
		const uint8_t *e = (const uint8_t *)a.data + i * 0xB8;
		memcpy(&devs[ndevs].id, e, 8);
		memcpy(&devs[ndevs].hand, e + 0x10, 4);
		ndevs++;
	}
}

/* [0] left (handedness 1), [1] right (2): in hand by libossdk's own detection, or a button, trigger or grip used */
static void get_controllers(struct q1hands_shm *m, int use[2])
{
	use[0] = use[1] = 0;
	m->controller_in_hand[0] = m->controller_in_hand[1] = 0;
	m->controller_connected[0] = m->controller_connected[1] = 0;
	refresh_devices();
	for (int i = 0; i < ndevs; i++) {
		int slot = devs[i].hand == 1 ? 0 : devs[i].hand == 2 ? 1 : -1;
		if (slot < 0)
			continue;
		m->controller_connected[slot] = 1;
		if (ctrls) {
			void **vt = *(void ***)ctrls;
			uint64_t r = ((is_in_hand_fn)vt[2])(ctrls, devs[i].id);
			/* Optional<bool>: value in byte 0, has_value in byte 1 [I] */
			m->controller_in_hand[slot] = ((r >> 8) & 1) && (r & 1);
		}
		int act = 0;
		if (cinput) {
			void **vt = *(void ***)cinput;
			struct big_state s = ((cin_get_buttons_fn)vt[1])(cinput, devs[i].id);
			if (s.b[0x98]) {
				for (int k = 0; k < 18; k++)
					act |= s.b[k * 8] & 1;
				float trig, grip;
				memcpy(&trig, s.b + 0x90, 4);
				memcpy(&grip, s.b + 0x94, 4);
				act |= trig > 0.1f || grip > 0.1f;
			}
		}
		use[slot] = act || m->controller_in_hand[slot];
	}
}

/* ---- the hand tracking preference ---- */
static int pref_hands = -1; /* what we last set, -1 = never */
static int we_set;

static void set_hands_pref(int on)
{
	char cmd[160];
	snprintf(cmd, sizeof(cmd), "/system/bin/oculuspreferences --setc hand_tracking_enabled %s >/dev/null 2>&1 &",
		 on ? "true" : "false");
	system(cmd);
	pref_hands = on;
	we_set = 1;
	fprintf(stderr, "q1handsd: hand_tracking_enabled -> %s\n", on ? "true" : "false");
}

static int read_policy(void) /* 0 off, 1 auto, 2 hands */
{
	FILE *f = fopen("/etc/quest1-hands", "r");
	char w[16] = {0};
	if (f) {
		if (!fgets(w, sizeof(w), f))
			w[0] = 0;
		fclose(f);
	}
	return !strncmp(w, "auto", 4) ? 1 : !strncmp(w, "hands", 5) ? 2 : 0;
}

/* skeleton file written by "skeleton": the bind pose of both hands, from Meta's runtime library */
struct skel_file {
	uint32_t magic;
	float rot[2][Q1HANDS_SKEL_BONES][4];
	float pos[2][Q1HANDS_SKEL_BONES][3];
	int16_t parent[Q1HANDS_SKEL_BONES];
};
#define SKEL_MAGIC 0x4c454b53u
#define SKEL_PATH "/run/q1track/handskel.bin"

static int do_skeleton(const char *out, const char *lib)
{
	void *l = dlopen(lib, RTLD_NOW);
	if (!l) {
		fprintf(stderr, "q1handsd: dlopen %s: %s\n", lib, dlerror());
		return 1;
	}
	int (*get)(void *, int, void *) = (int (*)(void *, int, void *))dlsym(l, "vrapi_GetHandSkeleton");
	if (!get) {
		fprintf(stderr, "q1handsd: no vrapi_GetHandSkeleton\n");
		return 1;
	}
	static struct skel_file f;
	f.magic = SKEL_MAGIC;
	for (int h = 0; h < 2; h++) {
		static uint32_t buf[0x1000];
		memset(buf, 0, sizeof(buf));
		buf[0] = 0xdf300001u; /* header version */
		if (get(NULL, h + 1, buf) != 0 || buf[1] != Q1HANDS_SKEL_BONES) {
			fprintf(stderr, "q1handsd: vrapi_GetHandSkeleton(%d) failed (%u bones)\n", h + 1, buf[1]);
			return 1;
		}
		/* header 8 words, then 24 ovrPosef (x y z w, then x y z), then 24 int16 parents */
		const float *p = (const float *)(buf + 8);
		for (int b = 0; b < Q1HANDS_SKEL_BONES; b++) {
			memcpy(f.rot[h][b], p + b * 7, 16);
			memcpy(f.pos[h][b], p + b * 7 + 4, 12);
		}
		memcpy(f.parent, p + Q1HANDS_SKEL_BONES * 7, sizeof(f.parent));
	}
	int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0 || write(fd, &f, sizeof(f)) != (long)sizeof(f)) {
		fprintf(stderr, "q1handsd: cannot write %s\n", out);
		return 1;
	}
	close(fd);
	fprintf(stderr, "q1handsd: skeleton written to %s\n", out);
	return 0;
}

static void load_skeleton(struct q1hands_shm *m)
{
	static struct skel_file f;
	int fd = open(SKEL_PATH, O_RDONLY);
	if (fd < 0 || read(fd, &f, sizeof(f)) != (long)sizeof(f) || f.magic != SKEL_MAGIC) {
		fprintf(stderr, "q1handsd: no hand skeleton (%s)\n", SKEL_PATH);
		if (fd >= 0)
			close(fd);
		return;
	}
	close(fd);
	memcpy(m->bind_rot, f.rot, sizeof(f.rot));
	memcpy(m->bind_pos, f.pos, sizeof(f.pos));
	memcpy(m->bind_parent, f.parent, sizeof(f.parent));
	m->skeleton_valid = 1;
}

static int do_dump(double seconds)
{
	static struct q1hands_shm m;
	int64_t t0 = now_ns();
	while (!stop && now_ns() - t0 < (int64_t)(seconds * 1e9)) {
		int64_t t = now_ns();
		get_hands(t, &m);
		printf("%.2f enabled %u scale %.3f\n", (t - t0) / 1e9, m.hands_enabled, m.hand_scale);
		for (int h = 0; h < 2; h++) {
			struct q1hands_hand *o = &m.hand[h];
			printf("  %c status %u tracked %u ts %.3f root rot [%.3f %.3f %.3f %.3f] pos [%.3f %.3f %.3f] conf "
			       "[%.2f %.2f %.2f %.2f %.2f]\n",
			       h ? 'R' : 'L', o->status, o->tracked, o->sample_ns / 1e9, o->root_rot[0], o->root_rot[1],
			       o->root_rot[2], o->root_rot[3], o->root_pos[0], o->root_pos[1], o->root_pos[2],
			       o->confidence[0], o->confidence[1], o->confidence[2], o->confidence[3], o->confidence[4]);
		}
		fflush(stdout);
		sleep_until(t + 200000000LL);
	}
	return 0;
}

#define OFF 16 /* the header words (magic, version, seq, pid) stay as the writer made them */

static int do_serve(double hz)
{
	mkdir("/run/q1track", 0755);
	int fd = open(Q1HANDS_SHM_PATH ".tmp", O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0 || ftruncate(fd, sizeof(struct q1hands_shm)) < 0) {
		fprintf(stderr, "q1handsd: cannot create %s\n", Q1HANDS_SHM_PATH);
		return 1;
	}
	struct q1hands_shm *m = mmap(NULL, sizeof(*m), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (m == MAP_FAILED) {
		fprintf(stderr, "q1handsd: mmap failed\n");
		return 1;
	}
	m->magic = Q1HANDS_MAGIC;
	m->version = Q1HANDS_VERSION;
	m->writer_pid = (uint32_t)getpid();
	load_skeleton(m);
	/* the file driver_quest1 (user steamos) writes for q1handviz: made here, as the directory is root's */
	int vfd = open("/run/q1track/handviz", O_RDWR | O_CREAT | O_CLOEXEC, 0666);
	if (vfd >= 0) {
		fchmod(vfd, 0666);
		if (ftruncate(vfd, 1024) != 0)
			fprintf(stderr, "q1handsd: handviz file size failed\n");
		close(vfd);
	}
	rename(Q1HANDS_SHM_PATH ".tmp", Q1HANDS_SHM_PATH);
	fprintf(stderr, "q1handsd: publishing at %.0f Hz in %s\n", hz, Q1HANDS_SHM_PATH);

	int64_t next = now_ns(), last_policy = 0, last_switch = 0, ctl_since = 0, idle_since = 0, last_log = 0;
	int policy = 0, use_ctl = 0;
	uint32_t last_status[2] = {~0u, ~0u}, last_tracked[2] = {2, 2};
	static struct q1hands_shm w; /* built here, copied under the seqlock */
	memcpy(&w, m, sizeof(w));
	while (!stop) {
		int64_t t = now_ns();
		if (t - last_policy > 1000000000LL) {
			int p = read_policy();
			if (p != policy)
				fprintf(stderr, "q1handsd: policy %d\n", p);
			policy = p;
			last_policy = t;
		}
		get_head(t, &w);
		get_hands(t, &w);
		w.mode = w.hands_enabled ? Q1HANDS_MODE_HANDS : Q1HANDS_MODE_CONTROLLERS;
		int use[2];
		get_controllers(&w, use);
		use_ctl = use[0] || use[1];
		int any_ctl = w.controller_connected[0] || w.controller_connected[1];

		/* mode policy: never touched while /etc/quest1-hands is "off" (what we set is put back) */
		if (policy == 0) {
			if (we_set && pref_hands == 1)
				set_hands_pref(0);
			we_set = 0;
		} else if (t - last_switch > 3000000000LL) {
			if (use_ctl) {
				idle_since = 0;
				if (!ctl_since)
					ctl_since = t;
			} else {
				ctl_since = 0;
				if (!idle_since)
					idle_since = t;
			}
			int cur = pref_hands < 0 ? (int)w.hands_enabled : pref_hands;
			int want = cur;
			if (policy == 2)
				want = 1;
			else if (use_ctl && t - ctl_since > 200000000LL)
				want = 0;
			else if (!use_ctl && (t - idle_since > 3000000000LL || !any_ctl))
				want = 1;
			if (want != cur) {
				set_hands_pref(want);
				last_switch = t;
			}
		}

		for (int h = 0; h < 2; h++)
			if (w.hand[h].status != last_status[h] || w.hand[h].tracked != last_tracked[h]) {
				fprintf(stderr, "q1handsd: hand %c status %u tracked %u conf %.2f %.2f %.2f %.2f %.2f\n",
					h ? 'R' : 'L', w.hand[h].status, w.hand[h].tracked, w.hand[h].confidence[0],
					w.hand[h].confidence[1], w.hand[h].confidence[2], w.hand[h].confidence[3],
					w.hand[h].confidence[4]);
				last_status[h] = w.hand[h].status;
				last_tracked[h] = w.hand[h].tracked;
			}
		if (t - last_log > 30000000000LL) {
			fprintf(stderr, "q1handsd: mode %u enabled %u ctl in use %d connected %u %u in hand %u %u\n", w.mode,
				w.hands_enabled, use_ctl, w.controller_connected[0], w.controller_connected[1],
				w.controller_in_hand[0], w.controller_in_hand[1]);
			last_log = t;
		}

		w.update_ns = t;
		w.updates++;
		m->seq++;
		__atomic_thread_fence(__ATOMIC_RELEASE);
		memcpy((char *)m + OFF, (char *)&w + OFF, sizeof(w) - OFF);
		__atomic_thread_fence(__ATOMIC_RELEASE);
		m->seq++;
		next += (int64_t)(1e9 / hz);
		if (next < t)
			next = t;
		sleep_until(next);
	}
	if (we_set && pref_hands == 1)
		set_hands_pref(0);
	unlink(Q1HANDS_SHM_PATH);
	return 0;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	if (argc >= 3 && !strcmp(argv[1], "skeleton"))
		return do_skeleton(argv[2], argc > 3 ? argv[3] : "/data/local/tmp/q1vrapi/libvrapiimpl.so");
	if (argc < 2 || (strcmp(argv[1], "serve") && strcmp(argv[1], "dump"))) {
		fprintf(stderr, "usage: %s serve [hz] | dump [seconds] | skeleton <out> [lib]\n", argv[0]);
		return 2;
	}
	if (load_ossdk() < 0)
		return 1;
	if (!strcmp(argv[1], "dump"))
		return do_dump(argc > 2 ? atof(argv[2]) : 1);
	return do_serve(argc > 2 ? atof(argv[2]) : 90);
}
