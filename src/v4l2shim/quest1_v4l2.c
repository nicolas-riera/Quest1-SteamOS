// quest1_v4l2: the mainline V4L2 memory-to-memory encoder API, as Steam's Remote Play uses it, on the Quest 1's
// venus encoder (downstream msm_vidc of kernel 4.4, /dev/video33). LD_PRELOAD'ed into the Steam client; inert in
// any process that never opens the encoder (one compare per 'V' ioctl, mmap and close).
//
// Steam (steamui.so, CGameStreamVideoStageV4L2) opens /dev/video-enc0 (a udev symlink to the encoder) and does
// what a mainline stateful encoder takes:
//   S_FMT OUTPUT (NV12 or RGB32, the stream's size), S_FMT CAPTURE (H264 or HEVC), S_SELECTION crop, S_PARM,
//   S_CTRL: FRAME_RC_ENABLE, BITRATE_MODE (VBR), GOP_SIZE (INT_MAX: no periodic keyframe), PREPEND_SPSPPS_TO_IDR,
//   H264 profile and level or HEVC profile and level, BITRATE and BITRATE_PEAK (also while streaming),
//   REQBUFS + QUERYBUF + mmap on both queues (MMAP), QBUF the capture buffers, STREAMON both, then per frame:
//   copy the frame in (rows of plane_fmt[0].bytesperline, the UV plane right under the S_FMT height), QBUF it,
//   poll, DQBUF the bitstream, DQBUF the frame back; FORCE_KEY_FRAME when the client lost a frame.
// What msm_vidc wants instead, and what this layer does about it:
//  - USERPTR buffers backed by ION only (plane.reserved[0] = the ION fd, [1] = the offset): REQBUFS/QBUF/DQBUF go
//    to the driver as USERPTR on ION buffers allocated here; QUERYBUF and mmap are answered here (the mmap offset
//    is a cookie naming the queue and the buffer).
//  - Every buffer registered with the firmware (PREPARE_BUF) right after REQBUFS, as Android's encoder does: without
//    it the session's START asserts in the firmware (Err_Fatal, venus_venc_host_driver.c:1754; the driver brings
//    the firmware back at the next open). Found by running Android's own encoder through libhybris (omxprobe.c).
//  - STREAMON OUTPUT is held until the first raw frame is queued (Android's order: the firmware starts with a
//    frame waiting).
//  - The CAPTURE format first (it opens the firmware session): an OUTPUT S_FMT that comes first is answered here
//    and sent to the driver right after the CAPTURE one.
//  - S_FMT OUTPUT leaves bytesperline at 0 (Steam would copy every row onto the first): it is filled with venus's
//    stride (NV12: width aligned to 128; RGB32: width * 4 aligned to 128).
//  - NV12's UV plane starts at stride * scanlines (the height aligned to 32), Steam writes it at stride * height:
//    when stride * height is a whole number of pages, Steam's mapping of an OUTPUT buffer is two mappings of the
//    ION buffer, so its UV rows land where venus reads them (no copy); otherwise the UV plane is moved at QBUF.
//  - No S_SELECTION: a crop of the whole frame is accepted. No FRAME_RC_ENABLE (rate control is on with any
//    bitrate mode), GOP_SIZE is NUM_P_FRAMES (+ IDR_PERIOD 1: every intra frame an IDR), FORCE_KEY_FRAME is
//    REQUEST_IFRAME, HEVC profile and level are the private VIDC controls (other numbering).
//  - PREPEND_SPSPPS_TO_IDR is msm_vidc's HEADER_MODE "joined with every I frame" (the firmware writes the sequence
//    headers in front of each IDR). H.264 also gets picture order count type 2 (no reordering: a decoder shows
//    each frame at once), Android's low-latency setting.
//  - A sequence header buffer of its own (V4L2_QCOM_BUF_FLAG_CODECCONFIG, the SEPARATE header mode) is kept here
//    and goes back to the driver, and a keyframe that does not start with the headers gets them in front.
//  - Private buffer flags (QCOM_*, one of them the same bit as V4L2_BUF_FLAG_LAST) are cleared.
// A control the driver refuses is logged once and reported as done: Steam drops the hardware encoder on any error.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))

