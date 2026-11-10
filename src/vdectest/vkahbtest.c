// vkahbtest: can the Adreno blob sample, with no copy, the kind of buffer the venus decoder fills?
//
// A gralloc buffer (AHardwareBuffer, NV12 in venus's layout: what `vdectest -g` decodes into, bit-exact) gets four
// flat colours written by the CPU, is imported in Vulkan (VK_ANDROID_external_memory_android_hardware_buffer, an
// external format with its sampler YCbCr conversion) and sampled by a compute shader into RGBA; the result is
// compared with the colours the conversion the driver suggests should give. This is the path a decoded video
// frame takes to the compositor without a CPU copy.
//
// Build (on the headset): glslangValidator -V --vn vkahb_spv -o vkahb_spv.h vkahbtest.comp
//                         gcc -O2 -Wall -o vkahbtest vkahbtest.c -L/opt/hybris/lib -lvulkan -ldl
// Run: LD_LIBRARY_PATH=/opt/hybris/lib LD_PRELOAD=/opt/hybris/lib/libbionictls.so ./vkahbtest [WxH]
#define _GNU_SOURCE
#define VK_USE_PLATFORM_ANDROID_KHR
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include "vkahb_spv.h"

struct ahb_desc {
	uint32_t width, height, layers, format;
	uint64_t usage;
	uint32_t stride, rfu0;
	uint64_t rfu1;
};
struct native_handle {
	int version, num_fds, num_ints;
	int data[];
};
#define AHB_FORMAT_Y8Cb8Cr8_420 0x23
#define AHB_USAGE_CPU_READ_OFTEN 3ull
#define AHB_USAGE_GPU_SAMPLED_IMAGE (1ull << 8)
#define AHB_USAGE_VIDEO_ENCODE (1ull << 16)
#define AL(x, a) (((x) + (a)-1) / (a) * (a))

#define CHECK(r, what)                                                       \
	do {                                                                     \
		VkResult r_ = (r);                                                   \
		if (r_ != VK_SUCCESS) {                                              \
			fprintf(stderr, "vkahbtest: %s: VkResult %d\n", what, (int)r_); \
			return 1;                                                        \
		}                                                                    \
	} while (0)

static uint32_t mem_type(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	return UINT32_MAX;
}

static int clamp8(double v)
{
	return v < 0 ? 0 : v > 255 ? 255 : (int)(v + 0.5);
}

