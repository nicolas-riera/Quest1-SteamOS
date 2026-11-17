// quest1_modifier_hooks: the entry points of the stream path: barriers, image views, modifier images, layouts.
// Part of quest1_modifier.c (included by it, in this order: one translation unit).

//! vkCmdPipelineBarrier: vrcompositor's barrier on an NV12 image of vrlink's, before it reads the frame: the frame is
//! copied into the RGBA image it reads instead. Every barrier then goes on to direct scanout's when it is on.
static VKAPI_ATTR void VKAPI_CALL CmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
                                                     VkDependencyFlags dep, uint32_t nm, const VkMemoryBarrier *m,
                                                     uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni,
                                                     const VkImageMemoryBarrier *im)
{
	int copies[4], n = 0;
	for (uint32_t i = 0; atomic_load(&qcp_n) && i < ni && n < 4; i++) {
		// vrcompositor takes the frame from the foreign queue before it reads it and gives it back after: only
		// the first is a new frame to copy (both were copied, a 1024x4096 pass for nothing each frame, 2026-11-11)
		if (im[i].dstQueueFamilyIndex == VK_QUEUE_FAMILY_FOREIGN_EXT ||
		    im[i].dstQueueFamilyIndex == VK_QUEUE_FAMILY_EXTERNAL)
			continue;
		int k = qcp_find(im[i].image);
		if (k >= 0)
			copies[n++] = k;
	}
	if (n)
		rm_flush(cb); // (a render pass held back for merging is closed first)
	static int scanout = -1;
	if (scanout < 0)
		scanout = !getenv("QUEST1_NO_DISPLAY") && qs_enabled();
	if (scanout)
		qs_CmdPipelineBarrier(cb, src, dst, dep, nm, m, nb, b, ni, im);
	else
		((PFN_vkCmdPipelineBarrier)device_of(cb)->gdpa(device_of(cb)->handle, "vkCmdPipelineBarrier"))(
		    cb, src, dst, dep, nm, m, nb, b, ni, im);
	for (int i = 0; i < n; i++)
		qcp_record(cb, copies[i]);
}

//! A view of one of our NV12 images: on the external format, as a whole (color aspect); for vrcompositor, a view of
//! its RGBA copy instead (above).
//! Any other view goes on to direct scanout's own vkCreateImageView when it is on (quest1_scanout.c: the twin views
//! of the two panel halves, which vrcompositor renders into; this intercept is found first, and without them every
//! frame on the panels was black, 2026-11-11).
static VKAPI_ATTR VkResult VKAPI_CALL CreateImageView(VkDevice device, const VkImageViewCreateInfo *ci,
                                                     const VkAllocationCallbacks *alloc, VkImageView *out)
{
	PFN_vkCreateImageView next = device_of(device)->CreateImageView;
	int n = qnv_find(ci->image);
	if (n < 0)
		return qs_enabled() ? qs_CreateImageView(device, ci, alloc, out) : next(device, ci, alloc, out);
	int k = !qdb_vrlink() ? qcp_get(device_of(device), device, n) : -1;
	if (k >= 0) {
		VkImageViewCreateInfo c = *ci;
		c.pNext = NULL;
		pthread_mutex_lock(&qcs.lock);
		c.image = qcp[k].out;
		pthread_mutex_unlock(&qcs.lock);
		c.format = qcp_srgb_out() ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
		c.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		c.components = (VkComponentMapping){0};
		return next(device, &c, alloc, out);
	}
	VkImageViewCreateInfo c = *ci;
	c.format = VK_FORMAT_UNDEFINED;
	c.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	c.components = (VkComponentMapping){0};
	VkResult r = next(device, &c, alloc, out);
	static atomic_int logs;
	if (atomic_fetch_add(&logs, 1) < 8 || r != VK_SUCCESS) {
		char chain[64] = "";
		for (const VkBaseInStructure *s = ci->pNext; s && strlen(chain) < sizeof(chain) - 12; s = s->pNext)
			snprintf(chain + strlen(chain), sizeof(chain) - strlen(chain), " %d", s->sType);
		LOG("dma-buf: view %p of NV12 image %p (format %d, aspects 0x%x, chain:%s, %s conversion): %d\n",
		    (void *)*out, (void *)ci->image, ci->format, ci->subresourceRange.aspectMask, chain,
		    find_struct(ci->pNext, VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO) ? "with a" : "no", r);
	}
	return r;
}

