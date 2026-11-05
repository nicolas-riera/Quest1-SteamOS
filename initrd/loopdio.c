// loopdio /dev/loopN: direct I/O for a loop device holding an ext4 image (dual boot: holo.img on userdata).
// Buffered, every block read through the loop device is cached twice: in the image's page cache on userdata
// and in the file system's own above it (~335 MB of holo.img after a boot, and loop0 copying pages back and
// forth when memory is short). Direct I/O needs the loop device's logical block size to be at least the
// backing device's (4096 on the Quest's UFS), so it takes the ext4 block size first (LOOP_SET_BLOCK_SIZE,
// backported in Meta's 4.4), then LOOP_SET_DIRECT_IO. Run before mounting. On failure the device stays
// buffered: still usable.
#include <fcntl.h>
#include <linux/loop.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef LOOP_SET_DIRECT_IO
#define LOOP_SET_DIRECT_IO 0x4C08
#endif
#ifndef LOOP_SET_BLOCK_SIZE
#define LOOP_SET_BLOCK_SIZE 0x4C09
#endif

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: loopdio /dev/loopN\n");
		return 2;
	}
	int fd = open(argv[1], O_RDWR);
	if (fd < 0) {
		perror(argv[1]);
		return 1;
	}
	// ext4 superblock at 1024: s_log_block_size at +24, s_magic at +56
	uint8_t sb[1024];
	if (pread(fd, sb, sizeof(sb), 1024) != sizeof(sb) || sb[56] != 0x53 || sb[57] != 0xef) {
		fprintf(stderr, "loopdio: %s: no ext4 file system\n", argv[1]);
		return 1;
	}
	uint32_t log;
	memcpy(&log, sb + 24, 4);
	unsigned long bs = 1024ul << log, page = sysconf(_SC_PAGESIZE);
	if (bs > page)
		bs = page;
	if (ioctl(fd, LOOP_SET_BLOCK_SIZE, bs) < 0) {
		perror("loopdio: LOOP_SET_BLOCK_SIZE");
		return 1;
	}
	if (ioctl(fd, LOOP_SET_DIRECT_IO, 1ul) < 0) {
		perror("loopdio: LOOP_SET_DIRECT_IO");
		return 1;
	}
	printf("loopdio: %s: %lu-byte blocks, direct I/O\n", argv[1], bs);
	return 0;
}
