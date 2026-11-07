// vdectest: decode an H.264 or HEVC Annex-B stream on the Quest 1's venus decoder (downstream msm_vidc, /dev/video32)
// through V4L2, write the decoded NV12 frames (cropped) and report speed and latency.
//
//   vdectest [-d /dev/video32] [-c h264|hevc] -s WxH [-f nv12|ubwc] [-m primary|split] [-O display|decode] [-L]
//            [-n frames] [-o out.nv12] stream.264
//
// What the downstream driver wants (drivers/media/platform/msm/vidc, read 2026-11-07), unlike the mainline
// stateful decoder API:
//  - USERPTR buffers only, backed by ION: plane.m.userptr = our mapping, plane.reserved[0] = the ION fd,
//    plane.reserved[1] = the offset in it (msm_smem imports the fd with ion_import_dma_buf: ION buffers only).
//  - One whole frame (access unit) per OUTPUT buffer by default (frame assembly mode).
//  - CAPTURE layout from G_FMT: bytesperline = Y stride, plane_fmt[0].reserved[0] = Y scanlines, UV at
//    stride * scanlines. A second CAPTURE plane, when there is one, is extradata.
//  - Private events (V4L2_EVENT_MSM_VIDC_*): port settings changed (sufficient or not: u.data u32 [0] height,
//    [1] width), flush done, system error. End of stream is the buffer flag V4L2_QCOM_BUF_FLAG_EOS.
//  - Linear NV12 out of a UBWC DPB is "split" mode: STREAM_OUTPUT_MODE secondary + DPB_COLOR_FORMAT UBWC
//    (what the driver itself switches to while sizing its scratch buffers).
// Only ioctls on the decoder node and ION allocations: nothing is written anywhere but the -o file.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// --- msm_vidc's private V4L2 additions (values from the kernel's include/uapi/linux/{videodev2,v4l2-controls}.h)
#define FMT_NV12_UBWC v4l2_fourcc('Q', '1', '2', '8')
#define VIDC_CID(n) ((V4L2_CTRL_CLASS_MPEG | 0x2000) + (n))
#define CID_OUTPUT_ORDER VIDC_CID(7)       // 0 display, 1 decode
#define CID_STREAM_OUTPUT_MODE VIDC_CID(38) // 0 primary, 1 secondary
#define CID_DPB_COLOR_FORMAT VIDC_CID(73)   // 0 none, 1 UBWC
#define CID_LOWLATENCY_MODE VIDC_CID(88)    // 0 off, 1 on
#define EV_VIDC(n) (V4L2_EVENT_PRIVATE_START + 0x1000 + (n))
#define EV_FLUSH_DONE EV_VIDC(1)
#define EV_SETTINGS_SUFFICIENT EV_VIDC(2)
#define EV_SETTINGS_INSUFFICIENT EV_VIDC(3)
#define EV_BITDEPTH_INSUFFICIENT EV_VIDC(4)
#define EV_SYS_ERROR EV_VIDC(5)
#define EV_RELEASE_BUFFER_REF EV_VIDC(6)
#define EV_HW_OVERLOAD EV_VIDC(8)
#define EV_HW_UNSUPPORTED EV_VIDC(10)
#define QCOM_BUF_FLAG_EOS 0x02000000

// --- legacy ION (kernel 4.4 staging: handles, then a dma-buf fd)
struct ion_allocation_data {
	size_t len;
	size_t align;
	unsigned int heap_id_mask;
	unsigned int flags;
	int handle;
};
struct ion_fd_data {
	int handle;
	int fd;
};
struct ion_handle_data {
	int handle;
};
#define ION_IOC_ALLOC _IOWR('I', 0, struct ion_allocation_data)
#define ION_IOC_FREE _IOWR('I', 1, struct ion_handle_data)
#define ION_IOC_SHARE _IOWR('I', 4, struct ion_fd_data)
#define ION_SYSTEM_HEAP_ID 25 // ION_IOMMU_HEAP_ID on this kernel: what msm_vidc itself allocates from

