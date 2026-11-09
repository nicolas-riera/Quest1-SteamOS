// q1screencast: the ScreenCast portal backend for the X screen Steam runs on (:1), so Remote Play captures it
// through PipeWire ("Desktop PipeWire NV12") instead of reading it back with OpenGL on llvmpipe ("Desktop OpenGL
// NV12": 20 to 80 ms a frame and most of Steam's CPU). See docs/streaming.md.
//
// - D-Bus: org.freedesktop.impl.portal.ScreenCast on the session bus as org.freedesktop.impl.portal.desktop.quest1
//   (xdg-desktop-portal picks it through /etc/xdg-desktop-portal/portals.conf). Every request is granted at once:
//   one screen, no dialog. Only host apps (empty app id) get it; Flatpak apps are refused.
// - PipeWire: one video source node per started session, NV12 or BGRx, the X screen's size, MemFd buffers. It exists
//   only while a session is open. NV12 is BT.709 full range, declared in the format: Steam's NV12 format asks for
//   exactly that (colorMatrix BT709, colorRange 0-255); BT.601 limited range looked washed out on the PC, and a
//   format that declares another colorspace does not match Steam's, which then takes BGRx.
// - Frames, at most 60 a second, plus one a second when nothing changes:
//   - while the headset's VR view is streamed (q1apps made /tmp/quest1-spectator): straight from Monado's double
//     buffer /dev/shm/quest1-spectator, each new frame once it is complete (its header's seq). Monado makes them
//     NV12 on the GPU, so they are only copied (1.4 MB). The X server is skipped, and nothing tears. After 0.5 s
//     without one, the X screen again: q1apps' cover there is dark blue.
//   - otherwise XShmGetImage of the root window (the X server answers between two drawings: no tearing), when the
//     screen changed (XDamage) or the pointer moved. Steam asks for the cursor "hidden" and draws it itself; in
//     the other modes the X cursor is drawn in. Reading Xvfb's framebuffer file (-fbdir) directly saved its 3.7 ms
//     but tore whenever the screen was drawn during the read.
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <errno.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/type-info.h>
#include <spa/utils/result.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#define BUS_NAME "org.freedesktop.impl.portal.desktop.quest1"
#define OBJ_PATH "/org/freedesktop/portal/desktop"
#define SPECT_TRIGGER "/tmp/quest1-spectator"
#define MAX_FPS 60
#define IDLE_FRAME_S 1.0
#define SPECT_STALE_S 0.5 // as q1apps' SPECT_IDLE_S

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// --- one session: the X capture and its PipeWire node ---------------------------------------------------------

struct session {
	struct session *next;
	char *path;
	sd_bus_slot *slot;
	uint32_t cursor_mode; // 1 hidden, 2 embedded, 4 metadata (drawn in as well)

	struct pw_thread_loop *loop;
	struct pw_context *ctx;
	struct pw_core *core;
	struct pw_stream *stream;
	struct spa_hook stream_listener;
	struct spa_source *timer, *xio;
	uint32_t node_id;
	bool failed;
	uint32_t format; // SPA_VIDEO_FORMAT_NV12 or _BGRx once negotiated
	bool streaming;

	Display *dpy;
	Window root;
	int width, height;
	XImage *img;
	XShmSegmentInfo shm;
	uint8_t *sp_map; // Monado's spectator file, see spect_frames
	size_t sp_size;
	ino_t sp_ino, sp_bad_ino;
	uint32_t sp_seq;   // the last frame sent from it
	uint32_t sp_seen;  // the last seq seen, at sp_changed
	double sp_changed;
	bool sp_live; // VR frames keep coming: they are the source
	int damage_event;
	Damage damage;
	bool dirty;
	int ptr_x, ptr_y;
	double last_frame;
	uint64_t seq;
	// time spent per frame, logged every 10 s
	double stat_t0, stat_ms, stat_get_ms;
	unsigned stat_frames;
};

static struct session *sessions;

