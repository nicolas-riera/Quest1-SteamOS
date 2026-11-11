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

static void qnv_forget(VkImage image);

//! Forget a destroyed image (with a modifier, or NV12).
static void qdb_mod_forget(VkImage image)
{
	qnv_forget(image);
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

// --- venus NV12 as an Android external format
//
// The blob's linear NV12 is not venus's (rows of the width rounded up to 128 bytes, UV plane after the rows rounded
// up to 32), and the blob samples venus NV12 only from a gralloc buffer of that layout, as an Android external
// format with a sampler YCbCr conversion (src/vdectest/vkahbtest.c). So for vrlink an NV12 image with the linear
// modifier is created as an external-format image (format UNDEFINED, VkExternalFormatANDROID); its dma-buf is
// imported as a gralloc NV12 buffer (the native handle of one of our own of the same size, with the fd replaced),
// dedicated to it; its YCbCr conversions and views take the external format instead of NV12.

#define QNV_AHB_FORMAT 0x23 // AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420: venus's NV12 with the video usage
#define QNV_AHB_USAGE (3ull | (1ull << 8) | (1ull << 16)) // CPU read often, GPU sampled image, video encode

static struct
{
	VkImage image;
	uint32_t w, h;
	VkDeviceSize pitch, uv; //!< the caller's plane layouts
} qnv[32];
static atomic_int qnv_n;

static int qnv_find(VkImage image)
{
	if (!image || !atomic_load(&qnv_n))
		return -1;
	int found = -1;
	pthread_mutex_lock(&qdb_lock);
	for (unsigned i = 0; i < sizeof(qnv) / sizeof(qnv[0]) && found < 0; i++)
		if (qnv[i].image == image)
			found = (int)i;
	pthread_mutex_unlock(&qdb_lock);
	return found;
}

static void qnv_forget(VkImage image)
{
	if (!atomic_load(&qnv_n))
		return;
	pthread_mutex_lock(&qdb_lock);
	for (unsigned i = 0; i < sizeof(qnv) / sizeof(qnv[0]); i++)
		if (qnv[i].image == image) {
			qnv[i].image = VK_NULL_HANDLE;
			atomic_fetch_sub(&qnv_n, 1);
		}
	pthread_mutex_unlock(&qdb_lock);
}

//! The external format of venus NV12 gralloc buffers (and its features), from a small one; 0 if there is none.
static uint64_t qnv_external_format(struct device *d, VkDevice device, VkFormatFeatureFlags *features)
{
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	static uint64_t fmt;
	static VkFormatFeatureFlags feat;
	static bool tried;
	pthread_mutex_lock(&lock);
	if (!tried) {
		tried = true;
		pthread_once(&qdb.once, qdb_load);
		PFN_vkGetAndroidHardwareBufferPropertiesANDROID props_of = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)d->gdpa(
		    device, "vkGetAndroidHardwareBufferPropertiesANDROID");
		qdb_desc md = {64, 64, 1, QNV_AHB_FORMAT, QNV_AHB_USAGE, 0, 0, 0};
		AHardwareBuffer *b = NULL;
		if (props_of && qdb.allocate && qdb.allocate(&md, &b) == 0) {
			VkAndroidHardwareBufferFormatPropertiesANDROID fp = {
			    VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
			VkAndroidHardwareBufferPropertiesANDROID bp = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
			                                               &fp};
			if (props_of(device, b, &bp) == VK_SUCCESS) {
				fmt = fp.externalFormat;
				feat = fp.formatFeatures;
			}
			qdb.release(b);
		}
		LOG("dma-buf: venus NV12 is external format 0x%llx (features 0x%x)\n", (unsigned long long)fmt, feat);
	}
	pthread_mutex_unlock(&lock);
	if (features)
		*features = feat;
	return fmt;
}

