// test_facet: exercises the layer's Facet emulation (quest1_facet.c): VK_KHR_dynamic_rendering through
// render passes (clear pass, readback) and VK_EXT_host_query_reset (one-shot reset).
// Run with QUEST1_STEAMVR_COMPAT=1 QUEST1_FACET=1 like test_timeline (lavapipe in WSL or the headset).
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s = %d\n", #x, r_); exit(1); } } while (0)

int main(void)
{
	void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
	if (!lib) { fprintf(stderr, "%s\n", dlerror()); return 1; }
	PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
	VkInstance inst;
	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	CHECK(((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &inst));
#define I(n) PFN_##n n = (PFN_##n)gipa(inst, #n)
	I(vkEnumeratePhysicalDevices); I(vkEnumerateDeviceExtensionProperties); I(vkCreateDevice); I(vkGetDeviceProcAddr);
	I(vkGetPhysicalDeviceMemoryProperties); I(vkGetPhysicalDeviceFeatures2);
	uint32_t n = 1;
	VkPhysicalDevice pd;
	vkEnumeratePhysicalDevices(inst, &n, &pd);
	uint32_t ne = 0;
	vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
	VkExtensionProperties *ep = calloc(ne, sizeof(*ep));
	vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ep);
	int dr = 0, hq = 0;
	for (uint32_t i = 0; i < ne; i++) {
		dr |= !strcmp(ep[i].extensionName, "VK_KHR_dynamic_rendering");
		hq |= !strcmp(ep[i].extensionName, "VK_EXT_host_query_reset");
	}
	printf("dynamic_rendering %d host_query_reset %d\n", dr, hq);
	VkPhysicalDeviceHostQueryResetFeatures hqf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES};
	VkPhysicalDeviceDynamicRenderingFeatures drf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, &hqf};
	VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &drf};
	vkGetPhysicalDeviceFeatures2(pd, &f2);
	printf("features: dynamicRendering %u hostQueryReset %u\n", drf.dynamicRendering, hqf.hostQueryReset);
	float prio = 1;
	VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
	const char *ex[] = {"VK_KHR_dynamic_rendering", "VK_EXT_host_query_reset"};
	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &qi, 0, NULL, 2, ex};
	VkDevice dev;
	CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
#define D(n) PFN_##n n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
	D(vkGetDeviceQueue); D(vkCreateImage); D(vkGetImageMemoryRequirements); D(vkAllocateMemory); D(vkBindImageMemory);
	D(vkCreateImageView); D(vkCreateBuffer); D(vkGetBufferMemoryRequirements); D(vkBindBufferMemory); D(vkMapMemory);
	D(vkCreateCommandPool); D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer); D(vkEndCommandBuffer);
	D(vkCmdBeginRenderingKHR); D(vkCmdEndRenderingKHR); D(vkCmdCopyImageToBuffer); D(vkQueueSubmit); D(vkQueueWaitIdle);
	D(vkCreateQueryPool); D(vkResetQueryPoolEXT); D(vkGetQueryPoolResults); D(vkCmdWriteTimestamp); D(vkCmdPipelineBarrier);
	VkQueue q;
	vkGetDeviceQueue(dev, 0, 0, &q);
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {64, 64, 1}, 1, 1,
	                        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
	                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
	                        VK_IMAGE_LAYOUT_UNDEFINED};
	VkImage im;
	CHECK(vkCreateImage(dev, &ic, NULL, &im));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(dev, im, &mr);
	uint32_t t = 0;
	while (!(mr.memoryTypeBits & (1u << t)) || !(mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) t++;
	VkMemoryAllocateInfo ma = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, t};
	VkDeviceMemory mem;
	CHECK(vkAllocateMemory(dev, &ma, NULL, &mem));
	CHECK(vkBindImageMemory(dev, im, mem, 0));
	VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, im, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
	                            {0, 0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	VkImageView view;
	CHECK(vkCreateImageView(dev, &vi, NULL, &view));
	VkBufferCreateInfo bc = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 64 * 64 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                         VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
	VkBuffer buf;
	CHECK(vkCreateBuffer(dev, &bc, NULL, &buf));
	vkGetBufferMemoryRequirements(dev, buf, &mr);
	t = 0;
	while (!(mr.memoryTypeBits & (1u << t)) || (mp.memoryTypes[t].propertyFlags & 6) != 6) t++;
	ma.allocationSize = mr.size;
	ma.memoryTypeIndex = t;
	VkDeviceMemory bm;
	CHECK(vkAllocateMemory(dev, &ma, NULL, &bm));
	CHECK(vkBindBufferMemory(dev, buf, bm, 0));
	uint8_t *px;
	CHECK(vkMapMemory(dev, bm, 0, VK_WHOLE_SIZE, 0, (void **)&px));
	memset(px, 0, 64 * 64 * 4);
	VkQueryPoolCreateInfo qp = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, 2};
	VkQueryPool pool;
	CHECK(vkCreateQueryPool(dev, &qp, NULL, &pool));
	vkResetQueryPoolEXT(dev, pool, 0, 2);
	VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, 0};
	VkCommandPool cp;
	CHECK(vkCreateCommandPool(dev, &cpi, NULL, &cp));
	VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
	VkCommandBuffer cb;
	CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	CHECK(vkBeginCommandBuffer(cb, &bi));
	VkImageMemoryBarrier b0 = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	                           VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 0, im,
	                           {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &b0);
	vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool, 0);
	VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	                                VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_CLEAR,
	                                VK_ATTACHMENT_STORE_OP_STORE, {.color = {{0.25f, 0.5f, 0.75f, 1.0f}}}};
	VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {64, 64}}, 1, 0, 1, &ca, NULL, NULL};
	vkCmdBeginRenderingKHR(cb, &ri);
	vkCmdEndRenderingKHR(cb);
	vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, 1);
	VkImageMemoryBarrier b1 = b0;
	b1.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	b1.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	b1.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
	VkBufferImageCopy rg = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {64, 64, 1}};
	vkCmdCopyImageToBuffer(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &rg);
	CHECK(vkEndCommandBuffer(cb));
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb};
	CHECK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
	CHECK(vkQueueWaitIdle(q));
	uint64_t ts[2];
	CHECK(vkGetQueryPoolResults(dev, pool, 0, 2, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT));
	uint8_t *p = px + (10 * 64 + 10) * 4;
	printf("pixel(10,10) = %u %u %u %u (want 64 128 191 255), timestamps %s\n", p[0], p[1], p[2], p[3], ts[1] >= ts[0] ? "ok" : "BAD");
	int ok = p[3] == 255 && abs(p[0] - 64) <= 1;
	puts(ok ? "PASS" : "FAIL");
	return !ok;
}
