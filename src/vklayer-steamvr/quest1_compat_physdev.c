// quest1_compat_physdev: physical device queries (extensions, features, properties) and the debug utils stubs.
// Part of quest1_compat.c (included by it, in this order: one translation unit).

// --- physical device queries ------------------------------------------------------------------------

static VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(VkPhysicalDevice pd, const char *layer,
                                                                         uint32_t *count, VkExtensionProperties *props)
{
	if (layer && strcmp(layer, LAYER_NAME) == 0) {
		*count = 0;
		return VK_SUCCESS;
	}
	struct instance *in = instance_of(pd);
	uint32_t n = 0;
	VkResult r = in->EnumerateDeviceExtensionProperties(pd, layer, &n, NULL);
	if (r != VK_SUCCESS || layer != NULL)
		return in->EnumerateDeviceExtensionProperties(pd, layer, count, props);
	VkExtensionProperties *all = calloc(n + 16, sizeof(*all));
	in->EnumerateDeviceExtensionProperties(pd, NULL, &n, all);
	pthread_once(&fake_once, init_fake);
	for (uint32_t f = 0; f < fake_count; f++) {
		bool present = false;
		for (uint32_t i = 0; i < n; i++)
			present |= strcmp(all[i].extensionName, fake_names[f]) == 0;
		if (!present) {
			snprintf(all[n].extensionName, VK_MAX_EXTENSION_NAME_SIZE, "%s", fake_names[f]);
			all[n++].specVersion = fake_spec[f];
		}
	}
	if (!props) {
		*count = n;
		free(all);
		return VK_SUCCESS;
	}
	uint32_t c = *count < n ? *count : n;
	memcpy(props, all, c * sizeof(*all));
	*count = c;
	free(all);
	return c < n ? VK_INCOMPLETE : VK_SUCCESS;
}

static void fill_fake_features(VkPhysicalDeviceFeatures2 *f)
{
	for (VkBaseOutStructure *s = (VkBaseOutStructure *)f->pNext; s; s = s->pNext) {
		if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES)
			((VkPhysicalDeviceTimelineSemaphoreFeatures *)s)->timelineSemaphore = VK_TRUE;
		else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
			VkPhysicalDeviceVulkan12Features *v = (void *)s;
			v->timelineSemaphore = VK_TRUE;
			v->hostQueryReset = fx_enabled();
		} else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES)
			((VkPhysicalDeviceVulkan13Features *)s)->dynamicRendering = fx_enabled();
		else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES)
			((VkPhysicalDeviceHostQueryResetFeatures *)s)->hostQueryReset = fx_enabled();
		else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES)
			((VkPhysicalDeviceDynamicRenderingFeatures *)s)->dynamicRendering = fx_enabled();
		else if (s->sType == 1000201000)
			memset((char *)s + sizeof(VkBaseOutStructure), 0, sizeof(VkBool32) * 2);
		else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR)
			((VkPhysicalDevicePresentIdFeaturesKHR *)s)->presentId = VK_TRUE;
		else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR)
			((VkPhysicalDevicePresentWaitFeaturesKHR *)s)->presentWait = VK_TRUE;
		else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT) {
			VkPhysicalDeviceExtendedDynamicState3FeaturesEXT *e = (void *)s;
			void *next = e->pNext;
			memset(e, 0, sizeof(*e));
			e->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT;
			e->pNext = next;
		}
	}
}

// Structures the blob (or the Android loader) does not know: loader chain links, the debug messenger of
// the stripped VK_EXT_debug_utils, and the feature/property structures of the advertised-only extensions.
static bool is_unknown_struct(VkStructureType t)
{
	switch (t) {
	case VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO:
	case VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO:
	case VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT:
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES:
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES:
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT:
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_PROPERTIES_EXT:
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR:
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES:
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES:
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES:
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES:
	case 1000201000: // PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_NV
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR:
	case VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT:
	case VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT:
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT:
	case VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT:
	case VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT: return true;
	default: return false;
	}
}