static VkResult qdb_create_modifier_image(struct device *d, VkDevice device, const VkImageCreateInfo *ci,
                                          const VkAllocationCallbacks *alloc, VkImage *out)
{
	const VkImageDrmFormatModifierExplicitCreateInfoEXT *ex =
	    find_struct(ci->pNext, VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT);
	if (ci->format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM && ex && qdb_nv12_mod(ex->drmFormatModifier) &&
	    ex->drmFormatModifierPlaneCount == 2)
		return qnv_create_image(d, device, ci, ex, alloc, out);
	const VkImageDrmFormatModifierListCreateInfoEXT *li =
	    find_struct(ci->pNext, VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT);
	bool linear = ex && ex->drmFormatModifier == QDB_MOD_LINEAR;
	for (uint32_t i = 0; li && i < li->drmFormatModifierCount; i++)
		linear |= li->pDrmFormatModifiers[i] == QDB_MOD_LINEAR;
	const uint32_t planes = qdb_plane_count(ci->format);
	if (!linear || (ex && ex->drmFormatModifierPlaneCount != planes)) {
		LOG("dma-buf: image %ux%u format %d: only the linear modifier, with one layout per plane\n", ci->extent.width,
		    ci->extent.height, ci->format);
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}
	// a linear image bound at its first plane, with the modifier structures out of the chain for the call
	qdb_plane_offset first = {QDB_STRUCTURE_TYPE_PLANE_OFFSET, NULL, ex ? ex->pPlaneLayouts[0].offset : 0};
	VkBaseOutStructure *cut[2], *before[2];
	int nc = 0;
	VkBaseOutStructure *prev = (VkBaseOutStructure *)ci;
	for (VkBaseOutStructure *s = prev->pNext; s && nc < 2;) {
		VkBaseOutStructure *next = s->pNext;
		if (s->sType == VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT ||
		    s->sType == VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT) {
			cut[nc] = s;
			before[nc++] = prev;
			prev->pNext = next;
		} else {
			prev = s;
		}
		s = next;
	}
	first.pNext = ci->pNext;
	VkImageCreateInfo lin = *ci;
	lin.tiling = VK_IMAGE_TILING_LINEAR;
	lin.pNext = &first;
	VkResult r = qdb_create_image(d, device, &lin, alloc, out);
	for (int i = nc - 1; i >= 0; i--) {
		cut[i]->pNext = before[i]->pNext;
		before[i]->pNext = cut[i];
	}
	if (r != VK_SUCCESS) {
		LOG("dma-buf: linear image %ux%u format %d usage 0x%x: %d\n", ci->extent.width, ci->extent.height, ci->format,
		    ci->usage, r);
		return r;
	}
	// the caller's plane layouts must be the blob's
	bool same = true;
	for (uint32_t p = 0; ex && p < planes; p++) {
		VkImageSubresource sub = {qdb_plane_aspect(ci->format, p), 0, 0};
		VkSubresourceLayout l = {0};
		d->GetImageSubresourceLayout(device, *out, &sub, &l);
		const VkSubresourceLayout *want = &ex->pPlaneLayouts[p];
		if (l.offset != want->offset - first.offset || l.rowPitch != want->rowPitch) {
			LOG("dma-buf: image %ux%u format %d plane %u: offset %llu pitch %llu given, the blob's are %llu and %llu\n",
			    ci->extent.width, ci->extent.height, ci->format, p, (unsigned long long)want->offset,
			    (unsigned long long)want->rowPitch, (unsigned long long)(l.offset + first.offset),
			    (unsigned long long)l.rowPitch);
			same = false;
		}
	}
	unsigned i = 0;
	if (same) {
		pthread_mutex_lock(&qdb_lock);
		while (i < sizeof(qdb_mods) / sizeof(qdb_mods[0]) && qdb_mods[i].image)
			i++;
		if (i < sizeof(qdb_mods) / sizeof(qdb_mods[0])) {
			qdb_mods[i].image = *out;
			qdb_mods[i].base = first.offset;
			atomic_fetch_add(&qdb_nmods, 1);
		}
		pthread_mutex_unlock(&qdb_lock);
	}
	if (!same || i == sizeof(qdb_mods) / sizeof(qdb_mods[0])) {
		qdb_take_offset(*out);
		((PFN_vkDestroyImage)d->gdpa(device, "vkDestroyImage"))(device, *out, alloc);
		*out = VK_NULL_HANDLE;
		return same ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT;
	}
	if (qdb_logs++ < 16)
		LOG("dma-buf: image %ux%u format %d usage 0x%x: linear, %u plane(s), first at %llu\n", ci->extent.width,
		    ci->extent.height, ci->format, ci->usage, planes, (unsigned long long)first.offset);
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL GetImageDrmFormatModifierProperties(VkDevice device, VkImage image,
                                                                         VkImageDrmFormatModifierPropertiesEXT *p)
{
	(void)device;
	int n = qnv_find(image);
	p->drmFormatModifier = n >= 0 ? qnv[n].mod : QDB_MOD_LINEAR;
	return VK_SUCCESS;
}

//! The memory planes of an image with a modifier are its planes, relative to where it is bound.
static VKAPI_ATTR void VKAPI_CALL GetImageSubresourceLayout(VkDevice device, VkImage image,
                                                           const VkImageSubresource *sub, VkSubresourceLayout *l)
{
	struct device *d = device_of(device);
	int n = qnv_find(image);
	if (n >= 0) { // the caller's own layouts: plane 1 (memory or format plane) is UV
		bool uv = sub->aspectMask & (VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT | VK_IMAGE_ASPECT_PLANE_1_BIT);
		*l = (VkSubresourceLayout){uv ? qnv[n].uv : 0, qnv[n].pitch * (uv ? (qnv[n].h + 1) / 2 : qnv[n].h),
		                           qnv[n].pitch, 0, 0};
		return;
	}
	VkDeviceSize base;
	if (!qdb_mod_base(image, &base)) {
		d->GetImageSubresourceLayout(device, image, sub, l);
		return;
	}
	VkImageSubresource s = *sub;
	const VkImageAspectFlags mem = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT | VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT |
	                               VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT;
	if (s.aspectMask & mem) {
		uint32_t p = (uint32_t)(__builtin_ctz(s.aspectMask & mem) - __builtin_ctz(VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT));
		// memory plane p: the color aspect of a one-plane format (p = 0), else plane p
		s.aspectMask = p ? (VkImageAspectFlags)VK_IMAGE_ASPECT_PLANE_0_BIT << p : VK_IMAGE_ASPECT_COLOR_BIT;
		d->GetImageSubresourceLayout(device, image, &s, l);
		if (!p && !l->rowPitch) { // plane 0 of a multi-planar format
			s.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
			d->GetImageSubresourceLayout(device, image, &s, l);
		}
	} else {
		d->GetImageSubresourceLayout(device, image, &s, l);
	}
	l->offset += base;
}