static VkResult qnv_create_image(struct device *d, VkDevice device, const VkImageCreateInfo *ci,
                                 const VkImageDrmFormatModifierExplicitCreateInfoEXT *ex,
                                 const VkAllocationCallbacks *alloc, VkImage *out)
{
	uint64_t xfmt = qnv_external_format(d, device, NULL);
	if (!xfmt || ex->pPlaneLayouts[0].offset != 0) {
		LOG("dma-buf: NV12 image %ux%u: %s\n", ci->extent.width, ci->extent.height,
		    xfmt ? "first plane not at 0" : "no external format");
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}
	VkExternalFormatANDROID xf = {VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID, NULL, xfmt};
	VkExternalMemoryImageCreateInfo emi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &xf,
	                                       VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
	VkImageCreateInfo c = *ci;
	c.pNext = &emi; // nothing of the caller's chain applies to this image (format lists, the modifier)
	c.flags = 0;
	c.format = VK_FORMAT_UNDEFINED;
	c.tiling = VK_IMAGE_TILING_OPTIMAL;
	c.usage = VK_IMAGE_USAGE_SAMPLED_BIT; // all an external format allows
	VkResult r = d->CreateImage(device, &c, alloc, out);
	if (r != VK_SUCCESS) {
		LOG("dma-buf: NV12 image %ux%u as external format 0x%llx: %d\n", ci->extent.width, ci->extent.height,
		    (unsigned long long)xfmt, r);
		return r;
	}
	qdb_take_offset(*out);
	qdb_mod_forget(*out);
	qnv_forget(*out);
	unsigned i = 0;
	pthread_mutex_lock(&qdb_lock);
	while (i < sizeof(qnv) / sizeof(qnv[0]) && qnv[i].image)
		i++;
	if (i < sizeof(qnv) / sizeof(qnv[0])) {
		qnv[i].image = *out;
		qnv[i].w = ci->extent.width;
		qnv[i].h = ci->extent.height;
		qnv[i].pitch = ex->pPlaneLayouts[0].rowPitch;
		qnv[i].uv = ex->pPlaneLayouts[1].offset;
		atomic_fetch_add(&qnv_n, 1);
	}
	pthread_mutex_unlock(&qdb_lock);
	if (i == sizeof(qnv) / sizeof(qnv[0])) {
		((PFN_vkDestroyImage)d->gdpa(device, "vkDestroyImage"))(device, *out, alloc);
		*out = VK_NULL_HANDLE;
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	if (qdb_logs++ < 16)
		LOG("dma-buf: NV12 image %ux%u (usage 0x%x flags 0x%x, pitch %llu, UV at %llu): external format\n",
		    ci->extent.width, ci->extent.height, ci->usage, ci->flags, (unsigned long long)ex->pPlaneLayouts[0].rowPitch,
		    (unsigned long long)ex->pPlaneLayouts[1].offset);
	return VK_SUCCESS;
}

//! vkAllocateMemory of a dma-buf for an NV12 image of ours (dedicated to it): true if it was one (*r is the result).
static bool qnv_import(struct device *d, VkDevice device, const VkMemoryAllocateInfo *ai, int fd,
                       const VkAllocationCallbacks *alloc, VkDeviceMemory *out, VkResult *r)
{
	const VkMemoryDedicatedAllocateInfo *ded = find_struct(ai->pNext, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
	int i = ded ? qnv_find(ded->image) : -1;
	if (i < 0)
		return false;
	*r = VK_ERROR_INVALID_EXTERNAL_HANDLE;
	pthread_once(&qdb.once, qdb_load);
	PFN_vkGetAndroidHardwareBufferPropertiesANDROID props_of =
	    (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)d->gdpa(device, "vkGetAndroidHardwareBufferPropertiesANDROID");
	AHardwareBuffer *b = NULL;
	off_t size = lseek(fd, 0, SEEK_END);
	// gralloc takes the buffer only when its size is exactly the one it gives that frame: with the video usage it is
	// venus's own (room for the decoder's metadata after the planes), without it just the two planes
	static const uint64_t usages[] = {QNV_AHB_USAGE, QNV_AHB_USAGE & ~(1ull << 16)};
	int sizes[2] = {-1, -1};
	for (unsigned u = 0; u < 2 && !b && props_of && qdb.allocate; u++) {
		qdb_desc md = {qnv[i].w, qnv[i].h, 1, QNV_AHB_FORMAT, usages[u], 0, 0, 0};
		AHardwareBuffer *model = NULL;
		if (qdb.allocate(&md, &model) != 0)
			continue;
		// the model's description as gralloc gives it (its stride, ...): the handle is checked against it
		if (qdb.describe)
			qdb.describe(model, &md);
		const qdb_handle *m = qdb.native_handle(model);
		size_t bytes = sizeof(*m) + (size_t)(m->numFds + m->numInts) * sizeof(int);
		qdb_handle *h = m->numFds >= 1 && m->numInts > QDB_SIZE ? malloc(bytes) : NULL;
		if (h) {
			memcpy(h, m, bytes);
			const int *ints = h->data + h->numFds;
			sizes[u] = ints[QDB_SIZE];
			if (ints[QDB_SIZE] == size) {
				h->data[0] = fd; // the metadata fd stays the model's: the clone duplicates both
				int e = qdb.from_handle(&md, h, QDB_CLONE, &b);
				if (e != 0) {
					LOG("dma-buf: NV12 %ux%u: gralloc refuses the handle (%d, usage 0x%llx)\n", qnv[i].w, qnv[i].h, e,
					    (unsigned long long)usages[u]);
					b = NULL;
				}
			}
			free(h);
		}
		qdb.release(model);
	}
	if (!b) {
		LOG("dma-buf: NV12 %ux%u: a dma-buf of %lld bytes, gralloc's are %d and %d\n", qnv[i].w, qnv[i].h,
		    (long long)size, sizes[0], sizes[1]);
		return true;
	}
	VkAndroidHardwareBufferPropertiesANDROID props = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
	*r = props_of(device, b, &props);
	if (*r == VK_SUCCESS && props.memoryTypeBits) {
		VkMemoryDedicatedAllocateInfo dd = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, ded->image};
		VkImportAndroidHardwareBufferInfoANDROID imp = {VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, &dd, b};
		VkMemoryAllocateInfo mi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, props.allocationSize,
		                           (uint32_t)__builtin_ctz(props.memoryTypeBits)};
		*r = d->AllocateMemory(device, &mi, alloc, out);
	} else if (*r == VK_SUCCESS) {
		*r = VK_ERROR_INVALID_EXTERNAL_HANDLE;
	}
	qdb.release(b);
	if (qdb_logs++ < 16 || *r != VK_SUCCESS)
		LOG("dma-buf: NV12 %ux%u (dma-buf of %lld bytes) imported as a gralloc buffer: %d\n", qnv[i].w, qnv[i].h,
		    (long long)size, *r);
	return true;
}

