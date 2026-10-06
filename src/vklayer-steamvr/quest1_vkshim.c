// quest1_vkshim: a minimal libvulkan.so.1 for SteamVR on the Quest 1 (Holo + libhybris).
//
// The libvulkan.so.1 in /opt/hybris/lib wraps Android's loader, which never reads implicit-layer
// manifests, and it is not a Khronos ICD (no vk_icd* entry points; the Khronos loader would also
// overwrite the first word of every dispatchable handle, which the Android loader owns). So for
// SteamVR only, this library takes the place of libvulkan.so.1 (directory first on
// LD_LIBRARY_PATH) and chains: application -> quest1 compat layer -> hybris libvulkan.
// vrcompositor and vrserver dlopen libvulkan.so.1 and resolve everything through
// vkGetInstanceProcAddr / vkGetDeviceProcAddr, which is all this exports besides the global commands.
//
// Env: QUEST1_VKSHIM_NEXT  (default /opt/hybris/lib/libvulkan.so.1)
//      QUEST1_VKSHIM_LAYER (default /usr/local/lib/libVkLayer_quest1_steamvr_compat.so)
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPORT __attribute__((visibility("default")))

static PFN_vkGetInstanceProcAddr next_gipa;
static PFN_vkGetDeviceProcAddr next_gdpa;
static PFN_vkGetInstanceProcAddr layer_gipa;
static PFN_vkGetDeviceProcAddr layer_gdpa;
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void load(void)
{
	const char *np = getenv("QUEST1_VKSHIM_NEXT");
	const char *lp = getenv("QUEST1_VKSHIM_LAYER");
	if (!np)
		np = "/opt/hybris/lib/libvulkan.so.1";
	if (!lp)
		lp = "/usr/local/lib/libVkLayer_quest1_steamvr_compat.so";
	// by absolute path, so it does not resolve to ourselves through the shared soname; DEEPBIND
	// because the hybris wrapper looks its own vk* symbols up, which would otherwise find ours
	void *next = dlopen(np, RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
	if (!next) {
		fprintf(stderr, "quest1_vkshim: %s\n", dlerror());
		return;
	}
	next_gipa = (PFN_vkGetInstanceProcAddr)dlsym(next, "vkGetInstanceProcAddr");
	next_gdpa = (PFN_vkGetDeviceProcAddr)dlsym(next, "vkGetDeviceProcAddr");

	void *layer = dlopen(lp, RTLD_NOW | RTLD_LOCAL);
	PFN_vkNegotiateLoaderLayerInterfaceVersion neg =
	    layer ? (PFN_vkNegotiateLoaderLayerInterfaceVersion)dlsym(layer, "vkNegotiateLoaderLayerInterfaceVersion") : NULL;
	VkNegotiateLayerInterface ni = {LAYER_NEGOTIATE_INTERFACE_STRUCT, NULL, 2};
	if (neg && neg(&ni) == VK_SUCCESS) {
		layer_gipa = ni.pfnGetInstanceProcAddr;
		layer_gdpa = ni.pfnGetDeviceProcAddr;
	} else {
		fprintf(stderr, "quest1_vkshim: no compat layer (%s), passing straight through\n",
		        layer ? "no vkNegotiateLoaderLayerInterfaceVersion" : dlerror());
	}
	fprintf(stderr, "quest1_vkshim: next %s, layer %s\n", np, layer_gipa ? lp : "none");
}

#define READY() pthread_once(&once, load)

// vkCreateDevice needs an instance to look the layer's entry point up; remember the last one
// (SteamVR processes create a single instance)
static VkInstance last_instance;

// the layer may register dispatchable objects it creates itself; the Android loader below already
// owns their dispatch word, so there is nothing to set
static VKAPI_ATTR VkResult VKAPI_CALL set_instance_data(VkInstance i, void *o) { return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL set_device_data(VkDevice d, void *o) { return VK_SUCCESS; }

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *alloc,
                                                       VkInstance *out)
{
	READY();
	if (!next_gipa)
		return VK_ERROR_INITIALIZATION_FAILED;
	PFN_vkCreateInstance create = (PFN_vkCreateInstance)next_gipa(VK_NULL_HANDLE, "vkCreateInstance");
	if (!layer_gipa)
		return create(ci, alloc, out);

	VkLayerInstanceLink link = {NULL, next_gipa, NULL};
	VkLayerInstanceCreateInfo data = {VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO, ci->pNext, VK_LOADER_DATA_CALLBACK};
	data.u.pfnSetInstanceLoaderData = set_instance_data;
	VkLayerInstanceCreateInfo chain = {VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO, &data, VK_LAYER_LINK_INFO};
	chain.u.pLayerInfo = &link;
	VkInstanceCreateInfo copy = *ci;
	copy.pNext = &chain;
	create = (PFN_vkCreateInstance)layer_gipa(VK_NULL_HANDLE, "vkCreateInstance");
	VkResult r = create(&copy, alloc, out);
	if (r == VK_SUCCESS)
		last_instance = *out;
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci,
                                                   const VkAllocationCallbacks *alloc, VkDevice *out, VkInstance inst)
{
	VkLayerDeviceLink link = {NULL, next_gipa, next_gdpa};
	VkLayerDeviceCreateInfo data = {VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, ci->pNext, VK_LOADER_DATA_CALLBACK};
	data.u.pfnSetDeviceLoaderData = set_device_data;
	VkLayerDeviceCreateInfo chain = {VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, &data, VK_LAYER_LINK_INFO};
	chain.u.pLayerInfo = &link;
	VkDeviceCreateInfo copy = *ci;
	copy.pNext = &chain;
	PFN_vkCreateDevice create = (PFN_vkCreateDevice)layer_gipa(inst, "vkCreateDevice");
	return create(pd, &copy, alloc, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateDeviceTrampoline(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci,
                                                             const VkAllocationCallbacks *alloc, VkDevice *out)
{
	return CreateDevice(pd, ci, alloc, out, last_instance);
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name);

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *name)
{
	READY();
	if (strcmp(name, "vkGetDeviceProcAddr") == 0)
		return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
	return layer_gdpa ? layer_gdpa(device, name) : next_gdpa(device, name);
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char *layer, uint32_t *count,
                                                                             VkExtensionProperties *props)
{
	READY();
	return ((PFN_vkEnumerateInstanceExtensionProperties)next_gipa(VK_NULL_HANDLE,
	                                                              "vkEnumerateInstanceExtensionProperties"))(layer, count, props);
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t *count, VkLayerProperties *props)
{
	*count = 0; // the compat layer is always on and not enumerable
	return VK_SUCCESS;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t *version)
{
	READY();
	PFN_vkEnumerateInstanceVersion f =
	    (PFN_vkEnumerateInstanceVersion)next_gipa(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
	if (!f) {
		*version = VK_API_VERSION_1_0;
		return VK_SUCCESS;
	}
	return f(version);
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
	READY();
	if (!next_gipa)
		return NULL;
#define G(n, fn)                                                                                                       \
	if (strcmp(name, n) == 0)                                                                                      \
		return (PFN_vkVoidFunction)fn;
	G("vkGetInstanceProcAddr", vkGetInstanceProcAddr)
	G("vkGetDeviceProcAddr", vkGetDeviceProcAddr)
	G("vkCreateInstance", vkCreateInstance)
	G("vkEnumerateInstanceExtensionProperties", vkEnumerateInstanceExtensionProperties)
	G("vkEnumerateInstanceLayerProperties", vkEnumerateInstanceLayerProperties)
	G("vkEnumerateInstanceVersion", vkEnumerateInstanceVersion)
#undef G
	if (!layer_gipa)
		return next_gipa(instance, name);
	if (strcmp(name, "vkCreateDevice") == 0) {
		if (instance)
			last_instance = instance;
		return (PFN_vkVoidFunction)CreateDeviceTrampoline;
	}
	if (!instance)
		return NULL;
	return layer_gipa(instance, name);
}

EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *alloc)
{
	((PFN_vkDestroyInstance)vkGetInstanceProcAddr(instance, "vkDestroyInstance"))(instance, alloc);
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance instance, uint32_t *count,
                                                                 VkPhysicalDevice *pds)
{
	return ((PFN_vkEnumeratePhysicalDevices)vkGetInstanceProcAddr(instance, "vkEnumeratePhysicalDevices"))(instance,
	                                                                                                       count, pds);
}
