// q1overlay.h: what q1desktop and q1apps share to show X pixels as SteamVR dashboard overlays: Vulkan on the
// Adreno (loaded at run time), images refreshed from CPU pixels (only the rows that changed), and the overlay's
// mouse, scroll and keyboard events turned into XTest input.
#pragma once
#include <openvr.h>
// Vulkan is loaded at run time (the Adreno blob through libhybris exports only the loader entry points)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>

#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

using namespace vr;

static const char *g_prog = "q1overlay"; // log prefix, set by main
static bool g_debug;
static double g_changed_rows, g_cpu_s; // debug statistics: rows really uploaded, CPU copy time
#define DBG(...) do { if (g_debug) { fprintf(stderr, "%s: ", g_prog); fprintf(stderr, __VA_ARGS__); } } while (0)

static void die(const char *what)
{
	fprintf(stderr, "%s: %s\n", g_prog, what);
	exit(1);
}

static double now_s()
{
	timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

// --- Xvfb framebuffer (XWD file) -----------------------------------------------------------------------------

struct Xwd {
	const uint8_t *pixels = nullptr;
	uint32_t width = 0, height = 0, stride = 0;
	bool bgr = true; // memory order B,G,R,X (red mask 0xff0000 on a little-endian server)
};

static inline uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static inline Xwd xwd_open(const char *path)
{
	Xwd x;
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	struct stat st;
	if (fd < 0 || fstat(fd, &st) < 0)
		die("cannot open the Xvfb framebuffer file");
	const uint8_t *m = (const uint8_t *)mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (m == MAP_FAILED)
		die("mmap of the framebuffer failed");
	// XWDFileHeader: 25 big-endian CARD32 fields, then ncolors XWDColor (12 bytes), then the pixels
	uint32_t header_size = be32(m), bpp = be32(m + 44), red_mask = be32(m + 56), ncolors = be32(m + 76);
	x.width = be32(m + 16);
	x.height = be32(m + 20);
	x.stride = be32(m + 48);
	x.pixels = m + header_size + ncolors * 12;
	x.bgr = red_mask == 0xff0000;
	if (bpp != 32 || header_size + ncolors * 12 + (uint64_t)x.stride * x.height > (uint64_t)st.st_size)
		die("unsupported XWD framebuffer (32 bpp expected)");
	return x;
}

// --- Vulkan: sampled images, refreshed from a mapped staging buffer ------------------------------------------

#define VK_GLOBAL_FNS(X) X(vkCreateInstance)
#define VK_INSTANCE_FNS(X) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceQueueFamilyProperties) \
	X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)
#define VK_DEVICE_FNS(X) X(vkGetDeviceQueue) X(vkCreateImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) \
	X(vkBindImageMemory) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) \
	X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkCreateFence) X(vkBeginCommandBuffer) \
	X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkEndCommandBuffer) X(vkQueueSubmit) X(vkWaitForFences) \
	X(vkResetFences) X(vkDestroyImage) X(vkFreeMemory)
#define VK_DECLARE(f) static PFN_##f f;
VK_GLOBAL_FNS(VK_DECLARE)
VK_INSTANCE_FNS(VK_DECLARE)
VK_DEVICE_FNS(VK_DECLARE)
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;