// --- msm_vidc's private V4L2 additions (kernel include/uapi/linux/{v4l2-controls,videodev2}.h)
#define VIDC_CID(n) ((V4L2_CTRL_CLASS_MPEG | 0x2000) + (n))
#define CID_IDR_PERIOD VIDC_CID(9)
#define CID_NUM_P_FRAMES VIDC_CID(10)
#define CID_REQUEST_IFRAME VIDC_CID(12)
#define CID_HEVC_PROFILE VIDC_CID(65)    // 0 main, 1 main10, 2 main still picture
#define CID_HEVC_TIER_LEVEL VIDC_CID(66) // main tier level n: 2 * (mainline's HEVC level index), up to 5.2
#define CID_H264_PIC_ORDER_CNT VIDC_CID(86)
#define HEADER_MODE_JOINED_WITH_I_FRAME 2 // msm_vidc's: the sequence headers in front of every intra frame
#define QCOM_BUF_FLAG_CODECCONFIG 0x00020000
#define BUF_FLAGS_KEPT                                                                                          \
	(V4L2_BUF_FLAG_MAPPED | V4L2_BUF_FLAG_QUEUED | V4L2_BUF_FLAG_DONE | V4L2_BUF_FLAG_KEYFRAME |              \
	 V4L2_BUF_FLAG_PFRAME | V4L2_BUF_FLAG_BFRAME | V4L2_BUF_FLAG_ERROR | V4L2_BUF_FLAG_TIMECODE |             \
	 V4L2_BUF_FLAG_TIMESTAMP_MASK | V4L2_BUF_FLAG_TSTAMP_SRC_MASK)

// --- legacy ION (kernel 4.4 staging: a handle, then a dma-buf fd)
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

#define AL(x, a) (((x) + (a)-1) / (a) * (a))
#define PAGE 4096u
#define MAXBUF 32
#define MAXDEV 4
#define HDR_MAX 1024
#define COOKIE(q, i) (0x40000000u | (uint32_t)(q) << 24 | (uint32_t)(i) << 16)

enum { OUT, CAP }; // OUTPUT queue: raw frames in; CAPTURE queue: bitstream out

struct ionbuf {
	int fd;
	uint8_t *map; // ours, linear
	size_t size;
};

struct enc {
	int fd;
	int ion;
	bool cap_set, out_pending;
	struct v4l2_format out_fmt; // an OUTPUT S_FMT that came before the CAPTURE one
	uint32_t codec, pix, w, h;
	uint32_t stride, scan; // venus's layout of a raw frame
	uint32_t frame;        // a raw frame's size, as Steam sees it (sizeimage)
	uint32_t uv_steam, uv_venus, uv_len;
	bool remap; // Steam's mapping of a raw buffer puts its UV rows where venus reads them
	uint32_t cap_size;
	uint32_t count[2];
	struct ionbuf b[2][MAXBUF];
	uint8_t hdr[HDR_MAX];
	uint32_t hdr_len;
	bool out_on_pending; // STREAMON OUTPUT waits for the first raw frame
	uint64_t frames, keyframes, bytes;
	uint32_t refused[8];
};

static struct enc *encs[MAXDEV];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void logf_(const char *fmt, ...)
{
	char s[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(s, sizeof(s), fmt, ap);
	va_end(ap);
	fprintf(stderr, "quest1_v4l2: %s\n", s);
}

static int sys_ioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do
		r = (int)syscall(SYS_ioctl, fd, req, arg);
	while (r < 0 && errno == EINTR);
	return r;
}

static void *sys_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
	return (void *)syscall(SYS_mmap, addr, len, prot, flags, fd, off);
}

// the encoder's device number: the first video node whose card is msm_venc_<soc> (its sysfs name is empty)
static dev_t venc_dev;

