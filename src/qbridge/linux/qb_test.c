/*
 * qb_test: end-to-end check of the bridge. Renders with the Quest's Vulkan
 * driver (through libhybris) into the app's AHardwareBuffers, driven by the
 * tracking/input it receives. Eye colours follow the triggers, a square
 * follows the right thumbstick.
 *
 * Run inside the Holo chroot:
 *   LD_PRELOAD=/opt/hybris/lib/libbionictls.so LD_LIBRARY_PATH=/opt/hybris/lib ./qb_test
 */
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "qb_link.h"

#define CHECK(x)                                                                 \
	do {                                                                         \
		VkResult r_ = (x);                                                       \
		if (r_ != VK_SUCCESS) {                                                  \
			fprintf(stderr, "%s failed: %d (line %d)\n", #x, r_, __LINE__);      \
			exit(1);                                                             \
		}                                                                        \
	} while (0)

static VkInstance inst;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam;
static VkCommandPool pool;
static VkRenderPass rpass;
static PFN_vkGetAndroidHardwareBufferPropertiesANDROID p_getAhbProps;

struct slot {
	VkImage image;
	VkDeviceMemory mem;
	VkImageView view;
	VkFramebuffer fb;
	VkCommandBuffer cmd;
	VkFence fence;
};
static struct slot slots[QB_MAX_SLOTS];
static uint64_t imported_gen;
static volatile sig_atomic_t quit;

static void on_sig(int s) { (void)s; quit = 1; }

static void init_vulkan(void)
{
	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "qb_test";
	app.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	CHECK(vkCreateInstance(&ici, NULL, &inst));
	uint32_t n = 1;
	vkEnumeratePhysicalDevices(inst, &n, &phys);
	VkPhysicalDeviceProperties pp;
	vkGetPhysicalDeviceProperties(phys, &pp);
	fprintf(stderr, "GPU: %s, Vulkan %u.%u\n", pp.deviceName, VK_VERSION_MAJOR(pp.apiVersion),
	        VK_VERSION_MINOR(pp.apiVersion));

	VkQueueFamilyProperties qf[8];
	n = 8;
	vkGetPhysicalDeviceQueueFamilyProperties(phys, &n, qf);
	for (qfam = 0; qfam < n && !(qf[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT); qfam++)
		;
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = qfam;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;
	const char *exts[] = {VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
	                      VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME};
	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.enabledExtensionCount = 2;
	dci.ppEnabledExtensionNames = exts;
	CHECK(vkCreateDevice(phys, &dci, NULL, &dev));
	vkGetDeviceQueue(dev, qfam, 0, &queue);
	p_getAhbProps = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)vkGetDeviceProcAddr(
	    dev, "vkGetAndroidHardwareBufferPropertiesANDROID");

	VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpi.queueFamilyIndex = qfam;
	CHECK(vkCreateCommandPool(dev, &cpi, NULL, &pool));

	VkAttachmentDescription att = {0};
	att.format = VK_FORMAT_R8G8B8A8_UNORM;
	att.samples = VK_SAMPLE_COUNT_1_BIT;
	att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	att.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
	VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription sp = {0};
	sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	sp.colorAttachmentCount = 1;
	sp.pColorAttachments = &ref;
	VkRenderPassCreateInfo rpi = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
	rpi.attachmentCount = 1;
	rpi.pAttachments = &att;
	rpi.subpassCount = 1;
	rpi.pSubpasses = &sp;
	CHECK(vkCreateRenderPass(dev, &rpi, NULL, &rpass));
}

static void destroy_slots(void)
{
	vkDeviceWaitIdle(dev);
	for (int i = 0; i < QB_MAX_SLOTS; i++) {
		struct slot *s = &slots[i];
		if (!s->image)
			continue;
		vkDestroyFence(dev, s->fence, NULL);
		vkFreeCommandBuffers(dev, pool, 1, &s->cmd);
		vkDestroyFramebuffer(dev, s->fb, NULL);
		vkDestroyImageView(dev, s->view, NULL);
		vkDestroyImage(dev, s->image, NULL);
		vkFreeMemory(dev, s->mem, NULL);
		memset(s, 0, sizeof(*s));
	}
}

static void import_slots(struct qb_link *l)
{
	destroy_slots();
	for (int i = 0; i < QB_MAX_SLOTS; i++) {
		struct slot *s = &slots[i];
		VkAndroidHardwareBufferFormatPropertiesANDROID fmt = {
		    VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
		VkAndroidHardwareBufferPropertiesANDROID props = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
		                                                  &fmt};
		CHECK(p_getAhbProps(dev, l->ahb[i], &props));
		if (i == 0)
			fprintf(stderr, "AHB: format %d, size %llu, memory types %#x\n", fmt.format,
			        (unsigned long long)props.allocationSize, props.memoryTypeBits);

		VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
		ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
		VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext};
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = VK_FORMAT_R8G8B8A8_UNORM;
		ici.extent = (VkExtent3D){l->buffers.width, l->buffers.height, 1};
		ici.mipLevels = 1;
		ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		CHECK(vkCreateImage(dev, &ici, NULL, &s->image));

		VkImportAndroidHardwareBufferInfoANDROID imp = {VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
		imp.buffer = l->ahb[i];
		VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &imp};
		ded.image = s->image;
		VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded};
		mai.allocationSize = props.allocationSize;
		mai.memoryTypeIndex = __builtin_ctz(props.memoryTypeBits);
		CHECK(vkAllocateMemory(dev, &mai, NULL, &s->mem));
		CHECK(vkBindImageMemory(dev, s->image, s->mem, 0));

		VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vci.image = s->image;
		vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vci.format = VK_FORMAT_R8G8B8A8_UNORM;
		vci.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		CHECK(vkCreateImageView(dev, &vci, NULL, &s->view));
		VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
		fci.renderPass = rpass;
		fci.attachmentCount = 1;
		fci.pAttachments = &s->view;
		fci.width = l->buffers.width;
		fci.height = l->buffers.height;
		fci.layers = 1;
		CHECK(vkCreateFramebuffer(dev, &fci, NULL, &s->fb));

		VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		cai.commandPool = pool;
		cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cai.commandBufferCount = 1;
		CHECK(vkAllocateCommandBuffers(dev, &cai, &s->cmd));
		VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		CHECK(vkCreateFence(dev, &fi, NULL, &s->fence));
	}
	imported_gen = l->generation;
	fprintf(stderr, "imported %d AHardwareBuffers into Vulkan\n", QB_MAX_SLOTS);
}

