// quest1_modifier: VK_EXT_external_memory_dma_buf and VK_EXT_image_drm_format_modifier, linear only (included by
// quest1_compat.c, after quest1_dmabuf.c).
//
// Steam Link VR (vrlink) imports the venus decoder's frames as dma-bufs with an explicit DRM format modifier and
// creates its Vulkan device with both extensions, which the blob does not have (vkCreateDevice failed with
// VK_ERROR_EXTENSION_NOT_PRESENT, "Failed to init Vulkan", 2026-11-11). Here they are, for the one modifier a
// linear image has (DRM_FORMAT_MOD_LINEAR, 0): such an image is created linear, its memory is the dma-buf as a
// gralloc buffer (qdb_import), and it is bound at its first plane's offset (as the planes of quest1_dmabuf.c). The
// plane layouts the caller gives must be the blob's own linear ones (checked; the image is refused otherwise).
// Advertised to vrlink and vrcompositor only (below), so nothing else changes its choices; QUEST1_VK_DMABUF=1 makes
// another program vrlink.
//
// vrlink makes its decoder's frame buffers itself (2026-11-11): NV12 images with an explicit modifier, which is
// QCOM_COMPRESSED (UBWC, the Steam Frame decoder's), dedicated memory exported as a dma-buf (vkGetMemoryFdKHR), the
// fd queued on the decoder (V4L2 DMABUF) and handed to vrcompositor (IVRIPCResourceManagerClient::ImportDmabuf),
// which imports it to show the frame. Our decoder writes linear venus NV12 (libquest1_v4l2.so), so both modifiers
// of NV12 are that here: the memory is a gralloc NV12 buffer of ours (venus's layout, an ION buffer the decoder
// takes), the image its external format (below). vrcompositor gets the two extensions too, with modifiers for NV12
// only: the compositor offers its clients no other dma-buf format, so nothing else moves to dma-bufs.

#define QDB_MOD_LINEAR 0ull
#define QDB_MOD_QCOM_COMPRESSED 0x0500000000000001ull // fourcc_mod_code(QCOM, 1)

//! vrlink: the linear modifier for every format, imports of any dma-buf.
static bool qdb_vrlink(void)
{
	static int on = -1;
	if (on < 0) {
		const char *e = getenv("QUEST1_VK_DMABUF");
		on = !strcmp(program_invocation_short_name, "vrlink") || (e && atoi(e) == 1);
	}
	return on;
}

//! vrlink or vrcompositor: the dma-buf and modifier extensions.
static bool qdb_front(void)
{
	static int on = -1;
	if (on < 0)
		on = qdb_vrlink() || !strcmp(program_invocation_short_name, "vrcompositor");
	return on;
}