static bool is_venc(int fd, const struct stat *st)
{
	if (!S_ISCHR(st->st_mode))
		return false;
	if (venc_dev)
		return st->st_rdev == venc_dev;
	struct v4l2_capability cap;
	memset(&cap, 0, sizeof(cap));
	if (major(st->st_rdev) != 81 || sys_ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0 ||
	    strncmp((const char *)cap.card, "msm_venc", 8))
		return false;
	venc_dev = st->st_rdev;
	return true;
}

static struct enc *find(int fd)
{
	for (int i = 0; i < MAXDEV; i++) {
		struct enc *e = __atomic_load_n(&encs[i], __ATOMIC_ACQUIRE);
		if (e && e->fd == fd)
			return e;
	}
	return NULL;
}

// an fd of the encoder not seen yet: track it
static struct enc *adopt(int fd)
{
	struct stat st;
	if (fstat(fd, &st) < 0 || !is_venc(fd, &st))
		return NULL;
	struct enc *e = calloc(1, sizeof(*e));
	e->fd = fd;
	e->ion = open("/dev/ion", O_RDONLY | O_CLOEXEC);
	if (e->ion < 0)
		logf_("/dev/ion: %s (the encoder's buffers cannot be made)", strerror(errno));
	pthread_mutex_lock(&lock);
	int i = 0;
	while (i < MAXDEV && encs[i])
		i++;
	if (i < MAXDEV)
		__atomic_store_n(&encs[i], e, __ATOMIC_RELEASE);
	pthread_mutex_unlock(&lock);
	if (i == MAXDEV) {
		close(e->ion);
		free(e);
		return NULL;
	}
	return e;
}

static int ion_alloc(struct enc *e, size_t len, struct ionbuf *b)
{
	len = AL(len, PAGE);
	struct ion_allocation_data a = {.len = len, .align = PAGE, .heap_id_mask = 1u << ION_SYSTEM_HEAP_ID};
	if (sys_ioctl(e->ion, ION_IOC_ALLOC, &a) < 0)
		return -1;
	struct ion_fd_data f = {.handle = a.handle};
	int r = sys_ioctl(e->ion, ION_IOC_SHARE, &f);
	sys_ioctl(e->ion, ION_IOC_FREE, &a.handle); // the fd keeps the buffer
	if (r < 0)
		return -1;
	b->map = sys_mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, f.fd, 0);
	if (b->map == MAP_FAILED) {
		close(f.fd);
		return -1;
	}
	b->fd = f.fd;
	b->size = len;
	return 0;
}

static void free_bufs(struct enc *e, int q)
{
	for (uint32_t i = 0; i < e->count[q]; i++) {
		munmap(e->b[q][i].map, e->b[q][i].size);
		close(e->b[q][i].fd);
	}
	e->count[q] = 0;
}

static void forget(struct enc *e)
{
	pthread_mutex_lock(&lock);
	for (int i = 0; i < MAXDEV; i++)
		if (encs[i] == e)
			__atomic_store_n(&encs[i], NULL, __ATOMIC_RELEASE);
	pthread_mutex_unlock(&lock);
	if (e->frames)
		logf_("encoder closed: %llu frames, %llu keyframes, %.1f kB per frame", (unsigned long long)e->frames,
		      (unsigned long long)e->keyframes, e->bytes / 1024.0 / e->frames);
	free_bufs(e, OUT);
	free_bufs(e, CAP);
	if (e->ion >= 0)
		close(e->ion);
	free(e);
}

static int qof(uint32_t type)
{
	return type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ? OUT : type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ? CAP : -1;
}

static const char *fourcc(uint32_t f, char *s)
{
	for (int i = 0; i < 4; i++)
		s[i] = (char)(f >> (8 * i));
	s[4] = 0;
	return s;
}

// --- formats

