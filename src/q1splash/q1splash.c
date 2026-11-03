// q1splash: the boot animation, drawn straight on the Quest 1 panels like the Steam Frame does (no
// tracking, no compositor): reads raw RGBA frames from stdin (q1splash-play decodes Steam's own
// steamframe_startup_450x450.webm with GStreamer, paced to its clock) and shows each one centered
// around each lens axis, on a black background. Panels as src/q1black: gralloc buffers committed with
// MSMFB_ATOMIC_COMMIT on the DMA pipes 64/128, one per panel. The framebuffer is seen rotated by
// 180 degrees and its left half is the right eye; each half gets the image rotated, so it stays
// upright. Two buffers: a buffer is written again only after the next commit,
// which releases it. At the end of the stream the panels go black and are powered down, before
// Monado takes them (monado.service starts after quest1-bootsplash.service).
// Build (in Holo; msm_mdp_ext.h from the kernel uapi, as for src/q1black):
//   gcc -O2 -Wall -o q1splash q1splash.c -Iinc -I/opt/hybris/include -L/opt/hybris/lib
//       -Wl,-rpath,/opt/hybris/lib -lhybris-common
// Run: ... | LD_PRELOAD=/opt/hybris/lib/libbionictls.so q1splash <width> <height>
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

#define PANEL_W 1440
#define PANEL_H 1600

// Each lens axis in its half of the buffer, as Monado's warp has it (comp_window_mdp.c: lens centre
// 2.986 mm towards the nose and 2.72 mm up from the panel centre, 41.25 um pixels, the buffer seen turned
// by 180 degrees): the image sits
// at infinity, centered in both eyes.
static const int lens_x[2] = {PANEL_W / 2 + 72, PANEL_W / 2 - 72}, lens_y = PANEL_H / 2 + 66;

static volatile sig_atomic_t stop;
static void on_signal(int s) { stop = 1; }

static int (*ahb_lock)(void *, uint64_t, int32_t, const void *, void **);
static int (*ahb_unlock)(void *, int32_t *);

struct buffer {
	void *ahb;
	ahb_desc d;
	int fd;
	uint32_t flags;
};

static int alloc_buffer(int (*allocate)(const ahb_desc *, void **), void (*describe)(const void *, ahb_desc *),
                        const native_handle *(*get_handle)(const void *), struct buffer *b)
{
	b->d = (ahb_desc){.width = 2 * PANEL_W, .height = PANEL_H, .layers = 1, .format = AHB_FORMAT_R8G8B8A8_UNORM,
	                  .usage = AHB_USAGE_CPU_WRITE_OFTEN | AHB_USAGE_COMPOSER_OVERLAY};
	if (allocate(&b->d, &b->ahb))
		return -1;
	describe(b->ahb, &b->d);
	const native_handle *nh = get_handle(b->ahb);
	b->fd = nh->data[0];
	b->flags = (uint32_t)nh->data[3];
	return 0;
}

// no frame: all black; else the frame (w x h RGBA) rotated by 180 degrees around each lens axis
// (the rest of the buffer stays black from the first call)
static int draw(struct buffer *b, const uint8_t *frame, int w, int h)
{
	uint32_t *px = NULL;
	if (ahb_lock(b->ahb, AHB_USAGE_CPU_WRITE_OFTEN, -1, NULL, (void **)&px))
		return -1;
	const uint32_t stride = b->d.stride;
	if (!frame)
		for (uint32_t y = 0; y < PANEL_H; y++)
			for (uint32_t x = 0; x < 2 * PANEL_W; x++)
				px[(size_t)y * stride + x] = 0xff000000;
	else {
		const uint32_t *src = (const uint32_t *)frame;
		const int y0 = lens_y - h / 2;
		for (int half = 0; half < 2; half++)
			for (int y = 0; y < h; y++) {
				uint32_t *dst = px + (size_t)(y0 + h - 1 - y) * stride + half * PANEL_W + lens_x[half] - w / 2 + w - 1;
				const uint32_t *s = src + (size_t)y * w;
				for (int x = 0; x < w; x++)
					dst[-x] = s[x] | 0xff000000;
			}
	}
	ahb_unlock(b->ahb, NULL);
	return 0;
}