// BT.709 full range (what Steam asks for), 2x2 chroma average; BGRx in (X's 24-bit TrueColor on a little-endian
// server).
// The scalar loop does what the NEON one does 16 pixels at a time; it also takes the right edge.
static void nv12_scalar(const uint8_t *s0, const uint8_t *s1, uint8_t *y0, uint8_t *y1, uint8_t *c, int i, int w)
{
	for (; i < w; i += 2) {
		const uint8_t *p[4] = {s0 + i * 4, s0 + (i + 1) * 4, s1 + i * 4, s1 + (i + 1) * 4};
		uint8_t *o[4] = {y0 + i, y0 + i + 1, y1 + i, y1 + i + 1};
		int R = 0, G = 0, B = 0;
		for (int k = 0; k < 4; k++) {
			int b = p[k][0], g = p[k][1], r = p[k][2];
			*o[k] = (uint8_t)((54 * r + 183 * g + 19 * b + 128) >> 8);
			R += r;
			G += g;
			B += b;
		}
		R = (R + 2) >> 2;
		G = (G + 2) >> 2;
		B = (B + 2) >> 2;
		c[i] = (uint8_t)(((-29 * R - 99 * G + 128 * B + 128) >> 8) + 128);
		c[i + 1] = (uint8_t)(((128 * R - 116 * G - 12 * B + 128) >> 8) + 128);
	}
}

#ifdef __aarch64__
#include <arm_neon.h>

static inline uint8x8_t luma8(uint8x8_t r, uint8x8_t g, uint8x8_t b)
{
	uint16x8_t y = vmull_u8(r, vdup_n_u8(54));
	y = vmlal_u8(y, g, vdup_n_u8(183));
	y = vmlal_u8(y, b, vdup_n_u8(19));
	return vrshrn_n_u16(y, 8);
}

static inline uint8x16_t luma16(uint8x16x4_t p)
{
	return vcombine_u8(luma8(vget_low_u8(p.val[2]), vget_low_u8(p.val[1]), vget_low_u8(p.val[0])),
	                   luma8(vget_high_u8(p.val[2]), vget_high_u8(p.val[1]), vget_high_u8(p.val[0])));
}

// the 2x2 average of one channel over two rows of 16 pixels: 8 values
static inline int16x8_t avg2x2(uint8x16_t a, uint8x16_t b)
{
	return vreinterpretq_s16_u16(vrshrq_n_u16(vpadalq_u8(vpaddlq_u8(a), b), 2));
}

static inline uint8x8_t chroma8(int16x8_t a, int16_t ka, int16x8_t b, int16_t kb, int16x8_t c, int16_t kc)
{
	int16x8_t v = vmulq_n_s16(a, ka);
	v = vmlaq_n_s16(v, b, kb);
	v = vmlaq_n_s16(v, c, kc);
	return vqmovun_s16(vaddq_s16(vrshrq_n_s16(v, 8), vdupq_n_s16(128)));
}
#endif

static void bgrx_to_nv12(const uint8_t *src, int sstride, uint8_t *y, uint8_t *uv, int ystride, int w, int h)
{
	for (int j = 0; j < h; j += 2) {
		const uint8_t *s0 = src + (size_t)j * sstride, *s1 = s0 + (j + 1 < h ? sstride : 0);
		uint8_t *y0 = y + (size_t)j * ystride, *y1 = y0 + (j + 1 < h ? ystride : 0);
		uint8_t *c = uv + (size_t)(j / 2) * ystride;
		int i = 0;
#ifdef __aarch64__
		for (; i + 16 <= w; i += 16) {
			uint8x16x4_t a = vld4q_u8(s0 + i * 4), b = vld4q_u8(s1 + i * 4);
			vst1q_u8(y0 + i, luma16(a));
			vst1q_u8(y1 + i, luma16(b));
			int16x8_t B = avg2x2(a.val[0], b.val[0]), G = avg2x2(a.val[1], b.val[1]),
				  R = avg2x2(a.val[2], b.val[2]);
			uint8x8x2_t cv = {{chroma8(R, -29, G, -99, B, 128), chroma8(R, 128, G, -116, B, -12)}};
			vst2_u8(c + i, cv);
		}
#endif
		nv12_scalar(s0, s1, y0, y1, c, i, w);
	}
}

