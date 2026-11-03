// Copyright 2026, Quest1-SteamOS contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Compositor target for the Quest 1 panels through the MSM MDSS fbdev atomic commit.
 *
 * Zero copy: the target images are gralloc buffers (AHardwareBuffers through
 * libhybris, needs hwservicemanager + the graphics allocator HAL running) that
 * Vulkan renders into and the display engine scans out directly. Each half of
 * the 2880x1600 buffer goes to its own DMA pipe (one per panel/mixer).
 *
 * Frame pacing follows the vsync events of the video mode panels; commit
 * fences only signal one commit later, so they are kept as release fences for
 * reusing images.
 *
 * The proximity sensor (SyncBoss powerstate events) switches the panels off
 * while the headset is not worn; apps are then paced at a low rate.
 * QUEST1_IGNORE_PROX=1 shows frames regardless (development without wearing it).
 *
 * @ingroup comp_main
 */

#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_time.h"
#include "os/os_threading.h"
#include "os/os_time.h"

#include "main/comp_window.h"
#include "main/comp_compositor.h"

#include "quest1_direct_proto.h"
#include "quest1_warp_spv.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/fb.h>

#define __user
// msm_mdp.h has an enum value GC, also an Xlib type.
#define GC msm_mdp_GC
#include <linux/msm_mdp_ext.h>
#undef GC

#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan_android.h>

extern void *
android_dlopen(const char *filename, int flag);
extern void *
android_dlsym(void *handle, const char *symbol);

#define MDP_IMAGES 3
#define MDP_W 2880
#define MDP_H 1600
#define MDP_HALF (MDP_W / 2)
#define MDP_PERIOD_NS (U_TIME_1S_IN_NS / 72)
//! Exists while the panels are (going) off: an external scanout must not commit then.
#define MDP_PANELS_OFF "/tmp/quest1-panels-off"
//! Exists while the headset sleeps (quest1-standby): panels off as if the headset was removed.
#define MDP_STANDBY "/run/quest1-standby"
//! Time the compositor gets between waking up and the vsync that shows its frame.
#define MDP_RENDER_BUDGET_NS (15 * U_TIME_1MS_IN_NS)
#define PRIV_FLAGS_UBWC_ALIGNED 0x08000000
//! Frame period while the headset is not worn: apps keep running, slowly.
#define MDP_IDLE_PERIOD_NS (MDP_PERIOD_NS * 8)

// SyncBoss powerstate messages (drivers/staging/oculus uapi syncboss.h)
#define SYNCBOSS_DRIVER_MESSAGE_POWERSTATE_MSG 2
#define SYNCBOSS_PROX_EVENT_PROX_ON 2
#define SYNCBOSS_PROX_EVENT_PROX_OFF 3
struct syncboss_driver_message
{
	uint8_t header_version;
	uint8_t header_length;
	uint8_t from_driver;
	uint32_t type;
	uint32_t data;
} __attribute__((packed));

typedef struct
{
	int version, numFds, numInts;
	int data[];
} mdp_native_handle;

//! A buffer vrcompositor's layer handed over (one panel's half of one display image).
struct direct_buf
{
	AHardwareBuffer *ahb;
	int fd;
	uint32_t stride_px;
	bool ubwc;
	// Vulkan side (warp), made when needed
	VkImage image;
	VkDeviceMemory mem;
	VkImageView view;
	VkFramebuffer fb;      //!< outputs
	VkDescriptorSet set;   //!< sources
};

//! Re-project when /tmp/quest1-reproj does not say otherwise (it says "on" or "off", and tuning values, see warp_read_knobs)
#define MDP_WARP_DEFAULT_ON 1
#define WARP_GRID 40
//! Floats per warp vertex: position, then a texture coordinate per colour channel (red, green, blue)
#define WARP_VTX_FLOATS 8
//! With this file ("1"), SteamVR hands over its eye images undistorted (driver_quest1's ComputeDistortion is
//! the identity) and the warp does the lens distortion, the chromatic aberration and the re-projection in one
//! pass, like Meta's TimeWarp: no inverse lens model, so nothing to mismatch. Read at start.
#define WARP_LENS_FILE "/etc/quest1-warp-lens"
#define WARP_OUT_SETS 4
//! A stale frame is not warped again while its delta moved less than this since the last warp (0.4 px)
#define WARP_SKIP_DEG 0.03f

//! Objects of the warp pass (see warp_init).
struct direct_vk
{
	bool ready, failed;
	VkRenderPass rp;
	VkSampler sampler;
	VkDescriptorSetLayout dsl;
	VkPipelineLayout pl;
	VkPipeline pipe;
	VkDescriptorPool dpool;
	VkBuffer vbuf, ibuf;
	VkDeviceMemory vmem, imem;
	float *vptr;
	uint32_t nidx;
	VkCommandPool cpool;
	VkCommandBuffer cmd;
	VkFence fence;
	VkQueryPool qp;
};

struct mdp_direct_stats
{
	int64_t since_ns;
	uint32_t warp_commits, fresh_warped, raw_commits, no_set, angle_n, skipped;
	double gpu_ms_sum, gpu_ms_max, cpu_ms_sum, angle_sum, angle_max;
	double mesh_ms_sum, wait_ms_sum, commit_ms_sum;
	double ax_sum, ay_sum, az_sum; //!< |delta| about the head's x / y / z axes (deg)
	double oe_sum, oe_max;         //!< how far each new o estimate is from the running o (deg)
	uint32_t oe_n;
};

#define DIRECT_COMMITS 4

/*!
 * vrcompositor's layer (QUEST1_DIRECT_SCANOUT) hands us its half buffers and announces each finished
 * frame (quest1_direct_proto.h). Everything below is protected by `lock`, except where noted. Lock
 * order: lock, then the vsync thread's mutex.
 */
struct mdp_direct
{
	int listen_fd;
	int client_fd; //!< -1 when no layer is connected; read without the lock by external_scanout_active
	pthread_t thread;
	bool thread_running;
	volatile bool running;
	pthread_mutex_t lock;

	struct direct_buf buf[Q1D_MAX_IMAGES][2];
	uint32_t count, height;
	bool bgra;
	bool held[Q1D_MAX_IMAGES]; //!< the layer gave us this image (Q1D_FRAME) and it has not been released

	int latest; //!< newest frame's image, the source of every re-projection; -1 none
	struct
	{
		uint64_t frame;
		int64_t time_ns;
		float q[4];
		bool pose_valid;
	} latest_frame;
	bool fresh;        //!< `latest` was not on the panels yet
	bool need_refresh; //!< the panels came back on: show the latest frame again

	//! Commits that may still show an image: until their release fence signals
	struct
	{
		int release_fence, retire_fence; //!< -1 when none
		int image;                       //!< image of the layer shown by it
		uint64_t frame;                  //!< frame to report as displayed when the retire fence signals, 0 none
		uint64_t serial;                 //!< commit order
		int out;                         //!< output set (warped) shown by it, -1 when the layer's image
	} commits[DIRECT_COMMITS];
	uint64_t serial;

	//! Warp: output buffers, Vulkan objects, tuning knobs (warp_read_knobs)
	struct direct_buf out[WARP_OUT_SETS][2];
	struct direct_vk vk;
	struct
	{
		bool off;
		float yaw_deg, thr_deg, delay_ms;
		float rx_deg, ry_deg, rz_deg; //!< constant frame rotation R: delta' = conj(R) * delta * R
		int ack_mode;                 //!< perf: when a frame is reported displayed: 0 retire, 1 commit (default), 2 next vsync
		uint32_t sign;
		int64_t read_ns;
	} knobs;
	//! The warp on the panels: of frame `held_frame` by `held_delta`; a stale frame whose delta moved less than
	//! SKIP_DEG since is not warped again (the panels keep the commit). held_frame 0 when the panels show
	//! something else.
	uint64_t held_frame;
	uint64_t ack_pending; //!< ack mode vsync: frame committed, reported displayed at the next vsync
	float held_delta[4];
	uint32_t skipped;
	float o[4]; //!< device space = o * LOCAL, estimated from the render poses
	bool o_valid;
	//! The layer's images are undistorted eye renders (WARP_LENS_FILE): the warp does the lens distortion
	bool lens, lens_read;
	//! Their extent in tangents of each half's view (h: half 0 is view 1), as driver_quest1 projects them
	float tan_l[2], tan_r[2], tan_u[2], tan_d[2];
	uint64_t calibrated_frame;
	int64_t left_ns; //!< when the layer went away

	struct mdp_direct_stats stats;

	int (*recv_handle)(int, AHardwareBuffer **);
};

struct mdp_target
{
	struct comp_target base;

	struct mdp_direct direct;

	int fb_fd;
	int vsync_fd;
	int prox_fd; //!< /dev/syncboss_powerstate0, -1 when proximity is ignored.
	struct os_thread_helper vsync_thread;
	int64_t last_vsync_ns;   //!< Protected by vsync_thread's mutex.
	int64_t last_present_ns; //!< Protected by vsync_thread's mutex.

	const mdp_native_handle *(*get_native_handle)(const AHardwareBuffer *);

	struct comp_target_image images[MDP_IMAGES];
	AHardwareBuffer *ahb[MDP_IMAGES];
	VkDeviceMemory memory[MDP_IMAGES];
	int buffer_fd[MDP_IMAGES];
	bool ubwc[MDP_IMAGES];
	uint32_t stride_px[MDP_IMAGES];
	int release_fence[MDP_IMAGES]; //!< Signals when the image is no longer scanned out.
	uint32_t next_image;

	int64_t frame_id;
	int64_t last_display_ns;

	//! Timing statistics, logged every few seconds.
	struct
	{
		int64_t since_ns;
		uint32_t presents;
		int64_t fence_wait_ns, gpu_wait_ns, max_gpu_wait_ns, late_ns;
		uint32_t late;
	} stats;
	bool has_init_vulkan;
	bool unblanked; //!< Protected by vsync_thread's mutex.
	bool worn;      //!< Proximity sensor covered and not in standby. Protected by vsync_thread's mutex.
	bool prox_worn; //!< Proximity sensor covered. Protected by vsync_thread's mutex.
	bool standby;   //!< MDP_STANDBY exists. Protected by vsync_thread's mutex.

	bool external;               //!< Another process scans out (external_scanout_active).
	int64_t external_checked_ns; //!< When MDP_EXTERNAL_SCANOUT was last read.
};

static inline struct mdp_target *
mdpt(struct comp_target *ct)
{
	return (struct mdp_target *)ct;
}

static void
close_fd(int *fd)
{
	if (*fd >= 0) {
		close(*fd);
		*fd = -1;
	}
}


/*
 *
 * Vsync.
 *
 */

/*!
 * Panels on while frames come in, off otherwise: when the last app goes away
 * the compositor stops presenting and the display engine would keep scanning
 * out the last frame (OLED burn-in). The GPU runs at full clock only while
 * showing frames; msm-adreno-tz never ramps up without Android's services.
 * Call with vsync_thread's mutex held.
 */
/*!
 * mdss brings the panels back at its default backlight (127 of 255) on every unblank, while the
 * LED class keeps showing the last value written: rewriting that same value does nothing. Write
 * a different level first, then QUEST1_BRIGHTNESS (default 255).
 */
static void
set_brightness(void)
{
	const char *e = getenv("QUEST1_BRIGHTNESS");
	int level = e ? atoi(e) : 255;
	if (level <= 0 || level > 255) {
		return;
	}
	int fd = open("/sys/class/leds/lcd-backlight/brightness", O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		return;
	}
	char buf[8];
	int n = snprintf(buf, sizeof(buf), "%d", level > 1 ? level - 1 : 2);
	if (write(fd, buf, n) < 0 || (n = snprintf(buf, sizeof(buf), "%d", level), write(fd, buf, n) < 0)) {
		U_LOG_W("mdp: cannot set the backlight: %s", strerror(errno));
	}
	close(fd);
}

