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
// It also lets X11 clients present (quest1_wsi_x11.c, QUEST1_VKSHIM_WSI=1): the Android loader only
// knows Android/Wayland surfaces, the Steam webhelper's ANGLE Vulkan backend needs VK_KHR_xcb_surface.
//
// Env: QUEST1_VKSHIM_NEXT  (default /opt/hybris/lib/libvulkan.so.1)
//      QUEST1_VKSHIM_LAYER (default /usr/local/lib/libVkLayer_quest1_steamvr_compat.so, "none": no layer)
//      QUEST1_VKSHIM_WSI=1 X11 surfaces and swapchains
//      QUEST1_VKSHIM_CACHED_READBACK=1 really cached memory for CPU readback (see "readback memory" below)
#define _GNU_SOURCE // program_invocation_short_name
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include "quest1_wsi.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))

static PFN_vkGetInstanceProcAddr next_gipa;
static PFN_vkGetDeviceProcAddr next_gdpa;
static PFN_vkGetInstanceProcAddr layer_gipa;
static PFN_vkGetDeviceProcAddr layer_gdpa;
// the next element of the chain: the layer, or hybris directly
static PFN_vkGetInstanceProcAddr down_gipa;
static PFN_vkGetDeviceProcAddr down_gdpa;
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

	int nolayer = !*lp || strcmp(lp, "none") == 0;
	void *layer = nolayer ? NULL : dlopen(lp, RTLD_NOW | RTLD_LOCAL);
	PFN_vkNegotiateLoaderLayerInterfaceVersion neg =
	    layer ? (PFN_vkNegotiateLoaderLayerInterfaceVersion)dlsym(layer, "vkNegotiateLoaderLayerInterfaceVersion") : NULL;
	VkNegotiateLayerInterface ni = {LAYER_NEGOTIATE_INTERFACE_STRUCT, NULL, 2};
	if (neg && neg(&ni) == VK_SUCCESS) {
		layer_gipa = ni.pfnGetInstanceProcAddr;
		layer_gdpa = ni.pfnGetDeviceProcAddr;
	} else if (!nolayer) {
		fprintf(stderr, "quest1_vkshim: no compat layer (%s), passing straight through\n",
		        layer ? "no vkNegotiateLoaderLayerInterfaceVersion" : dlerror());
	}
	down_gipa = layer_gipa ? layer_gipa : next_gipa;
	down_gdpa = layer_gdpa ? layer_gdpa : next_gdpa;
	wsi_set_down(down_gipa);
	fprintf(stderr, "quest1_vkshim[%d]: next %s, layer %s, X11 WSI %s\n", (int)getpid(), np,
	        layer_gipa ? lp : "none", wsi_enabled() ? "on" : "off");
}

#define READY() pthread_once(&once, load)

// vkCreateDevice needs an instance to look the next entry point up; remember the last one
// (SteamVR processes create a single instance)
static VkInstance last_instance;