// the X cursor, alpha-blended into the captured BGRx image
static void draw_cursor(struct session *s)
{
	if (s->cursor_mode == 1 || access(SPECT_TRIGGER, F_OK) == 0)
		return;
	XFixesCursorImage *c = XFixesGetCursorImage(s->dpy);
	if (!c)
		return;
	int x0 = c->x - c->xhot, y0 = c->y - c->yhot;
	for (int j = 0; j < c->height; j++) {
		int y = y0 + j;
		if (y < 0 || y >= s->height)
			continue;
		uint8_t *row = (uint8_t *)s->img->data + (size_t)y * s->img->bytes_per_line;
		for (int i = 0; i < c->width; i++) {
			int x = x0 + i;
			if (x < 0 || x >= s->width)
				continue;
			unsigned long p = c->pixels[j * c->width + i]; // premultiplied ARGB
			unsigned a = (p >> 24) & 0xff;
			if (!a)
				continue;
			uint8_t *d = row + x * 4;
			d[0] = (uint8_t)((p & 0xff) + d[0] * (255 - a) / 255);
			d[1] = (uint8_t)(((p >> 8) & 0xff) + d[1] * (255 - a) / 255);
			d[2] = (uint8_t)(((p >> 16) & 0xff) + d[2] * (255 - a) / 255);
		}
	}
	XFree(c);
}

// Monado's spectator file (comp_window_mdp.c, spect_thread): two NV12 frames (BT.709 full range, made on the GPU)
// after a header that says which one is complete; seq is bumped (release) once a frame is
#define SPECT_FILE "/dev/shm/quest1-spectator"
#define SPECT_MAGIC 0x32535051u // "QPS2"
#define SPECT_HEADER 4096
struct spect_header {
	uint32_t magic, width, height, stride, frame_bytes;
	uint32_t index, seq;
};

static void spect_unmap(struct session *s)
{
	if (s->sp_map)
		munmap(s->sp_map, s->sp_size);
	s->sp_map = NULL;
	s->sp_ino = 0;
}

// the spectator's header while the VR view is streamed and its frames are the screen's size, else NULL (and the X
// screen is read). Monado makes the file again (another inode) when it restarts the spectator.
static const struct spect_header *spect_frames(struct session *s)
{
	struct stat st;
	if (access(SPECT_TRIGGER, F_OK) != 0 || stat(SPECT_FILE, &st) != 0) {
		spect_unmap(s);
		return NULL;
	}
	if (!s->sp_map || st.st_ino != s->sp_ino) {
		spect_unmap(s);
		if (st.st_ino == s->sp_bad_ino)
			return NULL; // looked at already: not frames we can use
		int fd = open(SPECT_FILE, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			return NULL;
		void *m = st.st_size >= SPECT_HEADER ? mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
		close(fd);
		if (m == MAP_FAILED)
			return NULL;
		const struct spect_header *h = m;
		if (h->magic != SPECT_MAGIC || h->width != (uint32_t)s->width || h->height != (uint32_t)s->height ||
		    h->stride != h->width || h->frame_bytes < h->width * h->height * 3 / 2 ||
		    SPECT_HEADER + 2 * (uint64_t)h->frame_bytes > (uint64_t)st.st_size) {
			fprintf(stderr, "q1screencast: %s holds no %dx%d NV12 frames: the X screen is streamed\n", SPECT_FILE,
			        s->width, s->height);
			s->sp_bad_ino = st.st_ino;
			munmap(m, st.st_size);
			return NULL;
		}
		s->sp_map = m;
		s->sp_size = st.st_size;
		s->sp_ino = st.st_ino;
		s->sp_seq = 0;
		fprintf(stderr, "q1screencast: frames from %s\n", SPECT_FILE);
	}
	return (const struct spect_header *)s->sp_map;
}

// a filled buffer to Steam; t0..t_get read the source (Monado's file, or the X screen), the rest converted
static void frame_done(struct session *s, struct spa_buffer *buf, struct pw_buffer *b, double t0, double t_get)
{
	struct spa_meta_header *h = spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof(*h));
	if (h) {
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		h->pts = SPA_TIMESPEC_TO_NSEC(&ts);
		h->flags = 0;
		h->seq = s->seq++;
		h->dts_offset = 0;
	}
	pw_stream_queue_buffer(s->stream, b);
	double t1 = now_s();
	s->stat_get_ms += (t_get - t0) * 1000;
	s->stat_ms += (t1 - t0) * 1000;
	s->stat_frames++;
	if (!s->stat_t0)
		s->stat_t0 = t0;
	if (t1 - s->stat_t0 >= 10) {
		fprintf(stderr, "q1screencast: %.1f frames/s, %.2f ms each (%.2f reading the source)\n",
		        s->stat_frames / (t1 - s->stat_t0), s->stat_ms / s->stat_frames, s->stat_get_ms / s->stat_frames);
		s->stat_t0 = t1;
		s->stat_ms = 0;
		s->stat_get_ms = 0;
		s->stat_frames = 0;
	}
}