static VKAPI_ATTR void VKAPI_CALL GetImageMemoryRequirements(VkDevice device, VkImage image, VkMemoryRequirements *req)
{
	struct device *d = device_of(device);
	int i = qnv_find(image);
	if (i < 0) {
		d->GetImageMemoryRequirements(device, image, req);
		return;
	}
	// an external-format image has no requirements of its own before its gralloc buffer is bound: the frame's
	req->size = (qnv[i].uv + qnv[i].pitch * ((qnv[i].h + 1) / 2) + 4095) & ~(VkDeviceSize)4095;
	req->alignment = 4096;
	req->memoryTypeBits = (uint32_t)((1ull << d->memprops.memoryTypeCount) - 1);
}

static void qnv_requirements2(VkDevice device, const VkImageMemoryRequirementsInfo2 *info, VkMemoryRequirements2 *req,
                              PFN_vkGetImageMemoryRequirements2 next)
{
	if (qnv_find(info->image) < 0) {
		next(device, info, req);
		return;
	}
	GetImageMemoryRequirements(device, info->image, &req->memoryRequirements);
	VkMemoryDedicatedRequirements *dr =
	    (VkMemoryDedicatedRequirements *)find_struct(req->pNext, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS);
	if (dr)
		dr->prefersDedicatedAllocation = dr->requiresDedicatedAllocation = VK_TRUE;
}

static VKAPI_ATTR void VKAPI_CALL GetImageMemoryRequirements2(VkDevice device, const VkImageMemoryRequirementsInfo2 *info,
                                                             VkMemoryRequirements2 *req)
{
	qnv_requirements2(device, info, req, device_of(device)->GetImageMemoryRequirements2);
}

