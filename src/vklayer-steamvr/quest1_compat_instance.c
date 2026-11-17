// quest1_compat_instance: instance creation and the layer's entry points.
// Part of quest1_compat.c (included by it, in this order: one translation unit).

// --- instance ------------------------------------------------------------------------------------------

static PFN_vkVoidFunction instance_intercept(const char *name);

static VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *alloc,
                                                     VkInstance *out)
{
	VkLayerInstanceCreateInfo *chain = (VkLayerInstanceCreateInfo *)ci->pNext;
	while (chain &&
	       !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO))
		chain = (VkLayerInstanceCreateInfo *)chain->pNext;
	if (!chain)
		return VK_ERROR_INITIALIZATION_FAILED;
	PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
	PFN_vkCreateInstance next_create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");

	// The Android loader lists VK_EXT_debug_utils but fails vkCreateInstance with it
	// (VK_ERROR_EXTENSION_NOT_PRESENT): drop it, its entry points become no-ops (debug_utils_stub)
	const char **exts = calloc(ci->enabledExtensionCount + 1, sizeof(char *));
	uint32_t ne = 0;
	for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
		if (strcmp(ci->ppEnabledExtensionNames[i], VK_EXT_DEBUG_UTILS_EXTENSION_NAME) != 0)
			exts[ne++] = ci->ppEnabledExtensionNames[i];
	VkInstanceCreateInfo copy = *ci;
	copy.enabledExtensionCount = ne;
	copy.ppEnabledExtensionNames = exts;
	VkResult r;
	WITH_UNLINKED(&copy, r = next_create(&copy, alloc, out));
	free(exts);
	if (r != VK_SUCCESS) {
		LOG("vkCreateInstance failed: %d\n", r);
		return r;
	}

	struct instance *in = calloc(1, sizeof(*in));
	in->key = key_of(*out);
	in->handle = *out;
	in->gipa = gipa;
	in->DestroyInstance = (PFN_vkDestroyInstance)gipa(*out, "vkDestroyInstance");
	in->EnumerateDeviceExtensionProperties =
	    (PFN_vkEnumerateDeviceExtensionProperties)gipa(*out, "vkEnumerateDeviceExtensionProperties");
	in->GetPhysicalDeviceFeatures2 = (PFN_vkGetPhysicalDeviceFeatures2)gipa(*out, "vkGetPhysicalDeviceFeatures2");
	if (!in->GetPhysicalDeviceFeatures2)
		in->GetPhysicalDeviceFeatures2 =
		    (PFN_vkGetPhysicalDeviceFeatures2)gipa(*out, "vkGetPhysicalDeviceFeatures2KHR");
	in->GetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)gipa(*out, "vkGetPhysicalDeviceProperties");
	in->GetPhysicalDeviceProperties2 =
	    (PFN_vkGetPhysicalDeviceProperties2)gipa(*out, "vkGetPhysicalDeviceProperties2");
	if (!in->GetPhysicalDeviceProperties2)
		in->GetPhysicalDeviceProperties2 =
		    (PFN_vkGetPhysicalDeviceProperties2)gipa(*out, "vkGetPhysicalDeviceProperties2KHR");
	in->GetPhysicalDeviceExternalSemaphoreProperties = (PFN_vkGetPhysicalDeviceExternalSemaphoreProperties)gipa(
	    *out, "vkGetPhysicalDeviceExternalSemaphoreProperties");
	if (!in->GetPhysicalDeviceExternalSemaphoreProperties)
		in->GetPhysicalDeviceExternalSemaphoreProperties = (PFN_vkGetPhysicalDeviceExternalSemaphoreProperties)gipa(
		    *out, "vkGetPhysicalDeviceExternalSemaphorePropertiesKHR");
	in->GetPhysicalDeviceFormatProperties2 =
	    (PFN_vkGetPhysicalDeviceFormatProperties2)gipa(*out, "vkGetPhysicalDeviceFormatProperties2");
	if (!in->GetPhysicalDeviceFormatProperties2)
		in->GetPhysicalDeviceFormatProperties2 =
		    (PFN_vkGetPhysicalDeviceFormatProperties2)gipa(*out, "vkGetPhysicalDeviceFormatProperties2KHR");
	in->GetPhysicalDeviceImageFormatProperties2 =
	    (PFN_vkGetPhysicalDeviceImageFormatProperties2)gipa(*out, "vkGetPhysicalDeviceImageFormatProperties2");
	if (!in->GetPhysicalDeviceImageFormatProperties2)
		in->GetPhysicalDeviceImageFormatProperties2 =
		    (PFN_vkGetPhysicalDeviceImageFormatProperties2)gipa(*out, "vkGetPhysicalDeviceImageFormatProperties2KHR");

	pthread_mutex_lock(&g_lock);
	for (int i = 0; i < MAX_OBJS; i++)
		if (!g_instances[i]) {
			g_instances[i] = in;
			break;
		}
	pthread_mutex_unlock(&g_lock);
	LOG("active\n");
	return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks *alloc)
{
	struct instance *in = instance_of(instance);
	pthread_mutex_lock(&g_lock);
	for (int i = 0; i < MAX_OBJS; i++)
		if (g_instances[i] == in)
			g_instances[i] = NULL;
	pthread_mutex_unlock(&g_lock);
	PFN_vkDestroyInstance destroy = in->DestroyInstance;
	free(in);
	destroy(instance, alloc);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char *name)
{
	PFN_vkVoidFunction f = instance_intercept(name);
	if (f)
		return f;
	f = device_intercept(name);
	if (!f)
		f = debug_utils_stub(name);
	if (!f)
		f = qd_proc(name);
	if (f)
		return f;
	struct instance *in = instance ? instance_of(instance) : NULL;
	return in ? in->gipa(instance, name) : NULL;
}