static void
set_panels_locked(struct mdp_target *t, bool on)
{
	if (on == t->unblanked) {
		return;
	}
	int gov = open("/sys/class/kgsl/kgsl-3d0/devfreq/governor", O_WRONLY | O_CLOEXEC);
	if (gov >= 0) {
		const char *g = on ? "performance" : "msm-adreno-tz";
		if (write(gov, g, strlen(g)) < 0) {
			U_LOG_W("mdp: cannot set the GPU governor: %s", strerror(errno));
		}
		close(gov);
	}
	// An external scanout (MDP_EXTERNAL_SCANOUT) must not commit while the panels power down: a
	// commit queued then never completes and wedges mdss (every later FBIOBLANK times out). So
	// MDP_PANELS_OFF tells it to stop first, and gives its last commit time to complete.
	if (!on) {
		int flag = open(MDP_PANELS_OFF, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
		if (flag >= 0) {
			close(flag);
		}
		os_nanosleep(60 * U_TIME_1MS_IN_NS);
	}
	// Off with HSYNC_SUSPEND: mdss powers the panels off as for POWERDOWN, but the fan driver
	// (drivers/platform/oculus/vs1-board.c) only stops the fan on POWERDOWN or VSYNC_SUSPEND,
	// so it keeps cooling the SoC, which may still be busy, while the headset is off the head.
	if (ioctl(t->fb_fd, FBIOBLANK, on ? FB_BLANK_UNBLANK : FB_BLANK_HSYNC_SUSPEND) < 0) {
		U_LOG_E("mdp: FBIOBLANK %s: %s", on ? "unblank" : "powerdown", strerror(errno));
	}
	if (on) {
		unlink(MDP_PANELS_OFF);
	}
	if (on) {
		unsigned int vsync_on = 1;
		ioctl(t->fb_fd, MSMFB_OVERLAY_VSYNC_CTRL, &vsync_on);
		set_brightness();
	}
	t->unblanked = on;
	U_LOG_W("mdp: panels %s", on ? "on" : "off");
}

static void
update_worn_locked(struct mdp_target *t);

/*!
 * Read the pending powerstate messages and track whether the headset is worn.
 * Call with vsync_thread's mutex held.
 */
static void
read_prox_locked(struct mdp_target *t)
{
	uint8_t buf[256];
	ssize_t n;
	while ((n = read(t->prox_fd, buf, sizeof(buf))) > 0) {
		for (ssize_t off = 0; off + (ssize_t)sizeof(struct syncboss_driver_message) <= n;) {
			struct syncboss_driver_message m;
			memcpy(&m, buf + off, sizeof(m));
			off += m.header_length ? m.header_length : sizeof(m);
			if (m.type != SYNCBOSS_DRIVER_MESSAGE_POWERSTATE_MSG ||
			    (m.data != SYNCBOSS_PROX_EVENT_PROX_ON && m.data != SYNCBOSS_PROX_EVENT_PROX_OFF)) {
				continue;
			}
			bool worn = m.data == SYNCBOSS_PROX_EVENT_PROX_ON;
			if (worn != t->prox_worn) {
				U_LOG_W("mdp: headset %s", worn ? "worn" : "removed");
			}
			t->prox_worn = worn;
		}
	}
	update_worn_locked(t);
}

/*!
 * worn = proximity covered and not in standby; panels off when it turns false.
 * Call with vsync_thread's mutex held.
 */
static void
update_worn_locked(struct mdp_target *t)
{
	bool standby = access(MDP_STANDBY, F_OK) == 0;
	if (standby != t->standby) {
		U_LOG_W("mdp: standby %s", standby ? "on" : "off");
		t->standby = standby;
	}
	t->worn = t->prox_worn && !standby;
	if (!t->worn) {
		set_panels_locked(t, false);
	}
}

/*
 *
 * Frames from vrcompositor's layer (see struct mdp_direct).
 *
 */

//! Call with D->lock held.
static void
direct_send(struct mdp_direct *D, const struct q1d_msg *m)
{
	if (D->client_fd >= 0) {
		(void)send(D->client_fd, m, sizeof(*m), MSG_NOSIGNAL | MSG_DONTWAIT);
	}
}

//! Call with D->lock held.
static void
direct_displayed(struct mdp_direct *D, uint64_t frame)
{
	if (frame == 0) {
		return;
	}
	struct q1d_msg m = {.type = Q1D_DISPLAYED, .frame = frame, .time_ns = os_monotonic_get_ns()};
	direct_send(D, &m);
}

//! Call with D->lock held: images the layer gave us that nothing shows or will show any more.
static void
direct_release_unused(struct mdp_direct *D)
{
	for (uint32_t i = 0; i < D->count; i++) {
		if (!D->held[i] || (int)i == D->latest) {
			continue;
		}
		bool used = false;
		for (int c = 0; c < DIRECT_COMMITS; c++) {
			used |= D->commits[c].release_fence >= 0 && D->commits[c].image == (int)i;
		}
		if (!used) {
			D->held[i] = false;
			struct q1d_msg m = {.type = Q1D_RELEASE, .index = i};
			direct_send(D, &m);
		}
	}
}

//! Call with D->lock held. The panels no longer show (or are about to stop showing) what these commits did.
static void
direct_flush_commits(struct mdp_direct *D)
{
	for (int c = 0; c < DIRECT_COMMITS; c++) {
		if (D->commits[c].retire_fence >= 0) {
			direct_displayed(D, D->commits[c].frame);
		}
		close_fd(&D->commits[c].release_fence);
		close_fd(&D->commits[c].retire_fence);
		D->commits[c].image = -1;
		D->commits[c].out = -1;
		D->commits[c].frame = 0;
	}
}

static void
warp_free(struct mdp_target *t);

//! Call with D->lock held.
static void
direct_reset(struct mdp_target *t)
{
	struct mdp_direct *D = &t->direct;
	direct_flush_commits(D);
	warp_free(t);
	for (uint32_t i = 0; i < Q1D_MAX_IMAGES; i++) {
		for (int h = 0; h < 2; h++) {
			if (D->buf[i][h].ahb != NULL) {
				AHardwareBuffer_release(D->buf[i][h].ahb);
			}
			D->buf[i][h] = (struct direct_buf){0};
		}
		D->held[i] = false;
	}
	D->count = 0;
	D->latest = -1;
	D->fresh = false;
}

static void
direct_disconnect(struct mdp_target *t)
{
	struct mdp_direct *D = &t->direct;
	pthread_mutex_lock(&D->lock);
	close_fd(&D->client_fd);
	direct_reset(t);
	D->left_ns = os_monotonic_get_ns();
	pthread_mutex_unlock(&D->lock);
	U_LOG_W("mdp: the layer of vrcompositor disconnected");
}

//! Receiver thread: accepts the layer, imports its buffers, queues its frames.
static void *
direct_thread_func(void *ptr)
{
	struct mdp_target *t = ptr;
	struct mdp_direct *D = &t->direct;
	pthread_setname_np(pthread_self(), "Quest1 direct");
	while (D->running) {
		struct pollfd p[2] = {{D->listen_fd, POLLIN, 0}, {D->client_fd, POLLIN, 0}};
		if (poll(p, 2, 200) <= 0) {
			continue;
		}
		if (p[0].revents & POLLIN) {
			int fd = accept4(D->listen_fd, NULL, NULL, SOCK_CLOEXEC);
			if (fd >= 0) {
				if (D->client_fd >= 0) {
					direct_disconnect(t);
				}
				pthread_mutex_lock(&D->lock);
				D->client_fd = fd;
				pthread_mutex_unlock(&D->lock);
				U_LOG_W("mdp: the layer of vrcompositor connected");
			}
			continue;
		}
		if (D->client_fd < 0 || p[1].revents == 0) {
			continue;
		}
		struct q1d_msg m;
		ssize_t n = recv(D->client_fd, &m, sizeof(m), 0);
		if (n != (ssize_t)sizeof(m)) {
			direct_disconnect(t);
			continue;
		}
		if (m.type == Q1D_BUFFER && m.index < Q1D_MAX_IMAGES && m.half < 2) {
			AHardwareBuffer *ahb = NULL;
			if (D->recv_handle(D->client_fd, &ahb) != 0 || ahb == NULL) {
				U_LOG_E("mdp: cannot receive a buffer from the layer");
				direct_disconnect(t);
				continue;
			}
			const mdp_native_handle *nh = t->get_native_handle(ahb);
			pthread_mutex_lock(&D->lock);
			struct direct_buf *b = &D->buf[m.index][m.half];
			if (b->ahb != NULL) {
				AHardwareBuffer_release(b->ahb);
			}
			b->ahb = ahb;
			b->fd = nh->data[0]; // gralloc private_handle_t: fd, fd_metadata, magic, flags, ...
			b->stride_px = m.stride_px;
			b->ubwc = m.ubwc;
			D->count = m.count < Q1D_MAX_IMAGES ? m.count : Q1D_MAX_IMAGES;
			D->height = m.height;
			D->bgra = m.bgra;
			pthread_mutex_unlock(&D->lock);
		} else if (m.type == Q1D_FRAME && m.index < D->count) {
			pthread_mutex_lock(&D->lock);
			D->held[m.index] = true;
			D->latest = (int)m.index;
			D->latest_frame.frame = m.frame;
			D->latest_frame.time_ns = m.time_ns;
			D->latest_frame.pose_valid = m.pose_valid != 0;
			memcpy(D->latest_frame.q, m.pose, sizeof(m.pose));
			D->fresh = true;
			direct_release_unused(D);
			pthread_mutex_unlock(&D->lock);
		}
	}
	return NULL;
}

static void
direct_init(struct mdp_target *t, void *nw)
{
	struct mdp_direct *D = &t->direct;
	D->client_fd = -1;
	D->listen_fd = -1;
	D->latest = -1;
	pthread_mutex_init(&D->lock, NULL);
	for (int c = 0; c < DIRECT_COMMITS; c++) {
		D->commits[c].release_fence = D->commits[c].retire_fence = -1;
		D->commits[c].image = -1;
		D->commits[c].out = -1;
	}
	*(void **)&D->recv_handle = android_dlsym(nw, "AHardwareBuffer_recvHandleFromUnixSocket");
	if (D->recv_handle == NULL) {
		U_LOG_W("mdp: no AHardwareBuffer_recvHandleFromUnixSocket: no frames from the layer of vrcompositor");
		return;
	}
	D->listen_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	struct sockaddr_un a = {AF_UNIX, Q1D_SOCKET};
	unlink(Q1D_SOCKET);
	if (D->listen_fd < 0 || bind(D->listen_fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(D->listen_fd, 1) != 0) {
		U_LOG_W("mdp: cannot listen on %s: %s", Q1D_SOCKET, strerror(errno));
		close_fd(&D->listen_fd);
		return;
	}
	chmod(Q1D_SOCKET, 0777); // vrcompositor runs as another user
	D->running = true;
	D->thread_running = pthread_create(&D->thread, NULL, direct_thread_func, t) == 0;
	U_LOG_W("mdp: listening for the frames of vrcompositor on %s", Q1D_SOCKET);
}

static void
direct_destroy(struct mdp_target *t)
{
	struct mdp_direct *D = &t->direct;
	D->running = false;
	if (D->thread_running) {
		pthread_join(D->thread, NULL);
	}
	pthread_mutex_lock(&D->lock);
	close_fd(&D->client_fd);
	direct_reset(t);
	pthread_mutex_unlock(&D->lock);
	if (D->listen_fd >= 0) {
		close_fd(&D->listen_fd);
		unlink(Q1D_SOCKET);
	}
}

//! Call with D->lock held: show the two half buffers b (image `idx` of the layer, or output set `out`) on the
//! panels, true when committed. `frame`: the layer's frame to report as displayed, 0 none.
static bool
direct_commit(struct mdp_target *t, const struct direct_buf *b2, int idx, int out, uint64_t frame)
{
	struct mdp_direct *D = &t->direct;
	if (b2[0].ahb == NULL || b2[1].ahb == NULL) {
		return false;
	}
	struct mdp_input_layer layers[2];
	memset(layers, 0, sizeof(layers));
	const uint32_t pipes[2] = {64, 128}; // DMA0, DMA1: one per panel
	for (int h = 0; h < 2; h++) {
		const struct direct_buf *b = &b2[h];
		struct mdp_input_layer *l = &layers[h];
		l->pipe_ndx = pipes[h];
		l->alpha = 0xff;
		l->blend_op = BLEND_OP_OPAQUE;
		l->color_space = MDP_CSC_ITU_R_709;
		l->src_rect = (struct mdp_rect){0, 0, MDP_HALF, D->height};
		l->dst_rect = (struct mdp_rect){h * MDP_HALF, 0, MDP_HALF, D->height};
		l->buffer.width = b->stride_px;
		l->buffer.height = D->height;
		l->buffer.format = b->ubwc ? MDP_RGBA_8888_UBWC : (D->bgra ? MDP_BGRA_8888 : MDP_RGBA_8888);
		l->buffer.planes[0].fd = b->fd;
		l->buffer.planes[0].stride = b->stride_px * 4;
		l->buffer.plane_count = 1;
		l->buffer.fence = -1;
	}
	struct mdp_layer_commit c;
	memset(&c, 0, sizeof(c));
	c.version = MDP_COMMIT_VERSION_1_0;
	c.commit_v1.input_layers = layers;
	c.commit_v1.input_layer_cnt = 2;
	c.commit_v1.release_fence = -1;
	c.commit_v1.retire_fence = -1;

	// serialized with the panel power changes (they hold the vsync thread's mutex)
	os_thread_helper_lock(&t->vsync_thread);
	bool ok = t->unblanked && t->worn && ioctl(t->fb_fd, MSMFB_ATOMIC_COMMIT, &c) == 0;
	int err = errno;
	bool unblanked = t->unblanked;
	os_thread_helper_unlock(&t->vsync_thread);
	if (!ok) {
		static unsigned errors;
		if (unblanked && errors++ < 5) {
			U_LOG_E("mdp: MSMFB_ATOMIC_COMMIT: %s (layer errors %d %d)", strerror(err), layers[0].error_code,
			        layers[1].error_code);
		}
		return false;
	}
	// a free slot, else the oldest commit's fences are long due: drop them
	int slot = -1;
	for (int k = 0; k < DIRECT_COMMITS && slot < 0; k++) {
		if (D->commits[k].release_fence < 0 && D->commits[k].retire_fence < 0) {
			slot = k;
		}
	}
	if (slot < 0) {
		slot = 0;
		for (int j = 1; j < DIRECT_COMMITS; j++) {
			if (D->commits[j].serial < D->commits[slot].serial) {
				slot = j;
			}
		}
		direct_displayed(D, D->commits[slot].frame);
		close_fd(&D->commits[slot].release_fence);
		close_fd(&D->commits[slot].retire_fence);
	}
	D->commits[slot].release_fence = c.commit_v1.release_fence;
	D->commits[slot].retire_fence = c.commit_v1.retire_fence;
	D->commits[slot].image = idx;
	D->commits[slot].out = out;
	D->commits[slot].frame = frame;
	D->commits[slot].serial = ++D->serial;
	return true;
}

/*
 *
 * Re-projection: the latest frame of the layer, warped to the head pose of the display time.
 *
 * Per eye a grid over the output panel: each vertex's panel point goes through the lens model to a
 * direction in the head frame of the display time, is rotated into the head frame the frame was
 * rendered in (q_delta = conj(q_render) * q_display), and back through the inverse lens model to a
 * point of the frame the layer's buffer holds (already lens-corrected by vrcompositor). The fragment
 * shader takes one bilinear sample. The lens model is the one of drivers/quest1/quest1_hmd.c
 * (hmd_compute_distortion, green channel), in the orientation Monado renders in: the panels show
 * the framebuffer turned by 180 degrees, and the left eye (view 0) is the framebuffer's right half.
 *
 */

#define Q1_PANEL_W_M 0.0594f
#define Q1_PANEL_H_M 0.0660f
#define Q1_METERS_PER_TAN_ANGLE 0.0389f
#define Q1_LENS_TO_SCREEN_X 0.002986f
// Meta's value: the lens centre 2.72 mm above the panel centre. It was mirrored here once, after driver_quest1's
// QUEST1_DIRECT_FLIP bit 16; but the driver's tangents run downwards, so its bit 16 is Meta's geometry. The mirrored
// lens centre (132 px off) made every re-projection shear (2026-11-03, measured against the driver's output).
#define Q1_LENS_TO_SCREEN_Y -0.00272f

static const float q1_k[11] = {1.0f,    1.0374f, 1.0810f, 1.1330f, 1.1970f, 1.2754f,
                               1.3771f, 1.5133f, 1.7018f, 1.9732f, 2.3000f};

//! Scale from screen radius^2 to tan-angle (Meta's CatmullRom10, as quest1_hmd.c).
static float
q1_catmull_rom10(float rsq)
{
	const float *K = q1_k;
	float f = 10.0f * rsq;
	int k = (int)floorf(f);
	k = k < 0 ? 0 : (k > 10 ? 10 : k);
	float t = f - (float)k;
	float p0, m0, p1, m1;
	if (k == 0) {
		p0 = 1.0f;
		m0 = K[1] - K[0];
		p1 = K[1];
		m1 = 0.5f * (K[2] - K[0]);
	} else if (k < 9) {
		p0 = K[k];
		m0 = 0.5f * (K[k + 1] - K[k - 1]);
		p1 = K[k + 1];
		m1 = 0.5f * (K[k + 2] - K[k]);
	} else if (k == 9) {
		p0 = K[9];
		m0 = 0.5f * (K[10] - K[8]);
		p1 = K[10];
		m1 = K[10] - K[9];
	} else {
		p0 = K[10];
		m0 = K[10] - K[9];
		p1 = p0 + m0;
		m1 = m0;
	}
	float omt = 1.0f - t;
	return (p0 * (1.0f + 2.0f * t) + m0 * t) * omt * omt + (p1 * (1.0f + 2.0f * omt) - m1 * omt) * t * t;
}

static void
quat_mul(const float *a, const float *b, float *o)
{
	o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
	o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
	o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
	o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

static void
quat_conj(const float *a, float *o)
{
	o[0] = -a[0];
	o[1] = -a[1];
	o[2] = -a[2];
	o[3] = a[3];
}

static void
quat_normalize(float *q)
{
	float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	if (n < 1e-6f) {
		q[0] = q[1] = q[2] = 0;
		q[3] = 1;
		return;
	}
	for (int i = 0; i < 4; i++) {
		q[i] /= n;
	}
}

static void
quat_rotate(const float *q, const float *v, float *o)
{
	float t[3] = {2 * (q[1] * v[2] - q[2] * v[1]), 2 * (q[2] * v[0] - q[0] * v[2]), 2 * (q[0] * v[1] - q[1] * v[0])};
	o[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
	o[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
	o[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
}

static float
quat_angle_deg(const float *q)
{
	// atan2 of the vector part: acos(w) has no precision left for the tiny angles of a still head
	return 2.0f * atan2f(sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]), fabsf(q[3])) * (180.0f / (float)M_PI);
}

//! Head orientation at `at_ns` (Monado's device space), false when the tracker has none.
static bool
head_orientation(struct mdp_target *t, int64_t at_ns, float *q)
{
	struct xrt_device *xdev = t->base.c->xdev;
	struct xrt_space_relation rel = XRT_SPACE_RELATION_ZERO;
	if (xdev == NULL || xrt_device_get_tracked_pose(xdev, XRT_INPUT_GENERIC_HEAD_POSE, at_ns, &rel) != XRT_SUCCESS ||
	    !(rel.relation_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT)) {
		return false;
	}
	q[0] = rel.pose.orientation.x;
	q[1] = rel.pose.orientation.y;
	q[2] = rel.pose.orientation.z;
	q[3] = rel.pose.orientation.w;
	return true;
}

//! Test and tuning knobs, from the file /tmp/quest1-reproj (read twice a second): "off" shows the layer's frames
//! as they are, "yaw=<deg>" adds a fixed yaw to the re-projection, "thr=<deg>" is the angle under which a fresh
//! frame is shown as it is (default 0.2), "delay=<ms>" the time from the vsync that commits a frame to the
//! pose it is warped to (default 1.5 periods), "sign=<mask>" negates the delta's x/y/z axes (1/2/4),
//! "rx=/ry=/rz=<deg>" a constant rotation R of the head frame against the eye frame (delta' = conj(R) delta R,
//! applied in that order x, y, z), "vel=0" (read by the head tracker, quest1_hmd.c) stops extrapolating Meta's
//! poses with their angular velocity.
static void
warp_read_knobs(struct mdp_direct *D, int64_t now)
{
	if (now - D->knobs.read_ns < 500 * U_TIME_1MS_IN_NS) {
		return;
	}
	D->knobs.read_ns = now;
	D->knobs.off = !MDP_WARP_DEFAULT_ON;
	D->knobs.yaw_deg = 0;
	D->knobs.thr_deg = 0.2f;
	D->knobs.delay_ms = 0;
	D->knobs.sign = 0;
	D->knobs.rx_deg = D->knobs.ry_deg = D->knobs.rz_deg = 0;
	// perf knobs (not the re-projection ones): "early" answers the layer's present wait as soon as the frame is
	// committed instead of when it is on the panels, so vrcompositor starts its next frame one vsync sooner
	// (2026-11-02, head still, panels on: 28 -> 36 fps, present-wait errors 740 -> 40 per 25 s; the default)
	D->knobs.ack_mode = 1;
	FILE *pf = fopen("/tmp/quest1-perf", "r");
	if (pf != NULL) {
		char pb[64] = {0};
		size_t pn = fread(pb, 1, sizeof(pb) - 1, pf);
		fclose(pf);
		pb[pn] = 0;
		if (strstr(pb, "retire") != NULL) {
			D->knobs.ack_mode = 0;
		} else if (strstr(pb, "early") != NULL) {
			D->knobs.ack_mode = 1;
		} else if (strstr(pb, "vsync") != NULL) {
			D->knobs.ack_mode = 2;
		}
	}
	FILE *f = fopen("/tmp/quest1-reproj", "r");
	if (f == NULL) {
		return;
	}
	char buf[160] = {0};
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	if (strstr(buf, "off") != NULL) {
		D->knobs.off = true;
	} else if (strstr(buf, "on") != NULL || buf[0] != 0) {
		D->knobs.off = false;
	}
	const char *p;
	if ((p = strstr(buf, "yaw=")) != NULL) {
		D->knobs.yaw_deg = (float)atof(p + 4);
	}
	if ((p = strstr(buf, "thr=")) != NULL) {
		D->knobs.thr_deg = (float)atof(p + 4);
	}
	if ((p = strstr(buf, "delay=")) != NULL) {
		D->knobs.delay_ms = (float)atof(p + 6);
	}
	if ((p = strstr(buf, "sign=")) != NULL) {
		D->knobs.sign = (uint32_t)atoi(p + 5);
	}
	if ((p = strstr(buf, "rx=")) != NULL) {
		D->knobs.rx_deg = (float)atof(p + 3);
	}
	if ((p = strstr(buf, "ry=")) != NULL) {
		D->knobs.ry_deg = (float)atof(p + 3);
	}
	if ((p = strstr(buf, "rz=")) != NULL) {
		D->knobs.rz_deg = (float)atof(p + 3);
	}
}

//! Inverse of the lens radial function m(r) = r * CR10(r^2) (screen radius -> tangent radius), tabulated once at
//! even steps of m: the warp needs it for every mesh vertex of every frame, and Newton's method on the spline cost
//! ~14 ms of CPU per frame.
#define LENS_INV_N 4096
#define LENS_INV_RMAX 2.0f
static float lens_inv_tab[LENS_INV_N + 1];
static float lens_inv_mmax;

static void
lens_inverse_init(void)
{
	if (lens_inv_mmax > 0) {
		return;
	}
	lens_inv_mmax = LENS_INV_RMAX * q1_catmull_rom10(LENS_INV_RMAX * LENS_INV_RMAX);
	// m(r) is monotonic: walk r finely and fill the table entries it passes
	const int steps = 64 * LENS_INV_N;
	float r_prev = 0, m_prev = 0;
	int k = 0;
	lens_inv_tab[k++] = 0;
	for (int i = 1; i <= steps && k <= LENS_INV_N; i++) {
		float r = LENS_INV_RMAX * (float)i / steps;
		float m = r * q1_catmull_rom10(r * r);
		while (k <= LENS_INV_N && (float)k / LENS_INV_N * lens_inv_mmax <= m) {
			float target = (float)k / LENS_INV_N * lens_inv_mmax;
			lens_inv_tab[k++] = m > m_prev ? r_prev + (r - r_prev) * (target - m_prev) / (m - m_prev) : r;
		}
		r_prev = r, m_prev = m;
	}
	while (k <= LENS_INV_N) {
		lens_inv_tab[k++] = LENS_INV_RMAX;
	}
}

//! Inverse of the lens: the screen point (x, y) whose tangent direction is (tx, ty).
static void
lens_inverse(float tx, float ty, float *x, float *y)
{
	float m = sqrtf(tx * tx + ty * ty);
	if (m < 1e-6f) {
		*x = *y = 0;
		return;
	}
	float f = m / lens_inv_mmax * LENS_INV_N;
	int k = (int)f;
	float r = k >= LENS_INV_N ? lens_inv_tab[LENS_INV_N] * f / LENS_INV_N
	                          : lens_inv_tab[k] + (lens_inv_tab[k + 1] - lens_inv_tab[k]) * (f - (float)k);
	*x = tx / m * r;
	*y = ty / m * r;
}

//! Meta's chromatic aberration (docs/quest1-lens.md): red and blue tangents scaled against green's
static void
q1_chroma(float rsq, float out[3])
{
	out[0] = 0.996f - 0.007f * rsq;
	out[1] = 1.0f;
	out[2] = 1.006f + 0.020f * rsq;
}

//! The mesh of one half buffer (h) of the output: WARP_VTX_FLOATS per vertex, x y (NDC) then u v (texture
//! coordinates) for red, green and blue.
//! D->lens: each vertex's view direction per channel through the lens model, turned by `delta`, projected into
//! the undistorted eye image (its tangent extent). Else the layer's image is already lens-corrected: the
//! direction goes back through the inverse lens model (green only, the same coordinates for the 3 channels).
static void
warp_build_mesh(const struct mdp_direct *D, int h, const float *delta, float *out)
{
	const int view = 1 - h; // half 0 (x = 0) is the right eye, view 1
	const float sx = view == 0 ? -1.0f : 1.0f;
	// each vertex's view direction through the lens does not change: computed once per half
	static float dirs[2][(WARP_GRID + 1) * (WARP_GRID + 1)][3][2];
	static bool have_dirs[2];
	lens_inverse_init();
	if (!have_dirs[h]) {
		for (int j = 0; j <= WARP_GRID; j++) {
			for (int i = 0; i <= WARP_GRID; i++) {
				float u = 1.0f - (float)i / WARP_GRID, v = 1.0f - (float)j / WARP_GRID;
				float x = ((u - 0.5f) * Q1_PANEL_W_M + sx * Q1_LENS_TO_SCREEN_X) / Q1_METERS_PER_TAN_ANGLE;
				float y = ((0.5f - v) * Q1_PANEL_H_M + Q1_LENS_TO_SCREEN_Y) / Q1_METERS_PER_TAN_ANGLE;
				float rsq = x * x + y * y, s = q1_catmull_rom10(rsq), c[3];
				q1_chroma(rsq, c);
				for (int k = 0; k < 3; k++) {
					dirs[h][j * (WARP_GRID + 1) + i][k][0] = x * s * c[k];
					dirs[h][j * (WARP_GRID + 1) + i][k][1] = y * s * c[k];
				}
			}
		}
		have_dirs[h] = true;
	}
	const float tl = D->tan_l[h], tr = D->tan_r[h], tu = D->tan_u[h], td = D->tan_d[h];
	for (int j = 0; j <= WARP_GRID; j++) {
		for (int i = 0; i <= WARP_GRID; i++) {
			float fx = (float)i / WARP_GRID, fy = (float)j / WARP_GRID;
			float *vtx = out + WARP_VTX_FLOATS * (j * (WARP_GRID + 1) + i);
			vtx[0] = 2.0f * fx - 1.0f;
			vtx[1] = 2.0f * fy - 1.0f;
			for (int k = 0; k < 3; k++) {
				if (!D->lens && k != 1) {
					continue;
				}
				const float *dv = dirs[h][j * (WARP_GRID + 1) + i][k];
				float d[3] = {dv[0], dv[1], -1.0f}, r[3];
				quat_rotate(delta, d, r);
				float tu_out = -10.0f, tv_out = -10.0f; // behind the eye: black
				if (r[2] < -0.05f) {
					float tx = r[0] / -r[2], ty = r[1] / -r[2];
					if (D->lens) {
						// the undistorted image as vrcompositor leaves it in the half: upright, x along
						// the buffer's x, the top of the view (tu) in the first row
						tu_out = (tx - tl) / (tr - tl);
						tv_out = (tu - ty) / (tu - td);
					} else {
						float x2, y2;
						lens_inverse(tx, ty, &x2, &y2);
						float su = (x2 * Q1_METERS_PER_TAN_ANGLE - sx * Q1_LENS_TO_SCREEN_X) / Q1_PANEL_W_M + 0.5f;
						float sv = 0.5f - (y2 * Q1_METERS_PER_TAN_ANGLE - Q1_LENS_TO_SCREEN_Y) / Q1_PANEL_H_M;
						tu_out = 1.0f - su;
						tv_out = 1.0f - sv;
					}
				}
				vtx[2 + 2 * k] = tu_out;
				vtx[3 + 2 * k] = tv_out;
			}
			if (!D->lens) {
				vtx[2] = vtx[6] = vtx[4];
				vtx[3] = vtx[7] = vtx[5];
			}
		}
	}
}

static uint32_t
warp_memory_type(struct vk_bundle *vk, uint32_t bits, VkMemoryPropertyFlags want)
{
	for (uint32_t i = 0; i < vk->device_memory_props.memoryTypeCount; i++) {
		if ((bits & (1u << i)) && (vk->device_memory_props.memoryTypes[i].propertyFlags & want) == want) {
			return i;
		}
	}
	return UINT32_MAX;
}

static void
warp_free_buf_vk(struct vk_bundle *vk, struct direct_buf *b)
{
	if (b->fb != VK_NULL_HANDLE) {
		vk->vkDestroyFramebuffer(vk->device, b->fb, NULL);
	}
	if (b->view != VK_NULL_HANDLE) {
		vk->vkDestroyImageView(vk->device, b->view, NULL);
	}
	if (b->image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, b->image, NULL);
	}
	if (b->mem != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, b->mem, NULL);
	}
	b->fb = VK_NULL_HANDLE;
	b->view = VK_NULL_HANDLE;
	b->image = VK_NULL_HANDLE;
	b->mem = VK_NULL_HANDLE;
	b->set = VK_NULL_HANDLE;
}

//! Import the gralloc buffer as a 1440 x height R8G8B8A8_UNORM image (sampled, or rendered to).
static bool
warp_import(struct mdp_target *t, struct direct_buf *b, bool output)
{
	struct mdp_direct *D = &t->direct;
	struct vk_bundle *vk = &t->base.c->base.vk;
	PFN_vkGetAndroidHardwareBufferPropertiesANDROID get_props =
	    (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)vk->vkGetDeviceProcAddr(
	        vk->device, "vkGetAndroidHardwareBufferPropertiesANDROID");
	if (get_props == NULL) {
		return false;
	}
	VkAndroidHardwareBufferFormatPropertiesANDROID fmt = {
	    .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
	VkAndroidHardwareBufferPropertiesANDROID props = {
	    .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID, .pNext = &fmt};
	if (get_props(vk->device, b->ahb, &props) != VK_SUCCESS) {
		return false;
	}
	VkExternalMemoryImageCreateInfo ext = {
	    .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
	    .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
	};
	VkImageCreateInfo ici = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .pNext = &ext,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = VK_FORMAT_R8G8B8A8_UNORM,
	    .extent = {MDP_HALF, D->height, 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = output ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_SAMPLED_BIT,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if (vk->vkCreateImage(vk->device, &ici, NULL, &b->image) != VK_SUCCESS) {
		return false;
	}
	VkImportAndroidHardwareBufferInfoANDROID imp = {
	    .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, .buffer = b->ahb};
	VkMemoryDedicatedAllocateInfo ded = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .pNext = &imp, .image = b->image};
	VkMemoryAllocateInfo mai = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .pNext = &ded,
	    .allocationSize = props.allocationSize,
	    .memoryTypeIndex = (uint32_t)__builtin_ctz(props.memoryTypeBits),
	};
	if (vk->vkAllocateMemory(vk->device, &mai, NULL, &b->mem) != VK_SUCCESS ||
	    vk->vkBindImageMemory(vk->device, b->image, b->mem, 0) != VK_SUCCESS) {
		warp_free_buf_vk(vk, b);
		return false;
	}
	VkImageViewCreateInfo vci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image = b->image,
	    .viewType = VK_IMAGE_VIEW_TYPE_2D,
	    .format = VK_FORMAT_R8G8B8A8_UNORM,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	// mdss scans MDP_RGBA_8888_UBWC out with red and blue swapped (blue shows orange): with UBWC
	// outputs, the warp samples its (linear) inputs swapped back
	if (!output && D->out[0][0].ubwc) {
		vci.components = (VkComponentMapping){VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G,
		                                      VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_A};
	}
	if (vk->vkCreateImageView(vk->device, &vci, NULL, &b->view) != VK_SUCCESS) {
		warp_free_buf_vk(vk, b);
		return false;
	}
	if (output) {
		VkFramebufferCreateInfo fci = {
		    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		    .renderPass = D->vk.rp,
		    .attachmentCount = 1,
		    .pAttachments = &b->view,
		    .width = MDP_HALF,
		    .height = D->height,
		    .layers = 1,
		};
		if (vk->vkCreateFramebuffer(vk->device, &fci, NULL, &b->fb) != VK_SUCCESS) {
			warp_free_buf_vk(vk, b);
			return false;
		}
	} else {
		VkDescriptorSetAllocateInfo dai = {
		    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		    .descriptorPool = D->vk.dpool,
		    .descriptorSetCount = 1,
		    .pSetLayouts = &D->vk.dsl,
		};
		if (vk->vkAllocateDescriptorSets(vk->device, &dai, &b->set) != VK_SUCCESS) {
			warp_free_buf_vk(vk, b);
			return false;
		}
		VkDescriptorImageInfo ii = {D->vk.sampler, b->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		VkWriteDescriptorSet w = {
		    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		    .dstSet = b->set,
		    .descriptorCount = 1,
		    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		    .pImageInfo = &ii,
		};
		vk->vkUpdateDescriptorSets(vk->device, 1, &w, 0, NULL);
	}
	return true;
}

//! Call with D->lock held: the Vulkan objects of the warp pass and the output buffers. False (once) on failure.
static void
warp_lens_setup(struct mdp_target *t);

static bool
warp_init(struct mdp_target *t)
{
	struct mdp_direct *D = &t->direct;
	struct vk_bundle *vk = &t->base.c->base.vk;
	if (D->vk.ready) {
		return true;
	}
	if (D->vk.failed || !t->has_init_vulkan || D->height == 0) {
		return false;
	}
	D->vk.failed = true;
	struct direct_vk *V = &D->vk;

	VkAttachmentDescription att = {
	    .format = VK_FORMAT_R8G8B8A8_UNORM,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
	    .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	    .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	    .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
	};
	VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
	                            .pColorAttachments = &ref};
	VkRenderPassCreateInfo rpci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
	                               .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sub};
	VkSamplerCreateInfo sci = {
	    .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
	    .magFilter = VK_FILTER_LINEAR,
	    .minFilter = VK_FILTER_LINEAR,
	    .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
	    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
	    .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
	    .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
	    .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
	};
	VkDescriptorSetLayoutBinding bind = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
	VkDescriptorSetLayoutCreateInfo dslci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
	                                         .bindingCount = 1, .pBindings = &bind};
	VkDescriptorPoolSize psz = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * Q1D_MAX_IMAGES};
	VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
	                                   .maxSets = 2 * Q1D_MAX_IMAGES, .poolSizeCount = 1, .pPoolSizes = &psz};
	if (vk->vkCreateRenderPass(vk->device, &rpci, NULL, &V->rp) != VK_SUCCESS ||
	    vk->vkCreateSampler(vk->device, &sci, NULL, &V->sampler) != VK_SUCCESS ||
	    vk->vkCreateDescriptorSetLayout(vk->device, &dslci, NULL, &V->dsl) != VK_SUCCESS ||
	    vk->vkCreateDescriptorPool(vk->device, &dpci, NULL, &V->dpool) != VK_SUCCESS) {
		U_LOG_E("mdp: warp: render pass, sampler or descriptors failed");
		return false;
	}
	VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
	                                   .pSetLayouts = &V->dsl};
	VkShaderModuleCreateInfo vsci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	                                 .codeSize = sizeof(q1_warp_vert_spv), .pCode = q1_warp_vert_spv};
	// three samples (one per channel) only when the warp does the lens' chromatic aberration
	warp_lens_setup(t);
	VkShaderModuleCreateInfo fsci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	                                 .codeSize = D->lens ? sizeof(q1_warp_frag_spv) : sizeof(q1_warp_frag1_spv),
	                                 .pCode = D->lens ? q1_warp_frag_spv : q1_warp_frag1_spv};
	VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
	if (vk->vkCreatePipelineLayout(vk->device, &plci, NULL, &V->pl) != VK_SUCCESS ||
	    vk->vkCreateShaderModule(vk->device, &vsci, NULL, &vs) != VK_SUCCESS ||
	    vk->vkCreateShaderModule(vk->device, &fsci, NULL, &fs) != VK_SUCCESS) {
		U_LOG_E("mdp: warp: pipeline layout or shaders failed");
		return false;
	}
	VkPipelineShaderStageCreateInfo stages[2] = {
	    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL},
	    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL},
	};
	VkVertexInputBindingDescription vb = {0, WARP_VTX_FLOATS * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
	VkVertexInputAttributeDescription va[4] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
	                                           {1, 0, VK_FORMAT_R32G32_SFLOAT, 2 * sizeof(float)},
	                                           {2, 0, VK_FORMAT_R32G32_SFLOAT, 4 * sizeof(float)},
	                                           {3, 0, VK_FORMAT_R32G32_SFLOAT, 6 * sizeof(float)}};
	VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	                                           .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
	                                           .vertexAttributeDescriptionCount = 4, .pVertexAttributeDescriptions = va};
	VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
	                                             .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
	VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
	                                        .viewportCount = 1, .scissorCount = 1};
	VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
	                                             .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
	                                             .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
	VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
	                                           .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
	VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
	VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
	                                          .attachmentCount = 1, .pAttachments = &cba};
	VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
	                                       .dynamicStateCount = 2, .pDynamicStates = dyn};
	VkGraphicsPipelineCreateInfo gpci = {
	    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
	    .stageCount = 2,
	    .pStages = stages,
	    .pVertexInputState = &vi,
	    .pInputAssemblyState = &ia,
	    .pViewportState = &vp,
	    .pRasterizationState = &rs,
	    .pMultisampleState = &ms,
	    .pColorBlendState = &cb,
	    .pDynamicState = &ds,
	    .layout = V->pl,
	    .renderPass = V->rp,
	};
	VkResult r = vk->vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &gpci, NULL, &V->pipe);
	vk->vkDestroyShaderModule(vk->device, vs, NULL);
	vk->vkDestroyShaderModule(vk->device, fs, NULL);
	if (r != VK_SUCCESS) {
		U_LOG_E("mdp: warp: vkCreateGraphicsPipelines: %d", r);
		return false;
	}

	// vertices (host visible, rewritten for every warp) and indices
	const uint32_t nverts = 2 * (WARP_GRID + 1) * (WARP_GRID + 1);
	V->nidx = WARP_GRID * WARP_GRID * 6;
	VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = nverts * WARP_VTX_FLOATS * sizeof(float),
	                          .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
	VkBufferCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = V->nidx * sizeof(uint16_t),
	                          .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
	VkBuffer *bufs[2] = {&V->vbuf, &V->ibuf};
	VkDeviceMemory *mems[2] = {&V->vmem, &V->imem};
	VkBufferCreateInfo *bcis[2] = {&bci, &ici};
	for (int k = 0; k < 2; k++) {
		VkMemoryRequirements mr;
		if (vk->vkCreateBuffer(vk->device, bcis[k], NULL, bufs[k]) != VK_SUCCESS) {
			return false;
		}
		vk->vkGetBufferMemoryRequirements(vk->device, *bufs[k], &mr);
		uint32_t type = warp_memory_type(vk, mr.memoryTypeBits,
		                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
		                            .memoryTypeIndex = type};
		void *map = NULL;
		if (type == UINT32_MAX || vk->vkAllocateMemory(vk->device, &mai, NULL, mems[k]) != VK_SUCCESS ||
		    vk->vkBindBufferMemory(vk->device, *bufs[k], *mems[k], 0) != VK_SUCCESS ||
		    vk->vkMapMemory(vk->device, *mems[k], 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS) {
			U_LOG_E("mdp: warp: vertex buffers failed");
			return false;
		}
		if (k == 0) {
			V->vptr = map;
		} else {
			uint16_t *idx = map;
			for (int j = 0; j < WARP_GRID; j++) {
				for (int i = 0; i < WARP_GRID; i++) {
					uint16_t a = (uint16_t)(j * (WARP_GRID + 1) + i), b = a + 1, c = a + WARP_GRID + 1, d = c + 1;
					uint16_t *q = idx + 6 * (j * WARP_GRID + i);
					q[0] = a, q[1] = b, q[2] = c, q[3] = b, q[4] = d, q[5] = c;
				}
			}
		}
	}

	VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	                                .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
	                                .queueFamilyIndex = vk->main_queue->family_index};
	VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkQueryPoolCreateInfo qpci = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
	                              .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2};
	if (vk->vkCreateCommandPool(vk->device, &cpci, NULL, &V->cpool) != VK_SUCCESS ||
	    vk->vkCreateFence(vk->device, &fci, NULL, &V->fence) != VK_SUCCESS ||
	    vk->vkCreateQueryPool(vk->device, &qpci, NULL, &V->qp) != VK_SUCCESS) {
		U_LOG_E("mdp: warp: command pool, fence or queries failed");
		return false;
	}
	VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	                                    .commandPool = V->cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	                                    .commandBufferCount = 1};
	if (vk->vkAllocateCommandBuffers(vk->device, &cbai, &V->cmd) != VK_SUCCESS) {
		return false;
	}

	// the output buffers: one per panel, as the layer's
	for (int k = 0; k < WARP_OUT_SETS; k++) {
		for (int h = 0; h < 2; h++) {
			struct direct_buf *b = &D->out[k][h];
			AHardwareBuffer_Desc desc = {
			    .width = MDP_HALF,
			    .height = D->height,
			    .layers = 1,
			    .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
			    .usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
			             AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY,
			};
			// UBWC outputs (gralloc's vendor usage bit, AHARDWAREBUFFER_USAGE_VENDOR_0): the warp pass costs
			// ~2.6 ms of GPU instead of ~3.9 ms into linear buffers (2026-11-02). QUEST1_WARP_USAGE=<hex>
			// replaces the extra usage bits (0: linear outputs).
			const char *ue = getenv("QUEST1_WARP_USAGE");
			desc.usage |= ue != NULL ? strtoull(ue, NULL, 16) : 0x10000000u;
			if (AHardwareBuffer_allocate(&desc, &b->ahb) != 0) {
				U_LOG_E("mdp: warp: AHardwareBuffer_allocate failed");
				return false;
			}
			AHardwareBuffer_describe(b->ahb, &desc);
			const mdp_native_handle *nh = t->get_native_handle(b->ahb);
			b->fd = nh->data[0];
			b->ubwc = ((uint32_t)nh->data[3] & PRIV_FLAGS_UBWC_ALIGNED) != 0;
			b->stride_px = desc.stride;
			if (!warp_import(t, b, true)) {
				U_LOG_E("mdp: warp: output import failed");
				return false;
			}
		}
	}
	U_LOG_W("mdp: warp ready: %d x 2 output buffers %ux%u (%s), %dx%d grid", WARP_OUT_SETS, MDP_HALF, D->height,
	        D->out[0][0].ubwc ? "UBWC" : "linear", WARP_GRID, WARP_GRID);
	V->failed = false;
	V->ready = true;
	return true;
}