struct buf {
	int fd; // ION dma-buf fd
	void *map;
	size_t len;
};

static int dev = -1, ion = -1;
static uint32_t cap_planes, out_planes;
static uint32_t stride, scanlines;
static uint32_t width, height;
static struct buf obuf[32], cbuf[32][2];
static uint32_t nobuf, ncbuf;

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int xioctl(unsigned long req, void *arg, const char *what)
{
	int r;
	do
		r = ioctl(dev, req, arg);
	while (r < 0 && errno == EINTR);
	if (r < 0 && what)
		fprintf(stderr, "vdectest: %s: %s\n", what, strerror(errno));
	return r;
}

static int ion_buf(size_t len, struct buf *b)
{
	len = (len + 4095) & ~(size_t)4095;
	struct ion_allocation_data a = {.len = len, .align = 4096, .heap_id_mask = 1u << ION_SYSTEM_HEAP_ID};
	if (ioctl(ion, ION_IOC_ALLOC, &a) < 0) {
		fprintf(stderr, "vdectest: ION alloc %zu: %s\n", len, strerror(errno));
		return -1;
	}
	struct ion_fd_data f = {.handle = a.handle};
	int r = ioctl(ion, ION_IOC_SHARE, &f);
	struct ion_handle_data h = {.handle = a.handle};
	ioctl(ion, ION_IOC_FREE, &h); // the fd keeps the buffer
	if (r < 0) {
		fprintf(stderr, "vdectest: ION share: %s\n", strerror(errno));
		return -1;
	}
	b->fd = f.fd;
	b->len = len;
	b->map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, f.fd, 0);
	if (b->map == MAP_FAILED) {
		fprintf(stderr, "vdectest: ION mmap: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

static void ion_free(struct buf *b)
{
	if (b->map && b->map != MAP_FAILED)
		munmap(b->map, b->len);
	if (b->fd >= 0)
		close(b->fd);
	memset(b, 0, sizeof(*b));
	b->fd = -1;
}

// --- access units of an Annex-B stream
struct au {
	size_t off, len;
};

static int nal_start(const uint8_t *p, size_t n, size_t i, size_t *hdr)
{
	if (i + 3 <= n && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
		*hdr = 3;
		return 1;
	}
	if (i + 4 <= n && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 1) {
		*hdr = 4;
		return 1;
	}
	return 0;
}

// A new access unit starts at a NAL that is a parameter set / SEI / AUD after a picture's slices, or at the first
// slice of a picture (first_mb_in_slice == 0, first_slice_segment_in_pic_flag).
static size_t split_aus(const uint8_t *p, size_t n, int hevc, struct au **out)
{
	size_t cap = 256, cnt = 0, h = 0, cur = (size_t)-1;
	int seen_vcl = 0;
	struct au *a = malloc(cap * sizeof(*a));
	for (size_t i = 0; i < n; i++) {
		if (!nal_start(p, n, i, &h))
			continue;
		size_t q = i + h;
		if (q + 2 >= n)
			break;
		int vcl, first, boundary;
		if (hevc) {
			int t = (p[q] >> 1) & 0x3f;
			vcl = t < 32;
			first = vcl && (p[q + 2] & 0x80);
			boundary = !vcl && (t == 32 || t == 33 || t == 34 || t == 35 || t == 39);
		} else {
			int t = p[q] & 0x1f;
			vcl = t >= 1 && t <= 5;
			first = vcl && (p[q + 1] & 0x80);
			boundary = !vcl && (t == 6 || t == 7 || t == 8 || t == 9);
		}
		if (cur == (size_t)-1 || (seen_vcl && (first || boundary))) {
			if (cur != (size_t)-1) {
				if (cnt == cap)
					a = realloc(a, (cap *= 2) * sizeof(*a));
				a[cnt].off = cur;
				a[cnt].len = i - cur;
				cnt++;
			}
			cur = i;
			seen_vcl = 0;
		}
		seen_vcl |= vcl;
		i = q;
	}
	if (cur != (size_t)-1 && cur < n) {
		if (cnt == cap)
			a = realloc(a, (cap + 1) * sizeof(*a));
		a[cnt].off = cur;
		a[cnt].len = n - cur;
		cnt++;
	}
	*out = a;
	return cnt;
}

// --- capture side
static int cap_setup(uint32_t count)
{
	struct v4l2_format f = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
	if (xioctl(VIDIOC_G_FMT, &f, "G_FMT capture") < 0)
		return -1;
	cap_planes = f.fmt.pix_mp.num_planes;
	width = f.fmt.pix_mp.width;
	height = f.fmt.pix_mp.height;
	stride = f.fmt.pix_mp.plane_fmt[0].bytesperline;
	scanlines = f.fmt.pix_mp.plane_fmt[0].reserved[0];
	struct v4l2_requestbuffers rb = {.count = count, .type = f.type, .memory = V4L2_MEMORY_USERPTR};
	if (xioctl(VIDIOC_REQBUFS, &rb, "REQBUFS capture") < 0)
		return -1;
	ncbuf = rb.count < 32 ? rb.count : 32;
	fprintf(stderr, "vdectest: capture %ux%u, stride %u, scanlines %u, %u plane(s) (%u + %u bytes), %u buffers\n", width,
	        height, stride, scanlines, cap_planes, f.fmt.pix_mp.plane_fmt[0].sizeimage,
	        cap_planes > 1 ? f.fmt.pix_mp.plane_fmt[1].sizeimage : 0, ncbuf);
	for (uint32_t i = 0; i < ncbuf; i++) {
		struct v4l2_plane pl[2] = {{0}};
		for (uint32_t k = 0; k < cap_planes && k < 2; k++) {
			if (ion_buf(f.fmt.pix_mp.plane_fmt[k].sizeimage, &cbuf[i][k]) < 0)
				return -1;
			pl[k].m.userptr = (unsigned long)cbuf[i][k].map;
			pl[k].length = f.fmt.pix_mp.plane_fmt[k].sizeimage;
			pl[k].reserved[0] = (uint32_t)cbuf[i][k].fd;
			pl[k].reserved[1] = 0;
		}
		struct v4l2_buffer b = {.type = f.type, .memory = V4L2_MEMORY_USERPTR, .index = i, .m.planes = pl,
		                        .length = cap_planes};
		if (xioctl(VIDIOC_QBUF, &b, "QBUF capture") < 0)
			return -1;
	}
	int t = f.type;
	return xioctl(VIDIOC_STREAMON, &t, "STREAMON capture");
}

static void cap_teardown(void)
{
	int t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	xioctl(VIDIOC_STREAMOFF, &t, "STREAMOFF capture");
	struct v4l2_requestbuffers rb = {.count = 0, .type = t, .memory = V4L2_MEMORY_USERPTR};
	xioctl(VIDIOC_REQBUFS, &rb, NULL);
	for (uint32_t i = 0; i < ncbuf; i++)
		for (int k = 0; k < 2; k++)
			ion_free(&cbuf[i][k]);
	ncbuf = 0;
}

static int set_ctrl(uint32_t id, int32_t v, const char *what)
{
	struct v4l2_control c = {.id = id, .value = v};
	return xioctl(VIDIOC_S_CTRL, &c, what);
}

static void usage(void)
{
	fprintf(stderr, "usage: vdectest [-d dev] [-c h264|hevc] -s WxH [-f nv12|ubwc] [-m primary|split] "
	                "[-O display|decode] [-L] [-n frames] [-o out.nv12] stream\n");
	exit(2);
}

int main(int argc, char **argv)
{
	const char *devpath = "/dev/video32", *outpath = NULL, *mode = NULL;
	int hevc = 0, ubwc = 0, decode_order = 0, lowlat = 0, maxframes = 0, opt;
	uint32_t w = 0, h = 0;
	while ((opt = getopt(argc, argv, "d:c:s:f:m:O:Ln:o:")) != -1) {
		switch (opt) {
		case 'd': devpath = optarg; break;
		case 'c': hevc = !strcmp(optarg, "hevc"); break;
		case 's': if (sscanf(optarg, "%ux%u", &w, &h) != 2) usage(); break;
		case 'f': ubwc = !strcmp(optarg, "ubwc"); break;
		case 'm': mode = optarg; break;
		case 'O': decode_order = !strcmp(optarg, "decode"); break;
		case 'L': lowlat = 1; break;
		case 'n': maxframes = atoi(optarg); break;
		case 'o': outpath = optarg; break;
		default: usage();
		}
	}
	if (optind != argc - 1 || !w || !h)
		usage();
	if (ubwc && outpath)
		fprintf(stderr, "vdectest: UBWC frames are written as they are (tiled, compressed): no comparison possible\n");

	int sf = open(argv[optind], O_RDONLY);
	struct stat st;
	if (sf < 0 || fstat(sf, &st) < 0) {
		perror(argv[optind]);
		return 1;
	}
	uint8_t *stream = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, sf, 0);
	struct au *aus;
	size_t naus = split_aus(stream, st.st_size, hevc, &aus);
	if (maxframes > 0 && (size_t)maxframes < naus)
		naus = maxframes;
	size_t maxau = 0;
	for (size_t i = 0; i < naus; i++)
		maxau = aus[i].len > maxau ? aus[i].len : maxau;
	fprintf(stderr, "vdectest: %s, %zu access units (largest %zu bytes)\n", hevc ? "HEVC" : "H.264", naus, maxau);

	dev = open(devpath, O_RDWR | O_NONBLOCK);
	ion = open("/dev/ion", O_RDONLY);
	if (dev < 0 || ion < 0) {
		fprintf(stderr, "vdectest: open %s / /dev/ion: %s\n", devpath, strerror(errno));
		return 1;
	}
	struct v4l2_capability cap = {0};
	if (xioctl(VIDIOC_QUERYCAP, &cap, "QUERYCAP") < 0)
		return 1;
	fprintf(stderr, "vdectest: %s (%s), caps 0x%x\n", (char *)cap.card, (char *)cap.driver, cap.capabilities);

	static const uint32_t evs[] = {EV_FLUSH_DONE, EV_SETTINGS_SUFFICIENT, EV_SETTINGS_INSUFFICIENT,
	                               EV_BITDEPTH_INSUFFICIENT, EV_SYS_ERROR, EV_RELEASE_BUFFER_REF,
	                               EV_HW_OVERLOAD, EV_HW_UNSUPPORTED};
	for (size_t i = 0; i < sizeof(evs) / sizeof(evs[0]); i++) {
		struct v4l2_event_subscription s = {.type = evs[i]};
		xioctl(VIDIOC_SUBSCRIBE_EVENT, &s, "SUBSCRIBE_EVENT");
	}

	struct v4l2_format f = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE};
	f.fmt.pix_mp.width = w;
	f.fmt.pix_mp.height = h;
	f.fmt.pix_mp.pixelformat = hevc ? V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_H264;
	f.fmt.pix_mp.num_planes = 1;
	if (xioctl(VIDIOC_S_FMT, &f, "S_FMT output") < 0)
		return 1;
	struct v4l2_format fc = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
	fc.fmt.pix_mp.width = w;
	fc.fmt.pix_mp.height = h;
	fc.fmt.pix_mp.pixelformat = ubwc ? FMT_NV12_UBWC : V4L2_PIX_FMT_NV12;
	if (xioctl(VIDIOC_S_FMT, &fc, "S_FMT capture") < 0)
		return 1;
	if (mode && !strcmp(mode, "split")) {
		set_ctrl(CID_STREAM_OUTPUT_MODE, 1, "STREAM_OUTPUT_MODE secondary");
		set_ctrl(CID_DPB_COLOR_FORMAT, 1, "DPB_COLOR_FORMAT UBWC");
	} else if (mode && !strcmp(mode, "primary")) {
		set_ctrl(CID_STREAM_OUTPUT_MODE, 0, "STREAM_OUTPUT_MODE primary");
	}
	if (decode_order)
		set_ctrl(CID_OUTPUT_ORDER, 1, "OUTPUT_ORDER decode");
	if (lowlat)
		set_ctrl(CID_LOWLATENCY_MODE, 1, "LOWLATENCY_MODE");

	// input buffers
	f = (struct v4l2_format){.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE};
	if (xioctl(VIDIOC_G_FMT, &f, "G_FMT output") < 0)
		return 1;
	out_planes = f.fmt.pix_mp.num_planes;
	size_t insz = f.fmt.pix_mp.plane_fmt[0].sizeimage;
	if (insz < maxau)
		insz = maxau;
	struct v4l2_requestbuffers rb = {.count = 6, .type = f.type, .memory = V4L2_MEMORY_USERPTR};
	if (xioctl(VIDIOC_REQBUFS, &rb, "REQBUFS output") < 0)
		return 1;
	nobuf = rb.count < 32 ? rb.count : 32;
	fprintf(stderr, "vdectest: output %u buffers of %zu bytes, %u plane(s)\n", nobuf, insz, out_planes);
	for (uint32_t i = 0; i < nobuf; i++)
		if (ion_buf(insz, &obuf[i]) < 0)
			return 1;

	struct v4l2_control mc = {.id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE};
	uint32_t capcount = xioctl(VIDIOC_G_CTRL, &mc, NULL) == 0 && mc.value > 0 ? (uint32_t)mc.value + 2 : 8;
	if (cap_setup(capcount) < 0)
		return 1;

	FILE *out = outpath ? fopen(outpath, "wb") : NULL;
	double *qtime = calloc(naus + 1, sizeof(double));
	double t0 = now_ms(), first_ms = -1, lat_sum = 0, lat_max = 0;
	size_t next = 0, decoded = 0, lat_n = 0;
	uint32_t free_in[32], nfree = 0;
	for (uint32_t i = 0; i < nobuf; i++)
		free_in[nfree++] = i;
	int streaming_out = 0, eos_sent = 0, done = 0;
	while (!done) {
		// feed every free input buffer
		while (nfree > 0 && !eos_sent) {
			uint32_t i = free_in[--nfree];
			struct v4l2_plane pl = {0};
			pl.m.userptr = (unsigned long)obuf[i].map;
			pl.length = obuf[i].len;
			pl.reserved[0] = (uint32_t)obuf[i].fd;
			struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_USERPTR,
			                        .index = i, .m.planes = &pl, .length = 1};
			if (next < naus) {
				memcpy(obuf[i].map, stream + aus[next].off, aus[next].len);
				pl.bytesused = aus[next].len;
				b.timestamp.tv_sec = (time_t)(next / 1000000);
				b.timestamp.tv_usec = (suseconds_t)(next % 1000000);
				qtime[next] = now_ms();
				next++;
			} else {
				pl.bytesused = 0;
				b.flags = QCOM_BUF_FLAG_EOS;
				eos_sent = 1;
			}
			if (xioctl(VIDIOC_QBUF, &b, "QBUF output") < 0)
				return 1;
			if (!streaming_out) {
				int t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
				if (xioctl(VIDIOC_STREAMON, &t, "STREAMON output") < 0)
					return 1;
				streaming_out = 1;
			}
		}
		struct pollfd p = {.fd = dev, .events = POLLIN | POLLOUT | POLLPRI | POLLRDNORM | POLLWRNORM};
		int r = poll(&p, 1, 3000);
		if (r == 0) {
			fprintf(stderr, "vdectest: nothing for 3 s (fed %zu, decoded %zu): stuck\n", next, decoded);
			break;
		}
		if (r < 0)
			continue;
		if (p.revents & POLLPRI) {
			struct v4l2_event ev;
			while (xioctl(VIDIOC_DQEVENT, &ev, NULL) == 0) {
				uint32_t *d = (uint32_t *)ev.u.data;
				if (ev.type == EV_SETTINGS_INSUFFICIENT || ev.type == EV_BITDEPTH_INSUFFICIENT) {
					fprintf(stderr, "vdectest: port settings changed, insufficient: %ux%u (flags 0x%x): reconfiguring\n",
					        d[1], d[0], d[2]);
					cap_teardown();
					if (cap_setup(capcount) < 0)
						return 1;
				} else if (ev.type == EV_SETTINGS_SUFFICIENT) {
					fprintf(stderr, "vdectest: port settings changed, sufficient\n");
				} else if (ev.type == EV_SYS_ERROR || ev.type == EV_HW_UNSUPPORTED) {
					fprintf(stderr, "vdectest: decoder error event 0x%x\n", ev.type);
					return 1;
				} else {
					fprintf(stderr, "vdectest: event 0x%x\n", ev.type);
				}
			}
		}
		if (p.revents & (POLLOUT | POLLWRNORM)) {
			struct v4l2_plane pl = {0};
			struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_USERPTR,
			                        .m.planes = &pl, .length = 1};
			while (xioctl(VIDIOC_DQBUF, &b, NULL) == 0)
				free_in[nfree++] = b.index;
		}
		if (p.revents & (POLLIN | POLLRDNORM)) {
			struct v4l2_plane pl[2];
			struct v4l2_buffer b;
			for (;;) {
				memset(pl, 0, sizeof(pl));
				b = (struct v4l2_buffer){.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_USERPTR,
				                         .m.planes = pl, .length = cap_planes};
				if (xioctl(VIDIOC_DQBUF, &b, NULL) < 0)
					break;
				if (pl[0].bytesused > 0) {
					double t = now_ms();
					size_t idx = (size_t)b.timestamp.tv_sec * 1000000 + (size_t)b.timestamp.tv_usec;
					if (first_ms < 0)
						first_ms = t - t0;
					if (idx < naus && qtime[idx] > 0) {
						double l = t - qtime[idx];
						lat_sum += l;
						lat_max = l > lat_max ? l : lat_max;
						lat_n++;
					}
					decoded++;
					if (out) {
						const uint8_t *y = cbuf[b.index][0].map;
						if (ubwc) {
							fwrite(y, 1, pl[0].bytesused, out);
						} else {
							for (uint32_t row = 0; row < height; row++)
								fwrite(y + (size_t)row * stride, 1, width, out);
							const uint8_t *uv = y + (size_t)stride * scanlines;
							for (uint32_t row = 0; row < height / 2; row++)
								fwrite(uv + (size_t)row * stride, 1, width, out);
						}
					}
				}
				if (b.flags & QCOM_BUF_FLAG_EOS) {
					done = 1;
					break;
				}
				// back to the decoder
				for (uint32_t k = 0; k < cap_planes && k < 2; k++) {
					pl[k].m.userptr = (unsigned long)cbuf[b.index][k].map;
					pl[k].length = cbuf[b.index][k].len;
					pl[k].reserved[0] = (uint32_t)cbuf[b.index][k].fd;
					pl[k].reserved[1] = 0;
					pl[k].bytesused = 0;
				}
				b.flags = 0;
				if (xioctl(VIDIOC_QBUF, &b, "QBUF capture") < 0)
					return 1;
			}
		}
	}
	double el = now_ms() - t0;
	fprintf(stderr, "vdectest: %zu of %zu frames in %.0f ms: %.1f fps; first frame after %.1f ms; latency %.1f ms avg, %.1f max\n",
	        decoded, naus, el, decoded * 1000.0 / el, first_ms, lat_n ? lat_sum / lat_n : 0, lat_max);
	if (out)
		fclose(out);
	int t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	xioctl(VIDIOC_STREAMOFF, &t, NULL);
	cap_teardown();
	close(dev);
	return decoded == naus ? 0 : 3;
}
