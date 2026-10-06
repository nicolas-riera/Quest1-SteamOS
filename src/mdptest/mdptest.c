// mdptest: show a gralloc (AHardwareBuffer) buffer on the Quest panels through the MDSS
// atomic commit ioctl, the zero-copy path a Monado compositor target will use.
// Build (in Holo, with the kernel's msm_mdp*.h uapi headers in ./inc/linux):
//   gcc -O2 -o mdptest mdptest.c -Iinc -I/opt/hybris/include -L/opt/hybris/lib -Wl,-rpath,/opt/hybris/lib -lhybris-common
// Run: LD_PRELOAD=/opt/hybris/lib/libbionictls.so ./mdptest [seconds] [cpu]
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

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
#define AHB_USAGE_GPU_SAMPLED_IMAGE (1ULL << 8)
#define AHB_USAGE_GPU_COLOR_OUTPUT (1ULL << 9)
#define AHB_USAGE_COMPOSER_OVERLAY (1ULL << 11)

static struct {
	int (*allocate)(const ahb_desc *, void **);
	void (*describe)(const void *, ahb_desc *);
	const native_handle *(*get_handle)(const void *);
	int (*lock)(void *, uint64_t, int32_t, const void *, void **);
	int (*unlock)(void *, int32_t *);
} nw;

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
	double seconds = argc > 1 ? atof(argv[1]) : 2;
	int cpu = argc > 2;

	void *lib = android_dlopen("libnativewindow.so", RTLD_NOW);
	if (!lib) {
		fprintf(stderr, "cannot load libnativewindow.so\n");
		return 1;
	}
	nw.allocate = android_dlsym(lib, "AHardwareBuffer_allocate");
	nw.describe = android_dlsym(lib, "AHardwareBuffer_describe");
	nw.get_handle = android_dlsym(lib, "AHardwareBuffer_getNativeHandle");
	nw.lock = android_dlsym(lib, "AHardwareBuffer_lock");
	nw.unlock = android_dlsym(lib, "AHardwareBuffer_unlock");

	ahb_desc d = {.width = 2880, .height = 1600, .layers = 1, .format = AHB_FORMAT_R8G8B8A8_UNORM,
	              .usage = AHB_USAGE_GPU_SAMPLED_IMAGE | AHB_USAGE_GPU_COLOR_OUTPUT | AHB_USAGE_COMPOSER_OVERLAY};
	if (cpu)
		d.usage |= AHB_USAGE_CPU_WRITE_OFTEN;
	void *ahb = NULL;
	int r = nw.allocate(&d, &ahb);
	if (r) {
		fprintf(stderr, "AHardwareBuffer_allocate: %d\n", r);
		return 1;
	}
	nw.describe(ahb, &d);
	const native_handle *nh = nw.get_handle(ahb);
	printf("ahb %ux%u stride %u usage %#llx; handle fds %d ints %d:", d.width, d.height, d.stride,
	       (unsigned long long)d.usage, nh->numFds, nh->numInts);
	for (int i = 0; i < nh->numFds + nh->numInts; i++)
		printf(" %#x", nh->data[i]);
	printf("\n");
	// gralloc private_handle_t: fd, fd_metadata, magic, flags, width, height, ...
	int buf_fd = nh->data[0];
	uint32_t flags = (uint32_t)nh->data[3];
	printf("priv flags %#x%s\n", flags, (flags & 0x08000000) ? " (UBWC)" : "");

	if (cpu) {
		uint32_t *px = NULL;
		r = nw.lock(ahb, AHB_USAGE_CPU_WRITE_OFTEN, -1, NULL, (void **)&px);
		if (r) {
			fprintf(stderr, "lock: %d\n", r);
			return 1;
		}
		// Dim colors (OLED): fb left half blue, right half green, white block top-left of each half.
		for (uint32_t y = 0; y < d.height; y++)
			for (uint32_t x = 0; x < d.width; x++) {
				uint32_t hx = x % 1440;
				uint32_t c = x < 1440 ? 0xff400000 : 0xff004000; // ABGR in memory = RGBA bytes
				if (hx < 200 && y < 200)
					c = 0xff808080;
				px[y * d.stride + x] = c;
			}
		nw.unlock(ahb, NULL);
	}

	int fb = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	if (fb < 0) {
		perror("/dev/fb0");
		return 1;
	}
	if (ioctl(fb, FBIOBLANK, FB_BLANK_UNBLANK) < 0)
		perror("FBIOBLANK unblank");
	int vs = open("/sys/class/graphics/fb0/vsync_event", O_RDONLY | O_CLOEXEC);
	char dummy[64];
	read(vs, dummy, sizeof(dummy));
	unsigned int vsync_on = 1;
	if (ioctl(fb, MSMFB_OVERLAY_VSYNC_CTRL, &vsync_on) < 0)
		perror("MSMFB_OVERLAY_VSYNC_CTRL");

	struct mdp_input_layer layers[2];
	memset(layers, 0, sizeof(layers));
	const uint32_t pipes[2] = {64, 128}; // DMA0, DMA1
	for (int i = 0; i < 2; i++) {
		struct mdp_input_layer *l = &layers[i];
		l->pipe_ndx = pipes[i];
		l->alpha = 0xff;
		l->z_order = 0;
		l->blend_op = BLEND_OP_OPAQUE;
		l->color_space = MDP_CSC_ITU_R_709;
		l->src_rect = (struct mdp_rect){i * 1440, 0, 1440, 1600};
		l->dst_rect = (struct mdp_rect){i * 1440, 0, 1440, 1600};
		l->buffer.width = d.stride;
		l->buffer.height = d.height;
		l->buffer.format = (flags & 0x08000000) ? MDP_RGBA_8888_UBWC : MDP_RGBA_8888;
		l->buffer.planes[0].fd = buf_fd;
		l->buffer.planes[0].offset = 0;
		l->buffer.planes[0].stride = d.stride * 4;
		l->buffer.plane_count = 1;
		l->buffer.fence = -1;
	}

	int frames = 0, fails = 0;
	double t0 = now_ms(), last = t0, max_dt = 0;
	while (now_ms() - t0 < seconds * 1000) {
		struct mdp_layer_commit c;
		memset(&c, 0, sizeof(c));
		c.version = MDP_COMMIT_VERSION_1_0;
		c.commit_v1.input_layers = layers;
		c.commit_v1.input_layer_cnt = 2;
		c.commit_v1.release_fence = -1;
		c.commit_v1.retire_fence = -1;
		if (ioctl(fb, MSMFB_ATOMIC_COMMIT, &c) < 0) {
			if (!fails++)
				fprintf(stderr, "MSMFB_ATOMIC_COMMIT: %s (layer errors %d %d)\n", strerror(errno),
				        layers[0].error_code, layers[1].error_code);
			if (fails > 3)
				break;
			usleep(20000);
			continue;
		}
		// The fences of a commit only signal once the next commit is on screen
		// (video mode panel): pace on the vsync event instead.
		if (c.commit_v1.retire_fence >= 0)
			close(c.commit_v1.retire_fence);
		if (c.commit_v1.release_fence >= 0)
			close(c.commit_v1.release_fence);
		char vbuf[64];
		struct pollfd p = {vs, POLLPRI | POLLERR, 0};
		if (poll(&p, 1, 100) > 0) {
			lseek(vs, 0, SEEK_SET);
			int n = read(vs, vbuf, sizeof(vbuf) - 1);
			vbuf[n > 0 ? n : 0] = 0;
			if (frames < 3)
				printf("vsync %s", vbuf);
		} else if (frames < 3) {
			printf("vsync timeout\n");
		}
		double t = now_ms();
		if (frames && t - last > max_dt)
			max_dt = t - last;
		last = t;
		frames++;
	}
	double el = (now_ms() - t0) / 1000;
	printf("commits ok %d in %.2f s (%.1f Hz), worst interval %.1f ms, failures %d\n", frames, el, frames / el,
	       max_dt, fails);

	vsync_on = 0;
	ioctl(fb, MSMFB_OVERLAY_VSYNC_CTRL, &vsync_on);
	// Panels off again (OLED burn-in).
	ioctl(fb, FBIOBLANK, FB_BLANK_POWERDOWN);
	close(fb);
	return frames > 0 ? 0 : 1;
}