static void on_process(void *data)
{
	struct session *s = data;
	struct pw_buffer *b = pw_stream_dequeue_buffer(s->stream);
	if (!b)
		return;
	struct spa_buffer *buf = b->buffer;
	struct spa_data *d = &buf->datas[0];
	double t0 = now_s();
	const uint8_t *src = NULL;
	int sstride = 0;
	// Monado's frames are NV12 already: copied as they are
	const struct spect_header *sp =
	    d->data && s->sp_live && s->format == SPA_VIDEO_FORMAT_NV12 ? spect_frames(s) : NULL;
	uint32_t seq = sp ? __atomic_load_n(&sp->seq, __ATOMIC_ACQUIRE) : 0;
	if (seq) {
		s->sp_seq = seq;
		const uint8_t *f =
		    s->sp_map + SPECT_HEADER + (size_t)__atomic_load_n(&sp->index, __ATOMIC_RELAXED) * sp->frame_bytes;
		uint32_t size = (uint32_t)s->width * (uint32_t)s->height * 3 / 2; // Y, then U,V, both s->width a row
		memcpy(d->data, f, size);
		d->chunk->offset = 0;
		d->chunk->stride = s->width;
		d->chunk->size = size;
		frame_done(s, buf, b, t0, now_s());
		return;
	} else if (d->data && XShmGetImage(s->dpy, s->root, s->img, 0, 0, AllPlanes)) {
		draw_cursor(s);
		src = (const uint8_t *)s->img->data;
		sstride = s->img->bytes_per_line;
	} else {
		d->chunk->size = 0;
		pw_stream_queue_buffer(s->stream, b);
		return;
	}
	double t_get = now_s();
	uint32_t stride, size;
	if (s->format == SPA_VIDEO_FORMAT_NV12) {
		stride = (uint32_t)s->width;
		size = stride * (uint32_t)s->height * 3 / 2;
		bgrx_to_nv12(src, sstride, d->data, (uint8_t *)d->data + (size_t)stride * s->height, (int)stride, s->width,
		             s->height);
	} else {
		stride = (uint32_t)s->width * 4;
		size = stride * (uint32_t)s->height;
		for (int j = 0; j < s->height; j++)
			memcpy((uint8_t *)d->data + (size_t)j * stride, src + (size_t)j * sstride, stride);
	}
	d->chunk->offset = 0;
	d->chunk->stride = (int32_t)stride;
	d->chunk->size = size;
	frame_done(s, buf, b, t0, t_get);
}

// every event Xlib holds: a reply read from the socket (XQueryPointer, XShmGetImage) queues the events that came
// before it, and the socket then no longer wakes the loop for them
static void x_events(struct session *s)
{
	while (XPending(s->dpy)) {
		XEvent ev;
		XNextEvent(s->dpy, &ev);
		if (ev.type == s->damage_event + XDamageNotify) {
			XDamageSubtract(s->dpy, s->damage, None, None);
			s->dirty = true;
		}
	}
}

static void on_x(void *data, int fd, uint32_t mask)
{
	(void)fd;
	(void)mask;
	x_events(data);
}

static void on_timer(void *data, uint64_t expirations)
{
	(void)expirations;
	struct session *s = data;
	if (!s->streaming)
		return;
	x_events(s); // drained either way
	double t = now_s();
	const struct spect_header *sp = spect_frames(s);
	uint32_t seq = sp ? __atomic_load_n(&sp->seq, __ATOMIC_ACQUIRE) : 0;
	if (seq != s->sp_seen) {
		s->sp_seen = seq;
		s->sp_changed = t;
	}
	// no VR frame for a while (the headset off and asleep): the X screen, where q1apps' cover turns dark blue
	s->sp_live = seq && t - s->sp_changed < SPECT_STALE_S;
	if (s->sp_live) { // only a new complete VR frame (q1apps' cover damages the screen with the same frames)
		s->dirty = seq != s->sp_seq;
	} else {
		Window rr, cr;
		int rx, ry, wx, wy;
		unsigned mask;
		if (s->cursor_mode != 1 && XQueryPointer(s->dpy, s->root, &rr, &cr, &rx, &ry, &wx, &wy, &mask) &&
		    (rx != s->ptr_x || ry != s->ptr_y)) {
			s->ptr_x = rx;
			s->ptr_y = ry;
			s->dirty = true;
		}
		x_events(s); // the events read along with XQueryPointer's reply
	}
	if (!s->dirty && t - s->last_frame < IDLE_FRAME_S)
		return;
	s->dirty = false;
	s->last_frame = t;
	pw_stream_trigger_process(s->stream);
}

