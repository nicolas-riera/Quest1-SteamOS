// driver_quest1: SteamVR HMD driver for the Oculus Quest 1 running SteamOS natively.
//
// The headset is already driven by Monado (IMU fusion, lens distortion, panel scan-out, proximity
// sensor). This driver is an OpenXR client of Monado:
//   - poses: xrLocateSpace(VIEW in LOCAL) at 250 Hz -> TrackedDevicePoseUpdated
//   - frames: the HMD declares an identity distortion, so vrcompositor composites every layer into
//     an undistorted side-by-side image. vrcompositor always presents through a direct-mode
//     display, which the compat Vulkan layer simulates (src/vklayer-steamvr/quest1_display.c): its
//     swapchain images reach this driver over a unix socket as OPAQUE_FD memory, are imported into
//     the driver's Vulkan device, each half is blitted into an OpenXR swapchain and submitted as a
//     projection layer; Monado then distorts and scans out.
//     QUEST1_VIRTUAL_DISPLAY=1 uses IVRVirtualDisplay instead (backbuffer through
//     IVRIPCResourceManagerClient); vrcompositor 2.17 still needs a working direct-mode window then.
//   - pacing: a frame thread runs xrWaitFrame; its wake-ups are the "vsyncs" that
//     GetTimeSinceLastVsync reports, and WaitForPresent returns once a frame was handed to Monado.
//
// Runs inside vrserver, which must be started with the xr-run environment (Adreno Vulkan through
// libhybris: LD_LIBRARY_PATH=/opt/hybris/lib, LD_PRELOAD=libbionictls.so, XDG_RUNTIME_DIR=/run/monado).
// OpenXR and Vulkan are loaded with dlopen, so building needs headers only.
//
// Environment knobs while the hand-off is being verified on the device (docs/steamvr-port.md §8.5):
//   QUEST1_VD_FORMAT   VkFormat of the backbuffer (default 43 = R8G8B8A8_SRGB)
//   QUEST1_VD_WAITIDLE 1 = wait for the device to be idle before reading the backbuffer

#include <openvr_driver.h>

#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_TIMESPEC
#include <time.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <dlfcn.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "../vklayer-steamvr/quest1_display_proto.h"

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
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

static double MonotonicSeconds()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// --- dynamically loaded OpenXR and Vulkan ------------------------------------------------------

#define XR_FUNCS(X)                                                                                                    \
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
	X(vkDeviceWaitIdle)                                                                                            \
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
	X(vkBindImageMemory)                                                                                           	X(vkCreateBuffer)                                                                                              	X(vkGetBufferMemoryRequirements)                                                                               	X(vkBindBufferMemory)                                                                                          	X(vkMapMemory)                                                                                                 	X(vkCmdCopyImageToBuffer)

#define DECLARE(name) static PFN_##name p##name;
XR_FUNCS(DECLARE)
VK_FUNCS(DECLARE)
static PFN_xrGetInstanceProcAddr pxrGetInstanceProcAddr;
static PFN_xrCreateInstance pxrCreateInstance;
static PFN_vkGetInstanceProcAddr pvkGetInstanceProcAddr;

static bool LoadLibraries()
{
	void *xr = dlopen("libopenxr_loader.so.1", RTLD_NOW | RTLD_LOCAL);
	void *vk = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
	if (!xr || !vk) {
		Log("quest1: dlopen failed: %s (run vrserver with the xr-run environment)\n", dlerror());
		return false;
	}
	pxrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)dlsym(xr, "xrGetInstanceProcAddr");
	pvkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(vk, "vkGetInstanceProcAddr");
	if (!pxrGetInstanceProcAddr || !pvkGetInstanceProcAddr)
		return false;
	pxrGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction *)&pxrCreateInstance);
	return pxrCreateInstance != nullptr;
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

// p * (x, 0, 0): an eye position from the head pose and half the IPD
static XrPosef EyeFromHead(const XrPosef &head, float x)
{
	const XrQuaternionf &q = head.orientation;
	float rx = x * (1 - 2 * (q.y * q.y + q.z * q.z));
	float ry = x * (2 * (q.x * q.y + q.w * q.z));
	float rz = x * (2 * (q.x * q.z - q.w * q.y));
	XrPosef e = head;
	e.position = {head.position.x + rx, head.position.y + ry, head.position.z + rz};
	return e;
}

// --- OpenXR backend ----------------------------------------------------------------------------------

//! A vrcompositor backbuffer imported from its OPAQUE_FD memory.
struct Backbuffer
{
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
};

class XrBackend
{
public:
	bool Init();
	void Shutdown();

	// tracking: the pose only changes when vrcompositor presents a frame. At each present of
	// frame k, the display socket thread calls onPresent, which publishes a new pose (predicted, no
	// velocity, so SteamVR renders with exactly that pose) before vrcompositor resumes; frame k+1 is
	// therefore rendered with it, and the frame thread submits frame k+1 to Monado with that same
	// pose. Monado's reprojection then only corrects the latency, not a render/submit mismatch.
	bool LocateAhead(DriverPose_t &pose, XrPosef *xrPose);
	void RecordFramePose(uint64_t frame, const XrPosef &pose);
	std::function<void(uint64_t presentedFrame)> onPresent;
	std::atomic<double> lastPresent{0}; //!< MonotonicSeconds of the last display present
	float headHeight = 1.65f;    //!< a 3DoF head sits at this height above SteamVR's floor
	std::atomic<uint64_t> predictNs{30000000}; //!< pose publication -> photons through vrcompositor, the driver, Monado
	//! debug, from /tmp/quest1-pose-mode: 0 submit the render pose, 1 submit the display-time pose
	std::atomic<int> submitMode{0};
	//! 3DoF neck model: the eyes turn about the neck, NeckToEye above and in front of it
	std::atomic<bool> neckModel{true};
	XrFovf fov[2] = {};
	float ipd = 0.063f;
	uint32_t eyeWidth = 0, eyeHeight = 0;
	//! vrcompositor's output (the simulated display), both eyes side by side. Its distortion pass
	//! fills it every frame, so a smaller one costs much less GPU (2448x1360 -> 1224x680: 40 ->
	//! 60 fps); the blit to Monado's swapchains scales it back. QUEST1_DISPLAY_SIZE=WxH, also read
	//! by the simulated display in the compat layer: both must agree.
	uint32_t OutputWidth() const { return outputWidth ? outputWidth : eyeWidth * 2; }
	uint32_t OutputHeight() const { return outputHeight ? outputHeight : eyeHeight; }
	uint32_t outputWidth = 0, outputHeight = 0;
	float displayHz = 72.0f;
	// debug (QUEST1_POSE_TAG=1): frames are posed alternately straight ahead / looking down, and
	// three rows of each displayed frame are read back, to see which pose a frame was rendered with
	bool poseTag = false;
	static float PoseTagPitch(uint64_t frame)
	{
		static const float deg[5] = {0, 30, -30, 40, -40};
		return deg[frame % 5] / 57.2958f;
	}

