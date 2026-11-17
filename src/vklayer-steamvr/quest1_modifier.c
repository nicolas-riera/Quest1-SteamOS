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

// The rest, in this order (one translation unit with quest1_compat.c):
#include "quest1_modifier_extfmt.c"
#include "quest1_modifier_export.c"
#include "quest1_modifier_copy.c"
#include "quest1_modifier_record.c"
#include "quest1_modifier_pose.c"
#include "quest1_modifier_hooks.c"
