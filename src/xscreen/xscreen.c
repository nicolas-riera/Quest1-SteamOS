// xscreen: show an X server's screen as a virtual screen in VR (OpenXR quad layer).
//
// Reads the live framebuffer that Xvfb exports with -fbdir (an XWD file the server mmaps as its
// screen), so no X protocol is involved. The pixels are copied into an OpenXR Vulkan swapchain at
// half the display rate and shown as a quad placed in front of where the user looks at start.
//
// Usage: xr-run xscreen [xwd file] [width m] [distance m] [timeout s]
//   defaults: /run/xvfb/Xvfb_screen0 1.6 1.4 0 (0 = no timeout)
//   SIGUSR1 recenters the screen in front of the current gaze; SIGINT/SIGTERM quit.
//   Putting the headset on (proximity sensor) also recenters it.
// Build: gcc -O2 -o xscreen xscreen.c -I/usr/include -L<loader dir> -lopenxr_loader -lvulkan -lm
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define MAX_IMAGES 8

static volatile sig_atomic_t quit_requested, recenter_requested = 1;

static void on_signal(int sig)
{
	if (sig == SIGUSR1)
		recenter_requested = 1;
	else
		quit_requested = 1;
}

static void die(const char *what, long code)
{
	fprintf(stderr, "xscreen: %s failed (%ld)\n", what, code);
	exit(1);
}

