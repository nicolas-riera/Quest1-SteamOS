// q1grid: lens calibration grid for the Quest 1 panels. Draws a regular grid of view directions
// (lines every `spacing` in tangent units, thicker axes through the lens centre) through the lens
// model, each colour channel with its own chromatic scale: with the right model, both eyes see
// straight, white, fused lines everywhere. The model is the one of driver_quest1 ComputeDistortion
// and Monado's warp (Meta's CatmullRom10, docs/quest1-lens.md); its parameters are read from
// /tmp/q1grid.conf ("key value" lines) and the grid is redrawn as soon as the file changes, so the
// parameters can be tuned live while the headset is worn.
//   mpt      metres per tan angle at the centre (0.0389)
//   kscale   scale of the radial correction K[i] - 1 (1)
//   lx, ly   panel centre - lens centre in metres (0.002986, -0.00272), lx mirrored per eye
//   r0, r1   red scale r0 + r1 * rsq (0.996, -0.007); b0, b1 the same for blue (1.006, 0.020)
//   flipv    1 mirrors the lens model vertically (vertical asymmetry check)
//   gdx, gdy green offset in panel pixels as seen (right, down): the OLED's green subpixels do not
//            sit with the red and blue ones; rdx, rdy, bdx, bdy the same for red and blue
//   spacing  grid step in tangent units (0.1); eyes: 3 both, 1 left only, 2 right only
// The panels show the framebuffer turned by 180 degrees; the left eye sees its right half.
// It owns the display: stop Monado and SteamVR first (only one process may commit to /dev/fb0).
// Build (in Holo, as src/q1black):
//   gcc -O2 -Wall -o q1grid q1grid.c -lm -Iinc -I/opt/hybris/include -L/opt/hybris/lib
//       -Wl,-rpath,/opt/hybris/lib -lhybris-common
// Run: LD_PRELOAD=/opt/hybris/lib/libbionictls.so ./q1grid [seconds (default 600)]
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
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

#define PANEL_W 1440
#define PANEL_H 1600
#define PANEL_W_M 0.0594f
#define PANEL_H_M 0.0660f
#define CONF "/tmp/q1grid.conf"

static struct {
	float mpt, kscale, lx, ly, r0, r1, b0, b1, spacing;
	float dx[3], dy[3]; // per-channel offsets in pixels (R, G, B)
	int flipv, eyes;
} P;

static volatile sig_atomic_t stop;
static void on_signal(int s) { stop = 1; }

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void defaults(void)
{
	P.mpt = 0.0389f, P.kscale = 1, P.lx = 0.002986f, P.ly = -0.00272f;
	P.r0 = 0.996f, P.r1 = -0.007f, P.b0 = 1.006f, P.b1 = 0.020f;
	P.spacing = 0.1f, P.flipv = 0, P.eyes = 3;
	memset(P.dx, 0, sizeof(P.dx));
	memset(P.dy, 0, sizeof(P.dy));
}

static void read_conf(void)
{
	defaults();
	FILE *f = fopen(CONF, "r");
	if (!f)
		return;
	char key[32];
	float v;
	while (fscanf(f, "%31s %f", key, &v) == 2) {
		if (!strcmp(key, "mpt")) P.mpt = v;
		else if (!strcmp(key, "kscale")) P.kscale = v;
		else if (!strcmp(key, "lx")) P.lx = v;
		else if (!strcmp(key, "ly")) P.ly = v;
		else if (!strcmp(key, "r0")) P.r0 = v;
		else if (!strcmp(key, "r1")) P.r1 = v;
		else if (!strcmp(key, "b0")) P.b0 = v;
		else if (!strcmp(key, "b1")) P.b1 = v;
		else if (!strcmp(key, "spacing") && v > 0.01f) P.spacing = v;
		else if (!strcmp(key, "flipv")) P.flipv = (int)v;
		else if (!strcmp(key, "eyes")) P.eyes = (int)v;
		else if (strlen(key) == 3 && strchr("rgb", key[0]) && key[1] == 'd' && strchr("xy", key[2]))
			(key[2] == 'x' ? P.dx : P.dy)[strchr("rgb", key[0]) - "rgb"] = v;
	}
	fclose(f);
}

//! Meta's CatmullRom10 radial scale (as driver_quest1 / Monado), K[i] - 1 scaled by kscale
static float catmull_rom10(float rsq)
{
	static const float K0[11] = {1.0f, 1.0374f, 1.0810f, 1.1330f, 1.1970f, 1.2754f,
	                             1.3771f, 1.5133f, 1.7018f, 1.9732f, 2.3000f};
	float K[11];
	for (int i = 0; i < 11; i++)
		K[i] = 1.0f + (K0[i] - 1.0f) * P.kscale;
	float fk = 10.0f * rsq;
	int k = (int)floorf(fk);
	k = k < 0 ? 0 : (k > 10 ? 10 : k);
	float t = fk - (float)k, p0, m0, p1, m1;
	if (k == 0) {
		p0 = 1.0f, m0 = K[1] - K[0], p1 = K[1], m1 = 0.5f * (K[2] - K[0]);
	} else if (k < 9) {
		p0 = K[k], m0 = 0.5f * (K[k + 1] - K[k - 1]), p1 = K[k + 1], m1 = 0.5f * (K[k + 2] - K[k]);
	} else if (k == 9) {
		p0 = K[9], m0 = 0.5f * (K[10] - K[8]), p1 = K[10], m1 = K[10] - K[9];
	} else {
		p0 = K[10], m0 = K[10] - K[9], p1 = p0 + m0, m1 = m0;
	}
	float omt = 1.0f - t;
	return (p0 * (1.0f + 2.0f * t) + m0 * t) * omt * omt + (p1 * (1.0f + 2.0f * omt) - m1 * omt) * t * t;
}

