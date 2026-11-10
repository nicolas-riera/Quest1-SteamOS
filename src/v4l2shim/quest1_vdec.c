// quest1_vdec: the mainline V4L2 stateful decoder API, as FFmpeg's *_v4l2m2m decoders use it, on the Quest 1's
// venus decoder (downstream msm_vidc of kernel 4.4, /dev/video32). Part of libquest1_v4l2.so (the hooks are in
// quest1_v4l2.c); inert in a process that never opens the decoder.
//
// What libavcodec (v4l2_m2m_dec.c, FFmpeg 8) does, the client this was written against:
//   opens every /dev/video* and keeps the first that takes its formats (QUERYCAP, ENUM_FMT, TRY_FMT), then
//   S_FMT OUTPUT (H264/HEVC/VP9..., the stream's size), S_FMT CAPTURE, REQBUFS + QUERYBUF + mmap of 16 OUTPUT
//   buffers (MMAP), SUBSCRIBE_EVENT SOURCE_CHANGE and EOS, QBUF of the first packet, STREAMON OUTPUT, and only
//   then the CAPTURE side: G_FMT, S_SELECTION + G_SELECTION (the picture inside the buffer), REQBUFS of 20,
//   QUERYBUF, mmap, QBUF of them all, STREAMON. Then poll (POLLIN: a frame, POLLOUT: a packet buffer came back,
//   POLLPRI: an event), DQBUF/QBUF. On SOURCE_CHANGE it reads G_FMT and, when the size changed, does STREAMOFF
//   CAPTURE at once, frees, S_FMT, and sets CAPTURE up again. At the end of the stream DECODER_CMD STOP, then it
//   takes frames until one is empty or has V4L2_BUF_FLAG_LAST. A frame is one plane: NV12's UV rows at
//   bytesperline * the format's height.
// What msm_vidc wants instead (src/vdectest/vdectest.c found the rules), and what this does about it:
//  - USERPTR buffers backed by ION only (plane.reserved[0] = the ION fd): REQBUFS/QBUF/DQBUF go to the driver as
//    USERPTR on ION buffers made here; QUERYBUF and mmap are answered here (the mmap offset is a cookie). The ION
//    buffers are cached ones: the driver cleans (OUTPUT) and invalidates (CAPTURE) the caches itself, and reading
//    a frame from uncached memory is very slow.
//  - Its own events (V4L2_EVENT_MSM_VIDC_*). The first sequence header always raises "port settings changed,
//    insufficient", and the firmware then waits: the capture queue must be flushed (DECODER_CMD flush), the
//    flush-done event awaited, every buffer taken back, then STREAMOFF, REQBUFS 0, S_FMT, REQBUFS, QBUF, STREAMON
//    (which sends the firmware its "continue"). A capture STREAMOFF with buffers still in the firmware kills it.
//    When the size is the one the client was told, all of that is done here with the client's buffers, and the
//    client gets a SOURCE_CHANGE that changes nothing; when it is another size, the queue is flushed here, the
//    client gets SOURCE_CHANGE with the new G_FMT, and its STREAMOFF/REQBUFS/STREAMON do the rest.
//  - A CAPTURE STREAMOFF while the stream runs is a flush first, for the same reason.
//  - CAPTURE has the decoded size (16-aligned: 1920x1088), venus's stride (width aligned to 128) and scanlines
//    (height aligned to 32), with UV at stride * scanlines: G_FMT says height = scanlines (so the client's UV
//    offset is venus's, no copy) and G_SELECTION the picture, which is the size the client gave on OUTPUT when
//    that is what rounds to the decoded size. The size on CAPTURE given by the client is not used.
//  - ENUM_FMT CAPTURE lists NV12 only (the driver also has its compressed UBWC formats); there is no TRY_FMT.
//  - No DECODER_CMD STOP: an empty OUTPUT buffer with the EOS flag (one more than the client asked for is kept
//    for that), and the CAPTURE buffer that comes back with the EOS flag is the LAST one.
//  - Private buffer flags are cleared (one is the same bit as V4L2_BUF_FLAG_LAST).
// Not handled: a client that waits for SOURCE_CHANGE before it sets CAPTURE up (the driver starts the firmware
// session only once both queues stream), DMABUF/USERPTR clients, 10-bit streams (NV12 only).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
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

#include "quest1_v4l2.h"

// --- msm_vidc's private V4L2 additions (kernel include/uapi/linux/{v4l2-controls,videodev2}.h)
#define CID_BUFFER_SIZE_LIMIT ((V4L2_CTRL_CLASS_MPEG | 0x2000) + 64) // the largest packet, in bytes
#define EV_VIDC(n) (V4L2_EVENT_PRIVATE_START + 0x1000 + (n))
#define EV_FLUSH_DONE EV_VIDC(1)
#define EV_SETTINGS_SUFFICIENT EV_VIDC(2)
#define EV_SETTINGS_INSUFFICIENT EV_VIDC(3)
#define EV_BITDEPTH_INSUFFICIENT EV_VIDC(4)
#define EV_SYS_ERROR EV_VIDC(5)
#define EV_HW_OVERLOAD EV_VIDC(8)
#define EV_HW_UNSUPPORTED EV_VIDC(10)
#define QCOM_BUF_FLAG_DATA_CORRUPT 0x00400000
#define QCOM_BUF_FLAG_EOS 0x02000000
#define DEC_QCOM_CMD_FLUSH 4
#define QCOM_CMD_FLUSH_CAPTURE (1 << 1)

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
#define ION_FLAG_CACHED 1