	// virtual display
	void Present(SharedTextureHandle_t backbuffer);
	void WaitForPresent();
	bool TimeSinceLastVsync(float *seconds, uint64_t *counter);
	bool useVirtualDisplay = false;

private:
	bool InitVulkan();
	bool InitSession();
	bool CreateSwapchains();
	Backbuffer *Import(SharedTextureHandle_t handle);
	bool BlitHalves(const Backbuffer &bb, uint32_t srcWidth, uint32_t srcHeight, VkImageLayout srcLayout,
	                const uint32_t index[2]);
	// simulated display (quest1_display.c in vrcompositor)
	void DisplayThread();
	void ImportDisplay();
	void ReleaseDisplayImage(int index);
	void FrameThread();
	XrTime Now();

	XrInstance instance = XR_NULL_HANDLE;
	XrSystemId system = XR_NULL_SYSTEM_ID;
	XrSession session = XR_NULL_HANDLE;
	XrSpace local = XR_NULL_HANDLE, view = XR_NULL_HANDLE;
	XrSwapchain swapchain[2] = {};
	std::vector<XrSwapchainImageVulkan2KHR> images[2];
	VkFormat backbufferFormat = VK_FORMAT_R8G8B8A8_SRGB;
	bool waitIdle = false;
	VkBuffer probeBuf = VK_NULL_HANDLE;
	VkDeviceMemory probeMem = VK_NULL_HANDLE;
	uint8_t *probeMap = nullptr;
	uint32_t probeWidth = 0;
	bool probed = false;
	bool CreateProbe(uint32_t width);
	// debug: touch /tmp/quest1-snap and the next displayed frame lands in /tmp/quest1-snap.ppm
	VkBuffer snapBuf = VK_NULL_HANDLE;
	VkDeviceMemory snapMem = VK_NULL_HANDLE;
	uint8_t *snapMap = nullptr;
	uint32_t snapW = 0, snapH = 0;
	bool snapped = false;
	bool CreateHostBuffer(VkDeviceSize size, VkBuffer *buf, VkDeviceMemory *mem, uint8_t **map);
	void WriteSnapshot(VkFormat format);

	VkInstance vkInstance = VK_NULL_HANDLE;
	VkPhysicalDevice phys = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	uint32_t queueFamily = 0;
	VkQueue queue = VK_NULL_HANDLE;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;

	std::map<SharedTextureHandle_t, Backbuffer> backbuffers; //!< Only touched by the frame thread.

	// hand-off between Present() (vrserver's virtual display thread) and the frame thread
	std::mutex frameMutex;
	std::condition_variable frameCond;
	SharedTextureHandle_t pending = 0;
	uint64_t presentCount = 0, consumedCount = 0;
	double lastVsync = 0;
	uint64_t vsyncCount = 0;

	std::thread frameThread;
	std::atomic<bool> running{false};
	bool sessionRunning = false;

	// simulated display: the socket thread fills these under frameMutex, the frame thread imports
	std::thread displayThread;
	int listenFd = -1, clientFd = -1;
	qd_msg displayDesc{};
	int displayFds[QD_MAX_IMAGES] = {-1, -1, -1, -1};
	bool displayChanged = false;
	int presentedIndex = -1;
	uint64_t presentedFrame = 0;

	// the pose each display frame was rendered with (RecordFramePose), looked up by the frame thread
	struct PoseSample
	{
		uint64_t frame = 0;
		XrPosef pose{};
	};
	std::mutex poseRingMutex;
	PoseSample poseRing[64];
	bool PoseForFrame(uint64_t frame, XrPosef *pose);
	// frame thread only
	Backbuffer displayImages[QD_MAX_IMAGES];
	uint32_t displayCount = 0, displayWidth = 0, displayHeight = 0;
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
	if (const char *f = getenv("QUEST1_VD_FORMAT"))
		backbufferFormat = (VkFormat)atoi(f);
	if (const char *w = getenv("QUEST1_VD_WAITIDLE"))
		waitIdle = atoi(w) != 0;
	if (const char *s = getenv("QUEST1_DISPLAY_SIZE"))
		if (sscanf(s, "%ux%u", &outputWidth, &outputHeight) != 2)
			outputWidth = outputHeight = 0;
	if (const char *v = getenv("QUEST1_VIRTUAL_DISPLAY"))
		useVirtualDisplay = atoi(v) != 0;
	if (const char *h = getenv("QUEST1_EYE_HEIGHT"))
		headHeight = (float)atof(h);
	if (const char *t = getenv("QUEST1_POSE_TAG"))
		poseTag = atoi(t) != 0;
	if (const char *n = getenv("QUEST1_NECK_MODEL"))
		neckModel = atoi(n) != 0;
	if (const char *ms = getenv("QUEST1_POSE_PREDICT_MS"))
		predictNs = (uint64_t)(atof(ms) * 1e6);
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

	if (!InitVulkan() || !InitSession() || !CreateSwapchains())
		return false;