//! Call with D->lock held, GPU idle: free everything of warp_init and the imports.
static void
warp_free(struct mdp_target *t)
{
	struct mdp_direct *D = &t->direct;
	struct vk_bundle *vk = &t->base.c->base.vk;
	struct direct_vk *V = &D->vk;
	if (!t->has_init_vulkan) {
		return;
	}
	for (int i = 0; i < Q1D_MAX_IMAGES; i++) {
		for (int h = 0; h < 2; h++) {
			warp_free_buf_vk(vk, &D->buf[i][h]);
		}
	}
	for (int k = 0; k < WARP_OUT_SETS; k++) {
		for (int h = 0; h < 2; h++) {
			struct direct_buf *b = &D->out[k][h];
			warp_free_buf_vk(vk, b);
			if (b->ahb != NULL) {
				AHardwareBuffer_release(b->ahb);
			}
			*b = (struct direct_buf){0};
		}
	}
	if (V->vmem != VK_NULL_HANDLE && V->vptr != NULL) {
		vk->vkUnmapMemory(vk->device, V->vmem);
	}
	if (V->imem != VK_NULL_HANDLE) {
		vk->vkUnmapMemory(vk->device, V->imem);
	}
	if (V->vbuf != VK_NULL_HANDLE) vk->vkDestroyBuffer(vk->device, V->vbuf, NULL);
	if (V->ibuf != VK_NULL_HANDLE) vk->vkDestroyBuffer(vk->device, V->ibuf, NULL);
	if (V->vmem != VK_NULL_HANDLE) vk->vkFreeMemory(vk->device, V->vmem, NULL);
	if (V->imem != VK_NULL_HANDLE) vk->vkFreeMemory(vk->device, V->imem, NULL);
	if (V->qp != VK_NULL_HANDLE) vk->vkDestroyQueryPool(vk->device, V->qp, NULL);
	if (V->fence != VK_NULL_HANDLE) vk->vkDestroyFence(vk->device, V->fence, NULL);
	if (V->cpool != VK_NULL_HANDLE) vk->vkDestroyCommandPool(vk->device, V->cpool, NULL);
	if (V->pipe != VK_NULL_HANDLE) vk->vkDestroyPipeline(vk->device, V->pipe, NULL);
	if (V->pl != VK_NULL_HANDLE) vk->vkDestroyPipelineLayout(vk->device, V->pl, NULL);
	if (V->dpool != VK_NULL_HANDLE) vk->vkDestroyDescriptorPool(vk->device, V->dpool, NULL);
	if (V->dsl != VK_NULL_HANDLE) vk->vkDestroyDescriptorSetLayout(vk->device, V->dsl, NULL);
	if (V->sampler != VK_NULL_HANDLE) vk->vkDestroySampler(vk->device, V->sampler, NULL);
	if (V->rp != VK_NULL_HANDLE) vk->vkDestroyRenderPass(vk->device, V->rp, NULL);
	*V = (struct direct_vk){0};
}