//! 1 when the view direction (tx, ty) is on a grid line; the line width grows with the local
//! magnification s so lines stay ~2-3 pixels wide
static int on_line(float tx, float ty, float s)
{
	float w = 0.0012f * s;
	float axis = 2.5f * w;
	if (fabsf(tx) < axis || fabsf(ty) < axis)
		return 1;
	float fx = tx / P.spacing - floorf(tx / P.spacing + 0.5f);
	float fy = ty / P.spacing - floorf(ty / P.spacing + 0.5f);
	return fabsf(fx) * P.spacing < w || fabsf(fy) * P.spacing < w;
}

static void draw(uint32_t *px, uint32_t stride)
{
	for (int Y = 0; Y < PANEL_H; Y++) {
		uint32_t *row = px + (size_t)Y * stride;
		for (int X = 0; X < 2 * PANEL_W; X++) {
			int h = X / PANEL_W, view = 1 - h; // half 0 (x = 0) is the right eye, view 1
			if (!(P.eyes & (view == 0 ? 1 : 2))) {
				row[X] = 0xff000000;
				continue;
			}
			float fx = ((X % PANEL_W) + 0.5f) / PANEL_W, fy = (Y + 0.5f) / PANEL_H;
			float sx = view == 0 ? -1.0f : 1.0f;
			float cs[3] = {0, 1.0f, 0};
			uint32_t c = 0xff000000;
			for (int ch = 0; ch < 3; ch++) {
				// the view is the half turned by 180 degrees; a subpixel sitting dx right of the pixel
				// centre shows the point dx further right
				float u = 1.0f - fx + P.dx[ch] / PANEL_W, v = 1.0f - fy + P.dy[ch] / PANEL_H;
				if (P.flipv)
					v = 1.0f - v;
				float x = ((u - 0.5f) * PANEL_W_M + sx * P.lx) / P.mpt;
				float y = ((0.5f - v) * PANEL_H_M + P.ly) / P.mpt;
				float rsq = x * x + y * y, s = catmull_rom10(rsq);
				cs[0] = P.r0 + P.r1 * rsq, cs[2] = P.b0 + P.b1 * rsq;
				if (on_line(x * s * cs[ch], y * s * cs[ch], s))
					c |= 0xffu << (8 * ch); // RGBA in memory: R is the low byte
			}
			row[X] = c;
		}
	}
}

int main(int argc, char **argv)
{
	double seconds = argc > 1 ? atof(argv[1]) : 600;
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

	ahb_desc d = {.width = 2 * PANEL_W, .height = PANEL_H, .layers = 1, .format = AHB_FORMAT_R8G8B8A8_UNORM,
	              .usage = AHB_USAGE_CPU_WRITE_OFTEN | AHB_USAGE_COMPOSER_OVERLAY};
	void *ahb = NULL;
	if (allocate(&d, &ahb)) {
		fprintf(stderr, "AHardwareBuffer_allocate failed\n");
		return 1;
	}
	describe(ahb, &d);
	const native_handle *nh = get_handle(ahb);
	int buf_fd = nh->data[0];
	uint32_t flags = (uint32_t)nh->data[3];
	if (flags & 0x08000000) {
		fprintf(stderr, "q1grid: the buffer is UBWC-compressed, cannot draw into it\n");
		return 1;
	}

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
		l->src_rect = (struct mdp_rect){i * PANEL_W, 0, PANEL_W, PANEL_H};
		l->dst_rect = (struct mdp_rect){i * PANEL_W, 0, PANEL_W, PANEL_H};
		l->buffer.width = d.stride;
		l->buffer.height = d.height;
		l->buffer.format = MDP_RGBA_8888;
		l->buffer.planes[0].fd = buf_fd;
		l->buffer.planes[0].stride = d.stride * 4;
		l->buffer.plane_count = 1;
		l->buffer.fence = -1;
	}

	time_t mtime = -1;
	int commits = 0, fails = 0, dirty = 1;
	double t0 = now_s(), last_commit = 0;
	while (!stop && (seconds <= 0 || now_s() - t0 < seconds)) {
		struct stat st;
		time_t m = stat(CONF, &st) == 0 ? st.st_mtime : 0;
		if (m != mtime) {
			mtime = m;
			read_conf();
			uint32_t *px = NULL;
			if (lock(ahb, AHB_USAGE_CPU_WRITE_OFTEN, -1, NULL, (void **)&px) == 0) {
				double td = now_s();
				draw(px, d.stride);
				unlock(ahb, NULL);
				printf("q1grid: mpt %.4f kscale %.3f lx %.5f ly %.5f r %.4f%+.4f b %.4f%+.4f flipv %d spacing %.2f "
				       "eyes %d, offsets R %.2f,%.2f G %.2f,%.2f B %.2f,%.2f (%.0f ms)\n",
				       P.mpt, P.kscale, P.lx, P.ly, P.r0, P.r1, P.b0, P.b1, P.flipv, P.spacing, P.eyes,
				       P.dx[0], P.dy[0], P.dx[1], P.dy[1], P.dx[2], P.dy[2], (now_s() - td) * 1000);
				dirty = 1;
			}
		}
		// video-mode panels keep scanning the last buffer: commit after a redraw, and every 0.5 s
		if (dirty || now_s() - last_commit > 0.5) {
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
			dirty = 0;
			last_commit = now_s();
		}
		usleep(50000);
	}
	printf("q1grid: %d commits, %d failures in %.1f s; powering the panels down\n", commits, fails,
	       now_s() - t0);
	ioctl(fb, FBIOBLANK, FB_BLANK_POWERDOWN);
	close(fb);
	return commits > 0 ? 0 : 1;
}
