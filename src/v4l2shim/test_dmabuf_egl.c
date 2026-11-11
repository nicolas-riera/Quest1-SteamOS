// test_dmabuf_egl: the stream client's dma-buf import through libquest1_angle_egl.so, as the client does it.
//
// An ION buffer laid out like a venus NV12 frame (rows of the width rounded up to 128, the UV plane after the Y rows
// rounded up to 32) gets a pattern; each plane becomes an EGL image (EGL_LINUX_DMA_BUF_EXT, DRM_FORMAT_R8 then
// DRM_FORMAT_GR88) given to an OpenGL texture with glEGLImageTargetTexture2DOES; both textures are read back and
// compared with the pattern, then the buffer is changed and read again (the texture is the buffer, not a copy).
//
// Build (on the headset): gcc -O2 -Wall -o test_dmabuf_egl test_dmabuf_egl.c -ldl
// Run as the Steam user with Steam's environment (LD_PRELOAD, LD_LIBRARY_PATH, QUEST1_VK*, DISPLAY):
//   QUEST1_ANGLE_EGL=~/.local/share/Steam/steamrtarm64/libEGL.so ANGLE_DEFAULT_PLATFORM=vulkan
//   ANGLE_FEATURE_OVERRIDES_DISABLED=supportsExtendedDynamicState ./test_dmabuf_egl [WxH ...]
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define SHIM "/usr/local/lib/libquest1_angle_egl.so"
#define AL(x, a) (((x) + (a)-1) / (a) * (a))

struct ion_allocation_data {
	size_t len, align;
	unsigned int heap_id_mask, flags;
	int handle;
};
struct ion_fd_data {
	int handle, fd;
};
#define ION_IOC_ALLOC _IOWR('I', 0, struct ion_allocation_data)
#define ION_IOC_FREE _IOWR('I', 1, int)
#define ION_IOC_SHARE _IOWR('I', 4, struct ion_fd_data)
#define ION_SYSTEM_HEAP_ID 25

static int ion_buffer(size_t len)
{
	int ion = open("/dev/ion", O_RDONLY | O_CLOEXEC);
	// uncached: what the CPU writes here must be in memory for the GPU, and this kernel has no dma-buf sync
	struct ion_allocation_data a = {.len = len, .align = 4096, .heap_id_mask = 1u << ION_SYSTEM_HEAP_ID};
	if (ion < 0 || ioctl(ion, ION_IOC_ALLOC, &a) < 0)
		return -1;
	struct ion_fd_data f = {.handle = a.handle};
	int r = ioctl(ion, ION_IOC_SHARE, &f);
	ioctl(ion, ION_IOC_FREE, &a.handle);
	close(ion);
	return r < 0 ? -1 : f.fd;
}

static uint8_t pat(int plane, int c, unsigned x, unsigned y, int gen)
{
	return plane == 0 ? (uint8_t)(x + 3 * y + gen * 17) : c == 0 ? (uint8_t)(5 * x + y + gen * 29) : (uint8_t)(x + 7 * y + gen * 41);
}

typedef void *(*gpa_t)(const char *);
static gpa_t gpa;
#define F(ret, name, ...) ret (*name)(__VA_ARGS__) = (ret(*)(__VA_ARGS__))gpa(#name)

static void fill(uint8_t *m, unsigned w, unsigned h, unsigned stride, unsigned scan, int gen)
{
	for (unsigned y = 0; y < h; y++)
		for (unsigned x = 0; x < w; x++)
			m[(size_t)y * stride + x] = pat(0, 0, x, y, gen);
	for (unsigned y = 0; y < h / 2; y++)
		for (unsigned x = 0; x < w / 2; x++) {
			m[(size_t)stride * scan + (size_t)y * stride + 2 * x] = pat(1, 0, x, y, gen);
			m[(size_t)stride * scan + (size_t)y * stride + 2 * x + 1] = pat(1, 1, x, y, gen);
		}
}

// reads texture tex (w x h) back and counts the pixels that are not the pattern
static long check(unsigned tex, int plane, unsigned w, unsigned h, int gen)
{
	F(void, glGenFramebuffers, int, unsigned *);
	F(void, glBindFramebuffer, unsigned, unsigned);
	F(void, glFramebufferTexture2D, unsigned, unsigned, unsigned, unsigned, int);
	F(unsigned, glCheckFramebufferStatus, unsigned);
	F(void, glReadPixels, int, int, int, int, unsigned, unsigned, void *);
	F(void, glDeleteFramebuffers, int, const unsigned *);
	unsigned fb;
	glGenFramebuffers(1, &fb);
	glBindFramebuffer(0x8D40, fb);
	glFramebufferTexture2D(0x8D40, 0x8CE0, 0x0DE1, tex, 0);
	unsigned st = glCheckFramebufferStatus(0x8D40);
	if (st != 0x8CD5) {
		printf("  plane %d: framebuffer status %#x\n", plane, st);
		return -1;
	}
	uint8_t *px = malloc((size_t)w * h * 4);
	glReadPixels(0, 0, w, h, 0x1908, 0x1401, px);
	long bad = 0;
	for (unsigned y = 0; y < h; y++)
		for (unsigned x = 0; x < w; x++) {
			const uint8_t *p = px + ((size_t)y * w + x) * 4;
			int ok = p[0] == pat(plane, 0, x, y, gen) && (plane == 0 || p[1] == pat(plane, 1, x, y, gen));
			if (!ok && bad++ < 3)
				printf("  plane %d gen %d (%u,%u): %u %u, expected %u %u\n", plane, gen, x, y, p[0], p[1],
				       pat(plane, 0, x, y, gen), plane ? pat(plane, 1, x, y, gen) : 0);
		}
	free(px);
	glBindFramebuffer(0x8D40, 0);
	glDeleteFramebuffers(1, &fb);
	return bad;
}

