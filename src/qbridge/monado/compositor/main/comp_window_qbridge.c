// Copyright 2026, qbridge contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Compositor target rendering into the qbridge app's AHardwareBuffers.
 *
 * The app (Meta OpenXR runtime) allocates the buffers and displays them as a
 * projection layer; this target imports them through
 * VK_ANDROID_external_memory_android_hardware_buffer (Quest Vulkan driver via
 * libhybris) and hands each finished image back with the pose it used.
 *
 * @ingroup comp_main
 */

#include "util/u_misc.h"
#include "util/u_pacing.h"
#include "os/os_time.h"

#include "main/comp_window.h"
#include "main/comp_compositor.h"

#include "qbridge/qbridge_interface.h"

#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan_android.h>


struct qbridge_target
{
	struct comp_target base;
	struct qb_sys *sys;
	struct u_pacing_compositor *upc;

	struct comp_target_image images[QB_MAX_SLOTS];
	VkDeviceMemory memory[QB_MAX_SLOTS];
	uint64_t generation;
	int64_t index;
	bool has_init_vulkan;
};

static inline struct qbridge_target *
qbt(struct comp_target *ct)
{
	return (struct qbridge_target *)ct;
}

static bool
target_init_pre_vulkan(struct comp_target *ct)
{
	return true;
}

static bool
target_init_post_vulkan(struct comp_target *ct, uint32_t preferred_width, uint32_t preferred_height)
{
	qbt(ct)->has_init_vulkan = true;
	return true;
}

static bool
target_check_ready(struct comp_target *ct)
{
	struct qbridge_target *t = qbt(ct);
	os_mutex_lock(&t->sys->mutex);
	bool ready = t->sys->link.fd >= 0;
	os_mutex_unlock(&t->sys->mutex);
	return ready;
}

static void
free_images(struct qbridge_target *t)
{
	struct vk_bundle *vk = &t->base.c->base.vk;
	for (uint32_t i = 0; i < QB_MAX_SLOTS; i++) {
		if (t->images[i].view != VK_NULL_HANDLE)
			vk->vkDestroyImageView(vk->device, t->images[i].view, NULL);
		if (t->images[i].handle != VK_NULL_HANDLE)
			vk->vkDestroyImage(vk->device, t->images[i].handle, NULL);
		if (t->memory[i] != VK_NULL_HANDLE)
			vk->vkFreeMemory(vk->device, t->memory[i], NULL);
		t->images[i] = (struct comp_target_image){0};
		t->memory[i] = VK_NULL_HANDLE;
	}
	t->base.images = NULL;
	t->base.image_count = 0;
}

static void
target_create_images(struct comp_target *ct,
                     const struct comp_target_create_images_info *create_info,
                     struct vk_bundle_queue *present_queue)
{
	struct qbridge_target *t = qbt(ct);
	struct vk_bundle *vk = &t->base.c->base.vk;
	PFN_vkGetAndroidHardwareBufferPropertiesANDROID get_props =
	    (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)vk->vkGetDeviceProcAddr(
	        vk->device, "vkGetAndroidHardwareBufferPropertiesANDROID");
	if (get_props == NULL) {
		COMP_ERROR(ct->c, "qbridge: VK_ANDROID_external_memory_android_hardware_buffer not enabled");
		return;
	}

	// The buffers are R8G8B8A8_UNORM; render through an sRGB view if asked.
	bool srgb = false;
	for (uint32_t i = 0; i < create_info->format_count; i++) {
		if (create_info->formats[i] == VK_FORMAT_R8G8B8A8_SRGB || create_info->formats[i] == VK_FORMAT_B8G8R8A8_SRGB) {
			srgb = true;
			break;
		}
		if (create_info->formats[i] == VK_FORMAT_R8G8B8A8_UNORM ||
		    create_info->formats[i] == VK_FORMAT_B8G8R8A8_UNORM) {
			break;
		}
	}
	VkFormat view_format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;

