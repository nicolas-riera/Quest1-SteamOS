// quest1_display_dispatch: the display entry points the layer hands out.
// Part of quest1_display.c (included by it, in this order: one translation unit).

// --- dispatch ----------------------------------------------------------------------------------------------

static PFN_vkVoidFunction qd_proc(const char *name)
{
	if (getenv("QUEST1_NO_DISPLAY"))
		return NULL;
#define Q(n) if (strcmp(name, "vk" #n) == 0) return (PFN_vkVoidFunction)qd_##n;
	// VK_KHR_display / display_properties2 / EXT_direct_mode_display / EXT_acquire_*_display
	Q(GetPhysicalDeviceDisplayPropertiesKHR)
	Q(GetPhysicalDeviceDisplayProperties2KHR)
	Q(GetPhysicalDeviceDisplayPlanePropertiesKHR)
	Q(GetPhysicalDeviceDisplayPlaneProperties2KHR)
	Q(GetDisplayPlaneSupportedDisplaysKHR)
	Q(GetDisplayModePropertiesKHR)
	Q(GetDisplayModeProperties2KHR)
	Q(CreateDisplayModeKHR)
	Q(GetDisplayPlaneCapabilitiesKHR)
	Q(GetDisplayPlaneCapabilities2KHR)
	Q(CreateDisplayPlaneSurfaceKHR)
	Q(GetRandROutputDisplayEXT)
	// surfaces and swapchains (pass through for the ones that are not ours)
	Q(DestroySurfaceKHR)
	Q(GetPhysicalDeviceSurfaceSupportKHR)
	Q(GetPhysicalDeviceSurfaceCapabilitiesKHR)
	Q(GetPhysicalDeviceSurfaceCapabilities2KHR)
	Q(GetPhysicalDeviceSurfaceCapabilities2EXT)
	Q(GetPhysicalDeviceSurfaceFormatsKHR)
	Q(GetPhysicalDeviceSurfaceFormats2KHR)
	Q(GetPhysicalDeviceSurfacePresentModesKHR)
	Q(CreateSwapchainKHR)
	Q(DestroySwapchainKHR)
	Q(GetSwapchainImagesKHR)
	Q(AcquireNextImageKHR)
	Q(AcquireNextImage2KHR)
	Q(QueuePresentKHR)
	Q(RegisterDisplayEventEXT)
	Q(GetSwapchainCounterEXT)
#undef Q
#define S(n, fn) if (strcmp(name, n) == 0) return (PFN_vkVoidFunction)fn;
	S("vkAcquireXlibDisplayEXT", qd_succeed)
	S("vkAcquireDrmDisplayEXT", qd_succeed)
	S("vkReleaseDisplayEXT", qd_succeed)
	S("vkDisplayPowerControlEXT", qd_succeed)
	S("vkWaitForPresentKHR", qd_WaitForPresentKHR)
	S("vkGetDrmDisplayEXT", qd_unsupported)
	S("vkRegisterDeviceEventEXT", qd_unsupported)
#undef S
	if (qs_enabled())
		return qs_trace_proc(name);
	return NULL;
}
