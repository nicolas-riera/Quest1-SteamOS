// Display smoke test for the Quest 1 MDSS framebuffer (fbdev, 2x 1440x1600).
// Left half: red background with one white block top-left; right half: green
// background with two blocks top-right. A white bar sweeps down so a live refresh
// is visible. Runs ~30 s, then leaves the last frame up.
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static void fill(uint32_t *p, int stride, int x0, int y0, int w, int h, uint32_t c)
{
	for (int y = y0; y < y0 + h; y++)
		for (int x = x0; x < x0 + w; x++)
			p[y * stride + x] = c;
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/fb0";
	int fd = -1;
	for (int i = 0; i < 50 && fd < 0; i++) {
		fd = open(dev, O_RDWR);
		if (fd < 0)
			usleep(100000);
	}
	if (fd < 0) {
		perror("open fb");
		return 1;
	}
	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo f;
	if (ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f)) {
		perror("FBIOGET");
		return 1;
	}
	printf("fb %s: %ux%u virt %ux%u bpp %u stride %u smem %u\n", f.id, v.xres, v.yres, v.xres_virtual,
	       v.yres_virtual, v.bits_per_pixel, f.line_length, f.smem_len);
	printf("red %u/%u green %u/%u blue %u/%u alpha %u/%u\n", v.red.offset, v.red.length, v.green.offset,
	       v.green.length, v.blue.offset, v.blue.length, v.transp.offset, v.transp.length);
	if (ioctl(fd, FBIOBLANK, FB_BLANK_UNBLANK))
		perror("FBIOBLANK unblank");

	uint8_t *mem = mmap(NULL, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mem == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	int stride = f.line_length / 4, W = v.xres, H = v.yres;
	int nbuf = v.yres_virtual >= 2 * v.yres ? 2 : 1;
#define RGB(r, g, b) (((uint32_t)(r) << v.red.offset) | ((uint32_t)(g) << v.green.offset) | \
		      ((uint32_t)(b) << v.blue.offset) | (v.transp.length ? 0xffu << v.transp.offset : 0))
	uint32_t red = RGB(160, 0, 0), green = RGB(0, 140, 0), white = RGB(255, 255, 255);

	struct timespec t0, t;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int frame = 0;; frame++) {
		int b = frame % nbuf;
		uint32_t *p = (uint32_t *)(mem + (size_t)b * v.yres * f.line_length);
		fill(p, stride, 0, 0, W / 2, H, red);
		fill(p, stride, W / 2, 0, W / 2, H, green);
		fill(p, stride, 200, 200, 200, 200, white);                      // left: one block, top-left
		fill(p, stride, W - 400, 200, 200, 200, white);                  // right: two blocks, top-right
		fill(p, stride, W - 400, 500, 200, 200, white);
		fill(p, stride, 0, (frame * 8) % (H - 40), W, 40, white);        // sweeping bar
		v.yoffset = b * v.yres;
		v.activate = FB_ACTIVATE_VBL;
		if (ioctl(fd, FBIOPAN_DISPLAY, &v) && frame == 0)
			perror("FBIOPAN_DISPLAY");
		clock_gettime(CLOCK_MONOTONIC, &t);
		if (frame % 72 == 0)
			printf("frame %d t=%.2f\n", frame, (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9);
		fflush(stdout);
		if (t.tv_sec - t0.tv_sec > 30)
			break;
	}
	printf("done\n");
	pause();
	return 0;
}