// venus's layout of a raw frame of this format and size (msm_media_info.h); false if not handled
static bool layout(struct enc *e, uint32_t pix, uint32_t w, uint32_t h)
{
	e->pix = pix, e->w = w, e->h = h;
	e->remap = false;
	e->uv_len = 0;
	switch (pix) {
	case V4L2_PIX_FMT_NV12:
	case V4L2_PIX_FMT_NV21: {
		e->stride = AL(w, 128);
		e->scan = AL(h, 32);
		uint32_t uv_scan = AL((h + 1) / 2, 16);
		// what VENUS_BUFFER_SIZE gives, with room for its extradata (the driver's own size replaces it)
		uint32_t extra = 8 * e->stride > 64 * 1024 ? 8 * e->stride : 64 * 1024;
		e->frame = AL(e->stride * e->scan + e->stride * uv_scan + PAGE + extra, PAGE);
		e->uv_steam = e->stride * h;
		e->uv_venus = e->stride * e->scan;
		e->uv_len = e->stride * ((h + 1) / 2);
		e->remap = e->uv_steam != e->uv_venus && e->uv_steam % PAGE == 0;
		return true;
	}
	case V4L2_PIX_FMT_RGB32:
		e->stride = AL(w * 4, 128);
		e->scan = AL(h, 32);
		e->frame = AL(e->stride * e->scan, PAGE);
		return true;
	}
	return false;
}

static void fill_out_fmt(struct enc *e, struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *p = &f->fmt.pix_mp;
	p->field = V4L2_FIELD_NONE;
	p->num_planes = 1;
	p->plane_fmt[0].bytesperline = e->stride;
	p->plane_fmt[0].sizeimage = e->frame;
	memset(p->plane_fmt[0].reserved, 0, sizeof(p->plane_fmt[0].reserved));
}

static int set_out_fmt(struct enc *e, struct v4l2_format *f)
{
	struct v4l2_format d = *f;
	if (sys_ioctl(e->fd, VIDIOC_S_FMT, &d) < 0) {
		int err = errno;
		char s[5];
		logf_("S_FMT OUTPUT %s %ux%u: %s", fourcc(f->fmt.pix_mp.pixelformat, s), f->fmt.pix_mp.width,
		      f->fmt.pix_mp.height, strerror(err));
		errno = err;
		return -1;
	}
	if (d.fmt.pix_mp.plane_fmt[0].sizeimage > e->frame)
		e->frame = AL(d.fmt.pix_mp.plane_fmt[0].sizeimage, PAGE);
	fill_out_fmt(e, f);
	return 0;
}

static int set_ctrl(struct enc *e, uint32_t id, int32_t v)
{
	struct v4l2_control c = {.id = id, .value = v};
	return sys_ioctl(e->fd, VIDIOC_S_CTRL, &c);
}

static int s_fmt(struct enc *e, struct v4l2_format *f)
{
	int q = qof(f->type);
	if (q == OUT) {
		if (!layout(e, f->fmt.pix_mp.pixelformat, f->fmt.pix_mp.width, f->fmt.pix_mp.height)) {
			errno = EINVAL;
			return -1;
		}
		if (!e->cap_set) { // answered now, sent after the CAPTURE format
			fill_out_fmt(e, f);
			e->out_fmt = *f;
			e->out_pending = true;
			return 0;
		}
		return set_out_fmt(e, f);
	}
	if (q != CAP)
		return sys_ioctl(e->fd, VIDIOC_S_FMT, f);
	if (sys_ioctl(e->fd, VIDIOC_S_FMT, f) < 0) {
		int err = errno;
		char s[5];
		logf_("S_FMT CAPTURE %s %ux%u: %s", fourcc(f->fmt.pix_mp.pixelformat, s), f->fmt.pix_mp.width,
		      f->fmt.pix_mp.height, strerror(err));
		errno = err;
		return -1;
	}
	e->cap_set = true;
	e->codec = f->fmt.pix_mp.pixelformat;
	e->cap_size = f->fmt.pix_mp.plane_fmt[0].sizeimage;
	e->hdr_len = 0;
	set_ctrl(e, CID_IDR_PERIOD, 1); // every intra frame an IDR: a forced keyframe is a decoder's entry point
	// the sequence headers in every IDR, as Steam asks (PREPEND_SPSPPS_TO_IDR), from the firmware itself
	set_ctrl(e, V4L2_CID_MPEG_VIDEO_HEADER_MODE, HEADER_MODE_JOINED_WITH_I_FRAME);
	if (e->codec == V4L2_PIX_FMT_H264) // POC type 2: no reordering, a decoder shows each frame at once (Android's low latency)
		set_ctrl(e, CID_H264_PIC_ORDER_CNT, 2);
	if (e->out_pending) {
		e->out_pending = false;
		if (set_out_fmt(e, &e->out_fmt) < 0)
			return -1;
	}
	return 0;
}