#define XR(call)                                                                                                       \
	do {                                                                                                           \
		XrResult r_ = (call);                                                                                  \
		if (XR_FAILED(r_))                                                                                     \
			die(#call, r_);                                                                                \
	} while (0)
#define VK(call)                                                                                                       \
	do {                                                                                                           \
		VkResult r_ = (call);                                                                                  \
		if (r_ != VK_SUCCESS)                                                                                  \
			die(#call, r_);                                                                                \
	} while (0)

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// --- proximity: recenter when the headset is put on -----------------------------------------

// SyncBoss powerstate message (drivers/staging/oculus uapi syncboss.h): type 2, data 2 = prox on
struct powerstate_msg {
	uint8_t header_version, header_length, from_driver;
	uint32_t type, data;
} __attribute__((packed));

static void poll_prox(int fd)
{
	uint8_t buf[256];
	ssize_t n;
	while (fd >= 0 && (n = read(fd, buf, sizeof(buf))) > 0) {
		for (ssize_t off = 0; off + (ssize_t)sizeof(struct powerstate_msg) <= n;) {
			struct powerstate_msg m;
			memcpy(&m, buf + off, sizeof(m));
			off += m.header_length ? m.header_length : sizeof(m);
			if (m.type == 2 && m.data == 2)
				recenter_requested = 1;
		}
	}
}

// --- XWD framebuffer --------------------------------------------------------------------------

struct xwd {
	const uint8_t *pixels;
	uint32_t width, height, stride;
	int bgr; // byte order in memory is B,G,R,X (red mask 0xff0000 on a little-endian server)
};

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void xwd_open(const char *path, struct xwd *x)
{
	int fd = open(path, O_RDONLY);
	struct stat st;
	if (fd < 0 || fstat(fd, &st) < 0) {
		fprintf(stderr, "xscreen: cannot open %s: %s\n", path, strerror(errno));
		exit(1);
	}
	const uint8_t *m = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (m == MAP_FAILED)
		die("mmap", errno);
	close(fd);
	// XWDFileHeader: 25 big-endian CARD32 fields, then ncolors XWDColor (12 bytes), then pixels
	uint32_t header_size = be32(m), bpp = be32(m + 44), red_mask = be32(m + 56), ncolors = be32(m + 76);
	x->width = be32(m + 16);
	x->height = be32(m + 20);
	x->stride = be32(m + 48);
	x->pixels = m + header_size + ncolors * 12;
	x->bgr = red_mask == 0xff0000;
	if (bpp != 32 || (uint64_t)x->stride * x->height > (uint64_t)st.st_size) {
		fprintf(stderr, "xscreen: unsupported XWD (%u bpp, %ux%u)\n", bpp, x->width, x->height);
		exit(1);
	}
}

// Copy the screen into a tightly packed buffer in the swapchain's byte order, alpha forced opaque.
static void xwd_copy(const struct xwd *x, uint8_t *dst, int dst_bgr)
{
	for (uint32_t y = 0; y < x->height; y++) {
		const uint32_t *s = (const uint32_t *)(x->pixels + (size_t)y * x->stride);
		uint32_t *d = (uint32_t *)(dst + (size_t)y * x->width * 4);
		if (dst_bgr == x->bgr) {
			for (uint32_t i = 0; i < x->width; i++)
				d[i] = s[i] | 0xff000000u;
		} else {
			for (uint32_t i = 0; i < x->width; i++) {
				uint32_t p = s[i];
				d[i] = (p & 0x0000ff00u) | (p >> 16 & 0xffu) | (p & 0xffu) << 16 | 0xff000000u;
			}
		}
	}
}

// --- Vulkan through XR_KHR_vulkan_enable2 ----------------------------------------------------

struct gfx {
	VkInstance instance;
	VkPhysicalDevice phys;
	VkDevice device;
	uint32_t queue_family;
	VkQueue queue;
	VkCommandPool pool;
	VkCommandBuffer cmd;
	VkFence fence;
	VkBuffer staging;
	VkDeviceMemory staging_mem;
	uint8_t *staging_map;
};

#define XR_PROC(inst, name, var) XR(xrGetInstanceProcAddr(inst, #name, (PFN_xrVoidFunction *)&var))

static void gfx_init(XrInstance xi, XrSystemId sys, struct gfx *g)
{
	PFN_xrGetVulkanGraphicsRequirements2KHR get_reqs;
	PFN_xrCreateVulkanInstanceKHR create_instance;
	PFN_xrGetVulkanGraphicsDevice2KHR get_device;
	PFN_xrCreateVulkanDeviceKHR create_device;
	XR_PROC(xi, xrGetVulkanGraphicsRequirements2KHR, get_reqs);
	XR_PROC(xi, xrCreateVulkanInstanceKHR, create_instance);
	XR_PROC(xi, xrGetVulkanGraphicsDevice2KHR, get_device);
	XR_PROC(xi, xrCreateVulkanDeviceKHR, create_device);

	XrGraphicsRequirementsVulkan2KHR reqs = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
	XR(get_reqs(xi, sys, &reqs));

	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "xscreen";
	app.apiVersion = VK_API_VERSION_1_0;
	VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	XrVulkanInstanceCreateInfoKHR xici = {XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
	xici.systemId = sys;
	xici.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
	xici.vulkanCreateInfo = &ici;
	VkResult vr;
	XR(create_instance(xi, &xici, &g->instance, &vr));
	VK(vr);

	XrVulkanGraphicsDeviceGetInfoKHR gdi = {XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
	gdi.systemId = sys;
	gdi.vulkanInstance = g->instance;
	XR(get_device(xi, &gdi, &g->phys));

	uint32_t nfam = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(g->phys, &nfam, NULL);
	VkQueueFamilyProperties fams[16];
	nfam = nfam > 16 ? 16 : nfam;
	vkGetPhysicalDeviceQueueFamilyProperties(g->phys, &nfam, fams);
	g->queue_family = UINT32_MAX;
	for (uint32_t i = 0; i < nfam && g->queue_family == UINT32_MAX; i++)
		if (fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
			g->queue_family = i;
	if (g->queue_family == UINT32_MAX)
		die("graphics queue lookup", 0);

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = g->queue_family;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;
	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	XrVulkanDeviceCreateInfoKHR xdci = {XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
	xdci.systemId = sys;
	xdci.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
	xdci.vulkanPhysicalDevice = g->phys;
	xdci.vulkanCreateInfo = &dci;
	XR(create_device(xi, &xdci, &g->device, &vr));
	VK(vr);
	vkGetDeviceQueue(g->device, g->queue_family, 0, &g->queue);

	VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pci.queueFamilyIndex = g->queue_family;
	VK(vkCreateCommandPool(g->device, &pci, NULL, &g->pool));
	VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cai.commandPool = g->pool;
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 1;
	VK(vkAllocateCommandBuffers(g->device, &cai, &g->cmd));
	VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VK(vkCreateFence(g->device, &fci, NULL, &g->fence));
}

static void gfx_staging(struct gfx *g, VkDeviceSize size)
{
	VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = size;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	VK(vkCreateBuffer(g->device, &bci, NULL, &g->staging));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(g->device, g->staging, &mr);
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(g->phys, &mp);
	const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; i++)
		if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			type = i;
	if (type == UINT32_MAX)
		die("host-visible memory lookup", 0);
	VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = type;
	VK(vkAllocateMemory(g->device, &mai, NULL, &g->staging_mem));
	VK(vkBindBufferMemory(g->device, g->staging, g->staging_mem, 0));
	VK(vkMapMemory(g->device, g->staging_mem, 0, VK_WHOLE_SIZE, 0, (void **)&g->staging_map));
}

static void barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
		    VkAccessFlags dst_access, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
	VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcAccessMask = src_access;
	b.dstAccessMask = dst_access;
	b.oldLayout = from;
	b.newLayout = to;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = img;
	b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

// Upload the staging buffer into a swapchain image. OpenXR hands over and takes back color
// swapchain images in COLOR_ATTACHMENT_OPTIMAL.
static void gfx_upload(struct gfx *g, VkImage img, uint32_t w, uint32_t h)
{
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VK(vkResetCommandBuffer(g->cmd, 0));
	VK(vkBeginCommandBuffer(g->cmd, &bi));
	barrier(g->cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
		VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	VkBufferImageCopy copy = {0};
	copy.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copy.imageExtent = (VkExtent3D){w, h, 1};
	vkCmdCopyBufferToImage(g->cmd, g->staging, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	barrier(g->cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
	VK(vkEndCommandBuffer(g->cmd));
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &g->cmd;
	VK(vkQueueSubmit(g->queue, 1, &si, g->fence));
	VK(vkWaitForFences(g->device, 1, &g->fence, VK_TRUE, UINT64_MAX));
	VK(vkResetFences(g->device, 1, &g->fence));
}

// --- placement --------------------------------------------------------------------------------

// Put the quad `dist` m ahead of the gaze, level with the eyes, turned to face the user (yaw only).
static XrPosef place_quad(XrSpace view, XrSpace local, XrTime t, float dist)
{
	XrPosef pose = {{0, 0, 0, 1}, {0, 0, -dist}};
	XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
	if (XR_FAILED(xrLocateSpace(view, local, t, &loc)) ||
	    !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
		return pose;
	XrQuaternionf q = loc.pose.orientation;
	// forward = q * (0,0,-1)
	float fx = -2 * (q.x * q.z + q.w * q.y);
	float fz = -(1 - 2 * (q.x * q.x + q.y * q.y));
	float n = sqrtf(fx * fx + fz * fz);
	if (n < 1e-3f)
		return pose;
	fx /= n;
	fz /= n;
	float yaw = atan2f(-fx, -fz);
	pose.orientation = (XrQuaternionf){0, sinf(yaw / 2), 0, cosf(yaw / 2)};
	pose.position = (XrVector3f){loc.pose.position.x + fx * dist, loc.pose.position.y, loc.pose.position.z + fz * dist};
	return pose;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/run/xvfb/Xvfb_screen0";
	float width_m = argc > 2 ? strtof(argv[2], NULL) : 1.6f;
	float dist = argc > 3 ? strtof(argv[3], NULL) : 1.4f;
	double timeout = argc > 4 ? strtod(argv[4], NULL) : 0;

	struct sigaction sa = {0};
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);

	struct xwd x;
	xwd_open(path, &x);
	int prox_fd = open("/dev/syncboss_powerstate0", O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	printf("xscreen: %s %ux%u, %.2f m wide at %.2f m\n", path, x.width, x.height, width_m, dist);

	const char *exts[] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
	XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
	strcpy(ci.applicationInfo.applicationName, "xscreen");
	ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	ci.enabledExtensionCount = 1;
	ci.enabledExtensionNames = exts;
	XrInstance xi;
	XR(xrCreateInstance(&ci, &xi));
	XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrSystemId sys;
	XR(xrGetSystem(xi, &sgi, &sys));

	struct gfx g = {0};
	gfx_init(xi, sys, &g);
	gfx_staging(&g, (VkDeviceSize)x.width * x.height * 4);

	XrGraphicsBindingVulkan2KHR binding = {XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
	binding.instance = g.instance;
	binding.physicalDevice = g.phys;
	binding.device = g.device;
	binding.queueFamilyIndex = g.queue_family;
	XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
	sci.next = &binding;
	sci.systemId = sys;
	XrSession session;
	XR(xrCreateSession(xi, &sci, &session));

	XrReferenceSpaceCreateInfo rsci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	rsci.poseInReferenceSpace.orientation.w = 1;
	rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	XrSpace local, view;
	XR(xrCreateReferenceSpace(session, &rsci, &local));
	rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	XR(xrCreateReferenceSpace(session, &rsci, &view));

	// sRGB formats keep X's sRGB-encoded bytes correct; BGRA first avoids a swizzle.
	int64_t formats[64], prefer[] = {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM,
					 VK_FORMAT_R8G8B8A8_UNORM};
	uint32_t nformats = 0;
	XR(xrEnumerateSwapchainFormats(session, 64, &nformats, formats));
	int64_t format = 0;
	for (unsigned p = 0; p < 4 && !format; p++)
		for (uint32_t i = 0; i < nformats; i++)
			if (formats[i] == prefer[p])
				format = prefer[p];
	if (!format)
		die("swapchain format lookup", 0);
	int dst_bgr = format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM;

	XrSwapchainCreateInfo swci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	swci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
			  XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
	swci.format = format;
	swci.sampleCount = 1;
	swci.width = x.width;
	swci.height = x.height;
	swci.faceCount = 1;
	swci.arraySize = 1;
	swci.mipCount = 1;
	XrSwapchain swapchain;
	XR(xrCreateSwapchain(session, &swci, &swapchain));
	XrSwapchainImageVulkan2KHR images[MAX_IMAGES];
	uint32_t nimages = 0;
	for (int i = 0; i < MAX_IMAGES; i++)
		images[i] = (XrSwapchainImageVulkan2KHR){XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR};
	XR(xrEnumerateSwapchainImages(swapchain, MAX_IMAGES, &nimages, (XrSwapchainImageBaseHeader *)images));
	printf("xscreen: swapchain format %ld, %u images\n", (long)format, nimages);

	XrCompositionLayerQuad quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
	quad.space = local;
	quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	quad.subImage.swapchain = swapchain;
	quad.subImage.imageRect.extent = (XrExtent2Di){(int32_t)x.width, (int32_t)x.height};
	quad.size = (XrExtent2Df){width_m, width_m * x.height / x.width};

	int running = 0, have_image = 0, exit_requested = 0;
	uint64_t frame = 0;
	double start = now_s();
	for (;;) {
		if ((quit_requested || (timeout > 0 && now_s() - start > timeout)) && !exit_requested) {
			exit_requested = 1;
			if (running)
				xrRequestExitSession(session);
			else
				break;
		}

		XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
		while (xrPollEvent(xi, &ev) == XR_SUCCESS) {
			if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
				XrSessionState s = ((XrEventDataSessionStateChanged *)&ev)->state;
				if (s == XR_SESSION_STATE_READY) {
					XrSessionBeginInfo sbi = {XR_TYPE_SESSION_BEGIN_INFO};
					sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					XR(xrBeginSession(session, &sbi));
					running = 1;
				} else if (s == XR_SESSION_STATE_STOPPING) {
					xrEndSession(session);
					running = 0;
					goto done;
				} else if (s == XR_SESSION_STATE_EXITING || s == XR_SESSION_STATE_LOSS_PENDING) {
					goto done;
				}
			} else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
				goto done;
			}
			ev = (XrEventDataBuffer){XR_TYPE_EVENT_DATA_BUFFER};
		}
		if (!running) {
			usleep(10000);
			continue;
		}

		poll_prox(prox_fd);
		XrFrameState fs = {XR_TYPE_FRAME_STATE};
		XR(xrWaitFrame(session, NULL, &fs));
		XR(xrBeginFrame(session, NULL));

		if (recenter_requested) {
			recenter_requested = 0;
			quad.pose = place_quad(view, local, fs.predictedDisplayTime, dist);
		}
		// refresh at half the display rate: plenty for a desktop UI, halves the copy cost
		if (fs.shouldRender && (!have_image || frame % 2 == 0)) {
			uint32_t idx;
			XR(xrAcquireSwapchainImage(swapchain, NULL, &idx));
			XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			wi.timeout = XR_INFINITE_DURATION;
			XR(xrWaitSwapchainImage(swapchain, &wi));
			xwd_copy(&x, g.staging_map, dst_bgr);
			gfx_upload(&g, images[idx].image, x.width, x.height);
			XR(xrReleaseSwapchainImage(swapchain, NULL));
			have_image = 1;
		}
		frame++;

		const XrCompositionLayerBaseHeader *layers[] = {(XrCompositionLayerBaseHeader *)&quad};
		XrFrameEndInfo fei = {XR_TYPE_FRAME_END_INFO};
		fei.displayTime = fs.predictedDisplayTime;
		fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		fei.layerCount = fs.shouldRender && have_image ? 1 : 0;
		fei.layers = layers;
		XR(xrEndFrame(session, &fei));
	}
done:
	printf("xscreen: exiting after %lu frames\n", (unsigned long)frame);
	xrDestroySwapchain(swapchain);
	xrDestroySession(session);
	xrDestroyInstance(xi);
	return 0;
}