static VkClearRect rect(int x, int y, int w, int h)
{
	return (VkClearRect){{{x, y}, {(uint32_t)w, (uint32_t)h}}, 0, 1};
}

static void clear(VkCommandBuffer c, float r, float g, float b, VkClearRect rc)
{
	VkClearAttachment a = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {.float32 = {r, g, b, 1.0f}}}};
	vkCmdClearAttachments(c, 1, &a, 1, &rc);
}

static void render(struct qb_link *l, int si)
{
	struct slot *s = &slots[si];
	const struct qb_pose_msg *p = &l->pose;
	int w = l->buffers.width, h = l->buffers.height, ew = w / 2;
	VkCommandBuffer c = s->cmd;
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(c, &bi);
	VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
	rbi.renderPass = rpass;
	rbi.framebuffer = s->fb;
	rbi.renderArea = (VkRect2D){{0, 0}, {(uint32_t)w, (uint32_t)h}};
	vkCmdBeginRenderPass(c, &rbi, VK_SUBPASS_CONTENTS_INLINE);

	float lt = p->controller[0].trigger, rt = p->controller[1].trigger;
	float sx = p->controller[1].thumbstick_x, sy = p->controller[1].thumbstick_y;
	bool btn = (p->controller[0].buttons | p->controller[1].buttons) & (QB_BTN_A | QB_BTN_B | QB_BTN_X | QB_BTN_Y);
	for (int e = 0; e < 2; e++) {
		int x0 = e * ew;
		clear(c, 0.1f + 0.8f * lt, 0.15f, 0.1f + 0.8f * rt, rect(x0, 0, ew, h));
		/* centre cross */
		clear(c, 1, 1, 1, rect(x0 + ew / 2 - 2, h / 2 - 40, 4, 80));
		clear(c, 1, 1, 1, rect(x0 + ew / 2 - 40, h / 2 - 2, 80, 4));
		/* square driven by the right stick (image rows go top-down) */
		int qx = x0 + ew / 2 + (int)(sx * ew * 0.35f) - 50;
		int qy = h / 2 - (int)(sy * h * 0.35f) - 50;
		clear(c, btn ? 1.0f : 0.1f, 0.9f, 0.2f, rect(qx, qy, 100, 100));
	}
	vkCmdEndRenderPass(c);

	/* Hand the image over to the app's GL context. */
	VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	b.srcQueueFamilyIndex = qfam;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
	b.image = s->image;
	b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
	                     0, NULL, 0, NULL, 1, &b);
	vkEndCommandBuffer(c);

	VkSubmitInfo sub = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	sub.commandBufferCount = 1;
	sub.pCommandBuffers = &c;
	vkResetFences(dev, 1, &s->fence);
	CHECK(vkQueueSubmit(queue, 1, &sub, s->fence));
	vkWaitForFences(dev, 1, &s->fence, VK_TRUE, UINT64_MAX);
}

int main(void)
{
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	init_vulkan();
	struct qb_link l;
	int r = qb_link_init(&l);
	if (r) {
		fprintf(stderr, "qb_link_init: %s\n", strerror(-r));
		return 1;
	}
	fprintf(stderr, "waiting for the qbridge app...\n");
	uint64_t frames = 0;
	time_t last = time(NULL);
	while (!quit) {
		if (!qb_link_accept(&l, 500))
			continue;
		if (imported_gen != l.generation)
			import_slots(&l);
		if (!qb_link_wait_pose(&l, 100) || !l.have_pose)
			continue;
		int si = qb_link_acquire(&l);
		if (si < 0)
			continue;
		render(&l, si);
		qb_link_present(&l, si, l.pose.seq);
		frames++;
		if (time(NULL) != last) {
			last = time(NULL);
			fprintf(stderr, "frames %llu | head %.2f %.2f %.2f | trig %.2f %.2f\n", (unsigned long long)frames,
			        l.pose.head.position.x, l.pose.head.position.y, l.pose.head.position.z,
			        l.pose.controller[0].trigger, l.pose.controller[1].trigger);
		}
	}
	destroy_slots();
	qb_link_fini(&l);
	return 0;
}