// --- controls

static int s_ctrl(struct enc *e, struct v4l2_control *c)
{
	uint32_t id = c->id;
	int32_t v = c->value;
	switch (id) {
	case V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE:
		return 0; // rate control is on with any bitrate mode
	case V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR:
		id = V4L2_CID_MPEG_VIDEO_HEADER_MODE;
		v = v ? HEADER_MODE_JOINED_WITH_I_FRAME : V4L2_MPEG_VIDEO_HEADER_MODE_SEPARATE;
		break;
	case V4L2_CID_MPEG_VIDEO_GOP_SIZE:
		id = CID_NUM_P_FRAMES;
		v = v > 0 ? v - 1 : 0;
		break;
	case V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME:
		id = CID_REQUEST_IFRAME;
		v = 0;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_PROFILE:
		id = CID_HEVC_PROFILE;
		v = v == V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10 ? 1 : v == V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_STILL_PICTURE ? 2 : 0;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_LEVEL:
		id = CID_HEVC_TIER_LEVEL;
		v = 2 * v > 18 ? 18 : 2 * v;
		break;
	}
	if (set_ctrl(e, id, v) == 0)
		return 0;
	int err = errno;
	bool seen = false;
	for (int i = 0; i < 8 && e->refused[i] && !seen; i++)
		seen = e->refused[i] == c->id;
	for (int i = 0; i < 8 && !seen; i++)
		if (!e->refused[i]) {
			e->refused[i] = c->id;
			break;
		}
	if (!seen)
		logf_("control 0x%x = %d (sent as 0x%x = %d) refused: %s; carrying on", c->id, c->value, id, v,
		      strerror(err));
	return 0;
}

// --- buffers

static void to_driver(struct enc *e, int q, const struct v4l2_buffer *b, struct v4l2_buffer *d, struct v4l2_plane *p);

static int reqbufs(struct enc *e, struct v4l2_requestbuffers *rb)
{
	int q = qof(rb->type);
	if (q < 0 || rb->memory != V4L2_MEMORY_MMAP)
		return sys_ioctl(e->fd, VIDIOC_REQBUFS, rb);
	struct v4l2_requestbuffers d = *rb;
	d.memory = V4L2_MEMORY_USERPTR;
	if (sys_ioctl(e->fd, VIDIOC_REQBUFS, &d) < 0) {
		int err = errno;
		logf_("REQBUFS %s %u: %s", q == OUT ? "OUTPUT" : "CAPTURE", rb->count, strerror(err));
		errno = err;
		return -1;
	}
	free_bufs(e, q);
	if (d.count > MAXBUF) {
		errno = ENOMEM;
		return -1;
	}
	if (q == CAP && !e->cap_size) {
		struct v4l2_format f = {.type = rb->type};
		if (sys_ioctl(e->fd, VIDIOC_G_FMT, &f) == 0)
			e->cap_size = f.fmt.pix_mp.plane_fmt[0].sizeimage;
	}
	size_t size = q == CAP ? e->cap_size : e->frame + (e->remap ? e->uv_venus - e->uv_steam : 0);
	for (uint32_t i = 0; i < d.count; i++)
		if (e->ion < 0 || ion_alloc(e, size, &e->b[q][i]) < 0) {
			int err = errno;
			logf_("ION buffer of %zu bytes: %s", size, strerror(err));
			e->count[q] = i;
			free_bufs(e, q);
			errno = ENOMEM;
			return -1;
		}
	e->count[q] = d.count;
	rb->count = d.count;
	// registered with the firmware now, as Android's encoder does: the session's START asserts
	// (venus_venc_host_driver.c:1754) when its bitstream buffers were not set before it
	for (uint32_t i = 0; i < d.count; i++) {
		struct v4l2_buffer b = {.index = i, .type = rb->type, .memory = V4L2_MEMORY_MMAP}, db;
		struct v4l2_plane p;
		to_driver(e, q, &b, &db, &p);
		if (sys_ioctl(e->fd, VIDIOC_PREPARE_BUF, &db) < 0)
			logf_("PREPARE_BUF %s %u: %s", q == OUT ? "OUTPUT" : "CAPTURE", i, strerror(errno));
	}
	if (d.count) {
		char s[5];
		if (q == OUT)
			logf_("raw frames: %s %ux%u, stride %u, %u lines, %u buffers%s", fourcc(e->pix, s), e->w, e->h,
			      e->stride, e->scan, d.count,
			      e->uv_len && e->uv_steam != e->uv_venus ? e->remap ? ", UV plane remapped" : ", UV plane moved"
			                                              : "");
		else
			logf_("bitstream: %s, %u buffers of %zu kB", fourcc(e->codec, s), d.count, size / 1024);
	}
	return 0;
}

