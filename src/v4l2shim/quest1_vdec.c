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
//  - A client that sets CAPTURE up only at the first SOURCE_CHANGE (Steam Link VR's vrlink: OUTPUT, STREAMON
//    OUTPUT, packets, then an epoll wait for the event) would wait forever: the driver starts the firmware session
//    only once both queues stream. So CAPTURE is set up here with buffers of our own, of the OUTPUT size
//    (CAPTURE smaller than OUTPUT: the firmware calls the stream unsupported and dies, Err_Fatal
//    video_decoder_utils.c:3206): at once for a client that waits with epoll, after 50 ms of packets with no
//    CAPTURE for one that polls. The firmware's "insufficient" at the first sequence header tells the stream's
//    size, those buffers go back, and the client gets its SOURCE_CHANGE (boot_capture/boot_end).
//  - vrlink takes all its 16 packet buffers being held by the decoder at once as fatal ("Could not find a free
//    OUTPUT buffer", then it stops). The firmware holds every packet from the first sequence header until CAPTURE
//    is set up again, and every packet before it until the session runs: with 50 ms of wait, packets that came
//    before the I-frame vrlink asks for after a reset of its own, and the client's CAPTURE set-up, that was more
//    than 16 (2026-11-11). Hence CAPTURE at once, and packets before the stream's start (stream_start) given
//    back undecoded to such a client (the firmware drops them anyway).
//  - A client that waits with epoll: the driver's events are taken here, so the client's epoll set gets an
//    eventfd of ours, readable while a SOURCE_CHANGE (EPOLLPRI on the decoder for the client) or a packet buffer
//    given back here (EPOLLOUT) waits; the driver's own EPOLLPRI on the decoder is hidden from it, and its EPOLLIN
//    passes only with a frame for the client, taken from the driver first: vrlink stops decoding for good at a DQBUF
//    that finds nothing (q1dec_epoll_ctl/q1dec_epoll_events/frame_ready).
//  - A client size larger than venus decodes (more than 4096 on a side: vrlink says 1536x6144 for a 1024x4096
//    atlas) is a bound, not the stream's: a session that size fails at the STREAMON after the first
//    reconfiguration (EINVAL, 2026-11-11), so the driver gets the default size; the firmware reads the stream's.
//  - DEC_DISPLAY_DELAY_ENABLE with a delay of 0 (each frame out as soon as it is decoded: vrlink asks for it) is
//    the firmware's decode order, with realtime priority and a 90 fps operating rate: 1024x4096 HEVC at 72 fps
//    then takes 9.5 ms from packet to frame (51 fps flat out without them, 2026-11-11, vdectest).
//  - A 10-bit stream (the driver's "insufficient" says so) is decoded in split mode: 10-bit references, 8-bit NV12
//    frames, so the client sees NV12 as for any other stream (apply_cap_fmt).
// Not handled: USERPTR clients.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include "quest1_v4l2.h"

// --- msm_vidc's private V4L2 additions (kernel include/uapi/linux/{v4l2-controls,videodev2}.h)
#define VIDC_CID(n) ((V4L2_CTRL_CLASS_MPEG | 0x2000) + (n))
#define CID_CONTINUE_DATA VIDC_CID(5)       // 1: the driver continues by itself at a sufficient sequence
#define CID_OUTPUT_ORDER VIDC_CID(7)        // 0 display, 1 decode
#define CID_BUFFER_SIZE_LIMIT VIDC_CID(64)  // the largest packet, in bytes
#define CID_PRIORITY VIDC_CID(83)           // 0 realtime, 1 not
#define CID_OPERATING_RATE VIDC_CID(84)     // frames per second, Q16
#define CID_PERF_LEVEL VIDC_CID(26)         // 0 nominal, 2 turbo (the core's top clock for this session)
#define CID_OUTPUT_MODE VIDC_CID(38)        // 0 primary (the frames are the references), 1 secondary (split mode)
#define CID_DPB_FORMAT VIDC_CID(73)         // the reference pictures in split mode: 1 UBWC, 2 TP10 UBWC
#define BIT_DEPTH_10 1                      // MSM_VIDC_BIT_DEPTH_10, in a sequence event
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
#define QCOM_CMD_FLUSH_OUTPUT (1 << 0)
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
	dev_t rdev; // the device and node fd was opened on (see find() in quest1_v4l2.c)
	ino_t ino;
	bool stale; // fd was closed behind the shim: skipped, never freed by the lookup
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
	bool cap_dmabuf;     // the client brings the frame buffers itself, as dma-buf fds (Steam's stream client)
	int cap_cfd[MAXBUF]; // its fd of each, -1 before it queued one
	bool on[2];      // the driver's queues stream
	bool reconfig;   // CAPTURE was flushed for a new size: it waits for the client's STREAMOFF
	bool eos_sent, last_seen, dead;
	bool sub_src;
	pthread_t ev_thread; // takes the driver's events as they come
	bool ev_started, closing;
	uint64_t first_packet_ns; // when the stream's first packet was queued
	bool quiet_change;        // an event of the driver that changed nothing for the client was taken
	uint32_t pending_src, ev_seq;
	struct v4l2_event late; // a sequence event that came while a flush was awaited
	bool has_late;
	struct v4l2_buffer fifo[MAXBUF]; // frames taken back by a flush, not given to the client yet
	uint32_t fifo_used[MAXBUF], fifo_n;
	uint64_t frames;
	bool boot, booted; // CAPTURE is set up here until the stream's size is known (the client waits for it)
	bool boot_sized;   // ... and is at the stream's size now: the client's CAPTURE calls are kept until its STREAMON
	bool cap_pending;  // the client set its CAPTURE up meanwhile (DMABUF): pend_count buffers, the ones queued
	uint32_t pend_count;
	int pend_fd[MAXBUF];
	bool pend_q[MAXBUF];
	bool await_cap;    // the firmware waits for the client's CAPTURE (its "port reconfiguration")
	bool early;        // the client was given the size of the stream before (known_*) and sets its CAPTURE up
	uint64_t seq_ns;   // when the stream's first sequence came (for a client that waits with epoll)
	bool stall_logged;
	bool low_latency;  // frames in decode order, realtime priority (the client's DISPLAY_DELAY 0)
	uint64_t clock_ns; // when the turbo clock was last asked for again (keep_clock)
	bool ten_bit;      // the stream is 10-bit: split mode, 8-bit NV12 frames of 10-bit references (split: set)
	bool split;
	int evfd;          // in the client's epoll set: readable while a SOURCE_CHANGE or a skipped packet waits; -1 if none
	bool ev_ready;     // it is readable
	int epfd;          // that epoll set, -1 if none
	uint64_t epdata;   // the client's data for the decoder in it
	bool start_seen;   // a packet decoding can start at was queued (skipped_*: the ones before it, given back)
	uint32_t skipped[MAXBUF + 1], skipped_n, skipped_all;
	struct timeval skipped_ts[MAXBUF + 1];
};

#define BOOT_AFTER_NS 50000000ull
#define CLIENT_CAPTURE 9 // frame buffers for a client that waits with epoll: the firmware's 6, and 3 for it to hold
#define SKIP_MAX 144 // packets given back while no stream start came: 2 s at 72 fps, then they all go to the driver
#define LOW_LATENCY_FPS 90
#define PACKET_MAX (4u << 20)
#define VENUS_MAX_SIDE 4096u
#define EP_TAG 0x7131646563000000ull // our eventfd's data in a client's epoll set, | the decoder's slot

static struct q1dec *decs[MAXDEV];
static uint32_t known_w, known_h, known_pix; // the size of the last stream in this process, and its codec

static void stream_size(struct q1dec *e, uint32_t w, uint32_t h)
{
	known_w = w;
	known_h = h;
	known_pix = e->out_fmt.fmt.pix_mp.pixelformat;
}
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