//! Call with D->lock held: a free output set (no commit shows it), or -1.
static int
warp_free_set(struct mdp_direct *D)
{
	for (int k = 0; k < WARP_OUT_SETS; k++) {
		bool used = false;
		for (int c = 0; c < DIRECT_COMMITS; c++) {
			used |= D->commits[c].out == k && (D->commits[c].release_fence >= 0 || D->commits[c].retire_fence >= 0);
		}
		if (!used) {
			return k;
		}
	}
	return -1;
}

//! Reads WARP_LENS_FILE and the views' tangent extents once (D->lens).
static void
warp_lens_setup(struct mdp_target *t)
{
	struct mdp_direct *D = &t->direct;
	if (D->lens_read) {
		return;
	}
	D->lens_read = true;
	FILE *lf = fopen(WARP_LENS_FILE, "r");
	D->lens = lf != NULL && fgetc(lf) == '1';
	if (lf != NULL) {
		fclose(lf);
	}
	struct xrt_device *xdev = t->base.c->xdev;
	if (D->lens && (xdev == NULL || xdev->hmd == NULL)) {
		U_LOG_E("mdp: warp: no HMD views, the lens distortion stays with SteamVR");
		D->lens = false;
	}
	if (!D->lens) {
		return;
	}
	for (int h = 0; h < 2; h++) {
		// driver_quest1 projects the view's angles (its QUEST1_DIRECT_FLIP 16 swaps up and down in its
		// downward-running tangents: the same extent)
		const struct xrt_fov *f = &xdev->hmd->distortion.fov[1 - h];
		D->tan_l[h] = tanf(f->angle_left);
		D->tan_r[h] = tanf(f->angle_right);
		D->tan_u[h] = tanf(f->angle_up);
		D->tan_d[h] = tanf(f->angle_down);
	}
	U_LOG_W("mdp: warp: lens distortion here (%s): view 0 tangents l %.3f r %.3f u %.3f d %.3f", WARP_LENS_FILE,
	        D->tan_l[1], D->tan_r[1], D->tan_u[1], D->tan_d[1]);
}