//! A modifier an NV12 image can have here (both are linear venus NV12).
static bool qdb_nv12_mod(uint64_t m)
{
	return m == QDB_MOD_LINEAR || m == QDB_MOD_QCOM_COMPRESSED;
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
static void qnv_prewarm(void);

static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFormatProperties2(VkPhysicalDevice pd, VkFormat format,
                                                                    VkFormatProperties2 *p)
{
	struct instance *in = instance_of(pd);
	WITH_UNLINKED(p, in->GetPhysicalDeviceFormatProperties2(pd, format, p));
	if (!qdb_front())
		return;
	VkFormatFeatureFlags lin = p->formatProperties.linearTilingFeatures;
	bool nv12 = format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	if (nv12)
		qnv_prewarm(); // vrlink asks at its start, seconds before the PC streams
	if (!nv12 && !qdb_vrlink())
		lin = 0;
	static const uint64_t mods[] = {QDB_MOD_LINEAR, QDB_MOD_QCOM_COMPRESSED};
	uint32_t n = !lin ? 0 : nv12 ? 2 : 1;
	for (VkBaseOutStructure *s = p->pNext; s; s = s->pNext) {
		if (s->sType == VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT) {
			VkDrmFormatModifierPropertiesListEXT *l = (void *)s;
			for (uint32_t i = 0; l->pDrmFormatModifierProperties && i < n && i < l->drmFormatModifierCount; i++)
				l->pDrmFormatModifierProperties[i] = (VkDrmFormatModifierPropertiesEXT){mods[i], qdb_plane_count(format), lin};
			l->drmFormatModifierCount = l->pDrmFormatModifierProperties && l->drmFormatModifierCount < n ? l->drmFormatModifierCount : n;
		} else if (s->sType == VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT) {
			VkDrmFormatModifierPropertiesList2EXT *l = (void *)s;
			for (uint32_t i = 0; l->pDrmFormatModifierProperties && i < n && i < l->drmFormatModifierCount; i++)
				l->pDrmFormatModifierProperties[i] =
				    (VkDrmFormatModifierProperties2EXT){mods[i], qdb_plane_count(format), lin};
			l->drmFormatModifierCount = l->pDrmFormatModifierProperties && l->drmFormatModifierCount < n ? l->drmFormatModifierCount : n;
		} else {
			continue;
		}
		if (n && qdb_logs++ < 16)
			LOG("dma-buf: format %d: %u modifier(s), %u plane(s), features 0x%x\n", format, n, qdb_plane_count(format), lin);
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
	bool nv12 = info->format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	if (!qdb_front() || (!nv12 && !qdb_vrlink()) ||
	    (mod && (!mi || (mi->drmFormatModifier != QDB_MOD_LINEAR && !(nv12 && qdb_nv12_mod(mi->drmFormatModifier))))) ||
	    (dmabuf && info->tiling == VK_IMAGE_TILING_OPTIMAL)) {
		r = VK_ERROR_FORMAT_NOT_SUPPORTED;
	} else {
		// a plain linear image: the blob's limits for it; the dma-buf import (and NV12's export) is ours
		VkPhysicalDeviceImageFormatInfo2 plain = *info;
		plain.pNext = NULL;
		plain.tiling = VK_IMAGE_TILING_LINEAR;
		WITH_UNLINKED(out, r = in->GetPhysicalDeviceImageFormatProperties2(pd, &plain, out));
		VkExternalImageFormatProperties *ep =
		    (VkExternalImageFormatProperties *)find_struct(out->pNext, VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES);
		VkExternalMemoryFeatureFlags feat = VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
		if (nv12) // a gralloc buffer of ours, dedicated to the image
			feat |= VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT;
		VkExternalMemoryHandleTypeFlags t = dmabuf ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT : 0;
		if (r == VK_SUCCESS && ep)
			ep->externalMemoryProperties = (VkExternalMemoryProperties){feat, nv12 ? t : 0, t};
	}
	if (qdb_logs++ < 16)
		LOG("dma-buf: image format %d %s 0x%llx, usage 0x%x flags 0x%x%s: %d\n", info->format,
		    mod ? "with modifier" : "linear", mod && mi ? (unsigned long long)mi->drmFormatModifier : 0ull, info->usage,
		    info->flags, dmabuf ? ", dma-buf" : "", r);
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
	uint64_t mod;           //!< the caller's modifier
} qnv[128]; // (vrcompositor keeps the last sets of vrlink while it makes a new one at each decoder reset)
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

// Images with a modifier and NV12 images are forgotten when destroyed: else only a new image that got the same
// handle took an entry back, and vrcompositor ran out of them after a few of vrlink's decoder resets (2026-11-11:
// "Unable to create VkImage", and vrlink waited for its texture forever).
static void qcp_forget(VkDevice device, VkImage nv12);

static VKAPI_ATTR void VKAPI_CALL DestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *alloc)
{
	struct device *d = device_of(device);
	if (image)
		qcp_forget(device, image); // (its RGBA copy's view of it goes first)
	((PFN_vkDestroyImage)d->gdpa(device, "vkDestroyImage"))(device, image, alloc);
	if (image)
		qdb_mod_forget(image);
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
	qnv_prewarm();
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
		qnv[i].mod = ex->drmFormatModifier;
		atomic_fetch_add(&qnv_n, 1);
	}
	pthread_mutex_unlock(&qdb_lock);
	if (i == sizeof(qnv) / sizeof(qnv[0])) {
		LOG("dma-buf: NV12 image %ux%u: %u of them already\n", ci->extent.width, ci->extent.height, i);
		((PFN_vkDestroyImage)d->gdpa(device, "vkDestroyImage"))(device, *out, alloc);
		*out = VK_NULL_HANDLE;
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	if (qdb_logs++ < 16)
		LOG("dma-buf: NV12 image %p %ux%u (modifier 0x%llx, usage 0x%x flags 0x%x, pitch %llu, UV at %llu): external format\n",
		    (void *)*out, ci->extent.width, ci->extent.height, (unsigned long long)ex->drmFormatModifier, ci->usage, ci->flags,
		    (unsigned long long)ex->pPlaneLayouts[0].rowPitch, (unsigned long long)ex->pPlaneLayouts[1].offset);
	return VK_SUCCESS;
}

// The gralloc buffer whose handle an imported NV12 dma-buf takes as a model, and its description as gralloc gives it
// (its stride, ...: the handle is checked against it). Kept: making one for each import was a 6 MB buffer each
// time, while vrlink waits for vrcompositor to import its 16 frame buffers.
static AHardwareBuffer *qnv_model(unsigned w, unsigned h, uint64_t usage, qdb_desc *md)
{
	static struct
	{
		AHardwareBuffer *b;
		unsigned w, h;
		uint64_t usage;
		qdb_desc md;
	} models[4]; // (two usages are tried, for one size in practice)
	AHardwareBuffer *b = NULL;
	pthread_mutex_lock(&qdb_lock);
	unsigned k = 0;
	for (; k < 4 && models[k].b; k++)
		if (models[k].w == w && models[k].h == h && models[k].usage == usage) {
			b = models[k].b;
			*md = models[k].md;
			break;
		}
	if (!b && k < 4) {
		qdb_desc d = {w, h, 1, QNV_AHB_FORMAT, usage, 0, 0, 0};
		if (qdb.allocate(&d, &b) == 0) {
			if (qdb.describe)
				qdb.describe(b, &d);
			*md = d;
			models[k].b = b;
			models[k].w = w;
			models[k].h = h;
			models[k].usage = usage;
			models[k].md = d;
		} else {
			b = NULL;
		}
	}
	pthread_mutex_unlock(&qdb_lock);
	return b;
}

// The dma-buf of each NV12 import, for vkGetMemoryFdKHR: vrcompositor exports what it imported again ("prepare image
// for export", as a cross-process shared resource), which the blob cannot do for memory of a gralloc buffer; without
// it every frame vrlink submits is an invalid texture (2026-11-11).
static struct
{
	VkDeviceMemory mem;
	int fd;
} qnv_imp[128];
static atomic_int qnv_nimp;

static void qnv_imported(VkDeviceMemory m, int fd)
{
	int own = fcntl(fd, F_DUPFD_CLOEXEC, 0);
	unsigned k = 0;
	pthread_mutex_lock(&qdb_lock);
	while (k < sizeof(qnv_imp) / sizeof(qnv_imp[0]) && qnv_imp[k].mem)
		k++;
	if (own >= 0 && k < sizeof(qnv_imp) / sizeof(qnv_imp[0])) {
		qnv_imp[k].mem = m;
		qnv_imp[k].fd = own;
		atomic_fetch_add(&qnv_nimp, 1);
		own = -1;
	}
	pthread_mutex_unlock(&qdb_lock);
	if (own >= 0)
		close(own);
}

//! A new fd of the dma-buf an NV12 import was made of, -1 if m is none.
static int qnv_import_fd(VkDeviceMemory m)
{
	int fd = -1;
	if (!m || !atomic_load(&qnv_nimp))
		return -1;
	pthread_mutex_lock(&qdb_lock);
	for (unsigned k = 0; k < sizeof(qnv_imp) / sizeof(qnv_imp[0]) && fd < 0; k++)
		if (qnv_imp[k].mem == m)
			fd = fcntl(qnv_imp[k].fd, F_DUPFD_CLOEXEC, 0);
	pthread_mutex_unlock(&qdb_lock);
	return fd;
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
		qdb_desc md;
		AHardwareBuffer *model = qnv_model(qnv[i].w, qnv[i].h, usages[u], &md);
		if (!model)
			continue;
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
		if (*r == VK_SUCCESS)
			qnv_imported(*out, fd);
	} else if (*r == VK_SUCCESS) {
		*r = VK_ERROR_INVALID_EXTERNAL_HANDLE;
	}
	qdb.release(b);
	if (qdb_logs++ < 16 || *r != VK_SUCCESS)
		LOG("dma-buf: NV12 %ux%u (dma-buf of %lld bytes) imported as a gralloc buffer: %d\n", qnv[i].w, qnv[i].h,
		    (long long)size, *r);
	return true;
}

// --- NV12 memory made here and exported as a dma-buf (vrlink's frame buffers)

static struct
{
	VkDeviceMemory mem;
	AHardwareBuffer *b; //!< ours: its fd is the dma-buf
	unsigned w, h;
} qnv_exp[128];
static atomic_int qnv_nexp;