// after a check that fd is still the decoder's device (find() in quest1_v4l2.c says why)
struct q1dec *q1dec_find(int fd)
{
	for (int i = 0; i < MAXDEV; i++) {
		struct q1dec *e = __atomic_load_n(&decs[i], __ATOMIC_ACQUIRE);
		if (e && e->fd == fd && !__atomic_load_n(&e->stale, __ATOMIC_ACQUIRE)) {
			struct stat st;
			if (fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == e->rdev && st.st_ino == e->ino)
				return e;
			if (!__atomic_exchange_n(&e->stale, true, __ATOMIC_ACQ_REL))
				logf_("fd %d is no longer the decoder (closed behind the shim): skipped from now on", fd);
		}
	}
	return NULL;
}

// Read by a writev of the samples into a pipe, not through the pointer: the decoder's thread may unmap a buffer at a
// reconfiguration, and the kernel's copy then gives an error instead of a crash (which would take Steam down with
// vrlink; process_vm_readv cannot read these mappings at all: EFAULT). The decoder itself stays allocated while
// `lock` is held (q1dec_forget takes it first).
bool q1dec_client_frame_sample(unsigned index, unsigned mean[4], unsigned *peak, unsigned chroma[2])
{
	enum { ROWS = 8, COLS = 16, UVROWS = 4, UVCOLS = 8 };
	static struct {
		uint8_t y[4][ROWS][COLS];
		uint8_t uv[4][UVROWS][UVCOLS][2]; // NV12: U then V, at half the size, after stride x scan bytes of Y
	} px;
	static int pipefd[2] = {-1, -1};
	struct iovec remote[4 * ROWS * COLS + 4 * UVROWS * UVCOLS];
	bool ok = false;
	pthread_mutex_lock(&lock);
	if (pipefd[0] < 0 && pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) < 0) {
		pipefd[0] = -1;
		pthread_mutex_unlock(&lock);
		return false;
	}
	for (int d = 0; d < MAXDEV && !ok; d++) {
		struct q1dec *e = decs[d];
		if (!e || !e->cap_dmabuf || index >= e->count[CAP] || index >= MAXBUF || !e->b[CAP][index].map ||
		    !e->stride || e->ch < 4 * ROWS)
			continue;
		const struct ionbuf *b = &e->b[CAP][index];
		size_t n = 0;
		for (unsigned q = 0; q < 4; q++)
			for (unsigned r = 0; r < ROWS; r++)
				for (unsigned c = 0; c < COLS; c++) {
					size_t row = q * (e->ch / 4) + (2 * r + 1) * (e->ch / 4) / (2 * ROWS);
					size_t off = row * e->stride + (2 * c + 1) * e->cw / (2 * COLS);
					if (off >= b->size)
						off = 0;
					remote[n].iov_base = b->map + off;
					remote[n++].iov_len = 1;
				}
		for (unsigned q = 0; q < 4; q++)
			for (unsigned r = 0; r < UVROWS; r++)
				for (unsigned c = 0; c < UVCOLS; c++) {
					size_t row = (q * (e->ch / 4) + (2 * r + 1) * (e->ch / 4) / (2 * UVROWS)) / 2;
					size_t off = (size_t)e->stride * e->scan + row * e->stride +
					             (((2 * c + 1) * e->cw / (2 * UVCOLS)) & ~1u);
					if (off + 2 > b->size)
						off = 0;
					remote[n].iov_base = b->map + off;
					remote[n++].iov_len = 2;
				}
		struct dma_buf_sync sy = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
		syscall(SYS_ioctl, b->fd, DMA_BUF_IOCTL_SYNC, &sy);
		ssize_t got = writev(pipefd[1], remote, (int)n);
		ok = got == (ssize_t)sizeof(px);
		while (got > 0) { // empty the pipe whatever came
			ssize_t r = read(pipefd[0], &px, sizeof(px));
			if (r <= 0)
				break;
			got -= r;
		}
		sy.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
		syscall(SYS_ioctl, b->fd, DMA_BUF_IOCTL_SYNC, &sy);
	}
	pthread_mutex_unlock(&lock);
	if (!ok)
		return false;
	*peak = 0;
	for (unsigned q = 0; q < 4; q++) {
		unsigned sum = 0;
		for (unsigned r = 0; r < ROWS; r++)
			for (unsigned c = 0; c < COLS; c++) {
				sum += px.y[q][r][c];
				if (px.y[q][r][c] > *peak)
					*peak = px.y[q][r][c];
			}
		mean[q] = sum / (ROWS * COLS);
	}
	unsigned su = 0, sv = 0;
	for (unsigned q = 0; q < 4; q++)
		for (unsigned r = 0; r < UVROWS; r++)
			for (unsigned c = 0; c < UVCOLS; c++) {
				su += px.uv[q][r][c][0];
				sv += px.uv[q][r][c][1];
			}
	chroma[0] = su / (4 * UVROWS * UVCOLS);
	chroma[1] = sv / (4 * UVROWS * UVCOLS);
	return true;
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
	e->rdev = st.st_rdev;
	e->ino = st.st_ino;
	e->ion = e->evfd = e->epfd = -1;
	pthread_mutex_init(&e->mu, NULL);
	pthread_mutex_lock(&lock);
	int i = 0; // a free slot, or a stale entry's (left allocated: it may still be in use, see find())
	while (i < MAXDEV && decs[i] && !__atomic_load_n(&decs[i]->stale, __ATOMIC_ACQUIRE))
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

// Packet buffers of a closed decoder, for the next one of this process: vrlink opens a new decoder at each reset of
// its own and must have its first frame 800 ms after it; making its 17 buffers of 4 MB took 140 ms (2026-11-11).
static struct ionbuf out_cache[MAXBUF + 1];
static pthread_mutex_t out_cache_mu = PTHREAD_MUTEX_INITIALIZER;

static int out_alloc(struct q1dec *e, size_t len, struct ionbuf *b)
{
	bool got = false;
	pthread_mutex_lock(&out_cache_mu);
	for (uint32_t i = 0; i <= MAXBUF && !got; i++)
		if (out_cache[i].size == AL(len, PAGE)) {
			*b = out_cache[i];
			memset(&out_cache[i], 0, sizeof(out_cache[i]));
			got = true;
		}
	pthread_mutex_unlock(&out_cache_mu);
	return got ? 0 : ion_alloc(e, len, b);
}

static void out_keep(struct q1dec *e)
{
	pthread_mutex_lock(&out_cache_mu);
	for (uint32_t i = 0, k = 0; i <= MAXBUF; i++) {
		if (!e->b[OUT][i].size)
			continue;
		while (k <= MAXBUF && out_cache[k].size)
			k++;
		if (k > MAXBUF)
			break;
		out_cache[k] = e->b[OUT][i];
		memset(&e->b[OUT][i], 0, sizeof(e->b[OUT][i]));
	}
	pthread_mutex_unlock(&out_cache_mu);
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
	q1dec_closing(e);
	if (e->frames)
		logf_("closed: %llu frames", (unsigned long long)e->frames);
	out_keep(e);
	free_bufs(e, OUT);
	free_bufs(e, CAP);
	if (e->ion >= 0)
		syscall(SYS_close, e->ion);
	if (e->evfd >= 0)
		syscall(SYS_close, e->evfd); // which takes it out of the client's epoll set
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
	if (e->ten_bit && !e->split) {
		// A 10-bit stream (HEVC Main10: Steam Link VR's default for a Frame): venus keeps its reference pictures in
		// its 10-bit format (TP10 UBWC, buffers the driver makes) and writes each frame as an 8-bit NV12 copy into
		// CAPTURE, as Qualcomm's own decoder does for a client that reads frames. Set at a reconfiguration, when
		// CAPTURE has no buffers. The driver makes as many references (9.3 MB each at 1024x4096) as CAPTURE had
		// frame buffers before.
		struct v4l2_ext_control x[2] = {{.id = CID_OUTPUT_MODE, .value = 1}, {.id = CID_DPB_FORMAT, .value = 2}};
		struct v4l2_ext_controls xs = {.ctrl_class = V4L2_CTRL_CLASS_MPEG, .count = 2, .controls = x};
		if (sys_ioctl(e->fd, VIDIOC_S_EXT_CTRLS, &xs) < 0) {
			int err = errno;
			logf_("10-bit stream, split mode: %s", strerror(err));
			return fail(err);
		}
		e->split = true;
		logf_("10-bit stream: references in TP10 UBWC, frames in 8-bit NV12");
	}
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

static void *ev_loop(void *arg);

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
	e->ev_started = pthread_create(&e->ev_thread, NULL, ev_loop, e) == 0;
}

