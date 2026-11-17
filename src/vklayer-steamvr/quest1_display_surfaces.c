// quest1_display_surfaces: display surfaces.
// Part of quest1_display.c (included by it, in this order: one translation unit).

// --- surfaces ----------------------------------------------------------------------------------------------

#define QD_MAX_SURFACES 4
static void *qd_surfaces[QD_MAX_SURFACES];

static bool qd_is_surface(VkSurfaceKHR s)
{
	pthread_mutex_lock(&g_lock);
	bool r = false;
	for (int i = 0; i < QD_MAX_SURFACES; i++)
		r |= s != VK_NULL_HANDLE && qd_surfaces[i] == (void *)(uintptr_t)s;
	pthread_mutex_unlock(&g_lock);
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_CreateDisplayPlaneSurfaceKHR(VkInstance instance,
                                                                    const VkDisplaySurfaceCreateInfoKHR *ci,
                                                                    const VkAllocationCallbacks *alloc,
                                                                    VkSurfaceKHR *surface)
{
	void *s = malloc(16);
	pthread_mutex_lock(&g_lock);
	int slot = -1;
	for (int i = 0; i < QD_MAX_SURFACES && slot < 0; i++)
		if (!qd_surfaces[i])
			slot = i;
	if (slot >= 0)
		qd_surfaces[slot] = s;
	pthread_mutex_unlock(&g_lock);
	if (slot < 0) {
		free(s);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	*surface = (VkSurfaceKHR)(uintptr_t)s;
	LOG("display surface created (%ux%u @ %.1f Hz)\n", qd_width, qd_height, qd_mhz / 1000.0);
	return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL qd_DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                                                     const VkAllocationCallbacks *alloc)
{
	if (qd_is_surface(surface)) {
		pthread_mutex_lock(&g_lock);
		for (int i = 0; i < QD_MAX_SURFACES; i++)
			if (qd_surfaces[i] == (void *)(uintptr_t)surface)
				qd_surfaces[i] = NULL;
		pthread_mutex_unlock(&g_lock);
		free((void *)(uintptr_t)surface);
		return;
	}
	struct instance *in = instance_of(instance);
	PFN_vkDestroySurfaceKHR next = (PFN_vkDestroySurfaceKHR)in->gipa(instance, "vkDestroySurfaceKHR");
	if (next)
		next(instance, surface, alloc);
}

#define NEXT_INSTANCE_FN(pd, type, name) ((type)instance_of(pd)->gipa(instance_of(pd)->handle, name))

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice pd, uint32_t family,
                                                                          VkSurfaceKHR surface, VkBool32 *supported)
{
	if (qd_is_surface(surface)) {
		*supported = VK_TRUE;
		return VK_SUCCESS;
	}
	return NEXT_INSTANCE_FN(pd, PFN_vkGetPhysicalDeviceSurfaceSupportKHR, "vkGetPhysicalDeviceSurfaceSupportKHR")(
	    pd, family, surface, supported);
}

static void qd_fill_caps(VkSurfaceCapabilitiesKHR *c)
{
	pthread_once(&qd_once, qd_init);
	memset(c, 0, sizeof(*c));
	c->minImageCount = 3;
	c->maxImageCount = QD_MAX_IMAGES;
	c->currentExtent = c->minImageExtent = c->maxImageExtent = (VkExtent2D){qd_width, qd_height};
	c->maxImageArrayLayers = 1;
	c->supportedTransforms = c->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	c->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	c->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice pd,
                                                                               VkSurfaceKHR surface,
                                                                               VkSurfaceCapabilitiesKHR *caps)
{
	if (qd_is_surface(surface)) {
		qd_fill_caps(caps);
		return VK_SUCCESS;
	}
	return NEXT_INSTANCE_FN(pd, PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR,
	                        "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")(pd, surface, caps);
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetPhysicalDeviceSurfaceCapabilities2KHR(
    VkPhysicalDevice pd, const VkPhysicalDeviceSurfaceInfo2KHR *info, VkSurfaceCapabilities2KHR *caps)
{
	if (qd_is_surface(info->surface)) {
		qd_fill_caps(&caps->surfaceCapabilities);
		return VK_SUCCESS;
	}
	return NEXT_INSTANCE_FN(pd, PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR,
	                        "vkGetPhysicalDeviceSurfaceCapabilities2KHR")(pd, info, caps);
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetPhysicalDeviceSurfaceCapabilities2EXT(VkPhysicalDevice pd,
                                                                                VkSurfaceKHR surface,
                                                                                VkSurfaceCapabilities2EXT *caps)
{
	if (!qd_is_surface(surface))
		return VK_ERROR_SURFACE_LOST_KHR;
	VkSurfaceCapabilitiesKHR c;
	qd_fill_caps(&c);
	caps->minImageCount = c.minImageCount;
	caps->maxImageCount = c.maxImageCount;
	caps->currentExtent = c.currentExtent;
	caps->minImageExtent = c.minImageExtent;
	caps->maxImageExtent = c.maxImageExtent;
	caps->maxImageArrayLayers = c.maxImageArrayLayers;
	caps->supportedTransforms = c.supportedTransforms;
	caps->currentTransform = c.currentTransform;
	caps->supportedCompositeAlpha = c.supportedCompositeAlpha;
	caps->supportedUsageFlags = c.supportedUsageFlags;
	caps->supportedSurfaceCounters = VK_SURFACE_COUNTER_VBLANK_BIT_EXT;
	return VK_SUCCESS;
}

static const VkSurfaceFormatKHR qd_formats[] = {
    {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
};
#define QD_FORMAT_COUNT (sizeof(qd_formats) / sizeof(qd_formats[0]))

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice pd, VkSurfaceKHR surface,
                                                                          uint32_t *count, VkSurfaceFormatKHR *formats)
{
	if (!qd_is_surface(surface))
		return NEXT_INSTANCE_FN(pd, PFN_vkGetPhysicalDeviceSurfaceFormatsKHR,
		                        "vkGetPhysicalDeviceSurfaceFormatsKHR")(pd, surface, count, formats);
	if (!formats) {
		*count = QD_FORMAT_COUNT;
		return VK_SUCCESS;
	}
	uint32_t n = *count < QD_FORMAT_COUNT ? *count : QD_FORMAT_COUNT;
	memcpy(formats, qd_formats, n * sizeof(*formats));
	*count = n;
	return n < QD_FORMAT_COUNT ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetPhysicalDeviceSurfaceFormats2KHR(
    VkPhysicalDevice pd, const VkPhysicalDeviceSurfaceInfo2KHR *info, uint32_t *count, VkSurfaceFormat2KHR *formats)
{
	if (!qd_is_surface(info->surface))
		return NEXT_INSTANCE_FN(pd, PFN_vkGetPhysicalDeviceSurfaceFormats2KHR,
		                        "vkGetPhysicalDeviceSurfaceFormats2KHR")(pd, info, count, formats);
	if (!formats) {
		*count = QD_FORMAT_COUNT;
		return VK_SUCCESS;
	}
	uint32_t n = *count < QD_FORMAT_COUNT ? *count : QD_FORMAT_COUNT;
	for (uint32_t i = 0; i < n; i++)
		formats[i].surfaceFormat = qd_formats[i];
	*count = n;
	return n < QD_FORMAT_COUNT ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice pd,
                                                                               VkSurfaceKHR surface, uint32_t *count,
                                                                               VkPresentModeKHR *modes)
{
	static const VkPresentModeKHR all[] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR,
	                                       VK_PRESENT_MODE_IMMEDIATE_KHR};
	if (!qd_is_surface(surface))
		return NEXT_INSTANCE_FN(pd, PFN_vkGetPhysicalDeviceSurfacePresentModesKHR,
		                        "vkGetPhysicalDeviceSurfacePresentModesKHR")(pd, surface, count, modes);
	if (!modes) {
		*count = 3;
		return VK_SUCCESS;
	}
	uint32_t n = *count < 3 ? *count : 3;
	memcpy(modes, all, n * sizeof(*modes));
	*count = n;
	return n < 3 ? VK_INCOMPLETE : VK_SUCCESS;
}
