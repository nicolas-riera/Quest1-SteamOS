// sdftest: how the Adreno 540 driver samples a CPU-written linear R8 image, the way vrcompositor's room setup does
// with its "Playspace area SDF" texture (512x512 R8_UNORM, LINEAR tiling, usage TRANSFER_SRC|TRANSFER_DST, written
// through a mapping, sampled through an image view). A compute shader samples every texel and the result is
// compared with what the CPU wrote, for several usages, initial layouts, barriers and memory types.
// Build: build_sdftest.sh (WSL). Run on the headset:
//   LD_LIBRARY_PATH=/usr/local/lib/quest1-vk:/opt/hybris/lib LD_PRELOAD=/opt/hybris/lib/libbionictls.so ./sdftest
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdftest_cs.h" // sdftest_cs[]: the compute shader (sdftest.comp)

#define W 512
#define H 512

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static VkPhysicalDeviceMemoryProperties mp;
#define IFN(name) static PFN_##name name
IFN(vkGetDeviceProcAddr);
#define D(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)

static uint8_t pattern(uint32_t x, uint32_t y)
{
	return (uint8_t)((x * 7 + y * 3) & 0xff);
}

static int pick_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid)
{
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want &&
		    !(mp.memoryTypes[i].propertyFlags & avoid))
			return (int)i;
	return -1;
}

struct variant {
	const char *name;
	VkImageUsageFlags usage;
	VkImageLayout initial, old; // image's initial layout, the barrier's old layout
	VkMemoryPropertyFlags want, avoid;
};

static void run(const struct variant *v, VkPipeline pipe, VkPipelineLayout pl, VkDescriptorSetLayout dsl,
                VkSampler smp, VkCommandPool pool)
{
	D(vkCreateImage); D(vkGetImageMemoryRequirements); D(vkAllocateMemory); D(vkBindImageMemory);
	D(vkGetImageSubresourceLayout); D(vkMapMemory); D(vkUnmapMemory); D(vkFlushMappedMemoryRanges);
	D(vkCreateImageView); D(vkCreateBuffer); D(vkGetBufferMemoryRequirements); D(vkBindBufferMemory);
	D(vkCreateDescriptorPool); D(vkAllocateDescriptorSets); D(vkUpdateDescriptorSets);
	D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer); D(vkEndCommandBuffer); D(vkCmdPipelineBarrier);
	D(vkCmdBindPipeline); D(vkCmdBindDescriptorSets); D(vkCmdDispatch); D(vkQueueSubmit); D(vkQueueWaitIdle);
	D(vkDestroyImage); D(vkDestroyImageView); D(vkFreeMemory); D(vkDestroyBuffer); D(vkDestroyDescriptorPool);

	VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = VK_FORMAT_R8_UNORM;
	ici.extent = (VkExtent3D){W, H, 1};
	ici.mipLevels = ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_LINEAR;
	ici.usage = v->usage;
	ici.initialLayout = v->initial;
	VkImage img;
	if (vkCreateImage(dev, &ici, NULL, &img)) {
		printf("%-34s create image failed\n", v->name);
		return;
	}
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(dev, img, &req);
	int t = pick_type(req.memoryTypeBits, v->want, v->avoid);
	if (t < 0) {
		printf("%-34s no memory type (bits 0x%x)\n", v->name, req.memoryTypeBits);
		vkDestroyImage(dev, img, NULL);
		return;
	}
	VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, (uint32_t)t};
	VkDeviceMemory mem;
	vkAllocateMemory(dev, &mai, NULL, &mem);
	vkBindImageMemory(dev, img, mem, 0);
	VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
	VkSubresourceLayout lay;
	vkGetImageSubresourceLayout(dev, img, &sub, &lay);
	uint8_t *p;
	vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&p);
	for (uint32_t y = 0; y < H; y++)
		for (uint32_t x = 0; x < W; x++)
			p[lay.offset + y * lay.rowPitch + x] = pattern(x, y);
	VkMappedMemoryRange r = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, mem, 0, VK_WHOLE_SIZE};
	vkFlushMappedMemoryRanges(dev, 1, &r);
	vkUnmapMemory(dev, mem);

	VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vci.image = img;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = VK_FORMAT_R8_UNORM;
	vci.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	VkImageView view;
	VkResult vr = vkCreateImageView(dev, &vci, NULL, &view);
	if (vr) {
		printf("%-34s create view failed %d\n", v->name, vr);
		return;
	}

	VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = W * H * 4;
	bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	VkBuffer buf;
	vkCreateBuffer(dev, &bci, NULL, &buf);
	VkMemoryRequirements breq;
	vkGetBufferMemoryRequirements(dev, buf, &breq);
	VkMemoryAllocateInfo bai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, breq.size,
	                            (uint32_t)pick_type(breq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
	                                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)};
	VkDeviceMemory bmem;
	vkAllocateMemory(dev, &bai, NULL, &bmem);
	vkBindBufferMemory(dev, buf, bmem, 0);

	VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1},
	                              {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
	VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 3, ps};
	VkDescriptorPool dp;
	vkCreateDescriptorPool(dev, &dpci, NULL, &dp);
	VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &dsl};
	VkDescriptorSet ds;
	vkAllocateDescriptorSets(dev, &dsai, &ds);
	VkDescriptorImageInfo ii = {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	VkDescriptorImageInfo si = {smp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
	VkDescriptorBufferInfo bi = {buf, 0, VK_WHOLE_SIZE};
	VkWriteDescriptorSet w[3] = {
	    {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ii, NULL, NULL},
	    {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, &si, NULL, NULL},
	    {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi, NULL}};
	vkUpdateDescriptorSets(dev, 3, w, 0, NULL);

	VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool,
	                                    VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
	VkCommandBuffer cb;
	vkAllocateCommandBuffers(dev, &cbai, &cb);
	VkCommandBufferBeginInfo cbbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	vkBeginCommandBuffer(cb, &cbbi);
	VkImageMemoryBarrier ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	ib.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
	ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	ib.oldLayout = v->old;
	ib.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	ib.image = img;
	ib.subresourceRange = vci.subresourceRange;
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1,
	                     &ib);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
	vkCmdDispatch(cb, W * H / 64, 1, 1);
	VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0,
	                     NULL);
	vkEndCommandBuffer(cb);
	VkSubmitInfo sinfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	sinfo.commandBufferCount = 1;
	sinfo.pCommandBuffers = &cb;
	vkQueueSubmit(queue, 1, &sinfo, VK_NULL_HANDLE);
	vkQueueWaitIdle(queue);

	uint32_t *o;
	vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **)&o);
	unsigned bad = 0, zero = 0;
	int fx = -1, fy = -1;
	for (uint32_t y = 0; y < H; y++)
		for (uint32_t x = 0; x < W; x++) {
			uint32_t got = o[y * W + x];
			if (got != pattern(x, y)) {
				if (!bad++) {
					fx = (int)x;
					fy = (int)y;
				}
				zero += got == 0;
			}
		}
	printf("%-34s type %d (0x%x) pitch %llu: %u/%u wrong (%u zero)", v->name, t, mp.memoryTypes[t].propertyFlags,
	       (unsigned long long)lay.rowPitch, bad, W * H, zero);
	if (bad)
		printf(", first at %d,%d: got %u want %u", fx, fy, o[fy * W + fx], pattern((uint32_t)fx, (uint32_t)fy));
	printf("\n");
	vkUnmapMemory(dev, bmem);
	vkDestroyDescriptorPool(dev, dp, NULL);
	vkDestroyBuffer(dev, buf, NULL);
	vkFreeMemory(dev, bmem, NULL);
	vkDestroyImageView(dev, view, NULL);
	vkDestroyImage(dev, img, NULL);
	vkFreeMemory(dev, mem, NULL);
}