#define AL(x, a) (((x) + (a)-1) / (a) * (a))
#define PAGE 4096u
#define MAXBUF 32
#define MAXDEV 4
#define COOKIE(q, i) (0x40000000u | (uint32_t)(q) << 24 | (uint32_t)(i) << 16)

enum { OUT, CAP }; // OUTPUT queue: the bitstream in; CAPTURE queue: frames out

struct ionbuf {
	int fd;
	uint8_t *map;
	size_t size;
};

struct q1dec {
	int fd, ion;
	pthread_mutex_t mu;
	bool set_up;
	struct v4l2_format out_fmt; // the client's OUTPUT format
	bool out_set;
	uint32_t in_size;
	uint32_t cw, ch;                   // the decoded size: what CAPTURE is set to in the driver
	uint32_t stride, scan;             // venus's layout of it
	uint32_t cap_size, extra_size;     // a frame buffer; the driver's second plane (extradata), if it has one
	uint32_t cap_planes;
	uint32_t crop_w, crop_h; // the picture
	bool cap_dirty;          // cw x ch is not the driver's CAPTURE format yet
	uint32_t count[2];       // the client's buffers (the driver has one more OUTPUT buffer: the EOS carrier)
	struct ionbuf b[2][MAXBUF + 1], x[MAXBUF];
	bool indrv[2][MAXBUF + 1];
	bool on[2];      // the driver's queues stream
	bool reconfig;   // CAPTURE was flushed for a new size: it waits for the client's STREAMOFF
	bool eos_sent, last_seen, dead;
	bool sub_src;
	uint32_t pending_src, ev_seq;
	struct v4l2_event late; // a sequence event that came while a flush was awaited
	bool has_late;
	struct v4l2_buffer fifo[MAXBUF]; // frames taken back by a flush, not given to the client yet
	uint32_t fifo_used[MAXBUF], fifo_n;
	uint64_t frames;
};

static struct q1dec *decs[MAXDEV];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void logf_(const char *fmt, ...)
{
	char s[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(s, sizeof(s), fmt, ap);
	va_end(ap);
	fprintf(stderr, "quest1_v4l2: decoder: %s\n", s);
}

static int sys_ioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do
		r = (int)syscall(SYS_ioctl, fd, req, arg);
	while (r < 0 && errno == EINTR);
	return r;
}

static int fail(int err)
{
	errno = err;
	return -1;
}

static const char *fourcc(uint32_t f, char *s)
{
	for (int i = 0; i < 4; i++)
		s[i] = (char)(f >> (8 * i));
	s[4] = 0;
	return s;
}

static int qof(uint32_t type)
{
	return type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ? OUT : type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ? CAP : -1;
}

// --- which fd is the decoder

struct q1dec *q1dec_find(int fd)
{
	for (int i = 0; i < MAXDEV; i++) {
		struct q1dec *e = __atomic_load_n(&decs[i], __ATOMIC_ACQUIRE);
		if (e && e->fd == fd)
			return e;
	}
	return NULL;
}

struct q1dec *q1dec_adopt(int fd)
{
	// the decoder's device number: the video node whose card is msm_vdec_<soc>; the others are remembered too
	// (QUERYCAP on the msm camera nodes logs a kernel warning each time)
	static dev_t vdec_dev, others[16];
	static int nothers;
	struct stat st;
	if (fstat(fd, &st) < 0 || !S_ISCHR(st.st_mode) || major(st.st_rdev) != 81)
		return NULL;
	if (!vdec_dev) {
		for (int i = 0; i < nothers; i++)
			if (others[i] == st.st_rdev)
				return NULL;
		struct v4l2_capability cap;
		memset(&cap, 0, sizeof(cap));
		if (sys_ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0 || strncmp((const char *)cap.card, "msm_vdec", 8)) {
			pthread_mutex_lock(&lock);
			if (nothers < 16)
				others[nothers++] = st.st_rdev;
			pthread_mutex_unlock(&lock);
			return NULL;
		}
		vdec_dev = st.st_rdev;
	}
	if (st.st_rdev != vdec_dev)
		return NULL;
	struct q1dec *e = calloc(1, sizeof(*e));
	e->fd = fd;
	e->ion = -1;
	pthread_mutex_init(&e->mu, NULL);
	pthread_mutex_lock(&lock);
	int i = 0;
	while (i < MAXDEV && decs[i])
		i++;
	if (i < MAXDEV)
		__atomic_store_n(&decs[i], e, __ATOMIC_RELEASE);
	pthread_mutex_unlock(&lock);
	if (i == MAXDEV) {
		free(e);
		return NULL;
	}
	return e;
}

// --- ION

static int ion_alloc(struct q1dec *e, size_t len, struct ionbuf *b)
{
	len = AL(len, PAGE);
	struct ion_allocation_data a = {.len = len, .align = PAGE, .heap_id_mask = 1u << ION_SYSTEM_HEAP_ID,
	                                .flags = ION_FLAG_CACHED};
	if (e->ion < 0 || sys_ioctl(e->ion, ION_IOC_ALLOC, &a) < 0)
		return -1;
	struct ion_fd_data f = {.handle = a.handle};
	int r = sys_ioctl(e->ion, ION_IOC_SHARE, &f);
	sys_ioctl(e->ion, ION_IOC_FREE, &a.handle); // the fd keeps the buffer
	if (r < 0)
		return -1;
	b->map = (void *)syscall(SYS_mmap, NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, f.fd, 0);
	if (b->map == MAP_FAILED) {
		syscall(SYS_close, f.fd);
		return -1;
	}
	b->fd = f.fd;
	b->size = len;
	return 0;
}