static bool pump(struct q1dec *e);
static void boot_check(struct q1dec *e);
static void boot_recycle(struct q1dec *e);
static void leave_reconfig(struct q1dec *e);
static int reqbufs(struct q1dec *e, struct v4l2_requestbuffers *rb);
static int adopt_dmabuf(struct q1dec *e, uint32_t i, int fd);
static int stream(struct q1dec *e, unsigned long req, int q);

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// The driver's events are taken here as they come, whatever the client is doing: the first packet of a stream
// always brings the firmware's "insufficient", and nothing is decoded until CAPTURE has been set up again
// (reconfigure()). A client that only looks at its frames would wait for nothing.
static void *ev_loop(void *arg)
{
	struct q1dec *e = arg;
	while (!e->closing) {
		struct pollfd pf = {.fd = e->fd, .events = POLLPRI | (__atomic_load_n(&e->boot, __ATOMIC_RELAXED) ? POLLIN : 0)};
		int n = q1_sys_poll(&pf, 1, 100);
		if (e->closing || (pf.revents & POLLNVAL))
			break;
		if (n > 0 && (pf.revents & (POLLPRI | POLLIN))) {
			pthread_mutex_lock(&e->mu);
			pump(e);
			if (e->boot)
				boot_recycle(e);
			pthread_mutex_unlock(&e->mu);
		} else if (n > 0) {
			usleep(5000); // the driver says POLLERR while a queue has no buffer: not an event
		}
		if (e->seq_ns && !e->frames && !e->stall_logged && now_ns() - e->seq_ns > 250000000ull) {
			pthread_mutex_lock(&e->mu);
			unsigned o = 0, c = 0;
			for (uint32_t i = 0; i <= MAXBUF; i++) {
				o += e->indrv[OUT][i];
				c += i < MAXBUF && e->indrv[CAP][i];
			}
			e->stall_logged = true;
			logf_("no frame 250 ms after the stream's first sequence: the driver holds %u packets and %u of %u frame "
			      "buffers, %u packets given back", o, c, e->count[CAP], e->skipped_n);
			pthread_mutex_unlock(&e->mu);
		}
		if (!e->booted && (e->first_packet_ns || e->epfd >= 0)) {
			pthread_mutex_lock(&e->mu);
			boot_check(e);
			pthread_mutex_unlock(&e->mu);
		}
	}
	return NULL;
}

// poll() of the client on the decoder alone. Steam's stream client gives the first frame 30 ms and starts the
// decoder all over when it has not come; the firmware's reconfiguration at the first packet takes longer than
// that. So the wait for the first frame goes on, up to a second after the first packet.
int q1dec_poll(struct q1dec *e, struct pollfd *fd, int timeout_ms)
{
	int r = q1_sys_poll(fd, 1, timeout_ms);
	while (r == 0 && timeout_ms >= 0 && (fd->events & POLLIN) && !e->frames && !e->dead && !e->closing &&
	       e->first_packet_ns && now_ns() - e->first_packet_ns < 1000000000ull)
		r = q1_sys_poll(fd, 1, 10);
	return r;
}

void q1dec_closing(struct q1dec *e)
{
	pthread_mutex_lock(&e->mu);
	leave_reconfig(e); // (the driver's close stops the session as STREAMOFF does)
	pthread_mutex_unlock(&e->mu);
	e->closing = true;
	if (e->ev_started) {
		pthread_join(e->ev_thread, NULL);
		e->ev_started = false;
	}
}

static bool codec_listed(struct q1dec *e, uint32_t pix)
{
	struct v4l2_fmtdesc d = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE};
	for (; sys_ioctl(e->fd, VIDIOC_ENUM_FMT, &d) == 0; d.index++)
		if (d.pixelformat == pix)
			return true;
	return false;
}

// each frame out as soon as it is decoded, with the clocks of a live stream (the session must be open: after the
// OUTPUT format)
static void set_low_latency(struct q1dec *e)
{
	static const struct {
		uint32_t id;
		int32_t v;
	} c[] = {{CID_OUTPUT_ORDER, 1}, {CID_PRIORITY, 0}, {CID_OPERATING_RATE, LOW_LATENCY_FPS << 16}, {CID_PERF_LEVEL, 2}};
	int32_t got[sizeof(c) / sizeof(c[0])];
	for (size_t i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
		struct v4l2_control k = {.id = c[i].id, .value = c[i].v};
		if (sys_ioctl(e->fd, VIDIOC_S_CTRL, &k) < 0)
			logf_("control 0x%x = %d (low latency): %s", c[i].id, c[i].v, strerror(errno));
		k.value = -1;
		got[i] = sys_ioctl(e->fd, VIDIOC_G_CTRL, &k) == 0 ? k.value : -1;
	}
	// read back: the driver's clock follows the operating rate and turbo (msm_comm_get_inst_load)
	logf_("low latency: output order %d (1 = decode), priority %d (0 = realtime), %d fps, perf level %d (2 = turbo)",
	      got[0], got[1], got[2] < 0 ? -1 : got[2] >> 16, got[3]);
}

// The turbo clock asked again every 2 s while a low-latency client takes frames. The driver sets the core's clock when
// a control or a session changes, and once that failed ("Failed to set clock rate: -19" as Steam Link VR's stream
// started) the core stayed at its lowest, 200 of 533 MHz, for the whole session: 74 ms from packet to frame instead of
// 14-17, every frame late, the PC holding its encoder back and the stream trembling (2026-11-17, SteamVR 2.19's vrlink
// at its first sessions). The driver sets the clock again in its handler of the perf level, but V4L2's control framework
// calls it only when the value changes: nominal then turbo (the clock goes to the load's rate for an instant, then
// back to turbo).
static void keep_clock(struct q1dec *e)
{
	if (!e->low_latency)
		return;
	uint64_t t = now_ns(), was = __atomic_load_n(&e->clock_ns, __ATOMIC_RELAXED);
	if (t - was < 2000000000ull || !__atomic_compare_exchange_n(&e->clock_ns, &was, t, false, __ATOMIC_RELAXED,
	                                                             __ATOMIC_RELAXED))
		return;
	struct v4l2_control k = {.id = CID_PERF_LEVEL, .value = 0};
	sys_ioctl(e->fd, VIDIOC_S_CTRL, &k);
	k.value = 2;
	sys_ioctl(e->fd, VIDIOC_S_CTRL, &k);
}