int main(int argc, char **argv)
{
	uint32_t w = 1920, h = 1088;
	if (argc > 1)
		sscanf(argv[1], "%ux%u", &w, &h);

	// --- the gralloc buffer
	void *hc = dlopen("/opt/hybris/lib/libhybris-common.so.1", RTLD_NOW | RTLD_GLOBAL);
	void *(*adlopen)(const char *, int) = hc ? (void *(*)(const char *, int))dlsym(hc, "android_dlopen") : NULL;
	void *(*adlsym)(void *, const char *) = hc ? (void *(*)(void *, const char *))dlsym(hc, "android_dlsym") : NULL;
	void *nw = adlopen && adlsym ? adlopen("libnativewindow.so", RTLD_NOW) : NULL;
	if (!nw) {
		fprintf(stderr, "vkahbtest: no libhybris libnativewindow\n");
		return 1;
	}
	int (*ahb_allocate)(const struct ahb_desc *, struct AHardwareBuffer **) = adlsym(nw, "AHardwareBuffer_allocate");
	const struct native_handle *(*ahb_handle)(const struct AHardwareBuffer *) =
	    adlsym(nw, "AHardwareBuffer_getNativeHandle");
	struct ahb_desc d = {.width = w, .height = h, .layers = 1, .format = AHB_FORMAT_Y8Cb8Cr8_420,
	                     .usage = AHB_USAGE_CPU_READ_OFTEN | AHB_USAGE_GPU_SAMPLED_IMAGE | AHB_USAGE_VIDEO_ENCODE};
	struct AHardwareBuffer *ahb = NULL;
	if (ahb_allocate(&d, &ahb) || !ahb) {
		fprintf(stderr, "vkahbtest: AHardwareBuffer_allocate failed\n");
		return 1;
	}
	int fd = ahb_handle(ahb)->data[0];
	off_t len = lseek(fd, 0, SEEK_END);
	uint32_t stride = AL(w, 128), scan = AL(h, 32); // venus's NV12
	uint8_t *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (m == MAP_FAILED || (size_t)len < (size_t)stride * scan * 3 / 2) {
		fprintf(stderr, "vkahbtest: buffer of %lld bytes for %ux%u\n", (long long)len, w, h);
		return 1;
	}
	// four quadrants: Y, Cb, Cr
	static const uint8_t col[4][3] = {{235, 128, 128}, {81, 90, 240}, {145, 54, 34}, {41, 240, 110}};
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++)
			m[(size_t)y * stride + x] = col[(y >= h / 2) * 2 + (x >= w / 2)][0];
	for (uint32_t y = 0; y < h / 2; y++)
		for (uint32_t x = 0; x < w / 2; x++) {
			const uint8_t *c = col[(y >= h / 4) * 2 + (x >= w / 4)];
			m[(size_t)stride * scan + (size_t)y * stride + 2 * x] = c[1];
			m[(size_t)stride * scan + (size_t)y * stride + 2 * x + 1] = c[2];
		}
	msync(m, len, MS_SYNC);

	// --- Vulkan
	VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
	VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai};
	VkInstance inst;
	CHECK(vkCreateInstance(&ici, NULL, &inst), "vkCreateInstance");
	uint32_t n = 1;
	VkPhysicalDevice pd;
	vkEnumeratePhysicalDevices(inst, &n, &pd);
	if (!n)
		return 1;
	uint32_t qn = 8;
	VkQueueFamilyProperties qp[8];
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qp);
	uint32_t qf = 0;
	while (qf < qn && !(qp[qf].queueFlags & VK_QUEUE_COMPUTE_BIT))
		qf++;
	static const char *exts[] = {"VK_ANDROID_external_memory_android_hardware_buffer", "VK_EXT_queue_family_foreign"};
	float prio = 1;
	VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qf,
	                               .queueCount = 1, .pQueuePriorities = &prio};
	VkPhysicalDeviceSamplerYcbcrConversionFeatures yf = {
	    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES, .samplerYcbcrConversion = VK_TRUE};
	VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &yf, .queueCreateInfoCount = 1,
	                          .pQueueCreateInfos = &qci, .enabledExtensionCount = 2, .ppEnabledExtensionNames = exts};
	VkDevice dev;
	CHECK(vkCreateDevice(pd, &dci, NULL, &dev), "vkCreateDevice (AHB + YCbCr conversion)");
	VkQueue queue;
	vkGetDeviceQueue(dev, qf, 0, &queue);

	VkResult (*get_props)(VkDevice, const struct AHardwareBuffer *, VkAndroidHardwareBufferPropertiesANDROID *) =
	    (void *)vkGetDeviceProcAddr(dev, "vkGetAndroidHardwareBufferPropertiesANDROID");
	if (!get_props) {
		fprintf(stderr, "vkahbtest: no vkGetAndroidHardwareBufferPropertiesANDROID\n");
		return 1;
	}
	VkAndroidHardwareBufferFormatPropertiesANDROID fp = {
	    .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
	VkAndroidHardwareBufferPropertiesANDROID bp = {.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
	                                               .pNext = &fp};
	CHECK(get_props(dev, ahb, &bp), "vkGetAndroidHardwareBufferPropertiesANDROID");
	fprintf(stderr,
	        "vkahbtest: buffer %ux%u (%lld bytes): Vulkan format %d, external format 0x%llx, features 0x%x, model %d, "
	        "range %d, chroma offsets %d %d, allocation %llu, memory types 0x%x\n",
	        w, h, (long long)len, fp.format, (unsigned long long)fp.externalFormat, fp.formatFeatures,
	        fp.suggestedYcbcrModel, fp.suggestedYcbcrRange, fp.suggestedXChromaOffset, fp.suggestedYChromaOffset,
	        (unsigned long long)bp.allocationSize, bp.memoryTypeBits);

	VkExternalFormatANDROID xf = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID, .externalFormat = fp.externalFormat};
	VkFilter filter = (fp.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT)
	                      ? VK_FILTER_LINEAR
	                      : VK_FILTER_NEAREST;
	VkSamplerYcbcrConversionCreateInfo yci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
	                                          .pNext = &xf,
	                                          .format = VK_FORMAT_UNDEFINED,
	                                          .ycbcrModel = fp.suggestedYcbcrModel,
	                                          .ycbcrRange = fp.suggestedYcbcrRange,
	                                          .components = fp.samplerYcbcrConversionComponents,
	                                          .xChromaOffset = fp.suggestedXChromaOffset,
	                                          .yChromaOffset = fp.suggestedYChromaOffset,
	                                          .chromaFilter = filter};
	VkSamplerYcbcrConversion conv;
	CHECK(vkCreateSamplerYcbcrConversion(dev, &yci, NULL, &conv), "vkCreateSamplerYcbcrConversion");
	VkSamplerYcbcrConversionInfo cinfo = {.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO, .conversion = conv};

	VkExternalFormatANDROID xf2 = xf;
	VkExternalMemoryImageCreateInfo emi = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .pNext = &xf2,
	                                       .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
	VkImageCreateInfo imi = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	                         .pNext = &emi,
	                         .imageType = VK_IMAGE_TYPE_2D,
	                         .format = VK_FORMAT_UNDEFINED,
	                         .extent = {w, h, 1},
	                         .mipLevels = 1,
	                         .arrayLayers = 1,
	                         .samples = VK_SAMPLE_COUNT_1_BIT,
	                         .tiling = VK_IMAGE_TILING_OPTIMAL,
	                         .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
	                         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
	VkImage img;
	CHECK(vkCreateImage(dev, &imi, NULL, &img), "vkCreateImage (external format)");
	VkMemoryDedicatedAllocateInfo ded = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = img};
	VkImportAndroidHardwareBufferInfoANDROID imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
	                                                .pNext = &ded, .buffer = ahb};
	uint32_t mt = 0;
	while (mt < 32 && !(bp.memoryTypeBits & (1u << mt)))
		mt++;
	VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp,
	                            .allocationSize = bp.allocationSize, .memoryTypeIndex = mt};
	VkDeviceMemory imem;
	CHECK(vkAllocateMemory(dev, &mai, NULL, &imem), "vkAllocateMemory (import)");
	CHECK(vkBindImageMemory(dev, img, imem, 0), "vkBindImageMemory");
	VkImageViewCreateInfo ivi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	                             .pNext = &cinfo,
	                             .image = img,
	                             .viewType = VK_IMAGE_VIEW_TYPE_2D,
	                             .format = VK_FORMAT_UNDEFINED,
	                             .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	VkImageView view;
	CHECK(vkCreateImageView(dev, &ivi, NULL, &view), "vkCreateImageView");
	VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
	                           .pNext = &cinfo,
	                           .magFilter = filter,
	                           .minFilter = filter,
	                           .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
	                           .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	                           .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	                           .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
	VkSampler sampler;
	CHECK(vkCreateSampler(dev, &sci, NULL, &sampler), "vkCreateSampler");

	// the result: RGBA words, readable by the CPU
	VkDeviceSize osize = (VkDeviceSize)w * h * 4;
	VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = osize,
	                          .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
	VkBuffer obuf;
	CHECK(vkCreateBuffer(dev, &bci, NULL, &obuf), "vkCreateBuffer");
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(dev, obuf, &mr);
	VkMemoryAllocateInfo omai = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
	    .memoryTypeIndex = mem_type(pd, mr.memoryTypeBits,
	                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
	VkDeviceMemory omem;
	CHECK(vkAllocateMemory(dev, &omai, NULL, &omem), "vkAllocateMemory (result)");
	CHECK(vkBindBufferMemory(dev, obuf, omem, 0), "vkBindBufferMemory");
	uint32_t *out;
	CHECK(vkMapMemory(dev, omem, 0, VK_WHOLE_SIZE, 0, (void **)&out), "vkMapMemory");

	// the compute pass
	VkDescriptorSetLayoutBinding lb[2] = {
	    {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
	     .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .pImmutableSamplers = &sampler},
	    {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
	     .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
	VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
	                                       .bindingCount = 2, .pBindings = lb};
	VkDescriptorSetLayout dsl;
	CHECK(vkCreateDescriptorSetLayout(dev, &dli, NULL, &dsl), "vkCreateDescriptorSetLayout");
	VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 8};
	VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
	                                  .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
	VkPipelineLayout pl;
	CHECK(vkCreatePipelineLayout(dev, &pli, NULL, &pl), "vkCreatePipelineLayout");
	VkShaderModuleCreateInfo smi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(vkahb_spv),
	                                .pCode = vkahb_spv};
	VkShaderModule sm;
	CHECK(vkCreateShaderModule(dev, &smi, NULL, &sm), "vkCreateShaderModule");
	VkComputePipelineCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
	                                   .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	                                             .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main"},
	                                   .layout = pl};
	VkPipeline pipe;
	CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe), "vkCreateComputePipelines");
	VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
	VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
	                                  .poolSizeCount = 2, .pPoolSizes = ps};
	VkDescriptorPool pool;
	CHECK(vkCreateDescriptorPool(dev, &dpi, NULL, &pool), "vkCreateDescriptorPool");
	VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool,
	                                   .descriptorSetCount = 1, .pSetLayouts = &dsl};
	VkDescriptorSet set;
	CHECK(vkAllocateDescriptorSets(dev, &dai, &set), "vkAllocateDescriptorSets");
	VkDescriptorImageInfo dii = {.sampler = sampler, .imageView = view,
	                             .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	VkDescriptorBufferInfo dbi = {.buffer = obuf, .range = VK_WHOLE_SIZE};
	VkWriteDescriptorSet wr[2] = {
	    {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
	     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii},
	    {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1,
	     .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi}};
	vkUpdateDescriptorSets(dev, 2, wr, 0, NULL);

	VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf};
	VkCommandPool cpool;
	CHECK(vkCreateCommandPool(dev, &cpci, NULL, &cpool), "vkCreateCommandPool");
	VkCommandBufferAllocateInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool,
	                                   .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
	VkCommandBuffer cb;
	CHECK(vkAllocateCommandBuffers(dev, &cbi, &cb), "vkAllocateCommandBuffers");
	VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	vkBeginCommandBuffer(cb, &bi);
	// the buffer comes from outside Vulkan with its content: taken over from the "foreign" queue family
	VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	                          .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	                          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	                          .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	                          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
	                          .dstQueueFamilyIndex = qf,
	                          .image = img,
	                          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL,
	                     1, &b);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, NULL);
	uint32_t pc[2] = {w, h};
	vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pc);
	vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
	vkEndCommandBuffer(cb);
	VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
	CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit");
	CHECK(vkQueueWaitIdle(queue), "vkQueueWaitIdle");

	// --- what the conversion should give
	int bad = 0;
	for (int q = 0; q < 4; q++) {
		double Y = col[q][0], Cb = col[q][1] - 128.0, Cr = col[q][2] - 128.0;
		int narrow = fp.suggestedYcbcrRange == VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
		if (narrow) {
			Y = (Y - 16) * 255 / 219;
			Cb *= 255.0 / 224;
			Cr *= 255.0 / 224;
		}
		double kr = 0.299, kb = 0.114; // BT.601
		if (fp.suggestedYcbcrModel == VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709)
			kr = 0.2126, kb = 0.0722;
		double kg = 1 - kr - kb;
		int e[3] = {clamp8(Y + 2 * (1 - kr) * Cr), clamp8(Y - 2 * kb * (1 - kb) / kg * Cb - 2 * kr * (1 - kr) / kg * Cr),
		            clamp8(Y + 2 * (1 - kb) * Cb)};
		uint32_t x = (q & 1) * w / 2 + w / 4, y = (q >> 1) * h / 2 + h / 4;
		uint32_t v = out[(size_t)y * w + x];
		int g[3] = {v & 255, (v >> 8) & 255, (v >> 16) & 255};
		int ok = abs(g[0] - e[0]) <= 4 && abs(g[1] - e[1]) <= 4 && abs(g[2] - e[2]) <= 4;
		bad += !ok;
		fprintf(stderr, "vkahbtest: quadrant %d (Y %3d Cb %3d Cr %3d): GPU %3d %3d %3d, expected %3d %3d %3d: %s\n", q,
		        col[q][0], col[q][1], col[q][2], g[0], g[1], g[2], e[0], e[1], e[2], ok ? "ok" : "DIFFERENT");
	}
	// the whole picture: every pixel of a quadrant like its centre (away from the borders between quadrants)
	size_t off = 0;
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++) {
			if ((x + 2 >= w / 2 && x < w / 2 + 2) || (y + 2 >= h / 2 && y < h / 2 + 2))
				continue;
			int q = (y >= h / 2) * 2 + (x >= w / 2);
			off += out[(size_t)y * w + x] != out[(size_t)((q >> 1) * h / 2 + h / 4) * w + (q & 1) * w / 2 + w / 4];
		}
	fprintf(stderr, "vkahbtest: %zu pixels differ from their quadrant's centre\n", off);
	return bad || off ? 2 : 0;
}