static void ion_free(struct ionbuf *b)
{
	if (b->size) {
		munmap(b->map, b->size);
		syscall(SYS_close, b->fd);
	}
	memset(b, 0, sizeof(*b));
}

static void free_bufs(struct q1dec *e, int q)
{
	for (uint32_t i = 0; i <= MAXBUF; i++) {
		ion_free(&e->b[q][i]);
		e->indrv[q][i] = false;
	}
	if (q == CAP)
		for (uint32_t i = 0; i < MAXBUF; i++)
			ion_free(&e->x[i]);
	e->count[q] = 0;
}

void q1dec_forget(struct q1dec *e)
{
	pthread_mutex_lock(&lock);
	for (int i = 0; i < MAXDEV; i++)
		if (decs[i] == e)
			__atomic_store_n(&decs[i], NULL, __ATOMIC_RELEASE);
	pthread_mutex_unlock(&lock);
	if (e->frames)
		logf_("closed: %llu frames", (unsigned long long)e->frames);
	free_bufs(e, OUT);
	free_bufs(e, CAP);
	if (e->ion >= 0)
		syscall(SYS_close, e->ion);
	pthread_mutex_destroy(&e->mu);
	free(e);
}

// --- formats

// the decoded size, venus's layout of it (until the driver says its own) and the picture inside
static void set_dims(struct q1dec *e, uint32_t w, uint32_t h)
{
	uint32_t hw = e->out_set ? e->out_fmt.fmt.pix_mp.width : 0, hh = e->out_set ? e->out_fmt.fmt.pix_mp.height : 0;
	e->cw = w;
	e->ch = h;
	e->stride = AL(w, 128);
	e->scan = AL(h, 32);
	e->cap_size = AL(e->stride * e->scan + e->stride * AL((h + 1) / 2, 16) + PAGE, PAGE);
	bool hint = hw && hh && AL(hw, 16) == w && AL(hh, 16) == h;
	e->crop_w = hint ? hw : w;
	e->crop_h = hint ? hh : h;
	e->cap_dirty = true;
}

// CAPTURE's format in the driver: only with no buffers on that queue
static int apply_cap_fmt(struct q1dec *e)
{
	struct v4l2_format f = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
	f.fmt.pix_mp.width = e->cw;
	f.fmt.pix_mp.height = e->ch;
	f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	if (sys_ioctl(e->fd, VIDIOC_S_FMT, &f) < 0 || sys_ioctl(e->fd, VIDIOC_G_FMT, &f) < 0) {
		int err = errno;
		logf_("S_FMT CAPTURE NV12 %ux%u: %s", e->cw, e->ch, strerror(err));
		return fail(err);
	}
	e->cap_planes = f.fmt.pix_mp.num_planes > 1 ? 2 : 1;
	e->stride = f.fmt.pix_mp.plane_fmt[0].bytesperline;
	e->scan = f.fmt.pix_mp.plane_fmt[0].reserved[0];
	e->cap_size = AL(f.fmt.pix_mp.plane_fmt[0].sizeimage, PAGE);
	e->extra_size = e->cap_planes > 1 ? AL(f.fmt.pix_mp.plane_fmt[1].sizeimage, PAGE) : 0;
	e->cap_dirty = false;
	return 0;
}

// CAPTURE's format as the client sees it
static void fill_cap_fmt(struct q1dec *e, struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *p = &f->fmt.pix_mp;
	memset(p, 0, sizeof(*p));
	p->width = e->cw;
	p->height = e->scan; // the UV rows start at bytesperline * this
	p->pixelformat = V4L2_PIX_FMT_NV12;
	p->field = V4L2_FIELD_NONE;
	p->num_planes = 1;
	p->plane_fmt[0].bytesperline = e->stride;
	p->plane_fmt[0].sizeimage = e->cap_size;
}

static void set_up(struct q1dec *e)
{
	static const uint32_t evs[] = {EV_FLUSH_DONE, EV_SETTINGS_SUFFICIENT, EV_SETTINGS_INSUFFICIENT,
	                               EV_BITDEPTH_INSUFFICIENT, EV_SYS_ERROR, EV_HW_OVERLOAD, EV_HW_UNSUPPORTED};
	if (e->set_up)
		return;
	e->set_up = true;
	for (size_t i = 0; i < sizeof(evs) / sizeof(evs[0]); i++) {
		struct v4l2_event_subscription s = {.type = evs[i]};
		if (sys_ioctl(e->fd, VIDIOC_SUBSCRIBE_EVENT, &s) < 0)
			logf_("SUBSCRIBE_EVENT 0x%x: %s", evs[i], strerror(errno));
	}
	e->ion = open("/dev/ion", O_RDONLY | O_CLOEXEC);
	if (e->ion < 0)
		logf_("/dev/ion: %s (the decoder's buffers cannot be made)", strerror(errno));
	if (!e->cw)
		set_dims(e, 1920, 1088);
}

static bool codec_listed(struct q1dec *e, uint32_t pix)
{
	struct v4l2_fmtdesc d = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE};
	for (; sys_ioctl(e->fd, VIDIOC_ENUM_FMT, &d) == 0; d.index++)
		if (d.pixelformat == pix)
			return true;
	return false;
}