//! Call with D->lock held: draw the latest frame warped by `delta` into output set `o`. GPU time in ms.
static bool
warp_render(struct mdp_target *t, int o, const float *delta, double *gpu_ms)
{
	struct mdp_direct *D = &t->direct;
	struct vk_bundle *vk = &t->base.c->base.vk;
	struct direct_vk *V = &D->vk;
	struct direct_buf *src = D->buf[D->latest];
	for (int h = 0; h < 2; h++) {
		if (src[h].image == VK_NULL_HANDLE && !warp_import(t, &src[h], false)) {
			U_LOG_E("mdp: warp: cannot import the layer's buffer");
			return false;
		}
	}
	int64_t tm0 = os_monotonic_get_ns();
	for (int h = 0; h < 2; h++) {
		warp_build_mesh(D, h, delta, V->vptr + h * WARP_VTX_FLOATS * (WARP_GRID + 1) * (WARP_GRID + 1));
	}
	D->stats.mesh_ms_sum += (double)(os_monotonic_get_ns() - tm0) / 1e6;

	vk->vkResetCommandPool(vk->device, V->cpool, 0);
	VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	                               .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	VkCommandBuffer cmd = V->cmd;
	vk->vkBeginCommandBuffer(cmd, &bi);
	vk->vkCmdResetQueryPool(cmd, V->qp, 0, 2);
	vk->vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, V->qp, 0);
	for (int h = 0; h < 2; h++) {
		VkImageMemoryBarrier ib = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
		    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, // the layer rendered it in another process
		    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		    .image = src[h].image,
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
		vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
		                         NULL, 0, NULL, 1, &ib);
		VkRenderPassBeginInfo rpbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		                              .renderPass = V->rp,
		                              .framebuffer = D->out[o][h].fb,
		                              .renderArea = {{0, 0}, {MDP_HALF, D->height}}};
		vk->vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
		VkViewport vp = {0, 0, (float)MDP_HALF, (float)D->height, 0, 1};
		VkRect2D sc = {{0, 0}, {MDP_HALF, D->height}};
		vk->vkCmdSetViewport(cmd, 0, 1, &vp);
		vk->vkCmdSetScissor(cmd, 0, 1, &sc);
		vk->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, V->pipe);
		vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, V->pl, 0, 1, &src[h].set, 0, NULL);
		VkDeviceSize off = (VkDeviceSize)h * WARP_VTX_FLOATS * sizeof(float) * (WARP_GRID + 1) * (WARP_GRID + 1);
		vk->vkCmdBindVertexBuffers(cmd, 0, 1, &V->vbuf, &off);
		vk->vkCmdBindIndexBuffer(cmd, V->ibuf, 0, VK_INDEX_TYPE_UINT16);
		vk->vkCmdDrawIndexed(cmd, V->nidx, 1, 0, 0, 0);
		vk->vkCmdEndRenderPass(cmd);
	}
	vk->vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, V->qp, 1);
	vk->vkEndCommandBuffer(cmd);

	VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
	int64_t tw0 = os_monotonic_get_ns();
	vk_queue_lock(vk->main_queue);
	VkResult r = vk->vkQueueSubmit(vk->main_queue->queue, 1, &si, V->fence);
	vk_queue_unlock(vk->main_queue);
	if (r != VK_SUCCESS) {
		U_LOG_E("mdp: warp: vkQueueSubmit: %d", r);
		return false;
	}
	r = vk->vkWaitForFences(vk->device, 1, &V->fence, VK_TRUE, 50 * U_TIME_1MS_IN_NS);
	vk->vkResetFences(vk->device, 1, &V->fence);
	D->stats.wait_ms_sum += (double)(os_monotonic_get_ns() - tw0) / 1e6;
	if (r != VK_SUCCESS) {
		U_LOG_E("mdp: warp: the GPU did not finish in 50 ms (%d)", r);
		return false;
	}
	uint64_t ts[2];
	if (vk->vkGetQueryPoolResults(vk->device, V->qp, 0, 2, sizeof(ts), ts, sizeof(uint64_t),
	                              VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
		*gpu_ms = (double)(ts[1] - ts[0]) * vk->features.timestamp_period * 1e-6;
	}
	return true;
}