int main(int argc, char **argv)
{
	void *egl = dlopen(getenv("SHIM") ? getenv("SHIM") : SHIM, RTLD_NOW);
	if (!egl) {
		fprintf(stderr, "dlopen: %s\n", dlerror());
		return 1;
	}
	gpa = (gpa_t)dlsym(egl, "eglGetProcAddress");
	F(void *, eglGetDisplay, void *);
	F(int, eglInitialize, void *, int *, int *);
	F(const char *, eglQueryString, void *, int);
	F(int, eglChooseConfig, void *, const int *, void **, int, int *);
	F(void *, eglCreateContext, void *, void *, void *, const int *);
	F(int, eglMakeCurrent, void *, void *, void *, void *);
	F(void *, eglCreateImageKHR, void *, void *, unsigned, void *, const int *);
	F(int, eglDestroyImageKHR, void *, void *);
	F(void, glEGLImageTargetTexture2DOES, unsigned, void *);
	F(void, glGenTextures, int, unsigned *);
	F(void, glBindTexture, unsigned, unsigned);
	F(void, glDeleteTextures, int, const unsigned *);
	F(void, glFinish, void);
	F(unsigned, glGetError, void);
	if (!eglGetDisplay || !eglCreateImageKHR || !glEGLImageTargetTexture2DOES) {
		fprintf(stderr, "missing functions\n");
		return 1;
	}
	void *d = eglGetDisplay(NULL);
	int maj, min;
	if (!d || !eglInitialize(d, &maj, &min)) {
		fprintf(stderr, "no EGL display\n");
		return 1;
	}
	int has = strstr(eglQueryString(d, 0x3055), "EGL_EXT_image_dma_buf_import") != NULL;
	printf("EGL_EXT_image_dma_buf_import: %s\n", has ? "yes" : "NO");
	int ca[] = {0x3040, 0x40 /* ES3 */, 0x3033, 0x1 /* pbuffer */, 0x3038};
	void *cfg;
	int n = 0;
	eglChooseConfig(d, ca, &cfg, 1, &n);
	int cx[] = {0x3098, 3, 0x3038};
	void *c = n ? eglCreateContext(d, cfg, NULL, cx) : NULL;
	if (!c || !eglMakeCurrent(d, NULL, NULL, c)) {
		fprintf(stderr, "no context\n");
		return 1;
	}

	int fails = !has;
	const char *def[] = {"1920x1080", "1280x720", "3840x2160", "270x1080"};
	int nsz = argc > 1 ? argc - 1 : 4;
	for (int s = 0; s < nsz; s++) {
		unsigned w, h;
		if (sscanf(argc > 1 ? argv[s + 1] : def[s], "%ux%u", &w, &h) != 2)
			continue;
		unsigned stride = AL(w, 128), scan = AL(h, 32);
		size_t len = AL((size_t)stride * scan * 3 / 2, 4096);
		int fd = ion_buffer(len);
		uint8_t *m = fd < 0 ? MAP_FAILED : mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (m == MAP_FAILED) {
			printf("%ux%u: no ION buffer\n", w, h);
			fails++;
			continue;
		}
		fill(m, w, h, stride, scan, 0);
		unsigned tex[2];
		void *img[2] = {0};
		glGenTextures(2, tex);
		for (int p = 0; p < 2; p++) {
			int a[] = {0x3057, (int)(p ? w / 2 : w), 0x3056, (int)(p ? h / 2 : h), 0x3271,
			           p ? 0x38385247 : 0x20203852, 0x3272, fd, 0x3273, p ? (int)(stride * scan) : 0, 0x3274,
			           (int)stride, 0x3038};
			img[p] = eglCreateImageKHR(d, NULL, 0x3270, NULL, a);
			glBindTexture(0x0DE1, tex[p]);
			if (img[p])
				glEGLImageTargetTexture2DOES(0x0DE1, img[p]);
		}
		// a stride beyond the width must be refused (the client then copies by CPU)
		int expect = stride == w;
		if (!img[0] != !img[1] || !!img[0] != expect) {
			printf("%ux%u (stride %u): images %p %p, %s expected\n", w, h, stride, img[0], img[1],
			       expect ? "two" : "none");
			fails++;
		} else if (!img[0]) {
			printf("%ux%u (stride %u): refused, as it must be\n", w, h, stride);
		} else {
			long bad = 0;
			for (int gen = 0; gen < 2; gen++) {
				if (gen) {
					glFinish();
					fill(m, w, h, stride, scan, gen);
				}
				bad += check(tex[0], 0, w, h, gen) != 0;
				bad += check(tex[1], 1, w / 2, h / 2, gen) != 0;
			}
			unsigned e = glGetError();
			printf("%ux%u: %s (GL error %#x)\n", w, h, bad ? "DIFFERENT" : "both planes exact, and after a change", e);
			fails += bad || e;
		}
		for (int p = 0; p < 2; p++)
			if (img[p])
				eglDestroyImageKHR(d, img[p]);
		glDeleteTextures(2, tex);
		munmap(m, len);
		close(fd);
	}
	printf("%s\n", fails ? "FAIL" : "PASS");
	return fails != 0;
}