static int s_ctrl(struct q1dec *e, struct v4l2_control *c)
{
	if (c->id == V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE) {
		pthread_mutex_lock(&e->mu);
		e->low_latency = c->value != 0;
		logf_("the client asks for %s (DISPLAY_DELAY_ENABLE %d, OUTPUT format %s)",
		      e->low_latency ? "each frame at once" : "frames in display order", c->value, e->out_set ? "set" : "not set yet");
		if (e->low_latency && e->out_set)
			set_low_latency(e);
		pthread_mutex_unlock(&e->mu);
		return 0;
	}
	if (c->id == V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY) // decode order shows each frame at once: a delay of 0
		return c->value == 0 ? 0 : fail(EINVAL);
	return sys_ioctl(e->fd, VIDIOC_S_CTRL, c);
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
	// libavcodec asks for 16 of them. Limited to half a raw frame of this stream (what libavcodec itself asks for),
	// and to 4 MB (vrlink's own largest frame; it says 1536x6144 and asks for 16).
	uint32_t limit = d.fmt.pix_mp.width * d.fmt.pix_mp.height * 3 / 4;
	if (limit > PACKET_MAX)
		limit = PACKET_MAX;
	struct v4l2_control lc = {.id = CID_BUFFER_SIZE_LIMIT, .value = (int32_t)AL(limit < (1u << 19) ? 1u << 19 : limit, PAGE)};
	if (d.fmt.pix_mp.width > VENUS_MAX_SIDE || d.fmt.pix_mp.height > VENUS_MAX_SIDE) {
		// The firmware finds its resources sufficient at the first sequence only for the size given here: the size
		// of this client's last stream when there was one (vrlink's decoder again after a reset of its own)
		bool known = known_w && known_pix == p->pixelformat;
		logf_("%ux%u is larger than venus decodes: a bound, the stream's size comes from the stream (%ux%u until then)",
		      d.fmt.pix_mp.width, d.fmt.pix_mp.height, known ? known_w : 1920, known ? known_h : 1088);
		d.fmt.pix_mp.width = known ? known_w : 1920;
		d.fmt.pix_mp.height = known ? known_h : 1088;
	}
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
	if (e->low_latency)
		set_low_latency(e);
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

// The firmware's flush of the given queues, and its flush-done event awaited (2 s at most)
static int flush(struct q1dec *e, uint32_t queues)
{
	struct v4l2_decoder_cmd dc = {.cmd = DEC_QCOM_CMD_FLUSH, .flags = queues};
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
		if (left <= 0 || q1_sys_poll(&pf, 1, left) <= 0) {
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
	return 0;
}

// Flush CAPTURE and take every buffer back (the firmware must hold none at STREAMOFF). Frames that were decoded
// before the flush are kept for the client when asked.
static int drain_capture(struct q1dec *e, bool keep)
{
	if (flush(e, QCOM_CMD_FLUSH_CAPTURE) < 0)
		return -1;
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
	int rr = sys_ioctl(e->fd, VIDIOC_REQBUFS, &rb);
	if (rr == 0 && rb.count != e->count[CAP])
		logf_("reconfiguration: the driver wants %u frame buffers, the client has %u", rb.count, e->count[CAP]);
	if (rr < 0 || rb.count < e->count[CAP]) {
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

// our eventfd in the client's epoll set is readable while a SOURCE_CHANGE or a skipped packet waits for it
static void ev_sync(struct q1dec *e)
{
	bool want = e->pending_src || e->skipped_n;
	if (e->evfd < 0 || want == e->ev_ready)
		return;
	uint64_t v = 1;
	if (want ? write(e->evfd, &v, sizeof(v)) == sizeof(v) : (read(e->evfd, &v, sizeof(v)) == sizeof(v) || errno == EAGAIN))
		e->ev_ready = want;
	else
		logf_("eventfd: %s", strerror(errno));
}

// a SOURCE_CHANGE for the client
static void src_pending(struct q1dec *e)
{
	e->pending_src++;
	ev_sync(e);
}

static void src_taken(struct q1dec *e)
{
	ev_sync(e);
}

// Whether decoding can start at a packet: parameter sets or an IDR/CRA/BLA picture before its first other picture
// (H.264, HEVC; other codecs and packets with no start code: yes). The firmware drops the pictures before such a
// packet anyway, but holds their buffers meanwhile.
static bool stream_start(uint32_t codec, const uint8_t *p, size_t n)
{
	bool hevc = codec == V4L2_PIX_FMT_HEVC, nal = false;
	if (!hevc && codec != V4L2_PIX_FMT_H264)
		return true;
	for (size_t i = 0; i + 3 < n; i++) {
		if (p[i] || p[i + 1] || p[i + 2] != 1)
			continue;
		nal = true;
		unsigned t = hevc ? (p[i + 3] >> 1) & 0x3f : p[i + 3] & 0x1f;
		if (hevc ? (t >= 16 && t <= 21) || (t >= 32 && t <= 34) : t == 5 || t == 7 || t == 8)
			return true;
		if (hevc ? t < 32 : t >= 1 && t <= 4) // another picture
			return false;
		i += 3;
	}
	return !nal;
}

// CAPTURE of our own for a client that waits for the stream's size before it sets CAPTURE up: the firmware session
// starts with both queues streaming, and its "insufficient" at the first sequence header says the size.
static int boot_capture(struct q1dec *e)
{
	int t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (apply_cap_fmt(e) < 0)
		return -1;
	struct v4l2_requestbuffers rb = {.count = min_capture(e), .type = t, .memory = V4L2_MEMORY_USERPTR};
	if (sys_ioctl(e->fd, VIDIOC_REQBUFS, &rb) < 0) {
		logf_("REQBUFS CAPTURE %u (until the stream's size is known): %s", rb.count, strerror(errno));
		return -1;
	}
	uint32_t n = rb.count > MAXBUF ? MAXBUF : rb.count;
	e->cap_dmabuf = false;
	e->count[CAP] = n;
	for (uint32_t i = 0; i < n; i++)
		if (ion_alloc(e, e->cap_size, &e->b[CAP][i]) < 0 ||
		    (e->cap_planes > 1 && ion_alloc(e, e->extra_size, &e->x[i]) < 0) || drv_qbuf(e, CAP, i, NULL, 0) < 0)
			return -1;
	if (sys_ioctl(e->fd, VIDIOC_STREAMON, &t) < 0) {
		logf_("STREAMON CAPTURE (until the stream's size is known): %s", strerror(errno));
		return -1;
	}
	e->on[CAP] = e->boot = true;
	logf_("no CAPTURE from the client yet: %u buffers of %ux%u here until the stream's size is known", n, e->cw,
	      e->ch);
	return 0;
}

// our CAPTURE goes (every buffer back from the firmware first); the client is told the stream's size if asked
static void boot_end(struct q1dec *e, uint32_t w, uint32_t h, bool tell)
{
	int t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	struct v4l2_requestbuffers rb = {.count = 0, .type = t, .memory = V4L2_MEMORY_USERPTR};
	if ((e->on[CAP] && drain_capture(e, false) < 0) || sys_ioctl(e->fd, VIDIOC_STREAMOFF, &t) < 0 ||
	    sys_ioctl(e->fd, VIDIOC_REQBUFS, &rb) < 0)
		dead(e, "our CAPTURE could not be taken down");
	e->on[CAP] = false;
	free_bufs(e, CAP);
	e->boot = e->boot_sized = false; // (only now: its frames done in the driver are not the client's)
	if (!tell)
		return;
	set_dims(e, w, h);
	stream_size(e, w, h);
	e->await_cap = true;
	if (e->epfd >= 0) { // what comes until the client's CAPTURE streams is given back; then the next stream start
		e->start_seen = false;
		e->skipped_all = 0;
	}
	src_pending(e);
	logf_("the stream is %ux%u: CAPTURE is the client's now", w, h);
}

// The stream's size came: our CAPTURE is set up again at that size, so the firmware goes on decoding (into buffers of
// ours, given straight back) while the client sets its own up, and the client gets its SOURCE_CHANGE. A firmware
// that waits for CAPTURE instead holds every packet (vrlink, ~25 ms for each of its 16 frame buffers, ran out of
// packet buffers) and takes no stop and no flush and continue in between well (2026-11-11).
static int boot_resize(struct q1dec *e, uint32_t w, uint32_t h)
{
	int t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	struct v4l2_requestbuffers rb = {.count = 0, .type = t, .memory = V4L2_MEMORY_USERPTR};
	if (drain_capture(e, false) < 0 || sys_ioctl(e->fd, VIDIOC_STREAMOFF, &t) < 0 ||
	    sys_ioctl(e->fd, VIDIOC_REQBUFS, &rb) < 0) {
		logf_("our CAPTURE at the stream's size: taking the first one down: %s", strerror(errno));
		return -1;
	}
	e->on[CAP] = false;
	free_bufs(e, CAP);
	set_dims(e, w, h);
	if (apply_cap_fmt(e) < 0)
		return -1;
	rb.count = min_capture(e);
	if (sys_ioctl(e->fd, VIDIOC_REQBUFS, &rb) < 0) {
		logf_("our CAPTURE at the stream's size: REQBUFS %u: %s", rb.count, strerror(errno));
		return -1;
	}
	uint32_t n = rb.count > MAXBUF ? MAXBUF : rb.count;
	e->count[CAP] = n;
	for (uint32_t i = 0; i < n; i++)
		if (ion_alloc(e, e->cap_size, &e->b[CAP][i]) < 0 ||
		    (e->cap_planes > 1 && ion_alloc(e, e->extra_size, &e->x[i]) < 0) || drv_qbuf(e, CAP, i, NULL, 0) < 0) {
			logf_("our CAPTURE at the stream's size: %u buffers of %u kB: %s", n, e->cap_size / 1024, strerror(errno));
			return -1;
		}
	if (sys_ioctl(e->fd, VIDIOC_STREAMON, &t) < 0) { // (the firmware's "continue")
		logf_("our CAPTURE at the stream's size: STREAMON: %s", strerror(errno));
		return -1;
	}
	e->on[CAP] = true;
	e->boot_sized = true;
	stream_size(e, w, h);
	src_pending(e);
	logf_("the stream is %ux%u: decoded into %u buffers of ours (%u kB each) until the client's CAPTURE streams", w, h,
	      n, e->cap_size / 1024);
	return 0;
}

// The client's CAPTURE STREAMON after it set its buffers up while ours ran (cap_pending): ours go (the session
// stops), the client's are set up and queued, and the session starts again with them; decoding resumes at the
// next stream start, which vrlink asks for itself when no frame comes for 300 ms.
static int boot_swap(struct q1dec *e)
{
	boot_end(e, 0, 0, false);
	if (e->dead)
		return fail(EIO);
	e->cap_pending = false;
	struct v4l2_requestbuffers rb = {.count = e->pend_count, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
	                                 .memory = V4L2_MEMORY_DMABUF};
	if (reqbufs(e, &rb) < 0)
		return -1;
	for (uint32_t i = 0; i < e->pend_count && i < e->count[CAP]; i++)
		if (e->pend_q[i] && (adopt_dmabuf(e, i, e->pend_fd[i]) < 0 || drv_qbuf(e, CAP, i, NULL, 0) < 0))
			return -1;
	e->start_seen = false;
	e->skipped_all = 0;
	int r = stream(e, VIDIOC_STREAMON, CAP);
	if (r == 0)
		logf_("the client's CAPTURE streams: decoding again from the next stream start");
	return r;
}

// The client stops before it set CAPTURE up (vrlink resets its decoder when no frame came for 800 ms, which can be
// right then). The driver's STOP at the last STREAMOFF (or at close) is never answered by a firmware that waits
// for its port reconfiguration: 2 s, an abort, 2 s more, then a system error that takes the video core down
// (2026-11-11). A flush of both queues first takes it out of that wait.
static void leave_reconfig(struct q1dec *e)
{
	if (!e->await_cap)
		return;
	e->await_cap = false;
	if (e->dead || !e->on[OUT])
		return;
	if (flush(e, QCOM_CMD_FLUSH_OUTPUT | QCOM_CMD_FLUSH_CAPTURE) < 0)
		logf_("stopped while the firmware waited for CAPTURE: flush failed");
}

// Our CAPTURE's buffers that the firmware gave back (empty, one for each picture it dropped before the stream's
// start) go straight back to it: left done in the driver, they keep the decoder readable (POLLIN) for the client,
// whose epoll_wait would wake for nothing until the stream's size is known.
static void boot_recycle(struct q1dec *e)
{
	struct v4l2_buffer d;
	struct v4l2_plane p[2];
	struct pollfd pf = {.fd = e->fd, .events = POLLIN}; // (the client may have opened the device blocking)
	while (e->boot && e->on[CAP] && q1_sys_poll(&pf, 1, 0) > 0 && (pf.revents & POLLIN) && drv_dqbuf(e, CAP, &d, p) == 0) {
		if (d.index >= MAXBUF)
			continue;
		e->indrv[CAP][d.index] = false;
		if (drv_qbuf(e, CAP, d.index, NULL, 0) < 0)
			break;
	}
}

static void boot_check(struct q1dec *e)
{
	if (e->booted || e->dead || e->closing || !e->sub_src || !e->on[OUT] || e->on[CAP] || e->count[CAP])
		return;
	// a client that waits with epoll gets it at once (vrlink, which takes every packet buffer being held as fatal);
	// one that polls may still set CAPTURE up itself after its first packets
	if (e->epfd < 0 && (!e->first_packet_ns || now_ns() - e->first_packet_ns < BOOT_AFTER_NS))
		return;
	e->booted = true;
	uint32_t pix = e->out_fmt.fmt.pix_mp.pixelformat;
	if (e->epfd >= 0 && known_w && known_pix == pix) {
		// vrlink's decoder again after a reset of its own: the stream's size is the one its last decoder saw. It
		// sets its CAPTURE up now, while the I-frame it asked for comes, instead of after it, then after another
		// one (it resets again when no frame came 800 ms after its reset, which that took, 2026-11-11).
		set_dims(e, known_w, known_h);
		apply_cap_fmt(e); // (the driver's frame size, as the client is told)
		// With CAPTURE at the stream's size already, the firmware finds its buffers sufficient at the first sequence:
		// the driver then sends it its "continue" itself instead of an "insufficient" that would want CAPTURE set up
		// again, which the firmware did not always survive here (every packet and frame buffer held, 2026-11-11).
		struct v4l2_control cd = {.id = CID_CONTINUE_DATA, .value = 1};
		if (sys_ioctl(e->fd, VIDIOC_S_CTRL, &cd) < 0)
			logf_("control 0x%x = 1 (continue at a sufficient sequence): %s", cd.id, strerror(errno));
		e->early = true;
		e->start_seen = false;
		e->skipped_all = 0;
		src_pending(e);
		logf_("the stream should be %ux%u as before: the client's CAPTURE at once", known_w, known_h);
		return;
	}
	if (boot_capture(e) < 0) {
		boot_end(e, 0, 0, false);
		dead(e, "CAPTURE could not be set up");
	}
}

// a "port settings changed" event of the driver
static void seq_changed(struct q1dec *e, const struct v4l2_event *ev)
{
	const uint32_t *d = (const uint32_t *)ev->u.data;
	uint32_t h = d[0], w = d[1];
	bool insufficient = ev->type != EV_SETTINGS_SUFFICIENT;
	if (d[3] == BIT_DEPTH_10 && !e->ten_bit) {
		e->ten_bit = true; // (the next CAPTURE set-up changes the mode: apply_cap_fmt)
		e->cap_dirty = true;
		logf_("the stream is 10-bit");
	} else if (d[3] > BIT_DEPTH_10) {
		logf_("bit depth %u: not handled", d[3]);
	}
	if (e->boot) {
		w = w ? w : e->cw;
		h = h ? h : e->ch;
		if (e->boot_sized && w == e->cw && h == e->ch) { // about our CAPTURE at the stream's size already
			if (insufficient && reconfigure(e) < 0)
				dead(e, "reconfiguration of our CAPTURE failed");
			return;
		}
		// the size for our CAPTURE when an epoll client waits (vrlink); else the client's own CAPTURE, set up
		// while the firmware waits for it
		if (e->epfd < 0 || boot_resize(e, w, h) < 0)
			boot_end(e, w, h, true);
		return;
	}
	if (w && h && (w != e->cw || h != e->ch)) {
		logf_("the stream is %ux%u, not %ux%u: new CAPTURE format", w, h, e->cw, e->ch);
		stream_size(e, w, h);
		if (e->on[CAP]) {
			if (drain_capture(e, true) < 0) {
				dead(e, "flush failed");
				return;
			}
			e->reconfig = true;
		}
		set_dims(e, w, h);
	} else {
		uint64_t t0 = now_ns();
		if (insufficient && e->on[CAP] && reconfigure(e) < 0) {
			dead(e, "reconfiguration failed");
			return;
		}
		if (e->epfd >= 0 && !e->frames && !e->seq_ns)
			e->seq_ns = now_ns();
		if (e->epfd >= 0 && !e->frames)
			logf_("the stream's first sequence: %s, %ux%u (%u buffers), %s in %.0f ms",
			      insufficient ? "insufficient" : "sufficient", w, h, e->count[CAP],
			      insufficient && e->on[CAP] ? "CAPTURE set up again" : "nothing to do", (now_ns() - t0) / 1e6);
		// Nothing changed for the client, so no source change: Steam's stream client sets CAPTURE up again at any
		// of them, which this driver does not survive.
		e->quiet_change = true;
		return;
	}
	src_pending(e);
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
		} else {
			// never a wait here, whatever the client opened the device with: the lock is held
			struct pollfd pf = {.fd = e->fd, .events = POLLPRI};
			if (e->dead || e->closing || q1_sys_poll(&pf, 1, 0) <= 0 || !(pf.revents & POLLPRI) ||
			    sys_ioctl(e->fd, VIDIOC_DQEVENT, &ev) < 0)
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
	// a client that opened the device without O_NONBLOCK waits here for an event (Steam's stream client has a
	// thread that does only that): waited for outside the lock
	bool wait = !(fcntl(e->fd, F_GETFL) & O_NONBLOCK);
	for (;;) {
		pthread_mutex_lock(&e->mu);
		pump(e);
		if (!e->sub_src) {
			e->pending_src = 0;
			src_taken(e);
		} else if (!wait && !e->pending_src && e->quiet_change && e->epfd < 0)
			e->pending_src = 1; // a client that polls (libavcodec) was woken by the driver's event and must find
			                    // one to take: one that says nothing changed, which it checks
		e->quiet_change = false;
		if (e->pending_src)
			break;
		bool is_dead = e->dead;
		pthread_mutex_unlock(&e->mu);
		if (!wait || is_dead)
			return fail(is_dead ? EIO : ENOENT);
		struct pollfd pf = {.fd = e->fd, .events = POLLPRI};
		int n = q1_sys_poll(&pf, 1, 200);
		if (n < 0 && errno != EINTR)
			return -1;
		if (pf.revents & POLLNVAL)
			return fail(EBADF);
		if (n > 0 && !(pf.revents & POLLPRI))
			usleep(20000); // the driver says POLLERR while a queue has no buffer: not an event
	}
	memset(ev, 0, sizeof(*ev));
	ev->type = V4L2_EVENT_SOURCE_CHANGE;
	ev->u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION;
	ev->pending = --e->pending_src;
	src_taken(e);
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
	bool dmabuf = q == CAP && rb->memory == V4L2_MEMORY_DMABUF;
	if (q < 0 || (rb->memory != V4L2_MEMORY_MMAP && !dmabuf))
		return sys_ioctl(e->fd, VIDIOC_REQBUFS, rb);
	set_up(e);
	if (dmabuf && rb->count > CLIENT_CAPTURE && e->epfd >= 0) {
		// vrlink makes a Vulkan image for each and shares it with vrcompositor, ~15 ms each, in the thread that
		// takes its 16 packet buffers back: 16 of them took longer than those last at 72 fps (2026-11-11). It reads
		// the count back.
		rb->count = CLIENT_CAPTURE;
	}
	if (q == CAP && e->boot && e->boot_sized && e->epfd >= 0) { // kept until its STREAMON (boot_swap)
		e->pend_count = rb->count > MAXBUF ? MAXBUF : rb->count;
		e->cap_pending = e->pend_count > 0;
		for (uint32_t i = 0; i < MAXBUF; i++) {
			e->pend_fd[i] = -1;
			e->pend_q[i] = false;
		}
		rb->count = e->pend_count;
		return 0;
	}
	if (q == CAP && e->boot) // the client sets CAPTURE up before the stream's size came after all
		boot_end(e, 0, 0, false);
	if (e->on[q])
		stream(e, VIDIOC_STREAMOFF, q);
	struct v4l2_requestbuffers d = *rb;
	d.memory = V4L2_MEMORY_USERPTR;
	if (!rb->count) {
		int r = sys_ioctl(e->fd, VIDIOC_REQBUFS, &d);
		if (q == OUT)
			out_keep(e);
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
	if (q == OUT)
		out_keep(e);
	free_bufs(e, q);
	uint32_t n = d.count > MAXBUF + (q == OUT) ? MAXBUF + (q == OUT) : d.count;
	// The client has that many and no more: Steam's stream client keeps 16 in a table of 16, does not read the
	// count back and writes past the table when given more (its STEAMLINK_V4L2_BUFFER_COUNT too). The driver
	// registers 18 before it has seen the stream and decodes with the 16 it is given (8 are enough for 1080p).
	if (dmabuf && n > rb->count)
		n = rb->count;
	if (q == CAP) {
		e->cap_dmabuf = dmabuf;
		for (uint32_t i = 0; i < MAXBUF; i++)
			e->cap_cfd[i] = -1;
	}
	for (uint32_t i = 0; i < n; i++)
		if ((q == OUT && out_alloc(e, e->in_size, &e->b[q][i]) < 0) ||
		    (q == CAP && !dmabuf && ion_alloc(e, e->cap_size, &e->b[q][i]) < 0) ||
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
		logf_("frames: NV12 %ux%u in %ux%u, stride %u, %u lines, %u buffers of %u kB%s", e->crop_w, e->crop_h, e->cw,
		      e->ch, e->stride, e->scan, e->count[q], e->cap_size / 1024, dmabuf ? " (dma-bufs of the client)" : "");
	else
		logf_("bitstream: %s, %u buffers of %u kB", fourcc(e->out_fmt.fmt.pix_mp.pixelformat, s), e->count[q],
		      e->in_size / 1024);
	return 0;
}

static bool mine(struct q1dec *e, struct v4l2_buffer *b, int *q)
{
	*q = qof(b->type);
	return *q >= 0 && (b->memory == V4L2_MEMORY_MMAP || (*q == CAP && e->cap_dmabuf && b->memory == V4L2_MEMORY_DMABUF)) &&
	       b->index < e->count[*q] && b->length >= 1 && b->m.planes;
}

// the dma-buf the client brings for a frame buffer: an ION buffer (the only kind the driver takes), mapped here once
static int adopt_dmabuf(struct q1dec *e, uint32_t i, int fd)
{
	struct ionbuf *b = &e->b[CAP][i];
	if (e->cap_cfd[i] == fd && b->size)
		return 0;
	off_t len = lseek(fd, 0, SEEK_END);
	if (len < (off_t)e->cap_size) {
		logf_("frame buffer %u of the client is %lld bytes, the decoder needs %u", i, (long long)len, e->cap_size);
		return fail(EINVAL);
	}
	int own = fcntl(fd, F_DUPFD_CLOEXEC, 0);
	if (own < 0)
		return -1;
	void *map = (void *)syscall(SYS_mmap, NULL, (size_t)len, PROT_READ | PROT_WRITE, MAP_SHARED, own, 0);
	if (map == MAP_FAILED) {
		int err = errno;
		syscall(SYS_close, own);
		logf_("frame buffer %u of the client cannot be mapped: %s", i, strerror(err));
		return fail(err);
	}
	ion_free(b);
	b->fd = own;
	b->map = map;
	b->size = (size_t)len;
	e->cap_cfd[i] = fd;
	return 0;
}

static int querybuf(struct q1dec *e, struct v4l2_buffer *b)
{
	int q;
	if (qof(b->type) == CAP && b->memory == V4L2_MEMORY_DMABUF && e->cap_pending && b->length >= 1 && b->m.planes) {
		if (b->index >= e->pend_count) // the client's buffers, kept until its STREAMON (boot_swap)
			return fail(EINVAL);
		memset(&b->m.planes[0], 0, sizeof(b->m.planes[0]));
		b->m.planes[0].length = e->cap_size;
		b->m.planes[0].m.fd = e->pend_fd[b->index];
		b->length = 1;
		b->flags = V4L2_BUF_FLAG_TIMESTAMP_COPY | (e->pend_q[b->index] ? V4L2_BUF_FLAG_QUEUED : 0);
		return 0;
	}
	if (!mine(e, b, &q))
		return sys_ioctl(e->fd, VIDIOC_QUERYBUF, b);
	struct v4l2_plane *p = &b->m.planes[0];
	memset(p, 0, sizeof(*p));
	p->length = (uint32_t)e->b[q][b->index].size;
	if (q == CAP && e->cap_dmabuf && !p->length) // the client's, before it queued it
		p->length = e->cap_size;
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
	if (qof(b->type) == CAP && b->memory == V4L2_MEMORY_DMABUF && b->length >= 1 && b->m.planes) {
		pthread_mutex_lock(&e->mu);
		bool kept = e->cap_pending && b->index < e->pend_count;
		if (kept) { // set up at the client's STREAMON (boot_swap)
			e->pend_fd[b->index] = b->m.planes[0].m.fd;
			e->pend_q[b->index] = true;
		}
		pthread_mutex_unlock(&e->mu);
		if (kept) {
			b->flags = V4L2_BUF_FLAG_QUEUED | V4L2_BUF_FLAG_TIMESTAMP_COPY;
			return 0;
		}
	}
	if (!mine(e, b, &q))
		return sys_ioctl(e->fd, VIDIOC_QBUF, b);
	pthread_mutex_lock(&e->mu);
	if (!e->sub_src)
		pump(e);
	int r = 0;
	if (e->dead)
		r = fail(EIO);
	else if (q == OUT) {
		if (!e->first_packet_ns)
			e->first_packet_ns = now_ns();
		const struct v4l2_plane *pl = &b->m.planes[0];
		const struct ionbuf *ib = &e->b[OUT][b->index];
		size_t end = pl->bytesused < ib->size ? pl->bytesused : ib->size, off = pl->data_offset < end ? pl->data_offset : 0;
		// Before the stream's start, a client that waits with epoll gets its packets back at once (our eventfd: its
		// EPOLLOUT); vrlink sends pictures that need others until the I-frame it asks for after a reset of its own.
		// So it does while it sets its CAPTURE up (await_cap): the firmware would hold them all, and vrlink takes
		// ~25 ms for each of its 16 frame buffers (2026-11-11); decoding starts again at the next stream start.
		bool waiting = (e->await_cap || e->early) && e->epfd >= 0;
		if (!e->start_seen && !waiting &&
		    (e->epfd < 0 || e->skipped_all >= SKIP_MAX || e->skipped_n > MAXBUF ||
		     stream_start(e->out_fmt.fmt.pix_mp.pixelformat, ib->map + off, end - off))) {
			e->start_seen = true;
			if (e->skipped_all)
				logf_("%u packets given back undecoded before the stream's start", e->skipped_all);
		}
		if (e->start_seen && !waiting) {
			r = drv_qbuf(e, OUT, b->index, b, 0);
		} else {
			e->skipped[e->skipped_n] = b->index;
			e->skipped_ts[e->skipped_n++] = b->timestamp;
			if (!waiting)
				e->skipped_all++;
			ev_sync(e);
		}
	}
	else if (e->cap_dmabuf && adopt_dmabuf(e, b->index, b->m.planes[0].m.fd) < 0)
		r = -1;
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
	if (q == CAP && e->cap_dmabuf)
		sp->m.fd = e->cap_cfd[d->index];
	else
		sp->m.mem_offset = COOKIE(q, d->index);
}

static int dqbuf(struct q1dec *e, struct v4l2_buffer *b)
{
	int q = qof(b->type);
	if (q == CAP && __atomic_load_n(&e->boot_sized, __ATOMIC_RELAXED) && e->epfd >= 0)
		return fail(EAGAIN); // the CAPTURE in the driver is ours yet
	if (q < 0 || (b->memory != V4L2_MEMORY_MMAP && !(q == CAP && e->cap_dmabuf && b->memory == V4L2_MEMORY_DMABUF)) ||
	    b->length < 1 || !b->m.planes)
		return sys_ioctl(e->fd, VIDIOC_DQBUF, b);
	pthread_mutex_lock(&e->mu);
	if (!e->sub_src)
		pump(e);
	int err = 0;
	if (e->dead) {
		err = EIO;
	} else if (q == OUT && e->skipped_n) {
		struct v4l2_buffer d = {.index = e->skipped[0], .timestamp = e->skipped_ts[0]};
		to_client(e, q, b, &d, 0);
		e->skipped_n--;
		memmove(&e->skipped[0], &e->skipped[1], e->skipped_n * sizeof(e->skipped[0]));
		memmove(&e->skipped_ts[0], &e->skipped_ts[1], e->skipped_n * sizeof(e->skipped_ts[0]));
		ev_sync(e);
		pthread_mutex_unlock(&e->mu);
		return 0;
	} else if (q == CAP) {
		if (e->fifo_n) {
			to_client(e, q, b, &e->fifo[0], e->fifo_used[0]);
			e->fifo_n--;
			memmove(&e->fifo[0], &e->fifo[1], e->fifo_n * sizeof(e->fifo[0]));
			memmove(&e->fifo_used[0], &e->fifo_used[1], e->fifo_n * sizeof(e->fifo_used[0]));
			if (!e->frames++ && e->epfd >= 0)
				logf_("first frame, %.0f ms after the first packet", (now_ns() - e->first_packet_ns) / 1e6);
			pthread_mutex_unlock(&e->mu);
			keep_clock(e);
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
			if (p[0].bytesused && e->frames == 1 && e->epfd >= 0)
				logf_("first frame, %.0f ms after the first packet", (now_ns() - e->first_packet_ns) / 1e6);
			if (eos)
				e->last_seen = true;
		}
		to_client(e, q, b, &d, q == CAP ? p[0].bytesused : 0);
		pthread_mutex_unlock(&e->mu);
		if (q == CAP)
			keep_clock(e);
		return 0;
	}
}

static int stream(struct q1dec *e, unsigned long req, int q)
{
	int t = q == OUT ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (q == CAP && e->boot && e->boot_sized && e->epfd >= 0) { // the CAPTURE in the driver is ours yet
		if (req == VIDIOC_STREAMOFF) {
			memset(e->pend_q, 0, sizeof(e->pend_q));
			return 0;
		}
		return e->cap_pending ? boot_swap(e) : fail(EINVAL);
	}
	if (req == VIDIOC_STREAMOFF && q == OUT)
		leave_reconfig(e);
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
	if (req == VIDIOC_STREAMON && q == CAP && e->await_cap) {
		e->await_cap = false; // (the driver sent the firmware its "continue")
		if (e->epfd >= 0)
			logf_("the client's CAPTURE streams: decoding again from the next stream start");
	}
	if (req == VIDIOC_STREAMON && q == CAP && e->early) {
		e->early = false;
		e->start_seen = false;
		logf_("the client's CAPTURE streams before the stream: decoding from its start");
	}
	if (req == VIDIOC_STREAMOFF) {
		memset(e->indrv[q], 0, sizeof(e->indrv[q]));
		if (q == CAP) {
			e->reconfig = false;
			e->fifo_n = 0;
			e->last_seen = false;
		} else {
			e->eos_sent = false;
			e->skipped_n = 0; // the client's again, like the driver's
			ev_sync(e);
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
	case VIDIOC_S_CTRL:
		return s_ctrl(e, arg);
	}
	return sys_ioctl(e->fd, req, arg);
}

// --- a client that waits with epoll

static atomic_int ep_users; // decoders with our eventfd in an epoll set

struct closer {
	struct q1dec *e;
	int fd;
};

static void *close_run(void *arg)
{
	struct closer *c = arg;
	q1dec_closing(c->e);
	syscall(SYS_close, c->fd);
	q1dec_forget(c->e);
	free(c);
	return NULL;
}

// A decoder of a client that waits with epoll (vrlink) is closed in the background: its session stopping took
// 240 ms of the 800 vrlink gives its next decoder after a reset of its own (2026-11-11). It leaves the client's epoll
// set now; the fd number stays taken until it is closed, so no other file of the client is mistaken for it.
int q1dec_close_async(struct q1dec *e, int fd)
{
	struct closer *c = e->epfd >= 0 ? malloc(sizeof(*c)) : NULL;
	if (!c)
		return 0;
	pthread_mutex_lock(&e->mu);
	syscall(SYS_epoll_ctl, e->epfd, EPOLL_CTL_DEL, fd, NULL);
	if (e->evfd >= 0)
		syscall(SYS_epoll_ctl, e->epfd, EPOLL_CTL_DEL, e->evfd, NULL);
	e->epfd = -1;
	atomic_fetch_sub(&ep_users, 1);
	pthread_mutex_unlock(&e->mu);
	c->e = e;
	c->fd = fd;
	pthread_t t;
	pthread_attr_t a;
	pthread_attr_init(&a);
	pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
	int r = pthread_create(&t, &a, close_run, c);
	pthread_attr_destroy(&a);
	if (r != 0) {
		close_run(c);
		return 1;
	}
	return 1;
}

static int slot_of(struct q1dec *e)
{
	for (int i = 0; i < MAXDEV; i++)
		if (__atomic_load_n(&decs[i], __ATOMIC_ACQUIRE) == e)
			return i;
	return -1;
}

// after the client's epoll_ctl on the decoder fd went through
void q1dec_epoll_ctl(struct q1dec *e, int epfd, int op, const struct epoll_event *ev)
{
	pthread_mutex_lock(&e->mu);
	if (op == EPOLL_CTL_DEL) {
		if (e->epfd == epfd && e->evfd >= 0) {
			syscall(SYS_epoll_ctl, epfd, EPOLL_CTL_DEL, e->evfd, NULL);
			e->epfd = -1;
			atomic_fetch_sub(&ep_users, 1);
		}
	} else if (ev) {
		e->epdata = ev->data.u64;
		if (e->evfd < 0)
			e->evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
		int slot = slot_of(e);
		if (e->evfd >= 0 && slot >= 0 && e->epfd != epfd) {
			if (e->epfd >= 0) {
				syscall(SYS_epoll_ctl, e->epfd, EPOLL_CTL_DEL, e->evfd, NULL);
				atomic_fetch_sub(&ep_users, 1);
				e->epfd = -1;
			}
			struct epoll_event x = {.events = EPOLLIN, .data.u64 = EP_TAG | (uint64_t)slot};
			if (syscall(SYS_epoll_ctl, epfd, EPOLL_CTL_ADD, e->evfd, &x) == 0) {
				e->epfd = epfd;
				atomic_fetch_add(&ep_users, 1);
				ev_sync(e);
				boot_check(e);
			} else {
				logf_("eventfd into the client's epoll set: %s", strerror(errno));
			}
		}
	}
	pthread_mutex_unlock(&e->mu);
}

// Whether a frame waits for the client, taken from the driver into fifo if need be (lock held). The driver says
// POLLIN for buffers this library takes back itself (a reconfiguration's flush) and for empty ones (pictures the
// firmware dropped), and vrlink takes a DQBUF that finds nothing after an EPOLLIN as fatal: its decoder thread stops
// and it resets the decoder every 800 ms from then on (2026-11-11, a 10-bit stream's reconfiguration).
static bool frame_ready(struct q1dec *e)
{
	struct pollfd pf = {.fd = e->fd, .events = POLLIN};
	while (!e->fifo_n && !e->last_seen && !e->boot && !e->reconfig && e->on[CAP] && !e->dead &&
	       q1_sys_poll(&pf, 1, 0) > 0 && (pf.revents & POLLIN)) {
		struct v4l2_buffer d;
		struct v4l2_plane p[2];
		if (drv_dqbuf(e, CAP, &d, p) < 0 || d.index >= MAXBUF)
			break;
		e->indrv[CAP][d.index] = false;
		bool eos = d.flags & QCOM_BUF_FLAG_EOS;
		if (!p[0].bytesused && !eos) {
			if (drv_qbuf(e, CAP, d.index, NULL, 0) < 0)
				break;
			continue;
		}
		e->last_seen = eos;
		e->fifo[0] = d;
		e->fifo_used[0] = p[0].bytesused;
		e->fifo_n = 1;
	}
	return e->fifo_n || e->last_seen;
}

// what the client's epoll_wait on epfd gives it: our eventfd's readiness is EPOLLPRI (a SOURCE_CHANGE) and/or
// EPOLLOUT (a packet buffer back) on the decoder, the driver's own EPOLLPRI is not (its events are taken here);
// returns the new count
int q1dec_epoll_events(int epfd, struct epoll_event *ev, int n)
{
	if (n <= 0 || !atomic_load(&ep_users))
		return n;
	struct q1dec *mine[MAXDEV];
	int nm = 0;
	for (int i = 0; i < MAXDEV; i++) {
		struct q1dec *e = __atomic_load_n(&decs[i], __ATOMIC_ACQUIRE);
		if (e && e->epfd == epfd)
			mine[nm++] = e;
	}
	if (!nm)
		return n;
	int out = 0;
	for (int i = 0; i < n; i++) {
		struct epoll_event x = ev[i];
		bool dec = false;
		if ((x.data.u64 & ~0xffull) == EP_TAG) {
			uint64_t slot = x.data.u64 & 0xff;
			struct q1dec *e = slot < MAXDEV ? __atomic_load_n(&decs[slot], __ATOMIC_ACQUIRE) : NULL;
			if (!e || e->epfd != epfd)
				continue;
			x.events = (__atomic_load_n(&e->pending_src, __ATOMIC_RELAXED) ? EPOLLPRI : 0) |
			           (__atomic_load_n(&e->skipped_n, __ATOMIC_RELAXED) ? EPOLLOUT : 0);
			if (!x.events)
				continue;
			x.data.u64 = e->epdata;
			dec = true;
		} else {
			for (int k = 0; k < nm; k++)
				if (x.data.u64 == mine[k]->epdata) {
					x.events &= ~(uint32_t)EPOLLPRI;
					if (x.events & (EPOLLIN | EPOLLRDNORM)) { // only for a frame there is (and not of our CAPTURE)
						pthread_mutex_lock(&mine[k]->mu);
						if (!frame_ready(mine[k]))
							x.events &= ~(uint32_t)(EPOLLIN | EPOLLRDNORM);
						pthread_mutex_unlock(&mine[k]->mu);
					}
					dec = true;
				}
			if (!x.events)
				continue;
		}
		int j = dec ? 0 : out; // a decoder's two entries are one
		while (j < out && ev[j].data.u64 != x.data.u64)
			j++;
		if (j < out)
			ev[j].events |= x.events;
		else
			ev[out++] = x;
	}
	return out;
}
