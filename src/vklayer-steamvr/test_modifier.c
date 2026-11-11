// test_modifier: a venus decoder frame imported as Steam Link VR (vrlink) does it, through the layer's
// VK_EXT_external_memory_dma_buf and VK_EXT_image_drm_format_modifier (quest1_modifier.c).
//
// An ION buffer laid out like a venus NV12 frame (rows of the width rounded up to 128, the UV plane after the Y rows
// rounded up to 32) gets a pattern; an image with the linear modifier and those plane layouts is created on it
// (VkImageDrmFormatModifierExplicitCreateInfoEXT, DMA_BUF external memory), its memory imported from the fd
// (vkGetMemoryFdPropertiesKHR, VkImportMemoryFdInfoKHR), and each plane copied to a buffer and compared.
//
// Build (on the headset): gcc -O2 -Wall -o test_modifier test_modifier.c -ldl
// Run as the Steam user with Steam's environment (LD_PRELOAD, LD_LIBRARY_PATH, QUEST1_VK*) and
// QUEST1_VK_DMABUF=1 (the extensions are advertised to vrlink only): ./test_modifier [WxH ...]
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define AL(x, a) (((x) + (a)-1) / (a) * (a))
#define CHECK(x)                                                                                                       \
	do {                                                                                                           \
		VkResult r_ = (x);                                                                                     \
		if (r_ != VK_SUCCESS) {                                                                                \
			fprintf(stderr, "%s = %d\n", #x, r_);                                                          \
			return 1;                                                                                      \
		}                                                                                                      \
	} while (0)

struct ion_allocation_data {
	size_t len, align;
	unsigned int heap_id_mask, flags;
	int handle;
};
struct ion_fd_data {
	int handle, fd;
};
#define ION_IOC_ALLOC _IOWR('I', 0, struct ion_allocation_data)
#define ION_IOC_FREE _IOWR('I', 1, int)
#define ION_IOC_SHARE _IOWR('I', 4, struct ion_fd_data)
#define ION_SYSTEM_HEAP_ID 25

static int ion_buffer(size_t len)
{
	int ion = open("/dev/ion", O_RDONLY | O_CLOEXEC);
	struct ion_allocation_data a = {.len = len, .align = 4096, .heap_id_mask = 1u << ION_SYSTEM_HEAP_ID};
	if (ion < 0 || ioctl(ion, ION_IOC_ALLOC, &a) < 0)
		return -1;
	struct ion_fd_data f = {.handle = a.handle};
	int r = ioctl(ion, ION_IOC_SHARE, &f);
	ioctl(ion, ION_IOC_FREE, &a.handle);
	close(ion);
	return r < 0 ? -1 : f.fd;
}

static uint8_t pat(int plane, unsigned x, unsigned y) { return plane == 0 ? (uint8_t)(x + 3 * y) : (uint8_t)(5 * x + y + 1); }

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkDevice dev;
#define I(n) PFN_##n n = (PFN_##n)gipa(inst, #n)

// one frame of w x h in format f (NV12 or R8): 0 if it reads back exact
static int run(VkPhysicalDevice pd, uint32_t qf, VkFormat f, unsigned w, unsigned h)
{
	I(vkGetPhysicalDeviceFormatProperties2);
	I(vkGetPhysicalDeviceImageFormatProperties2);
	I(vkCreateImage);
	I(vkDestroyImage);
	I(vkGetImageMemoryRequirements);
	I(vkAllocateMemory);
	I(vkFreeMemory);
	I(vkBindImageMemory);
	I(vkCreateBuffer);
	I(vkDestroyBuffer);
	I(vkGetBufferMemoryRequirements);
	I(vkBindBufferMemory);
	I(vkMapMemory);
	I(vkGetPhysicalDeviceMemoryProperties);
	I(vkCreateCommandPool);
	I(vkDestroyCommandPool);
	I(vkAllocateCommandBuffers);
	I(vkBeginCommandBuffer);
	I(vkEndCommandBuffer);
	I(vkCmdPipelineBarrier);
	I(vkCmdCopyImageToBuffer);
	I(vkGetDeviceQueue);
	I(vkQueueSubmit);
	I(vkQueueWaitIdle);
	I(vkGetDeviceProcAddr);
	PFN_vkGetMemoryFdPropertiesKHR fdprops = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdPropertiesKHR");
	PFN_vkGetImageDrmFormatModifierPropertiesEXT modprops = (PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkGetDeviceProcAddr(
	    dev, "vkGetImageDrmFormatModifierPropertiesEXT");
	const int nv12 = f == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	printf("%s %ux%u: ", nv12 ? "NV12" : "R8", w, h);

	VkDrmFormatModifierPropertiesEXT mp[4];
	VkDrmFormatModifierPropertiesListEXT ml = {VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT, NULL, 4, mp};
	VkFormatProperties2 fp = {VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &ml};
	vkGetPhysicalDeviceFormatProperties2(pd, f, &fp);
	if (ml.drmFormatModifierCount != 1 || mp[0].drmFormatModifier != 0) {
		printf("no linear modifier listed (%u)\n", ml.drmFormatModifierCount);
		return 1;
	}
	VkPhysicalDeviceImageDrmFormatModifierInfoEXT mi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
	VkPhysicalDeviceExternalImageFormatInfo ei = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, &mi,
	                                              VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
	VkPhysicalDeviceImageFormatInfo2 ifi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &ei, f, VK_IMAGE_TYPE_2D,
	                                        VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
	                                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
	VkExternalImageFormatProperties ep = {VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
	VkImageFormatProperties2 ip = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &ep};
	CHECK(vkGetPhysicalDeviceImageFormatProperties2(pd, &ifi, &ip));
	if (!(ep.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
		printf("dma-buf not importable\n");
		return 1;
	}

	// the frame, as venus lays it out
	const unsigned stride = AL(w, 128), scan = AL(h, 32);
	const size_t uv = (size_t)stride * scan, size = AL(nv12 ? uv + (size_t)stride * AL(h / 2, 16) : uv, 4096);
	int fd = ion_buffer(size);
	uint8_t *m = fd >= 0 ? mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
	if (m == MAP_FAILED) {
		printf("no ION buffer\n");
		return 1;
	}
	for (unsigned y = 0; y < h; y++)
		for (unsigned x = 0; x < w; x++)
			m[(size_t)y * stride + x] = pat(0, x, y);
	for (unsigned y = 0; nv12 && y < h / 2; y++)
		for (unsigned x = 0; x < w; x++)
			m[uv + (size_t)y * stride + x] = pat(1, x, y);
	munmap(m, size);

	VkSubresourceLayout planes[2] = {{0, 0, stride, 0, 0}, {uv, 0, stride, 0, 0}};
	VkImageDrmFormatModifierExplicitCreateInfoEXT ex = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
	                                                     NULL, 0, nv12 ? 2 : 1, planes};
	VkExternalMemoryImageCreateInfo emi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &ex,
	                                       VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
	VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &emi};
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = f;
	ici.extent = (VkExtent3D){w, h, 1};
	ici.mipLevels = ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
	ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkImage img;
	CHECK(vkCreateImage(dev, &ici, NULL, &img));
	VkImageDrmFormatModifierPropertiesEXT imp = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT};
	CHECK(modprops(dev, img, &imp));

	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(dev, img, &req);
	VkMemoryFdPropertiesKHR mfp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
	CHECK(fdprops(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &mfp));
	if (!(req.memoryTypeBits & mfp.memoryTypeBits)) {
		printf("no memory type for both the image (0x%x) and the fd (0x%x)\n", req.memoryTypeBits, mfp.memoryTypeBits);
		return 1;
	}
	VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, img};
	VkImportMemoryFdInfoKHR imf = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, &ded,
	                               VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd}; // the import owns fd
	VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imf, req.size,
	                            (uint32_t)__builtin_ctz(req.memoryTypeBits & mfp.memoryTypeBits)};
	VkDeviceMemory mem;
	CHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
	CHECK(vkBindImageMemory(dev, img, mem, 0));

	// each plane to a host-visible buffer, tightly packed
	const size_t ysize = (size_t)w * h, total = nv12 ? ysize + ysize / 2 : ysize;
	VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, total, VK_BUFFER_USAGE_TRANSFER_DST_BIT};
	VkBuffer buf;
	CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
	VkMemoryRequirements breq;
	vkGetBufferMemoryRequirements(dev, buf, &breq);
	VkPhysicalDeviceMemoryProperties mprops;
	vkGetPhysicalDeviceMemoryProperties(pd, &mprops);
	uint32_t t = 0;
	while (t < mprops.memoryTypeCount &&
	       !((breq.memoryTypeBits >> t & 1) && (mprops.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
	         (mprops.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)))
		t++;
	VkMemoryAllocateInfo bai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, breq.size, t};
	VkDeviceMemory bmem;
	CHECK(vkAllocateMemory(dev, &bai, NULL, &bmem));
	CHECK(vkBindBufferMemory(dev, buf, bmem, 0));

	VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qf};
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));
	VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool,
	                                   VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
	VkCommandBuffer cb;
	CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	CHECK(vkBeginCommandBuffer(cb, &bi));
	VkImageAspectFlags all = nv12 ? VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_READ_BIT,
	                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_FOREIGN_EXT,
	                          qf, img, {all, 0, 1, 0, 1}};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
	VkBufferImageCopy rg[2] = {
	    {0, 0, 0, {nv12 ? VK_IMAGE_ASPECT_PLANE_0_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}},
	    {ysize, 0, 0, {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1}, {0, 0, 0}, {w / 2, h / 2, 1}}};
	vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, nv12 ? 2 : 1, rg);
	CHECK(vkEndCommandBuffer(cb));
	VkQueue q;
	vkGetDeviceQueue(dev, qf, 0, &q);
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb};
	CHECK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
	CHECK(vkQueueWaitIdle(q));

	uint8_t *r;
	CHECK(vkMapMemory(dev, bmem, 0, total, 0, (void **)&r));
	long bad[2] = {0, 0};
	for (unsigned y = 0; y < h; y++)
		for (unsigned x = 0; x < w; x++)
			bad[0] += r[(size_t)y * w + x] != pat(0, x, y);
	for (unsigned y = 0; nv12 && y < h / 2; y++)
		for (unsigned x = 0; x < w; x++)
			bad[1] += r[ysize + (size_t)y * w + x] != pat(1, x, y);
	printf("%ld wrong of %zu (Y), %ld wrong of %zu (UV): %s\n", bad[0], ysize, bad[1], nv12 ? ysize / 2 : 0,
	       bad[0] || bad[1] ? "FAIL" : "PASS");
	vkDestroyCommandPool(dev, pool, NULL);
	vkDestroyBuffer(dev, buf, NULL);
	vkFreeMemory(dev, bmem, NULL);
	vkDestroyImage(dev, img, NULL);
	vkFreeMemory(dev, mem, NULL);
	return bad[0] || bad[1];
}

