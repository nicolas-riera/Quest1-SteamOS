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
#include "battery.h"
#include "camera.h"
#include "chaperone_frame.h"
#include "controllers.h"

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
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

//! The IPD set with the Quest 1's slider, in mm, 0 if unknown. The slider moves each panel together
//! with its lens, so it only changes the eye separation, not the distortion. The slider is a
//! potentiometer on the PMIC's ADC; as Meta's IAD sensor HAL: voltage -> resistance (9.1 kOhm
//! divider from 3.3 V), interpolated between the factory points /persist/IPD_{MIN,MAX} (mV) and
//! /persist/IAD_{MIN,MAX} (mm), plus 54.6 mm. QUEST1_IPD_MM overrides it.
static float SliderIpdMm()
{
	if (const char *env = getenv("QUEST1_IPD_MM"))
		return (float)atof(env);
	static float rMin = -1, rMax, iadMin, iadMax;
	auto readf = [](const char *path, float *v) {
		FILE *f = fopen(path, "r");
		bool ok = f && fscanf(f, "%f", v) == 1;
		if (f)
			fclose(f);
		return ok;
	};
	auto ohms = [](float volts) { return volts * 9.1f / (3.3f - volts); };
	if (rMin < 0) {
		float mvMin, mvMax;
		if (!readf("/persist/IPD_MIN", &mvMin) || !readf("/persist/IPD_MAX", &mvMax) ||
		    !readf("/persist/IAD_MIN", &iadMin) || !readf("/persist/IAD_MAX", &iadMax) || mvMax <= mvMin) {
			rMin = 0, rMax = 0;
			return 0;
		}
		rMin = ohms(mvMin / 1000), rMax = ohms(mvMax / 1000);
	}
	if (rMax <= rMin)
		return 0;
	FILE *f = fopen("/sys/devices/soc/800f000.qcom,spmi/spmi-0/spmi0-00/"
	                "800f000.qcom,spmi:qcom,pm8998@0:vadc@3100/ipd_sensor", "r");
	long uv = 0;
	bool ok = f && fscanf(f, "Result:%ld", &uv) == 1;
	if (f)
		fclose(f);
	if (!ok)
		return 0;
	float r = std::min(std::max(ohms(uv * 1e-6f), rMin), rMax);
	return iadMin + (iadMax - iadMin) * (r - rMin) / (rMax - rMin) + 54.6f;
}

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
	//! what the Touch controllers need of the session (controllers.cpp)
	ControllerXr ControllerContext()
	{
		return {pxrGetInstanceProcAddr, instance, session, local, [this] { return Now(); }, headHeight};
	}

	// tracking: the pose only changes when vrcompositor presents a frame. At each present of
	// frame k, the display socket thread calls onPresent, which publishes a new pose (predicted, no
	// velocity, so SteamVR renders with exactly that pose) before vrcompositor resumes; frame k+1 is
	// therefore rendered with it, and the frame thread submits frame k+1 to Monado with that same
	// pose. Monado's reprojection then only corrects the latency, not a render/submit mismatch.
	bool LocateAhead(DriverPose_t &pose, XrPosef *xrPose);
	//! The head's pose in SteamVR's raw space (as the HMD pose: floor and anchor applied) at a CLOCK_MONOTONIC time
	bool LocateRawAt(int64_t monoNs, HmdMatrix34_t &raw);
	void ApplyNeckModel(XrSpaceLocation &loc);
	void HoldPosition(XrSpaceLocation &loc);
	std::mutex holdMutex;
	XrVector3f lastTrackedPos = {};
	bool haveTrackedPos = false;
	int64_t lastWhenNs = 0; //!< CLOCK_MONOTONIC time of the last LocateAhead's prediction (same thread as its caller)
	void RecordFramePose(uint64_t frame, const XrPosef &pose);
	std::function<void(uint64_t presentedFrame)> onPresent;
	std::atomic<double> lastPresent{0}; //!< MonotonicSeconds of the last display present
	float headHeight = 1.65f;    //!< a 3DoF head sits at this height above SteamVR's floor
	//! pose publication -> photons through vrcompositor, the layer and Monado: adjusted to what Monado measures
	//! (ReadPoseAge), unless fixed by QUEST1_POSE_PREDICT_MS or /tmp/quest1-pose-mode
	std::atomic<uint64_t> predictNs{50000000};
	std::atomic<bool> predictFixed{false};
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
	//! QUEST1_DIRECT_SCANOUT=1: vrcompositor distorts for the lenses itself and the Vulkan layer puts
	//! its 2880x1600 output straight on the panels (quest1_scanout.c); Monado only tracks.
	bool directScanout = false;
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
		int64_t whenNs = 0;
	};
	std::mutex poseRingMutex;
	PoseSample poseRing[64];
	bool PoseForFrame(uint64_t frame, XrPosef *pose, int64_t *whenNs = nullptr);
	void FillPosed(qd_msg &ack);
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
	if (const char *ds = getenv("QUEST1_DIRECT_SCANOUT"))
		directScanout = atoi(ds) == 1;
	if (const char *v = getenv("QUEST1_VIRTUAL_DISPLAY"))
		useVirtualDisplay = atoi(v) != 0;
	if (const char *h = getenv("QUEST1_EYE_HEIGHT"))
		headHeight = (float)atof(h);
	if (const char *t = getenv("QUEST1_POSE_TAG"))
		poseTag = atoi(t) != 0;
	if (const char *n = getenv("QUEST1_NECK_MODEL"))
		neckModel = atoi(n) != 0;
	if (const char *ms = getenv("QUEST1_POSE_PREDICT_MS")) {
		predictNs = (uint64_t)(atof(ms) * 1e6);
		predictFixed = true;
	}
	Quest1FrameInit(headHeight);
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
	if (float mm = SliderIpdMm(); mm > 55.0f && mm < 75.0f) // the slider is set to the user's IPD
		ipd = mm / 1000.0f;
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
	Quest1FrameApply(pose); // raw space: floor and anchor (chaperone_frame.h)
	pose.qRotation.w = 1;
	pose.deviceIsConnected = true;
	pose.result = TrackingResult_Running_OK;

	XrTime when = Now() + (XrTime)predictNs;
	{
		timespec mono;
		clock_gettime(CLOCK_MONOTONIC, &mono);
		lastWhenNs = (int64_t)mono.tv_sec * 1000000000ll + mono.tv_nsec + (int64_t)predictNs;
	}
	XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
	if (XR_FAILED(pxrLocateSpace(view, local, when, &loc)) ||
	    !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
		pose.result = TrackingResult_Running_OutOfRange;
		return false;
	}
	HoldPosition(loc);
	ApplyNeckModel(loc);
	*xrPose = loc.pose;
	Quest1FrameUpdate(loc.pose, (loc.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) != 0);
	Quest1FrameApply(pose);
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