static int s_fmt(struct q1dec *e, struct v4l2_format *f, bool try)
{
	int q = qof(f->type);
	struct v4l2_pix_format_mplane *p = &f->fmt.pix_mp;
	if (q == CAP) {
		if (try && p->pixelformat != V4L2_PIX_FMT_NV12)
			return fail(EINVAL);
		if (!try) {
			set_up(e);
			if (!e->out_set && p->width && p->height)
				set_dims(e, AL(p->width, 16), AL(p->height, 16));
			if (e->cap_dirty && !e->count[CAP] && !e->on[CAP])
				apply_cap_fmt(e); // else at REQBUFS
		} else if (!e->cw) {
			set_dims(e, AL(p->width ? p->width : 1920, 16), AL(p->height ? p->height : 1080, 16));
		}
		fill_cap_fmt(e, f);
		return 0;
	}
	if (q != OUT)
		return sys_ioctl(e->fd, try ? VIDIOC_TRY_FMT : VIDIOC_S_FMT, f);
	if (try) {
		if (!codec_listed(e, p->pixelformat))
			return fail(EINVAL);
		p->num_planes = 1;
		if (p->plane_fmt[0].sizeimage < (1u << 20))
			p->plane_fmt[0].sizeimage = 1u << 20;
		return 0;
	}
	set_up(e);
	struct v4l2_format d = *f;
	if (!d.fmt.pix_mp.width || !d.fmt.pix_mp.height) {
		d.fmt.pix_mp.width = 1920;
		d.fmt.pix_mp.height = 1080;
	}
	d.fmt.pix_mp.num_planes = 1;
	// A packet buffer: the driver wants 12 MB each whatever the stream (its largest picture, 3840x2160 * 3/2), and
	// libavcodec asks for 16 of them. Limited to half a raw frame of this stream (what libavcodec itself asks for).
	uint32_t limit = d.fmt.pix_mp.width * d.fmt.pix_mp.height * 3 / 4;
	struct v4l2_control lc = {.id = CID_BUFFER_SIZE_LIMIT, .value = (int32_t)AL(limit < (1u << 19) ? 1u << 19 : limit, PAGE)};
	// (S_FMT works the size out: once for the codec, which opens the firmware session the control needs, and once
	// more with the limit)
	d.fmt.pix_mp.plane_fmt[0].sizeimage = 0;
	if (sys_ioctl(e->fd, VIDIOC_S_FMT, &d) < 0 || sys_ioctl(e->fd, VIDIOC_S_CTRL, &lc) < 0 ||
	    sys_ioctl(e->fd, VIDIOC_S_FMT, &d) < 0 || sys_ioctl(e->fd, VIDIOC_G_FMT, &d) < 0) {
		int err = errno;
		char s[5];
		logf_("S_FMT OUTPUT %s %ux%u: %s", fourcc(p->pixelformat, s), p->width, p->height, strerror(err));
		return fail(err);
	}
	e->in_size = AL(d.fmt.pix_mp.plane_fmt[0].sizeimage, PAGE);
	p->num_planes = 1;
	p->field = V4L2_FIELD_NONE;
	memset(&p->plane_fmt[0], 0, sizeof(p->plane_fmt[0]));
	p->plane_fmt[0].sizeimage = e->in_size;
	e->out_fmt = *f;
	e->out_set = true;
	set_dims(e, AL(d.fmt.pix_mp.width, 16), AL(d.fmt.pix_mp.height, 16));
	if (!e->count[CAP] && !e->on[CAP])
		apply_cap_fmt(e);
	return 0;
}

static int g_fmt(struct q1dec *e, struct v4l2_format *f)
{
	int q = qof(f->type);
	if (q == CAP) {
		set_up(e);
		fill_cap_fmt(e, f);
		return 0;
	}
	if (q == OUT && e->out_set) {
		*f = e->out_fmt;
		return 0;
	}
	return sys_ioctl(e->fd, VIDIOC_G_FMT, f);
}

static int enum_fmt(struct q1dec *e, struct v4l2_fmtdesc *d)
{
	if (qof(d->type) != CAP)
		return sys_ioctl(e->fd, VIDIOC_ENUM_FMT, d);
	if (d->index)
		return fail(EINVAL);
	d->flags = 0;
	d->pixelformat = V4L2_PIX_FMT_NV12;
	snprintf((char *)d->description, sizeof(d->description), "Y/CbCr 4:2:0");
	memset(d->reserved, 0, sizeof(d->reserved));
	return 0;
}