static void on_state_changed(void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error)
{
	(void)old;
	struct session *s = data;
	fprintf(stderr, "q1screencast: stream %s%s%s\n", pw_stream_state_as_string(state), error ? ": " : "",
	        error ? error : "");
	if (state == PW_STREAM_STATE_ERROR)
		s->failed = true;
	if (s->node_id == SPA_ID_INVALID && state >= PW_STREAM_STATE_PAUSED)
		s->node_id = pw_stream_get_node_id(s->stream);
	bool on = state == PW_STREAM_STATE_STREAMING;
	if (on != s->streaming) {
		s->streaming = on;
		s->dirty = true;
		struct timespec period = {0, on ? 1000000000L / MAX_FPS : 0};
		pw_loop_update_timer(pw_thread_loop_get_loop(s->loop), s->timer, &period, on ? &period : NULL, false);
	}
	pw_thread_loop_signal(s->loop, false);
}

static void on_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	struct session *s = data;
	if (!param || id != SPA_PARAM_Format)
		return;
	struct spa_video_info_raw info;
	if (spa_format_video_raw_parse(param, &info) < 0)
		return;
	s->format = info.format;
	uint32_t stride = s->format == SPA_VIDEO_FORMAT_NV12 ? (uint32_t)s->width : (uint32_t)s->width * 4;
	uint32_t size = s->format == SPA_VIDEO_FORMAT_NV12 ? stride * s->height * 3 / 2 : stride * s->height;
	fprintf(stderr, "q1screencast: format %s %dx%d\n", s->format == SPA_VIDEO_FORMAT_NV12 ? "NV12" : "BGRx",
	        s->width, s->height);
	uint8_t pbuf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(pbuf, sizeof(pbuf));
	const struct spa_pod *params[2];
	params[0] = spa_pod_builder_add_object(
		&b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers,
		SPA_POD_CHOICE_RANGE_Int(4, 2, 8), SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1), SPA_PARAM_BUFFERS_size,
		SPA_POD_Int(size), SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride), SPA_PARAM_BUFFERS_dataType,
		SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemFd));
	params[1] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type,
	                                       SPA_POD_Id(SPA_META_Header), SPA_PARAM_META_size,
	                                       SPA_POD_Int(sizeof(struct spa_meta_header)));
	pw_stream_update_params(s->stream, params, 2);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state_changed,
	.param_changed = on_param_changed,
	.process = on_process,
};

static const struct spa_pod *format_pod(struct spa_pod_builder *b, uint32_t fmt, int w, int h)
{
	struct spa_pod_frame f;
	spa_pod_builder_push_object(b, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(
		b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype,
		SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format, SPA_POD_Id(fmt), SPA_FORMAT_VIDEO_size,
		SPA_POD_Rectangle(&SPA_RECTANGLE(w, h)), SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&SPA_FRACTION(0, 1)),
		SPA_FORMAT_VIDEO_maxFramerate,
		SPA_POD_CHOICE_RANGE_Fraction(&SPA_FRACTION(MAX_FPS, 1), &SPA_FRACTION(1, 1), &SPA_FRACTION(MAX_FPS, 1)), 0);
	if (fmt == SPA_VIDEO_FORMAT_NV12)
		spa_pod_builder_add(b, SPA_FORMAT_VIDEO_colorRange, SPA_POD_Id(SPA_VIDEO_COLOR_RANGE_0_255),
		                    SPA_FORMAT_VIDEO_colorMatrix, SPA_POD_Id(SPA_VIDEO_COLOR_MATRIX_BT709), 0);
	return spa_pod_builder_pop(b, &f);
}

static void x_close(struct session *s)
{
	spect_unmap(s);
	if (!s->dpy)
		return;
	if (s->img) {
		XShmDetach(s->dpy, &s->shm);
		XDestroyImage(s->img);
		shmdt(s->shm.shmaddr);
	}
	if (s->damage)
		XDamageDestroy(s->dpy, s->damage);
	XCloseDisplay(s->dpy);
	s->dpy = NULL;
	s->img = NULL;
}

