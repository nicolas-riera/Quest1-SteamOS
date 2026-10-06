// qbridge Android side: an OpenXR app on Meta's runtime that streams tracking
// to the Linux (Holo) userland running in a chroot on the same headset, and
// displays the stereo frames Linux renders into shared AHardwareBuffers.
// Without a Linux peer it shows a test pattern.

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <math.h>
#include <poll.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#define XR_USE_TIMESPEC
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "qbridge_proto.h"

#define TAG "qbridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define XR_CHECK(call)                                                        \
	do {                                                                      \
		XrResult r_ = (call);                                                 \
		if (XR_FAILED(r_)) {                                                  \
			LOGE("%s failed: %d (%s:%d)", #call, r_, __FILE__, __LINE__);     \
			return false;                                                     \
		}                                                                     \
	} while (0)

static int64_t now_ns()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// ---------------------------------------------------------------------------
// GL helpers

static PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC p_eglGetNativeClientBufferANDROID;
static PFNEGLCREATEIMAGEKHRPROC p_eglCreateImageKHR;
static PFNEGLDESTROYIMAGEKHRPROC p_eglDestroyImageKHR;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC p_glEGLImageTargetTexture2DOES;

static const char *kVert = R"(#version 300 es
out vec2 uv;
void main() {
	vec2 p = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)) * 2.0;
	uv = p;
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
})";

// Linux renders sRGB-encoded bytes into an UNORM buffer; the swapchain is
// sRGB, so decode here and let the hardware re-encode (net identity).
// Vulkan image rows go top-down, GL textures are sampled bottom-up: flip Y.
static const char *kFragBlit = R"(#version 300 es
precision mediump float;
uniform sampler2D tex;
uniform vec4 rect; // x0, y0, x1, y1 of this eye in the source image (0..1)
in vec2 uv;
out vec4 color;
vec3 srgb_to_linear(vec3 c) {
	return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}
void main() {
	vec2 st = vec2(mix(rect.x, rect.z, uv.x), mix(rect.w, rect.y, uv.y));
	vec4 c = texture(tex, st);
	color = vec4(srgb_to_linear(c.rgb), 1.0);
})";

static const char *kFragPattern = R"(#version 300 es
precision mediump float;
uniform float eye;
uniform float t;
in vec2 uv;
out vec4 color;
void main() {
	vec2 g = abs(fract(uv * 16.0) - 0.5);
	float line = step(0.46, max(g.x, g.y));
	vec3 base = eye < 0.5 ? vec3(0.05, 0.10, 0.30) : vec3(0.30, 0.05, 0.10);
	float pulse = 0.5 + 0.5 * sin(t * 2.0);
	float cr = step(abs(uv.x - 0.5), 0.004) + step(abs(uv.y - 0.5), 0.004);
	color = vec4(base + line * 0.4 + cr * vec3(pulse), 1.0);
})";

static GLuint compile(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, nullptr);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024];
		glGetShaderInfoLog(s, sizeof(log), nullptr, log);
		LOGE("shader: %s", log);
	}
	return s;
}

static GLuint link_program(const char *vs, const char *fs)
{
	GLuint p = glCreateProgram();
	glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
	glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
	glLinkProgram(p);
	return p;
}

// ---------------------------------------------------------------------------
// App state

struct Slot {
	AHardwareBuffer *ahb = nullptr;
	EGLImageKHR image = EGL_NO_IMAGE_KHR;
	GLuint tex = 0;
};

struct HandActions {
	XrPath path = XR_NULL_PATH;
	XrSpace grip_space = XR_NULL_HANDLE, aim_space = XR_NULL_HANDLE;
};

struct App {
	android_app *android = nullptr;
	bool resumed = false;

	// EGL
	EGLDisplay display = EGL_NO_DISPLAY;
	EGLConfig config = nullptr;
	EGLContext context = EGL_NO_CONTEXT;
	EGLSurface pbuffer = EGL_NO_SURFACE;
	GLuint prog_blit = 0, prog_pattern = 0, fbo = 0, vao = 0;

	// OpenXR
	XrInstance instance = XR_NULL_HANDLE;
	XrSystemId system = XR_NULL_SYSTEM_ID;
	XrSession session = XR_NULL_HANDLE;
	XrSessionState state = XR_SESSION_STATE_UNKNOWN;
	bool running = false;
	XrSpace base_space = XR_NULL_HANDLE, view_space = XR_NULL_HANDLE;
	XrSwapchain swapchain = XR_NULL_HANDLE;
	std::vector<XrSwapchainImageOpenGLESKHR> sc_images;
	uint32_t eye_w = 0, eye_h = 0;
	float refresh = 72.0f;
	bool has_hands = false, has_timespec = false;
	PFN_xrCreateHandTrackerEXT xrCreateHandTrackerEXT = nullptr;
	PFN_xrLocateHandJointsEXT xrLocateHandJointsEXT = nullptr;
	PFN_xrConvertTimeToTimespecTimeKHR xrConvertTimeToTimespecTimeKHR = nullptr;
	XrHandTrackerEXT hand_tracker[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};