// Unlink the structures the blob does not know from an output chain, call down, relink.
#define WITH_UNLINKED(head_ptr, call)                                                                                  \
	do {                                                                                                           \
		VkBaseOutStructure *saved[16];                                                                         \
		VkBaseOutStructure *prevs[16];                                                                         \
		int ns = 0;                                                                                            \
		VkBaseOutStructure *prev = (VkBaseOutStructure *)(head_ptr);                                           \
		for (VkBaseOutStructure *s = prev->pNext; s && ns < 16;) {                                             \
			VkBaseOutStructure *next = s->pNext;                                                           \
			if (is_unknown_struct(s->sType)) {                                                             \
				saved[ns] = s;                                                                         \
				prevs[ns++] = prev;                                                                    \
				prev->pNext = next;                                                                    \
			} else {                                                                                       \
				prev = s;                                                                              \
			}                                                                                              \
			s = next;                                                                                      \
		}                                                                                                      \
		call;                                                                                                  \
		for (int i = ns - 1; i >= 0; i--) {                                                                    \
			saved[i]->pNext = prevs[i]->pNext;                                                             \
			prevs[i]->pNext = saved[i];                                                                    \
		}                                                                                                      \
	} while (0)

static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f)
{
	struct instance *in = instance_of(pd);
	WITH_UNLINKED(f, in->GetPhysicalDeviceFeatures2(pd, f));
	fill_fake_features(f);
}

//! QUEST1_FAKE_CPU_DEVICE=1: report the GPU as a CPU device. Mesa runs zink on X servers without
//! DRI3 (Xvfb) only through drisw, and there it only picks CPU devices (lavapipe); the rendering
//! still happens on the Adreno.
static void fake_device_type(VkPhysicalDeviceProperties *p)
{
	static int fake = -1;
	if (fake < 0)
		fake = getenv("QUEST1_FAKE_CPU_DEVICE") && atoi(getenv("QUEST1_FAKE_CPU_DEVICE")) == 1;
	if (fake)
		p->deviceType = VK_PHYSICAL_DEVICE_TYPE_CPU;
}

static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties(VkPhysicalDevice pd, VkPhysicalDeviceProperties *p)
{
	instance_of(pd)->GetPhysicalDeviceProperties(pd, p);
	fake_device_type(p);
}

static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p)
{
	struct instance *in = instance_of(pd);
	WITH_UNLINKED(p, in->GetPhysicalDeviceProperties2(pd, p));
	fake_device_type(&p->properties);
	for (VkBaseOutStructure *s = (VkBaseOutStructure *)p->pNext; s; s = s->pNext)
		if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES)
			((VkPhysicalDeviceTimelineSemaphoreProperties *)s)->maxTimelineSemaphoreValueDifference = UINT64_MAX;
		else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES)
			((VkPhysicalDeviceVulkan12Properties *)s)->maxTimelineSemaphoreValueDifference = UINT64_MAX;
}

static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceExternalSemaphoreProperties(
    VkPhysicalDevice pd, const VkPhysicalDeviceExternalSemaphoreInfo *info, VkExternalSemaphoreProperties *out)
{
	const VkSemaphoreTypeCreateInfo *type = find_struct(info->pNext, VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO);
	if ((type && type->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE) ||
	    info->handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT) {
		// emulated: timeline semaphores share their counter as an OPAQUE_FD (a memfd)
		bool ok = info->handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT && type &&
		          type->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE;
		out->exportFromImportedHandleTypes = ok ? VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT : 0;
		out->compatibleHandleTypes = out->exportFromImportedHandleTypes;
		out->externalSemaphoreFeatures =
		    ok ? VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT : 0;
		return;
	}
	struct instance *in = instance_of(pd);
	VkPhysicalDeviceExternalSemaphoreInfo plain = *info;
	plain.pNext = NULL;
	in->GetPhysicalDeviceExternalSemaphoreProperties(pd, &plain, out);
}

// --- VK_EXT_debug_utils stubs (the extension is stripped at instance creation) ---------------------------

static VKAPI_ATTR VkResult VKAPI_CALL CreateDebugUtilsMessenger(VkInstance instance,
                                                                const VkDebugUtilsMessengerCreateInfoEXT *ci,
                                                                const VkAllocationCallbacks *alloc,
                                                                VkDebugUtilsMessengerEXT *out)
{
	*out = (VkDebugUtilsMessengerEXT)(uintptr_t)1;
	return VK_SUCCESS;
}

// every other debug_utils command returns void or VkResult: VK_SUCCESS is 0
static VKAPI_ATTR VkResult VKAPI_CALL debug_utils_noop(void) { return VK_SUCCESS; }

static PFN_vkVoidFunction debug_utils_stub(const char *name)
{
	if (!strstr(name, "DebugUtils"))
		return NULL;
	if (strcmp(name, "vkCreateDebugUtilsMessengerEXT") == 0)
		return (PFN_vkVoidFunction)CreateDebugUtilsMessenger;
	return (PFN_vkVoidFunction)debug_utils_noop;
}