static bool x_open(struct session *s)
{
	s->dpy = XOpenDisplay(NULL);
	if (!s->dpy) {
		fprintf(stderr, "q1screencast: cannot open the X display\n");
		return false;
	}
	int ev, err;
	if (!XShmQueryExtension(s->dpy) || !XDamageQueryExtension(s->dpy, &s->damage_event, &err) ||
	    !XFixesQueryExtension(s->dpy, &ev, &err)) {
		fprintf(stderr, "q1screencast: the X server lacks MIT-SHM, DAMAGE or XFIXES\n");
		x_close(s);
		return false;
	}
	s->root = DefaultRootWindow(s->dpy);
	s->width = DisplayWidth(s->dpy, DefaultScreen(s->dpy)) & ~1;
	s->height = DisplayHeight(s->dpy, DefaultScreen(s->dpy)) & ~1;
	s->img = XShmCreateImage(s->dpy, DefaultVisual(s->dpy, DefaultScreen(s->dpy)),
	                         DefaultDepth(s->dpy, DefaultScreen(s->dpy)), ZPixmap, NULL, &s->shm, s->width,
	                         s->height);
	if (!s->img || s->img->bits_per_pixel != 32) {
		fprintf(stderr, "q1screencast: the X screen is not 32 bits per pixel\n");
		x_close(s);
		return false;
	}
	s->shm.shmid = shmget(IPC_PRIVATE, (size_t)s->img->bytes_per_line * s->height, IPC_CREAT | 0600);
	s->shm.shmaddr = s->img->data = shmat(s->shm.shmid, NULL, 0);
	s->shm.readOnly = False;
	XShmAttach(s->dpy, &s->shm);
	XSync(s->dpy, False);
	shmctl(s->shm.shmid, IPC_RMID, NULL); // gone once both sides detach
	s->damage = XDamageCreate(s->dpy, s->root, XDamageReportNonEmpty);
	XFlush(s->dpy);
	return true;
}

static void session_stop(struct session *s)
{
	if (s->loop) {
		pw_thread_loop_lock(s->loop);
		if (s->stream)
			pw_stream_destroy(s->stream);
		s->stream = NULL;
		pw_thread_loop_unlock(s->loop);
		pw_thread_loop_stop(s->loop);
		if (s->core)
			pw_core_disconnect(s->core);
		if (s->ctx)
			pw_context_destroy(s->ctx);
		pw_thread_loop_destroy(s->loop);
		s->loop = NULL;
		s->core = NULL;
		s->ctx = NULL;
	}
	x_close(s);
}

// the node, made when the session starts; returns its id or SPA_ID_INVALID
static uint32_t session_start(struct session *s)
{
	if (s->loop)
		return s->node_id;
	if (!x_open(s))
		return SPA_ID_INVALID;
	s->node_id = SPA_ID_INVALID;
	s->failed = false;
	s->loop = pw_thread_loop_new("q1screencast", NULL);
	s->ctx = pw_context_new(pw_thread_loop_get_loop(s->loop), NULL, 0);
	pw_thread_loop_lock(s->loop);
	pw_thread_loop_start(s->loop);
	s->core = pw_context_connect(s->ctx, NULL, 0);
	if (!s->core) {
		pw_thread_loop_unlock(s->loop);
		fprintf(stderr, "q1screencast: cannot connect to PipeWire: %s\n", strerror(errno));
		session_stop(s);
		return SPA_ID_INVALID;
	}
	struct pw_loop *l = pw_thread_loop_get_loop(s->loop);
	s->timer = pw_loop_add_timer(l, on_timer, s);
	s->xio = pw_loop_add_io(l, ConnectionNumber(s->dpy), SPA_IO_IN, false, on_x, s);
	s->stream = pw_stream_new(s->core, "quest1-screen",
	                          pw_properties_new(PW_KEY_MEDIA_CLASS, "Video/Source", PW_KEY_MEDIA_ROLE, "Screen",
	                                            PW_KEY_NODE_DESCRIPTION, "Quest 1 screen", NULL));
	pw_stream_add_listener(s->stream, &s->stream_listener, &stream_events, s);
	uint8_t pbuf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(pbuf, sizeof(pbuf));
	const struct spa_pod *params[2] = {format_pod(&b, SPA_VIDEO_FORMAT_NV12, s->width, s->height),
	                                   format_pod(&b, SPA_VIDEO_FORMAT_BGRx, s->width, s->height)};
	pw_stream_connect(s->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS,
	                  params, 2);
	double deadline = now_s() + 5;
	while (s->node_id == SPA_ID_INVALID && !s->failed && now_s() < deadline)
		pw_thread_loop_timed_wait(s->loop, 1);
	uint32_t id = s->node_id;
	pw_thread_loop_unlock(s->loop);
	if (id == SPA_ID_INVALID) {
		fprintf(stderr, "q1screencast: the stream got no node\n");
		session_stop(s);
	} else {
		fprintf(stderr, "q1screencast: node %u, %dx%d\n", id, s->width, s->height);
	}
	return id;
}