	XrActionSet action_set = XR_NULL_HANDLE;
	XrAction a_grip = XR_NULL_HANDLE, a_aim = XR_NULL_HANDLE, a_trigger = XR_NULL_HANDLE,
	         a_squeeze = XR_NULL_HANDLE, a_stick = XR_NULL_HANDLE, a_trigger_touch = XR_NULL_HANDLE,
	         a_stick_click = XR_NULL_HANDLE, a_stick_touch = XR_NULL_HANDLE,
	         a_primary = XR_NULL_HANDLE, a_secondary = XR_NULL_HANDLE,
	         a_primary_touch = XR_NULL_HANDLE, a_secondary_touch = XR_NULL_HANDLE,
	         a_menu = XR_NULL_HANDLE, a_haptic = XR_NULL_HANDLE;
	HandActions hand[2];

	// Bridge
	int sock = -1;
	int64_t next_connect_ns = 0;
	Slot slots[QB_MAX_SLOTS];
	uint32_t img_w = 0, img_h = 0;
	int shown_slot = -1;
	uint64_t shown_seq = 0;
	uint64_t pose_seq = 0;
	static constexpr int kHistory = 64;
	struct { uint64_t seq; XrView views[2]; } history[kHistory] = {};
	uint64_t frames = 0, linux_frames = 0;
	int64_t last_log_ns = 0;
};

// ---------------------------------------------------------------------------
// EGL

