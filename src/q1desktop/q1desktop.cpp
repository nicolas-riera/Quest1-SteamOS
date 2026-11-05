// q1desktop: an X display (an Xvfb) as a SteamVR dashboard overlay, with mouse, scroll and keyboard input.
// On the Steam Frame, desktop mode is a nested Plasma inside a gamescope whose OpenVR backend shows it as the
// dashboard overlay "valve.plasma.desktopmode". The Quest 1's Adreno 540 blob can't run gamescope (no dma-buf
// import / modifiers), so this plays that role for a Plasma X11 session on its own Xvfb (docs/desktop.md):
//  - pixels: Xvfb exports its framebuffer as an XWD file (-fbdir), mapped here; XDamage says what changed;
//    only the changed rows are copied (alpha forced opaque) into a Vulkan image given to SetOverlayTexture.
//    Nothing is copied while the dashboard is hidden or the screen is idle.
//  - input: the dashboard laser (overlay mouse events), the trackpad/stick scroll and the SteamVR keyboard
//    (control bar keyboard button) become XTest events on that Xvfb.
// Usage (as steamos, SteamVR running): DISPLAY=:2 q1desktop <xwd file>
// Env: Q1D_KEY (valve.plasma.desktopmode), Q1D_NAME (Desktop), Q1D_WIDTH (overlay width in m, 2.67 like the
// Frame), Q1D_ICON (dashboard thumbnail, png), Q1D_HZ (max uploads per second, 60), Q1D_SHOW=0 (don't open
// the dashboard on it at start), Q1D_DEBUG=1.
// Exits when the overlay is closed from its control bar (exit 0) or SteamVR quits (exit 1).
#include <openvr.h>
// Vulkan is loaded at run time (the Adreno blob through libhybris exports only the loader entry points)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>

#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xdamage.h>
#include <X11/keysym.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
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

static bool g_debug;
static volatile sig_atomic_t g_show; // SIGUSR1: open the dashboard on the overlay (a second launch)
static double g_changed_rows, g_cpu_s; // debug statistics: rows really uploaded, CPU copy time
#define DBG(...) do { if (g_debug) fprintf(stderr, "q1desktop: " __VA_ARGS__); } while (0)

static void die(const char *what)
{
	fprintf(stderr, "q1desktop: %s\n", what);
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

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static Xwd xwd_open(const char *path)
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

// --- Vulkan: one sampled image, refreshed from a mapped staging buffer -----------------------------------------

#define VK_GLOBAL_FNS(X) X(vkCreateInstance)
#define VK_INSTANCE_FNS(X) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceQueueFamilyProperties) \
	X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)
#define VK_DEVICE_FNS(X) X(vkGetDeviceQueue) X(vkCreateImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) \
	X(vkBindImageMemory) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) \
	X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkCreateFence) X(vkBeginCommandBuffer) \
	X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkEndCommandBuffer) X(vkQueueSubmit) X(vkWaitForFences) \
	X(vkResetFences)
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