//! debug: `touch /tmp/quest1-snap-monado` writes what the next warped commit shows to /tmp/quest1-snap-monado.ppm
static void
warp_snapshot(struct mdp_direct *D, int o)
{
	if (unlink("/tmp/quest1-snap-monado") != 0) {
		return;
	}
	if (D->out[o][0].ubwc || D->out[o][1].ubwc) {
		U_LOG_W("mdp: snapshot of a UBWC buffer not supported");
		return;
	}
	uint32_t w = MDP_W, h = D->height;
	uint8_t *p[2];
	size_t size[2];
	for (int k = 0; k < 2; k++) {
		size[k] = (size_t)D->out[o][k].stride_px * 4 * h;
		p[k] = mmap(NULL, size[k], PROT_READ, MAP_SHARED, D->out[o][k].fd, 0);
	}
	FILE *f = p[0] != MAP_FAILED && p[1] != MAP_FAILED ? fopen("/tmp/quest1-snap-monado.ppm.tmp", "wb") : NULL;
	if (f) {
		fprintf(f, "P6\n%u %u\n255\n", w, h);
		uint8_t *row = malloc(w * 3);
		for (uint32_t y = 0; y < h; y++) {
			for (uint32_t x = 0; x < w; x++) {
				int k = x >= MDP_HALF;
				memcpy(row + x * 3, p[k] + (size_t)y * D->out[o][k].stride_px * 4 + (x - k * MDP_HALF) * 4, 3);
			}
			fwrite(row, 1, w * 3, f);
		}
		free(row);
		fclose(f);
		rename("/tmp/quest1-snap-monado.ppm.tmp", "/tmp/quest1-snap-monado.ppm");
		U_LOG_W("mdp: snapshot written");
	}
	for (int k = 0; k < 2; k++) {
		if (p[k] != MAP_FAILED) {
			munmap(p[k], size[k]);
		}
	}
}

/*!
 * Vsync thread, before it waits: file descriptors of the commits to watch: the retire fence signals
 * when the commit is on the panels (frame displayed), the release fence when it was replaced
 * (image free). Returns how many were put in p.
 */
static int
direct_collect_fences(struct mdp_target *t, struct pollfd *p, int max)
{
	struct mdp_direct *D = &t->direct;
	int n = 0;
	pthread_mutex_lock(&D->lock);
	for (int c = 0; c < DIRECT_COMMITS && n + 2 <= max; c++) {
		if (D->commits[c].retire_fence >= 0) {
			p[n++] = (struct pollfd){D->commits[c].retire_fence, POLLIN, 0};
		}
		if (D->commits[c].release_fence >= 0) {
			p[n++] = (struct pollfd){D->commits[c].release_fence, POLLIN, 0};
		}
	}
	pthread_mutex_unlock(&D->lock);
	return n;
}

//! After the wait: handle the signaled fences of direct_collect_fences.
static void
direct_handle_fences(struct mdp_target *t, const struct pollfd *p, int n)
{
	struct mdp_direct *D = &t->direct;
	pthread_mutex_lock(&D->lock);
	for (int k = 0; k < n; k++) {
		if (p[k].revents == 0) {
			continue;
		}
		for (int c = 0; c < DIRECT_COMMITS; c++) {
			if (D->commits[c].retire_fence == p[k].fd) {
				direct_displayed(D, D->commits[c].frame);
				close_fd(&D->commits[c].retire_fence);
			} else if (D->commits[c].release_fence == p[k].fd) {
				close_fd(&D->commits[c].release_fence);
				D->commits[c].image = -1;
				D->commits[c].out = -1;
			}
		}
	}
	direct_release_unused(D);
	pthread_mutex_unlock(&D->lock);
}

//! The frame a commit reports as displayed when it retires (0 none). Only the "retire" ack mode uses the retire
//! fence; the others report the frame themselves (`direct_ack_early`: at the commit, or at the next vsync, which
//! is when the commit is latched). Reusing a buffer never depends on this ack: the layer gets a buffer back
//! with Q1D_RELEASE, sent when the commit's release fence has signaled (direct_release_unused).
static uint64_t
direct_ack_frame(struct mdp_direct *D)
{
	return D->fresh && D->knobs.ack_mode == 0 ? D->latest_frame.frame : 0;
}

static void
direct_ack_early(struct mdp_direct *D)
{
	if (!D->fresh || D->knobs.ack_mode == 0) {
		return;
	}
	if (D->knobs.ack_mode == 1) {
		direct_displayed(D, D->latest_frame.frame);
	} else {
		if (D->ack_pending != 0) {
			direct_displayed(D, D->ack_pending); // a newer frame replaced it before its vsync
		}
		D->ack_pending = D->latest_frame.frame;
	}
}

/*!
 * Vsync thread, every wake-up (`vsync` when a vsync event came, else 0): puts the frames on the panels.
 * Called without the vsync thread's mutex.
 *
 * At a vsync, the next one shows what is committed now: a fresh frame of the layer whose render pose
 * is close to the head pose of that time is committed as it is (no copy, no GPU); otherwise the latest
 * frame is warped to that pose (every vsync, so a frame that stays is re-projected again and again).
 */
static void
direct_tick(struct mdp_target *t, int64_t vsync)
{
	struct mdp_direct *D = &t->direct;
	if (D->client_fd < 0) {
		return;
	}
	pthread_mutex_lock(&D->lock);
	if (D->client_fd < 0) {
		pthread_mutex_unlock(&D->lock);
		return;
	}
	int64_t now = os_monotonic_get_ns();
	os_thread_helper_lock(&t->vsync_thread);
	bool worn = t->worn;
	if (worn && D->latest >= 0) {
		t->last_present_ns = now; // keeps the panels on (see the vsync thread)
		if (!t->unblanked) {
			set_panels_locked(t, true);
			D->need_refresh = true;
		}
	}
	bool on = t->unblanked && worn;
	os_thread_helper_unlock(&t->vsync_thread);
	warp_read_knobs(D, now);
	if (D->ack_pending != 0 && (vsync > 0 || !on)) {
		direct_displayed(D, D->ack_pending);
		D->ack_pending = 0;
	}

	if (!on) {
		// panels off: nothing is displayed, nothing to wait for
		direct_flush_commits(D);
		if (D->fresh) {
			direct_displayed(D, D->latest_frame.frame);
			D->fresh = false;
		}
		D->need_refresh = true;
	} else if (vsync > 0 && D->latest >= 0) {
		warp_lens_setup(t);
		bool commit_raw = D->fresh || D->need_refresh;
		bool warped = false;
		bool want_warp = (D->lens || !D->knobs.off) && D->buf[D->latest][1].ahb != NULL;
		if (want_warp && D->lens && (D->knobs.off || !D->latest_frame.pose_valid)) {
			// undistorted images are never shown as they are: warp by the identity
			static const float identity[4] = {0, 0, 0, 1};
			commit_raw = false;
			if ((D->fresh || D->need_refresh) && warp_init(t)) {
				int o = warp_free_set(D);
				double gpu_ms = 0;
				if (o >= 0 && warp_render(t, o, identity, &gpu_ms) &&
				    direct_commit(t, D->out[o], -1, o, direct_ack_frame(D))) {
					warped = true;
					D->held_frame = D->latest_frame.frame;
					memcpy(D->held_delta, identity, sizeof(identity));
					D->stats.warp_commits++;
				}
			}
		} else if (want_warp) {
			// the pose the frame is shown for: the commit is latched at the next vsync, scanned out from then on
			int64_t display_ns = vsync + (int64_t)((D->knobs.delay_ms > 0 ? D->knobs.delay_ms * 1e6 : 1.5 * MDP_PERIOD_NS));
			float qd[4], qr[4];
			memcpy(qr, D->latest_frame.q, sizeof(qr));
			float qt[4];
			if (head_orientation(t, display_ns, qd) && head_orientation(t, D->latest_frame.time_ns, qt)) {
				if (D->latest_frame.frame != D->calibrated_frame) {
					// o: Monado's device space = o * LOCAL (a constant), averaged over the frames since the
					// prediction error of each one is random
					float cr[4], oe[4];
					quat_conj(qr, cr);
					quat_mul(qt, cr, oe);
					if (!D->o_valid) {
						memcpy(D->o, oe, sizeof(oe));
						D->o_valid = true;
					} else {
						float dot = D->o[0] * oe[0] + D->o[1] * oe[1] + D->o[2] * oe[2] + D->o[3] * oe[3];
						{
							float co2[4], rl[4];
							quat_conj(D->o, co2);
							quat_mul(co2, oe, rl);
							float a = quat_angle_deg(rl);
							a = a > 180.0f ? 360.0f - a : a;
							D->stats.oe_sum += a;
							D->stats.oe_max = a > D->stats.oe_max ? a : D->stats.oe_max;
							D->stats.oe_n++;
						}
						for (int i = 0; i < 4; i++) {
							D->o[i] += 0.05f * ((dot < 0 ? -oe[i] : oe[i]) - D->o[i]);
						}
						quat_normalize(D->o);
					}
					D->calibrated_frame = D->latest_frame.frame;
				}
				// delta = conj(q_render) * conj(o) * q_dev(display time) = conj(q_render) * q_local(display time)
				float co[4], cr[4], a[4], delta[4];
				quat_conj(D->o, co);
				quat_conj(qr, cr);
				quat_mul(co, qd, a);
				if (D->knobs.yaw_deg != 0) {
					float hy = D->knobs.yaw_deg * (float)M_PI / 360.0f;
					float qy[4] = {0, sinf(hy), 0, cosf(hy)}, b[4];
					quat_mul(a, qy, b);
					memcpy(a, b, sizeof(a));
				}
				quat_mul(cr, a, delta);
				if (D->knobs.sign & 1) delta[0] = -delta[0];
				if (D->knobs.sign & 2) delta[1] = -delta[1];
				if (D->knobs.sign & 4) delta[2] = -delta[2];
				quat_normalize(delta);
				if (D->knobs.rx_deg != 0 || D->knobs.ry_deg != 0 || D->knobs.rz_deg != 0) {
					float hx = D->knobs.rx_deg * (float)M_PI / 360.0f, hy = D->knobs.ry_deg * (float)M_PI / 360.0f,
					      hz = D->knobs.rz_deg * (float)M_PI / 360.0f;
					float qx[4] = {sinf(hx), 0, 0, cosf(hx)}, qy[4] = {0, sinf(hy), 0, cosf(hy)},
					      qz[4] = {0, 0, sinf(hz), cosf(hz)}, rq[4], rt[4], rc[4], t1[4], t2[4];
					quat_mul(qx, qy, rt);
					quat_mul(rt, qz, rq);
					quat_conj(rq, rc);
					quat_mul(rc, delta, t1);
					quat_mul(t1, rq, t2);
					memcpy(delta, t2, sizeof(t2));
					quat_normalize(delta);
				}
				D->stats.ax_sum += fabsf(delta[0]) * 2 * (180.0f / (float)M_PI);
				D->stats.ay_sum += fabsf(delta[1]) * 2 * (180.0f / (float)M_PI);
				D->stats.az_sum += fabsf(delta[2]) * 2 * (180.0f / (float)M_PI);
				float angle = quat_angle_deg(delta);
				D->stats.angle_sum += angle;
				D->stats.angle_max = angle > D->stats.angle_max ? angle : D->stats.angle_max;
				D->stats.angle_n++;
				float rel[4], hc[4];
				quat_conj(D->held_delta, hc);
				quat_mul(hc, delta, rel);
				if (!D->fresh && !D->need_refresh && D->held_frame == D->latest_frame.frame &&
				    quat_angle_deg(rel) < WARP_SKIP_DEG) {
					// the panels show this frame warped to a pose within a fraction of a pixel of this one
					D->stats.skipped++;
					commit_raw = false;
				} else if (!D->lens && D->fresh && angle < D->knobs.thr_deg && D->knobs.yaw_deg == 0) {
					commit_raw = true; // close enough: no GPU work
				} else if (warp_init(t)) {
					int o = warp_free_set(D);
					double gpu_ms = 0;
					int64_t t0 = os_monotonic_get_ns();
					if (o >= 0 && warp_render(t, o, delta, &gpu_ms)) {
						warp_snapshot(D, o);
						int64_t tc0 = os_monotonic_get_ns();
						bool committed = direct_commit(t, D->out[o], -1, o, direct_ack_frame(D));
						D->stats.commit_ms_sum += (double)(os_monotonic_get_ns() - tc0) / 1e6;
						if (committed) {
							warped = true;
							D->held_frame = D->latest_frame.frame;
							memcpy(D->held_delta, delta, sizeof(delta[0]) * 4);
							D->stats.warp_commits++;
							D->stats.gpu_ms_sum += gpu_ms;
							D->stats.gpu_ms_max = gpu_ms > D->stats.gpu_ms_max ? gpu_ms : D->stats.gpu_ms_max;
							D->stats.cpu_ms_sum += (double)(os_monotonic_get_ns() - t0) / 1e6;
							if (D->fresh) {
								D->stats.fresh_warped++;
							}
						}
					} else if (o < 0) {
						D->stats.no_set++;
					}
				}
			}
		}
		if (warped) {
			direct_ack_early(D);
			D->fresh = false;
			D->need_refresh = false;
		} else if (commit_raw && direct_commit(t, D->buf[D->latest], D->latest, -1, direct_ack_frame(D))) {
			D->stats.raw_commits++;
			// the panels show this frame as rendered, i.e. warped by the identity: a head that stays still
			// (delta < WARP_SKIP_DEG) needs no warp pass for it (it used to re-warp every raw frame once)
			D->held_frame = D->latest_frame.frame;
			D->held_delta[0] = D->held_delta[1] = D->held_delta[2] = 0;
			D->held_delta[3] = 1;
			direct_ack_early(D);
			D->fresh = false;
			D->need_refresh = false;
		}
	}
	direct_release_unused(D);
	if (D->stats.since_ns == 0) {
		D->stats.since_ns = now;
	} else if (now - D->stats.since_ns > 2 * U_TIME_1S_IN_NS) {
		double el = (double)(now - D->stats.since_ns) / 1e9;
		struct mdp_direct_stats *s = &D->stats;
		U_LOG_W("mdp: direct: %.1f/s warped (%.1f/s of them fresh), %.1f/s raw, %.1f/s kept; warp gpu %.2f ms avg (max %.2f), "
		        "cpu %.2f ms (mesh %.2f, gpu wait %.2f, commit %.2f); delta %.2f deg avg (max %.2f), "
		        "per axis x %.2f y %.2f z %.2f; o jitter %.2f deg avg (max %.2f); no free set %u",
		        s->warp_commits / el, s->fresh_warped / el, s->raw_commits / el, s->skipped / el,
		        s->warp_commits ? s->gpu_ms_sum / s->warp_commits : 0.0, s->gpu_ms_max,
		        s->warp_commits ? s->cpu_ms_sum / s->warp_commits : 0.0,
		        s->warp_commits ? s->mesh_ms_sum / s->warp_commits : 0.0,
		        s->warp_commits ? s->wait_ms_sum / s->warp_commits : 0.0,
		        s->warp_commits ? s->commit_ms_sum / s->warp_commits : 0.0, s->angle_n ? s->angle_sum / s->angle_n : 0.0,
		        s->angle_max,
		        s->angle_n ? s->ax_sum / s->angle_n : 0.0, s->angle_n ? s->ay_sum / s->angle_n : 0.0,
		        s->angle_n ? s->az_sum / s->angle_n : 0.0, s->oe_n ? s->oe_sum / s->oe_n : 0.0, s->oe_max,
		        s->no_set);
		*s = (struct mdp_direct_stats){.since_ns = now};
	}
	pthread_mutex_unlock(&D->lock);
}