// the layer may register dispatchable objects it creates itself; the Android loader below already
// owns their dispatch word, so there is nothing to set
static VKAPI_ATTR VkResult VKAPI_CALL set_instance_data(VkInstance i, void *o) { return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL set_device_data(VkDevice d, void *o) { return VK_SUCCESS; }

static int is_wsi_ext(const char *name)
{
	for (unsigned k = 0; k < wsi_instance_ext_count; k++)
		if (strcmp(name, wsi_instance_exts[k]) == 0)
			return 1;
	return 0;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *alloc,
                                                       VkInstance *out)
{
	READY();
	if (!next_gipa)
		return VK_ERROR_INITIALIZATION_FAILED;
	// the X11 surface extensions are implemented here: do not pass them down
	VkInstanceCreateInfo filtered = *ci;
	const char *names[128];
	if (wsi_enabled() && ci->enabledExtensionCount <= 128) {
		uint32_t n = 0;
		for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
			if (!is_wsi_ext(ci->ppEnabledExtensionNames[i]))
				names[n++] = ci->ppEnabledExtensionNames[i];
		filtered.enabledExtensionCount = n;
		filtered.ppEnabledExtensionNames = names;
		ci = &filtered;
	}
	PFN_vkCreateInstance create = (PFN_vkCreateInstance)next_gipa(VK_NULL_HANDLE, "vkCreateInstance");
	VkInstanceCreateInfo copy = *ci;
	if (layer_gipa) {
		VkLayerInstanceLink link = {NULL, next_gipa, NULL};
		VkLayerInstanceCreateInfo data = {VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO, ci->pNext,
		                                  VK_LOADER_DATA_CALLBACK};
		data.u.pfnSetInstanceLoaderData = set_instance_data;
		VkLayerInstanceCreateInfo chain = {VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO, &data, VK_LAYER_LINK_INFO};
		chain.u.pLayerInfo = &link;
		copy.pNext = &chain;
		create = (PFN_vkCreateInstance)layer_gipa(VK_NULL_HANDLE, "vkCreateInstance");
		VkResult r = create(&copy, alloc, out);
		if (r == VK_SUCCESS) {
			last_instance = *out;
			wsi_instance_created(*out);
		}
		return r;
	}
	VkResult r = create(&copy, alloc, out);
	if (r == VK_SUCCESS) {
		last_instance = *out;
		wsi_instance_created(*out);
	}
	if (getenv("QUEST1_VKSHIM_DEBUG"))
		fprintf(stderr, "quest1_vkshim: vkCreateInstance = %d, %p\n", r, r == VK_SUCCESS ? (void *)*out : NULL);
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci,
                                                   const VkAllocationCallbacks *alloc, VkDevice *out, VkInstance inst)
{
	VkResult r;
	if (layer_gipa) {
		VkLayerDeviceLink link = {NULL, next_gipa, next_gdpa};
		VkLayerDeviceCreateInfo data = {VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, ci->pNext,
		                                VK_LOADER_DATA_CALLBACK};
		data.u.pfnSetDeviceLoaderData = set_device_data;
		VkLayerDeviceCreateInfo chain = {VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, &data, VK_LAYER_LINK_INFO};
		chain.u.pLayerInfo = &link;
		VkDeviceCreateInfo copy = *ci;
		copy.pNext = &chain;
		PFN_vkCreateDevice create = (PFN_vkCreateDevice)layer_gipa(inst, "vkCreateDevice");
		r = create(pd, &copy, alloc, out);
	} else {
		// Steam's CEF GPU process (it loads no layer) composites the pages in GPU batches of up to ~40 ms (a
		// repaint, e.g. the laser hovering the dashboard). At the default priority (kgsl MED) they share
		// vrcompositor's ring buffer and it misses frames; LOW puts them on the ring below, which vrcompositor
		// preempts. Same as the layer does in the other Steam processes (quest1_compat.c).
		VkDeviceCreateInfo copy = *ci;
		VkDeviceQueueCreateInfo queues[8];
		VkDeviceQueueGlobalPriorityCreateInfoKHR low[8];
		const char *exts[64];
		const char *prog = program_invocation_short_name;
		PFN_vkEnumerateDeviceExtensionProperties enum_exts =
		    (PFN_vkEnumerateDeviceExtensionProperties)next_gipa(inst, "vkEnumerateDeviceExtensionProperties");
		bool have = false;
		uint32_t n = 0;
		VkExtensionProperties props[256];
		if ((!strcmp(prog, "steamwebhelper") || !strcmp(prog, "vrwebhelper")) && !getenv("QUEST1_COMPAT_KEEP_PRIORITY") &&
		    ci->queueCreateInfoCount <= 8 && ci->enabledExtensionCount < 63 && enum_exts &&
		    enum_exts(pd, NULL, &n, NULL) == VK_SUCCESS && n <= 256 && enum_exts(pd, NULL, &n, props) == VK_SUCCESS)
			for (uint32_t i = 0; i < n; i++)
				have |= !strcmp(props[i].extensionName, VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME);
		if (have) {
			bool enabled = false;
			for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
				exts[i] = ci->ppEnabledExtensionNames[i];
				enabled |= !strcmp(exts[i], VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME);
			}
			copy.enabledExtensionCount = ci->enabledExtensionCount;
			if (!enabled)
				exts[copy.enabledExtensionCount++] = VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME;
			copy.ppEnabledExtensionNames = exts;
			for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++) {
				queues[i] = ci->pQueueCreateInfos[i];
				bool set = false;
				for (const VkBaseInStructure *p = queues[i].pNext; p; p = p->pNext)
					set |= p->sType == VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR;
				if (set)
					continue;
				low[i] = (VkDeviceQueueGlobalPriorityCreateInfoKHR){
				    VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR, queues[i].pNext,
				    VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR};
				queues[i].pNext = &low[i];
			}
			copy.pQueueCreateInfos = queues;
		}
		r = ((PFN_vkCreateDevice)next_gipa(inst, "vkCreateDevice"))(pd, &copy, alloc, out);
		if (r != VK_SUCCESS && have) // never refused, LOW needs no privilege; but keep the device either way
			r = ((PFN_vkCreateDevice)next_gipa(inst, "vkCreateDevice"))(pd, ci, alloc, out);
	}
	if (r == VK_SUCCESS && wsi_enabled())
		wsi_device_created(inst, pd, ci, *out, down_gdpa);
	return r;
}