static void session_free(struct session *s)
{
	for (struct session **p = &sessions; *p; p = &(*p)->next)
		if (*p == s) {
			*p = s->next;
			break;
		}
	session_stop(s);
	sd_bus_slot_unref(s->slot);
	free(s->path);
	free(s);
}

static struct session *session_find(const char *path)
{
	for (struct session *s = sessions; s; s = s->next)
		if (!strcmp(s->path, path))
			return s;
	return NULL;
}

// --- D-Bus ------------------------------------------------------------------------------------------------------

static int m_session_close(sd_bus_message *m, void *data, sd_bus_error *err)
{
	(void)err;
	struct session *s = data;
	fprintf(stderr, "q1screencast: session %s closed\n", s->path);
	session_free(s);
	return sd_bus_reply_method_return(m, "");
}

static const sd_bus_vtable session_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Close", "", "", m_session_close, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_SIGNAL("Closed", "", 0),
	SD_BUS_VTABLE_END,
};

// the u values of an a{sv} we care about
static int read_options(sd_bus_message *m, uint32_t *cursor_mode, uint32_t *persist_mode)
{
	int r = sd_bus_message_enter_container(m, 'a', "{sv}");
	if (r < 0)
		return r;
	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *key;
		sd_bus_message_read(m, "s", &key);
		uint32_t *dst = !strcmp(key, "cursor_mode") ? cursor_mode : !strcmp(key, "persist_mode") ? persist_mode : NULL;
		const char *contents;
		char type;
		sd_bus_message_peek_type(m, &type, &contents);
		if (dst && contents && !strcmp(contents, "u")) {
			sd_bus_message_enter_container(m, 'v', "u");
			sd_bus_message_read(m, "u", dst);
			sd_bus_message_exit_container(m);
		} else {
			sd_bus_message_skip(m, "v");
		}
		sd_bus_message_exit_container(m);
	}
	return sd_bus_message_exit_container(m);
}

static int m_create_session(sd_bus_message *m, void *data, sd_bus_error *err)
{
	(void)data;
	(void)err;
	const char *handle, *session_path, *app_id;
	int r = sd_bus_message_read(m, "oos", &handle, &session_path, &app_id);
	if (r < 0)
		return r;
	if (app_id[0] || session_find(session_path)) {
		fprintf(stderr, "q1screencast: refused a session for \"%s\"\n", app_id);
		return sd_bus_reply_method_return(m, "ua{sv}", 2, 0);
	}
	struct session *s = calloc(1, sizeof(*s));
	s->path = strdup(session_path);
	s->cursor_mode = 2;
	s->node_id = SPA_ID_INVALID;
	r = sd_bus_add_object_vtable(sd_bus_message_get_bus(m), &s->slot, s->path, "org.freedesktop.impl.portal.Session",
	                             session_vtable, s);
	if (r < 0) {
		free(s->path);
		free(s);
		return sd_bus_reply_method_return(m, "ua{sv}", 2, 0);
	}
	s->next = sessions;
	sessions = s;
	fprintf(stderr, "q1screencast: session %s\n", s->path);
	return sd_bus_reply_method_return(m, "ua{sv}", 0, 1, "session_id", "s", s->path);
}

static int m_select_sources(sd_bus_message *m, void *data, sd_bus_error *err)
{
	(void)data;
	(void)err;
	const char *handle, *session_path, *app_id;
	int r = sd_bus_message_read(m, "oos", &handle, &session_path, &app_id);
	if (r < 0)
		return r;
	struct session *s = session_find(session_path);
	if (!s)
		return sd_bus_reply_method_return(m, "ua{sv}", 2, 0);
	uint32_t persist = 0;
	read_options(m, &s->cursor_mode, &persist);
	fprintf(stderr, "q1screencast: sources: cursor mode %u, persist mode %u\n", s->cursor_mode, persist);
	return sd_bus_reply_method_return(m, "ua{sv}", 0, 0);
}