static void vk_load_global()
{
	void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
	if (!lib || !(vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr")))
		die("cannot load libvulkan.so.1");
#define VK_LOAD_GLOBAL(f) if (!(f = (PFN_##f)vkGetInstanceProcAddr(VK_NULL_HANDLE, #f))) die("missing " #f);
	VK_GLOBAL_FNS(VK_LOAD_GLOBAL)
}

static void vk_load_instance(VkInstance i)
{
#define VK_LOAD_INSTANCE(f) if (!(f = (PFN_##f)vkGetInstanceProcAddr(i, #f))) die("missing " #f);
	VK_INSTANCE_FNS(VK_LOAD_INSTANCE)
}

static void vk_load_device(VkDevice d)
{
#define VK_LOAD_DEVICE(f) if (!(f = (PFN_##f)vkGetDeviceProcAddr(d, #f))) die("missing " #f);
	VK_DEVICE_FNS(VK_LOAD_DEVICE)
}

//! The device, and one staging buffer shared by every surface (uploads are synchronous).
struct Gfx {
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice phys = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t family = 0;
	VkBuffer staging = VK_NULL_HANDLE;
	VkDeviceMemory staging_mem = VK_NULL_HANDLE;
	uint8_t *staging_map = nullptr;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
};

//! One overlay's image: what was uploaded last (shadow), to copy only the rows that changed.
struct Surface {
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory mem = VK_NULL_HANDLE;
	VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
	uint32_t width = 0, height = 0;
	bool first = true;
	std::vector<uint8_t> shadow;
};

static std::vector<std::string> split(const std::string &s)
{
	std::vector<std::string> v;
	std::istringstream in(s);
	for (std::string w; in >> w;)
		v.push_back(w);
	return v;
}

static uint32_t mem_type(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties p;
	vkGetPhysicalDeviceMemoryProperties(phys, &p);
	for (uint32_t i = 0; i < p.memoryTypeCount; i++)
		if ((bits & 1u << i) && (p.memoryTypes[i].propertyFlags & want) == want)
			return i;
	die("no suitable Vulkan memory type");
	return 0;
}

#define VKCHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s: %s = %d\n", g_prog, #x, r_); exit(1); } } while (0)

static void gfx_init(Gfx &g, VkDeviceSize staging_bytes)
{
	char buf[4096];
	VRCompositor()->GetVulkanInstanceExtensionsRequired(buf, sizeof(buf));
	std::vector<std::string> iext = split(buf);
	std::vector<const char *> ip;
	for (auto &e : iext)
		ip.push_back(e.c_str());
	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = g_prog;
	app.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	ici.enabledExtensionCount = ip.size();
	ici.ppEnabledExtensionNames = ip.data();
	vk_load_global();
	VKCHECK(vkCreateInstance(&ici, nullptr, &g.instance));
	vk_load_instance(g.instance);

	uint64_t out = 0;
	VRSystem()->GetOutputDevice(&out, TextureType_Vulkan, g.instance);
	g.phys = (VkPhysicalDevice)out;
	if (!g.phys) {
		uint32_t n = 1;
		VKCHECK(vkEnumeratePhysicalDevices(g.instance, &n, &g.phys));
	}

	uint32_t nq = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &nq, nullptr);
	std::vector<VkQueueFamilyProperties> qf(nq);
	vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &nq, qf.data());
	for (g.family = 0; g.family < nq; g.family++)
		if (qf[g.family].queueFlags & VK_QUEUE_GRAPHICS_BIT)
			break;
	if (g.family == nq)
		die("no graphics queue");

	VRCompositor()->GetVulkanDeviceExtensionsRequired(g.phys, buf, sizeof(buf));
	std::vector<std::string> dext = split(buf);
	std::vector<const char *> dp;
	for (auto &e : dext)
		dp.push_back(e.c_str());
	float prio = 0.5f;
	VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = g.family;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;
	VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.enabledExtensionCount = dp.size();
	dci.ppEnabledExtensionNames = dp.data();
	VKCHECK(vkCreateDevice(g.phys, &dci, nullptr, &g.device));
	vk_load_device(g.device);
	vkGetDeviceQueue(g.device, g.family, 0, &g.queue);
	DBG("Vulkan: %zu instance, %zu device extensions\n", ip.size(), dp.size());

	VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bi.size = staging_bytes;
	bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	VKCHECK(vkCreateBuffer(g.device, &bi, nullptr, &g.staging));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(g.device, g.staging, &mr);
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = mem_type(g.phys, mr.memoryTypeBits,
				       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VKCHECK(vkAllocateMemory(g.device, &mai, nullptr, &g.staging_mem));
	VKCHECK(vkBindBufferMemory(g.device, g.staging, g.staging_mem, 0));
	VKCHECK(vkMapMemory(g.device, g.staging_mem, 0, VK_WHOLE_SIZE, 0, (void **)&g.staging_map));

	VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pci.queueFamilyIndex = g.family;
	VKCHECK(vkCreateCommandPool(g.device, &pci, nullptr, &g.pool));
	VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cai.commandPool = g.pool;
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 1;
	VKCHECK(vkAllocateCommandBuffers(g.device, &cai, &g.cmd));
	VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VKCHECK(vkCreateFence(g.device, &fci, nullptr, &g.fence));
}

// in the pixels' own byte order: no swizzle on the CPU
static void surface_init(Gfx &g, Surface &s, uint32_t w, uint32_t h, bool bgr)
{
	s.width = w;
	s.height = h;
	s.first = true;
	s.format = bgr ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
	VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ii.imageType = VK_IMAGE_TYPE_2D;
	ii.format = s.format;
	ii.extent = {w, h, 1};
	ii.mipLevels = 1;
	ii.arrayLayers = 1;
	ii.samples = VK_SAMPLE_COUNT_1_BIT;
	ii.tiling = VK_IMAGE_TILING_OPTIMAL;
	ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VKCHECK(vkCreateImage(g.device, &ii, nullptr, &s.image));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(g.device, s.image, &mr);
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = mem_type(g.phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VKCHECK(vkAllocateMemory(g.device, &mai, nullptr, &s.mem));
	VKCHECK(vkBindImageMemory(g.device, s.image, s.mem, 0));
}

static inline void surface_destroy(Gfx &g, Surface &s)
{
	if (s.image)
		vkDestroyImage(g.device, s.image, nullptr);
	if (s.mem)
		vkFreeMemory(g.device, s.mem, nullptr);
	s = Surface();
}

static Texture_t surface_texture(const Gfx &g, const Surface &s, VRVulkanTextureData_t &vk)
{
	vk = {};
	vk.m_nImage = (uint64_t)s.image;
	vk.m_pDevice = g.device;
	vk.m_pPhysicalDevice = g.phys;
	vk.m_pInstance = g.instance;
	vk.m_pQueue = g.queue;
	vk.m_nQueueFamilyIndex = g.family;
	vk.m_nWidth = s.width;
	vk.m_nHeight = s.height;
	vk.m_nFormat = s.format;
	vk.m_nSampleCount = 1;
	return {&vk, TextureType_Vulkan, ColorSpace_Auto};
}

// Copy the rows of [y0, y1) of a width x height 32-bit picture (stride bytes per row) that really changed (an X11
// window damages its whole self every frame, a moving cursor changes a few rows) into the staging buffer
// (opaque), then into the image, left in TRANSFER_SRC_OPTIMAL as OpenVR expects of submitted Vulkan textures.
// False if nothing changed.
static bool upload(Gfx &g, Surface &s, const uint8_t *pixels, uint32_t stride, uint32_t y0, uint32_t y1)
{
	const size_t row = (size_t)s.width * 4;
	double t0 = now_s();
	if (s.first) {
		y0 = 0;
		y1 = s.height;
		s.shadow.assign(row * s.height, 0);
	}
	uint32_t c0 = y1, c1 = y0;
	for (uint32_t y = y0; y < y1; y++) {
		const uint8_t *src = pixels + (size_t)y * stride;
		uint8_t *p = s.shadow.data() + (size_t)y * row;
		if (!s.first && !memcmp(src, p, row))
			continue;
		memcpy(p, src, row);
		c0 = std::min(c0, y);
		c1 = y + 1;
	}
	if (c1 <= c0) {
		g_cpu_s += now_s() - t0;
		return false;
	}
	// every row of the copied band from this surface's own shadow: the staging buffer is shared by the surfaces
	// (q1apps), the unchanged rows in it may hold another one's
	for (uint32_t y = c0; y < c1; y++) {
		const uint32_t *p = (const uint32_t *)(s.shadow.data() + (size_t)y * row);
		uint32_t *d = (uint32_t *)(g.staging_map + (size_t)y * row);
		for (uint32_t i = 0; i < s.width; i++)
			d[i] = p[i] | 0xff000000u;
	}
	g_cpu_s += now_s() - t0;
	g_changed_rows += c1 - c0;
	y0 = c0;
	y1 = c1;

	VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VKCHECK(vkBeginCommandBuffer(g.cmd, &cbi));
	VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcAccessMask = s.first ? 0 : VK_ACCESS_TRANSFER_READ_BIT;
	b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	b.oldLayout = s.first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = s.image;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
			     nullptr, 1, &b);
	VkBufferImageCopy r{};
	r.bufferOffset = (VkDeviceSize)y0 * s.width * 4;
	r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	r.imageOffset = {0, (int32_t)y0, 0};
	r.imageExtent = {s.width, y1 - y0, 1};
	vkCmdCopyBufferToImage(g.cmd, g.staging, s.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
	b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
	b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
			     VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
			     nullptr, 1, &b);
	VKCHECK(vkEndCommandBuffer(g.cmd));
	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &g.cmd;
	VKCHECK(vkQueueSubmit(g.queue, 1, &si, g.fence));
	VKCHECK(vkWaitForFences(g.device, 1, &g.fence, VK_TRUE, 1000000000ull));
	VKCHECK(vkResetFences(g.device, 1, &g.fence));
	s.first = false;
	return true;
}

// --- X input (XTest) ---------------------------------------------------------------------------------------

struct XInput {
	Display *dpy = nullptr;
	KeyCode scratch = 0; // a keycode without keysyms, remapped for characters the keymap lacks
};

static void x_find_scratch(XInput &xi)
{
	int lo, hi, per;
	XDisplayKeycodes(xi.dpy, &lo, &hi);
	KeySym *map = XGetKeyboardMapping(xi.dpy, lo, hi - lo + 1, &per);
	if (!map)
		return;
	for (int k = hi; k >= lo; k--) {
		bool empty = true;
		for (int i = 0; i < per; i++)
			if (map[(k - lo) * per + i] != NoSymbol)
				empty = false;
		if (empty) {
			xi.scratch = k;
			break;
		}
	}
	XFree(map);
}

static void x_key(XInput &xi, KeySym ks)
{
	Display *d = xi.dpy;
	KeyCode kc = XKeysymToKeycode(d, ks);
	bool shift = false;
	if (kc) {
		if (XkbKeycodeToKeysym(d, kc, 0, 0) != ks && XkbKeycodeToKeysym(d, kc, 0, 1) == ks)
			shift = true;
	} else if (xi.scratch) {
		KeySym syms[2] = {ks, ks};
		XChangeKeyboardMapping(d, xi.scratch, 2, syms, 1);
		XSync(d, False);
		kc = xi.scratch;
	} else {
		return;
	}
	KeyCode sh = XKeysymToKeycode(d, XK_Shift_L);
	if (shift && sh)
		XTestFakeKeyEvent(d, sh, True, 0);
	XTestFakeKeyEvent(d, kc, True, 0);
	XTestFakeKeyEvent(d, kc, False, 0);
	if (shift && sh)
		XTestFakeKeyEvent(d, sh, False, 0);
	XFlush(d);
}

// SteamVR keyboard input: UTF-8 text, with \b (backspace), \n (enter) and \t
static void x_type(XInput &xi, const char *s, size_t n)
{
	for (size_t i = 0; i < n && s[i];) {
		uint32_t c = (uint8_t)s[i], len = 1;
		if (c >= 0xf0 && i + 3 < n) {
			c = (c & 7) << 18 | (s[i + 1] & 0x3f) << 12 | (s[i + 2] & 0x3f) << 6 | (s[i + 3] & 0x3f);
			len = 4;
		} else if (c >= 0xe0 && i + 2 < n) {
			c = (c & 15) << 12 | (s[i + 1] & 0x3f) << 6 | (s[i + 2] & 0x3f);
			len = 3;
		} else if (c >= 0xc0 && i + 1 < n) {
			c = (c & 31) << 6 | (s[i + 1] & 0x3f);
			len = 2;
		}
		i += len;
		KeySym ks;
		switch (c) {
		case '\b': ks = XK_BackSpace; break;
		case '\n': case '\r': ks = XK_Return; break;
		case '\t': ks = XK_Tab; break;
		case 0x1b: ks = XK_Escape; break;
		case 0x7f: ks = XK_Delete; break;
		default: ks = c < 0x100 ? c : 0x01000000 | c; break;
		}
		x_key(xi, ks);
	}
}

static void x_button(XInput &xi, unsigned b, bool down)
{
	XTestFakeButtonEvent(xi.dpy, b, down, 0);
	XFlush(xi.dpy);
}

// An overlay mouse or keyboard event as X input on a width x height screen; false if it is not an input event.
static bool x_input_event(XInput &xi, const VREvent_t &ev, uint32_t width, uint32_t height)
{
	switch (ev.eventType) {
	case VREvent_MouseMove: {
		// overlay mouse coordinates have their origin at the bottom left
		int mx = (int)ev.data.mouse.x, my = (int)height - 1 - (int)ev.data.mouse.y;
		mx = std::max(0, std::min((int)width - 1, mx));
		my = std::max(0, std::min((int)height - 1, my));
		XTestFakeMotionEvent(xi.dpy, -1, mx, my, 0);
		XFlush(xi.dpy);
		return true;
	}
	case VREvent_MouseButtonDown:
	case VREvent_MouseButtonUp: {
		unsigned b = ev.data.mouse.button == VRMouseButton_Right ? 3
			   : ev.data.mouse.button == VRMouseButton_Middle ? 2 : 1;
		x_button(xi, b, ev.eventType == VREvent_MouseButtonDown);
		return true;
	}
	case VREvent_ScrollDiscrete: {
		// X wheel buttons: 4 up, 5 down, 6 left, 7 right
		int n = (int)ev.data.scroll.ydelta;
		for (int i = 0; i < abs(n); i++) {
			x_button(xi, n > 0 ? 4 : 5, true);
			x_button(xi, n > 0 ? 4 : 5, false);
		}
		n = (int)ev.data.scroll.xdelta;
		for (int i = 0; i < abs(n); i++) {
			x_button(xi, n > 0 ? 7 : 6, true);
			x_button(xi, n > 0 ? 7 : 6, false);
		}
		return true;
	}
	case VREvent_KeyboardCharInput:
		x_type(xi, ev.data.keyboard.cNewInput, sizeof(ev.data.keyboard.cNewInput));
		return true;
	default:
		return false;
	}
}

// The options gamescope gives an app's overlay: physical width, mouse input, scroll, control bar with keyboard
// and close, click stabilization.
static void overlay_setup(VROverlayHandle_t ov, float width_m, uint32_t width, uint32_t height)
{
	VROverlay()->SetOverlayWidthInMeters(ov, width_m);
	VROverlay()->SetOverlayInputMethod(ov, VROverlayInputMethod_Mouse);
	HmdVector2_t scale = {(float)width, (float)height};
	VROverlay()->SetOverlayMouseScale(ov, &scale);
	VROverlay()->SetOverlayFlag(ov, VROverlayFlags_SendVRDiscreteScrollEvents, true);
	VROverlay()->SetOverlayFlag(ov, VROverlayFlags_EnableControlBar, true);
	VROverlay()->SetOverlayFlag(ov, VROverlayFlags_EnableControlBarKeyboard, true);
	VROverlay()->SetOverlayFlag(ov, VROverlayFlags_EnableControlBarClose, true);
	VROverlay()->SetOverlayFlag(ov, VROverlayFlags_EnableClickStabilization, true);
}