// vrlink sets its 16 frame buffers up in its decoder thread, which takes no packet buffer back meanwhile, and gives
// up when the decoder holds all 16 (220 ms at 72 fps). A gralloc buffer of 6 MB took 13 to 66 ms to make, 1.1 s for
// the 16 (2026-11-11); the Frame's driver takes a few. So the buffers vrlink frees stay here for its next set (it
// sets them up again at each reset of its decoder), and 16 of its size are made in the background as soon as it
// asks about NV12, before the PC streams.
#define QNV_POOL 32
#define QNV_PREWARM_N 16
#define QNV_PREWARM_W 1024 // vrlink's atlas for the Quest 1 identity: four 1024x1024 views
#define QNV_PREWARM_H 4096
static struct
{
	AHardwareBuffer *b;
	unsigned w, h;
} qnv_pool[QNV_POOL];

static AHardwareBuffer *qnv_pool_take(unsigned w, unsigned h)
{
	AHardwareBuffer *b = NULL;
	pthread_mutex_lock(&qdb_lock);
	for (unsigned k = 0; k < QNV_POOL && !b; k++)
		if (qnv_pool[k].b && qnv_pool[k].w == w && qnv_pool[k].h == h) {
			b = qnv_pool[k].b;
			qnv_pool[k].b = NULL;
		}
	pthread_mutex_unlock(&qdb_lock);
	return b;
}

static void qnv_pool_put(AHardwareBuffer *b, unsigned w, unsigned h)
{
	unsigned k = 0;
	pthread_mutex_lock(&qdb_lock);
	while (k < QNV_POOL && qnv_pool[k].b)
		k++;
	if (k < QNV_POOL) {
		qnv_pool[k].b = b;
		qnv_pool[k].w = w;
		qnv_pool[k].h = h;
	}
	pthread_mutex_unlock(&qdb_lock);
	if (k == QNV_POOL)
		qdb.release(b);
}

