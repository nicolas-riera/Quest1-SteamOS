// SPDX-License-Identifier: MIT
/*
 * q1trackd: Meta's Quest 1 tracking (trackingservice, "Path M" in docs/tracking-6dof.md) for
 * glibc programs. A bionic executable that loads Meta's libossdk.oculus.so from the read-only
 * system_a mount and publishes the head pose (and later the controllers) in shared memory
 * (q1track_shm.h), where Monado's quest1 driver reads it.
 *
 * Why a separate bionic process: libossdk pulls in libandroid_runtime, libgui, libEGL and the
 * runtime APEX (linker namespaces from /system/etc/ld.config.29.txt), which libhybris inside
 * Monado cannot load safely. Run it as root (vrfocusserver grants uid 0 background head and input
 * tracking focus) with the tracking stack's property area (ro.vndk.version=29) bound over
 * /dev/__properties__, from a /data/local/tmp path (the linker's [unrestricted] section), as
 * holo-android-tracking's android-q1trackd unit does.
 *
 * Usage: q1trackd print [seconds] [hz]   print head poses (default 10 s at 10 Hz)
 *        q1trackd serve [hz]             publish poses in Q1TRACK_SHM_PATH (default 500 Hz)
 *
 * Built without the NDK: plain C with the glibc headers, linked against the device's bionic
 * libc/libdl (see build.sh); only types whose layout is the same in both libcs are used (no
 * errno, no struct sigaction, no stdio FILE internals).
 *
 * libossdk ABI (docs/tracking-ipc.md §2.4, this system build): createHeadTracker(8) returns an
 * IHeadTracker v8; vtable[0] getState(const Timestamp&) returns Optional<RigidBodyTrackingData v5>
 * through x8 (has_value at +0xA0), vtable[1] getIpd() returns Optional<float> packed in x0.
 */
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "q1track_shm.h"

/* ---- bionic process start (crtbegin_dynamic without the NDK) ---- */
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

/* ---- libossdk ---- */
struct head_state { /* Optional<RigidBodyTrackingData v5>, over-allocated */
	_Alignas(16) uint8_t b[0x100];
};
typedef struct head_state (*head_get_state_fn)(void *self, const int64_t *t);
typedef uint64_t (*head_get_ipd_fn)(void *self);

static void *head; /* IHeadTracker v8 */
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
		fprintf(stderr, "q1trackd: dlopen libossdk.oculus.so: %s\n", dlerror());
		return -1;
	}
	/* binder threads, for the trackingservice/vrfocus callbacks (the CLI starts them too) */
	void *ndk = dlopen("libbinder_ndk.so", RTLD_NOW);
	void (*start_pool)(void) = ndk ? (void (*)(void))dlsym(ndk, "ABinderProcess_startThreadPool") : NULL;
	if (start_pool)
		start_pool();
	else
		fprintf(stderr, "q1trackd: no ABinderProcess_startThreadPool\n");

	void *(*create_head)(long) = (void *(*)(long))dlsym(lib, "createHeadTracker");
	if (!create_head || !(head = create_head(8))) {
		fprintf(stderr, "q1trackd: createHeadTracker(8) failed\n");
		return -1;
	}
	return 0;
}

static void get_head(int64_t t, struct q1track_pose *out)
{
	void **vt = *(void ***)head;
	struct head_state s;
	memset(&s, 0, sizeof(s));
	s = ((head_get_state_fn)vt[0])(head, &t);
	memset(out, 0, sizeof(*out));
	if (!s.b[0xA0] || !s.b[0x00])
		return;
	out->valid = 1;
	memcpy(&out->status, s.b + 0x04, 4);
	memcpy(out->orientation, s.b + 0x10, 16);
	memcpy(out->position, s.b + 0x20, 12);
	memcpy(out->angular_velocity, s.b + 0x2c, 12);
	memcpy(out->linear_velocity, s.b + 0x38, 12);
	memcpy(out->angular_acceleration, s.b + 0x44, 12);
	memcpy(out->linear_acceleration, s.b + 0x50, 12);
	memcpy(&out->sample_ns, s.b + 0x60, 8);
	memcpy(&out->predicted_ns, s.b + 0x68, 8);
}