static int selection(struct q1dec *e, struct v4l2_selection *s)
{
	if (s->type != V4L2_BUF_TYPE_VIDEO_CAPTURE && s->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		return fail(EINVAL);
	set_up(e);
	s->r.left = s->r.top = 0;
	s->r.width = e->crop_w;
	s->r.height = e->crop_h;
	return 0;
}

// --- the driver's queues

static int drv_qbuf(struct q1dec *e, int q, uint32_t i, const struct v4l2_buffer *from, uint32_t flags)
{
	struct v4l2_plane p[2];
	struct v4l2_buffer d;
	memset(p, 0, sizeof(p));
	memset(&d, 0, sizeof(d));
	d.type = q == OUT ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	d.memory = V4L2_MEMORY_USERPTR;
	d.index = i;
	d.flags = flags;
	d.m.planes = p;
	d.length = q == CAP ? e->cap_planes : 1;
	p[0].m.userptr = (unsigned long)e->b[q][i].map;
	p[0].length = (uint32_t)e->b[q][i].size;
	p[0].reserved[0] = (uint32_t)e->b[q][i].fd;
	if (from) { // a packet
		d.timestamp = from->timestamp;
		p[0].bytesused = from->m.planes[0].bytesused;
		p[0].data_offset = from->m.planes[0].data_offset;
	}
	if (q == CAP && e->cap_planes > 1) {
		p[1].m.userptr = (unsigned long)e->x[i].map;
		p[1].length = (uint32_t)e->x[i].size;
		p[1].reserved[0] = (uint32_t)e->x[i].fd;
	}
	if (sys_ioctl(e->fd, VIDIOC_QBUF, &d) < 0) {
		int err = errno;
		logf_("QBUF %s %u: %s", q == OUT ? "OUTPUT" : "CAPTURE", i, strerror(err));
		return fail(err);
	}
	e->indrv[q][i] = true;
	return 0;
}

static int drv_dqbuf(struct q1dec *e, int q, struct v4l2_buffer *d, struct v4l2_plane *p)
{
	memset(p, 0, 2 * sizeof(*p));
	memset(d, 0, sizeof(*d));
	d->type = q == OUT ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	d->memory = V4L2_MEMORY_USERPTR;
	d->m.planes = p;
	d->length = q == CAP ? e->cap_planes : 1;
	return sys_ioctl(e->fd, VIDIOC_DQBUF, d);
}

static void dead(struct q1dec *e, const char *why)
{
	if (!e->dead)
		logf_("%s: the stream stops here", why);
	e->dead = true;
}

// Flush CAPTURE and take every buffer back (the firmware must hold none at STREAMOFF). Frames that were decoded
// before the flush are kept for the client when asked.
static int drain_capture(struct q1dec *e, bool keep)
{
	struct v4l2_decoder_cmd dc = {.cmd = DEC_QCOM_CMD_FLUSH, .flags = QCOM_CMD_FLUSH_CAPTURE};
	if (sys_ioctl(e->fd, VIDIOC_DECODER_CMD, &dc) < 0) {
		logf_("flush: %s", strerror(errno));
		return -1;
	}
	struct timespec t0, t;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (bool done = false; !done;) {
		clock_gettime(CLOCK_MONOTONIC, &t);
		int left = 2000 - (int)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
		struct pollfd pf = {.fd = e->fd, .events = POLLPRI};
		if (left <= 0 || poll(&pf, 1, left) <= 0) {
			logf_("no flush-done event after 2 s");
			return -1;
		}
		struct v4l2_event ev;
		while (!done && sys_ioctl(e->fd, VIDIOC_DQEVENT, &ev) == 0) {
			if (ev.type == EV_FLUSH_DONE) {
				done = true;
			} else if (ev.type == EV_SYS_ERROR || ev.type == EV_HW_UNSUPPORTED || ev.type == EV_HW_OVERLOAD) {
				return -1;
			} else if (ev.type == EV_SETTINGS_INSUFFICIENT || ev.type == EV_BITDEPTH_INSUFFICIENT ||
			           ev.type == EV_SETTINGS_SUFFICIENT) {
				e->late = ev;
				e->has_late = true;
			}
		}
	}
	struct v4l2_buffer d;
	struct v4l2_plane p[2];
	while (drv_dqbuf(e, CAP, &d, p) == 0) {
		if (d.index >= MAXBUF)
			continue;
		e->indrv[CAP][d.index] = false;
		if (keep && p[0].bytesused && !(d.flags & QCOM_BUF_FLAG_EOS) && e->fifo_n < MAXBUF) {
			e->fifo[e->fifo_n] = d;
			e->fifo_used[e->fifo_n++] = p[0].bytesused;
		}
	}
	return 0;
}

static uint32_t min_capture(struct q1dec *e)
{
	struct v4l2_control mc = {.id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE};
	return sys_ioctl(e->fd, VIDIOC_G_CTRL, &mc) == 0 && mc.value > 0 ? (uint32_t)mc.value : 6;
}

// The firmware's "insufficient" with the size the client already has: the driver's reconfiguration, done with
// the client's buffers, which it does not see.
static int reconfigure(struct q1dec *e)
{
	bool was[MAXBUF];
	memcpy(was, e->indrv[CAP], sizeof(was));
	if (drain_capture(e, true) < 0)
		return -1;
	for (uint32_t k = 0; k < e->fifo_n; k++)
		was[e->fifo[k].index] = false; // a frame for the client, which queues the buffer again itself
	int t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	struct v4l2_requestbuffers rb = {.count = 0, .type = t, .memory = V4L2_MEMORY_USERPTR};
	if (sys_ioctl(e->fd, VIDIOC_STREAMOFF, &t) < 0 || sys_ioctl(e->fd, VIDIOC_REQBUFS, &rb) < 0) {
		logf_("reconfiguration, STREAMOFF/REQBUFS 0: %s", strerror(errno));
		return -1;
	}
	e->on[CAP] = false;
	if (apply_cap_fmt(e) < 0)
		return -1;
	uint32_t min = min_capture(e);
	if (e->cap_size > e->b[CAP][0].size || (e->cap_planes > 1 && e->extra_size > e->x[0].size) || min > e->count[CAP]) {
		logf_("the stream needs %u buffers of %u bytes, the client has %u of %zu", min, e->cap_size, e->count[CAP],
		      e->b[CAP][0].size);
		return -1;
	}
	rb.count = e->count[CAP];
	if (sys_ioctl(e->fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < e->count[CAP]) {
		logf_("reconfiguration, REQBUFS %u: %s", e->count[CAP], strerror(errno));
		return -1;
	}
	for (uint32_t i = 0; i < e->count[CAP]; i++)
		if (was[i] && drv_qbuf(e, CAP, i, NULL, 0) < 0)
			return -1;
	if (sys_ioctl(e->fd, VIDIOC_STREAMON, &t) < 0) {
		logf_("reconfiguration, STREAMON: %s", strerror(errno));
		return -1;
	}
	e->on[CAP] = true;
	return 0;
}

// a "port settings changed" event of the driver
static void seq_changed(struct q1dec *e, const struct v4l2_event *ev)
{
	const uint32_t *d = (const uint32_t *)ev->u.data;
	uint32_t h = d[0], w = d[1];
	bool insufficient = ev->type != EV_SETTINGS_SUFFICIENT;
	if (d[3] > 0) // HAL_COLOR_FORMAT 8-bit is 0
		logf_("%u-bit stream: only 8-bit NV12 comes out of this", d[3] == 1 ? 10 : d[3]);
	if (w && h && (w != e->cw || h != e->ch)) {
		logf_("the stream is %ux%u, not %ux%u: new CAPTURE format", w, h, e->cw, e->ch);
		if (e->on[CAP]) {
			if (drain_capture(e, true) < 0) {
				dead(e, "flush failed");
				return;
			}
			e->reconfig = true;
		}
		set_dims(e, w, h);
	} else if (insufficient && e->on[CAP]) {
		if (reconfigure(e) < 0) {
			dead(e, "reconfiguration failed");
			return;
		}
	}
	e->pending_src++;
}

// the driver's events, handled; true if there was any
static bool pump(struct q1dec *e)
{
	struct v4l2_event ev;
	bool any = false;
	for (;;) {
		if (e->has_late) {
			ev = e->late;
			e->has_late = false;
		} else if (e->dead || sys_ioctl(e->fd, VIDIOC_DQEVENT, &ev) < 0) {
			break;
		}
		any = true;
		if (ev.type == EV_SETTINGS_INSUFFICIENT || ev.type == EV_BITDEPTH_INSUFFICIENT ||
		    ev.type == EV_SETTINGS_SUFFICIENT)
			seq_changed(e, &ev);
		else if (ev.type == EV_SYS_ERROR)
			dead(e, "the firmware reported a system error");
		else if (ev.type == EV_HW_UNSUPPORTED)
			dead(e, "stream not supported by the hardware");
		else if (ev.type == EV_HW_OVERLOAD)
			dead(e, "the video hardware is overloaded");
	}
	return any;
}

static int dqevent(struct q1dec *e, struct v4l2_event *ev)
{
	pthread_mutex_lock(&e->mu);
	bool any = pump(e);
	if (!e->sub_src)
		e->pending_src = 0;
	else if (any && !e->pending_src)
		e->pending_src = 1; // the driver's event woke the client: it gets one that changes nothing
	if (!e->pending_src) {
		pthread_mutex_unlock(&e->mu);
		return fail(ENOENT);
	}
	memset(ev, 0, sizeof(*ev));
	ev->type = V4L2_EVENT_SOURCE_CHANGE;
	ev->u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION;
	ev->pending = --e->pending_src;
	ev->sequence = e->ev_seq++;
	clock_gettime(CLOCK_MONOTONIC, &ev->timestamp);
	pthread_mutex_unlock(&e->mu);
	return 0;
}

static int subscribe(struct q1dec *e, unsigned long req, struct v4l2_event_subscription *s)
{
	if (s->type == V4L2_EVENT_SOURCE_CHANGE) {
		e->sub_src = req == VIDIOC_SUBSCRIBE_EVENT;
		return 0;
	}
	if (s->type == V4L2_EVENT_EOS) // the end is the LAST buffer
		return 0;
	return sys_ioctl(e->fd, req, s);
}

// --- buffers

static int stream(struct q1dec *e, unsigned long req, int q);

static int reqbufs(struct q1dec *e, struct v4l2_requestbuffers *rb)
{
	int q = qof(rb->type);
	if (q < 0 || rb->memory != V4L2_MEMORY_MMAP)
		return sys_ioctl(e->fd, VIDIOC_REQBUFS, rb);
	set_up(e);
	if (e->on[q])
		stream(e, VIDIOC_STREAMOFF, q);
	struct v4l2_requestbuffers d = *rb;
	d.memory = V4L2_MEMORY_USERPTR;
	if (!rb->count) {
		int r = sys_ioctl(e->fd, VIDIOC_REQBUFS, &d);
		free_bufs(e, q);
		return r;
	}
	if (q == CAP) {
		if (e->cap_dirty && apply_cap_fmt(e) < 0)
			return -1;
		uint32_t min = min_capture(e) + 2; // two for the client to hold
		if (d.count < min)
			d.count = min;
		if (d.count > MAXBUF)
			d.count = MAXBUF;
	} else {
		if (!e->in_size)
			e->in_size = 1u << 20;
		if (d.count > MAXBUF)
			d.count = MAXBUF;
		d.count++; // the EOS carrier
	}
	if (sys_ioctl(e->fd, VIDIOC_REQBUFS, &d) < 0) {
		int err = errno;
		logf_("REQBUFS %s %u: %s", q == OUT ? "OUTPUT" : "CAPTURE", d.count, strerror(err));
		return fail(err);
	}
	free_bufs(e, q);
	uint32_t n = d.count > MAXBUF + (q == OUT) ? MAXBUF + (q == OUT) : d.count;
	for (uint32_t i = 0; i < n; i++)
		if (ion_alloc(e, q == CAP ? e->cap_size : e->in_size, &e->b[q][i]) < 0 ||
		    (q == CAP && e->cap_planes > 1 && ion_alloc(e, e->extra_size, &e->x[i]) < 0)) {
			logf_("ION buffer of %u bytes: %s", q == CAP ? e->cap_size : e->in_size, strerror(errno));
			free_bufs(e, q);
			d.count = 0;
			sys_ioctl(e->fd, VIDIOC_REQBUFS, &d);
			return fail(ENOMEM);
		}
	e->count[q] = q == OUT ? n - 1 : n;
	rb->count = e->count[q];
	char s[5];
	if (q == CAP)
		logf_("frames: NV12 %ux%u in %ux%u, stride %u, %u lines, %u buffers of %u kB", e->crop_w, e->crop_h, e->cw,
		      e->ch, e->stride, e->scan, e->count[q], e->cap_size / 1024);
	else
		logf_("bitstream: %s, %u buffers of %u kB", fourcc(e->out_fmt.fmt.pix_mp.pixelformat, s), e->count[q],
		      e->in_size / 1024);
	return 0;
}

static bool mine(struct q1dec *e, struct v4l2_buffer *b, int *q)
{
	*q = qof(b->type);
	return *q >= 0 && b->memory == V4L2_MEMORY_MMAP && b->index < e->count[*q] && b->length >= 1 && b->m.planes;
}

static int querybuf(struct q1dec *e, struct v4l2_buffer *b)
{
	int q;
	if (!mine(e, b, &q))
		return sys_ioctl(e->fd, VIDIOC_QUERYBUF, b);
	struct v4l2_plane *p = &b->m.planes[0];
	memset(p, 0, sizeof(*p));
	p->length = (uint32_t)e->b[q][b->index].size;
	p->m.mem_offset = COOKIE(q, b->index);
	b->length = 1;
	b->flags = V4L2_BUF_FLAG_TIMESTAMP_COPY | (e->indrv[q][b->index] ? V4L2_BUF_FLAG_QUEUED : 0);
	return 0;
}

void *q1dec_mmap(struct q1dec *e, void *addr, size_t len, int prot, int flags, off_t off)
{
	uint32_t c = (uint32_t)off;
	int q = (c >> 24) & 0x3f, i = (c >> 16) & 0xff;
	if ((c & 0xc000ffffu) != 0x40000000u || q > CAP || i >= (int)e->count[q] || len > e->b[q][i].size) {
		errno = EINVAL;
		return MAP_FAILED;
	}
	return (void *)syscall(SYS_mmap, addr, len, prot, (flags & ~MAP_PRIVATE) | MAP_SHARED, e->b[q][i].fd, 0);
}

static int qbuf(struct q1dec *e, struct v4l2_buffer *b)
{
	int q;
	if (!mine(e, b, &q))
		return sys_ioctl(e->fd, VIDIOC_QBUF, b);
	pthread_mutex_lock(&e->mu);
	if (!e->sub_src)
		pump(e);
	int r = 0;
	if (e->dead)
		r = fail(EIO);
	else if (q == OUT)
		r = drv_qbuf(e, OUT, b->index, b, 0);
	else if (!e->reconfig) // else the client is about to make new buffers
		r = drv_qbuf(e, CAP, b->index, NULL, 0);
	int err = errno;
	pthread_mutex_unlock(&e->mu);
	if (r < 0)
		return fail(err);
	b->flags = V4L2_BUF_FLAG_QUEUED | V4L2_BUF_FLAG_TIMESTAMP_COPY;
	return 0;
}

static void to_client(struct q1dec *e, int q, struct v4l2_buffer *b, const struct v4l2_buffer *d, uint32_t used)
{
	struct v4l2_plane *sp = b->m.planes;
	b->index = d->index;
	b->flags = (d->flags & (V4L2_BUF_FLAG_KEYFRAME | V4L2_BUF_FLAG_PFRAME | V4L2_BUF_FLAG_BFRAME)) |
	           V4L2_BUF_FLAG_TIMESTAMP_COPY;
	if (q == CAP && (d->flags & QCOM_BUF_FLAG_DATA_CORRUPT))
		b->flags |= V4L2_BUF_FLAG_ERROR;
	if (q == CAP && (d->flags & QCOM_BUF_FLAG_EOS))
		b->flags |= V4L2_BUF_FLAG_LAST;
	b->field = V4L2_FIELD_NONE;
	b->timestamp = d->timestamp;
	b->timecode = d->timecode;
	b->sequence = d->sequence;
	b->length = 1;
	memset(sp, 0, sizeof(*sp));
	sp->bytesused = used;
	sp->length = (uint32_t)e->b[q][d->index].size;
	sp->m.mem_offset = COOKIE(q, d->index);
}

static int dqbuf(struct q1dec *e, struct v4l2_buffer *b)
{
	int q = qof(b->type);
	if (q < 0 || b->memory != V4L2_MEMORY_MMAP || b->length < 1 || !b->m.planes)
		return sys_ioctl(e->fd, VIDIOC_DQBUF, b);
	pthread_mutex_lock(&e->mu);
	if (!e->sub_src)
		pump(e);
	int err = 0;
	if (e->dead) {
		err = EIO;
	} else if (q == CAP) {
		if (e->fifo_n) {
			to_client(e, q, b, &e->fifo[0], e->fifo_used[0]);
			e->fifo_n--;
			memmove(&e->fifo[0], &e->fifo[1], e->fifo_n * sizeof(e->fifo[0]));
			memmove(&e->fifo_used[0], &e->fifo_used[1], e->fifo_n * sizeof(e->fifo_used[0]));
			e->frames++;
			pthread_mutex_unlock(&e->mu);
			return 0;
		}
		if (e->last_seen)
			err = EPIPE;
		else if (e->reconfig)
			err = EAGAIN;
	}
	pthread_mutex_unlock(&e->mu);
	if (err)
		return fail(err);
	for (;;) {
		struct v4l2_buffer d;
		struct v4l2_plane p[2];
		if (drv_dqbuf(e, q, &d, p) < 0)
			return -1;
		pthread_mutex_lock(&e->mu);
		if (d.index > MAXBUF) {
			pthread_mutex_unlock(&e->mu);
			return fail(EINVAL);
		}
		e->indrv[q][d.index] = false;
		bool eos = q == CAP && (d.flags & QCOM_BUF_FLAG_EOS);
		if ((q == OUT && d.index >= e->count[OUT]) || // the EOS carrier
		    (q == CAP && !p[0].bytesused && !eos && drv_qbuf(e, CAP, d.index, NULL, 0) == 0)) { // nothing in it
			pthread_mutex_unlock(&e->mu);
			continue;
		}
		if (q == CAP) {
			if (p[0].bytesused && !e->frames++ && p[0].reserved[4] &&
			    (p[0].reserved[4] != e->crop_w || p[0].reserved[5] != e->crop_h))
				logf_("the firmware's picture is %ux%u, the client was told %ux%u", p[0].reserved[4], p[0].reserved[5],
				      e->crop_w, e->crop_h);
			if (eos)
				e->last_seen = true;
		}
		to_client(e, q, b, &d, q == CAP ? p[0].bytesused : 0);
		pthread_mutex_unlock(&e->mu);
		return 0;
	}
}

static int stream(struct q1dec *e, unsigned long req, int q)
{
	int t = q == OUT ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (req == VIDIOC_STREAMOFF && q == CAP && e->on[CAP]) {
		pump(e);
		// the firmware must hold no capture buffer at STREAMOFF while the stream runs
		if (!e->reconfig && e->on[OUT] && !e->dead && drain_capture(e, false) < 0)
			dead(e, "flush failed");
	}
	int r = sys_ioctl(e->fd, req, &t);
	if (r < 0) {
		int err = errno;
		logf_("%s %s: %s", req == VIDIOC_STREAMON ? "STREAMON" : "STREAMOFF", q == OUT ? "OUTPUT" : "CAPTURE",
		      strerror(err));
		errno = err;
		if (req == VIDIOC_STREAMON)
			return r;
	}
	e->on[q] = req == VIDIOC_STREAMON;
	if (req == VIDIOC_STREAMOFF) {
		memset(e->indrv[q], 0, sizeof(e->indrv[q]));
		if (q == CAP) {
			e->reconfig = false;
			e->fifo_n = 0;
			e->last_seen = false;
		} else {
			e->eos_sent = false;
		}
	}
	return r;
}

static int decoder_cmd(struct q1dec *e, unsigned long req, struct v4l2_decoder_cmd *c)
{
	if (c->cmd != V4L2_DEC_CMD_STOP && c->cmd != V4L2_DEC_CMD_START)
		return sys_ioctl(e->fd, req, c);
	if (req == VIDIOC_TRY_DECODER_CMD)
		return 0;
	int r = 0;
	pthread_mutex_lock(&e->mu);
	if (c->cmd == V4L2_DEC_CMD_START) {
		e->eos_sent = e->last_seen = false;
	} else if (!e->eos_sent) {
		// the end of the stream: an empty packet with the EOS flag; the frame buffer that comes back with it is
		// the last one
		if (e->on[OUT] && e->on[CAP] && !e->dead && e->b[OUT][e->count[OUT]].size)
			r = drv_qbuf(e, OUT, e->count[OUT], NULL, QCOM_BUF_FLAG_EOS);
		else
			e->last_seen = true;
		e->eos_sent = r == 0;
	}
	int err = errno;
	pthread_mutex_unlock(&e->mu);
	return r < 0 ? fail(err) : 0;
}

int q1dec_ioctl(struct q1dec *e, unsigned long req, void *arg)
{
	int r;
	switch (req) {
	case VIDIOC_QUERYCAP: {
		struct v4l2_capability *cap = arg;
		r = sys_ioctl(e->fd, req, arg);
		if (r == 0) {
			cap->device_caps |= V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
			cap->capabilities |= cap->device_caps | V4L2_CAP_DEVICE_CAPS;
		}
		return r;
	}
	case VIDIOC_ENUM_FMT:
		return enum_fmt(e, arg);
	case VIDIOC_G_FMT:
		return g_fmt(e, arg);
	case VIDIOC_S_FMT:
	case VIDIOC_TRY_FMT:
		pthread_mutex_lock(&e->mu);
		r = s_fmt(e, arg, req == VIDIOC_TRY_FMT);
		pthread_mutex_unlock(&e->mu);
		return r;
	case VIDIOC_G_SELECTION:
	case VIDIOC_S_SELECTION:
		return selection(e, arg);
	case VIDIOC_SUBSCRIBE_EVENT:
	case VIDIOC_UNSUBSCRIBE_EVENT:
		return subscribe(e, req, arg);
	case VIDIOC_DQEVENT:
		return dqevent(e, arg);
	case VIDIOC_REQBUFS:
		pthread_mutex_lock(&e->mu);
		r = reqbufs(e, arg);
		pthread_mutex_unlock(&e->mu);
		return r;
	case VIDIOC_QUERYBUF:
		return querybuf(e, arg);
	case VIDIOC_QBUF:
		return qbuf(e, arg);
	case VIDIOC_DQBUF:
		return dqbuf(e, arg);
	case VIDIOC_STREAMON:
	case VIDIOC_STREAMOFF: {
		int q = qof(*(uint32_t *)arg);
		if (q < 0)
			return sys_ioctl(e->fd, req, arg);
		pthread_mutex_lock(&e->mu);
		r = stream(e, req, q);
		pthread_mutex_unlock(&e->mu);
		return r;
	}
	case VIDIOC_DECODER_CMD:
	case VIDIOC_TRY_DECODER_CMD:
		return decoder_cmd(e, req, arg);
	}
	return sys_ioctl(e->fd, req, arg);
}