int main(void)
{
	void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
	if (!lib) {
		fprintf(stderr, "%s\n", dlerror());
		return 1;
	}
	gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	if (((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &inst))
		return 1;
	uint32_t n = 1;
	((PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices"))(inst, &n, &pd);
	((PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties"))(pd, &mp);
	float prio = 1;
	VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	if (((PFN_vkCreateDevice)gipa(inst, "vkCreateDevice"))(pd, &dci, NULL, &dev))
		return 1;
	vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
	D(vkGetDeviceQueue); D(vkCreateShaderModule); D(vkCreateDescriptorSetLayout); D(vkCreatePipelineLayout);
	D(vkCreateComputePipelines); D(vkCreateSampler); D(vkCreateCommandPool);
	vkGetDeviceQueue(dev, 0, 0, &queue);

	VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(sdftest_cs),
	                                 sdftest_cs};
	VkShaderModule sm;
	vkCreateShaderModule(dev, &smci, NULL, &sm);
	VkDescriptorSetLayoutBinding b[3] = {
	    {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
	    {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
	    {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
	VkDescriptorSetLayoutCreateInfo dslci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 3, b};
	VkDescriptorSetLayout dsl;
	vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl);
	VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 0, NULL};
	VkPipelineLayout pl;
	vkCreatePipelineLayout(dev, &plci, NULL, &pl);
	VkComputePipelineCreateInfo cpci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	cpci.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
	                                               VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", NULL};
	cpci.layout = pl;
	VkPipeline pipe;
	if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe)) {
		fprintf(stderr, "compute pipeline failed\n");
		return 1;
	}
	VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VkSampler smp;
	vkCreateSampler(dev, &sci, NULL, &smp);
	VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, 0};
	VkCommandPool pool;
	vkCreateCommandPool(dev, &cpi, NULL, &pool);

	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		printf("memory type %u: flags 0x%x\n", i, mp.memoryTypes[i].propertyFlags);
	const VkMemoryPropertyFlags HV = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, HC = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	const VkImageUsageFlags XFER = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	const struct variant vs[] = {
	    {"xfer, undef->read, any HV", XFER, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_UNDEFINED, HV, 0},
	    {"xfer, preinit->read, any HV", XFER, VK_IMAGE_LAYOUT_PREINITIALIZED, VK_IMAGE_LAYOUT_PREINITIALIZED, HV, 0},
	    {"xfer, undef, general->read", XFER, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, HV, 0},
	    {"xfer, undef->read, coherent", XFER, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_UNDEFINED, HV | HC, 0},
	    {"xfer, undef->read, non-coherent", XFER, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_UNDEFINED, HV, HC},
	    {"sampled, undef->read, any HV", XFER | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
	     VK_IMAGE_LAYOUT_UNDEFINED, HV, 0},
	    {"sampled, preinit->read, any HV", XFER | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_LAYOUT_PREINITIALIZED,
	     VK_IMAGE_LAYOUT_PREINITIALIZED, HV, 0},
	    {"sampled, undef->read, coherent", XFER | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
	     VK_IMAGE_LAYOUT_UNDEFINED, HV | HC, 0},
	};
	for (unsigned i = 0; i < sizeof(vs) / sizeof(vs[0]); i++)
		run(&vs[i], pipe, pl, dsl, smp, pool);
	return 0;
}