static bool init_egl(App &a)
{
	a.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	eglInitialize(a.display, nullptr, nullptr);
	const EGLint cfg_attr[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
	                           EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
	                           EGL_NONE};
	EGLint n = 0;
	if (!eglChooseConfig(a.display, cfg_attr, &a.config, 1, &n) || n == 0) {
		LOGE("eglChooseConfig failed");
		return false;
	}
	const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
	a.context = eglCreateContext(a.display, a.config, EGL_NO_CONTEXT, ctx_attr);
	const EGLint pb_attr[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
	a.pbuffer = eglCreatePbufferSurface(a.display, a.config, pb_attr);
	if (!eglMakeCurrent(a.display, a.pbuffer, a.pbuffer, a.context)) {
		LOGE("eglMakeCurrent failed");
		return false;
	}
	p_eglGetNativeClientBufferANDROID =
	    (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress("eglGetNativeClientBufferANDROID");
	p_eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
	p_eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
	p_glEGLImageTargetTexture2DOES =
	    (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
	a.prog_blit = link_program(kVert, kFragBlit);
	a.prog_pattern = link_program(kVert, kFragPattern);
	glGenFramebuffers(1, &a.fbo);
	glGenVertexArrays(1, &a.vao);
	LOGI("GL: %s / %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));
	return true;
}

// ---------------------------------------------------------------------------
// OpenXR setup

static XrPath path(App &a, const char *s)
{
	XrPath p = XR_NULL_PATH;
	xrStringToPath(a.instance, s, &p);
	return p;
}

static bool init_instance(App &a)
{
	PFN_xrInitializeLoaderKHR init_loader = nullptr;
	xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&init_loader);
	if (init_loader) {
		XrLoaderInitInfoAndroidKHR li{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
		li.applicationVM = a.android->activity->vm;
		li.applicationContext = a.android->activity->clazz;
		init_loader((const XrLoaderInitInfoBaseHeaderKHR *)&li);
	}

	uint32_t count = 0;
	xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
	std::vector<XrExtensionProperties> props(count, {XR_TYPE_EXTENSION_PROPERTIES});
	xrEnumerateInstanceExtensionProperties(nullptr, count, &count, props.data());
	std::vector<const char *> exts = {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
	                                  XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME};
	std::string all;
	for (auto &p : props) {
		all += p.extensionName;
		all += ' ';
		if (!strcmp(p.extensionName, XR_EXT_HAND_TRACKING_EXTENSION_NAME)) {
			a.has_hands = true;
			exts.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
		} else if (!strcmp(p.extensionName, XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME)) {
			a.has_timespec = true;
			exts.push_back(XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME);
		}
	}
	LOGI("runtime extensions: %s", all.c_str());

	XrInstanceCreateInfoAndroidKHR android_ci{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
	android_ci.applicationVM = a.android->activity->vm;
	android_ci.applicationActivity = a.android->activity->clazz;
	XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
	ci.next = &android_ci;
	strcpy(ci.applicationInfo.applicationName, "qbridge");
	ci.applicationInfo.applicationVersion = 1;
	strcpy(ci.applicationInfo.engineName, "qbridge");
	ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
	ci.enabledExtensionCount = (uint32_t)exts.size();
	ci.enabledExtensionNames = exts.data();
	XR_CHECK(xrCreateInstance(&ci, &a.instance));

	XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
	xrGetInstanceProperties(a.instance, &ip);
	LOGI("runtime: %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
	     XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

	XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO};
	si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XR_CHECK(xrGetSystem(a.instance, &si, &a.system));

	if (a.has_hands) {
		XrSystemHandTrackingPropertiesEXT hp{XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT};
		XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
		sp.next = &hp;
		xrGetSystemProperties(a.instance, a.system, &sp);
		a.has_hands = hp.supportsHandTracking;
		LOGI("system: %s, hand tracking %d", sp.systemName, a.has_hands);
		xrGetInstanceProcAddr(a.instance, "xrCreateHandTrackerEXT", (PFN_xrVoidFunction *)&a.xrCreateHandTrackerEXT);
		xrGetInstanceProcAddr(a.instance, "xrLocateHandJointsEXT", (PFN_xrVoidFunction *)&a.xrLocateHandJointsEXT);
	}
	if (a.has_timespec)
		xrGetInstanceProcAddr(a.instance, "xrConvertTimeToTimespecTimeKHR",
		                      (PFN_xrVoidFunction *)&a.xrConvertTimeToTimespecTimeKHR);
	return true;
}

static bool make_action(App &a, XrAction *out, const char *name, XrActionType type, bool both_hands)
{
	XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
	ci.actionType = type;
	strcpy(ci.actionName, name);
	strcpy(ci.localizedActionName, name);
	XrPath sub[2] = {a.hand[0].path, a.hand[1].path};
	if (both_hands) {
		ci.countSubactionPaths = 2;
		ci.subactionPaths = sub;
	}
	XR_CHECK(xrCreateAction(a.action_set, &ci, out));
	return true;
}

static bool init_actions(App &a)
{
	XrActionSetCreateInfo asi{XR_TYPE_ACTION_SET_CREATE_INFO};
	strcpy(asi.actionSetName, "qbridge");
	strcpy(asi.localizedActionSetName, "qbridge");
	XR_CHECK(xrCreateActionSet(a.instance, &asi, &a.action_set));
	a.hand[0].path = path(a, "/user/hand/left");
	a.hand[1].path = path(a, "/user/hand/right");

	bool ok = make_action(a, &a.a_grip, "grip", XR_ACTION_TYPE_POSE_INPUT, true) &&
	          make_action(a, &a.a_aim, "aim", XR_ACTION_TYPE_POSE_INPUT, true) &&
	          make_action(a, &a.a_trigger, "trigger", XR_ACTION_TYPE_FLOAT_INPUT, true) &&
	          make_action(a, &a.a_squeeze, "squeeze", XR_ACTION_TYPE_FLOAT_INPUT, true) &&
	          make_action(a, &a.a_stick, "thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, true) &&
	          make_action(a, &a.a_trigger_touch, "trigger_touch", XR_ACTION_TYPE_BOOLEAN_INPUT, true) &&
	          make_action(a, &a.a_stick_click, "thumbstick_click", XR_ACTION_TYPE_BOOLEAN_INPUT, true) &&
	          make_action(a, &a.a_stick_touch, "thumbstick_touch", XR_ACTION_TYPE_BOOLEAN_INPUT, true) &&
	          make_action(a, &a.a_primary, "primary", XR_ACTION_TYPE_BOOLEAN_INPUT, true) &&
	          make_action(a, &a.a_secondary, "secondary", XR_ACTION_TYPE_BOOLEAN_INPUT, true) &&
	          make_action(a, &a.a_primary_touch, "primary_touch", XR_ACTION_TYPE_BOOLEAN_INPUT, true) &&
	          make_action(a, &a.a_secondary_touch, "secondary_touch", XR_ACTION_TYPE_BOOLEAN_INPUT, true) &&
	          make_action(a, &a.a_menu, "menu", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
	          make_action(a, &a.a_haptic, "haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, true);
	if (!ok)
		return false;

	struct B { XrAction act; const char *p; } binds[] = {
	    {a.a_grip, "/user/hand/left/input/grip/pose"},       {a.a_grip, "/user/hand/right/input/grip/pose"},
	    {a.a_aim, "/user/hand/left/input/aim/pose"},         {a.a_aim, "/user/hand/right/input/aim/pose"},
	    {a.a_trigger, "/user/hand/left/input/trigger/value"}, {a.a_trigger, "/user/hand/right/input/trigger/value"},
	    {a.a_squeeze, "/user/hand/left/input/squeeze/value"}, {a.a_squeeze, "/user/hand/right/input/squeeze/value"},
	    {a.a_stick, "/user/hand/left/input/thumbstick"},     {a.a_stick, "/user/hand/right/input/thumbstick"},
	    {a.a_trigger_touch, "/user/hand/left/input/trigger/touch"},
	    {a.a_trigger_touch, "/user/hand/right/input/trigger/touch"},
	    {a.a_stick_click, "/user/hand/left/input/thumbstick/click"},
	    {a.a_stick_click, "/user/hand/right/input/thumbstick/click"},
	    {a.a_stick_touch, "/user/hand/left/input/thumbstick/touch"},
	    {a.a_stick_touch, "/user/hand/right/input/thumbstick/touch"},
	    {a.a_primary, "/user/hand/left/input/x/click"},      {a.a_primary, "/user/hand/right/input/a/click"},
	    {a.a_secondary, "/user/hand/left/input/y/click"},    {a.a_secondary, "/user/hand/right/input/b/click"},
	    {a.a_primary_touch, "/user/hand/left/input/x/touch"}, {a.a_primary_touch, "/user/hand/right/input/a/touch"},
	    {a.a_secondary_touch, "/user/hand/left/input/y/touch"},
	    {a.a_secondary_touch, "/user/hand/right/input/b/touch"},
	    {a.a_menu, "/user/hand/left/input/menu/click"},
	    {a.a_haptic, "/user/hand/left/output/haptic"},       {a.a_haptic, "/user/hand/right/output/haptic"},
	};
	std::vector<XrActionSuggestedBinding> sb;
	for (auto &b : binds)
		sb.push_back({b.act, path(a, b.p)});
	XrInteractionProfileSuggestedBinding isb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
	isb.interactionProfile = path(a, "/interaction_profiles/oculus/touch_controller");
	isb.countSuggestedBindings = (uint32_t)sb.size();
	isb.suggestedBindings = sb.data();
	XR_CHECK(xrSuggestInteractionProfileBindings(a.instance, &isb));
	return true;
}

static bool init_session(App &a)
{
	PFN_xrGetOpenGLESGraphicsRequirementsKHR get_req = nullptr;
	xrGetInstanceProcAddr(a.instance, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&get_req);
	XrGraphicsRequirementsOpenGLESKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
	XR_CHECK(get_req(a.instance, a.system, &req));

	XrGraphicsBindingOpenGLESAndroidKHR gb{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
	gb.display = a.display;
	gb.config = a.config;
	gb.context = a.context;
	XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
	sci.next = &gb;
	sci.systemId = a.system;
	XR_CHECK(xrCreateSession(a.instance, &sci, &a.session));

	// Prefer STAGE (floor-level, guardian-aligned), fall back to LOCAL.
	uint32_t n = 0;
	xrEnumerateReferenceSpaces(a.session, 0, &n, nullptr);
	std::vector<XrReferenceSpaceType> types(n);
	xrEnumerateReferenceSpaces(a.session, n, &n, types.data());
	XrReferenceSpaceType base = XR_REFERENCE_SPACE_TYPE_LOCAL;
	for (auto t : types)
		if (t == XR_REFERENCE_SPACE_TYPE_STAGE)
			base = t;
	XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	rs.poseInReferenceSpace.orientation.w = 1.0f;
	rs.referenceSpaceType = base;
	XR_CHECK(xrCreateReferenceSpace(a.session, &rs, &a.base_space));
	rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	XR_CHECK(xrCreateReferenceSpace(a.session, &rs, &a.view_space));
	LOGI("base space: %s", base == XR_REFERENCE_SPACE_TYPE_STAGE ? "STAGE" : "LOCAL");

	for (int h = 0; h < 2; h++) {
		XrActionSpaceCreateInfo as{XR_TYPE_ACTION_SPACE_CREATE_INFO};
		as.poseInActionSpace.orientation.w = 1.0f;
		as.subactionPath = a.hand[h].path;
		as.action = a.a_grip;
		XR_CHECK(xrCreateActionSpace(a.session, &as, &a.hand[h].grip_space));
		as.action = a.a_aim;
		XR_CHECK(xrCreateActionSpace(a.session, &as, &a.hand[h].aim_space));
	}
	XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
	at.countActionSets = 1;
	at.actionSets = &a.action_set;
	XR_CHECK(xrAttachSessionActionSets(a.session, &at));

	if (a.has_hands) {
		for (int h = 0; h < 2; h++) {
			XrHandTrackerCreateInfoEXT hc{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
			hc.hand = h == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
			hc.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
			if (XR_FAILED(a.xrCreateHandTrackerEXT(a.session, &hc, &a.hand_tracker[h])))
				a.has_hands = false;
		}
	}

	// One side-by-side swapchain, both eyes in it, matching the shared buffers.
	XrViewConfigurationView vcv[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
	XR_CHECK(xrEnumerateViewConfigurationViews(a.instance, a.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
	                                           &n, vcv));
	a.eye_w = vcv[0].recommendedImageRectWidth;
	a.eye_h = vcv[0].recommendedImageRectHeight;
	LOGI("eye size %ux%u (max %ux%u)", a.eye_w, a.eye_h, vcv[0].maxImageRectWidth, vcv[0].maxImageRectHeight);

	XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
	sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
	sc.format = GL_SRGB8_ALPHA8;
	sc.sampleCount = 1;
	sc.width = a.eye_w * 2;
	sc.height = a.eye_h;
	sc.faceCount = 1;
	sc.arraySize = 1;
	sc.mipCount = 1;
	XR_CHECK(xrCreateSwapchain(a.session, &sc, &a.swapchain));
	xrEnumerateSwapchainImages(a.swapchain, 0, &n, nullptr);
	a.sc_images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
	xrEnumerateSwapchainImages(a.swapchain, n, &n, (XrSwapchainImageBaseHeader *)a.sc_images.data());
	return true;
}

// ---------------------------------------------------------------------------
// Bridge socket

static void bridge_close(App &a)
{
	if (a.sock >= 0) {
		close(a.sock);
		LOGI("bridge: disconnected");
	}
	a.sock = -1;
	a.shown_slot = -1;
	for (auto &s : a.slots) {
		if (s.tex)
			glDeleteTextures(1, &s.tex);
		if (s.image != EGL_NO_IMAGE_KHR)
			p_eglDestroyImageKHR(a.display, s.image);
		if (s.ahb)
			AHardwareBuffer_release(s.ahb);
		s = Slot();
	}
}

static bool bridge_send(App &a, const void *msg, size_t len)
{
	if (a.sock < 0)
		return false;
	if (send(a.sock, msg, len, MSG_NOSIGNAL) != (ssize_t)len) {
		if (errno != EAGAIN)
			bridge_close(a);
		return false;
	}
	return true;
}

static void bridge_connect(App &a)
{
	if (a.sock >= 0 || now_ns() < a.next_connect_ns)
		return;
	a.next_connect_ns = now_ns() + 1000000000LL;
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path + 1, QB_SOCKET_NAME, strlen(QB_SOCKET_NAME));
	socklen_t len = offsetof(sockaddr_un, sun_path) + 1 + strlen(QB_SOCKET_NAME);
	if (connect(fd, (sockaddr *)&addr, len) != 0) {
		close(fd);
		return;
	}
	a.sock = fd;
	LOGI("bridge: connected");

	qb_hello h{};
	h.hdr = {QB_MSG_HELLO, sizeof(h)};
	h.version = QB_PROTO_VERSION;
	h.eye_width = a.eye_w;
	h.eye_height = a.eye_h;
	h.refresh_rate = a.refresh;
	h.has_hand_tracking = a.has_hands;
	bridge_send(a, &h, sizeof(h));

	// Shared stereo images: allocated here (real gralloc), imported by Linux.
	a.img_w = a.eye_w * 2;
	a.img_h = a.eye_h;
	AHardwareBuffer_Desc d{};
	d.width = a.img_w;
	d.height = a.img_h;
	d.layers = 1;
	d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
	d.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
	qb_buffers b{};
	b.hdr = {QB_MSG_BUFFERS, sizeof(b)};
	b.count = QB_MAX_SLOTS;
	b.width = a.img_w;
	b.height = a.img_h;
	b.format = d.format;
	bridge_send(a, &b, sizeof(b));
	for (auto &s : a.slots) {
		if (AHardwareBuffer_allocate(&d, &s.ahb) != 0) {
			LOGE("AHardwareBuffer_allocate failed");
			bridge_close(a);
			return;
		}
		if (AHardwareBuffer_sendHandleToUnixSocket(s.ahb, a.sock) != 0) {
			LOGE("sendHandleToUnixSocket failed");
			bridge_close(a);
			return;
		}
		EGLClientBuffer cb = p_eglGetNativeClientBufferANDROID(s.ahb);
		const EGLint attr[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
		s.image = p_eglCreateImageKHR(a.display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, cb, attr);
		glGenTextures(1, &s.tex);
		glBindTexture(GL_TEXTURE_2D, s.tex);
		p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)s.image);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	}
	fcntl(a.sock, F_SETFL, fcntl(a.sock, F_GETFL) | O_NONBLOCK);
	LOGI("bridge: shared %d buffers %ux%u", QB_MAX_SLOTS, a.img_w, a.img_h);
}

static void bridge_poll(App &a)
{
	if (a.sock < 0)
		return;
	uint8_t buf[512];
	for (;;) {
		ssize_t n = recv(a.sock, buf, sizeof(buf), MSG_DONTWAIT);
		if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
			bridge_close(a);
			return;
		}
		if (n < (ssize_t)sizeof(qb_header))
			return;
		auto *hdr = (qb_header *)buf;
		if (hdr->type == QB_MSG_FRAME && n >= (ssize_t)sizeof(qb_frame)) {
			auto *f = (qb_frame *)buf;
			if (f->slot >= QB_MAX_SLOTS)
				continue;
			if (a.shown_slot >= 0 && a.shown_slot != (int)f->slot) {
				qb_release r{{QB_MSG_RELEASE, sizeof(r)}, (uint32_t)a.shown_slot};
				bridge_send(a, &r, sizeof(r));
			}
			a.shown_slot = f->slot;
			a.shown_seq = f->pose_seq;
			a.linux_frames++;
		} else if (hdr->type == QB_MSG_HAPTIC && n >= (ssize_t)sizeof(qb_haptic)) {
			auto *h = (qb_haptic *)buf;
			XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
			hi.action = a.a_haptic;
			hi.subactionPath = a.hand[h->hand & 1].path;
			if (h->amplitude <= 0.0f) {
				xrStopHapticFeedback(a.session, &hi);
			} else {
				XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
				v.amplitude = h->amplitude;
				v.frequency = h->frequency > 0 ? h->frequency : XR_FREQUENCY_UNSPECIFIED;
				v.duration = h->duration_ns > 0 ? h->duration_ns : XR_MIN_HAPTIC_DURATION;
				xrApplyHapticFeedback(a.session, &hi, (XrHapticBaseHeader *)&v);
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Tracking sample

static void to_qb(const XrPosef &p, qb_pose &o)
{
	o.orientation = {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
	o.position = {p.position.x, p.position.y, p.position.z};
}

static uint32_t to_flags(XrSpaceLocationFlags f)
{
	return (f & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT ? QB_POSE_ORIENTATION_VALID : 0) |
	       (f & XR_SPACE_LOCATION_POSITION_VALID_BIT ? QB_POSE_POSITION_VALID : 0) |
	       (f & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT ? QB_POSE_ORIENTATION_TRACKED : 0) |
	       (f & XR_SPACE_LOCATION_POSITION_TRACKED_BIT ? QB_POSE_POSITION_TRACKED : 0);
}

static bool get_bool(App &a, XrAction act, int h)
{
	XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
	gi.action = act;
	gi.subactionPath = act == a.a_menu ? XR_NULL_PATH : a.hand[h].path;
	XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};
	xrGetActionStateBoolean(a.session, &gi, &s);
	return s.isActive && s.currentState;
}

static float get_float(App &a, XrAction act, int h)
{
	XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
	gi.action = act;
	gi.subactionPath = a.hand[h].path;
	XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT};
	xrGetActionStateFloat(a.session, &gi, &s);
	return s.isActive ? s.currentState : 0.0f;
}

static void sample(App &a, XrTime t, const XrView views[2], qb_pose_msg &m)
{
	memset(&m, 0, sizeof(m));
	m.hdr = {QB_MSG_POSE, sizeof(m)};
	m.seq = ++a.pose_seq;
	m.display_time_ns = t;
	if (a.xrConvertTimeToTimespecTimeKHR) {
		timespec ts;
		if (XR_SUCCEEDED(a.xrConvertTimeToTimespecTimeKHR(a.instance, t, &ts)))
			m.display_time_ns = ts.tv_sec * 1000000000LL + ts.tv_nsec;
	}

	XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
	xrLocateSpace(a.view_space, a.base_space, t, &loc);
	m.head_flags = to_flags(loc.locationFlags);
	to_qb(loc.pose, m.head);
	for (int e = 0; e < 2; e++) {
		to_qb(views[e].pose, m.eye_pose[e]);
		m.eye_fov[e] = {views[e].fov.angleLeft, views[e].fov.angleRight, views[e].fov.angleUp,
		                views[e].fov.angleDown};
	}

	XrActiveActionSet aas{a.action_set, XR_NULL_PATH};
	XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
	sync.countActiveActionSets = 1;
	sync.activeActionSets = &aas;
	xrSyncActions(a.session, &sync);

	for (int h = 0; h < 2; h++) {
		qb_controller &c = m.controller[h];
		XrSpaceVelocity vel{XR_TYPE_SPACE_VELOCITY};
		XrSpaceLocation gl{XR_TYPE_SPACE_LOCATION};
		gl.next = &vel;
		xrLocateSpace(a.hand[h].grip_space, a.base_space, t, &gl);
		XrSpaceLocation al{XR_TYPE_SPACE_LOCATION};
		xrLocateSpace(a.hand[h].aim_space, a.base_space, t, &al);
		c.grip_flags = to_flags(gl.locationFlags);
		c.aim_flags = to_flags(al.locationFlags);
		to_qb(gl.pose, c.grip);
		to_qb(al.pose, c.aim);
		c.linear_velocity = {vel.linearVelocity.x, vel.linearVelocity.y, vel.linearVelocity.z};
		c.angular_velocity = {vel.angularVelocity.x, vel.angularVelocity.y, vel.angularVelocity.z};
		c.active = (gl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) != 0;

		XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
		gi.action = a.a_stick;
		gi.subactionPath = a.hand[h].path;
		XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
		xrGetActionStateVector2f(a.session, &gi, &st);
		c.thumbstick_x = st.currentState.x;
		c.thumbstick_y = st.currentState.y;
		c.trigger = get_float(a, a.a_trigger, h);
		c.squeeze = get_float(a, a.a_squeeze, h);
		uint32_t b = 0;
		if (get_bool(a, a.a_primary, h)) b |= h ? QB_BTN_A : QB_BTN_X;
		if (get_bool(a, a.a_secondary, h)) b |= h ? QB_BTN_B : QB_BTN_Y;
		if (get_bool(a, a.a_primary_touch, h)) b |= h ? QB_TOUCH_A : QB_TOUCH_X;
		if (get_bool(a, a.a_secondary_touch, h)) b |= h ? QB_TOUCH_B : QB_TOUCH_Y;
		if (get_bool(a, a.a_stick_click, h)) b |= QB_BTN_THUMBSTICK;
		if (get_bool(a, a.a_stick_touch, h)) b |= QB_TOUCH_THUMBSTICK;
		if (get_bool(a, a.a_trigger_touch, h)) b |= QB_TOUCH_TRIGGER;
		if (h == 0 && get_bool(a, a.a_menu, 0)) b |= QB_BTN_MENU;
		c.buttons = b;

		if (a.has_hands) {
			XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT];
			XrHandJointLocationsEXT jl{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
			jl.jointCount = XR_HAND_JOINT_COUNT_EXT;
			jl.jointLocations = joints;
			XrHandJointsLocateInfoEXT li{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
			li.baseSpace = a.base_space;
			li.time = t;
			if (XR_SUCCEEDED(a.xrLocateHandJointsEXT(a.hand_tracker[h], &li, &jl)) && jl.isActive) {
				qb_hand &hd = m.hand[h];
				hd.active = 1;
				for (int j = 0; j < QB_HAND_JOINTS; j++) {
					to_qb(joints[j].pose, hd.joints[j]);
					hd.radii[j] = joints[j].radius;
					hd.joint_flags[j] = to_flags(joints[j].locationFlags);
				}
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Frame loop

static void render_frame(App &a)
{
	XrFrameState fs{XR_TYPE_FRAME_STATE};
	if (XR_FAILED(xrWaitFrame(a.session, nullptr, &fs)))
		return;
	xrBeginFrame(a.session, nullptr);

	XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
	XrViewState vs{XR_TYPE_VIEW_STATE};
	XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
	vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	vli.displayTime = fs.predictedDisplayTime;
	vli.space = a.base_space;
	uint32_t nv = 0;
	xrLocateViews(a.session, &vli, &vs, 2, &nv, views);

	qb_pose_msg pm;
	sample(a, fs.predictedDisplayTime, views, pm);
	auto &hist = a.history[pm.seq % App::kHistory];
	hist.seq = pm.seq;
	hist.views[0] = views[0];
	hist.views[1] = views[1];
	bridge_connect(a);
	bridge_send(a, &pm, sizeof(pm));
	bridge_poll(a);

	// Views to submit: those Linux rendered with (Meta's compositor then
	// reprojects to the actual head pose), or the current ones.
	const XrView *submit = views;
	bool linux_frame = a.sock >= 0 && a.shown_slot >= 0;
	if (linux_frame) {
		auto &h = a.history[a.shown_seq % App::kHistory];
		if (h.seq == a.shown_seq)
			submit = h.views;
	}

	std::vector<XrCompositionLayerBaseHeader *> layers;
	XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
	                                          {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
	XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	if (fs.shouldRender && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
		uint32_t idx = 0;
		xrAcquireSwapchainImage(a.swapchain, nullptr, &idx);
		XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
		wi.timeout = XR_INFINITE_DURATION;
		xrWaitSwapchainImage(a.swapchain, &wi);

		glBindFramebuffer(GL_FRAMEBUFFER, a.fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, a.sc_images[idx].image, 0);
		glBindVertexArray(a.vao);
		glDisable(GL_DEPTH_TEST);
		for (int e = 0; e < 2; e++) {
			glViewport(e * a.eye_w, 0, a.eye_w, a.eye_h);
			if (linux_frame) {
				glUseProgram(a.prog_blit);
				glActiveTexture(GL_TEXTURE0);
				glBindTexture(GL_TEXTURE_2D, a.slots[a.shown_slot].tex);
				glUniform1i(glGetUniformLocation(a.prog_blit, "tex"), 0);
				glUniform4f(glGetUniformLocation(a.prog_blit, "rect"), e * 0.5f, 0.0f, e * 0.5f + 0.5f, 1.0f);
			} else {
				glUseProgram(a.prog_pattern);
				glUniform1f(glGetUniformLocation(a.prog_pattern, "eye"), (float)e);
				glUniform1f(glGetUniformLocation(a.prog_pattern, "t"), (float)(now_ns() % 100000000000LL) * 1e-9f);
			}
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		}
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		xrReleaseSwapchainImage(a.swapchain, nullptr);

		for (int e = 0; e < 2; e++) {
			pv[e].pose = submit[e].pose;
			pv[e].fov = submit[e].fov;
			pv[e].subImage.swapchain = a.swapchain;
			pv[e].subImage.imageRect.offset = {(int32_t)(e * a.eye_w), 0};
			pv[e].subImage.imageRect.extent = {(int32_t)a.eye_w, (int32_t)a.eye_h};
		}
		layer.space = a.base_space;
		layer.viewCount = 2;
		layer.views = pv;
		layers.push_back((XrCompositionLayerBaseHeader *)&layer);
	}

	XrFrameEndInfo fe{XR_TYPE_FRAME_END_INFO};
	fe.displayTime = fs.predictedDisplayTime;
	fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	fe.layerCount = (uint32_t)layers.size();
	fe.layers = layers.data();
	xrEndFrame(a.session, &fe);

	a.frames++;
	if (now_ns() - a.last_log_ns > 2000000000LL) {
		a.last_log_ns = now_ns();
		LOGI("frames %llu linux %llu bridge %s | head %.2f %.2f %.2f | L %d trig %.2f | R %d trig %.2f btn %#x | hands %d %d",
		     (unsigned long long)a.frames, (unsigned long long)a.linux_frames, a.sock >= 0 ? "up" : "down",
		     pm.head.position.x, pm.head.position.y, pm.head.position.z, pm.controller[0].active,
		     pm.controller[0].trigger, pm.controller[1].active, pm.controller[1].trigger,
		     pm.controller[0].buttons | pm.controller[1].buttons, pm.hand[0].active, pm.hand[1].active);
	}
}

static void poll_xr_events(App &a)
{
	XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
	while (xrPollEvent(a.instance, &ev) == XR_SUCCESS) {
		if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			auto *e = (XrEventDataSessionStateChanged *)&ev;
			a.state = e->state;
			LOGI("session state %d", a.state);
			if (a.state == XR_SESSION_STATE_READY) {
				XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
				bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				xrBeginSession(a.session, &bi);
				a.running = true;
			} else if (a.state == XR_SESSION_STATE_STOPPING) {
				xrEndSession(a.session);
				a.running = false;
			} else if (a.state == XR_SESSION_STATE_EXITING || a.state == XR_SESSION_STATE_LOSS_PENDING) {
				a.running = false;
				ANativeActivity_finish(a.android->activity);
			}
		}
		ev = {XR_TYPE_EVENT_DATA_BUFFER};
	}
}

static void on_cmd(android_app *app, int32_t cmd)
{
	App &a = *(App *)app->userData;
	if (cmd == APP_CMD_RESUME)
		a.resumed = true;
	else if (cmd == APP_CMD_PAUSE)
		a.resumed = false;
}

void android_main(android_app *app)
{
	App a;
	a.android = app;
	app->userData = &a;
	app->onAppCmd = on_cmd;

	if (!init_egl(a) || !init_instance(a) || !init_actions(a) || !init_session(a)) {
		LOGE("init failed");
		ANativeActivity_finish(app->activity);
	}

	while (!app->destroyRequested) {
		int events;
		android_poll_source *src;
		int timeout = (a.running || a.resumed) ? 0 : 100;
		while (ALooper_pollOnce(timeout, nullptr, &events, (void **)&src) >= 0) {
			if (src)
				src->process(app, src);
			if (app->destroyRequested)
				break;
			timeout = 0;
		}
		if (a.session == XR_NULL_HANDLE)
			continue;
		poll_xr_events(a);
		if (a.running)
			render_frame(a);
	}

	bridge_close(a);
	if (a.session)
		xrDestroySession(a.session);
	if (a.instance)
		xrDestroyInstance(a.instance);
}