// --- readback memory (QUEST1_VKSHIM_CACHED_READBACK=1, set by steam-webhelper-gpu) --------------------------------
// The Adreno 540 blob lists a HOST_VISIBLE|HOST_CACHED|HOST_COHERENT type (2) that is write-combined in fact (kgsl
// cache mode, test_readback: CPU reads at ~220 MB/s), and a HOST_CACHED non-coherent type (1) that is really cached
// (~4 GB/s, 2 ms to invalidate 8 MB), which it allows for transfer-destination-only buffers. Chromium reads every
// frame of Steam's off-screen pages back through ANGLE (whose allocator prefers HOST_CACHED for that), and copying
// 8 MB out of type 2 took ~36 ms of the VizCompositorThread a frame (2026-11-08). So: type 2 is reported without
// HOST_CACHED, and buffers that may use it may use type 1 too, so that what wants cached memory gets real cached
// memory. ANGLE handles non-coherent memory (flush and invalidate around its maps).
static int cached_type = -1, fake_cached_type = -1; // type 1 and type 2 on the A540, found by their flags
static pthread_once_t cached_once = PTHREAD_ONCE_INIT;
static PFN_vkGetPhysicalDeviceMemoryProperties real_mem_props;

static void cached_init_types(VkPhysicalDevice pd)
{
	const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, hc = VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
	                            coh = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	PFN_vkGetPhysicalDeviceMemoryProperties get = real_mem_props;
	if (!get)
		get = (PFN_vkGetPhysicalDeviceMemoryProperties)down_gipa(last_instance, "vkGetPhysicalDeviceMemoryProperties");
	VkPhysicalDeviceMemoryProperties mp;
	get(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
		VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
		if ((f & (hv | hc | coh)) == (hv | hc) && cached_type < 0)
			cached_type = (int)i;
		if ((f & (hv | hc | coh)) == (hv | hc | coh) && fake_cached_type < 0)
			fake_cached_type = (int)i;
	}
	if (cached_type < 0 || fake_cached_type < 0)
		cached_type = fake_cached_type = -1;
	fprintf(stderr, "quest1_vkshim[%d]: cached readback memory: type %d (type %d is reported write-combined)\n",
	        (int)getpid(), cached_type, fake_cached_type);
}

static bool cached_readback(void)
{
	static int on = -1;
	if (on < 0) {
		const char *e = getenv("QUEST1_VKSHIM_CACHED_READBACK");
		on = e && *e && strcmp(e, "0") != 0;
	}
	return on;
}

static VkPhysicalDevice cached_pd;
static void cached_init(void) { cached_init_types(cached_pd); }

static void fix_mem_props(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties *mp)
{
	cached_pd = pd;
	pthread_once(&cached_once, cached_init);
	if (fake_cached_type >= 0 && (uint32_t)fake_cached_type < mp->memoryTypeCount)
		mp->memoryTypes[fake_cached_type].propertyFlags &= ~VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
}

static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceMemoryProperties(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties *mp)
{
	real_mem_props(pd, mp);
	fix_mem_props(pd, mp);
}

static PFN_vkGetPhysicalDeviceMemoryProperties2 real_mem_props2;
static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties2 *mp)
{
	real_mem_props2(pd, mp);
	fix_mem_props(pd, &mp->memoryProperties);
}

static void fix_bits(uint32_t *bits)
{
	if (cached_type >= 0 && (*bits & (1u << fake_cached_type)))
		*bits |= 1u << cached_type;
}

static PFN_vkGetBufferMemoryRequirements real_buf_reqs;
static VKAPI_ATTR void VKAPI_CALL GetBufferMemoryRequirements(VkDevice d, VkBuffer b, VkMemoryRequirements *r)
{
	real_buf_reqs(d, b, r);
	fix_bits(&r->memoryTypeBits);
}

