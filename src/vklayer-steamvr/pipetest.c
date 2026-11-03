// pipetest <vs.spv> <fs.spv> [noperspective-free variants are made offline]: build one graphics pipeline from two SPIR-V
// modules on the headset's Vulkan driver and print the result, to find what the Adreno blob refuses in a shader without
// restarting SteamVR. The layout is vrcompositor's roomview one (set 0: UBOs 0 and 4, sampled images 9-11, samplers
// 25-27); vertex input = one float3 at location 0; one RGBA8 colour attachment; dynamic viewport and scissor.
// Run: LD_LIBRARY_PATH=/usr/local/lib/quest1-vk:/opt/hybris/lib LD_PRELOAD=/opt/hybris/lib/libbionictls.so ./pipetest a b
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkDevice dev;
#define IFN(name) PFN_##name name = (PFN_##name)gipa(inst, #name)
#define DFN(name) PFN_##name name = (PFN_##name)gdpa(dev, #name)

static void *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*len = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	void *p = malloc(*len);
	if (fread(p, 1, *len, f) != *len) {
		free(p);
		p = NULL;
	}
	fclose(f);
	return p;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <vs.spv> <fs.spv>\n", argv[0]);
		return 2;
	}
	void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
	if (!lib) {
		fprintf(stderr, "dlopen: %s\n", dlerror());
		return 1;
	}
	gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
	IFN(vkCreateInstance);
	VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0};
	VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
	VkResult r = vkCreateInstance(&ici, NULL, &inst);
	if (r) {
		fprintf(stderr, "vkCreateInstance: %d\n", r);
		return 1;
	}
	IFN(vkEnumeratePhysicalDevices);
	IFN(vkGetPhysicalDeviceQueueFamilyProperties);
	IFN(vkCreateDevice);
	IFN(vkGetDeviceProcAddr);
	PFN_vkGetDeviceProcAddr gdpa = vkGetDeviceProcAddr;
	uint32_t n = 1;
	VkPhysicalDevice pd;
	vkEnumeratePhysicalDevices(inst, &n, &pd);
	uint32_t nq = 8;
	VkQueueFamilyProperties qf[8];
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
	uint32_t fam = 0;
	while (fam < nq && !(qf[fam].queueFlags & VK_QUEUE_GRAPHICS_BIT))
		fam++;
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = fam,
	                               .queueCount = 1, .pQueuePriorities = &prio};
	VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
	                          .pQueueCreateInfos = &qci};
	r = vkCreateDevice(pd, &dci, NULL, &dev);
	if (r) {
		fprintf(stderr, "vkCreateDevice: %d\n", r);
		return 1;
	}
	DFN(vkCreateShaderModule);
	DFN(vkCreateDescriptorSetLayout);
	DFN(vkCreatePipelineLayout);
	DFN(vkCreateRenderPass);
	DFN(vkCreateGraphicsPipelines);

	VkShaderModule mod[2];
	for (int i = 0; i < 2; i++) {
		size_t len = 0;
		void *code = slurp(argv[1 + i], &len);
		if (!code) {
			fprintf(stderr, "cannot read %s\n", argv[1 + i]);
			return 1;
		}
		VkShaderModuleCreateInfo smci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = len,
		                                 .pCode = code};
		r = vkCreateShaderModule(dev, &smci, NULL, &mod[i]);
		printf("shader module %s: %d\n", argv[1 + i], r);
		if (r)
			return 1;
	}

	const struct { uint32_t binding; VkDescriptorType type; } b[] = {
	    {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER}, {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
	    {9, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},  {10, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
	    {11, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE}, {25, VK_DESCRIPTOR_TYPE_SAMPLER},
	    {26, VK_DESCRIPTOR_TYPE_SAMPLER},       {27, VK_DESCRIPTOR_TYPE_SAMPLER},
	};
	VkDescriptorSetLayoutBinding lb[8];
	uint32_t nb = 8;
	for (int i = 0; i < 8; i++)
		lb[i] = (VkDescriptorSetLayoutBinding){.binding = b[i].binding, .descriptorType = b[i].type, .descriptorCount = 1,
		                                       .stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS};
	if (getenv("PT_VRC_LAYOUT")) { // vrcompositor's actual roomview layout: no 11/27, samplers fragment-only
		const struct { uint32_t binding; VkDescriptorType type; uint32_t stages; } v[] = {
		    {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0x1}, {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0x10},
		    {9, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 0x11}, {10, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 0x11},
		    {25, VK_DESCRIPTOR_TYPE_SAMPLER, 0x10},      {26, VK_DESCRIPTOR_TYPE_SAMPLER, 0x10},
		};
		nb = 6;
		for (uint32_t i = 0; i < nb; i++)
			lb[i] = (VkDescriptorSetLayoutBinding){.binding = v[i].binding, .descriptorType = v[i].type,
			                                       .descriptorCount = 1, .stageFlags = v[i].stages};
	}
	VkDescriptorSetLayoutCreateInfo dslci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
	                                         .bindingCount = nb, .pBindings = lb};
	VkDescriptorSetLayout dsl;
	r = vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl);
	VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
	                                   .pSetLayouts = &dsl};
	VkPipelineLayout pl;
	r |= vkCreatePipelineLayout(dev, &plci, NULL, &pl);
	VkAttachmentDescription att = {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
	                               .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
	                               .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	                               .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
	                               .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	                               .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
	                            .pColorAttachments = &ref};
	VkRenderPassCreateInfo rpci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
	                               .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sub};
	VkRenderPass rp;
	r |= vkCreateRenderPass(dev, &rpci, NULL, &rp);
	if (r) {
		fprintf(stderr, "layout/render pass: %d\n", r);
		return 1;
	}

	VkPipelineShaderStageCreateInfo st[2] = {
	    {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
	     .module = mod[0], .pName = getenv("VS_NAME") ? getenv("VS_NAME") : "VS_MAIN"},
	    {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
	     .module = mod[1], .pName = getenv("FS_NAME") ? getenv("FS_NAME") : "PS_MAIN"},
	};
	VkVertexInputBindingDescription vb = {0, 12, VK_VERTEX_INPUT_RATE_VERTEX};
	VkVertexInputAttributeDescription va = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
	VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	                                           .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
	                                           .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &va};
	VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
	                                             .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
	VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
	                                        .viewportCount = 1, .scissorCount = 1};
	VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
	                                             .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1.0f};
	VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
	                                           .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
	VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
	VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
	                                          .attachmentCount = 1, .pAttachments = &cba};
	VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
	                                       .dynamicStateCount = 2, .pDynamicStates = dyn};
	VkGraphicsPipelineCreateInfo gpci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
	                                     .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
	                                     .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
	                                     .pColorBlendState = &cb, .pDynamicState = &ds, .layout = pl, .renderPass = rp};
	VkPipeline pipe = VK_NULL_HANDLE;
	r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe);
	printf("pipeline: %d (%p)\n", r, (void *)pipe);
	return r ? 3 : 0;
}