static int m_start(sd_bus_message *m, void *data, sd_bus_error *err)
{
	(void)data;
	(void)err;
	const char *handle, *session_path, *app_id, *parent;
	int r = sd_bus_message_read(m, "ooss", &handle, &session_path, &app_id, &parent);
	if (r < 0)
		return r;
	struct session *s = session_find(session_path);
	uint32_t node = s ? session_start(s) : SPA_ID_INVALID;
	if (node == SPA_ID_INVALID)
		return sd_bus_reply_method_return(m, "ua{sv}", 2, 0);
	sd_bus_message *reply;
	r = sd_bus_message_new_method_return(m, &reply);
	if (r < 0)
		return r;
	sd_bus_message_append(reply, "u", 0);
	sd_bus_message_open_container(reply, 'a', "{sv}");
	sd_bus_message_open_container(reply, 'e', "sv");
	sd_bus_message_append(reply, "s", "streams");
	sd_bus_message_open_container(reply, 'v', "a(ua{sv})");
	sd_bus_message_open_container(reply, 'a', "(ua{sv})");
	sd_bus_message_open_container(reply, 'r', "ua{sv}");
	sd_bus_message_append(reply, "u", node);
	sd_bus_message_append(reply, "a{sv}", 4, "source_type", "u", 1, "id", "s", "0", "position", "(ii)", 0, 0, "size",
	                      "(ii)", s->width, s->height);
	sd_bus_message_close_container(reply);
	sd_bus_message_close_container(reply);
	sd_bus_message_close_container(reply);
	sd_bus_message_close_container(reply);
	// one screen and no choice: a restore token always gives the same
	sd_bus_message_append(reply, "{sv}", "persist_mode", "u", 2);
	sd_bus_message_append(reply, "{sv}", "restore_data", "(suv)", "quest1", 1, "s", "screen");
	sd_bus_message_close_container(reply);
	r = sd_bus_send(NULL, reply, NULL);
	sd_bus_message_unref(reply);
	return r;
}

static int p_u(sd_bus *bus, const char *path, const char *iface, const char *prop, sd_bus_message *reply,
               void *data, sd_bus_error *err)
{
	(void)bus;
	(void)path;
	(void)iface;
	(void)data;
	(void)err;
	uint32_t v = !strcmp(prop, "AvailableSourceTypes") ? 1 /* monitor */
	             : !strcmp(prop, "AvailableCursorModes") ? 1 | 2 | 4 /* metadata: drawn in too */
	                                                     : 5;
	return sd_bus_message_append(reply, "u", v);
}

static const sd_bus_vtable screencast_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("CreateSession", "oosa{sv}", "ua{sv}", m_create_session, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("SelectSources", "oosa{sv}", "ua{sv}", m_select_sources, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Start", "oossa{sv}", "ua{sv}", m_start, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_PROPERTY("AvailableSourceTypes", "u", p_u, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("AvailableCursorModes", "u", p_u, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("version", "u", p_u, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_VTABLE_END,
};

int main(void)
{
	setvbuf(stderr, NULL, _IOLBF, 0);
	XInitThreads();
	pw_init(NULL, NULL);
	sd_bus *bus;
	int r = sd_bus_open_user(&bus);
	if (r < 0) {
		fprintf(stderr, "q1screencast: no session bus: %s\n", strerror(-r));
		return 1;
	}
	r = sd_bus_add_object_vtable(bus, NULL, OBJ_PATH, "org.freedesktop.impl.portal.ScreenCast", screencast_vtable,
	                             NULL);
	if (r >= 0)
		r = sd_bus_request_name(bus, BUS_NAME, 0);
	if (r < 0) {
		fprintf(stderr, "q1screencast: cannot serve %s: %s\n", BUS_NAME, strerror(-r));
		return 1;
	}
	for (;;) {
		r = sd_bus_process(bus, NULL);
		if (r < 0)
			break;
		if (r == 0 && sd_bus_wait(bus, UINT64_MAX) < 0)
			break;
	}
	while (sessions)
		session_free(sessions);
	sd_bus_unref(bus);
	pw_deinit();
	return 0;
}