static void *
vsync_thread_func(void *ptr)
{
	struct mdp_target *t = ptr;
	os_thread_helper_name(&t->vsync_thread, "Quest1 vsync");

	char buf[64];
	os_thread_helper_lock(&t->vsync_thread);
	while (os_thread_helper_is_running_locked(&t->vsync_thread)) {
		os_thread_helper_unlock(&t->vsync_thread);

		struct pollfd p[2 + 2 * DIRECT_COMMITS] = {{t->vsync_fd, POLLPRI | POLLERR, 0}, {t->prox_fd, POLLIN, 0}};
		int nfences = direct_collect_fences(t, p + 2, 2 * DIRECT_COMMITS);
		int64_t ts = 0;
		if (poll(p, 2 + nfences, 100) > 0 && p[0].revents != 0) {
			lseek(t->vsync_fd, 0, SEEK_SET);
			ssize_t n = read(t->vsync_fd, buf, sizeof(buf) - 1);
			buf[n > 0 ? n : 0] = '\0';
			long long v = 0;
			if (sscanf(buf, "VSYNC=%lld", &v) == 1) {
				ts = v; // CLOCK_MONOTONIC ns
			}
		}

		direct_handle_fences(t, p + 2, nfences);
		os_thread_helper_lock(&t->vsync_thread);
		if (ts > 0) {
			t->last_vsync_ns = ts;
		}
		if (t->prox_fd >= 0 && (p[1].revents & POLLIN)) {
			read_prox_locked(t);
		} else {
			update_worn_locked(t);
		}
		if (t->unblanked && os_monotonic_get_ns() - t->last_present_ns > U_TIME_1S_IN_NS) {
			set_panels_locked(t, false);
		}
		os_thread_helper_unlock(&t->vsync_thread);
		direct_tick(t, ts);
		os_thread_helper_lock(&t->vsync_thread);
	}
	os_thread_helper_unlock(&t->vsync_thread);
	return NULL;
}

static int64_t
get_last_vsync(struct mdp_target *t, bool *out_worn)
{
	os_thread_helper_lock(&t->vsync_thread);
	int64_t v = t->last_vsync_ns;
	*out_worn = t->worn;
	os_thread_helper_unlock(&t->vsync_thread);
	return v;
}


/*
 *
 * Images.
 *
 */

static void
free_images(struct mdp_target *t)
{
	struct vk_bundle *vk = &t->base.c->base.vk;
	for (uint32_t i = 0; i < MDP_IMAGES; i++) {
		if (t->images[i].view != VK_NULL_HANDLE) {
			vk->vkDestroyImageView(vk->device, t->images[i].view, NULL);
		}
		if (t->images[i].handle != VK_NULL_HANDLE) {
			vk->vkDestroyImage(vk->device, t->images[i].handle, NULL);
		}
		if (t->memory[i] != VK_NULL_HANDLE) {
			vk->vkFreeMemory(vk->device, t->memory[i], NULL);
		}
		if (t->ahb[i] != NULL) {
			AHardwareBuffer_release(t->ahb[i]);
		}
		close_fd(&t->release_fence[i]);
		t->images[i] = (struct comp_target_image){0};
		t->memory[i] = VK_NULL_HANDLE;
		t->ahb[i] = NULL;
		t->buffer_fd[i] = -1;
	}
	t->base.images = NULL;
	t->base.image_count = 0;
}

static bool
create_image(struct mdp_target *t, uint32_t i, VkImageUsageFlags usage, VkFormat view_format)
{
	struct comp_target *ct = &t->base;
	struct vk_bundle *vk = &ct->c->base.vk;
	PFN_vkGetAndroidHardwareBufferPropertiesANDROID get_props =
	    (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)vk->vkGetDeviceProcAddr(
	        vk->device, "vkGetAndroidHardwareBufferPropertiesANDROID");
	if (get_props == NULL) {
		COMP_ERROR(ct->c, "mdp: VK_ANDROID_external_memory_android_hardware_buffer not enabled");
		return false;
	}

	AHardwareBuffer_Desc desc = {
	    .width = MDP_W,
	    .height = MDP_H,
	    .layers = 1,
	    .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
	    .usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
	             AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY,
	};
	int r = AHardwareBuffer_allocate(&desc, &t->ahb[i]);
	if (r != 0) {
		COMP_ERROR(ct->c, "mdp: AHardwareBuffer_allocate: %d (is the graphics allocator HAL running?)", r);
		return false;
	}
	AHardwareBuffer_describe(t->ahb[i], &desc);
	const mdp_native_handle *nh = t->get_native_handle(t->ahb[i]);
	// gralloc private_handle_t: fd, fd_metadata, magic, flags, ...
	t->buffer_fd[i] = nh->data[0];
	t->ubwc[i] = ((uint32_t)nh->data[3] & PRIV_FLAGS_UBWC_ALIGNED) != 0;
	t->stride_px[i] = desc.stride;

	VkAndroidHardwareBufferFormatPropertiesANDROID fmt = {
	    .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
	VkAndroidHardwareBufferPropertiesANDROID props = {
	    .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID, .pNext = &fmt};
	VkResult ret = get_props(vk->device, t->ahb[i], &props);
	if (ret != VK_SUCCESS) {
		COMP_ERROR(ct->c, "mdp: vkGetAndroidHardwareBufferPropertiesANDROID: %d", ret);
		return false;
	}

	VkFormat formats[2] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB};
	VkImageFormatListCreateInfo list = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, .viewFormatCount = 2, .pViewFormats = formats};
	VkExternalMemoryImageCreateInfo ext = {
	    .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
	    .pNext = &list,
	    .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
	};
	VkImageCreateInfo ici = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .pNext = &ext,
	    .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = VK_FORMAT_R8G8B8A8_UNORM,
	    .extent = {MDP_W, MDP_H, 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = usage,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	ret = vk->vkCreateImage(vk->device, &ici, NULL, &t->images[i].handle);
	if (ret != VK_SUCCESS) {
		COMP_ERROR(ct->c, "mdp: vkCreateImage (usage %#x): %d", usage, ret);
		return false;
	}

	VkImportAndroidHardwareBufferInfoANDROID imp = {
	    .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, .buffer = t->ahb[i]};
	VkMemoryDedicatedAllocateInfo ded = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .pNext = &imp, .image = t->images[i].handle};
	VkMemoryAllocateInfo mai = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .pNext = &ded,
	    .allocationSize = props.allocationSize,
	    .memoryTypeIndex = (uint32_t)__builtin_ctz(props.memoryTypeBits),
	};
	ret = vk->vkAllocateMemory(vk->device, &mai, NULL, &t->memory[i]);
	if (ret == VK_SUCCESS) {
		ret = vk->vkBindImageMemory(vk->device, t->images[i].handle, t->memory[i], 0);
	}
	if (ret != VK_SUCCESS) {
		COMP_ERROR(ct->c, "mdp: AHB import: %d", ret);
		return false;
	}

	VkImageViewCreateInfo vci = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image = t->images[i].handle,
	    .viewType = VK_IMAGE_VIEW_TYPE_2D,
	    .format = view_format,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	ret = vk->vkCreateImageView(vk->device, &vci, NULL, &t->images[i].view);
	return ret == VK_SUCCESS;
}

static void
target_create_images(struct comp_target *ct,
                     const struct comp_target_create_images_info *create_info,
                     struct vk_bundle_queue *present_queue)
{
	struct mdp_target *t = mdpt(ct);

	// The buffers are R8G8B8A8_UNORM; render through an sRGB view if asked.
	bool srgb = false;
	for (uint32_t i = 0; i < create_info->format_count; i++) {
		VkFormat f = create_info->formats[i];
		if (f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB) {
			srgb = true;
			break;
		}
		if (f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_B8G8R8A8_UNORM) {
			break;
		}
	}
	VkFormat view_format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;

	free_images(t);
	for (uint32_t i = 0; i < MDP_IMAGES; i++) {
		if (!create_image(t, i, create_info->image_usage, view_format)) {
			free_images(t);
			return;
		}
	}
	t->next_image = 0;
	t->base.image_count = MDP_IMAGES;
	t->base.images = t->images;
	t->base.width = MDP_W;
	t->base.height = MDP_H;
	t->base.format = view_format;
	t->base.final_layout = VK_IMAGE_LAYOUT_GENERAL;
	COMP_INFO(ct->c, "mdp: %d gralloc buffers %ux%u (stride %u px, %s, %s view)", MDP_IMAGES, MDP_W, MDP_H,
	          t->stride_px[0], t->ubwc[0] ? "UBWC" : "linear", srgb ? "sRGB" : "UNORM");
}


/*
 *
 * Target members.
 *
 */

static bool
target_init_pre_vulkan(struct comp_target *ct)
{
	return true;
}

static bool
target_init_post_vulkan(struct comp_target *ct, uint32_t preferred_width, uint32_t preferred_height)
{
	mdpt(ct)->has_init_vulkan = true;
	return true;
}

/*!
 * SteamVR's vrcompositor scans its own, already distorted frames out on the panels (the Quest 1
 * Vulkan layer, QUEST1_DIRECT_SCANOUT) while this file names a live process. The compositor then
 * renders and commits nothing; it only keeps the panels on while the headset is worn.
 */
#define MDP_EXTERNAL_SCANOUT "/tmp/quest1-scanout"

static bool
external_scanout_active(struct mdp_target *t)
{
	int64_t now = os_monotonic_get_ns();
	if (now - t->external_checked_ns < 100 * U_TIME_1MS_IN_NS) {
		return t->external;
	}
	t->external_checked_ns = now;
	bool active = false;
	FILE *f = fopen(MDP_EXTERNAL_SCANOUT, "r");
	if (f != NULL) {
		int pid = 0;
		active = fscanf(f, "%d", &pid) == 1 && pid > 0 && (kill(pid, 0) == 0 || errno == EPERM);
		fclose(f);
	}
	// the layer's last commit must be on the panels before ours: a short grace after it went away
	active = active || t->direct.client_fd >= 0 || now - t->direct.left_ns < 300 * U_TIME_1MS_IN_NS;
	if (active != t->external) {
		U_LOG_W("mdp: external scanout %s", active ? "started: compositing paused" : "stopped");
	}
	t->external = active;
	return active;
}

static bool
target_check_ready(struct comp_target *ct)
{
	struct mdp_target *t = mdpt(ct);
	if (!external_scanout_active(t)) {
		return true;
	}
	// No frame of ours: keep the panels and the GPU clock as if we were presenting.
	os_thread_helper_lock(&t->vsync_thread);
	if (t->worn) {
		t->last_present_ns = os_monotonic_get_ns();
		set_panels_locked(t, true);
	}
	os_thread_helper_unlock(&t->vsync_thread);
	return false;
}

