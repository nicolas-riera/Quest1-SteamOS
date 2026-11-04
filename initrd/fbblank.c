// Switch the Quest 1 panels off when nothing else drives the display (the installer's micro Linux).
// At boot the panels still show the bootloader's last frame (Meta's logo, continuous splash): the MDSS
// driver only takes them over when a client opens the framebuffer, unblanks it and commits a frame, and
// its memory only exists once mapped (write() fails with ENODEV before). So: open, unblank (take over),
// map, black frame in every buffer, commit, then power the panels down. Exit status 0 when all went through.
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/fb0";
	int fd = -1;
	for (int i = 0; i < 100 && fd < 0; i++) {  // the device appears a few seconds into the boot
		fd = open(dev, O_RDWR);
		if (fd < 0)
			usleep(100000);
	}
	if (fd < 0) {
		perror("fbblank: open");
		return 1;
	}
	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo f;
	if (ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f)) {
		perror("fbblank: FBIOGET");
		return 1;
	}
	int rc = 0;
	if (ioctl(fd, FBIOBLANK, FB_BLANK_UNBLANK)) {
		perror("fbblank: unblank");
		rc = 1;
	}
	void *mem = mmap(NULL, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mem == MAP_FAILED) {
		perror("fbblank: mmap");
		rc = 1;
	} else {
		memset(mem, 0, f.smem_len);
		v.yoffset = 0;
		v.activate = FB_ACTIVATE_VBL;
		if (ioctl(fd, FBIOPAN_DISPLAY, &v)) {
			perror("fbblank: pan");
			rc = 1;
		}
		usleep(100000);  // a few refreshes with the black frame before the panels go off
	}
	if (ioctl(fd, FBIOBLANK, FB_BLANK_POWERDOWN)) {
		perror("fbblank: powerdown");
		rc = 1;
	}
	printf("fbblank: %s %ux%u, %s\n", f.id, v.xres, v.yres, rc ? "errors" : "panels off");
	return rc;
}
