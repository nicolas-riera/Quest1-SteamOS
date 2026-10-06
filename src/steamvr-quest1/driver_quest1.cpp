// driver_quest1: SteamVR HMD driver for the Oculus Quest 1 running SteamOS natively.
//
// The headset is already driven by Monado (IMU fusion, lens distortion, panel scan-out, proximity
// sensor). This driver is an OpenXR client of Monado:
//   - poses: xrLocateSpace(VIEW in LOCAL) at a few hundred Hz -> TrackedDevicePoseUpdated
//   - frames (driver direct mode): SteamVR's compositor renders undistorted eye images into
//     textures this driver allocates (gralloc buffers, so they can cross processes); Present()
//     hands them to a frame thread that blits them into OpenXR swapchains and submits a
//     projection layer. Monado applies the lens distortion, so ComputeDistortion is the identity.
//   - pacing: the frame thread runs xrWaitFrame and turns its wake-ups into vsync events.
//
// Runs inside vrserver, which must be started with the xr-run environment (Adreno Vulkan through
// libhybris: LD_LIBRARY_PATH=/opt/hybris/lib, LD_PRELOAD=libbionictls.so, XDG_RUNTIME_DIR=/run/monado).
// OpenXR and Vulkan are loaded with dlopen, so building needs headers only.

#include <openvr_driver.h>

#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_TIMESPEC
#include <time.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

using namespace vr;

static void Log(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	VRDriverLog()->Log(buf);
}

// --- dynamically loaded OpenXR and Vulkan ------------------------------------------------------

#define XR_FUNCS(X)                                                                                                    \
	X(xrCreateInstance)                                                                                            \
	X(xrDestroyInstance)                                                                                           \
	X(xrGetSystem)                                                                                                 \
	X(xrCreateSession)                                                                                             \
	X(xrDestroySession)                                                                                            \
	X(xrBeginSession)                                                                                              \
	X(xrEndSession)                                                                                                \
	X(xrRequestExitSession)                                                                                        \
	X(xrPollEvent)                                                                                                 \
	X(xrCreateReferenceSpace)                                                                                      \
	X(xrLocateSpace)                                                                                               \
	X(xrLocateViews)                                                                                               \
	X(xrEnumerateViewConfigurationViews)                                                                           \
	X(xrEnumerateSwapchainFormats)                                                                                 \
	X(xrCreateSwapchain)                                                                                           \
	X(xrDestroySwapchain)                                                                                          \
	X(xrEnumerateSwapchainImages)                                                                                  \
	X(xrAcquireSwapchainImage)                                                                                     \
	X(xrWaitSwapchainImage)                                                                                        \
	X(xrReleaseSwapchainImage)                                                                                     \
	X(xrWaitFrame)                                                                                                 \
	X(xrBeginFrame)                                                                                                \
	X(xrEndFrame)                                                                                                  \
	X(xrGetVulkanGraphicsRequirements2KHR)                                                                         \
	X(xrCreateVulkanInstanceKHR)                                                                                   \
	X(xrGetVulkanGraphicsDevice2KHR)                                                                               \
	X(xrCreateVulkanDeviceKHR)                                                                                     \
	X(xrConvertTimespecTimeToTimeKHR)

#define VK_FUNCS(X)                                                                                                    \
	X(vkGetPhysicalDeviceQueueFamilyProperties)                                                                    \
	X(vkGetPhysicalDeviceMemoryProperties)                                                                         \
	X(vkGetDeviceQueue)                                                                                            \
	X(vkCreateCommandPool)                                                                                         \
	X(vkAllocateCommandBuffers)                                                                                    \
	X(vkResetCommandBuffer)                                                                                        \
	X(vkBeginCommandBuffer)                                                                                        \
	X(vkEndCommandBuffer)                                                                                          \
	X(vkCmdPipelineBarrier)                                                                                        \
	X(vkCmdBlitImage)                                                                                              \
	X(vkQueueSubmit)                                                                                               \
	X(vkCreateFence)                                                                                               \
	X(vkWaitForFences)                                                                                             \
	X(vkResetFences)                                                                                               \
	X(vkCreateImage)                                                                                               \
	X(vkDestroyImage)                                                                                              \
	X(vkGetImageMemoryRequirements)                                                                                \
	X(vkAllocateMemory)                                                                                            \
	X(vkFreeMemory)                                                                                                \
	X(vkBindImageMemory)                                                                                           \
	X(vkGetMemoryAndroidHardwareBufferANDROID)

#define DECLARE(name) static PFN_##name p##name;
XR_FUNCS(DECLARE)
VK_FUNCS(DECLARE)
static PFN_xrGetInstanceProcAddr pxrGetInstanceProcAddr;
static PFN_vkGetInstanceProcAddr pvkGetInstanceProcAddr;

// libnativewindow (Android) through libhybris: AHB native handle = the gralloc buffer's dma-buf fd
struct native_handle
{
	int version, numFds, numInts;
	int data[0];
};
typedef const native_handle *(*PFN_AHardwareBuffer_getNativeHandle)(const AHardwareBuffer *);
typedef void (*PFN_AHardwareBuffer_acquire)(AHardwareBuffer *);
typedef void (*PFN_AHardwareBuffer_release)(AHardwareBuffer *);
static PFN_AHardwareBuffer_getNativeHandle pAHardwareBuffer_getNativeHandle;
static PFN_AHardwareBuffer_acquire pAHardwareBuffer_acquire;
static PFN_AHardwareBuffer_release pAHardwareBuffer_release;

