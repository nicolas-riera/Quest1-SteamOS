// quest1_wsi_x11: VK_KHR_xcb_surface / VK_KHR_xlib_surface + VK_KHR_swapchain for X11 windows on top
// of a driver without X11 WSI (the Adreno Android blob via libhybris). Built into quest1_vkshim.
#pragma once
#include <vulkan/vulkan.h>

// enabled by QUEST1_VKSHIM_WSI=1
int wsi_enabled(void);
// the next element of the chain (compat layer or hybris libvulkan)
void wsi_set_down(PFN_vkGetInstanceProcAddr gipa);
void wsi_instance_created(VkInstance instance);
void wsi_device_created(VkInstance instance, VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, VkDevice device,
                        PFN_vkGetDeviceProcAddr gdpa);
// the instance extensions this adds (VK_KHR_xcb_surface, VK_KHR_xlib_surface)
extern const char *const wsi_instance_exts[];
extern const unsigned wsi_instance_ext_count;
// our entry point for an instance- or device-level command, or NULL
PFN_vkVoidFunction wsi_proc(const char *name);