static void *qnv_prewarm_run(void *arg)
{
	(void)arg;
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	unsigned n = 0;
	for (; n < QNV_PREWARM_N; n++) {
		qdb_desc md = {QNV_PREWARM_W, QNV_PREWARM_H, 1, QNV_AHB_FORMAT, QNV_AHB_USAGE, 0, 0, 0};
		AHardwareBuffer *b = NULL;
		if (qdb.allocate(&md, &b) != 0)
			break;
		qnv_pool_put(b, QNV_PREWARM_W, QNV_PREWARM_H);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	LOG("dma-buf: %u NV12 %ux%u frame buffers made ahead in %.0f ms\n", n, QNV_PREWARM_W, QNV_PREWARM_H,
	    (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
	return NULL;
}

static void qnv_prewarm_once(void)
{
	pthread_once(&qdb.once, qdb_load);
	pthread_t t;
	if (qdb.allocate && pthread_create(&t, NULL, qnv_prewarm_run, NULL) == 0)
		pthread_detach(t);
}

//! vrlink's frame buffers, made in the background (once)
static void qnv_prewarm(void)
{
	static pthread_once_t once = PTHREAD_ONCE_INIT;
	if (qdb_vrlink())
		pthread_once(&once, qnv_prewarm_once);
}

//! vkAllocateMemory of exportable (dma-buf) memory for an NV12 image of ours: true if it was one (*r is the result).
static bool qnv_export(struct device *d, VkDevice device, const VkMemoryAllocateInfo *ai,
                       const VkAllocationCallbacks *alloc, VkDeviceMemory *out, VkResult *r)
{
	const VkExportMemoryAllocateInfo *ex = find_struct(ai->pNext, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
	const VkMemoryDedicatedAllocateInfo *ded = find_struct(ai->pNext, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
	int i = ex && (ex->handleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) && ded ? qnv_find(ded->image) : -1;
	if (i < 0)
		return false;
	pthread_once(&qdb.once, qdb_load);
	PFN_vkGetAndroidHardwareBufferPropertiesANDROID props_of =
	    (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)d->gdpa(device, "vkGetAndroidHardwareBufferPropertiesANDROID");
	qdb_desc md = {qnv[i].w, qnv[i].h, 1, QNV_AHB_FORMAT, QNV_AHB_USAGE, 0, 0, 0};
	AHardwareBuffer *b = props_of ? qnv_pool_take(qnv[i].w, qnv[i].h) : NULL;
	if (!b && (!props_of || !qdb.allocate || qdb.allocate(&md, &b) != 0)) {
		LOG("dma-buf: NV12 %ux%u: no gralloc buffer\n", qnv[i].w, qnv[i].h);
		*r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
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
		*r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}
	unsigned k = 0;
	if (*r == VK_SUCCESS) {
		pthread_mutex_lock(&qdb_lock);
		while (k < sizeof(qnv_exp) / sizeof(qnv_exp[0]) && qnv_exp[k].mem)
			k++;
		if (k < sizeof(qnv_exp) / sizeof(qnv_exp[0])) {
			qnv_exp[k].mem = *out;
			qnv_exp[k].b = b;
			qnv_exp[k].w = qnv[i].w;
			qnv_exp[k].h = qnv[i].h;
			atomic_fetch_add(&qnv_nexp, 1);
		}
		pthread_mutex_unlock(&qdb_lock);
		if (k == sizeof(qnv_exp) / sizeof(qnv_exp[0])) {
			d->FreeMemory(device, *out, alloc);
			*out = VK_NULL_HANDLE;
			*r = VK_ERROR_TOO_MANY_OBJECTS;
		}
	}
	if (*r != VK_SUCCESS)
		qdb.release(b);
	if (qdb_logs++ < 16 || *r != VK_SUCCESS)
		LOG("dma-buf: NV12 %ux%u exportable, a gralloc buffer of %llu bytes: %d\n", qnv[i].w, qnv[i].h,
		    (unsigned long long)props.allocationSize, *r);
	return true;
}

static AHardwareBuffer *qnv_exported(VkDeviceMemory m)
{
	AHardwareBuffer *b = NULL;
	if (!m || !atomic_load(&qnv_nexp))
		return NULL;
	pthread_mutex_lock(&qdb_lock);
	for (unsigned k = 0; k < sizeof(qnv_exp) / sizeof(qnv_exp[0]) && !b; k++)
		if (qnv_exp[k].mem == m)
			b = qnv_exp[k].b;
	pthread_mutex_unlock(&qdb_lock);
	return b;
}

//! After vkFreeMemory: our gralloc buffer of exported memory goes too.
static void qnv_free_memory(VkDeviceMemory m)
{
	if (m && atomic_load(&qnv_nimp)) {
		int fd = -1;
		pthread_mutex_lock(&qdb_lock);
		for (unsigned k = 0; k < sizeof(qnv_imp) / sizeof(qnv_imp[0]) && fd < 0; k++)
			if (qnv_imp[k].mem == m) {
				fd = qnv_imp[k].fd;
				qnv_imp[k].mem = VK_NULL_HANDLE;
				atomic_fetch_sub(&qnv_nimp, 1);
			}
		pthread_mutex_unlock(&qdb_lock);
		if (fd >= 0) {
			close(fd);
			return;
		}
	}
	if (!m || !atomic_load(&qnv_nexp))
		return;
	AHardwareBuffer *b = NULL;
	unsigned w = 0, h = 0;
	pthread_mutex_lock(&qdb_lock);
	for (unsigned k = 0; k < sizeof(qnv_exp) / sizeof(qnv_exp[0]) && !b; k++)
		if (qnv_exp[k].mem == m) {
			b = qnv_exp[k].b;
			w = qnv_exp[k].w;
			h = qnv_exp[k].h;
			qnv_exp[k].mem = VK_NULL_HANDLE;
			atomic_fetch_sub(&qnv_nexp, 1);
		}
	pthread_mutex_unlock(&qdb_lock);
	if (b)
		qnv_pool_put(b, w, h); // for vrlink's next set
}

static VKAPI_ATTR VkResult VKAPI_CALL GetMemoryFd(VkDevice device, const VkMemoryGetFdInfoKHR *gi, int *fd)
{
	struct device *d = device_of(device);
	if (gi->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT ||
	    gi->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT) {
		// an NV12 import: its dma-buf again, as either kind (an OPAQUE_FD one comes back to qnv_import)
		*fd = qnv_import_fd(gi->memory);
		if (*fd >= 0) {
			static atomic_int logs;
			if (atomic_fetch_add(&logs, 1) < 2)
				LOG("dma-buf: NV12 import exported again (handle type 0x%x)\n", gi->handleType);
			return VK_SUCCESS;
		}
	}
	if (gi->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) {
		AHardwareBuffer *b = qnv_exported(gi->memory);
		const qdb_handle *h = b ? qdb.native_handle(b) : NULL;
		*fd = h && h->numFds >= 1 ? fcntl(h->data[0], F_DUPFD_CLOEXEC, 0) : -1;
		if (*fd < 0)
			LOG("dma-buf: no fd for memory %p (not exportable here)\n", (void *)gi->memory);
		return *fd >= 0 ? VK_SUCCESS : VK_ERROR_TOO_MANY_OBJECTS;
	}
	PFN_vkGetMemoryFdKHR next = (PFN_vkGetMemoryFdKHR)d->gdpa(device, "vkGetMemoryFdKHR");
	return next ? next(device, gi, fd) : VK_ERROR_EXTENSION_NOT_PRESENT;
}

static VKAPI_ATTR void VKAPI_CALL GetImageMemoryRequirements(VkDevice device, VkImage image, VkMemoryRequirements *req)
{
	struct device *d = device_of(device);
	int i = qnv_find(image);
	if (i < 0) {
		d->GetImageMemoryRequirements(device, image, req);
		return;
	}
	// an external-format image has no requirements of its own before its gralloc buffer is bound: venus's frame
	// (VENUS_BUFFER_SIZE: planes of rows rounded up to 128 bytes, 32 and 16 lines, 4096 bytes, and the decoder's
	// metadata: 16 kB or 8 rows),
	// whatever layouts the caller gave (vrlink's are for UBWC)
	VkDeviceSize stride = (qnv[i].w + 127) & ~127u, y = stride * ((qnv[i].h + 31) & ~31u),
	             uv = stride * ((((qnv[i].h + 1) / 2) + 15) & ~15u), extra = 8 * stride > 16384 ? 8 * stride : 16384;
	req->size = (y + uv + 4096 + extra + 4095) & ~(VkDeviceSize)4095;
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
	if (!qdb_front() || ci->format != VK_FORMAT_G8_B8R8_2PLANE_420_UNORM || !(xfmt = qnv_external_format(d, device, &feat))) {
		static atomic_int logs;
		if (qdb_front() && atomic_fetch_add(&logs, 1) < 4)
			LOG("dma-buf: YCbCr conversion of format %d left as it is\n", ci->format);
		return next(device, ci, alloc, out);
	}
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

// --- vrlink's frames in vrcompositor: an RGBA copy of each NV12 frame
//
// vrcompositor draws a frame of vrlink in its distortion pass (here distort_ps_layered: the Frame's
// distort_ps_achromatic_se2, which would blend the inset in, is not used), through a plain view of the NV12 image and
// a separate sampler. The blob samples NV12 only through a YCbCr conversion, which takes an
// immutable combined sampler, so nothing came out (black, 2026-11-11). Here each of vrcompositor's NV12 images has an
// RGBA8 copy, whose views vrcompositor gets instead of NV12 ones, filled right after vrcompositor's barrier on the NV12
// image, which comes each frame before its distortion pass reads it. The copy holds RGB (BT.709, full range):
// vrcompositor shows what it samples as it is, and raw YCbCr (R = Cr, G = Y, B = Cb) came out magenta.
// vrcompositor reads each eye's wide view only, so the copy goes on to an output at its eye buffers' size where each
// wide view gets its foveated inset blended in (nv12_inset.frag, the insets' places from vrlink: quest1_views.h).
// Without it the centre showed the wide view's, coded with few bits under the inset: blocks as soon as anything moved
// (user, 2026-11-12: "art abstrait"; sharp with it). It costs ~2.5 ms of GPU a frame (58-60 fps against ~69 with the
// dashboard open, panels forced on).
// The copies and outputs of a size share one memory: each frame copies only the frame it then reads, after the reads
// of the frames before it (the passes' dependency), so one of each is all they need. vrlink shares 16 frame buffers:
// 16 copies of 16 MB each were made at once as the stream started, and the headset swapped hard (2026-11-12).

#include "quest1_nv12_spv.h"
#include "quest1_views.h"

enum { QCP_LEVELS = 3 }; //!< the copy and two halvings, for the inset that comes out smaller (see qcp_record)

static struct qnv_copy
{
	VkImage nv12; //!< vrcompositor's NV12 image
	VkImage img;  //!< the RGBA8 copy with its smaller levels, bound to the shared memory of its size
	VkImage out;  //!< what vrcompositor reads: the wide views with their insets, at its eye buffers' size
	int mem, out_mem;          //!< their memories (qcs.mem)
	VkImageView src, dst;      //!< the NV12 through the conversion, the copy as an attachment
	VkImageView all, out_view; //!< the copy with its levels, the output as an attachment
	VkFramebuffer fb, out_fb;
	VkDescriptorSet set, out_set;
	uint32_t w, h, ow, oh;
} qcp[128];
static atomic_int qcp_n;

static struct
{
	pthread_mutex_t lock;
	VkDevice device;
	bool tried, ok;
	VkSamplerYcbcrConversion conv;
	VkSampler sampler;
	VkDescriptorSetLayout dsl;
	VkPipelineLayout pl;
	VkRenderPass rp;
	VkPipeline pipe;
	VkSampler sampler2; //!< the inset pass's: linear, with levels
	VkDescriptorSetLayout dsl2;
	VkPipelineLayout pl2;
	VkPipeline pipe2;
	VkDescriptorPool pool;
	struct
	{
		VkDeviceMemory mem;
		VkDeviceSize size;
		uint32_t type;
		int users;
	} mem[4]; //!< the copies' and outputs' memories, one per size
} qcs = {PTHREAD_MUTEX_INITIALIZER};

//! log2 of x >= 1 within 0.01 (0 below; the layer does without libm)
static float qcp_log2(float x)
{
	float e = 0;
	while (x >= 2.0f) {
		x *= 0.5f;
		e += 1.0f;
	}
	return x < 1.0f ? 0.0f : e + (x - 1.0f) * (1.4427f - 0.4427f * (x - 1.0f));
}

//! nv12_inset.frag's push constants
struct qcp_inset
{
	float place[4]; //!< the inset over the wide view, in the atlas (u1 <= u0: none)
	float view[4];  //!< the inset's own view
	float lod;
};

//! vrlink's views (quest1_views.h) in vrcompositor, or false when it published none.
static bool qcp_views(struct q1_views *out)
{
	static const struct q1_views *v;
	static uint64_t tried_ns;
	if (strcmp(program_invocation_short_name, "vrcompositor") != 0) // (not test_modifier's frames)
		return false;
	if (!v) {
		uint64_t t = now_ns();
		if (t - tried_ns < 1000000000ull) // vrlink writes it at its first frame
			return false;
		tried_ns = t;
		int fd = open(Q1_VIEWS_PATH, O_RDONLY | O_CLOEXEC);
		void *m = fd >= 0 ? mmap(NULL, sizeof(*v), PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
		if (fd >= 0)
			close(fd);
		if (m == MAP_FAILED)
			return false;
		v = m;
		LOG("dma-buf: Steam Link VR's foveated insets from %s\n", Q1_VIEWS_PATH);
	}
	for (int tries = 0; tries < 8; tries++) {
		uint32_t seq = __atomic_load_n(&v->seq, __ATOMIC_ACQUIRE);
		if (seq & 1)
			continue;
		memcpy(out, (const void *)v, sizeof(*out));
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (__atomic_load_n(&v->seq, __ATOMIC_RELAXED) == seq)
			return out->eyes == 2;
	}
	return false;
}

//! Call with qcs.lock held: the conversion, sampler, pipeline and pool, once per device.
static bool qcs_init(struct device *d, VkDevice device)
{
	if (qcs.tried)
		return qcs.ok && qcs.device == device;
	qcs.tried = true;
	qcs.device = device;
	VkFormatFeatureFlags feat = 0;
	uint64_t xfmt = qnv_external_format(d, device, &feat);
	if (!xfmt)
		return false;
	VkExternalFormatANDROID xf = {VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID, NULL, xfmt};
	VkSamplerYcbcrConversionCreateInfo cc = {VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO, &xf,
	                                         VK_FORMAT_UNDEFINED, VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,
	                                         VK_SAMPLER_YCBCR_RANGE_ITU_FULL, {0},
	                                         (feat & VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT)
	                                             ? VK_CHROMA_LOCATION_COSITED_EVEN : VK_CHROMA_LOCATION_MIDPOINT,
	                                         VK_CHROMA_LOCATION_MIDPOINT, VK_FILTER_NEAREST, VK_FALSE};
	if (feat & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT)
		cc.chromaFilter = VK_FILTER_LINEAR;
	VkResult r = d->CreateSamplerYcbcrConversion(device, &cc, NULL, &qcs.conv);
	VkSamplerYcbcrConversionInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO, NULL, qcs.conv};
	// the copy is the frame's size: each pixel is read where it is (the conversion wants its chroma filter here too)
	VkSamplerCreateInfo sc = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, &ci};
	sc.magFilter = sc.minFilter = cc.chromaFilter;
	sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	if (r == VK_SUCCESS)
		r = ((PFN_vkCreateSampler)d->gdpa(device, "vkCreateSampler"))(device, &sc, NULL, &qcs.sampler);
	VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
	                                  &qcs.sampler};
	VkDescriptorSetLayoutCreateInfo dc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b};
	if (r == VK_SUCCESS)
		r = d->CreateDescriptorSetLayout(device, &dc, NULL, &qcs.dsl);
	VkPipelineLayoutCreateInfo plc = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &qcs.dsl};
	if (r == VK_SUCCESS)
		r = d->CreatePipelineLayout(device, &plc, NULL, &qcs.pl);
	VkAttachmentDescription att = {0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	                               VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	                               VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL};
	VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription sub = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ref};
	VkSubpassDependency dep[2] = {
	    // after the copy's last readers (the frames before), and the decoder's frame (vrcompositor waited for it)
	    {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
	     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, 0},
	    // before vrcompositor reads the copy
	    {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT, 0},
	};
	VkRenderPassCreateInfo rc = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &att, 1, &sub, 2, dep};
	if (r == VK_SUCCESS)
		r = d->CreateRenderPass(device, &rc, NULL, &qcs.rp);
	PFN_vkCreateShaderModule csm = (PFN_vkCreateShaderModule)d->gdpa(device, "vkCreateShaderModule");
	PFN_vkDestroyShaderModule dsm = (PFN_vkDestroyShaderModule)d->gdpa(device, "vkDestroyShaderModule");
	VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
	VkShaderModuleCreateInfo mv = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(nv12_copy_vert),
	                               nv12_copy_vert};
	VkShaderModuleCreateInfo mf = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(nv12_copy_frag),
	                               nv12_copy_frag};
	if (r == VK_SUCCESS)
		r = csm(device, &mv, NULL, &vs);
	if (r == VK_SUCCESS)
		r = csm(device, &mf, NULL, &fs);
	VkPipelineShaderStageCreateInfo st[2] = {
	    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"},
	    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"},
	};
	VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
	                                             VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
	VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1};
	VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineColorBlendAttachmentState ba = {0};
	ba.colorWriteMask = 0xf;
	VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = 1;
	cb.pAttachments = &ba;
	VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn};
	VkGraphicsPipelineCreateInfo gp = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	gp.stageCount = 2;
	gp.pStages = st;
	gp.pVertexInputState = &vi;
	gp.pInputAssemblyState = &ia;
	gp.pViewportState = &vp;
	gp.pRasterizationState = &rs;
	gp.pMultisampleState = &ms;
	gp.pColorBlendState = &cb;
	gp.pDynamicState = &ds;
	gp.layout = qcs.pl;
	gp.renderPass = qcs.rp;
	if (r == VK_SUCCESS)
		r = d->CreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, NULL, &qcs.pipe);
	if (fs)
		dsm(device, fs, NULL);
	fs = VK_NULL_HANDLE;
	// the inset pass (nv12_inset.frag): the copy through a linear sampler with its levels, the inset in push constants
	VkSamplerCreateInfo sc2 = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sc2.magFilter = sc2.minFilter = VK_FILTER_LINEAR;
	sc2.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	sc2.addressModeU = sc2.addressModeV = sc2.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sc2.maxLod = QCP_LEVELS;
	if (r == VK_SUCCESS)
		r = ((PFN_vkCreateSampler)d->gdpa(device, "vkCreateSampler"))(device, &sc2, NULL, &qcs.sampler2);
	b.pImmutableSamplers = &qcs.sampler2;
	if (r == VK_SUCCESS)
		r = d->CreateDescriptorSetLayout(device, &dc, NULL, &qcs.dsl2);
	VkPushConstantRange pr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(struct qcp_inset)};
	VkPipelineLayoutCreateInfo plc2 = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &qcs.dsl2, 1, &pr};
	if (r == VK_SUCCESS)
		r = d->CreatePipelineLayout(device, &plc2, NULL, &qcs.pl2);
	VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(nv12_inset_frag),
	                               nv12_inset_frag};
	if (r == VK_SUCCESS)
		r = csm(device, &mi, NULL, &fs);
	st[1].module = fs;
	gp.layout = qcs.pl2;
	if (r == VK_SUCCESS)
		r = d->CreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, NULL, &qcs.pipe2);
	if (vs)
		dsm(device, vs, NULL);
	if (fs)
		dsm(device, fs, NULL);
	VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256};
	VkDescriptorPoolCreateInfo pc = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL,
	                                 VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, 256, 1, &ps};
	if (r == VK_SUCCESS)
		r = ((PFN_vkCreateDescriptorPool)d->gdpa(device, "vkCreateDescriptorPool"))(device, &pc, NULL, &qcs.pool);
	qcs.ok = r == VK_SUCCESS;
	LOG("dma-buf: NV12 frames copied as RGBA for vrcompositor (chroma %s, filter %s): %d\n",
	    cc.xChromaOffset == VK_CHROMA_LOCATION_COSITED_EVEN ? "co-sited" : "midpoint",
	    cc.chromaFilter == VK_FILTER_LINEAR ? "linear" : "nearest", r);
	return qcs.ok;
}