static bool LoadLibraries()
{
	void *xr = dlopen("libopenxr_loader.so.1", RTLD_NOW | RTLD_LOCAL);
	void *vk = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
	void *hybris = dlopen("libhybris-common.so.1", RTLD_NOW | RTLD_GLOBAL);
	if (!xr || !vk || !hybris) {
		Log("quest1: dlopen failed: %s (run vrserver with the xr-run environment)\n", dlerror());
		return false;
	}
	pxrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)dlsym(xr, "xrGetInstanceProcAddr");
	pvkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(vk, "vkGetInstanceProcAddr");
	auto android_dlopen = (void *(*)(const char *, int))dlsym(hybris, "android_dlopen");
	auto android_dlsym = (void *(*)(void *, const char *))dlsym(hybris, "android_dlsym");
	void *nw = android_dlopen ? android_dlopen("libnativewindow.so", RTLD_NOW) : nullptr;
	if (!pxrGetInstanceProcAddr || !pvkGetInstanceProcAddr || !nw) {
		Log("quest1: missing xrGetInstanceProcAddr, vkGetInstanceProcAddr or libnativewindow\n");
		return false;
	}
	pAHardwareBuffer_getNativeHandle =
	    (PFN_AHardwareBuffer_getNativeHandle)android_dlsym(nw, "AHardwareBuffer_getNativeHandle");
	pAHardwareBuffer_acquire = (PFN_AHardwareBuffer_acquire)android_dlsym(nw, "AHardwareBuffer_acquire");
	pAHardwareBuffer_release = (PFN_AHardwareBuffer_release)android_dlsym(nw, "AHardwareBuffer_release");
	pxrCreateInstance = (PFN_xrCreateInstance)nullptr;
	pxrGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *)&pxrCreateInstance);
	return pxrCreateInstance && pAHardwareBuffer_getNativeHandle;
}

