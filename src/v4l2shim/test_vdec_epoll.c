// test_vdec_epoll: Steam Link VR's (vrlink's) order of calls on the decoder, through libquest1_v4l2.so: low latency
// asked for (DISPLAY_DELAY 0), OUTPUT set up with a bound for the size (1536x6144), STREAMON OUTPUT, packets, an
// epoll wait for SOURCE_CHANGE, and only then CAPTURE, with dma-bufs of the client's own (ION here; vrlink's are
// Vulkan memory it exports). Paced like a live stream; reports the frames, when the first came, and the latency.
//
//   LD_PRELOAD=libquest1_v4l2.so test_vdec_epoll stream.265 [fps]
// HEVC Annex-B with no B-frames (ffmpeg -f lavfi -i testsrc2=size=1024x4096:rate=72 -t 1 -c:v hevc_nvenc -bf 0
// -tune ull -f hevc). Only ioctls on the decoder and ION allocations.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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

#define NOUT 16
#define NCAP 16
#define DATA 0x43

static int fd;

static double now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static int x(unsigned long req, void *arg, const char *what)
{
	int r = ioctl(fd, req, arg);
	if (r < 0)
		fprintf(stderr, "test_vdec_epoll: %s: %s\n", what, strerror(errno));
	return r;
}

// the start of each access unit: a VPS/SPS/PPS/AUD/prefix SEI after slices, or a picture's first slice
static size_t split(const uint8_t *p, size_t n, size_t *start, size_t max)
{
	size_t cnt = 0;
	int seen_vcl = 0;
	for (size_t i = 0; i + 5 < n && cnt < max; i++) {
		if (!(p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1))
			continue;
		size_t s = i > 0 && p[i - 1] == 0 ? i - 1 : i;
		int t = (p[i + 3] >> 1) & 0x3f, vcl = t < 32, first = vcl && (p[i + 5] & 0x80);
		int boundary = !vcl && (t == 32 || t == 33 || t == 34 || t == 35 || t == 39);
		if (!cnt || (seen_vcl && (first || boundary))) {
			start[cnt++] = s;
			seen_vcl = 0;
		}
		seen_vcl |= vcl;
		i += 3;
	}
	return cnt;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: test_vdec_epoll stream.265 [fps]\n");
		return 2;
	}
	double fps = argc > 2 ? atof(argv[2]) : 72;
	int sf = open(argv[1], O_RDONLY);
	struct stat st;
	if (sf < 0 || fstat(sf, &st) < 0)
		return 1;
	uint8_t *s = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, sf, 0);
	static size_t au[4096];
	size_t naus = split(s, st.st_size, au, 4095);
	au[naus] = st.st_size;

	fd = open("/dev/video32", O_RDWR | O_NONBLOCK | O_CLOEXEC);
	int ion = open("/dev/ion", O_RDONLY | O_CLOEXEC);
	if (fd < 0 || ion < 0)
		return 1;
	struct v4l2_control c = {V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE, 1};
	x(VIDIOC_S_CTRL, &c, "DISPLAY_DELAY_ENABLE");
	c = (struct v4l2_control){V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY, 0};
	x(VIDIOC_S_CTRL, &c, "DISPLAY_DELAY");
	struct v4l2_event_subscription sub = {.type = V4L2_EVENT_SOURCE_CHANGE};
	if (x(VIDIOC_SUBSCRIBE_EVENT, &sub, "SUBSCRIBE SOURCE_CHANGE") < 0)
		return 1;
	struct v4l2_format f = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE};
	f.fmt.pix_mp.width = 1536;
	f.fmt.pix_mp.height = 6144;
	f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_HEVC;
	f.fmt.pix_mp.num_planes = 1;
	if (x(VIDIOC_S_FMT, &f, "S_FMT OUTPUT") < 0)
		return 1;
	f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	if (x(VIDIOC_S_FMT, &f, "S_FMT CAPTURE") < 0)
		return 1;
	struct v4l2_requestbuffers rb = {.count = NOUT, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_MMAP};
	if (x(VIDIOC_REQBUFS, &rb, "REQBUFS OUTPUT") < 0)
		return 1;
	uint8_t *in[NOUT];
	uint32_t inlen[NOUT];
	int infree[NOUT];
	for (unsigned i = 0; i < rb.count; i++) {
		struct v4l2_plane p = {0};
		struct v4l2_buffer b = {.index = i, .type = rb.type, .memory = V4L2_MEMORY_MMAP, .m.planes = &p, .length = 1};
		if (x(VIDIOC_QUERYBUF, &b, "QUERYBUF OUTPUT") < 0)
			return 1;
		in[i] = mmap(NULL, p.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, p.m.mem_offset);
		inlen[i] = p.length;
		infree[i] = in[i] != MAP_FAILED;
	}
	int t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	if (x(VIDIOC_STREAMON, &t, "STREAMON OUTPUT") < 0)
		return 1;
	int ep = epoll_create1(EPOLL_CLOEXEC);
	struct epoll_event ee = {.events = EPOLLIN | EPOLLPRI | EPOLLOUT, .data.u64 = DATA};
	if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ee) < 0)
		return 1;

	double t0 = now_ms(), first = -1, lat = 0, latmax = 0, sent[4096];
	size_t next = 0, frames = 0, live = 0, events = 0, wakes = 0;
	int capfd[NCAP], ncap = 0;
	while (frames < naus && now_ms() - t0 < 10000) {
		while (next < naus && now_ms() >= t0 + next * 1000.0 / fps) {
			unsigned i = 0;
			while (i < rb.count && !infree[i])
				i++;
			if (i == rb.count)
				break;
			uint32_t len = (uint32_t)(au[next + 1] - au[next]);
			if (len > inlen[i])
				return 1;
			memcpy(in[i], s + au[next], len);
			struct v4l2_plane p = {.bytesused = len, .length = inlen[i]};
			struct v4l2_buffer b = {.index = i, .type = rb.type, .memory = V4L2_MEMORY_MMAP, .m.planes = &p, .length = 1};
			b.timestamp.tv_usec = (long)next;
			if (x(VIDIOC_QBUF, &b, "QBUF OUTPUT") < 0)
				return 1;
			infree[i] = 0;
			sent[next++] = now_ms();
		}
		struct epoll_event got[4];
		int n = epoll_wait(ep, got, 4, 2);
		for (int k = 0; k < n; k++) {
			wakes++;
			if (got[k].data.u64 != DATA) {
				fprintf(stderr, "test_vdec_epoll: an event with data 0x%llx\n", (unsigned long long)got[k].data.u64);
				return 1;
			}
			struct v4l2_event ev;
			while ((got[k].events & EPOLLPRI) && ioctl(fd, VIDIOC_DQEVENT, &ev) == 0) {
				events++;
				if (ev.type != V4L2_EVENT_SOURCE_CHANGE)
					continue;
				// vrlink's CaptureSetup
				struct v4l2_format g = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
				if (x(VIDIOC_G_FMT, &g, "G_FMT CAPTURE") < 0)
					return 1;
				printf("SOURCE_CHANGE after %.1f ms: %ux%u, stride %u, %u bytes\n", now_ms() - t0, g.fmt.pix_mp.width,
				       g.fmt.pix_mp.height, g.fmt.pix_mp.plane_fmt[0].bytesperline, g.fmt.pix_mp.plane_fmt[0].sizeimage);
				int ct = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
				x(VIDIOC_STREAMOFF, &ct, "STREAMOFF CAPTURE");
				struct v4l2_requestbuffers cr = {.count = 0, .type = ct, .memory = V4L2_MEMORY_DMABUF};
				x(VIDIOC_REQBUFS, &cr, "REQBUFS CAPTURE 0");
				if (x(VIDIOC_S_FMT, &g, "S_FMT CAPTURE") < 0)
					return 1;
				cr.count = NCAP;
				if (x(VIDIOC_REQBUFS, &cr, "REQBUFS CAPTURE") < 0)
					return 1;
				ncap = (int)cr.count;
				for (int i = 0; i < ncap; i++) {
					struct ion_allocation_data a = {.len = (g.fmt.pix_mp.plane_fmt[0].sizeimage + 4095) & ~4095u,
					                                .align = 4096, .heap_id_mask = 1u << 25};
					struct ion_fd_data fdd;
					if (ioctl(ion, ION_IOC_ALLOC, &a) < 0)
						return 1;
					fdd.handle = a.handle;
					ioctl(ion, ION_IOC_SHARE, &fdd);
					ioctl(ion, ION_IOC_FREE, &a.handle);
					capfd[i] = fdd.fd;
					struct v4l2_plane p = {.m.fd = capfd[i], .length = (uint32_t)a.len};
					struct v4l2_buffer b = {.index = (uint32_t)i, .type = ct, .memory = V4L2_MEMORY_DMABUF, .m.planes = &p,
					                        .length = 1};
					if (x(VIDIOC_QBUF, &b, "QBUF CAPTURE") < 0)
						return 1;
				}
				if (x(VIDIOC_STREAMON, &ct, "STREAMON CAPTURE") < 0)
					return 1;
			}
			struct v4l2_plane p;
			struct v4l2_buffer b;
			for (;;) {
				memset(&p, 0, sizeof(p));
				b = (struct v4l2_buffer){.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_MMAP,
				                         .m.planes = &p, .length = 1};
				if (!(got[k].events & EPOLLOUT) || ioctl(fd, VIDIOC_DQBUF, &b) < 0)
					break;
				infree[b.index] = 1;
			}
			for (;;) {
				memset(&p, 0, sizeof(p));
				b = (struct v4l2_buffer){.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_DMABUF,
				                         .m.planes = &p, .length = 1};
				if (!ncap || !(got[k].events & EPOLLIN) || ioctl(fd, VIDIOC_DQBUF, &b) < 0)
					break;
				if (p.bytesused) {
					size_t id = (size_t)b.timestamp.tv_usec;
					double l = id < next ? now_ms() - sent[id] : 0;
					if (first < 0)
						first = now_ms() - t0;
					else if (sent[id] - t0 > first) { // sent once the stream ran: the backlog of the start left out
						lat += l;
						latmax = l > latmax ? l : latmax;
						live++;
					}
					frames++;
				}
				p.m.fd = capfd[b.index];
				x(VIDIOC_QBUF, &b, "QBUF CAPTURE (again)");
			}
		}
	}
	printf("%zu of %zu frames in %.0f ms; first after %.1f ms; then %zu frames %.1f ms from packet to frame (max %.1f);"
	       " %zu events, %zu wakes\n",
	       frames, naus, now_ms() - t0, first, live, live ? lat / live : 0, latmax, events, wakes);
	close(fd);
	return frames == naus ? 0 : 1;
}