static VKAPI_ATTR void VKAPI_CALL GetImageMemoryRequirements2KHR(VkDevice device,
                                                                const VkImageMemoryRequirementsInfo2 *info,
                                                                VkMemoryRequirements2 *req)
{
	struct device *d = device_of(device);
	qnv_requirements2(device, info, req,
	                  d->GetImageMemoryRequirements2KHR ? d->GetImageMemoryRequirements2KHR : d->GetImageMemoryRequirements2);
}

//! An NV12 conversion of vrlink's: the external format's.
static VkResult qnv_conversion(VkDevice device, const VkSamplerYcbcrConversionCreateInfo *ci,
                               const VkAllocationCallbacks *alloc, VkSamplerYcbcrConversion *out,
                               PFN_vkCreateSamplerYcbcrConversion next)
{
	struct device *d = device_of(device);
	VkFormatFeatureFlags feat = 0;
	uint64_t xfmt = 0;
	if (!qdb_front() || ci->format != VK_FORMAT_G8_B8R8_2PLANE_420_UNORM || !(xfmt = qnv_external_format(d, device, &feat)))
		return next(device, ci, alloc, out);
	VkExternalFormatANDROID xf = {VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID, NULL, xfmt};
	VkSamplerYcbcrConversionCreateInfo c = *ci;
	c.pNext = &xf;
	c.format = VK_FORMAT_UNDEFINED;
	if (!(feat & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT))
		c.chromaFilter = VK_FILTER_NEAREST;
	VkResult r = next(device, &c, alloc, out);
	LOG("dma-buf: NV12 YCbCr conversion (model %d, range %d, offsets %d %d, filter %d) on the external format: %d\n",
	    ci->ycbcrModel, ci->ycbcrRange, ci->xChromaOffset, ci->yChromaOffset, c.chromaFilter, r);
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateSamplerYcbcrConversion(VkDevice device,
                                                                  const VkSamplerYcbcrConversionCreateInfo *ci,
                                                                  const VkAllocationCallbacks *alloc,
                                                                  VkSamplerYcbcrConversion *out)
{
	return qnv_conversion(device, ci, alloc, out, device_of(device)->CreateSamplerYcbcrConversion);
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateSamplerYcbcrConversionKHR(VkDevice device,
                                                                     const VkSamplerYcbcrConversionCreateInfo *ci,
                                                                     const VkAllocationCallbacks *alloc,
                                                                     VkSamplerYcbcrConversion *out)
{
	struct device *d = device_of(device);
	return qnv_conversion(device, ci, alloc, out,
	                      d->CreateSamplerYcbcrConversionKHR ? d->CreateSamplerYcbcrConversionKHR
	                                                         : d->CreateSamplerYcbcrConversion);
}

//! A view of one of our NV12 images: on the external format, as a whole (color aspect).
static VKAPI_ATTR VkResult VKAPI_CALL CreateImageView(VkDevice device, const VkImageViewCreateInfo *ci,
                                                     const VkAllocationCallbacks *alloc, VkImageView *out)
{
	PFN_vkCreateImageView next = device_of(device)->CreateImageView;
	if (qnv_find(ci->image) < 0)
		return next(device, ci, alloc, out);
	VkImageViewCreateInfo c = *ci;
	c.format = VK_FORMAT_UNDEFINED;
	c.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	c.components = (VkComponentMapping){0};
	VkResult r = next(device, &c, alloc, out);
	if (qdb_logs++ < 16 || r != VK_SUCCESS)
		LOG("dma-buf: view of an NV12 image (format %d, aspects 0x%x, %s conversion): %d\n", ci->format,
		    ci->subresourceRange.aspectMask,
		    find_struct(ci->pNext, VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO) ? "with a" : "no", r);
	return r;
}

static VkResult qdb_create_modifier_image(struct device *d, VkDevice device, const VkImageCreateInfo *ci,
                                          const VkAllocationCallbacks *alloc, VkImage *out)
{
	const VkImageDrmFormatModifierExplicitCreateInfoEXT *ex =
	    find_struct(ci->pNext, VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT);
	if (ci->format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM && ex && ex->drmFormatModifier == QDB_MOD_LINEAR &&
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
	(void)image;
	p->drmFormatModifier = QDB_MOD_LINEAR;
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
