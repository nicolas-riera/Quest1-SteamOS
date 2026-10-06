// test_wsi_x11: present through quest1_vkshim's X11 WSI and check the window contents.
// Resolves everything through dlopen("libvulkan.so.1") + vkGetInstanceProcAddr like ANGLE/volk.
//   QUEST1_VKSHIM_WSI=1 QUEST1_VKSHIM_LAYER=none LD_LIBRARY_PATH=<shim dir>
//   LD_PRELOAD=/opt/hybris/lib/libbionictls.so DISPLAY=:2 ./test_wsi_x11 [frames] [reload]
// Clears each frame to a different color, then reads the window back with GetImage.
#define VK_USE_PLATFORM_XCB_KHR
#include <vulkan/vulkan.h>
#include <xcb/xcb.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(x)                                                                                                       \
	do {                                                                                                           \
		VkResult r_ = (x);                                                                                     \
		if (r_ < 0) {                                                                                          \
			fprintf(stderr, "FAIL %s = %d (line %d)\n", #x, r_, __LINE__);                               \
			exit(1);                                                                                       \
		}                                                                                                      \
	} while (0)

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkDevice dev;
#define IF(n) PFN_vk##n n = (PFN_vk##n)gipa(inst, "vk" #n)
#define DF(n) PFN_vk##n n = (PFN_vk##n)gdpa(dev, "vk" #n)

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
	int frames = argc > 1 ? atoi(argv[1]) : 120;
	const uint32_t W = 640, H = 400;
	void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
	if (!lib) {
		fprintf(stderr, "%s\n", dlerror());
		return 1;
	}
	gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
	if (argc > 2 && strcmp(argv[2], "reload") == 0) {
		// like Chromium: GPU info collection creates and destroys an instance and unloads Vulkan,
		// then ANGLE loads it again
		VkInstanceCreateInfo ci0 = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		CHECK(((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ci0, NULL, &inst));
		((PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance"))(inst, NULL);
		dlclose(lib);
		lib = dlopen("libvulkan.so.1", RTLD_NOW);
		gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
		printf("reloaded\n");
	}

	const char *iexts[] = {"VK_KHR_surface", "VK_KHR_xcb_surface"};
	VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "test_wsi_x11", 1, NULL, 0, VK_API_VERSION_1_1};
	VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai, 0, NULL, 2, iexts};
	CHECK(((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &inst));
	IF(EnumeratePhysicalDevices);
	IF(CreateDevice);
	IF(CreateXcbSurfaceKHR);
	IF(GetPhysicalDeviceSurfaceCapabilitiesKHR);
	IF(GetPhysicalDeviceSurfaceFormatsKHR);
	IF(GetPhysicalDeviceSurfaceSupportKHR);
	IF(DestroySurfaceKHR);
	IF(DestroyInstance);
	PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
	uint32_t n = 1;
	VkPhysicalDevice pd;
	CHECK(EnumeratePhysicalDevices(inst, &n, &pd));

	xcb_connection_t *c = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(c)) {
		fprintf(stderr, "no X\n");
		return 1;
	}
	xcb_screen_t *scr = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
	xcb_window_t win = xcb_generate_id(c);
	xcb_create_window(c, XCB_COPY_FROM_PARENT, win, scr->root, 0, 0, W, H, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
	                  scr->root_visual, 0, NULL);
	xcb_map_window(c, win);
	xcb_flush(c);

	VkSurfaceKHR surf;
	VkXcbSurfaceCreateInfoKHR sci = {VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR, NULL, 0, c, win};
	CHECK(CreateXcbSurfaceKHR(inst, &sci, NULL, &surf));
	VkBool32 sup = 0;
	CHECK(GetPhysicalDeviceSurfaceSupportKHR(pd, 0, surf, &sup));
	VkSurfaceCapabilitiesKHR caps;
	CHECK(GetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps));
	VkSurfaceFormatKHR fmts[8];
	n = 8;
	CHECK(GetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &n, fmts));
	printf("support %u, extent %ux%u, images %u..%u, %u formats (first %d)\n", sup, caps.currentExtent.width,
	       caps.currentExtent.height, caps.minImageCount, caps.maxImageCount, n, fmts[0].format);

	float prio = 1;
	VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
	const char *dexts[] = {"VK_KHR_swapchain"};
	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 1, dexts, NULL};
	CHECK(CreateDevice(pd, &dci, NULL, &dev));
	DF(GetDeviceQueue);
	DF(CreateSwapchainKHR);
	DF(GetSwapchainImagesKHR);
	DF(AcquireNextImageKHR);
	DF(QueuePresentKHR);
	DF(DestroySwapchainKHR);
	DF(CreateCommandPool);
	DF(AllocateCommandBuffers);
	DF(BeginCommandBuffer);
	DF(EndCommandBuffer);
	DF(CmdPipelineBarrier);
	DF(CmdClearColorImage);
	DF(QueueSubmit);
	DF(CreateSemaphore);
	DF(CreateFence);
	DF(WaitForFences);
	DF(ResetFences);
	DF(DeviceWaitIdle);
	DF(DestroyDevice);
	DF(ResetCommandBuffer);
	VkQueue q;
	GetDeviceQueue(dev, 0, 0, &q);

	VkSwapchainCreateInfoKHR swci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
	swci.surface = surf;
	swci.minImageCount = 3;
	swci.imageFormat = fmts[0].format;
	swci.imageColorSpace = fmts[0].colorSpace;
	swci.imageExtent = caps.currentExtent;
	swci.imageArrayLayers = 1;
	swci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	swci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	swci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	swci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	swci.clipped = VK_TRUE;
	VkSwapchainKHR sc;
	CHECK(CreateSwapchainKHR(dev, &swci, NULL, &sc));
	VkImage imgs[8];
	n = 8;
	CHECK(GetSwapchainImagesKHR(dev, sc, &n, imgs));

	VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
	                               VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0};
	VkCommandPool pool;
	CHECK(CreateCommandPool(dev, &pci, NULL, &pool));
	VkCommandBuffer cmd;
	VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool,
	                                   VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
	CHECK(AllocateCommandBuffers(dev, &cai, &cmd));
	VkSemaphoreCreateInfo semci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	VkSemaphore acq, done;
	CHECK(CreateSemaphore(dev, &semci, NULL, &acq));
	CHECK(CreateSemaphore(dev, &semci, NULL, &done));
	VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT};
	VkFence fence;
	CHECK(CreateFence(dev, &fci, NULL, &fence));

	VkClearColorValue col = {{0}};
	double t0 = now();
	for (int f = 0; f < frames; f++) {
		uint32_t idx;
		CHECK(AcquireNextImageKHR(dev, sc, UINT64_MAX, acq, VK_NULL_HANDLE, &idx));
		CHECK(WaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
		CHECK(ResetFences(dev, 1, &fence));
		ResetCommandBuffer(cmd, 0);
		VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		BeginCommandBuffer(cmd, &bi);
		VkImageSubresourceRange rng = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
		                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                          VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, imgs[idx], rng};
		CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
		                   NULL, 1, &b);
		// last frame: pure red (B8G8R8A8: pixel 0x00ff0000 in the X image)
		if (f == frames - 1)
			col = (VkClearColorValue){{1, 0, 0, 1}};
		else
			col = (VkClearColorValue){{0, (f % 60) / 60.0f, 1 - (f % 60) / 60.0f, 1}};
		CmdClearColorImage(cmd, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col, 1, &rng);
		b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		b.dstAccessMask = 0;
		b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0,
		                   NULL, 1, &b);
		EndCommandBuffer(cmd);
		VkPipelineStageFlags st = VK_PIPELINE_STAGE_TRANSFER_BIT;
		VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 1, &acq, &st, 1, &cmd, 1, &done};
		CHECK(QueueSubmit(q, 1, &si, fence));
		VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &done, 1, &sc, &idx, NULL};
		CHECK(QueuePresentKHR(q, &pi));
	}
	DeviceWaitIdle(dev);
	double dt = now() - t0;
	// the presentation thread may still be uploading the last frame: destroying the swapchain joins it
	DestroySwapchainKHR(dev, sc, NULL);
	printf("%d frames in %.2f s = %.1f fps\n", frames, dt, frames / dt);

	xcb_get_image_reply_t *img = xcb_get_image_reply(
	    c, xcb_get_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP, win, W / 2, H / 2, 1, 1, ~0u), NULL);
	uint32_t px = img ? *(uint32_t *)xcb_get_image_data(img) & 0xffffff : 0xdeadbeef;
	printf("center pixel 0x%06x: %s\n", px, px == 0xff0000 ? "PASS" : "FAIL");
	free(img);
	DestroyDevice(dev, NULL);
	DestroySurfaceKHR(inst, surf, NULL);
	DestroyInstance(inst, NULL);
	xcb_disconnect(c);
	return px == 0xff0000 ? 0 : 1;
}