//! Tracking lost (Meta's 6DoF status falls to 0, Monado clears POSITION_TRACKED and its position goes to the origin, which
//! put the head on the floor): keep the last tracked position, the head turning about it with the neck model, as Meta's
//! OS does. A head never tracked since SteamVR started (3DoF, cameras off) is unchanged.
void XrBackend::HoldPosition(XrSpaceLocation &loc)
{
	std::lock_guard<std::mutex> l(holdMutex);
	if (loc.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) {
		lastTrackedPos = loc.pose.position;
		haveTrackedPos = true;
	} else if (haveTrackedPos) {
		loc.pose.position = lastTrackedPos;
	}
}

void XrBackend::ApplyNeckModel(XrSpaceLocation &loc)
{
	// no neck model when Monado tracks the head's position (Meta's 6DoF): it would move the eyes twice
	if (neckModel && !(loc.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT)) {
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
}

bool XrBackend::LocateRawAt(int64_t monoNs, HmdMatrix34_t &raw)
{
	if (!session || !pxrConvertTimespecTimeToTimeKHR)
		return false;
	timespec ts = {(time_t)(monoNs / 1000000000ll), (long)(monoNs % 1000000000ll)};
	XrTime when = 0;
	if (XR_FAILED(pxrConvertTimespecTimeToTimeKHR(instance, &ts, &when)))
		return false;
	XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
	if (XR_FAILED(pxrLocateSpace(view, local, when, &loc)) || !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
		return false;
	HoldPosition(loc);
	ApplyNeckModel(loc);
	DriverPose_t p = {};
	Quest1FrameApply(p);
	// raw = worldFromDriver * head: rotation qw * q, translation qw * position + tw
	const HmdQuaternion_t &w = p.qWorldFromDriverRotation;
	const XrQuaternionf &h = loc.pose.orientation;
	const double qw = w.w * h.w - w.x * h.x - w.y * h.y - w.z * h.z, qx = w.w * h.x + w.x * h.w + w.y * h.z - w.z * h.y,
	             qy = w.w * h.y - w.x * h.z + w.y * h.w + w.z * h.x, qz = w.w * h.z + w.x * h.y - w.y * h.x + w.z * h.w;
	auto rotate = [](const HmdQuaternion_t &q, const double v[3], double o[3]) {
		const double tx = 2 * (q.y * v[2] - q.z * v[1]), ty = 2 * (q.z * v[0] - q.x * v[2]), tz = 2 * (q.x * v[1] - q.y * v[0]);
		o[0] = v[0] + q.w * tx + (q.y * tz - q.z * ty);
		o[1] = v[1] + q.w * ty + (q.z * tx - q.x * tz);
		o[2] = v[2] + q.w * tz + (q.x * ty - q.y * tx);
	};
	const double pos[3] = {loc.pose.position.x, loc.pose.position.y, loc.pose.position.z};
	double t[3];
	rotate(w, pos, t);
	const double m[3][3] = {{1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw), 2 * (qx * qz + qy * qw)},
	                        {2 * (qx * qy + qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw)},
	                        {2 * (qx * qz - qy * qw), 2 * (qy * qz + qx * qw), 1 - 2 * (qx * qx + qy * qy)}};
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 3; c++)
			raw.m[r][c] = (float)m[r][c];
		raw.m[r][3] = (float)(t[r] + p.vecWorldFromDriverTranslation[r]);
	}
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
	poseRing[frame % 64] = {frame, pose, lastWhenNs};
}