static bool
target_has_images(struct comp_target *ct)
{
	return mdpt(ct)->base.images != NULL;
}

static VkResult
target_acquire(struct comp_target *ct, uint32_t *out_index)
{
	struct mdp_target *t = mdpt(ct);
	uint32_t i = t->next_image;
	t->next_image = (i + 1) % MDP_IMAGES;

	// Wait until the display engine no longer reads this buffer.
	int64_t t0 = os_monotonic_get_ns();
	if (t->release_fence[i] >= 0) {
		struct pollfd p = {t->release_fence[i], POLLIN, 0};
		if (poll(&p, 1, 100) == 0) {
			COMP_WARN(ct->c, "mdp: release fence of image %u timed out", i);
		}
		close_fd(&t->release_fence[i]);
	}
	t->stats.fence_wait_ns += os_monotonic_get_ns() - t0;
	*out_index = i;
	return VK_SUCCESS;
}

static VkResult
target_present(struct comp_target *ct,
               struct vk_bundle_queue *present_queue,
               uint32_t index,
               uint64_t timeline_semaphore_value,
               int64_t desired_present_time_ns,
               int64_t present_slop_ns)
{
	struct mdp_target *t = mdpt(ct);
	struct vk_bundle *vk = &t->base.c->base.vk;

	//! @todo Pass an acquire fence (sync fd) instead of waiting for the GPU here.
	int64_t t0 = os_monotonic_get_ns();
	vk_queue_lock(present_queue);
	vk->vkQueueWaitIdle(present_queue->queue);
	vk_queue_unlock(present_queue);
	int64_t t1 = os_monotonic_get_ns();
	int64_t gpu_wait = t1 - t0;
	t->stats.gpu_wait_ns += gpu_wait;
	if (gpu_wait > t->stats.max_gpu_wait_ns) {
		t->stats.max_gpu_wait_ns = gpu_wait;
	}
	if (t1 > desired_present_time_ns) {
		t->stats.late++;
		t->stats.late_ns += t1 - desired_present_time_ns;
	}
	t->stats.presents++;
	if (t->stats.since_ns == 0) {
		t->stats.since_ns = t1;
	} else if (t1 - t->stats.since_ns > 2 * U_TIME_1S_IN_NS) {
		double n = t->stats.presents, el = (double)(t1 - t->stats.since_ns) / 1e9;
		COMP_INFO(ct->c, "mdp: %.1f fps, fence wait %.2f ms, gpu wait %.2f ms (max %.2f), late %u (%.2f ms avg)",
		          n / el, t->stats.fence_wait_ns / n / 1e6,
		          t->stats.gpu_wait_ns / n / 1e6, t->stats.max_gpu_wait_ns / 1e6, t->stats.late,
		          t->stats.late ? t->stats.late_ns / (double)t->stats.late / 1e6 : 0.0);
		t->stats = (typeof(t->stats)){.since_ns = t1};
	}

	os_thread_helper_lock(&t->vsync_thread);
	bool worn = t->worn;
	if (worn) {
		t->last_present_ns = t1;
		set_panels_locked(t, true);
	}
	os_thread_helper_unlock(&t->vsync_thread);
	if (!worn) {
		return VK_SUCCESS; // panels off: nothing to scan out
	}

	// One DMA pipe per panel; each pipe fetches its half of the buffer.
	struct mdp_input_layer layers[2];
	memset(layers, 0, sizeof(layers));
	const uint32_t pipes[2] = {64, 128}; // DMA0, DMA1
	for (int h = 0; h < 2; h++) {
		struct mdp_input_layer *l = &layers[h];
		l->pipe_ndx = pipes[h];
		l->alpha = 0xff;
		l->blend_op = BLEND_OP_OPAQUE;
		l->color_space = MDP_CSC_ITU_R_709;
		l->src_rect = (struct mdp_rect){h * MDP_HALF, 0, MDP_HALF, MDP_H};
		l->dst_rect = (struct mdp_rect){h * MDP_HALF, 0, MDP_HALF, MDP_H};
		l->buffer.width = t->stride_px[index];
		l->buffer.height = MDP_H;
		l->buffer.format = t->ubwc[index] ? MDP_RGBA_8888_UBWC : MDP_RGBA_8888;
		l->buffer.planes[0].fd = t->buffer_fd[index];
		l->buffer.planes[0].stride = t->stride_px[index] * 4;
		l->buffer.plane_count = 1;
		l->buffer.fence = -1;
	}
	struct mdp_layer_commit c;
	memset(&c, 0, sizeof(c));
	c.version = MDP_COMMIT_VERSION_1_0;
	c.commit_v1.input_layers = layers;
	c.commit_v1.input_layer_cnt = 2;
	c.commit_v1.release_fence = -1;
	c.commit_v1.retire_fence = -1;
	// Only one committer at a time, and never ours while the layer's frames are on the panels (the pipes'
	// geometry changes between the two, mdss refuses a commit that races with the other's)
	os_thread_helper_lock(&t->vsync_thread);
	bool layer_owns = t->direct.client_fd >= 0 || os_monotonic_get_ns() - t->direct.left_ns < 300 * U_TIME_1MS_IN_NS;
	int ret = layer_owns ? 0 : ioctl(t->fb_fd, MSMFB_ATOMIC_COMMIT, &c);
	int err = errno;
	os_thread_helper_unlock(&t->vsync_thread);
	if (layer_owns) {
		return VK_SUCCESS;
	}
	if (ret < 0) {
		COMP_ERROR(ct->c, "mdp: MSMFB_ATOMIC_COMMIT: %s (layer errors %d %d)", strerror(err), layers[0].error_code,
		           layers[1].error_code);
		return VK_ERROR_SURFACE_LOST_KHR;
	}
	close_fd(&c.commit_v1.retire_fence);
	close_fd(&t->release_fence[index]);
	t->release_fence[index] = c.commit_v1.release_fence;
	return VK_SUCCESS;
}

static VkResult
target_wait_for_present(struct comp_target *ct, time_duration_ns timeout_ns)
{
	return VK_ERROR_EXTENSION_NOT_PRESENT;
}

static void
target_flush(struct comp_target *ct)
{}

static void
target_calc_frame_pacing(struct comp_target *ct,
                         int64_t *out_frame_id,
                         int64_t *out_wake_up_time_ns,
                         int64_t *out_desired_present_time_ns,
                         int64_t *out_present_slop_ns,
                         int64_t *out_predicted_display_time_ns)
{
	struct mdp_target *t = mdpt(ct);
	int64_t now = os_monotonic_get_ns();
	bool worn;
	int64_t vsync = get_last_vsync(t, &worn);
	if (vsync <= 0) {
		vsync = now;
	}
	// The panels stop sending vsyncs once off: the last one is extrapolated.
	int64_t period = worn ? MDP_PERIOD_NS : MDP_IDLE_PERIOD_NS;

	// First vsync that leaves the compositor its render budget, and never the
	// same one twice.
	int64_t target = vsync + MDP_PERIOD_NS;
	while (target < now + MDP_RENDER_BUDGET_NS || target <= t->last_display_ns) {
		target += period;
	}
	t->last_display_ns = target;

	*out_frame_id = ++t->frame_id;
	*out_wake_up_time_ns = target - MDP_RENDER_BUDGET_NS;
	*out_desired_present_time_ns = target;
	*out_present_slop_ns = U_TIME_HALF_MS_IN_NS;
	// The commit is latched at that vsync and scanned out during the next period.
	*out_predicted_display_time_ns = target + MDP_PERIOD_NS / 2;
}

static void
target_mark_timing_point(struct comp_target *ct, enum comp_target_timing_point point, int64_t frame_id, int64_t when_ns)
{}

static VkResult
target_update_timings(struct comp_target *ct)
{
	return VK_SUCCESS;
}

static void
target_info_gpu(struct comp_target *ct, int64_t frame_id, int64_t gpu_start_ns, int64_t gpu_end_ns, int64_t when_ns)
{}

static VkResult
target_queue_supports_present(struct comp_target *ct, struct vk_bundle_queue *queue, VkBool32 *out_supported)
{
	*out_supported = VK_TRUE;
	return VK_SUCCESS;
}

static void
target_set_title(struct comp_target *ct, const char *title)
{}

static bool
target_is_shared_presentable_image(struct comp_target *ct)
{
	return false;
}

static void
target_destroy(struct comp_target *ct)
{
	struct mdp_target *t = mdpt(ct);
	direct_destroy(t);
	os_thread_helper_destroy(&t->vsync_thread);
	if (t->has_init_vulkan) {
		free_images(t);
	}
	if (t->fb_fd >= 0) {
		unsigned int off = 0;
		ioctl(t->fb_fd, MSMFB_OVERLAY_VSYNC_CTRL, &off);
		t->unblanked = true; // force: also blank what an earlier process left on
		set_panels_locked(t, false);
	}
	close_fd(&t->prox_fd);
	close_fd(&t->vsync_fd);
	close_fd(&t->fb_fd);
	free(t);
}


/*
 *
 * Factory.
 *
 */

static bool
factory_detect(const struct comp_target_factory *ctf, struct comp_compositor *c)
{
	return access("/dev/syncboss0", F_OK) == 0 && access("/sys/class/graphics/fb0/vsync_event", R_OK) == 0;
}

static bool
factory_create_target(const struct comp_target_factory *ctf, struct comp_compositor *c, struct comp_target **out_ct)
{
	void *nw = android_dlopen("libnativewindow.so", RTLD_NOW);
	void *get_handle = nw != NULL ? android_dlsym(nw, "AHardwareBuffer_getNativeHandle") : NULL;
	if (get_handle == NULL) {
		COMP_ERROR(c, "mdp: no AHardwareBuffer_getNativeHandle in libnativewindow.so");
		return false;
	}

	struct mdp_target *t = U_TYPED_CALLOC(struct mdp_target);
	t->fb_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	t->vsync_fd = open("/sys/class/graphics/fb0/vsync_event", O_RDONLY | O_CLOEXEC);
	for (uint32_t i = 0; i < MDP_IMAGES; i++) {
		t->buffer_fd[i] = -1;
		t->release_fence[i] = -1;
	}
	if (t->fb_fd < 0 || t->vsync_fd < 0) {
		COMP_ERROR(c, "mdp: cannot open /dev/fb0 or its vsync_event: %s", strerror(errno));
		close_fd(&t->fb_fd);
		close_fd(&t->vsync_fd);
		free(t);
		return false;
	}
	*(void **)&t->get_native_handle = get_handle;
	direct_init(t, nw);

	// Shown until the sensor says otherwise: its first state comes right after opening.
	t->worn = true;
	t->prox_worn = true;
	t->prox_fd = -1;
	const char *ignore = getenv("QUEST1_IGNORE_PROX");
	if (ignore == NULL || strcmp(ignore, "1") != 0) {
		t->prox_fd = open("/dev/syncboss_powerstate0", O_RDONLY | O_CLOEXEC | O_NONBLOCK);
		if (t->prox_fd < 0) {
			COMP_WARN(c, "mdp: no proximity sensor (%s), panels always on", strerror(errno));
		}
	}

	unsigned int on = 1;
	if (ioctl(t->fb_fd, MSMFB_OVERLAY_VSYNC_CTRL, &on) < 0) {
		COMP_WARN(c, "mdp: MSMFB_OVERLAY_VSYNC_CTRL: %s", strerror(errno));
	}
	os_thread_helper_init(&t->vsync_thread);
	os_thread_helper_start(&t->vsync_thread, vsync_thread_func, t);

	t->base.name = "mdp";
	t->base.c = c;
	t->base.init_pre_vulkan = target_init_pre_vulkan;
	t->base.init_post_vulkan = target_init_post_vulkan;
	t->base.check_ready = target_check_ready;
	t->base.create_images = target_create_images;
	t->base.has_images = target_has_images;
	t->base.acquire = target_acquire;
	t->base.present = target_present;
	t->base.wait_for_present = target_wait_for_present;
	t->base.flush = target_flush;
	t->base.calc_frame_pacing = target_calc_frame_pacing;
	t->base.mark_timing_point = target_mark_timing_point;
	t->base.update_timings = target_update_timings;
	t->base.info_gpu = target_info_gpu;
	t->base.set_title = target_set_title;
	t->base.queue_supports_present = target_queue_supports_present;
	t->base.is_shared_presentable_image = target_is_shared_presentable_image;
	t->base.destroy = target_destroy;
	t->base.wait_for_present_supported = false;
	*out_ct = &t->base;
	COMP_INFO(c, "mdp: compositor target created");
	return true;
}

static const char *mdp_device_extensions[] = {
    VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
    VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
};

const struct comp_target_factory comp_target_factory_mdp = {
    .name = "Quest 1 panels (MSM MDSS atomic commit)",
    .identifier = "mdp",
    .requires_vulkan_for_create = false,
    .is_deferred = false,
    .required_instance_version = 0,
    .required_instance_extensions = NULL,
    .required_instance_extension_count = 0,
    .optional_device_extensions = mdp_device_extensions,
    .optional_device_extension_count = ARRAY_SIZE(mdp_device_extensions),
    .detect = factory_detect,
    .create_target = factory_create_target,
};