	free_images(t);
	os_mutex_lock(&t->sys->mutex);
	struct qb_link *l = &t->sys->link;
	uint32_t w = l->buffers.width, h = l->buffers.height;
	bool ok = l->fd >= 0;
	for (uint32_t i = 0; ok && i < QB_MAX_SLOTS; i++) {
		VkAndroidHardwareBufferFormatPropertiesANDROID fmt = {
		    .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
		VkAndroidHardwareBufferPropertiesANDROID props = {
		    .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID, .pNext = &fmt};
		VkResult r = get_props(vk->device, l->ahb[i], &props);
		if (r != VK_SUCCESS) {
			COMP_ERROR(ct->c, "qbridge: vkGetAndroidHardwareBufferPropertiesANDROID: %d", r);
			ok = false;
			break;
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
		    .extent = {w, h, 1},
		    .mipLevels = 1,
		    .arrayLayers = 1,
		    .samples = VK_SAMPLE_COUNT_1_BIT,
		    .tiling = VK_IMAGE_TILING_OPTIMAL,
		    .usage = create_info->image_usage,
		    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};
		r = vk->vkCreateImage(vk->device, &ici, NULL, &t->images[i].handle);
		if (r != VK_SUCCESS) {
			COMP_ERROR(ct->c, "qbridge: vkCreateImage (usage %#x): %d", create_info->image_usage, r);
			ok = false;
			break;
		}

		VkImportAndroidHardwareBufferInfoANDROID imp = {
		    .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, .buffer = l->ahb[i]};
		VkMemoryDedicatedAllocateInfo ded = {
		    .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .pNext = &imp, .image = t->images[i].handle};
		VkMemoryAllocateInfo mai = {
		    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		    .pNext = &ded,
		    .allocationSize = props.allocationSize,
		    .memoryTypeIndex = (uint32_t)__builtin_ctz(props.memoryTypeBits),
		};
		r = vk->vkAllocateMemory(vk->device, &mai, NULL, &t->memory[i]);
		if (r == VK_SUCCESS)
			r = vk->vkBindImageMemory(vk->device, t->images[i].handle, t->memory[i], 0);
		if (r != VK_SUCCESS) {
			COMP_ERROR(ct->c, "qbridge: AHB import: %d", r);
			ok = false;
			break;
		}

		VkImageViewCreateInfo vci = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image = t->images[i].handle,
		    .viewType = VK_IMAGE_VIEW_TYPE_2D,
		    .format = view_format,
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
		r = vk->vkCreateImageView(vk->device, &vci, NULL, &t->images[i].view);
		if (r != VK_SUCCESS) {
			ok = false;
			break;
		}
	}
	t->generation = l->generation;
	os_mutex_unlock(&t->sys->mutex);

	if (!ok) {
		free_images(t);
		return;
	}
	t->base.image_count = QB_MAX_SLOTS;
	t->base.images = t->images;
	t->base.width = w;
	t->base.height = h;
	t->base.format = view_format;
	t->base.final_layout = VK_IMAGE_LAYOUT_GENERAL;
	COMP_INFO(ct->c, "qbridge: imported %d AHardwareBuffers %ux%u (%s view)", QB_MAX_SLOTS, w, h,
	          srgb ? "sRGB" : "UNORM");
}

static bool
target_has_images(struct comp_target *ct)
{
	return qbt(ct)->base.images != NULL;
}

static VkResult
target_acquire(struct comp_target *ct, uint32_t *out_index)
{
	struct qbridge_target *t = qbt(ct);
	for (int tries = 0; tries < 200; tries++) {
		os_mutex_lock(&t->sys->mutex);
		bool stale = t->sys->link.fd < 0 || t->sys->link.generation != t->generation;
		int slot = stale ? -1 : qb_link_acquire(&t->sys->link);
		os_mutex_unlock(&t->sys->mutex);
		if (stale) {
			//! @todo Recreate images when the app reconnects.
			return VK_ERROR_SURFACE_LOST_KHR;
		}
		if (slot >= 0) {
			t->index = slot;
			*out_index = (uint32_t)slot;
			return VK_SUCCESS;
		}
		os_nanosleep(U_TIME_1MS_IN_NS / 2);
	}
	return VK_TIMEOUT;
}

static VkResult
target_present(struct comp_target *ct,
               struct vk_bundle_queue *present_queue,
               uint32_t index,
               uint64_t timeline_semaphore_value,
               int64_t desired_present_time_ns,
               int64_t present_slop_ns)
{
	struct qbridge_target *t = qbt(ct);
	struct vk_bundle *vk = &t->base.c->base.vk;

	// The app samples the image as soon as it gets FRAME: finish the GPU work
	// first (no shared sync object across the two processes yet).
	vk_queue_lock(present_queue);
	vk->vkQueueWaitIdle(present_queue->queue);
	vk_queue_unlock(present_queue);

	os_mutex_lock(&t->sys->mutex);
	qb_link_present(&t->sys->link, (int)index, t->sys->render_seq);
	os_mutex_unlock(&t->sys->mutex);
	t->index = -1;
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
	struct qbridge_target *t = qbt(ct);
	int64_t frame_id = -1, wake = 0, desired = 0, slop = 0, predicted = 0, period = 0, min_period = 0;
	u_pc_predict(t->upc, os_monotonic_get_ns(), &frame_id, &wake, &desired, &slop, &predicted, &period,
	             &min_period);
	*out_frame_id = frame_id;
	*out_wake_up_time_ns = wake;
	*out_desired_present_time_ns = desired;
	*out_present_slop_ns = slop;
	*out_predicted_display_time_ns = predicted;
}

static void
target_mark_timing_point(struct comp_target *ct, enum comp_target_timing_point point, int64_t frame_id, int64_t when_ns)
{
	struct qbridge_target *t = qbt(ct);
	switch (point) {
	case COMP_TARGET_TIMING_POINT_WAKE_UP: u_pc_mark_point(t->upc, U_TIMING_POINT_WAKE_UP, frame_id, when_ns); break;
	case COMP_TARGET_TIMING_POINT_BEGIN: u_pc_mark_point(t->upc, U_TIMING_POINT_BEGIN, frame_id, when_ns); break;
	case COMP_TARGET_TIMING_POINT_SUBMIT_BEGIN:
		u_pc_mark_point(t->upc, U_TIMING_POINT_SUBMIT_BEGIN, frame_id, when_ns);
		break;
	case COMP_TARGET_TIMING_POINT_SUBMIT_END:
		u_pc_mark_point(t->upc, U_TIMING_POINT_SUBMIT_END, frame_id, when_ns);
		break;
	default: break;
	}
}

static VkResult
target_update_timings(struct comp_target *ct)
{
	return VK_SUCCESS;
}

static void
target_info_gpu(struct comp_target *ct, int64_t frame_id, int64_t gpu_start_ns, int64_t gpu_end_ns, int64_t when_ns)
{
	u_pc_info_gpu(qbt(ct)->upc, frame_id, gpu_start_ns, gpu_end_ns, when_ns);
}

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
	struct qbridge_target *t = qbt(ct);
	if (t->has_init_vulkan) {
		free_images(t);
	}
	u_pc_destroy(&t->upc);
	free(t);
}