static float get_ipd(void)
{
	void **vt = *(void ***)head;
	uint64_t r = ((head_get_ipd_fn)vt[1])(head);
	float ipd;
	uint32_t bits = (uint32_t)r;
	memcpy(&ipd, &bits, 4);
	return (r >> 32) & 0xff ? ipd : 0.0f;
}

static int do_print(double seconds, double hz)
{
	int64_t t0 = now_ns(), next = t0;
	printf("ipd %.4f\n", get_ipd());
	while (!stop && now_ns() - t0 < (int64_t)(seconds * 1e9)) {
		struct q1track_pose p;
		int64_t t = now_ns();
		get_head(t, &p);
		if (!p.valid)
			printf("%.3f no head pose\n", (t - t0) / 1e9);
		else
			printf("%.3f status %x%s%s pos [%.4f %.4f %.4f] rot [%.4f %.4f %.4f %.4f] angvel [%.3f %.3f %.3f] "
			       "age %.1f ms\n",
			       (t - t0) / 1e9, p.status, p.status & Q1TRACK_POS_TRACKED ? " 6dof" : "",
			       p.status & Q1TRACK_ORI_TRACKED ? " ori" : "", p.position[0], p.position[1], p.position[2],
			       p.orientation[0], p.orientation[1], p.orientation[2], p.orientation[3],
			       p.angular_velocity[0], p.angular_velocity[1], p.angular_velocity[2],
			       (t - p.sample_ns) / 1e6);
		fflush(stdout);
		next += (int64_t)(1e9 / hz);
		sleep_until(next);
	}
	return 0;
}

static int do_serve(double hz)
{
	mkdir("/run/q1track", 0755);
	int fd = open(Q1TRACK_SHM_PATH ".tmp", O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0 || ftruncate(fd, sizeof(struct q1track_shm)) < 0) {
		fprintf(stderr, "q1trackd: cannot create %s\n", Q1TRACK_SHM_PATH);
		return 1;
	}
	struct q1track_shm *m = mmap(NULL, sizeof(*m), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (m == MAP_FAILED) {
		fprintf(stderr, "q1trackd: mmap failed\n");
		return 1;
	}
	m->magic = Q1TRACK_MAGIC;
	m->version = Q1TRACK_VERSION;
	m->writer_pid = (uint32_t)getpid();
	rename(Q1TRACK_SHM_PATH ".tmp", Q1TRACK_SHM_PATH);
	fprintf(stderr, "q1trackd: publishing at %.0f Hz in %s\n", hz, Q1TRACK_SHM_PATH);

	int64_t next = now_ns(), last_log = 0, last_ipd = 0;
	uint64_t valid = 0, n = 0;
	float ipd = 0;
	while (!stop) {
		struct q1track_pose p;
		int64_t t = now_ns();
		get_head(t, &p);
		if (t - last_ipd > 1000000000) {
			ipd = get_ipd();
			last_ipd = t;
		}
		m->seq++;
		__atomic_thread_fence(__ATOMIC_RELEASE);
		m->head = p;
		m->ipd = ipd;
		m->update_ns = t;
		m->updates++;
		__atomic_thread_fence(__ATOMIC_RELEASE);
		m->seq++;

		n++;
		valid += p.valid;
		if (t - last_log > 10000000000LL) {
			fprintf(stderr, "q1trackd: %llu/%llu head samples valid, status %x, pos [%.3f %.3f %.3f]\n",
				(unsigned long long)valid, (unsigned long long)n, p.status, p.position[0],
				p.position[1], p.position[2]);
			last_log = t;
			valid = n = 0;
		}
		next += (int64_t)(1e9 / hz);
		if (next < t)
			next = t;
		sleep_until(next);
	}
	unlink(Q1TRACK_SHM_PATH);
	return 0;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	if (argc < 2 || (strcmp(argv[1], "print") && strcmp(argv[1], "serve"))) {
		fprintf(stderr, "usage: %s print [seconds] [hz] | serve [hz]\n", argv[0]);
		return 2;
	}
	if (load_ossdk() < 0)
		return 1;
	if (!strcmp(argv[1], "print"))
		return do_print(argc > 2 ? atof(argv[2]) : 10, argc > 3 ? atof(argv[3]) : 10);
	return do_serve(argc > 2 ? atof(argv[2]) : 500);
}
