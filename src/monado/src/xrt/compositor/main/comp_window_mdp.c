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

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
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

struct mdp_target
{
	struct comp_target base;

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
	bool worn;      //!< Proximity sensor covered. Protected by vsync_thread's mutex.
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
	// Off with HSYNC_SUSPEND: mdss powers the panels off as for POWERDOWN, but the fan driver
	// (drivers/platform/oculus/vs1-board.c) only stops the fan on POWERDOWN or VSYNC_SUSPEND,
	// so it keeps cooling the SoC, which may still be busy, while the headset is off the head.
	if (ioctl(t->fb_fd, FBIOBLANK, on ? FB_BLANK_UNBLANK : FB_BLANK_HSYNC_SUSPEND) < 0) {
		U_LOG_E("mdp: FBIOBLANK %s: %s", on ? "unblank" : "powerdown", strerror(errno));
	}
	if (on) {
		unsigned int vsync_on = 1;
		ioctl(t->fb_fd, MSMFB_OVERLAY_VSYNC_CTRL, &vsync_on);
		set_brightness();
	}
	t->unblanked = on;
	U_LOG_I("mdp: panels %s", on ? "on" : "off");
}

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
			if (worn != t->worn) {
				U_LOG_I("mdp: headset %s", worn ? "worn" : "removed");
			}
			t->worn = worn;
			if (!worn) {
				set_panels_locked(t, false);
			}
		}
	}
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

		struct pollfd p[2] = {{t->vsync_fd, POLLPRI | POLLERR, 0}, {t->prox_fd, POLLIN, 0}};
		int64_t ts = 0;
		if (poll(p, t->prox_fd >= 0 ? 2 : 1, 100) > 0 && p[0].revents != 0) {
			lseek(t->vsync_fd, 0, SEEK_SET);
			ssize_t n = read(t->vsync_fd, buf, sizeof(buf) - 1);
			buf[n > 0 ? n : 0] = '\0';
			long long v = 0;
			if (sscanf(buf, "VSYNC=%lld", &v) == 1) {
				ts = v; // CLOCK_MONOTONIC ns
			}
		}

		os_thread_helper_lock(&t->vsync_thread);
		if (ts > 0) {
			t->last_vsync_ns = ts;
		}
		if (t->prox_fd >= 0 && (p[1].revents & POLLIN)) {
			read_prox_locked(t);
		}
		if (t->unblanked && os_monotonic_get_ns() - t->last_present_ns > U_TIME_1S_IN_NS) {
			set_panels_locked(t, false);
		}
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

static bool
target_check_ready(struct comp_target *ct)
{
	return true;
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
	if (ioctl(t->fb_fd, MSMFB_ATOMIC_COMMIT, &c) < 0) {
		COMP_ERROR(ct->c, "mdp: MSMFB_ATOMIC_COMMIT: %s (layer errors %d %d)", strerror(errno),
		           layers[0].error_code, layers[1].error_code);
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

	// Shown until the sensor says otherwise: its first state comes right after opening.
	t->worn = true;
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