static bool mine(struct enc *e, struct v4l2_buffer *b, int *q)
{
	*q = qof(b->type);
	return *q >= 0 && b->memory == V4L2_MEMORY_MMAP && b->index < e->count[*q] && b->length >= 1 && b->m.planes;
}

// the size Steam's mapping of a buffer has
static uint32_t view_size(struct enc *e, int q, uint32_t i)
{
	return q == OUT ? e->frame : (uint32_t)e->b[q][i].size;
}

static int querybuf(struct enc *e, struct v4l2_buffer *b)
{
	int q;
	if (!mine(e, b, &q))
		return sys_ioctl(e->fd, VIDIOC_QUERYBUF, b);
	struct v4l2_plane *p = &b->m.planes[0];
	memset(p, 0, sizeof(*p));
	p->length = view_size(e, q, b->index);
	p->m.mem_offset = COOKIE(q, b->index);
	b->length = 1;
	b->flags = V4L2_BUF_FLAG_MAPPED | V4L2_BUF_FLAG_TIMESTAMP_COPY;
	return 0;
}

static void *map_buf(struct enc *e, void *addr, size_t len, int prot, int flags, off_t off)
{
	uint32_t c = (uint32_t)off;
	int q = (c >> 24) & 0x3f, i = (c >> 16) & 0xff;
	if ((c & 0xc000ffffu) != 0x40000000u || q > CAP || i >= (int)e->count[q] || len > view_size(e, q, i)) {
		errno = EINVAL;
		return MAP_FAILED;
	}
	struct ionbuf *b = &e->b[q][i];
	flags = (flags & ~MAP_PRIVATE) | MAP_SHARED;
	if (q == CAP || !e->remap)
		return sys_mmap(addr, len, prot, flags, b->fd, 0);
	// Steam's rows [0, h) are the buffer's; what follows (its UV plane) is the buffer from stride * scanlines
	uint8_t *v = sys_mmap(addr, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | (flags & MAP_FIXED), -1, 0);
	if (v == MAP_FAILED)
		return v;
	if (sys_mmap(v, e->uv_steam, prot, MAP_SHARED | MAP_FIXED, b->fd, 0) == MAP_FAILED ||
	    (len > e->uv_steam &&
	     sys_mmap(v + e->uv_steam, len - e->uv_steam, prot, MAP_SHARED | MAP_FIXED, b->fd, e->uv_venus) == MAP_FAILED)) {
		int err = errno;
		munmap(v, len);
		errno = err;
		return MAP_FAILED;
	}
	return v;
}

static void to_driver(struct enc *e, int q, const struct v4l2_buffer *b, struct v4l2_buffer *d, struct v4l2_plane *p)
{
	struct ionbuf *ib = &e->b[q][b->index];
	*d = *b;
	memset(p, 0, sizeof(*p));
	d->memory = V4L2_MEMORY_USERPTR;
	d->m.planes = p;
	d->length = 1;
	p->m.userptr = (unsigned long)ib->map;
	p->length = (uint32_t)ib->size;
	p->reserved[0] = (uint32_t)ib->fd;
	p->reserved[1] = 0;
}