	// xrLocateViews is only valid once the session has begun: wait for READY here (the frame
	// thread handles the later state changes)
	for (int i = 0; i < 500 && !sessionRunning; i++) {
		XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
		while (!sessionRunning && pxrPollEvent(instance, &ev) == XR_SUCCESS) {
			if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED &&
			    ((XrEventDataSessionStateChanged *)&ev)->state == XR_SESSION_STATE_READY) {
				XrSessionBeginInfo sbi{XR_TYPE_SESSION_BEGIN_INFO};
				sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				XR_CHECK(pxrBeginSession(session, &sbi));
				sessionRunning = true;
			}
			ev = {XR_TYPE_EVENT_DATA_BUFFER};
		}
		if (!sessionRunning)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	if (!sessionRunning) {
		Log("quest1: the OpenXR session never became READY\n");
		return false;
	}

	// FOV and IPD from the views (fixed for this HMD)
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
	Log("quest1: eye %ux%u, ipd %.1f mm, fov L %.1f/%.1f/%.1f/%.1f deg, backbuffer VkFormat %d\n", eyeWidth,
	    eyeHeight, ipd * 1000, fov[0].angleLeft * 57.2958f, fov[0].angleRight * 57.2958f,
	    fov[0].angleUp * 57.2958f, fov[0].angleDown * 57.2958f, (int)backbufferFormat);

	running = true;
	frameThread = std::thread(&XrBackend::FrameThread, this);
	if (!useVirtualDisplay)
		displayThread = std::thread(&XrBackend::DisplayThread, this);
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