#define XR_CHECK(call)                                                                                                 \
	do {                                                                                                           \
		XrResult r_ = (call);                                                                                  \
		if (XR_FAILED(r_)) {                                                                                   \
			Log("quest1: %s failed (%d)\n", #call, (int)r_);                                               \
			return false;                                                                                  \
		}                                                                                                      \
	} while (0)
#define VK_CHECK(call)                                                                                                 \
	do {                                                                                                           \
		VkResult r_ = (call);                                                                                  \
		if (r_ != VK_SUCCESS) {                                                                                \
			Log("quest1: %s failed (%d)\n", #call, (int)r_);                                               \
			return false;                                                                                  \
		}                                                                                                      \
	} while (0)

// --- math ---------------------------------------------------------------------------------------

static XrPosef PoseFromMatrix(const HmdMatrix34_t &m)
{
	XrPosef p;
	float tr = m.m[0][0] + m.m[1][1] + m.m[2][2];
	XrQuaternionf &q = p.orientation;
	if (tr > 0) {
		float s = sqrtf(tr + 1.0f) * 2;
		q.w = 0.25f * s;
		q.x = (m.m[2][1] - m.m[1][2]) / s;
		q.y = (m.m[0][2] - m.m[2][0]) / s;
		q.z = (m.m[1][0] - m.m[0][1]) / s;
	} else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
		float s = sqrtf(1.0f + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2;
		q.w = (m.m[2][1] - m.m[1][2]) / s;
		q.x = 0.25f * s;
		q.y = (m.m[0][1] + m.m[1][0]) / s;
		q.z = (m.m[0][2] + m.m[2][0]) / s;
	} else if (m.m[1][1] > m.m[2][2]) {
		float s = sqrtf(1.0f + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2;
		q.w = (m.m[0][2] - m.m[2][0]) / s;
		q.x = (m.m[0][1] + m.m[1][0]) / s;
		q.y = 0.25f * s;
		q.z = (m.m[1][2] + m.m[2][1]) / s;
	} else {
		float s = sqrtf(1.0f + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2;
		q.w = (m.m[1][0] - m.m[0][1]) / s;
		q.x = (m.m[0][2] + m.m[2][0]) / s;
		q.y = (m.m[1][2] + m.m[2][1]) / s;
		q.z = 0.25f * s;
	}
	p.position = {m.m[0][3], m.m[1][3], m.m[2][3]};
	return p;
}

// p * (offset, 0, 0): the eye position from the head pose and half the IPD
static XrPosef EyeFromHead(const XrPosef &head, float x)
{
	const XrQuaternionf &q = head.orientation;
	// rotate (x,0,0) by q
	float rx = x * (1 - 2 * (q.y * q.y + q.z * q.z));
	float ry = x * (2 * (q.x * q.y + q.w * q.z));
	float rz = x * (2 * (q.x * q.z - q.w * q.y));
	XrPosef e = head;
	e.position = {head.position.x + rx, head.position.y + ry, head.position.z + rz};
	return e;
}

// SteamVR on Linux describes swap texture formats as VkFormat; also accept the DXGI values it
// uses on Windows, so a wrong guess shows up as wrong colors rather than a failure.
static VkFormat ToVkFormat(uint32_t f)
{
	switch (f) {
	case 28: return VK_FORMAT_R8G8B8A8_UNORM; // DXGI_FORMAT_R8G8B8A8_UNORM
	case 29: return VK_FORMAT_R8G8B8A8_SRGB;  // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
	case 87: return VK_FORMAT_B8G8R8A8_UNORM; // DXGI_FORMAT_B8G8R8A8_UNORM
	case 91: return VK_FORMAT_B8G8R8A8_SRGB;  // DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
	default: return (VkFormat)f;
	}
}

// --- shared textures ------------------------------------------------------------------------------

// One texture SteamVR's compositor renders into. Backed by a gralloc buffer (AHardwareBuffer) so it
// can cross into vrcompositor; imported here as a VkImage to blit from.
struct SharedTexture
{
	SharedTextureHandle_t handle = 0; //!< What SteamVR sees (see AllocateShared).
	uint32_t pid = 0;
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	AHardwareBuffer *ahb = nullptr;
	int fd = -1; //!< dma-buf fd of the gralloc buffer, owned by the AHB.
	uint32_t width = 0, height = 0;
	VkFormat format = VK_FORMAT_UNDEFINED;
	SharedTexture *set[3] = {}; //!< The swap texture set this texture belongs to.
	uint32_t next = 0;          //!< Next index of the set (stored on set[0]).
};

// --- OpenXR backend ----------------------------------------------------------------------------------

class XrBackend
{
public:
	bool Init();
	void Shutdown();

	// tracking
	bool Locate(DriverPose_t &pose);
	XrFovf fov[2] = {};
	float ipd = 0.063f;
	uint32_t eyeWidth = 0, eyeHeight = 0;
	float displayHz = 72.0f;

	// direct mode
	bool AllocateShared(uint32_t pid, const IVRDriverDirectModeComponent::SwapTextureSetDesc_t &desc,
	                    IVRDriverDirectModeComponent::SwapTextureSet_t &out);
	void DestroyShared(SharedTextureHandle_t h);
	void DestroySharedForPid(uint32_t pid);
	void NextIndices(SharedTextureHandle_t handles[2], uint32_t (*indices)[2]);
	void Submit(const IVRDriverDirectModeComponent::SubmitLayerPerEye_t (&perEye)[2]);
	void Present();
	void WaitPresented();

	std::function<void()> onVsync;

private:
	bool InitVulkan();
	bool InitSession();
	bool CreateSwapchains(VkFormat format);
	void FrameThread();
	bool Blit(SharedTexture *src, const VRTextureBounds_t &b, int eye, uint32_t index);
	XrTime Now();

	XrInstance instance = XR_NULL_HANDLE;
	XrSystemId system = XR_NULL_SYSTEM_ID;
	XrSession session = XR_NULL_HANDLE;
	XrSpace local = XR_NULL_HANDLE, view = XR_NULL_HANDLE;
	XrSwapchain swapchain[2] = {};
	std::vector<XrSwapchainImageVulkan2KHR> images[2];
	VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;

	VkInstance vkInstance = VK_NULL_HANDLE;
	VkPhysicalDevice phys = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	uint32_t queueFamily = 0;
	VkQueue queue = VK_NULL_HANDLE;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;

	std::mutex texMutex;
	std::map<SharedTextureHandle_t, SharedTexture *> textures;
	uint64_t nextHandle = 1;

	// frame hand-off between Present() (vrcompositor's IPC thread) and the frame thread
	std::mutex frameMutex;
	std::condition_variable frameCond;
	IVRDriverDirectModeComponent::SubmitLayerPerEye_t pending[2] = {};
	bool havePending = false, presented = false;
	uint64_t presentCount = 0, consumedCount = 0;

	std::thread frameThread;
	std::atomic<bool> running{false};
	bool sessionRunning = false;
};

XrTime XrBackend::Now()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	XrTime t = 0;
	pxrConvertTimespecTimeToTimeKHR(instance, &ts, &t);
	return t;
}

bool XrBackend::Init()
{
	if (!LoadLibraries())
		return false;
	const char *exts[] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME};
	XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
	strcpy(ci.applicationInfo.applicationName, "SteamVR (driver_quest1)");
	strcpy(ci.applicationInfo.engineName, "SteamVR");
	ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	ci.enabledExtensionCount = 2;
	ci.enabledExtensionNames = exts;
	XR_CHECK(pxrCreateInstance(&ci, &instance));
#define LOAD_XR(name) pxrGetInstanceProcAddr(instance, #name, (PFN_xrVoidFunction *)&p##name);
	XR_FUNCS(LOAD_XR)
	XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XR_CHECK(pxrGetSystem(instance, &sgi, &system));

	XrViewConfigurationView vcv[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
	uint32_t n = 0;
	XR_CHECK(pxrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n,
	                                            vcv));
	eyeWidth = vcv[0].recommendedImageRectWidth;
	eyeHeight = vcv[0].recommendedImageRectHeight;

	if (!InitVulkan() || !InitSession())
		return false;

	// FOV and IPD from the views at the current time (fixed for this HMD)
	XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
	vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	vli.displayTime = Now();
	vli.space = view;
	XrViewState vs{XR_TYPE_VIEW_STATE};
	XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
	XR_CHECK(pxrLocateViews(session, &vli, &vs, 2, &n, views));
	fov[0] = views[0].fov;
	fov[1] = views[1].fov;
	ipd = fabsf(views[1].pose.position.x - views[0].pose.position.x);
	if (ipd < 0.05f || ipd > 0.08f)
		ipd = 0.063f;
	Log("quest1: eye %ux%u, ipd %.1f mm, fov L %.1f/%.1f/%.1f/%.1f deg\n", eyeWidth, eyeHeight, ipd * 1000,
	    fov[0].angleLeft * 57.2958f, fov[0].angleRight * 57.2958f, fov[0].angleUp * 57.2958f,
	    fov[0].angleDown * 57.2958f);

	running = true;
	frameThread = std::thread(&XrBackend::FrameThread, this);
	return true;
}

bool XrBackend::InitVulkan()
{
	XrGraphicsRequirementsVulkan2KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
	XR_CHECK(pxrGetVulkanGraphicsRequirements2KHR(instance, system, &reqs));

	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "driver_quest1";
	app.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	XrVulkanInstanceCreateInfoKHR xici{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
	xici.systemId = system;
	xici.pfnGetInstanceProcAddr = pvkGetInstanceProcAddr;
	xici.vulkanCreateInfo = &ici;
	VkResult vr;
	XR_CHECK(pxrCreateVulkanInstanceKHR(instance, &xici, &vkInstance, &vr));
	VK_CHECK(vr);

	XrVulkanGraphicsDeviceGetInfoKHR gdi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
	gdi.systemId = system;
	gdi.vulkanInstance = vkInstance;
	XR_CHECK(pxrGetVulkanGraphicsDevice2KHR(instance, &gdi, &phys));

#define LOAD_VK(name) p##name = (PFN_##name)pvkGetInstanceProcAddr(vkInstance, #name);
	VK_FUNCS(LOAD_VK)

	uint32_t nfam = 0;
	pvkGetPhysicalDeviceQueueFamilyProperties(phys, &nfam, nullptr);
	std::vector<VkQueueFamilyProperties> fams(nfam);
	pvkGetPhysicalDeviceQueueFamilyProperties(phys, &nfam, fams.data());
	queueFamily = UINT32_MAX;
	for (uint32_t i = 0; i < nfam && queueFamily == UINT32_MAX; i++)
		if (fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
			queueFamily = i;
	if (queueFamily == UINT32_MAX)
		return false;

	// AHB import/export to share the swap textures with vrcompositor
	const char *devExts[] = {
	    VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
	    VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
	    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
	    VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
	    VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
	};
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = queueFamily;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;
	VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.enabledExtensionCount = sizeof(devExts) / sizeof(devExts[0]);
	dci.ppEnabledExtensionNames = devExts;
	XrVulkanDeviceCreateInfoKHR xdci{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
	xdci.systemId = system;
	xdci.pfnGetInstanceProcAddr = pvkGetInstanceProcAddr;
	xdci.vulkanPhysicalDevice = phys;
	xdci.vulkanCreateInfo = &dci;
	XR_CHECK(pxrCreateVulkanDeviceKHR(instance, &xdci, &device, &vr));
	VK_CHECK(vr);
	pvkGetDeviceQueue(device, queueFamily, 0, &queue);

	VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pci.queueFamilyIndex = queueFamily;
	VK_CHECK(pvkCreateCommandPool(device, &pci, nullptr, &pool));
	VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cai.commandPool = pool;
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 1;
	VK_CHECK(pvkAllocateCommandBuffers(device, &cai, &cmd));
	VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VK_CHECK(pvkCreateFence(device, &fci, nullptr, &fence));
	return true;
}

bool XrBackend::InitSession()
{
	XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
	binding.instance = vkInstance;
	binding.physicalDevice = phys;
	binding.device = device;
	binding.queueFamilyIndex = queueFamily;
	XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
	sci.next = &binding;
	sci.systemId = system;
	XR_CHECK(pxrCreateSession(instance, &sci, &session));
	XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	rsci.poseInReferenceSpace.orientation.w = 1;
	rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	XR_CHECK(pxrCreateReferenceSpace(session, &rsci, &local));
	rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	XR_CHECK(pxrCreateReferenceSpace(session, &rsci, &view));
	return true;
}

bool XrBackend::CreateSwapchains(VkFormat want)
{
	int64_t formats[64];
	uint32_t n = 0;
	XR_CHECK(pxrEnumerateSwapchainFormats(session, 64, &n, formats));
	// Same sRGB-ness as SteamVR's textures so the blit does not re-encode
	bool srgb = want == VK_FORMAT_R8G8B8A8_SRGB || want == VK_FORMAT_B8G8R8A8_SRGB;
	VkFormat prefer[] = {srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM,
	                     srgb ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM};
	swapchainFormat = VK_FORMAT_UNDEFINED;
	for (VkFormat p : prefer)
		for (uint32_t i = 0; i < n && swapchainFormat == VK_FORMAT_UNDEFINED; i++)
			if (formats[i] == p)
				swapchainFormat = p;
	if (swapchainFormat == VK_FORMAT_UNDEFINED)
		swapchainFormat = (VkFormat)formats[0];

	for (int eye = 0; eye < 2; eye++) {
		XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
		ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
		                XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
		ci.format = swapchainFormat;
		ci.sampleCount = 1;
		ci.width = eyeWidth;
		ci.height = eyeHeight;
		ci.faceCount = ci.arraySize = ci.mipCount = 1;
		XR_CHECK(pxrCreateSwapchain(session, &ci, &swapchain[eye]));
		uint32_t count = 0;
		XR_CHECK(pxrEnumerateSwapchainImages(swapchain[eye], 0, &count, nullptr));
		images[eye].assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
		XR_CHECK(pxrEnumerateSwapchainImages(swapchain[eye], count, &count,
		                                     (XrSwapchainImageBaseHeader *)images[eye].data()));
	}
	Log("quest1: swapchains %ux%u format %d\n", eyeWidth, eyeHeight, (int)swapchainFormat);
	return true;
}

bool XrBackend::Locate(DriverPose_t &pose)
{
	pose = {};
	pose.qWorldFromDriverRotation.w = pose.qDriverFromHeadRotation.w = 1;
	pose.qRotation.w = 1;
	pose.deviceIsConnected = true;
	pose.result = TrackingResult_Running_OK;

	XrSpaceVelocity vel{XR_TYPE_SPACE_VELOCITY};
	XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
	loc.next = &vel;
	if (XR_FAILED(pxrLocateSpace(view, local, Now(), &loc)) ||
	    !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
		pose.result = TrackingResult_Running_OutOfRange;
		return false;
	}
	pose.qRotation = {loc.pose.orientation.w, loc.pose.orientation.x, loc.pose.orientation.y,
	                  loc.pose.orientation.z};
	pose.vecPosition[0] = loc.pose.position.x;
	pose.vecPosition[1] = loc.pose.position.y;
	pose.vecPosition[2] = loc.pose.position.z;
	if (vel.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) {
		// OpenXR gives the angular velocity in the base space (LOCAL), as OpenVR expects
		pose.vecAngularVelocity[0] = vel.angularVelocity.x;
		pose.vecAngularVelocity[1] = vel.angularVelocity.y;
		pose.vecAngularVelocity[2] = vel.angularVelocity.z;
	}
	pose.poseIsValid = true;
	pose.willDriftInYaw = true; // 3DoF IMU, no magnetometer
	return true;
}

bool XrBackend::AllocateShared(uint32_t pid, const IVRDriverDirectModeComponent::SwapTextureSetDesc_t &desc,
                               IVRDriverDirectModeComponent::SwapTextureSet_t &out)
{
	VkFormat format = ToVkFormat(desc.nFormat);
	SharedTexture *set[3] = {};
	for (int i = 0; i < 3; i++) {
		SharedTexture *t = new SharedTexture;
		t->pid = pid;
		t->width = desc.nWidth;
		t->height = desc.nHeight;
		t->format = format;

		VkExternalMemoryImageCreateInfo emi{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
		emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
		VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ici.pNext = &emi;
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = format;
		ici.extent = {desc.nWidth, desc.nHeight, 1};
		ici.mipLevels = ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
		            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VK_CHECK(pvkCreateImage(device, &ici, nullptr, &t->image));

		// Export a dedicated allocation as an AHB (gralloc buffer) so another process can import it
		VkMemoryRequirements mr;
		pvkGetImageMemoryRequirements(device, t->image, &mr);
		VkPhysicalDeviceMemoryProperties mp;
		pvkGetPhysicalDeviceMemoryProperties(phys, &mp);
		uint32_t type = UINT32_MAX;
		for (uint32_t m = 0; m < mp.memoryTypeCount && type == UINT32_MAX; m++)
			if ((mr.memoryTypeBits & (1u << m)) &&
			    (mp.memoryTypes[m].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
				type = m;
		VkExportMemoryAllocateInfo exp{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
		exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
		VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
		ded.pNext = &exp;
		ded.image = t->image;
		VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		mai.pNext = &ded;
		mai.allocationSize = 0; // determined by the AHB for exports with a dedicated image
		mai.memoryTypeIndex = type;
		VK_CHECK(pvkAllocateMemory(device, &mai, nullptr, &t->memory));
		VK_CHECK(pvkBindImageMemory(device, t->image, t->memory, 0));
		VkMemoryGetAndroidHardwareBufferInfoANDROID gi{
		    VK_STRUCTURE_TYPE_MEMORY_GET_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
		gi.memory = t->memory;
		VK_CHECK(pvkGetMemoryAndroidHardwareBufferANDROID(device, &gi, &t->ahb));
		const auto *nh = pAHardwareBuffer_getNativeHandle(t->ahb);
		t->fd = nh && nh->numFds > 0 ? nh->data[0] : -1;

		// TODO(steamvr-port.md): the handle SteamVR expects on Linux (dma-buf fd passed to
		// vrcompositor?). For now the dma-buf fd, which the vrcompositor-side layer maps back
		// to this AHB.
		t->handle = (SharedTextureHandle_t)t->fd;
		set[i] = t;
	}
	std::lock_guard<std::mutex> lock(texMutex);
	for (int i = 0; i < 3; i++) {
		memcpy(set[i]->set, set, sizeof(set));
		textures[set[i]->handle] = set[i];
		out.rSharedTextureHandles[i] = set[i]->handle;
	}
	out.unTextureFlags = 0;
	Log("quest1: swap texture set %ux%u format %u (VkFormat %d) for pid %u: fds %d %d %d\n", desc.nWidth,
	    desc.nHeight, desc.nFormat, (int)format, pid, set[0]->fd, set[1]->fd, set[2]->fd);
	return true;
}

void XrBackend::DestroyShared(SharedTextureHandle_t h)
{
	std::lock_guard<std::mutex> lock(texMutex);
	auto it = textures.find(h);
	if (it == textures.end())
		return;
	SharedTexture *set[3];
	memcpy(set, it->second->set, sizeof(set));
	for (SharedTexture *t : set) {
		textures.erase(t->handle);
		pvkDestroyImage(device, t->image, nullptr);
		pvkFreeMemory(device, t->memory, nullptr); // also releases the exported AHB reference
		delete t;
	}
}

void XrBackend::DestroySharedForPid(uint32_t pid)
{
	std::vector<SharedTextureHandle_t> handles;
	{
		std::lock_guard<std::mutex> lock(texMutex);
		for (auto &kv : textures)
			if (kv.second->pid == pid && kv.second->set[0] == kv.second)
				handles.push_back(kv.first);
	}
	for (auto h : handles)
		DestroyShared(h);
}

void XrBackend::NextIndices(SharedTextureHandle_t handles[2], uint32_t (*indices)[2])
{
	std::lock_guard<std::mutex> lock(texMutex);
	for (int i = 0; i < 2; i++) {
		auto it = textures.find(handles[i]);
		if (it == textures.end()) {
			(*indices)[i] = 0;
			continue;
		}
		SharedTexture *first = it->second->set[0];
		first->next = (first->next + 1) % 3;
		(*indices)[i] = first->next;
	}
}

void XrBackend::Submit(const IVRDriverDirectModeComponent::SubmitLayerPerEye_t (&perEye)[2])
{
	// Only the first (scene) layer is shown; overlays come later as quad layers.
	std::lock_guard<std::mutex> lock(frameMutex);
	if (!havePending) {
		pending[0] = perEye[0];
		pending[1] = perEye[1];
		havePending = true;
	}
}

void XrBackend::Present()
{
	std::lock_guard<std::mutex> lock(frameMutex);
	if (havePending) {
		presented = true;
		presentCount++;
	}
	frameCond.notify_all();
}

// PostPresent: hold vrcompositor until the frame thread took the frame, which paces it to Monado.
void XrBackend::WaitPresented()
{
	std::unique_lock<std::mutex> lock(frameMutex);
	uint64_t want = presentCount;
	frameCond.wait_for(lock, std::chrono::milliseconds(50), [&] { return consumedCount >= want || !running; });
}

bool XrBackend::Blit(SharedTexture *src, const VRTextureBounds_t &b, int eye, uint32_t index)
{
	VkImage dst = images[eye][index].image;
	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VK_CHECK(pvkResetCommandBuffer(cmd, 0));
	VK_CHECK(pvkBeginCommandBuffer(cmd, &bi));
	VkImageMemoryBarrier bar[2] = {{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}, {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}};
	// source: written by vrcompositor's queue (another process) -> acquire from the foreign queue
	bar[0].srcAccessMask = 0;
	bar[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	bar[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	bar[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	bar[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
	bar[0].dstQueueFamilyIndex = queueFamily;
	bar[0].image = src->image;
	bar[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	bar[1].srcAccessMask = 0;
	bar[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	bar[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	bar[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	bar[1].srcQueueFamilyIndex = bar[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	bar[1].image = dst;
	bar[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	pvkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
	                      nullptr, 2, bar);

	// bounds may flip vertically (vMin > vMax)
	VkImageBlit blit{};
	blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	blit.srcOffsets[0] = {(int32_t)(b.uMin * src->width), (int32_t)(b.vMin * src->height), 0};
	blit.srcOffsets[1] = {(int32_t)(b.uMax * src->width), (int32_t)(b.vMax * src->height), 1};
	blit.dstOffsets[0] = {0, 0, 0};
	blit.dstOffsets[1] = {(int32_t)eyeWidth, (int32_t)eyeHeight, 1};
	pvkCmdBlitImage(cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
	                &blit, VK_FILTER_LINEAR);

	// give the source back to vrcompositor, the destination to the OpenXR runtime
	bar[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	bar[0].dstAccessMask = 0;
	bar[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	bar[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
	bar[0].srcQueueFamilyIndex = queueFamily;
	bar[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
	bar[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	bar[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
	bar[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	bar[1].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	pvkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
	                      nullptr, 2, bar);
	VK_CHECK(pvkEndCommandBuffer(cmd));
	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	VK_CHECK(pvkQueueSubmit(queue, 1, &si, fence));
	VK_CHECK(pvkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
	VK_CHECK(pvkResetFences(device, 1, &fence));
	return true;
}

void XrBackend::FrameThread()
{
	pthread_setname_np(pthread_self(), "quest1 frames");
	XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
	                                          {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
	XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	proj.space = local;
	proj.viewCount = 2;
	proj.views = pv;
	bool haveLayer = false;

	while (running) {
		XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
		while (pxrPollEvent(instance, &ev) == XR_SUCCESS) {
			if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
				XrSessionState s = ((XrEventDataSessionStateChanged *)&ev)->state;
				if (s == XR_SESSION_STATE_READY) {
					XrSessionBeginInfo sbi{XR_TYPE_SESSION_BEGIN_INFO};
					sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					sessionRunning = XR_SUCCEEDED(pxrBeginSession(session, &sbi));
				} else if (s == XR_SESSION_STATE_STOPPING) {
					pxrEndSession(session);
					sessionRunning = false;
				}
			}
			ev = {XR_TYPE_EVENT_DATA_BUFFER};
		}
		if (!sessionRunning) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}

		XrFrameState fs{XR_TYPE_FRAME_STATE};
		if (XR_FAILED(pxrWaitFrame(session, nullptr, &fs)))
			break;
		if (onVsync)
			onVsync();
		pxrBeginFrame(session, nullptr);

		// Wait for SteamVR's frame until shortly before Monado needs ours.
		IVRDriverDirectModeComponent::SubmitLayerPerEye_t layer[2];
		bool fresh = false;
		{
			std::unique_lock<std::mutex> lock(frameMutex);
			auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(8);
			frameCond.wait_until(lock, deadline, [&] { return presented || !running; });
			if (presented) {
				layer[0] = pending[0];
				layer[1] = pending[1];
				fresh = true;
				presented = havePending = false;
			}
		}

		if (fresh && swapchain[0] == XR_NULL_HANDLE) {
			std::lock_guard<std::mutex> lock(texMutex);
			auto it = textures.find(layer[0].hTexture);
			if (it != textures.end())
				CreateSwapchains(it->second->format);
		}
		if (fresh && swapchain[0] != XR_NULL_HANDLE) {
			XrPosef head = PoseFromMatrix(layer[0].mHmdPose);
			for (int eye = 0; eye < 2; eye++) {
				SharedTexture *src = nullptr;
				{
					std::lock_guard<std::mutex> lock(texMutex);
					auto it = textures.find(layer[eye].hTexture);
					src = it != textures.end() ? it->second : nullptr;
				}
				if (!src)
					continue;
				uint32_t idx;
				pxrAcquireSwapchainImage(swapchain[eye], nullptr, &idx);
				XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
				wi.timeout = XR_INFINITE_DURATION;
				pxrWaitSwapchainImage(swapchain[eye], &wi);
				Blit(src, layer[eye].bounds, eye, idx);
				pxrReleaseSwapchainImage(swapchain[eye], nullptr);
				pv[eye].pose = EyeFromHead(head, eye == 0 ? -ipd / 2 : ipd / 2);
				pv[eye].fov = fov[eye];
				pv[eye].subImage.swapchain = swapchain[eye];
				pv[eye].subImage.imageRect = {{0, 0}, {(int32_t)eyeWidth, (int32_t)eyeHeight}};
			}
			haveLayer = true;
		}
		{
			std::lock_guard<std::mutex> lock(frameMutex);
			consumedCount = presentCount;
		}
		frameCond.notify_all();

		// Without a new SteamVR frame, show the previous one again rather than nothing.
		const XrCompositionLayerBaseHeader *layers[] = {(XrCompositionLayerBaseHeader *)&proj};
		XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
		fei.displayTime = fs.predictedDisplayTime;
		fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		fei.layerCount = haveLayer && fs.shouldRender ? 1 : 0;
		fei.layers = layers;
		pxrEndFrame(session, &fei);
	}
}

void XrBackend::Shutdown()
{
	running = false;
	frameCond.notify_all();
	if (session != XR_NULL_HANDLE && sessionRunning)
		pxrRequestExitSession(session);
	if (frameThread.joinable())
		frameThread.join();
	if (session != XR_NULL_HANDLE)
		pxrDestroySession(session);
	if (instance != XR_NULL_HANDLE)
		pxrDestroyInstance(instance);
	session = XR_NULL_HANDLE;
	instance = XR_NULL_HANDLE;
}

// --- SteamVR device ----------------------------------------------------------------------------------------

class Quest1Hmd : public ITrackedDeviceServerDriver, public IVRDisplayComponent, public IVRDriverDirectModeComponent
{
public:
	explicit Quest1Hmd(XrBackend &xr) : xr(xr) {}
	virtual ~Quest1Hmd() = default;

	EVRInitError Activate(uint32_t objectId) override
	{
		id = objectId;
		PropertyContainerHandle_t c = VRProperties()->TrackedDeviceToPropertyContainer(id);
		VRProperties()->SetStringProperty(c, Prop_ModelNumber_String, "Quest 1 (SteamOS)");
		VRProperties()->SetStringProperty(c, Prop_ManufacturerName_String, "Oculus");
		VRProperties()->SetStringProperty(c, Prop_TrackingSystemName_String, "quest1");
		VRProperties()->SetStringProperty(c, Prop_DriverVersion_String, "0.1");
		VRProperties()->SetFloatProperty(c, Prop_DisplayFrequency_Float, xr.displayHz);
		VRProperties()->SetFloatProperty(c, Prop_UserIpdMeters_Float, xr.ipd);
		VRProperties()->SetFloatProperty(c, Prop_UserHeadToEyeDepthMeters_Float, 0.0f);
		// Monado wakes the frame thread ~15 ms before the vsync that latches the frame, shown half a
		// period later; refined from xrWaitFrame once frames flow.
		VRProperties()->SetFloatProperty(c, Prop_SecondsFromVsyncToPhotons_Float, 0.022f);
		VRProperties()->SetUint64Property(c, Prop_CurrentUniverseId_Uint64, 2);
		VRProperties()->SetBoolProperty(c, Prop_IsOnDesktop_Bool, false);
		VRProperties()->SetBoolProperty(c, Prop_DisplayDebugMode_Bool, false);
		VRProperties()->SetBoolProperty(c, Prop_HasDriverDirectModeComponent_Bool, true);
		VRProperties()->SetBoolProperty(c, Prop_DriverDirectModeSendsVsyncEvents_Bool, true);
		VRProperties()->SetBoolProperty(c, Prop_DeviceProvidesBatteryStatus_Bool, false);

		xr.onVsync = [] { VRServerDriverHost()->VsyncEvent(0.0); };
		poseThreadRunning = true;
		poseThread = std::thread([this] {
			pthread_setname_np(pthread_self(), "quest1 poses");
			while (poseThreadRunning) {
				DriverPose_t p;
				xr.Locate(p);
				{
					std::lock_guard<std::mutex> lock(poseMutex);
					lastPose = p;
				}
				VRServerDriverHost()->TrackedDevicePoseUpdated(id, p, sizeof(p));
				std::this_thread::sleep_for(std::chrono::milliseconds(4)); // 250 Hz
			}
		});
		return VRInitError_None;
	}

	void Deactivate() override
	{
		poseThreadRunning = false;
		if (poseThread.joinable())
			poseThread.join();
		id = k_unTrackedDeviceIndexInvalid;
	}

	void EnterStandby() override {}

	void *GetComponent(const char *name) override
	{
		if (!strcmp(name, IVRDisplayComponent_Version))
			return static_cast<IVRDisplayComponent *>(this);
		if (!strcmp(name, IVRDriverDirectModeComponent_Version))
			return static_cast<IVRDriverDirectModeComponent *>(this);
		return nullptr;
	}

	void DebugRequest(const char *, char *response, uint32_t size) override
	{
		if (size)
			response[0] = 0;
	}

	DriverPose_t GetPose() override
	{
		std::lock_guard<std::mutex> lock(poseMutex);
		return lastPose;
	}

	// IVRDisplayComponent: a virtual display, distortion is Monado's job
	void GetWindowBounds(int32_t *x, int32_t *y, uint32_t *w, uint32_t *h) override
	{
		*x = *y = 0;
		*w = xr.eyeWidth * 2;
		*h = xr.eyeHeight;
	}
	bool IsDisplayOnDesktop() override { return false; }
	bool IsDisplayRealDisplay() override { return false; }
	void GetRecommendedRenderTargetSize(uint32_t *w, uint32_t *h) override
	{
		*w = xr.eyeWidth;
		*h = xr.eyeHeight;
	}
	void GetEyeOutputViewport(EVREye eye, uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h) override
	{
		*x = eye == Eye_Left ? 0 : xr.eyeWidth;
		*y = 0;
		*w = xr.eyeWidth;
		*h = xr.eyeHeight;
	}
	void GetProjectionRaw(EVREye eye, float *left, float *right, float *top, float *bottom) override
	{
		// OpenVR tangents: y grows downwards, so "top" is negative for an upward angle
		const XrFovf &f = xr.fov[eye == Eye_Left ? 0 : 1];
		*left = tanf(f.angleLeft);
		*right = tanf(f.angleRight);
		*top = -tanf(f.angleUp);
		*bottom = -tanf(f.angleDown);
	}
	DistortionCoordinates_t ComputeDistortion(EVREye, float u, float v) override
	{
		DistortionCoordinates_t d;
		d.rfRed[0] = d.rfGreen[0] = d.rfBlue[0] = u;
		d.rfRed[1] = d.rfGreen[1] = d.rfBlue[1] = v;
		return d;
	}
	bool ComputeInverseDistortion(HmdVector2_t *result, EVREye, uint32_t, float u, float v) override
	{
		result->v[0] = u;
		result->v[1] = v;
		return true;
	}

	// IVRDriverDirectModeComponent
	void CreateSwapTextureSet(uint32_t pid, const SwapTextureSetDesc_t *desc, SwapTextureSet_t *out) override
	{
		if (!xr.AllocateShared(pid, *desc, *out))
			memset(out, 0, sizeof(*out));
	}
	void DestroySwapTextureSet(SharedTextureHandle_t h) override { xr.DestroyShared(h); }
	void DestroyAllSwapTextureSets(uint32_t pid) override { xr.DestroySharedForPid(pid); }
	void GetNextSwapTextureSetIndex(SharedTextureHandle_t handles[2], uint32_t (*indices)[2]) override
	{
		xr.NextIndices(handles, indices);
	}
	void SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) override { xr.Submit(perEye); }
	void Present(SharedTextureHandle_t) override { xr.Present(); }
	void PostPresent(const Throttling_t *) override { xr.WaitPresented(); }

private:
	XrBackend &xr;
	uint32_t id = k_unTrackedDeviceIndexInvalid;
	std::mutex poseMutex;
	DriverPose_t lastPose{};
	std::thread poseThread;
	std::atomic<bool> poseThreadRunning{false};
};

class Quest1Provider : public IServerTrackedDeviceProvider
{
public:
	EVRInitError Init(IVRDriverContext *ctx) override
	{
		VR_INIT_SERVER_DRIVER_CONTEXT(ctx);
		if (!xr.Init()) {
			Log("quest1: cannot reach Monado through OpenXR, no headset\n");
			xr.Shutdown();
			return VRInitError_Driver_Failed;
		}
		hmd = new Quest1Hmd(xr);
		if (!VRServerDriverHost()->TrackedDeviceAdded("QUEST1-SteamOS", TrackedDeviceClass_HMD, hmd))
			return VRInitError_Driver_Failed;
		return VRInitError_None;
	}
	void Cleanup() override
	{
		xr.Shutdown();
		delete hmd;
		hmd = nullptr;
		VR_CLEANUP_SERVER_DRIVER_CONTEXT();
	}
	const char *const *GetInterfaceVersions() override { return k_InterfaceVersions; }
	void RunFrame() override
	{
		VREvent_t e;
		while (VRServerDriverHost()->PollNextEvent(&e, sizeof(e))) {
		}
	}
	bool ShouldBlockStandbyMode() override { return false; }
	void EnterStandby() override {}
	void LeaveStandby() override {}

private:
	XrBackend xr;
	Quest1Hmd *hmd = nullptr;
};

static Quest1Provider g_provider;

extern "C" __attribute__((visibility("default"))) void *HmdDriverFactory(const char *name, int *ret)
{
	if (!strcmp(name, IServerTrackedDeviceProvider_Version))
		return &g_provider;
	if (ret)
		*ret = VRInitError_Init_InterfaceNotFound;
	return nullptr;
}