int main(int argc, char **argv)
{
	void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
	if (!lib) {
		fprintf(stderr, "%s\n", dlerror());
		return 1;
	}
	gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
	PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	CHECK(vkCreateInstance(&ici, NULL, &inst));
	I(vkEnumeratePhysicalDevices);
	I(vkCreateDevice);
	I(vkGetPhysicalDeviceQueueFamilyProperties);
	uint32_t n = 1;
	VkPhysicalDevice pd;
	vkEnumeratePhysicalDevices(inst, &n, &pd);
	uint32_t nq = 8, qf = 0;
	VkQueueFamilyProperties qp[8];
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
	while (qf < nq && !(qp[qf].queueFlags & VK_QUEUE_GRAPHICS_BIT))
		qf++;
	// vrlink's device extensions
	const char *exts[] = {"VK_KHR_external_memory", "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf",
	                      "VK_EXT_image_drm_format_modifier", "VK_KHR_get_memory_requirements2",
	                      "VK_EXT_queue_family_foreign"};
	float prio = 1;
	VkDeviceQueueCreateInfo q = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qf, 1, &prio};
	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &q;
	dci.enabledExtensionCount = sizeof(exts) / sizeof(exts[0]);
	dci.ppEnabledExtensionNames = exts;
	CHECK(vkCreateDevice(pd, &dci, NULL, &dev));

	const char *def[] = {"1920x1080", "2880x1600", "1832x1920"};
	int fails = 0;
	for (int i = 0; i < (argc > 1 ? argc - 1 : 3); i++) {
		unsigned w, h;
		if (sscanf(argc > 1 ? argv[i + 1] : def[i], "%ux%u", &w, &h) != 2)
			continue;
		fails += run(pd, qf, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, w, h) != 0;
		fails += run(pd, qf, VK_FORMAT_R8_UNORM, w, h) != 0;
	}
	printf("%s\n", fails ? "FAIL" : "PASS");
	return fails != 0;
}
