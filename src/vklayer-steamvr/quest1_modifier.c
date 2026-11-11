// quest1_modifier: VK_EXT_external_memory_dma_buf and VK_EXT_image_drm_format_modifier, linear only (included by
// quest1_compat.c, after quest1_dmabuf.c).
//
// Steam Link VR (vrlink) imports the venus decoder's frames as dma-bufs with an explicit DRM format modifier and
// creates its Vulkan device with both extensions, which the blob does not have (vkCreateDevice failed with
// VK_ERROR_EXTENSION_NOT_PRESENT, "Failed to init Vulkan", 2026-11-11). Here they are, for the one modifier a
// linear image has (DRM_FORMAT_MOD_LINEAR, 0): such an image is created linear, its memory is the dma-buf as a
// gralloc buffer (qdb_import), and it is bound at its first plane's offset (as the planes of quest1_dmabuf.c). The
// plane layouts the caller gives must be the blob's own linear ones (checked; the image is refused otherwise).
// Advertised to vrlink only, so nothing else changes its choices; QUEST1_VK_DMABUF=1 for another program.

#define QDB_MOD_LINEAR 0ull

static bool qdb_front(void)
{
	static int on = -1;
	if (on < 0) {
		const char *e = getenv("QUEST1_VK_DMABUF");
		on = !strcmp(program_invocation_short_name, "vrlink") || (e && atoi(e) == 1);
	}
	return on;
}

static uint32_t qdb_plane_count(VkFormat f)
{
	switch (f) {
	case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
	case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM:
	case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16:
	case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16:
	case VK_FORMAT_G16_B16R16_2PLANE_420_UNORM:
	case VK_FORMAT_G16_B16R16_2PLANE_422_UNORM: return 2;
	case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM:
	case VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM:
	case VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM: return 3;
	default: return 1;
	}
}

//! The aspect of plane p of an image of format f, as the blob takes it.
static VkImageAspectFlags qdb_plane_aspect(VkFormat f, uint32_t p)
{
	return qdb_plane_count(f) == 1 ? VK_IMAGE_ASPECT_COLOR_BIT : (VkImageAspectFlags)VK_IMAGE_ASPECT_PLANE_0_BIT << p;
}

static unsigned qdb_logs; //!< the first calls are logged, to see what a new user asks for

static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFormatProperties2(VkPhysicalDevice pd, VkFormat format,
                                                                    VkFormatProperties2 *p)
{
	struct instance *in = instance_of(pd);
	WITH_UNLINKED(p, in->GetPhysicalDeviceFormatProperties2(pd, format, p));
	if (!qdb_front())
		return;
	VkFormatFeatureFlags lin = p->formatProperties.linearTilingFeatures;
	for (VkBaseOutStructure *s = p->pNext; s; s = s->pNext) {
		if (s->sType == VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT) {
			VkDrmFormatModifierPropertiesListEXT *l = (void *)s;
			if (l->pDrmFormatModifierProperties && l->drmFormatModifierCount && lin)
				l->pDrmFormatModifierProperties[0] =
				    (VkDrmFormatModifierPropertiesEXT){QDB_MOD_LINEAR, qdb_plane_count(format), lin};
			l->drmFormatModifierCount = lin ? 1 : 0;
		} else if (s->sType == VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT) {
			VkDrmFormatModifierPropertiesList2EXT *l = (void *)s;
			if (l->pDrmFormatModifierProperties && l->drmFormatModifierCount && lin)
				l->pDrmFormatModifierProperties[0] =
				    (VkDrmFormatModifierProperties2EXT){QDB_MOD_LINEAR, qdb_plane_count(format), lin};
			l->drmFormatModifierCount = lin ? 1 : 0;
		} else {
			continue;
		}
		if (qdb_logs++ < 16)
			LOG("dma-buf: format %d: modifier LINEAR, %u plane(s), features 0x%x\n", format, qdb_plane_count(format),
			    lin);
	}
}

static VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice pd,
                                                                            const VkPhysicalDeviceImageFormatInfo2 *info,
                                                                            VkImageFormatProperties2 *out)
{
	struct instance *in = instance_of(pd);
	const VkPhysicalDeviceExternalImageFormatInfo *ext =
	    find_struct(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO);
	bool dmabuf = ext && ext->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	bool mod = info->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
	VkResult r;
	if (!dmabuf && !mod) {
		WITH_UNLINKED(out, r = in->GetPhysicalDeviceImageFormatProperties2(pd, info, out));
		return r;
	}
	const VkPhysicalDeviceImageDrmFormatModifierInfoEXT *mi =
	    find_struct(info->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT);
	if (!qdb_front() || (mod && (!mi || mi->drmFormatModifier != QDB_MOD_LINEAR)) ||
	    (dmabuf && info->tiling == VK_IMAGE_TILING_OPTIMAL)) {
		r = VK_ERROR_FORMAT_NOT_SUPPORTED;
	} else {
		// a plain linear image: the blob's limits for it; the dma-buf import is ours
		VkPhysicalDeviceImageFormatInfo2 plain = *info;
		plain.pNext = NULL;
		plain.tiling = VK_IMAGE_TILING_LINEAR;
		WITH_UNLINKED(out, r = in->GetPhysicalDeviceImageFormatProperties2(pd, &plain, out));
		VkExternalImageFormatProperties *ep =
		    (VkExternalImageFormatProperties *)find_struct(out->pNext, VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES);
		if (r == VK_SUCCESS && ep)
			ep->externalMemoryProperties = (VkExternalMemoryProperties){
			    VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT, 0, dmabuf ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT : 0};
	}
	if (qdb_logs++ < 16)
		LOG("dma-buf: image format %d %s, usage 0x%x flags 0x%x%s: %d\n", info->format,
		    mod ? "with a modifier" : "linear", info->usage, info->flags, dmabuf ? ", dma-buf" : "", r);
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL GetMemoryFdProperties(VkDevice device, VkExternalMemoryHandleTypeFlagBits type,
                                                            int fd, VkMemoryFdPropertiesKHR *p)
{
	struct device *d = device_of(device);
	if (type != VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) {
		PFN_vkGetMemoryFdPropertiesKHR next = (PFN_vkGetMemoryFdPropertiesKHR)d->gdpa(device, "vkGetMemoryFdPropertiesKHR");
		return next ? next(device, type, fd, p) : VK_ERROR_INVALID_EXTERNAL_HANDLE;
	}
	// Any memory type: qdb_import takes one of the gralloc buffer's (0x18 here) whatever the caller asks, and the blob
	// binds an image that wants another (0x5 for a linear one) to that memory. A check that the dma-buf can be
	// wrapped is all that is left.
	PFN_vkGetAndroidHardwareBufferPropertiesANDROID props_of =
	    (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)d->gdpa(device, "vkGetAndroidHardwareBufferPropertiesANDROID");
	off_t size = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);
	AHardwareBuffer *b = size > 0 && size <= INT32_MAX && props_of ? qdb_wrap(fd, (uint32_t)size) : NULL;
	VkAndroidHardwareBufferPropertiesANDROID ap = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
	VkResult r = b ? props_of(device, b, &ap) : VK_ERROR_INVALID_EXTERNAL_HANDLE;
	if (b)
		qdb.release(b);
	p->memoryTypeBits = r == VK_SUCCESS && ap.memoryTypeBits ? (uint32_t)((1ull << d->memprops.memoryTypeCount) - 1) : 0;
	if (qdb_logs++ < 16 || r != VK_SUCCESS)
		LOG("dma-buf: fd of %lld bytes: memory types 0x%x (%d)\n", (long long)size, p->memoryTypeBits, r);
	return r == VK_SUCCESS ? VK_SUCCESS : VK_ERROR_INVALID_EXTERNAL_HANDLE;
}

// images created with a modifier, and where their first plane is in the memory they are bound to
static struct
{
	VkImage image;
	VkDeviceSize base;
} qdb_mods[64];
static atomic_int qdb_nmods;

static bool qdb_mod_base(VkImage image, VkDeviceSize *base)
{
	if (!atomic_load(&qdb_nmods))
		return false;
	bool found = false;
	pthread_mutex_lock(&qdb_lock);
	for (unsigned i = 0; i < sizeof(qdb_mods) / sizeof(qdb_mods[0]) && !found; i++)
		if (qdb_mods[i].image == image) {
			*base = qdb_mods[i].base;
			found = true;
		}
	pthread_mutex_unlock(&qdb_lock);
	return found;
}

//! Forget a destroyed image.
static void qdb_mod_forget(VkImage image)
{
	if (!atomic_load(&qdb_nmods))
		return;
	pthread_mutex_lock(&qdb_lock);
	for (unsigned i = 0; i < sizeof(qdb_mods) / sizeof(qdb_mods[0]); i++)
		if (qdb_mods[i].image == image) {
			qdb_mods[i].image = VK_NULL_HANDLE;
			atomic_fetch_sub(&qdb_nmods, 1);
		}
	pthread_mutex_unlock(&qdb_lock);
}

static VkResult qdb_create_modifier_image(struct device *d, VkDevice device, const VkImageCreateInfo *ci,
                                          const VkAllocationCallbacks *alloc, VkImage *out)
{
	const VkImageDrmFormatModifierExplicitCreateInfoEXT *ex =
	    find_struct(ci->pNext, VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT);
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
	(void)image;
	p->drmFormatModifier = QDB_MOD_LINEAR;
	return VK_SUCCESS;
}

//! The memory planes of an image with a modifier are its planes, relative to where it is bound.
static VKAPI_ATTR void VKAPI_CALL GetImageSubresourceLayout(VkDevice device, VkImage image,
                                                           const VkImageSubresource *sub, VkSubresourceLayout *l)
{
	struct device *d = device_of(device);
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