static PFN_vkGetBufferMemoryRequirements2 real_buf_reqs2, real_buf_reqs2_khr;
static VKAPI_ATTR void VKAPI_CALL GetBufferMemoryRequirements2(VkDevice d, const VkBufferMemoryRequirementsInfo2 *i,
                                                               VkMemoryRequirements2 *r)
{
	real_buf_reqs2(d, i, r);
	fix_bits(&r->memoryRequirements.memoryTypeBits);
}
static VKAPI_ATTR void VKAPI_CALL GetBufferMemoryRequirements2KHR(VkDevice d, const VkBufferMemoryRequirementsInfo2 *i,
                                                                  VkMemoryRequirements2 *r)
{
	real_buf_reqs2_khr(d, i, r);
	fix_bits(&r->memoryRequirements.memoryTypeBits);
}

//! Our wrapper for a memory query when the readback fix is on, else NULL. *real gets the next one.
static PFN_vkVoidFunction cached_proc(const char *name, PFN_vkVoidFunction next)
{
	if (!cached_readback() || !next)
		return NULL;
#define W(n, real, fn)                                                                                                 \
	if (strcmp(name, n) == 0) {                                                                                    \
		real = (__typeof__(real))next;                                                                         \
		return (PFN_vkVoidFunction)fn;                                                                         \
	}
	W("vkGetPhysicalDeviceMemoryProperties", real_mem_props, GetPhysicalDeviceMemoryProperties)
	W("vkGetPhysicalDeviceMemoryProperties2", real_mem_props2, GetPhysicalDeviceMemoryProperties2)
	W("vkGetPhysicalDeviceMemoryProperties2KHR", real_mem_props2, GetPhysicalDeviceMemoryProperties2)
	W("vkGetBufferMemoryRequirements", real_buf_reqs, GetBufferMemoryRequirements)
	W("vkGetBufferMemoryRequirements2", real_buf_reqs2, GetBufferMemoryRequirements2)
	W("vkGetBufferMemoryRequirements2KHR", real_buf_reqs2_khr, GetBufferMemoryRequirements2KHR)
#undef W
	return NULL;
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
	PFN_vkVoidFunction w = wsi_proc(name);
	if (w)
		return w;
	PFN_vkVoidFunction next = down_gdpa(device, name);
	PFN_vkVoidFunction c = cached_proc(name, next);
	return c ? c : next;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char *layer, uint32_t *count,
                                                                             VkExtensionProperties *props)
{
	READY();
	PFN_vkEnumerateInstanceExtensionProperties down = (PFN_vkEnumerateInstanceExtensionProperties)next_gipa(
	    VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
	if (layer || !wsi_enabled())
		return down(layer, count, props);
	// append VK_KHR_xcb_surface / VK_KHR_xlib_surface
	uint32_t n = 0;
	VkResult r = down(NULL, &n, NULL);
	if (r != VK_SUCCESS)
		return r;
	VkExtensionProperties *all = calloc(n + wsi_instance_ext_count, sizeof(*all));
	if (!all)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	r = down(NULL, &n, all);
	if (r < 0) {
		free(all);
		return r;
	}
	for (unsigned k = 0; k < wsi_instance_ext_count; k++) {
		int have = 0;
		for (uint32_t i = 0; i < n; i++)
			have |= strcmp(all[i].extensionName, wsi_instance_exts[k]) == 0;
		if (!have) {
			snprintf(all[n].extensionName, sizeof(all[n].extensionName), "%s", wsi_instance_exts[k]);
			all[n].specVersion = 6;
			n++;
		}
	}
	if (!props) {
		*count = n;
		r = VK_SUCCESS;
	} else {
		uint32_t m = *count < n ? *count : n;
		memcpy(props, all, m * sizeof(*props));
		*count = m;
		r = m < n ? VK_INCOMPLETE : VK_SUCCESS;
	}
	free(all);
	return r;
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
	if (!instance)
		return NULL;
	if (strcmp(name, "vkCreateDevice") == 0) {
		last_instance = instance;
		return (PFN_vkVoidFunction)CreateDeviceTrampoline;
	}
	PFN_vkVoidFunction w = wsi_proc(name);
	if (w)
		return w;
	PFN_vkVoidFunction next = down_gipa(instance, name);
	PFN_vkVoidFunction c = cached_proc(name, next);
	return c ? c : next;
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
