// q1black: light the Quest 1 panels with a black frame so the display produces TE (tearing effect)
// pulses while Monado is stopped. Meta's sensors HAL timestamps the SyncBoss IMU against the TE
// time in /sys/bus/spi/devices/spi12.0/control/te_timestamp and drops every IMU sample while TE is
// frozen, so a Path M tracking test (docs/tracking-6dof.md) needs the panels running.
// FBIOBLANK alone does not start TE: the MDP interface only starts on a real commit. After the
// first MSMFB_ATOMIC_COMMIT the video-mode panels keep scanning out the same (black) buffer; the
// frame is re-committed every `period` ms anyway, in case another client (vrcompositor's direct
// scanout) replaced it. OLED black = pixels off, so nothing burns in. On exit (time out, SIGINT,
// SIGTERM) the panels are powered down again.
// Build (in Holo; msm_mdp_ext.h from the kernel uapi, as for src/mdptest):
//   gcc -O2 -Wall -o q1black q1black.c -Iinc -I/opt/hybris/include -L/opt/hybris/lib
//       -Wl,-rpath,/opt/hybris/lib -lhybris-common
// Run: LD_PRELOAD=/opt/hybris/lib/libbionictls.so ./q1black [seconds (0 = until killed)] [period ms]
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#ifndef __user
#define __user // kernel uapi annotation
#endif
#include <linux/msm_mdp_ext.h>

extern void *android_dlopen(const char *filename, int flag);
extern void *android_dlsym(void *handle, const char *symbol);

typedef struct {
	uint32_t width, height, layers, format;
	uint64_t usage;
	uint32_t stride, rfu0;
	uint64_t rfu1;
} ahb_desc;

typedef struct {
	int version, numFds, numInts;
	int data[];
} native_handle;

#define AHB_FORMAT_R8G8B8A8_UNORM 1
#define AHB_USAGE_CPU_WRITE_OFTEN (3ULL << 4)
#define AHB_USAGE_COMPOSER_OVERLAY (1ULL << 11)

static volatile sig_atomic_t stop;
static void on_signal(int s) { stop = 1; }

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static unsigned long long te(void)
{
	unsigned long long v = 0;
	FILE *f = fopen("/sys/bus/spi/devices/spi12.0/control/te_timestamp", "r");
	if (f) {
		if (fscanf(f, "%llu", &v) != 1)
			v = 0;
		fclose(f);
	}
	return v;
}

int main(int argc, char **argv)
{
	double seconds = argc > 1 ? atof(argv[1]) : 60;
	int period_ms = argc > 2 ? atoi(argv[2]) : 500;
	setvbuf(stdout, NULL, _IOLBF, 0);

	void *lib = android_dlopen("libnativewindow.so", RTLD_NOW);
	if (!lib) {
		fprintf(stderr, "cannot load libnativewindow.so\n");
		return 1;
	}
	int (*allocate)(const ahb_desc *, void **) = android_dlsym(lib, "AHardwareBuffer_allocate");
	void (*describe)(const void *, ahb_desc *) = android_dlsym(lib, "AHardwareBuffer_describe");
	const native_handle *(*get_handle)(const void *) = android_dlsym(lib, "AHardwareBuffer_getNativeHandle");
	int (*lock)(void *, uint64_t, int32_t, const void *, void **) = android_dlsym(lib, "AHardwareBuffer_lock");
	int (*unlock)(void *, int32_t *) = android_dlsym(lib, "AHardwareBuffer_unlock");

	// linear CPU-writable buffer, cleared to opaque black
	ahb_desc d = {.width = 2880, .height = 1600, .layers = 1, .format = AHB_FORMAT_R8G8B8A8_UNORM,
	              .usage = AHB_USAGE_CPU_WRITE_OFTEN | AHB_USAGE_COMPOSER_OVERLAY};
	void *ahb = NULL;
	if (allocate(&d, &ahb)) {
		fprintf(stderr, "AHardwareBuffer_allocate failed\n");
		return 1;
	}
	describe(ahb, &d);
	uint32_t *px = NULL;
	if (lock(ahb, AHB_USAGE_CPU_WRITE_OFTEN, -1, NULL, (void **)&px)) {
		fprintf(stderr, "AHardwareBuffer_lock failed\n");
		return 1;
	}
	for (size_t i = 0; i < (size_t)d.stride * d.height; i++)
		px[i] = 0xff000000;
	unlock(ahb, NULL);
	const native_handle *nh = get_handle(ahb);
	int buf_fd = nh->data[0];
	uint32_t flags = (uint32_t)nh->data[3];

	int fb = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	if (fb < 0) {
		perror("/dev/fb0");
		return 1;
	}
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	unsigned long long te0 = te();
	if (ioctl(fb, FBIOBLANK, FB_BLANK_UNBLANK) < 0)
		perror("FBIOBLANK unblank");

	struct mdp_input_layer layers[2];
	memset(layers, 0, sizeof(layers));
	const uint32_t pipes[2] = {64, 128}; // DMA0, DMA1: one per panel, as Monado
	for (int i = 0; i < 2; i++) {
		struct mdp_input_layer *l = &layers[i];
		l->pipe_ndx = pipes[i];
		l->alpha = 0xff;
		l->blend_op = BLEND_OP_OPAQUE;
		l->color_space = MDP_CSC_ITU_R_709;
		l->src_rect = (struct mdp_rect){i * 1440, 0, 1440, 1600};
		l->dst_rect = (struct mdp_rect){i * 1440, 0, 1440, 1600};
		l->buffer.width = d.stride;
		l->buffer.height = d.height;
		l->buffer.format = (flags & 0x08000000) ? MDP_RGBA_8888_UBWC : MDP_RGBA_8888;
		l->buffer.planes[0].fd = buf_fd;
		l->buffer.planes[0].stride = d.stride * 4;
		l->buffer.plane_count = 1;
		l->buffer.fence = -1;
	}

	int commits = 0, fails = 0;
	double t0 = now_s();
	while (!stop && (seconds <= 0 || now_s() - t0 < seconds)) {
		struct mdp_layer_commit c;
		memset(&c, 0, sizeof(c));
		c.version = MDP_COMMIT_VERSION_1_0;
		c.commit_v1.input_layers = layers;
		c.commit_v1.input_layer_cnt = 2;
		c.commit_v1.release_fence = -1;
		c.commit_v1.retire_fence = -1;
		if (ioctl(fb, MSMFB_ATOMIC_COMMIT, &c) < 0) {
			fprintf(stderr, "MSMFB_ATOMIC_COMMIT: %s (layer errors %d %d)\n", strerror(errno),
			        layers[0].error_code, layers[1].error_code);
			if (++fails > 5)
				break;
		} else {
			commits++;
			if (c.commit_v1.retire_fence >= 0)
				close(c.commit_v1.retire_fence);
			if (c.commit_v1.release_fence >= 0)
				close(c.commit_v1.release_fence);
		}
		if (commits == 1) {
			usleep(100000);
			unsigned long long t1 = te();
			usleep(100000);
			unsigned long long t2 = te();
			printf("q1black: panels lit black; te_timestamp %llu -> %llu -> %llu (%s)\n", te0, t1, t2,
			       t2 != t1 ? "TE running" : "TE FROZEN");
		}
		usleep(period_ms * 1000);
	}
	printf("q1black: %d commits, %d failures in %.1f s; te_timestamp now %llu; powering the panels down\n",
	       commits, fails, now_s() - t0, te());
	ioctl(fb, FBIOBLANK, FB_BLANK_POWERDOWN);
	close(fb);
	return commits > 0 ? 0 : 1;
}