static int qbuf(struct enc *e, struct v4l2_buffer *b)
{
	int q;
	if (!mine(e, b, &q))
		return sys_ioctl(e->fd, VIDIOC_QBUF, b);
	struct v4l2_buffer d;
	struct v4l2_plane p;
	to_driver(e, q, b, &d, &p);
	if (q == OUT) {
		if (e->uv_len && e->uv_steam != e->uv_venus && !e->remap)
			memmove(e->b[q][b->index].map + e->uv_venus, e->b[q][b->index].map + e->uv_steam, e->uv_len);
		p.bytesused = e->frame;
	}
	int r = sys_ioctl(e->fd, VIDIOC_QBUF, &d);
	if (r < 0) {
		int err = errno;
		logf_("QBUF %s %u: %s", q == OUT ? "OUTPUT" : "CAPTURE", b->index, strerror(err));
		errno = err;
		return r;
	}
	b->flags = (d.flags & BUF_FLAGS_KEPT) | V4L2_BUF_FLAG_QUEUED;
	if (q == OUT && e->out_on_pending) {
		e->out_on_pending = false;
		int t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		if (sys_ioctl(e->fd, VIDIOC_STREAMON, &t) < 0) {
			int err = errno;
			logf_("STREAMON OUTPUT (at the first frame): %s", strerror(err));
			errno = err;
			return -1;
		}
	}
	return 0;
}

// does the bitstream start with a parameter set (H.264 SPS, HEVC VPS)?
static bool starts_with_headers(struct enc *e, const uint8_t *p, uint32_t n)
{
	uint32_t i = 0;
	while (i + 3 < n && !(p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1))
		i++;
	if (i + 3 >= n)
		return false;
	uint8_t nal = p[i + 3];
	return e->codec == V4L2_PIX_FMT_HEVC ? ((nal >> 1) & 0x3f) == 32 : (nal & 0x1f) == 7;
}

static int dqbuf(struct enc *e, struct v4l2_buffer *b)
{
	int q;
	if (!mine(e, b, &q))
		return sys_ioctl(e->fd, VIDIOC_DQBUF, b);
	struct v4l2_plane *sp = b->m.planes;
	struct v4l2_buffer d;
	struct v4l2_plane p;
	for (;;) {
		d = *b;
		memset(&p, 0, sizeof(p));
		d.memory = V4L2_MEMORY_USERPTR;
		d.m.planes = &p;
		d.length = 1;
		if (sys_ioctl(e->fd, VIDIOC_DQBUF, &d) < 0)
			return -1;
		if (q == OUT || d.index >= e->count[q] || !(d.flags & QCOM_BUF_FLAG_CODECCONFIG))
			break;
		// the sequence headers alone: kept for the keyframes, the buffer goes straight back
		struct ionbuf *ib = &e->b[q][d.index];
		uint32_t n = p.bytesused;
		if (p.data_offset < ib->size && n <= ib->size - p.data_offset && n <= HDR_MAX) {
			memcpy(e->hdr, ib->map + p.data_offset, n);
			e->hdr_len = n;
		} else {
			logf_("sequence headers of %u bytes not kept", n);
		}
		struct v4l2_buffer r = *b;
		r.index = d.index;
		struct v4l2_buffer rd;
		struct v4l2_plane rp;
		to_driver(e, q, &r, &rd, &rp);
		if (sys_ioctl(e->fd, VIDIOC_QBUF, &rd) < 0) {
			logf_("QBUF CAPTURE %u (after the headers): %s", d.index, strerror(errno));
			return -1;
		}
		struct pollfd pf = {.fd = e->fd, .events = POLLIN};
		poll(&pf, 1, 1000); // a frame follows at once; DQBUF itself waits or says EAGAIN
	}
	if (d.index >= e->count[q]) {
		errno = EINVAL;
		return -1;
	}
	struct ionbuf *ib = &e->b[q][d.index];
	uint32_t n = p.bytesused;
	if (q == CAP) {
		if (p.data_offset && p.data_offset < ib->size) {
			if (n > ib->size - p.data_offset)
				n = (uint32_t)(ib->size - p.data_offset);
			memmove(ib->map, ib->map + p.data_offset, n);
		}
		if (d.flags & V4L2_BUF_FLAG_KEYFRAME) {
			e->keyframes++;
			if (e->hdr_len && n + e->hdr_len <= ib->size && !starts_with_headers(e, ib->map, n)) {
				memmove(ib->map + e->hdr_len, ib->map, n);
				memcpy(ib->map, e->hdr, e->hdr_len);
				n += e->hdr_len;
			}
		}
		e->frames++;
		e->bytes += n;
	} else {
		n = 0;
	}
	b->index = d.index;
	b->flags = d.flags & BUF_FLAGS_KEPT & ~V4L2_BUF_FLAG_QUEUED;
	b->field = d.field;
	b->timestamp = d.timestamp;
	b->timecode = d.timecode;
	b->sequence = d.sequence;
	b->length = 1;
	b->m.planes = sp;
	memset(sp, 0, sizeof(*sp));
	sp->bytesused = n;
	sp->length = view_size(e, q, d.index);
	sp->m.mem_offset = COOKIE(q, d.index);
	return 0;
}