static int qcp_find(VkImage nv12)
{
	if (!nv12 || !atomic_load(&qcp_n))
		return -1;
	int found = -1;
	pthread_mutex_lock(&qcs.lock);
	for (unsigned i = 0; i < sizeof(qcp) / sizeof(qcp[0]) && found < 0; i++)
		if (qcp[i].nv12 == nv12)
			found = (int)i;
	pthread_mutex_unlock(&qcs.lock);
	return found;
}

//! Call with qcs.lock held: an RGBA8 image bound to the shared memory of its size (one per size: see above).
static VkResult qcp_image(struct device *d, VkDevice device, uint32_t w, uint32_t h, uint32_t levels, VkImage *img,
                          int *mem)
{
	VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ic.imageType = VK_IMAGE_TYPE_2D;
	ic.format = VK_FORMAT_R8G8B8A8_UNORM;
	ic.extent = (VkExtent3D){w, h, 1};
	ic.mipLevels = levels;
	ic.arrayLayers = 1;
	ic.samples = VK_SAMPLE_COUNT_1_BIT;
	ic.tiling = VK_IMAGE_TILING_OPTIMAL;
	ic.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	           VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	*mem = -1;
	VkResult r = d->CreateImage(device, &ic, NULL, img);
	VkMemoryRequirements req = {0};
	if (r == VK_SUCCESS)
		d->GetImageMemoryRequirements(device, *img, &req);
	uint32_t type = UINT32_MAX;
	for (uint32_t t = 0; t < d->memprops.memoryTypeCount && type == UINT32_MAX; t++)
		if ((req.memoryTypeBits & (1u << t)) &&
		    (d->memprops.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
			type = t;
	if (r == VK_SUCCESS && type == UINT32_MAX)
		r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
	const int nmem = (int)(sizeof(qcs.mem) / sizeof(qcs.mem[0]));
	int m = 0;
	while (r == VK_SUCCESS && m < nmem && !(qcs.mem[m].mem && qcs.mem[m].size == req.size && qcs.mem[m].type == type))
		m++;
	if (r == VK_SUCCESS && m == nmem) {
		m = 0;
		while (m < nmem && qcs.mem[m].mem)
			m++;
		VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type};
		r = m == nmem ? VK_ERROR_OUT_OF_DEVICE_MEMORY : d->AllocateMemory(device, &ai, NULL, &qcs.mem[m].mem);
		if (r == VK_SUCCESS) {
			qcs.mem[m].size = req.size;
			qcs.mem[m].type = type;
		}
	}
	if (r == VK_SUCCESS)
		r = d->BindImageMemory(device, *img, qcs.mem[m].mem, 0);
	if (r == VK_SUCCESS) {
		qcs.mem[m].users++;
		*mem = m;
	}
	return r;
}

//! Call with qcs.lock held: one user less of a shared memory; the memory to free when it was the last.
static VkDeviceMemory qcp_unref(int m)
{
	VkDeviceMemory unused = VK_NULL_HANDLE;
	if (m >= 0 && !--qcs.mem[m].users) {
		unused = qcs.mem[m].mem;
		qcs.mem[m].mem = VK_NULL_HANDLE;
	}
	return unused;
}

static void qcp_destroy(struct device *d, VkDevice device, const struct qnv_copy *c)
{
	PFN_vkFreeDescriptorSets fs = (PFN_vkFreeDescriptorSets)d->gdpa(device, "vkFreeDescriptorSets");
	PFN_vkDestroyFramebuffer df = (PFN_vkDestroyFramebuffer)d->gdpa(device, "vkDestroyFramebuffer");
	PFN_vkDestroyImageView dv = (PFN_vkDestroyImageView)d->gdpa(device, "vkDestroyImageView");
	PFN_vkDestroyImage di = (PFN_vkDestroyImage)d->gdpa(device, "vkDestroyImage");
	if (c->set)
		fs(device, qcs.pool, 1, &c->set);
	if (c->out_set)
		fs(device, qcs.pool, 1, &c->out_set);
	df(device, c->fb, NULL);
	df(device, c->out_fb, NULL);
	dv(device, c->dst, NULL);
	dv(device, c->src, NULL);
	dv(device, c->all, NULL);
	dv(device, c->out_view, NULL);
	di(device, c->img, NULL);
	di(device, c->out, NULL);
	pthread_mutex_lock(&qcs.lock);
	VkDeviceMemory m1 = qcp_unref(c->mem), m2 = qcp_unref(c->out_mem);
	pthread_mutex_unlock(&qcs.lock);
	if (m1)
		d->FreeMemory(device, m1, NULL);
	if (m2)
		d->FreeMemory(device, m2, NULL);
}

//! The RGBA copy of vrcompositor's NV12 image `n` and its output (made at its first view): its index, -1 if they
//! cannot be made. The output is 1.5 times the frame: vrcompositor's eye buffers are 1500x1664 here, over the field
//! of a 1024x1024 wide view (Steam Link VR's for a Frame), and the inset keeps that much of its detail.
static int qcp_get(struct device *d, VkDevice device, int n)
{
	int k = qcp_find(qnv[n].image);
	if (k >= 0)
		return k;
	pthread_mutex_lock(&qcs.lock);
	unsigned i = 0;
	while (i < sizeof(qcp) / sizeof(qcp[0]) && qcp[i].nv12)
		i++;
	if (i == sizeof(qcp) / sizeof(qcp[0]) || !qcs_init(d, device)) {
		pthread_mutex_unlock(&qcs.lock);
		return -1;
	}
	struct qnv_copy c = {.nv12 = qnv[n].image, .mem = -1, .out_mem = -1, .w = qnv[n].w, .h = qnv[n].h};
	c.ow = c.w * 3 / 2;
	c.oh = c.h * 3 / 2;
	VkResult r = qcp_image(d, device, c.w, c.h, QCP_LEVELS, &c.img, &c.mem);
	if (r == VK_SUCCESS)
		r = qcp_image(d, device, c.ow, c.oh, 1, &c.out, &c.out_mem);
	VkSamplerYcbcrConversionInfo cinfo = {VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO, NULL, qcs.conv};
	VkImageViewCreateInfo vc = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &cinfo, 0, c.nv12, VK_IMAGE_VIEW_TYPE_2D,
	                            VK_FORMAT_UNDEFINED, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	if (r == VK_SUCCESS)
		r = d->CreateImageView(device, &vc, NULL, &c.src);
	vc.pNext = NULL;
	vc.image = c.img;
	vc.format = VK_FORMAT_R8G8B8A8_UNORM;
	if (r == VK_SUCCESS)
		r = d->CreateImageView(device, &vc, NULL, &c.dst);
	vc.subresourceRange.levelCount = QCP_LEVELS;
	if (r == VK_SUCCESS)
		r = d->CreateImageView(device, &vc, NULL, &c.all);
	vc.image = c.out;
	vc.subresourceRange.levelCount = 1;
	if (r == VK_SUCCESS)
		r = d->CreateImageView(device, &vc, NULL, &c.out_view);
	PFN_vkCreateFramebuffer cf = (PFN_vkCreateFramebuffer)d->gdpa(device, "vkCreateFramebuffer");
	VkFramebufferCreateInfo fc = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, qcs.rp, 1, &c.dst, c.w, c.h, 1};
	if (r == VK_SUCCESS)
		r = cf(device, &fc, NULL, &c.fb);
	fc.pAttachments = &c.out_view;
	fc.width = c.ow;
	fc.height = c.oh;
	if (r == VK_SUCCESS)
		r = cf(device, &fc, NULL, &c.out_fb);
	PFN_vkAllocateDescriptorSets as = (PFN_vkAllocateDescriptorSets)d->gdpa(device, "vkAllocateDescriptorSets");
	VkDescriptorSetAllocateInfo sa = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, qcs.pool, 1, &qcs.dsl};
	if (r == VK_SUCCESS)
		r = as(device, &sa, &c.set);
	sa.pSetLayouts = &qcs.dsl2;
	if (r == VK_SUCCESS)
		r = as(device, &sa, &c.out_set);
	if (r == VK_SUCCESS) {
		VkDescriptorImageInfo ii[2] = {{VK_NULL_HANDLE, c.src, VK_IMAGE_LAYOUT_GENERAL},
		                               {VK_NULL_HANDLE, c.all, VK_IMAGE_LAYOUT_GENERAL}};
		VkWriteDescriptorSet w[2] = {
		    {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, c.set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii[0]},
		    {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, c.out_set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		     &ii[1]},
		};
		((PFN_vkUpdateDescriptorSets)d->gdpa(device, "vkUpdateDescriptorSets"))(device, 2, w, 0, NULL);
		qcp[i] = c;
		atomic_fetch_add(&qcp_n, 1);
	}
	pthread_mutex_unlock(&qcs.lock);
	if (r != VK_SUCCESS)
		qcp_destroy(d, device, &c);
	static atomic_int logs;
	if (atomic_fetch_add(&logs, 1) < 4 || r != VK_SUCCESS)
		LOG("dma-buf: RGBA copy %ux%u of NV12 image %p, output %ux%u: %d\n", c.w, c.h, (void *)c.nv12, c.ow, c.oh, r);
	return r == VK_SUCCESS ? (int)i : -1;
}

