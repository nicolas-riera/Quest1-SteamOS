// quest1_scanout_passes: vrcompositor's render passes on the display image: the merge (quest1_rpmerge.c), the scene's twins and copies.
// Part of quest1_scanout.c (included by it, in this order: one translation unit with quest1_compat.c).

#include "quest1_rpmerge.c"

// vrcompositor's distortion pass draws each eye in its own render pass whose render area is the
// eye's half of the display image. On the Adreno 540 blob, a render area at x = 1440 of a linear
// (gralloc) image comes out as copies of its first 320-px bin column. So those passes run over the
// whole image instead: a variant of the render pass that loads the attachments, and the clear the
// app asked for done with vkCmdClearAttachments on its own render area (its scissor already keeps
// the draws inside it).
struct qs_rp_variant
{
	VkRenderPass rp, load;
};
static struct qs_rp_variant qs_rp_variants[64];
static unsigned qs_rp_nvariants;
static pthread_mutex_t qs_rp_lock = PTHREAD_MUTEX_INITIALIZER;

static VKAPI_ATTR VkResult VKAPI_CALL qs_CreateRenderPass(VkDevice device, const VkRenderPassCreateInfo *ci,
                                                         const VkAllocationCallbacks *alloc, VkRenderPass *out)
{
	struct device *d = device_of(device);
	PFN_vkCreateRenderPass next = (PFN_vkCreateRenderPass)d->gdpa(device, "vkCreateRenderPass");
	VkResult r = next(device, ci, alloc, out);
	if (r != VK_SUCCESS || ci->attachmentCount == 0 || ci->attachmentCount > 4 || ci->subpassCount != 1)
		return r;
	VkAttachmentDescription att[4];
	bool clears = false;
	for (uint32_t i = 0; i < ci->attachmentCount; i++) {
		att[i] = ci->pAttachments[i];
		if (att[i].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR || att[i].loadOp == VK_ATTACHMENT_LOAD_OP_DONT_CARE) {
			clears |= att[i].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR;
			att[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		}
		if (att[i].stencilLoadOp == VK_ATTACHMENT_LOAD_OP_CLEAR)
			return r; // only colour clears are emulated
		// the image is in the final layout of the previous pass over it (the other eye's)
		if (att[i].initialLayout == VK_IMAGE_LAYOUT_UNDEFINED)
			att[i].initialLayout = att[i].finalLayout;
	}
	if (!clears)
		return r;
	VkRenderPassCreateInfo lci = *ci;
	lci.pAttachments = att;
	VkRenderPass load = VK_NULL_HANDLE;
	if (next(device, &lci, alloc, &load) != VK_SUCCESS)
		return r;
	pthread_mutex_lock(&qs_rp_lock);
	if (qs_rp_nvariants < sizeof(qs_rp_variants) / sizeof(qs_rp_variants[0]))
		qs_rp_variants[qs_rp_nvariants++] = (struct qs_rp_variant){*out, load};
	pthread_mutex_unlock(&qs_rp_lock);
	return r;
}

static VKAPI_ATTR void VKAPI_CALL qs_DestroyRenderPass(VkDevice device, VkRenderPass rp, const VkAllocationCallbacks *alloc)
{
	struct device *d = device_of(device);
	PFN_vkDestroyRenderPass next = (PFN_vkDestroyRenderPass)d->gdpa(device, "vkDestroyRenderPass");
	rm_forget_rp(d, rp);
	pthread_mutex_lock(&qs_rp_lock);
	for (unsigned i = 0; i < qs_rp_nvariants; i++)
		if (qs_rp_variants[i].rp == rp) {
			next(device, qs_rp_variants[i].load, alloc);
			qs_rp_variants[i] = qs_rp_variants[--qs_rp_nvariants];
			break;
		}
	pthread_mutex_unlock(&qs_rp_lock);
	next(device, rp, alloc);
}

static VkRenderPass qs_rp_load_variant(VkRenderPass rp)
{
	VkRenderPass load = VK_NULL_HANDLE;
	pthread_mutex_lock(&qs_rp_lock);
	for (unsigned i = 0; i < qs_rp_nvariants; i++)
		if (qs_rp_variants[i].rp == rp)
			load = qs_rp_variants[i].load;
	pthread_mutex_unlock(&qs_rp_lock);
	return load;
}

//! Scene mode: the index the next acquire of the direct swapchain returns, -1 if none is free now.
static int qs_reserve(struct qd_swapchain **out)
{
	pthread_mutex_lock(&qs_twin_lock);
	struct qd_swapchain *sc = qs_direct_scs[0];
	pthread_mutex_unlock(&qs_twin_lock);
	*out = sc;
	if (!sc)
		return -1;
	pthread_mutex_lock(&sc->mutex);
	int r = sc->reserved1 ? (int)sc->reserved1 - 1 : -1;
	for (uint32_t k = 0; k < sc->count && r < 0; k++) {
		uint32_t i = (sc->next + k) % sc->count;
		if (!sc->app_owned[i] && !sc->driver_owned[i])
			r = (int)i;
	}
	if (r >= 0 && !sc->reserved1)
		sc->hud_of[r] = 0; // a new frame in it (quest1_hud.c)
	if (r >= 0)
		sc->reserved1 = (uint32_t)r + 1;
	pthread_mutex_unlock(&sc->mutex);
	return r;
}

//! Scene mode: the twin of scene framebuffer `fb` whose scene attachment is gralloc buffer `half` (made at first use).
static VkFramebuffer qs_scene_twin(VkDevice device, VkFramebuffer fb, VkImage half)
{
	VkFramebuffer twin = VK_NULL_HANDLE;
	pthread_mutex_lock(&qs_scene_lock);
	for (unsigned i = 0; i < qs_nstwins && !twin; i++)
		if (qs_stwins[i].fb == (uint64_t)fb && qs_stwins[i].half == (uint64_t)half)
			twin = (VkFramebuffer)qs_stwins[i].twin;
	int s = -1;
	for (unsigned i = 0; i < qs_nsfbs && s < 0; i++)
		if (qs_scene_fbs[i].fb == (uint64_t)fb)
			s = (int)i;
	if (twin || s < 0 || qs_nstwins == 64) {
		pthread_mutex_unlock(&qs_scene_lock);
		return twin;
	}
	struct device *d = device_of(device);
	PFN_vkCreateImageView create_view = (PFN_vkCreateImageView)d->gdpa(device, "vkCreateImageView");
	PFN_vkCreateFramebuffer create_fb = (PFN_vkCreateFramebuffer)d->gdpa(device, "vkCreateFramebuffer");
	PFN_vkDestroyImageView destroy_view = (PFN_vkDestroyImageView)d->gdpa(device, "vkDestroyImageView");
	VkImageViewCreateInfo vci = qs_scene_fbs[s].vci;
	vci.pNext = NULL;
	vci.image = half;
	VkImageView view = VK_NULL_HANDLE;
	VkImageView att[QS_SFB_ATT];
	memcpy(att, qs_scene_fbs[s].att, sizeof(att));
	VkFramebufferCreateInfo fci = qs_scene_fbs[s].ci;
	fci.pAttachments = att;
	if (create_view(device, &vci, NULL, &view) == VK_SUCCESS) {
		att[qs_scene_fbs[s].color] = view;
		if (create_fb(device, &fci, NULL, &twin) == VK_SUCCESS)
			qs_stwins[qs_nstwins++] = (typeof(qs_stwins[0])){(uint64_t)fb, (uint64_t)half, (uint64_t)view, (uint64_t)twin};
		else {
			destroy_view(device, view, NULL);
			twin = VK_NULL_HANDLE;
		}
	}
	pthread_mutex_unlock(&qs_scene_lock);
	if (!twin)
		LOG("direct scanout: scene mode: no twin of framebuffer %p on %p\n", (void *)fb, (void *)half);
	return twin;
}

//! Scene mode, at the start of a distortion pass into `half` whose eye was rendered into vrcompositor's own image (no
//! swapchain index was free): copies the scene image it made ready for the pass into `half` and marks the pass's
//! commands to be dropped. False (the pass runs) when no scene image of the size is known.
static bool qs_scene_copy(VkCommandBuffer cb, VkImage half)
{
	pthread_mutex_lock(&qs_cbst_lock);
	typeof(qs_cbst[0]) *e = qs_cbst_get(cb, false);
	VkImage scene = e ? (VkImage)e->scene : VK_NULL_HANDLE;
	uint32_t sw = e ? e->w : 0, sh = e ? e->h : 0;
	if (e)
		e->scene = 0;
	pthread_mutex_unlock(&qs_cbst_lock);
	if (scene && (sw != qs_scene_w || sh != qs_scene_h)) {
		static uint32_t saved_w, saved_h;
		if (sw != saved_w || sh != saved_h) { // the size for the next start
			saved_w = sw;
			saved_h = sh;
			char path[256], tmp[272];
			qs_scene_size_path(path, sizeof(path));
			snprintf(tmp, sizeof(tmp), "%s.tmp", path);
			FILE *f = fopen(tmp, "w");
			if (f) {
				fprintf(f, "%u %u\n", sw, sh);
				fclose(f);
				rename(tmp, path);
			}
			LOG("direct scanout: scene mode: vrcompositor renders the eyes at %ux%u (buffers %ux%u): saved in %s, "
			    "used from its next start\n", sw, sh, qs_scene_w, qs_scene_h, path);
		}
		return false;
	}
	if (!scene) {
		static int logged;
		if (!logged++)
			LOG("direct scanout: scene mode: no %ux%u scene image before a distortion pass, it runs (logged once)\n",
			    qs_scene_w, qs_scene_h);
		return false;
	}
	struct device *d = device_of(cb);
	static PFN_vkCmdPipelineBarrier barrier;
	static PFN_vkCmdCopyImage copy;
	if (!barrier) {
		barrier = (PFN_vkCmdPipelineBarrier)d->gdpa(d->handle, "vkCmdPipelineBarrier");
		copy = (PFN_vkCmdCopyImage)d->gdpa(d->handle, "vkCmdCopyImage");
	}
	const VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	VkImageMemoryBarrier b[2] = {
	    {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
	     VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	     VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, scene, all},
	    {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
	     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, half, all},
	};
	barrier(cb, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, b);
	const VkImageSubresourceLayers l = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	VkImageCopy r = {l, {0, 0, 0}, l, {0, 0, 0}, {qs_scene_w, qs_scene_h, 1}};
	copy(cb, scene, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, half, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
	b[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	b[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	b[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	b[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	b[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	b[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
	b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	b[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 2, b);
	pthread_mutex_lock(&qs_cbst_lock);
	qs_cbst_get(cb, true)->skipping = true;
	pthread_mutex_unlock(&qs_cbst_lock);
	return true;
}