	// import of vrcompositor's backbuffers (OPAQUE_FD memory)
	const char *devExts[] = {
	    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
	    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
	    VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
	    VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
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

bool XrBackend::CreateSwapchains()
{
	int64_t formats[64];
	uint32_t n = 0;
	XR_CHECK(pxrEnumerateSwapchainFormats(session, 64, &n, formats));
	// a blit converts formats, but keep the sRGB-ness of the backbuffer so values are not re-encoded
	bool srgb = backbufferFormat == VK_FORMAT_R8G8B8A8_SRGB || backbufferFormat == VK_FORMAT_B8G8R8A8_SRGB;
	int64_t want = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, format = formats[0];
	for (uint32_t i = 0; i < n; i++)
		if (formats[i] == want)
			format = want;

	for (int eye = 0; eye < 2; eye++) {
		XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
		ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
		                XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
		ci.format = format;
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
	Log("quest1: swapchains %ux%u format %d\n", eyeWidth, eyeHeight, (int)format);
	return true;
}

bool XrBackend::LocateAhead(DriverPose_t &pose, XrPosef *xrPose)
{
	pose = {};
	pose.qWorldFromDriverRotation.w = pose.qDriverFromHeadRotation.w = 1;
	pose.vecWorldFromDriverTranslation[1] = headHeight;
	pose.qRotation.w = 1;
	pose.deviceIsConnected = true;
	pose.result = TrackingResult_Running_OK;

	XrTime when = Now() + (XrTime)predictNs;
	XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
	if (XR_FAILED(pxrLocateSpace(view, local, when, &loc)) ||
	    !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
		pose.result = TrackingResult_Running_OutOfRange;
		return false;
	}
	if (neckModel) {
		// Oculus SDK defaults: eyes 7.5 cm above and 8.05 cm in front of the neck pivot.
		// Level head = no offset, so headHeight stays the eye height.
		const XrVector3f n = {0, 0.075f, -0.0805f};
		const XrQuaternionf &q = loc.pose.orientation;
		XrVector3f t = {2 * (q.y * n.z - q.z * n.y), 2 * (q.z * n.x - q.x * n.z), 2 * (q.x * n.y - q.y * n.x)};
		XrVector3f r = {n.x + q.w * t.x + (q.y * t.z - q.z * t.y), n.y + q.w * t.y + (q.z * t.x - q.x * t.z),
		                n.z + q.w * t.z + (q.x * t.y - q.y * t.x)};
		loc.pose.position.x += r.x - n.x;
		loc.pose.position.y += r.y - n.y;
		loc.pose.position.z += r.z - n.z;
	}
	*xrPose = loc.pose;
	pose.qRotation = {loc.pose.orientation.w, loc.pose.orientation.x, loc.pose.orientation.y,
	                  loc.pose.orientation.z};
	pose.vecPosition[0] = loc.pose.position.x;
	pose.vecPosition[1] = loc.pose.position.y;
	pose.vecPosition[2] = loc.pose.position.z;
	// no velocities: SteamVR must not extrapolate, the pose is already predicted
	pose.poseIsValid = true;
	pose.willDriftInYaw = true; // 3DoF IMU, no magnetometer
	return true;
}

// The resource API gives an fd but no description: recreate the image as vrcompositor most
// likely did (display-sized color render target, optimal tiling, dedicated allocation).
Backbuffer *XrBackend::Import(SharedTextureHandle_t handle)
{
	auto it = backbuffers.find(handle);
	if (it != backbuffers.end())
		return &it->second;

	IVRIPCResourceManagerClient *rm = VRIPCResourceManager();
	uint64_t ipc = 0;
	int fd = -1;
	if (!rm || !rm->RefResource(handle, &ipc) || !rm->ReceiveSharedFd(ipc, &fd) || fd < 0) {
		Log("quest1: cannot get the fd of backbuffer %llu\n", (unsigned long long)handle);
		return nullptr;
	}

	Backbuffer bb;
	VkExternalMemoryImageCreateInfo emi{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
	emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.pNext = &emi;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = backbufferFormat;
	ici.extent = {eyeWidth * 2, eyeHeight, 1};
	ici.mipLevels = ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	            VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (pvkCreateImage(device, &ici, nullptr, &bb.image) != VK_SUCCESS) {
		close(fd);
		return nullptr;
	}
	VkMemoryRequirements mr;
	pvkGetImageMemoryRequirements(device, bb.image, &mr);
	VkPhysicalDeviceMemoryProperties mp;
	pvkGetPhysicalDeviceMemoryProperties(phys, &mp);
	uint32_t type = UINT32_MAX;
	for (uint32_t m = 0; m < mp.memoryTypeCount && type == UINT32_MAX; m++)
		if ((mr.memoryTypeBits & (1u << m)) && (mp.memoryTypes[m].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
			type = m;

	VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
	imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	imp.fd = fd; // owned by the driver once the import succeeds
	VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
	ded.pNext = &imp;
	ded.image = bb.image;
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.pNext = &ded;
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = type;
	VkResult r = pvkAllocateMemory(device, &mai, nullptr, &bb.memory);
	if (r != VK_SUCCESS || pvkBindImageMemory(device, bb.image, bb.memory, 0) != VK_SUCCESS) {
		Log("quest1: importing backbuffer %llu failed (%d, size %llu)\n", (unsigned long long)handle, (int)r,
		    (unsigned long long)mr.size);
		if (r != VK_SUCCESS)
			close(fd);
		pvkDestroyImage(device, bb.image, nullptr);
		return nullptr;
	}
	Log("quest1: imported backbuffer %llu (%ux%u, %llu bytes)\n", (unsigned long long)handle, eyeWidth * 2,
	    eyeHeight, (unsigned long long)mr.size);
	return &(backbuffers[handle] = bb);
}

bool XrBackend::BlitHalves(const Backbuffer &bb, uint32_t srcWidth, uint32_t srcHeight, VkImageLayout srcLayout,
                           const uint32_t index[2])
{
	if (waitIdle)
		pvkDeviceWaitIdle(device);
	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VK_CHECK(pvkResetCommandBuffer(cmd, 0));
	VK_CHECK(pvkBeginCommandBuffer(cmd, &bi));

	VkImageMemoryBarrier bar[3];
	for (auto &b : bar) {
		b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	}
	// source: written by vrcompositor (another process, external queue family). A display image
	// arrives in TRANSFER_SRC_OPTIMAL; for the virtual-display backbuffer the layout is unknown.
	bar[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	bar[0].oldLayout = srcLayout;
	bar[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	bar[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
	bar[0].dstQueueFamilyIndex = queueFamily;
	bar[0].image = bb.image;
	for (int eye = 0; eye < 2; eye++) {
		bar[1 + eye].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		bar[1 + eye].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		bar[1 + eye].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		bar[1 + eye].image = images[eye][index[eye]].image;
	}
	pvkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
	                      nullptr, 3, bar);

	bool scaled = srcWidth != eyeWidth * 2 || srcHeight != eyeHeight;
	for (int eye = 0; eye < 2; eye++) {
		VkImageBlit blit{};
		blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		blit.srcOffsets[0] = {(int32_t)(eye * srcWidth / 2), 0, 0};
		blit.srcOffsets[1] = {(int32_t)((eye + 1) * srcWidth / 2), (int32_t)srcHeight, 1};
		blit.dstOffsets[1] = {(int32_t)eyeWidth, (int32_t)eyeHeight, 1};
		pvkCmdBlitImage(cmd, bb.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, images[eye][index[eye]].image,
		                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, scaled ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
	}
	probed = false;
	if (poseTag && srcLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && CreateProbe(srcHeight)) {
		// in each eye, the columns at horizontal tangents -0.6 and +0.6
		VkBufferImageCopy rc[4] = {};
		for (int i = 0; i < 4; i++) {
			int eye = i / 2;
			float l = tanf(-fov[eye].angleLeft), r = tanf(fov[eye].angleRight);
			float tx = i % 2 ? 0.6f : -0.6f;
			rc[i].bufferOffset = (VkDeviceSize)i * srcHeight * 4;
			rc[i].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			rc[i].imageOffset = {(int32_t)(srcWidth / 2 * (eye + (tx + l) / (l + r))), 0, 0};
			rc[i].imageExtent = {1, srcHeight, 1};
		}
		pvkCmdCopyImageToBuffer(cmd, bb.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, probeBuf, 4, rc);
		VkMemoryBarrier hb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
		hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		pvkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, nullptr,
		                      0, nullptr);
		probed = true;
	}
	snapped = false;
	if (srcLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && access("/tmp/quest1-snap", F_OK) == 0) {
		if (snapW != srcWidth || snapH != srcHeight) {
			snapMap = nullptr;
			if (CreateHostBuffer((VkDeviceSize)srcWidth * srcHeight * 4, &snapBuf, &snapMem, &snapMap))
				snapW = srcWidth, snapH = srcHeight;
		}
		if (snapMap) {
			VkBufferImageCopy rc{};
			rc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			rc.imageExtent = {srcWidth, srcHeight, 1};
			pvkCmdCopyImageToBuffer(cmd, bb.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, snapBuf, 1, &rc);
			VkMemoryBarrier hb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
			hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
			pvkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0,
			                      nullptr, 0, nullptr);
			snapped = true;
		}
	}

	// backbuffer back to vrcompositor, swapchain images to the OpenXR runtime
	bar[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	bar[0].dstAccessMask = 0;
	bar[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	bar[0].newLayout = srcLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_GENERAL : srcLayout;
	bar[0].srcQueueFamilyIndex = queueFamily;
	bar[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
	for (int eye = 0; eye < 2; eye++) {
		bar[1 + eye].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		bar[1 + eye].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
		bar[1 + eye].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		bar[1 + eye].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}
	pvkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
	                      nullptr, 3, bar);
	VK_CHECK(pvkEndCommandBuffer(cmd));
	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	VK_CHECK(pvkQueueSubmit(queue, 1, &si, fence));
	// the Adreno blob returns VK_TIMEOUT at once for UINT64_MAX (overflow): wait in finite steps
	VkResult r;
	while ((r = pvkWaitForFences(device, 1, &fence, VK_TRUE, 100000000ull)) == VK_TIMEOUT)
		;
	pvkResetFences(device, 1, &fence);
	return r == VK_SUCCESS;
}

bool XrBackend::CreateHostBuffer(VkDeviceSize size, VkBuffer *buf, VkDeviceMemory *mem, uint8_t **map)
{
	VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = size;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	if (pvkCreateBuffer(device, &bci, nullptr, buf) != VK_SUCCESS)
		return false;
	VkMemoryRequirements mr{};
	pvkGetBufferMemoryRequirements(device, *buf, &mr);
	VkPhysicalDeviceMemoryProperties mp{};
	pvkGetPhysicalDeviceMemoryProperties(phys, &mp);
	const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	uint32_t type = UINT32_MAX;
	for (uint32_t t = 0; t < mp.memoryTypeCount && type == UINT32_MAX; t++)
		if ((mr.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & want) == want)
			type = t;
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = type;
	void *p = nullptr;
	if (type == UINT32_MAX || pvkAllocateMemory(device, &mai, nullptr, mem) != VK_SUCCESS ||
	    pvkBindBufferMemory(device, *buf, *mem, 0) != VK_SUCCESS ||
	    pvkMapMemory(device, *mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
		Log("quest1: cannot create a host-visible buffer of %llu bytes\n", (unsigned long long)size);
		return false;
	}
	*map = (uint8_t *)p;
	return true;
}

//! The snapshot as a binary PPM (RGB), then the trigger file is removed.
void XrBackend::WriteSnapshot(VkFormat format)
{
	bool bgr = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
	FILE *f = fopen("/tmp/quest1-snap.ppm.tmp", "wb");
	if (f) {
		fprintf(f, "P6\n%u %u\n255\n", snapW, snapH);
		std::vector<uint8_t> row(snapW * 3);
		for (uint32_t y = 0; y < snapH; y++) {
			const uint8_t *s = snapMap + (size_t)y * snapW * 4;
			for (uint32_t x = 0; x < snapW; x++) {
				row[x * 3 + 0] = s[x * 4 + (bgr ? 2 : 0)];
				row[x * 3 + 1] = s[x * 4 + 1];
				row[x * 3 + 2] = s[x * 4 + (bgr ? 0 : 2)];
			}
			fwrite(row.data(), 1, row.size(), f);
		}
		fclose(f);
		rename("/tmp/quest1-snap.ppm.tmp", "/tmp/quest1-snap.ppm");
	}
	unlink("/tmp/quest1-snap");
	Log("quest1: snapshot %ux%u written to /tmp/quest1-snap.ppm\n", snapW, snapH);
}

bool XrBackend::CreateProbe(uint32_t width)
{
	if (probeMap)
		return width == probeWidth;
	VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = (VkDeviceSize)width * 4 * 4;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	if (pvkCreateBuffer(device, &bci, nullptr, &probeBuf) != VK_SUCCESS)
		return false;
	VkMemoryRequirements mr{};
	pvkGetBufferMemoryRequirements(device, probeBuf, &mr);
	VkPhysicalDeviceMemoryProperties mp{};
	pvkGetPhysicalDeviceMemoryProperties(phys, &mp);
	const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	uint32_t type = UINT32_MAX;
	for (uint32_t t = 0; t < mp.memoryTypeCount && type == UINT32_MAX; t++)
		if ((mr.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & want) == want)
			type = t;
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = type;
	void *map = nullptr;
	if (type == UINT32_MAX || pvkAllocateMemory(device, &mai, nullptr, &probeMem) != VK_SUCCESS ||
	    pvkBindBufferMemory(device, probeBuf, probeMem, 0) != VK_SUCCESS ||
	    pvkMapMemory(device, probeMem, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS) {
		Log("quest1: cannot create the pose probe buffer\n");
		poseTag = false;
		return false;
	}
	probeMap = (uint8_t *)map;
	probeWidth = width;
	return true;
}

void XrBackend::Present(SharedTextureHandle_t backbuffer)
{
	std::lock_guard<std::mutex> lock(frameMutex);
	pending = backbuffer;
	presentCount++;
	frameCond.notify_all();
}

void XrBackend::RecordFramePose(uint64_t frame, const XrPosef &pose)
{
	std::lock_guard<std::mutex> lock(poseRingMutex);
	poseRing[frame % 64] = {frame, pose};
}

bool XrBackend::PoseForFrame(uint64_t frame, XrPosef *pose)
{
	std::lock_guard<std::mutex> lock(poseRingMutex);
	const PoseSample &slot = poseRing[frame % 64];
	if (frame == 0 || slot.frame != frame)
		return false;
	*pose = slot.pose;
	return true;
}

// --- simulated display receiver ---------------------------------------------------------------------------

void XrBackend::DisplayThread()
{
	pthread_setname_np(pthread_self(), "quest1 display");
	char path[108];
	if (const char *p = getenv("QUEST1_DISPLAY_SOCKET"))
		snprintf(path, sizeof(path), "%s", p);
	else
		snprintf(path, sizeof(path), "%s/quest1-display.sock",
		         getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "/tmp");
	listenFd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	sockaddr_un a{};
	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
	unlink(path);
	if (listenFd < 0 || bind(listenFd, (sockaddr *)&a, sizeof(a)) != 0 || listen(listenFd, 1) != 0) {
		Log("quest1: cannot listen on %s: %s\n", path, strerror(errno));
		return;
	}
	Log("quest1: display receiver listening on %s\n", path);

	while (running) {
		int fd = accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC);
		if (fd < 0)
			break; // Shutdown() closed the socket
		{
			std::lock_guard<std::mutex> lock(frameMutex);
			clientFd = fd;
		}
		Log("quest1: vrcompositor display connected\n");
		for (;;) {
			qd_msg m;
			char ctrl[CMSG_SPACE(sizeof(int) * QD_MAX_IMAGES)];
			iovec iov{&m, sizeof(m)};
			msghdr h{};
			h.msg_iov = &iov;
			h.msg_iovlen = 1;
			h.msg_control = ctrl;
			h.msg_controllen = sizeof(ctrl);
			ssize_t n = recvmsg(fd, &h, MSG_CMSG_CLOEXEC);
			if (n != (ssize_t)sizeof(m))
				break;
			std::unique_lock<std::mutex> lock(frameMutex);
			if (m.type == QD_SWAPCHAIN) {
				cmsghdr *c = CMSG_FIRSTHDR(&h);
				int nfd = c && c->cmsg_type == SCM_RIGHTS ? (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int)) : 0;
				for (int &old : displayFds)
					if (old >= 0) {
						close(old);
						old = -1;
					}
				if (m.count > QD_MAX_IMAGES || nfd != (int)m.count) {
					Log("quest1: bad display swapchain message (%u images, %d fds)\n", m.count, nfd);
					continue;
				}
				memcpy(displayFds, CMSG_DATA(c), sizeof(int) * nfd);
				displayDesc = m;
				displayChanged = true;
				presentedIndex = -1;
			} else if (m.type == QD_PRESENT && m.index < QD_MAX_IMAGES) {
				// a newer frame replaces one the frame thread has not taken yet
				if (presentedIndex >= 0) {
					qd_msg r{};
					r.type = QD_RELEASE;
					r.index = presentedIndex;
					send(fd, &r, sizeof(r), MSG_NOSIGNAL);
				}
				presentedIndex = (int)m.index;
				presentedFrame = m.frame;
				presentCount++;
				frameCond.notify_all();
				lock.unlock();
				if (m.flags & QD_PRESENT_POSED)
					continue; // the pose was published at its QD_POSE
				// the pose of the next frame, published before vrcompositor resumes (it waits for this)
				lastPresent = MonotonicSeconds();
				if (onPresent)
					onPresent(m.frame);
				qd_msg ack{};
				ack.type = QD_POSED;
				ack.frame = m.frame;
				send(fd, &ack, sizeof(ack), MSG_NOSIGNAL);
			} else if (m.type == QD_POSE) {
				// frame m.frame is submitted (still rendering): publish the next frame's pose now
				lock.unlock();
				lastPresent = MonotonicSeconds();
				if (onPresent)
					onPresent(m.frame);
				qd_msg ack{};
				ack.type = QD_POSED;
				ack.frame = m.frame;
				send(fd, &ack, sizeof(ack), MSG_NOSIGNAL);
			}
		}
		std::lock_guard<std::mutex> lock(frameMutex);
		close(fd);
		clientFd = -1;
		presentedIndex = -1;
		Log("quest1: vrcompositor display disconnected\n");
	}
}

void XrBackend::ReleaseDisplayImage(int index)
{
	std::lock_guard<std::mutex> lock(frameMutex);
	if (clientFd < 0 || index < 0)
		return;
	qd_msg r{};
	r.type = QD_RELEASE;
	r.index = (uint32_t)index;
	send(clientFd, &r, sizeof(r), MSG_NOSIGNAL);
}

// Frame thread: (re)import the images of a new display swapchain. The fds are taken under frameMutex.
void XrBackend::ImportDisplay()
{
	qd_msg m;
	int fds[QD_MAX_IMAGES];
	{
		std::lock_guard<std::mutex> lock(frameMutex);
		m = displayDesc;
		memcpy(fds, displayFds, sizeof(fds));
		for (int &fd : displayFds)
			fd = -1;
		displayChanged = false;
	}
	pvkDeviceWaitIdle(device);
	for (uint32_t i = 0; i < displayCount; i++) {
		pvkDestroyImage(device, displayImages[i].image, nullptr);
		pvkFreeMemory(device, displayImages[i].memory, nullptr);
		displayImages[i] = {};
	}
	displayCount = 0;

	VkPhysicalDeviceMemoryProperties mp;
	pvkGetPhysicalDeviceMemoryProperties(phys, &mp);
	VkFormat viewFormats[4];
	for (uint32_t i = 0; i < m.view_format_count && i < 4; i++)
		viewFormats[i] = (VkFormat)m.view_formats[i];
	for (uint32_t i = 0; i < m.count; i++) {
		// the same description as the exporting image (quest1_display.c)
		VkImageFormatListCreateInfo fl{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
		fl.viewFormatCount = m.view_format_count;
		fl.pViewFormats = viewFormats;
		VkExternalMemoryImageCreateInfo emi{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
		emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
		emi.pNext = m.view_format_count ? &fl : nullptr;
		VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ici.pNext = &emi;
		ici.flags = m.flags;
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = (VkFormat)m.format;
		ici.extent = {m.width, m.height, 1};
		ici.mipLevels = ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = m.usage;
		ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		Backbuffer &bb = displayImages[i];
		VkResult r = pvkCreateImage(device, &ici, nullptr, &bb.image);
		VkMemoryRequirements mr{};
		if (r == VK_SUCCESS)
			pvkGetImageMemoryRequirements(device, bb.image, &mr);
		uint32_t type = 0;
		for (uint32_t t = 0; t < mp.memoryTypeCount; t++)
			if ((mr.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
				type = t;
				break;
			}
		VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
		imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
		imp.fd = fds[i];
		VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
		ded.pNext = &imp;
		ded.image = bb.image;
		VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		mai.pNext = &ded;
		mai.allocationSize = m.size[i];
		mai.memoryTypeIndex = type;
		if (r == VK_SUCCESS)
			r = pvkAllocateMemory(device, &mai, nullptr, &bb.memory);
		if (r == VK_SUCCESS) {
			fds[i] = -1; // owned by the memory object now
			r = pvkBindImageMemory(device, bb.image, bb.memory, 0);
		}
		if (r != VK_SUCCESS) {
			Log("quest1: importing display image %u failed (%d)\n", i, (int)r);
			for (uint32_t k = i; k < m.count; k++)
				if (fds[k] >= 0)
					close(fds[k]);
			if (bb.memory)
				pvkFreeMemory(device, bb.memory, nullptr);
			if (bb.image)
				pvkDestroyImage(device, bb.image, nullptr);
			bb = {};
			displayCount = i;
			return;
		}
	}
	displayCount = m.count;
	displayWidth = m.width;
	displayHeight = m.height;
	Log("quest1: display swapchain imported: %u images %ux%u format %u\n", m.count, m.width, m.height, m.format);
}

void XrBackend::WaitForPresent()
{
	std::unique_lock<std::mutex> lock(frameMutex);
	uint64_t want = presentCount;
	frameCond.wait_for(lock, std::chrono::milliseconds(50), [&] { return consumedCount >= want || !running; });
}

bool XrBackend::TimeSinceLastVsync(float *seconds, uint64_t *counter)
{
	std::lock_guard<std::mutex> lock(frameMutex);
	if (vsyncCount == 0)
		return false;
	*seconds = (float)(MonotonicSeconds() - lastVsync);
	*counter = vsyncCount;
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
	uint64_t shown = 0, poseMisses = 0, totalMissLogs = 0;
	double statsSince = MonotonicSeconds();

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
		{
			std::lock_guard<std::mutex> lock(frameMutex);
			lastVsync = MonotonicSeconds();
			vsyncCount++;
		}
		pxrBeginFrame(session, nullptr);

		// SteamVR paces itself on our "vsyncs"; give its frame most of the period to arrive.
		SharedTextureHandle_t handle = 0;
		int displayIndex = -1;
		uint64_t displayFrame = 0;
		bool importDisplay = false;
		{
			std::unique_lock<std::mutex> lock(frameMutex);
			auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(8);
			frameCond.wait_until(lock, deadline,
			                     [&] { return pending != 0 || presentedIndex >= 0 || displayChanged || !running; });
			handle = pending;
			pending = 0;
			displayIndex = presentedIndex;
			displayFrame = presentedFrame;
			presentedIndex = -1;
			importDisplay = displayChanged;
		}
		if (importDisplay)
			ImportDisplay();

		Backbuffer *bb = nullptr;
		uint32_t srcW = eyeWidth * 2, srcH = eyeHeight;
		VkImageLayout srcLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if (displayIndex >= 0 && (uint32_t)displayIndex < displayCount) {
			bb = &displayImages[displayIndex];
			srcW = displayWidth;
			srcH = displayHeight;
			srcLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		} else if (handle) {
			bb = Import(handle);
		}
		if (bb && fs.shouldRender) {
			uint32_t idx[2];
			XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			wi.timeout = XR_INFINITE_DURATION;
			for (int eye = 0; eye < 2; eye++) {
				pxrAcquireSwapchainImage(swapchain[eye], nullptr, &idx[eye]);
				pxrWaitSwapchainImage(swapchain[eye], &wi);
			}
			BlitHalves(*bb, srcW, srcH, srcLayout, idx);
			if (snapped)
				WriteSnapshot((VkFormat)displayDesc.format);
			if (probed) {
				// brightest row of each probed column
				uint32_t bestRow[4] = {};
				for (int i = 0; i < 4; i++) {
					uint32_t best = 0;
					for (uint32_t y = 0; y < probeWidth; y++) {
						const uint8_t *px = probeMap + ((size_t)i * probeWidth + y) * 4;
						uint32_t v = px[0] + px[1] + px[2];
						if (v > best)
							best = v, bestRow[i] = y;
					}
				}
				Log("quest1: probe frame %llu pitch %+.0f: rows L %u %u R %u %u\n", (unsigned long long)displayFrame,
				    PoseTagPitch(displayFrame) * 57.2958f, bestRow[0], bestRow[1], bestRow[2], bestRow[3]);
			}
			for (int eye = 0; eye < 2; eye++)
				pxrReleaseSwapchainImage(swapchain[eye], nullptr);

			// the pose SteamVR rendered this frame with (published when the previous one was
			// presented); otherwise (virtual display, first frame) this frame's own
			XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
			if (displayIndex < 0 || submitMode == 1 || !PoseForFrame(displayFrame, &head.pose)) {
				pxrLocateSpace(view, local, fs.predictedDisplayTime, &head);
				if (displayIndex >= 0 && submitMode != 1 && poseMisses++ < 3 && totalMissLogs++ < 6)
					Log("quest1: no recorded pose for display frame %llu\n", (unsigned long long)displayFrame);
			}
			for (int eye = 0; eye < 2; eye++) {
				pv[eye].pose = EyeFromHead(head.pose, eye == 0 ? -ipd / 2 : ipd / 2);
				pv[eye].fov = fov[eye];
				pv[eye].subImage.swapchain = swapchain[eye];
				pv[eye].subImage.imageRect = {{0, 0}, {(int32_t)eyeWidth, (int32_t)eyeHeight}};
			}
			haveLayer = true;
			shown++;
		}
		ReleaseDisplayImage(displayIndex); // the blit waited for its fence
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

		double now = MonotonicSeconds();
		if (now - statsSince > 5.0) {
			Log("quest1: %.1f SteamVR frames/s shown, %llu without their render pose\n", shown / (now - statsSince),
			    (unsigned long long)poseMisses);
			shown = poseMisses = 0;
			statsSince = now;
		}
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
	if (listenFd >= 0) {
		shutdown(listenFd, SHUT_RDWR);
		close(listenFd);
	}
	{
		std::lock_guard<std::mutex> lock(frameMutex);
		if (clientFd >= 0)
			shutdown(clientFd, SHUT_RDWR);
	}
	if (displayThread.joinable())
		displayThread.join();
	for (uint32_t i = 0; i < displayCount; i++) {
		pvkDestroyImage(device, displayImages[i].image, nullptr);
		pvkFreeMemory(device, displayImages[i].memory, nullptr);
	}
	displayCount = 0;
	IVRIPCResourceManagerClient *rm = VRIPCResourceManager();
	for (auto &kv : backbuffers) {
		pvkDestroyImage(device, kv.second.image, nullptr);
		pvkFreeMemory(device, kv.second.memory, nullptr);
		if (rm)
			rm->UnrefResource(kv.first);
	}
	backbuffers.clear();
	if (session != XR_NULL_HANDLE)
		pxrDestroySession(session);
	if (instance != XR_NULL_HANDLE)
		pxrDestroyInstance(instance);
	session = XR_NULL_HANDLE;
	instance = XR_NULL_HANDLE;
}

// --- SteamVR device ----------------------------------------------------------------------------------------

class Quest1Hmd : public ITrackedDeviceServerDriver, public IVRDisplayComponent, public IVRVirtualDisplay
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
		VRProperties()->SetStringProperty(c, Prop_DriverVersion_String, "0.2");
		VRProperties()->SetFloatProperty(c, Prop_DisplayFrequency_Float, xr.displayHz);
		VRProperties()->SetFloatProperty(c, Prop_UserIpdMeters_Float, xr.ipd);
		VRProperties()->SetFloatProperty(c, Prop_UserHeadToEyeDepthMeters_Float, 0.0f);
		// Monado wakes the frame thread ~15 ms before the vsync that latches the frame, which is
		// shown half a period later.
		VRProperties()->SetFloatProperty(c, Prop_SecondsFromVsyncToPhotons_Float, 0.022f);
		VRProperties()->SetUint64Property(c, Prop_CurrentUniverseId_Uint64, 2);
		VRProperties()->SetBoolProperty(c, Prop_IsOnDesktop_Bool, false);
		VRProperties()->SetBoolProperty(c, Prop_DisplayDebugMode_Bool, false);
		VRProperties()->SetBoolProperty(c, Prop_DeviceProvidesBatteryStatus_Bool, false);

		poseThreadRunning = true;
		poseThread = std::thread([this] {
			pthread_setname_np(pthread_self(), "quest1 poses");
			// while vrcompositor presents, poses are published by PublishFramePose only; before
			// that (start-up, virtual display) at 250 Hz
			for (unsigned tick = 0; poseThreadRunning; tick++) {
				std::this_thread::sleep_for(std::chrono::milliseconds(4));
				if (tick % 128 == 0)
					ReadDebugMode();
				if (MonotonicSeconds() - xr.lastPresent < 0.2)
					continue;
				XrPosef unused;
				DriverPose_t p;
				xr.LocateAhead(p, &unused);
				Publish(p);
			}
		});
		xr.onPresent = [this](uint64_t presentedFrame) { PublishFramePose(presentedFrame + 1); };
		return VRInitError_None;
	}

	//! debug: "<submit mode> <prediction ms> <neck model 0/1>" in /tmp/quest1-pose-mode, applied at once
	void ReadDebugMode()
	{
		FILE *f = fopen("/tmp/quest1-pose-mode", "r");
		if (!f)
			return;
		int mode = 0, neck = 1;
		float ms = 30;
		if (fscanf(f, "%d %f %d", &mode, &ms, &neck) >= 1) {
			uint64_t ns = (uint64_t)(ms * 1e6);
			if (mode != xr.submitMode || ns != xr.predictNs || (neck != 0) != xr.neckModel)
				Log("quest1: debug mode: submit %s pose, prediction %.0f ms, neck model %s\n",
				    mode ? "display-time" : "render", ms, neck ? "on" : "off");
			xr.submitMode = mode;
			xr.predictNs = ns;
			xr.neckModel = neck != 0;
		}
		fclose(f);
	}

	void Publish(const DriverPose_t &p)
	{
		std::lock_guard<std::mutex> lock(poseMutex);
		lastPose = p;
		VRServerDriverHost()->TrackedDevicePoseUpdated(id, p, sizeof(p));
	}

	//! The pose display frame `frame` will be rendered with.
	void PublishFramePose(uint64_t frame)
	{
		XrPosef xrPose;
		DriverPose_t p;
		if (xr.LocateAhead(p, &xrPose)) {
			if (xr.poseTag) {
				float half = XrBackend::PoseTagPitch(frame) / 2; // a pitch
				xrPose.orientation = {sinf(half), 0, 0, cosf(half)};
				p.qRotation = {cosf(half), sinf(half), 0, 0};
			}
			xr.RecordFramePose(frame, xrPose);
		}
		Publish(p);
	}

	void Deactivate() override
	{
		xr.onPresent = nullptr;
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
		if (!strcmp(name, IVRVirtualDisplay_Version) && xr.useVirtualDisplay)
			return static_cast<IVRVirtualDisplay *>(this);
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

	// IVRDisplayComponent: the (simulated) direct-mode display; Monado applies the lens distortion
	void GetWindowBounds(int32_t *x, int32_t *y, uint32_t *w, uint32_t *h) override
	{
		*x = *y = 0;
		*w = xr.OutputWidth();
		*h = xr.OutputHeight();
	}
	bool IsDisplayOnDesktop() override { return false; }
	bool IsDisplayRealDisplay() override { return !xr.useVirtualDisplay; }
	void GetRecommendedRenderTargetSize(uint32_t *w, uint32_t *h) override
	{
		*w = xr.eyeWidth;
		*h = xr.eyeHeight;
	}
	void GetEyeOutputViewport(EVREye eye, uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h) override
	{
		*x = eye == Eye_Left ? 0 : xr.OutputWidth() / 2;
		*y = 0;
		*w = xr.OutputWidth() / 2;
		*h = xr.OutputHeight();
	}
	void GetProjectionRaw(EVREye eye, float *left, float *right, float *top, float *bottom) override
	{
		// vrcompositor's display output puts "top" at the bottom of the eye viewport: with
		// top = -tan(up), the horizon was measured ~130 rows (0.23 tan) too low on the 47° up /
		// 53° down Quest optics, which warps the world as the head turns. So top gets the lower
		// tangent (negative) and bottom the upper one, as in OpenXR.
		const XrFovf &f = xr.fov[eye == Eye_Left ? 0 : 1];
		*left = tanf(f.angleLeft);
		*right = tanf(f.angleRight);
		*top = tanf(f.angleDown);
		*bottom = tanf(f.angleUp);
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

	// IVRVirtualDisplay
	void Present(const PresentInfo_t *info, uint32_t size) override
	{
		if (size >= sizeof(SharedTextureHandle_t) && info->backbufferTextureHandle)
			xr.Present(info->backbufferTextureHandle);
	}
	void WaitForPresent() override { xr.WaitForPresent(); }
	bool GetTimeSinceLastVsync(float *seconds, uint64_t *counter) override
	{
		return xr.TimeSinceLastVsync(seconds, counter);
	}

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