//! After vkDestroyImage of an NV12 image: its copy goes too (vrcompositor destroyed its views of it first).
static void qcp_forget(VkDevice device, VkImage nv12)
{
	int k = qcp_find(nv12);
	if (k < 0)
		return;
	pthread_mutex_lock(&qcs.lock);
	struct qnv_copy c = qcp[k];
	memset(&qcp[k], 0, sizeof(qcp[k]));
	atomic_fetch_sub(&qcp_n, 1);
	pthread_mutex_unlock(&qcs.lock);
	qcp_destroy(device_of(device), device, &c);
}

//! Record the copy of NV12 image k's frame into its RGBA image (outside a render pass).
static void qcp_record(VkCommandBuffer cb, int k)
{
	struct device *d = device_of(cb);
	static PFN_vkCmdBeginRenderPass begin;
	static PFN_vkCmdEndRenderPass end;
	static PFN_vkCmdBindPipeline bind;
	static PFN_vkCmdBindDescriptorSets sets;
	static PFN_vkCmdSetViewport viewport;
	static PFN_vkCmdSetScissor scissor;
	static PFN_vkCmdDraw draw;
	static PFN_vkCmdBlitImage blit;
	static PFN_vkCmdPipelineBarrier barrier;
	static PFN_vkCmdPushConstants push;
	if (!draw) {
		VkDevice dev = d->handle;
		begin = (PFN_vkCmdBeginRenderPass)d->gdpa(dev, "vkCmdBeginRenderPass");
		end = (PFN_vkCmdEndRenderPass)d->gdpa(dev, "vkCmdEndRenderPass");
		bind = (PFN_vkCmdBindPipeline)d->gdpa(dev, "vkCmdBindPipeline");
		sets = (PFN_vkCmdBindDescriptorSets)d->gdpa(dev, "vkCmdBindDescriptorSets");
		viewport = (PFN_vkCmdSetViewport)d->gdpa(dev, "vkCmdSetViewport");
		scissor = (PFN_vkCmdSetScissor)d->gdpa(dev, "vkCmdSetScissor");
		blit = (PFN_vkCmdBlitImage)d->gdpa(dev, "vkCmdBlitImage");
		barrier = (PFN_vkCmdPipelineBarrier)d->gdpa(dev, "vkCmdPipelineBarrier");
		push = (PFN_vkCmdPushConstants)d->gdpa(dev, "vkCmdPushConstants");
		draw = (PFN_vkCmdDraw)d->gdpa(dev, "vkCmdDraw");
	}
	pthread_mutex_lock(&qcs.lock);
	struct qnv_copy c = qcp[k];
	pthread_mutex_unlock(&qcs.lock);
	if (!c.fb)
		return;
	VkRect2D area = {{0, 0}, {c.w, c.h}};
	VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, qcs.rp, c.fb, area, 0, NULL};
	begin(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
	bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, qcs.pipe);
	VkViewport v = {0, 0, (float)c.w, (float)c.h, 0, 1};
	viewport(cb, 0, 1, &v);
	scissor(cb, 0, 1, &area);
	sets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, qcs.pl, 0, 1, &c.set, 0, NULL);
	draw(cb, 3, 1, 0, 0);
	end(cb);
	// The smaller levels: Steam Link's foveated insets (a narrow field at 1024x1024) come out ~1.8 times smaller in
	// the output; read at one level, their detail would alias.
	VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	                          VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
	                          VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, c.img,
	                          {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	VkImageMemoryBarrier bl[2] = {b, b};
	bl[1].srcAccessMask = 0;
	bl[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	bl[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	bl[1].subresourceRange.baseMipLevel = 1;
	bl[1].subresourceRange.levelCount = QCP_LEVELS - 1;
	barrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, bl);
	for (uint32_t l = 1; l < QCP_LEVELS; l++) {
		int32_t w = (int32_t)(c.w >> (l - 1)), h = (int32_t)(c.h >> (l - 1));
		VkImageBlit r = {{VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 0, 1}, {{0, 0, 0}, {w, h, 1}},
		                 {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1}, {{0, 0, 0}, {w / 2, h / 2, 1}}};
		blit(cb, c.img, VK_IMAGE_LAYOUT_GENERAL, c.img, VK_IMAGE_LAYOUT_GENERAL, 1, &r, VK_FILTER_LINEAR);
		b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		b.subresourceRange.baseMipLevel = l;
		if (l + 1 < QCP_LEVELS)
			barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
	}
	// before vrcompositor reads it: level 0 by the copy pass, the others by the blits
	b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
	b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
	b.subresourceRange.baseMipLevel = 0;
	b.subresourceRange.levelCount = QCP_LEVELS;
	barrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
	        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
	// What vrcompositor reads: each eye's half of the atlas, its inset blended into its wide view (nv12_inset.frag)
	struct q1_views views;
	bool insets = qcp_views(&views);
	VkRect2D out = {{0, 0}, {c.ow, c.oh}};
	VkRenderPassBeginInfo bo = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, qcs.rp, c.out_fb, out, 0, NULL};
	begin(cb, &bo, VK_SUBPASS_CONTENTS_INLINE);
	bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, qcs.pipe2);
	VkViewport vo = {0, 0, (float)c.ow, (float)c.oh, 0, 1};
	viewport(cb, 0, 1, &vo);
	sets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, qcs.pl2, 0, 1, &c.out_set, 0, NULL);
	for (int e = 0; e < 2; e++) {
		struct qcp_inset pc = {{0}};
		VkRect2D half = {{0, (int32_t)(e * c.oh / 2)}, {c.ow, c.oh / 2}};
		if (insets) {
			const float *w = views.wide[e], *r = views.inset[e];
			memcpy(pc.place, r, sizeof(pc.place));
			// the inset is the view above its wide view, as large
			float iv[4] = {w[0], w[1] - (w[3] - w[1]), w[2], w[1]};
			memcpy(pc.view, iv, sizeof(pc.view));
			// its texels per output pixel: the level that fits
			float sx = (iv[2] - iv[0]) * c.w / ((r[2] - r[0]) * c.ow), sy = (iv[3] - iv[1]) * c.h / ((r[3] - r[1]) * c.oh);
			pc.lod = qcp_log2(sx > sy ? sx : sy);
			// vrcompositor reads the wide view only (and a row around it, filtering)
			int32_t y0 = (int32_t)(w[1] * c.oh) - 2, y1 = (int32_t)(w[3] * c.oh + 0.5f) + 2;
			half.offset.y = y0 < 0 ? 0 : y0;
			half.extent.height = (uint32_t)((y1 > (int32_t)c.oh ? (int32_t)c.oh : y1) - half.offset.y);
		}
		scissor(cb, 0, 1, &half);
		push(cb, qcs.pl2, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
		draw(cb, 3, 1, 0, 0);
	}
	end(cb);
	atomic_store(&qs_stream_ns, now_ns()); // (quest1_scanout.c: vrcompositor's distortion pass shows it)
}

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
		c.format = VK_FORMAT_R8G8B8A8_UNORM;
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
