// quest1_display_queries: the simulated display's queries (displays, modes, planes).
// Part of quest1_display.c (included by it, in this order: one translation unit).

// --- display queries -------------------------------------------------------------------------------------

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetPhysicalDeviceDisplayPropertiesKHR(VkPhysicalDevice pd, uint32_t *count,
                                                                             VkDisplayPropertiesKHR *props)
{
	pthread_once(&qd_once, qd_init);
	if (!props) {
		*count = 1;
		return VK_SUCCESS;
	}
	if (*count < 1)
		return VK_INCOMPLETE;
	*count = 1;
	memset(props, 0, sizeof(*props));
	props->display = QD_DISPLAY;
	props->displayName = QD_NAME;
	props->physicalDimensions = (VkExtent2D){121, 68}; // mm, informative
	props->physicalResolution = (VkExtent2D){qd_width, qd_height};
	props->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetPhysicalDeviceDisplayProperties2KHR(VkPhysicalDevice pd, uint32_t *count,
                                                                              VkDisplayProperties2KHR *props)
{
	if (!props)
		return qd_GetPhysicalDeviceDisplayPropertiesKHR(pd, count, NULL);
	return qd_GetPhysicalDeviceDisplayPropertiesKHR(pd, count, &props->displayProperties);
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetPhysicalDeviceDisplayPlanePropertiesKHR(VkPhysicalDevice pd,
                                                                                  uint32_t *count,
                                                                                  VkDisplayPlanePropertiesKHR *props)
{
	if (!props) {
		*count = 1;
		return VK_SUCCESS;
	}
	if (*count < 1)
		return VK_INCOMPLETE;
	*count = 1;
	props->currentDisplay = QD_DISPLAY;
	props->currentStackIndex = 0;
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetPhysicalDeviceDisplayPlaneProperties2KHR(
    VkPhysicalDevice pd, uint32_t *count, VkDisplayPlaneProperties2KHR *props)
{
	if (!props)
		return qd_GetPhysicalDeviceDisplayPlanePropertiesKHR(pd, count, NULL);
	return qd_GetPhysicalDeviceDisplayPlanePropertiesKHR(pd, count, &props->displayPlaneProperties);
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetDisplayPlaneSupportedDisplaysKHR(VkPhysicalDevice pd, uint32_t plane,
                                                                           uint32_t *count, VkDisplayKHR *displays)
{
	if (plane != 0 || !displays) {
		*count = plane == 0;
		return VK_SUCCESS;
	}
	if (*count < 1)
		return VK_INCOMPLETE;
	*count = 1;
	displays[0] = QD_DISPLAY;
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetDisplayModePropertiesKHR(VkPhysicalDevice pd, VkDisplayKHR display,
                                                                   uint32_t *count, VkDisplayModePropertiesKHR *props)
{
	pthread_once(&qd_once, qd_init);
	if (display != QD_DISPLAY) {
		*count = 0;
		return VK_SUCCESS;
	}
	if (!props) {
		*count = 1;
		return VK_SUCCESS;
	}
	if (*count < 1)
		return VK_INCOMPLETE;
	*count = 1;
	props->displayMode = QD_MODE;
	props->parameters.visibleRegion = (VkExtent2D){qd_width, qd_height};
	props->parameters.refreshRate = qd_mhz;
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetDisplayModeProperties2KHR(VkPhysicalDevice pd, VkDisplayKHR display,
                                                                    uint32_t *count, VkDisplayModeProperties2KHR *props)
{
	if (!props)
		return qd_GetDisplayModePropertiesKHR(pd, display, count, NULL);
	return qd_GetDisplayModePropertiesKHR(pd, display, count, &props->displayModeProperties);
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_CreateDisplayModeKHR(VkPhysicalDevice pd, VkDisplayKHR display,
                                                            const VkDisplayModeCreateInfoKHR *ci,
                                                            const VkAllocationCallbacks *alloc, VkDisplayModeKHR *mode)
{
	*mode = QD_MODE;
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetDisplayPlaneCapabilitiesKHR(VkPhysicalDevice pd, VkDisplayModeKHR mode,
                                                                      uint32_t plane,
                                                                      VkDisplayPlaneCapabilitiesKHR *caps)
{
	pthread_once(&qd_once, qd_init);
	memset(caps, 0, sizeof(*caps));
	caps->supportedAlpha = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
	caps->minSrcExtent = caps->maxSrcExtent = caps->minDstExtent = caps->maxDstExtent =
	    (VkExtent2D){qd_width, qd_height};
	caps->maxSrcPosition = caps->maxDstPosition = (VkOffset2D){0, 0};
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetDisplayPlaneCapabilities2KHR(VkPhysicalDevice pd,
                                                                       const VkDisplayPlaneInfo2KHR *info,
                                                                       VkDisplayPlaneCapabilities2KHR *caps)
{
	return qd_GetDisplayPlaneCapabilitiesKHR(pd, info->mode, info->planeIndex, &caps->capabilities);
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_succeed(void) { return VK_SUCCESS; }

static VKAPI_ATTR VkResult VKAPI_CALL qd_unsupported(void) { return VK_ERROR_INITIALIZATION_FAILED; }

// RandR has no output for us: vrcompositor then enumerates the Vulkan displays
static VKAPI_ATTR VkResult VKAPI_CALL qd_GetRandROutputDisplayEXT(VkPhysicalDevice pd, void *dpy,
                                                                unsigned long output, VkDisplayKHR *display)
{
	*display = VK_NULL_HANDLE;
	return VK_SUCCESS;
}