static int commit(int fb, const struct buffer *b)
{
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
		l->buffer.width = b->d.stride;
		l->buffer.height = b->d.height;
		l->buffer.format = (b->flags & 0x08000000) ? MDP_RGBA_8888_UBWC : MDP_RGBA_8888;
		l->buffer.planes[0].fd = b->fd;
		l->buffer.planes[0].stride = b->d.stride * 4;
		l->buffer.plane_count = 1;
		l->buffer.fence = -1;
	}
	struct mdp_layer_commit c;
	memset(&c, 0, sizeof(c));
	c.version = MDP_COMMIT_VERSION_1_0;
	c.commit_v1.input_layers = layers;
	c.commit_v1.input_layer_cnt = 2;
	c.commit_v1.release_fence = -1;
	c.commit_v1.retire_fence = -1;
	if (ioctl(fb, MSMFB_ATOMIC_COMMIT, &c) < 0) {
		fprintf(stderr, "q1splash: MSMFB_ATOMIC_COMMIT: %s (layer errors %d %d)\n", strerror(errno),
		        layers[0].error_code, layers[1].error_code);
		return -1;
	}
	if (c.commit_v1.retire_fence >= 0)
		close(c.commit_v1.retire_fence);
	if (c.commit_v1.release_fence >= 0)
		close(c.commit_v1.release_fence);
	return 0;
}

static size_t read_full(int fd, uint8_t *p, size_t n)
{
	size_t got = 0;
	while (got < n && !stop) {
		ssize_t r = read(fd, p + got, n - got);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		got += (size_t)r;
	}
	return got;
}

int main(int argc, char **argv)
{
	int w = argc > 1 ? atoi(argv[1]) : 450, h = argc > 2 ? atoi(argv[2]) : 450;
	if (w <= 0 || h <= 0 || w > PANEL_W - 2 * 72 || h > PANEL_H - 2 * 66) {
		fprintf(stderr, "usage: %s <width> <height> < rgba-frames\n", argv[0]);
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);

	void *lib = android_dlopen("libnativewindow.so", RTLD_NOW);
	if (!lib) {
		fprintf(stderr, "q1splash: cannot load libnativewindow.so\n");
		return 1;
	}
	int (*allocate)(const ahb_desc *, void **) = android_dlsym(lib, "AHardwareBuffer_allocate");
	void (*describe)(const void *, ahb_desc *) = android_dlsym(lib, "AHardwareBuffer_describe");
	const native_handle *(*get_handle)(const void *) = android_dlsym(lib, "AHardwareBuffer_getNativeHandle");
	ahb_lock = android_dlsym(lib, "AHardwareBuffer_lock");
	ahb_unlock = android_dlsym(lib, "AHardwareBuffer_unlock");

	struct buffer bufs[2];
	for (int i = 0; i < 2; i++)
		if (alloc_buffer(allocate, describe, get_handle, &bufs[i]) || draw(&bufs[i], NULL, 0, 0)) {
			fprintf(stderr, "q1splash: cannot allocate the panel buffers\n");
			return 1;
		}

	int fb = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	if (fb < 0) {
		perror("q1splash: /dev/fb0");
		return 1;
	}
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	if (ioctl(fb, FBIOBLANK, FB_BLANK_UNBLANK) < 0)
		perror("q1splash: FBIOBLANK unblank");
	int frames = 0, fails = 0, cur = 0;
	if (commit(fb, &bufs[1]) < 0) // black, while the decoder starts
		fails++;
	const size_t size = (size_t)w * h * 4;
	uint8_t *frame = malloc(size);
	while (frame && !stop && fails < 5 && read_full(0, frame, size) == size) {
		if (draw(&bufs[cur], frame, w, h) < 0 || commit(fb, &bufs[cur]) < 0) {
			fails++;
			continue;
		}
		frames++;
		cur ^= 1;
	}
	// end black, then hand the panels over powered down (as src/q1black)
	draw(&bufs[cur], NULL, 0, 0);
	commit(fb, &bufs[cur]);
	usleep(50000);
	ioctl(fb, FBIOBLANK, FB_BLANK_POWERDOWN);
	close(fb);
	printf("q1splash: %d frames shown, %d failed commits\n", frames, fails);
	free(frame);
	return frames > 0 ? 0 : 1;
}