bool XrBackend::PoseForFrame(uint64_t frame, XrPosef *pose, int64_t *whenNs)
{
	std::lock_guard<std::mutex> lock(poseRingMutex);
	const PoseSample &slot = poseRing[frame % 64];
	if (frame == 0 || slot.frame != frame)
		return false;
	*pose = slot.pose;
	if (whenNs)
		*whenNs = slot.whenNs;
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
				FillPosed(ack);
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
				FillPosed(ack);
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

//! The pose just published for frame ack.frame + 1, for the layer (and Monado's re-projection).
void XrBackend::FillPosed(qd_msg &ack)
{
	XrPosef p;
	int64_t when = 0;
	if (!PoseForFrame(ack.frame + 1, &p, &when))
		return;
	ack.pose_valid = 1;
	ack.pose[0] = p.orientation.x;
	ack.pose[1] = p.orientation.y;
	ack.pose[2] = p.orientation.z;
	ack.pose[3] = p.orientation.w;
	ack.pose_time_ns = when;
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
		double wake = MonotonicSeconds();
		{
			std::lock_guard<std::mutex> lock(frameMutex);
			lastVsync = wake;
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
			// vrcompositor's vkWaitForPresentKHR returns now: its "vsync" is this wake-up, the
			// moment its frames must be ready by
			if (displayIndex >= 0 && clientFd >= 0) {
				qd_msg d{};
				d.type = QD_DISPLAYED;
				d.frame = displayFrame;
				d.vblank_ns = (uint64_t)(wake * 1e9);
				send(clientFd, &d, sizeof(d), MSG_NOSIGNAL);
			}
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
		// No Prop_DriverProvidedChaperoneJson_String / Path_String: when a driver sets either, vrserver marks the
		// chaperone as the driver's and never writes chaperone_info.vrchap (CVRChaperoneServer: "Failed to presiste
		// chaperone data", the commit still reports success): the playspace lived in memory only and was gone at
		// the next SteamVR start (2026-11-08). The default universe 2 (standing = raw, which is floor-anchored,
		// chaperone_frame.h), without which the room setup draws no floor or brush, is written to the chaperone
		// file instead, by steamvr-q1 before vrserver starts (quest1-chaperone-default; Clear playspace keeps the universe).
		// Prop_SupportsInHMDRoomSetup_Bool (11003, not in the public headers): selects the Steam Frame's
		// shipping in-HMD playspace setup (vrcompositor draws it, the dashboard starts it) instead of
		// looking for the legacy "openvr.tool.steamvr_room_setup" app
		VRProperties()->SetBoolProperty(c, (ETrackedDeviceProperty)11003, true);
		// Prop_VRLinkClientHMDSupportsRoomSetupRequests_Bool (11004): the dashboard's room setup
		// buttons (Playspace menu, Floor/Boundary) run ConstructStore.setRoomSetupStep(), which ignores
		// every step except 0/1 ("HMD doesnt support setRoomSetupStep") unless this is true
		VRProperties()->SetBoolProperty(c, (ETrackedDeviceProperty)11004, true);
		// Prop_Hmd_RecommendedResolutionIsPerApp_Bool (2121, not in the public headers), as on the Steam Frame:
		// the dashboard's Video settings drop the global render resolution slider (systemInfo.resolution_per_app)
		// and each game's own Video settings keep theirs, in pixels.
		VRProperties()->SetBoolProperty(c, (ETrackedDeviceProperty)2121, true);
		// SteamVR's display brightness (Video settings, Steam's quick menu): steamvr/analogGain, applied to the
		// panels by Monado (PollSettingsActions; a gain in the display engine, the backlight stays where the panels
		// match). The dashboard's slider shows 100 * gain^(1/2.2) % and starts at 100 * MinUI %: 20 % = a 3 % gain.
		VRProperties()->SetBoolProperty(c, Prop_DisplaySupportsAnalogGain_Bool, true);
		VRProperties()->SetFloatProperty(c, Prop_DisplayMinAnalogGain_Float, 0.03f);
		VRProperties()->SetFloatProperty(c, Prop_DisplayMaxAnalogGain_Float, 1.0f);
		VRProperties()->SetFloatProperty(c, Prop_DisplayMinUIAnalogGain_Float, 0.2f);
		VRProperties()->SetBoolProperty(c, Prop_IsOnDesktop_Bool, false);
		VRProperties()->SetBoolProperty(c, Prop_DisplayDebugMode_Bool, false);
		VRProperties()->SetBoolProperty(c, Prop_DeviceProvidesBatteryStatus_Bool, false);
		Quest1BatteryAttach(Quest1Battery_Head, c); // battery.cpp: level and charging, polled
		Quest1CameraAttach(c, [this] { return GetPose(); }, // camera.cpp: tracking cameras (off unless enabled)
		                   [this](int64_t monoNs, HmdMatrix34_t &raw) { return xr.LocateRawAt(monoNs, raw); });

		poseThreadRunning = true;
		poseThread = std::thread([this] {
			pthread_setname_np(pthread_self(), "quest1 poses");
			// while vrcompositor presents, poses are published by PublishFramePose only; before
			// that (start-up, virtual display) at 250 Hz
			for (unsigned tick = 0; poseThreadRunning; tick++) {
				std::this_thread::sleep_for(std::chrono::milliseconds(4));
				if (tick % 128 == 0)
					ReadDebugMode();
				if (tick % 128 == 32)
					ReadPoseAge();
				if (tick % 128 == 64)
					UpdateIpd();
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

	//! the IPD slider moved: new eye separation for vrcompositor
	void UpdateIpd()
	{
		float mm = SliderIpdMm();
		if (mm < 55.0f || mm > 75.0f || fabsf(mm / 1000.0f - xr.ipd) < 0.0003f)
			return;
		xr.ipd = mm / 1000.0f;
		VRProperties()->SetFloatProperty(VRProperties()->TrackedDeviceToPropertyContainer(id), Prop_UserIpdMeters_Float,
		                                 xr.ipd);
		Log("quest1: IPD slider: %.1f mm\n", mm);
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
			xr.predictFixed = true;
			xr.neckModel = neck != 0;
		}
		fclose(f);
	}

	//! Monado writes "<sequence> <ms>" to /run/monado/quest1-pose-age every 2 s: how much later than the time their
	//! pose was predicted for the new frames reached the panels, on average. Half of it goes into the prediction
	//! (the frames' path through vrcompositor, the layer and Monado's commit is longer than the 30 ms first assumed:
	//! ~53 ms at 72 fps, 2026-11-07; the warp had to make up the rest, seen as distortion when the head turns).
	void ReadPoseAge()
	{
		if (xr.predictFixed)
			return;
		FILE *f = fopen("/run/monado/quest1-pose-age", "r");
		if (!f)
			return;
		unsigned long seq = 0;
		float ms = 0;
		int n = fscanf(f, "%lu %f", &seq, &ms);
		fclose(f);
		if (n != 2 || seq == poseAgeSeq)
			return;
		poseAgeSeq = seq;
		double cur = (double)xr.predictNs / 1e6;
		double next = std::min(90.0, std::max(20.0, cur + 0.5 * ms));
		xr.predictNs = (uint64_t)(next * 1e6);
		if (fabs(next - poseAgeLogged) >= 3) {
			Log("pose prediction %.1f ms (frames reached the panels %.1f ms after their pose's time)\n", next,
			    ms);
			poseAgeLogged = next;
		}
	}
	unsigned long poseAgeSeq = 0;
	double poseAgeLogged = 0;

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
		Quest1BatteryAttach(Quest1Battery_Head, k_ulInvalidPropertyContainer);
		Quest1CameraDetach();
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
		if (!strcmp(name, IVRCameraComponent_Version))
			return Quest1CameraComponent();
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
		// direct scanout: the panels show the framebuffer turned by 180°, the left eye sees its right half
		*x = (eye == Eye_Left) != xr.directScanout ? 0 : xr.OutputWidth() / 2;
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
		const XrFovf f = VFov(xr.fov[eye == Eye_Left ? 0 : 1]);
		*left = tanf(f.angleLeft);
		*right = tanf(f.angleRight);
		if (xr.directScanout && !(DirectFlip() & 4)) { // OpenVR's convention: y down
			*top = -tanf(f.angleUp);
			*bottom = -tanf(f.angleDown);
			return;
		}
		*top = tanf(f.angleDown);
		*bottom = tanf(f.angleUp);
	}
	//! debug: QUEST1_DIRECT_FLIP bits 1 = output v, 2 = texture v, 4 = old projection, 8 = output and
	//! texture u, to sort out orientations on the device. 3 orients the image (vrcompositor's
	//! distortion v runs bottom-up on both sides); 16 mirrors the lens model's vertical asymmetry
	//! (lens centre height, 47 deg up / 53 deg down) without turning the image. 19 is the best so far
	//! (user, 2026-11-01: distortion and chromatic aberration better than 3).
	static XrFovf VFov(XrFovf f)
	{
		if (DirectFlip() & 16) {
			float up = f.angleUp;
			f.angleUp = -f.angleDown;
			f.angleDown = -up;
		}
		return f;
	}
	static int DirectFlip()
	{
		static int f = getenv("QUEST1_DIRECT_FLIP") ? atoi(getenv("QUEST1_DIRECT_FLIP")) : 19;
		return f;
	}
	DistortionCoordinates_t ComputeDistortion(EVREye eye, float u, float v) override
	{
		DistortionCoordinates_t d;
		// /etc/quest1-warp-lens "1": Monado's warp does the lens distortion and the chromatic aberration with
		// the re-projection in one pass (like Meta's TimeWarp), vrcompositor hands over the undistorted eyes
		static const bool monadoLens = [] {
			FILE *f = fopen("/etc/quest1-warp-lens", "r");
			int c = f ? fgetc(f) : 0; // "2": the layer hands Monado the eye renders and drops the distortion pass
			bool on = c == '1' || c == '2';
			if (f)
				fclose(f);
			return on;
		}();
		if (!xr.directScanout || monadoLens) {
			d.rfRed[0] = d.rfGreen[0] = d.rfBlue[0] = u;
			d.rfRed[1] = d.rfGreen[1] = d.rfBlue[1] = v;
			return d;
		}
		// (u, v) in the eye's output viewport, framebuffer orientation; the viewer sees it turned by
		// 180°. Then Meta's Quest 1 lens model (CatmullRom10, as Monado's quest1_hmd.c): panel point
		// relative to the lens centre in tan units, radial scale, per-channel chromatic scale, and
		// the tangents mapped to the eye texture (v down from the top of the projection).
		if (DirectFlip() & 1)
			v = 1.0f - v;
		if (DirectFlip() & 8)
			u = 1.0f - u;
		int e = eye == Eye_Left ? 0 : 1;
		float pu = 1.0f - u, pv = 1.0f - v;
		float sx = e == 0 ? -1.0f : 1.0f;
		float x = ((pu - 0.5f) * kPanelW + sx * kLensToScreenX) / kMetersPerTan;
		float y = ((0.5f - pv) * kPanelH + (DirectFlip() & 16 ? -kLensToScreenY : kLensToScreenY)) / kMetersPerTan;
		float rsq = x * x + y * y;
		float sc = CatmullRom10(rsq);
		const XrFovf f = VFov(xr.fov[e]);
		float tl = tanf(f.angleLeft), tr = tanf(f.angleRight), tu = tanf(f.angleUp), td = tanf(f.angleDown);
		const float chroma[3] = {0.996f - 0.007f * rsq, 1.0f, 1.006f + 0.020f * rsq};
		float *out[3] = {d.rfRed, d.rfGreen, d.rfBlue};
		for (int c = 0; c < 3; c++) {
			float tx = x * sc * chroma[c], ty = y * sc * chroma[c];
			out[c][0] = (tx - tl) / (tr - tl);
			out[c][1] = (tu - ty) / (tu - td);
			if (DirectFlip() & 2)
				out[c][1] = 1.0f - out[c][1];
			if (DirectFlip() & 8)
				out[c][0] = 1.0f - out[c][0];
		}
		return d;
	}
	// Quest 1 optics: Meta's runtime defaults (Oculus SDK LensConfig), docs/quest1-lens.md
	static constexpr float kPanelW = 0.0594f, kPanelH = 0.0660f, kMetersPerTan = 0.0389f;
	static constexpr float kLensToScreenX = 0.002986f, kLensToScreenY = -0.00272f;
	static float CatmullRom10(float rsq)
	{
		static const float K[11] = {1.0f, 1.0374f, 1.0810f, 1.1330f, 1.1970f, 1.2754f,
		                            1.3771f, 1.5133f, 1.7018f, 1.9732f, 2.3000f};
		float fk = 10.0f * rsq;
		int k = (int)floorf(fk);
		k = k < 0 ? 0 : (k > 10 ? 10 : k);
		float t = fk - (float)k, p0, m0, p1, m1;
		if (k == 0) {
			p0 = 1.0f, m0 = K[1] - K[0], p1 = K[1], m1 = 0.5f * (K[2] - K[0]);
		} else if (k < 9) {
			p0 = K[k], m0 = 0.5f * (K[k + 1] - K[k - 1]), p1 = K[k + 1], m1 = 0.5f * (K[k + 2] - K[k]);
		} else if (k == 9) {
			p0 = K[9], m0 = 0.5f * (K[10] - K[8]), p1 = K[10], m1 = K[10] - K[9];
		} else {
			p0 = K[10], m0 = K[10] - K[9], p1 = p0 + m0, m1 = m0;
		}
		float omt = 1.0f - t;
		return (p0 * (1.0f + 2.0f * t) + m0 * t) * omt * omt + (p1 * (1.0f + 2.0f * omt) - m1 * omt) * t * t;
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
		SelectDashboardBindings();
		hmd = new Quest1Hmd(xr);
		if (!VRServerDriverHost()->TrackedDeviceAdded("QUEST1-SteamOS", TrackedDeviceClass_HMD, hmd))
			return VRInitError_Driver_Failed;
		controllers.Init(xr.ControllerContext());
		return VRInitError_None;
	}
	void Cleanup() override
	{
		Quest1BatteryShutdown();
		controllers.Shutdown();
		xr.Shutdown();
		delete hmd;
		hmd = nullptr;
		VR_CLEANUP_SERVER_DRIVER_CONTEXT();
	}
	const char *const *GetInterfaceVersions() override { return k_InterfaceVersions; }
	void RunFrame() override
	{
		VREvent_t e;
		while (VRServerDriverHost()->PollNextEvent(&e, sizeof(e)))
			controllers.OnEvent(e);
		PollSettingsActions();
	}
	bool ShouldBlockStandbyMode() override { return false; }
	void EnterStandby() override {}
	void LeaveStandby() override {}

private:
	//! The "Oculus Quest" page of SteamVR's settings (resources/settings/quest1_settingsschema.json, merged into
	//! SteamVR's schema by steamvr-q1). The schema has no button control: an action is a toggle the driver turns back
	//! off once it has acted. Checked twice a second.
	void PollSettingsActions()
	{
		const auto now = std::chrono::steady_clock::now();
		if (now < nextSettingsPoll)
			return;
		nextSettingsPoll = now + std::chrono::milliseconds(500);
		EVRSettingsError err = VRSettingsError_None;
		// the panels' brightness for Monado (comp_window_mdp.c MDP_BRIGHTNESS_FILE), in percent of the backlight,
		// from SteamVR's own display brightness (steamvr/analogGain, a linear gain: the dashboard's slider and
		// Steam's quick menu set it, Prop_DisplaySupportsAnalogGain_Bool shows them)
		SettingToFile("steamvr", "analogGain", 100.0f, 2.0f, 100.0f, "/tmp/quest1-brightness", writtenBrightness,
		              "display brightness %.0f %% (backlight)");
		err = VRSettingsError_None;
		if (!VRSettings()->GetBool("driver_quest1", "clearSpatialAnchors", &err) || err != VRSettingsError_None)
			return;
		// q1anchord (root, the tracking stack) removes the trigger and forgets every map (docs/boundary.md)
		if (FILE *f = fopen("/tmp/quest1-clear-map", "w"))
			fclose(f);
		Log("quest1: settings: clearing the spatial anchor map\n");
		VRSettings()->SetBool("driver_quest1", "clearSpatialAnchors", false, &err);
	}

	//! A float setting for another process: written to `path` (atomically) when it changes.
	//! (The value times `scale`, clamped to [lo, hi]: SteamVR's default analogGain is 1.3, our maximum 1.)
	void SettingToFile(const char *section, const char *key, float scale, float lo, float hi, const char *path,
	                   float &written, const char *what)
	{
		EVRSettingsError err = VRSettingsError_None;
		float v = VRSettings()->GetFloat(section, key, &err) * scale;
		v = roundf(std::min(std::max(v, lo), hi) * 10.0f) / 10.0f;
		if (err != VRSettingsError_None || v == written)
			return;
		std::string tmp = std::string(path) + ".tmp";
		FILE *f = fopen(tmp.c_str(), "w");
		if (!f)
			return;
		fprintf(f, "%.1f\n", v);
		fclose(f);
		if (rename(tmp.c_str(), path) != 0)
			return;
		written = v;
		char msg[128];
		snprintf(msg, sizeof(msg), what, v);
		Log("quest1: settings: %s\n", msg);
	}

	//! SteamVR takes the oculus_touch controller type's dashboard bindings from its own Oculus driver, so the
	//! default_bindings of our input profile are never used: no right Oculus button, no double click for the
	//! camera view, none of the Frame's grip actions. Select ours as the bindings UI would (it saves this key),
	//! unless the user picked another.
	static void SelectDashboardBindings()
	{
		Dl_info info{};
		if (!dladdr((void *)&SelectDashboardBindings, &info) || !info.dli_fname)
			return;
		std::string root = info.dli_fname; // <root>/bin/linuxarm64/driver_quest1.so
		for (int up = 0; up < 3; up++) {
			size_t slash = root.rfind('/');
			if (slash == std::string::npos)
				return;
			root.resize(slash);
		}
		const std::string url = "file://" + root + "/resources/input/quest1_touch_vrcompositor_bindings.json";
		const char *section = "openvr.component.vrcompositor", *key = "oculus_touch_250820_CurrentURL_steamvrinput";
		char cur[1024] = "";
		EVRSettingsError err = VRSettingsError_None;
		VRSettings()->GetString(section, key, cur, sizeof(cur), &err);
		if (url == cur || (cur[0] && !strstr(cur, "/drivers/oculus/")))
			return;
		VRSettings()->SetString(section, key, url.c_str(), &err);
		Log("quest1: dashboard bindings: %s (%d)\n", url.c_str(), (int)err);
	}

	XrBackend xr;
	Quest1Hmd *hmd = nullptr;
	Quest1Controllers controllers;
	std::chrono::steady_clock::time_point nextSettingsPoll{};
	float writtenBrightness = -1.0f;
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