static bool
factory_detect(const struct comp_target_factory *ctf, struct comp_compositor *c)
{
	return getenv("QBRIDGE_ENABLE") != NULL;
}

static bool
factory_create_target(const struct comp_target_factory *ctf, struct comp_compositor *c, struct comp_target **out_ct)
{
	struct qb_sys *sys = qb_sys_get();
	if (sys == NULL) {
		return false;
	}
	struct qbridge_target *t = U_TYPED_CALLOC(struct qbridge_target);
	t->sys = sys;
	t->index = -1;
	t->base.name = "qbridge";
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
	u_pc_fake_create(c->settings.nominal_frame_interval_ns, os_monotonic_get_ns(), &t->upc);
	*out_ct = &t->base;
	COMP_INFO(c, "qbridge: compositor target created");
	return true;
}

static const char *qbridge_device_extensions[] = {
    VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
    VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
};

const struct comp_target_factory comp_target_factory_qbridge = {
    .name = "qbridge (Meta runtime on the same Quest)",
    .identifier = "qbridge",
    .requires_vulkan_for_create = false,
    .is_deferred = false,
    .required_instance_version = 0,
    .required_instance_extensions = NULL,
    .required_instance_extension_count = 0,
    .optional_device_extensions = qbridge_device_extensions,
    .optional_device_extension_count = ARRAY_SIZE(qbridge_device_extensions),
    .detect = factory_detect,
    .create_target = factory_create_target,
};