static PFN_vkVoidFunction instance_intercept(const char *name)
{
	PFN_vkVoidFunction fxi = fx_iproc(name);
	if (fxi)
		return fxi;
#define I(n, fn)                                                                                                       \
	if (strcmp(name, n) == 0)                                                                                      \
		return (PFN_vkVoidFunction)fn;
	I("vkGetInstanceProcAddr", GetInstanceProcAddr)
	I("vkCreateInstance", CreateInstance)
	I("vkDestroyInstance", DestroyInstance)
	I("vkCreateDevice", CreateDevice)
	I("vkEnumerateDeviceExtensionProperties", EnumerateDeviceExtensionProperties)
	I("vkGetPhysicalDeviceFeatures2", GetPhysicalDeviceFeatures2)
	I("vkGetPhysicalDeviceFeatures2KHR", GetPhysicalDeviceFeatures2)
	I("vkGetPhysicalDeviceProperties", GetPhysicalDeviceProperties)
	I("vkGetPhysicalDeviceProperties2", GetPhysicalDeviceProperties2)
	I("vkGetPhysicalDeviceProperties2KHR", GetPhysicalDeviceProperties2)
	I("vkGetPhysicalDeviceExternalSemaphoreProperties", GetPhysicalDeviceExternalSemaphoreProperties)
	I("vkGetPhysicalDeviceExternalSemaphorePropertiesKHR", GetPhysicalDeviceExternalSemaphoreProperties)
	I("vkGetPhysicalDeviceFormatProperties2", GetPhysicalDeviceFormatProperties2)
	I("vkGetPhysicalDeviceFormatProperties2KHR", GetPhysicalDeviceFormatProperties2)
	I("vkGetPhysicalDeviceImageFormatProperties2", GetPhysicalDeviceImageFormatProperties2)
	I("vkGetPhysicalDeviceImageFormatProperties2KHR", GetPhysicalDeviceImageFormatProperties2)
#undef I
	return NULL;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v)
{
	if (v->loaderLayerInterfaceVersion > 2)
		v->loaderLayerInterfaceVersion = 2;
	v->pfnGetInstanceProcAddr = GetInstanceProcAddr;
	v->pfnGetDeviceProcAddr = GetDeviceProcAddr;
	v->pfnGetPhysicalDeviceProcAddr = NULL;
	return VK_SUCCESS;
}