struct Gfx {
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice phys = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t family = 0;
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory image_mem = VK_NULL_HANDLE;
	VkBuffer staging = VK_NULL_HANDLE;
	VkDeviceMemory staging_mem = VK_NULL_HANDLE;
	uint8_t *staging_map = nullptr;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
	bool first = true;
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

#define VKCHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "q1desktop: %s = %d\n", #x, r_); exit(1); } } while (0)

static void gfx_init(Gfx &g, uint32_t w, uint32_t h, bool bgr)
{
	char buf[4096];
	VRCompositor()->GetVulkanInstanceExtensionsRequired(buf, sizeof(buf));
	std::vector<std::string> iext = split(buf);
	std::vector<const char *> ip;
	for (auto &e : iext)
		ip.push_back(e.c_str());
	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "q1desktop";
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

	// the framebuffer's own byte order: no swizzle on the CPU
	g.format = bgr ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
	VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ii.imageType = VK_IMAGE_TYPE_2D;
	ii.format = g.format;
	ii.extent = {w, h, 1};
	ii.mipLevels = 1;
	ii.arrayLayers = 1;
	ii.samples = VK_SAMPLE_COUNT_1_BIT;
	ii.tiling = VK_IMAGE_TILING_OPTIMAL;
	ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VKCHECK(vkCreateImage(g.device, &ii, nullptr, &g.image));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(g.device, g.image, &mr);
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = mem_type(g.phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VKCHECK(vkAllocateMemory(g.device, &mai, nullptr, &g.image_mem));
	VKCHECK(vkBindImageMemory(g.device, g.image, g.image_mem, 0));

	VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bi.size = (VkDeviceSize)w * h * 4;
	bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	VKCHECK(vkCreateBuffer(g.device, &bi, nullptr, &g.staging));
	vkGetBufferMemoryRequirements(g.device, g.staging, &mr);
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

// Copy the rows of [y0, y1) that really changed (kwin's X11 output damages its whole window every frame, a
// moving cursor changes a few rows) into the staging buffer (opaque), then into the image, left in
// TRANSFER_SRC_OPTIMAL as OpenVR expects of submitted Vulkan textures. False if nothing changed.
static bool upload(Gfx &g, const Xwd &x, std::vector<uint8_t> &shadow, uint32_t y0, uint32_t y1)
{
	const size_t row = (size_t)x.width * 4;
	double t0 = now_s();
	if (g.first) {
		y0 = 0;
		y1 = x.height;
		shadow.assign(row * x.height, 0);
	}
	uint32_t c0 = y1, c1 = y0;
	for (uint32_t y = y0; y < y1; y++) {
		const uint8_t *s = x.pixels + (size_t)y * x.stride;
		uint8_t *p = shadow.data() + (size_t)y * row;
		if (!g.first && !memcmp(s, p, row))
			continue;
		memcpy(p, s, row);
		uint32_t *d = (uint32_t *)(g.staging_map + (size_t)y * row);
		for (uint32_t i = 0; i < x.width; i++)
			d[i] = ((const uint32_t *)p)[i] | 0xff000000u;
		c0 = std::min(c0, y);
		c1 = y + 1;
	}
	g_cpu_s += now_s() - t0;
	if (c1 <= c0)
		return false;
	g_changed_rows += c1 - c0;
	y0 = c0;
	y1 = c1;

	VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VKCHECK(vkBeginCommandBuffer(g.cmd, &cbi));
	VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcAccessMask = g.first ? 0 : VK_ACCESS_TRANSFER_READ_BIT;
	b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	b.oldLayout = g.first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = g.image;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
			     nullptr, 1, &b);
	VkBufferImageCopy r{};
	r.bufferOffset = (VkDeviceSize)y0 * x.width * 4;
	r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	r.imageOffset = {0, (int32_t)y0, 0};
	r.imageExtent = {x.width, y1 - y0, 1};
	vkCmdCopyBufferToImage(g.cmd, g.staging, g.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
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
	g.first = false;
	return true;
}

// --- X input (XTest) ---------------------------------------------------------------------------------------

struct XInput {
	Display *dpy = nullptr;
	KeyCode scratch = 0; // a keycode without keysyms, remapped for characters the keymap lacks
	int pending_scroll_x = 0, pending_scroll_y = 0;
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

// --- main --------------------------------------------------------------------------------------------------

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: DISPLAY=:N %s <Xvfb framebuffer xwd file>\n", argv[0]);
		return 2;
	}
	g_debug = getenv("Q1D_DEBUG") && *getenv("Q1D_DEBUG") == '1';
	// gamescope's keys: a window of an app Steam runs is "valve.steam.desktopgame.<appid>" (its
	// --vr-app-overlay-key), which the dashboard ties to that running app; the Frame's desktop session (not
	// launched by Steam) is "valve.plasma.desktopmode"
	std::string key_s = "valve.plasma.desktopmode";
	unsigned long long appid = getenv("SteamAppId") ? strtoull(getenv("SteamAppId"), nullptr, 10) : 0;
	if (!appid && getenv("SteamGameId")) // non-Steam shortcuts: the 64-bit game id carries the app id on top
		appid = strtoull(getenv("SteamGameId"), nullptr, 10) >> 32;
	if (appid)
		key_s = "valve.steam.desktopgame." + std::to_string(appid);
	if (getenv("Q1D_KEY"))
		key_s = getenv("Q1D_KEY");
	const char *key = key_s.c_str();
	DBG("overlay key %s (SteamAppId %s, SteamGameId %s)\n", key, getenv("SteamAppId") ?: "-",
	    getenv("SteamGameId") ?: "-");
	const char *name = getenv("Q1D_NAME") ? getenv("Q1D_NAME") : "Desktop";
	float width_m = getenv("Q1D_WIDTH") ? atof(getenv("Q1D_WIDTH")) : 2.67f;
	double min_dt = 1.0 / (getenv("Q1D_HZ") ? std::max(1, atoi(getenv("Q1D_HZ"))) : 60);

	XInput xi;
	xi.dpy = XOpenDisplay(nullptr);
	if (!xi.dpy)
		die("cannot open the X display (DISPLAY)");
	int dmg_ev, dmg_err, xt_ev, xt_err, xt_maj, xt_min;
	if (!XDamageQueryExtension(xi.dpy, &dmg_ev, &dmg_err))
		die("no DAMAGE extension on the X server");
	if (!XTestQueryExtension(xi.dpy, &xt_ev, &xt_err, &xt_maj, &xt_min))
		die("no XTEST extension on the X server");
	Window root = DefaultRootWindow(xi.dpy);
	XDamageCreate(xi.dpy, root, XDamageReportRawRectangles);
	x_find_scratch(xi);

	Xwd fb = xwd_open(argv[1]);
	DBG("framebuffer %ux%u stride %u %s\n", fb.width, fb.height, fb.stride, fb.bgr ? "BGRX" : "RGBX");

	EVRInitError err = VRInitError_None;
	VR_Init(&err, VRApplication_Overlay);
	if (err != VRInitError_None) {
		fprintf(stderr, "q1desktop: VR_Init: %s\n", VR_GetVRInitErrorAsEnglishDescription(err));
		return 1;
	}

	VROverlayHandle_t ov = k_ulOverlayHandleInvalid, thumb = k_ulOverlayHandleInvalid;
	EVROverlayError oe = VROverlay()->CreateDashboardOverlay(key, name, &ov, &thumb);
	if (oe == VROverlayError_KeyInUse && appid && !getenv("Q1D_KEY")) {
		// Steam's own game theater (steamui: "-legacygame ... -overlaykey valve.steam.desktopgame.<appid>")
		// may hold the app's key
		fprintf(stderr, "q1desktop: %s is taken, using valve.plasma.desktopmode\n", key);
		key_s = "valve.plasma.desktopmode";
		key = key_s.c_str();
		oe = VROverlay()->CreateDashboardOverlay(key, name, &ov, &thumb);
	}
	if (oe != VROverlayError_None) {
		fprintf(stderr, "q1desktop: CreateDashboardOverlay(%s): %s\n", key,
			VROverlay()->GetOverlayErrorNameFromEnum(oe));
		return 1;
	}
	if (getenv("Q1D_ICON") && thumb != k_ulOverlayHandleInvalid)
		VROverlay()->SetOverlayFromFile(thumb, getenv("Q1D_ICON"));
	VROverlay()->SetOverlayWidthInMeters(ov, width_m);
	VROverlay()->SetOverlayInputMethod(ov, VROverlayInputMethod_Mouse);
	HmdVector2_t scale = {(float)fb.width, (float)fb.height};
	VROverlay()->SetOverlayMouseScale(ov, &scale);
	VROverlay()->SetOverlayFlag(ov, VROverlayFlags_SendVRDiscreteScrollEvents, true);
	VROverlay()->SetOverlayFlag(ov, VROverlayFlags_EnableControlBar, true);
	VROverlay()->SetOverlayFlag(ov, VROverlayFlags_EnableControlBarKeyboard, true);
	VROverlay()->SetOverlayFlag(ov, VROverlayFlags_EnableControlBarClose, true);
	VROverlay()->SetOverlayFlag(ov, VROverlayFlags_EnableClickStabilization, true);

	Gfx g;
	gfx_init(g, fb.width, fb.height, fb.bgr);
	VRVulkanTextureData_t vk{};
	vk.m_nImage = (uint64_t)g.image;
	vk.m_pDevice = g.device;
	vk.m_pPhysicalDevice = g.phys;
	vk.m_pInstance = g.instance;
	vk.m_pQueue = g.queue;
	vk.m_nQueueFamilyIndex = g.family;
	vk.m_nWidth = fb.width;
	vk.m_nHeight = fb.height;
	vk.m_nFormat = g.format;
	vk.m_nSampleCount = 1;
	Texture_t tex = {&vk, TextureType_Vulkan, ColorSpace_Auto};

	std::vector<uint8_t> shadow; // the frame last uploaded
	upload(g, fb, shadow, 0, fb.height);
	VROverlay()->SetOverlayTexture(ov, &tex);
	if (!getenv("Q1D_SHOW") || *getenv("Q1D_SHOW") != '0')
		VROverlay()->ShowDashboard(key);

	signal(SIGUSR1, [](int) { g_show = 1; });

	// dirty rows from XDamage, flushed at most every min_dt while the overlay is seen
	uint32_t dirty0 = fb.height, dirty1 = 0;
	double last = 0;
	int xfd = ConnectionNumber(xi.dpy);
	unsigned uploads = 0;
	double stat_t = now_s(), up_s = 0, set_s = 0, rows = 0;
	for (;;) {
		while (XPending(xi.dpy)) {
			XEvent e;
			XNextEvent(xi.dpy, &e);
			if (e.type == dmg_ev + XDamageNotify) {
				auto *d = (XDamageNotifyEvent *)&e;
				int y0 = std::max(0, (int)d->area.y), y1 = std::min((int)fb.height, d->area.y + d->area.height);
				if (y1 > y0) {
					dirty0 = std::min(dirty0, (uint32_t)y0);
					dirty1 = std::max(dirty1, (uint32_t)y1);
				}
			}
		}

		VREvent_t ev;
		while (VROverlay()->PollNextOverlayEvent(ov, &ev, sizeof(ev))) {
			switch (ev.eventType) {
			case VREvent_MouseMove: {
				// overlay mouse coordinates have their origin at the bottom left
				int mx = (int)ev.data.mouse.x, my = (int)fb.height - 1 - (int)ev.data.mouse.y;
				mx = std::max(0, std::min((int)fb.width - 1, mx));
				my = std::max(0, std::min((int)fb.height - 1, my));
				XTestFakeMotionEvent(xi.dpy, -1, mx, my, 0);
				XFlush(xi.dpy);
				break;
			}
			case VREvent_MouseButtonDown:
			case VREvent_MouseButtonUp: {
				unsigned b = ev.data.mouse.button == VRMouseButton_Right ? 3
					   : ev.data.mouse.button == VRMouseButton_Middle ? 2 : 1;
				x_button(xi, b, ev.eventType == VREvent_MouseButtonDown);
				break;
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
				break;
			}
			case VREvent_KeyboardCharInput:
				x_type(xi, ev.data.keyboard.cNewInput, sizeof(ev.data.keyboard.cNewInput));
				break;
			case VREvent_OverlayClosed:
				DBG("closed from the control bar\n");
				VR_Shutdown();
				return 0;
			case VREvent_Quit:
				VR_Shutdown();
				return 1;
			default:
				DBG("overlay event %u\n", ev.eventType);
				break;
			}
		}
		// SteamVR going away: the system-wide quit event comes to the app, not the overlay
		while (VRSystem()->PollNextEvent(&ev, sizeof(ev)))
			if (ev.eventType == VREvent_Quit) {
				VRSystem()->AcknowledgeQuit_Exiting();
				VR_Shutdown();
				return 1;
			}

		if (g_show) {
			g_show = 0;
			VROverlay()->ShowDashboard(key);
		}

		double t = now_s();
		if (dirty1 > dirty0 && t - last >= min_dt && VROverlay()->IsOverlayVisible(ov)) {
			rows += dirty1 - dirty0;
			if (upload(g, fb, shadow, dirty0, dirty1)) {
				double t1 = now_s();
				VROverlay()->SetOverlayTexture(ov, &tex);
				up_s += t1 - t;
				set_s += now_s() - t1;
				uploads++;
			}
			dirty0 = fb.height;
			dirty1 = 0;
			last = t;
		}
		if (g_debug && t - stat_t >= 5) {
			DBG("%.1f uploads/s, %.0f rows damaged, %.0f uploaded, upload %.2f ms (CPU %.2f), SetOverlayTexture %.2f ms each\n",
			    uploads / (t - stat_t), uploads ? rows / uploads : 0, uploads ? g_changed_rows / uploads : 0, uploads ? up_s * 1e3 / uploads : 0, uploads ? g_cpu_s * 1e3 / uploads : 0,
			    uploads ? set_s * 1e3 / uploads : 0);
			uploads = 0;
			up_s = set_s = rows = g_changed_rows = g_cpu_s = 0;
			stat_t = t;
		}

		// sleep until X has something or the next upload slot (overlay events are polled at least at 60 Hz)
		pollfd p = {xfd, POLLIN, 0};
		poll(&p, 1, 16);
	}
}