static int s_selection(struct enc *e, struct v4l2_selection *s)
{
	if (s->type != V4L2_BUF_TYPE_VIDEO_OUTPUT && s->type != V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		return sys_ioctl(e->fd, VIDIOC_S_SELECTION, s);
	if (s->r.left || s->r.top || (e->w && ((uint32_t)s->r.width != e->w || s->r.height != e->h)))
		logf_("crop %ux%u+%d+%d of %ux%u not supported: the whole frame is encoded", s->r.width, s->r.height,
		      s->r.left, s->r.top, e->w, e->h);
	s->r.left = s->r.top = 0;
	s->r.width = e->w;
	s->r.height = e->h;
	return 0;
}

// --- the hooks

EXPORT int ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	va_start(ap, req);
	void *arg = va_arg(ap, void *);
	va_end(ap);
	if (_IOC_TYPE(req) != 'V')
		return sys_ioctl(fd, req, arg);
	struct enc *e = find(fd);
	if (!e && !(e = adopt(fd)))
		return sys_ioctl(fd, req, arg);
	switch (req) {
	case VIDIOC_S_FMT:
		return s_fmt(e, arg);
	case VIDIOC_S_CTRL:
		return s_ctrl(e, arg);
	case VIDIOC_REQBUFS:
		return reqbufs(e, arg);
	case VIDIOC_QUERYBUF:
		return querybuf(e, arg);
	case VIDIOC_QBUF:
		return qbuf(e, arg);
	case VIDIOC_DQBUF:
		return dqbuf(e, arg);
	case VIDIOC_S_SELECTION:
		return s_selection(e, arg);
	case VIDIOC_STREAMON:
	case VIDIOC_STREAMOFF: {
		if (*(int *)arg == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
			bool was = e->out_on_pending;
			e->out_on_pending = req == VIDIOC_STREAMON;
			if (req == VIDIOC_STREAMON || was)
				return 0; // the firmware starts with a frame queued (as Android's encoder does it)
		}
		int r = sys_ioctl(fd, req, arg);
		if (r < 0) {
			int err = errno;
			logf_("%s: %s", req == VIDIOC_STREAMON ? "STREAMON" : "STREAMOFF", strerror(err));
			errno = err;
		}
		return r;
	}
	}
	return sys_ioctl(fd, req, arg);
}

EXPORT void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
	struct enc *e = fd >= 0 ? find(fd) : NULL;
	if (e)
		return map_buf(e, addr, len, prot, flags, off);
	return sys_mmap(addr, len, prot, flags, fd, off);
}

EXPORT void *mmap64(void *addr, size_t len, int prot, int flags, int fd, off_t off)
	__attribute__((alias("mmap")));

EXPORT int close(int fd)
{
	struct enc *e = fd >= 0 ? find(fd) : NULL;
	int r = (int)syscall(SYS_close, fd);
	if (e)
		forget(e); // after the driver let go of the buffers
	return r;
}
