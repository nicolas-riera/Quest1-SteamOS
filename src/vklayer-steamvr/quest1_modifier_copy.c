// quest1_modifier_copy: vrlink's frames in vrcompositor: the RGBA copy of each NV12 frame and its output, set up.
// Part of quest1_modifier.c (included by it, in this order: one translation unit).

// --- vrlink's frames in vrcompositor: an RGBA copy of each NV12 frame
//
// vrcompositor draws a frame of vrlink in its distortion pass (here distort_ps_layered: the Frame's
// distort_ps_achromatic_se2, which would blend the inset in, is not used), through a plain view of the NV12 image and
// a separate sampler. The blob samples NV12 only through a YCbCr conversion, which takes an
// immutable combined sampler, so nothing came out (black, 2026-11-11). Here each of vrcompositor's NV12 images has an
// RGBA8 copy, whose views vrcompositor gets instead of NV12 ones, filled right after vrcompositor's barrier on the NV12
// image, which comes each frame before its distortion pass reads it. The copy holds RGB (BT.709, full range):
// vrcompositor shows what it samples as it is, and raw YCbCr (R = Cr, G = Y, B = Cb) came out magenta.
// vrcompositor reads each eye's wide view only, so the copy goes on to an output at its eye buffers' size where each
// wide view gets its foveated inset blended in (nv12_inset.frag, the insets' places from vrlink: quest1_views.h).
// Without it the centre showed the wide view's, coded with few bits under the inset: blocks as soon as anything moved
// (user, 2026-11-12: "art abstrait"; sharp with it). It costs ~2.5 ms of GPU a frame (58-60 fps against ~69 with the
// dashboard open, panels forced on).
// The copies of a size share one memory, and their outputs are one image (qcp_out_get): each frame copies only the
// frame it then reads, after the reads of the frames before it (the passes' dependency), so one of each is all they
// need. vrlink shares 16 frame buffers: 16 copies of 16 MB each were made at once as the stream started, and the
// headset swapped hard (2026-11-12).

#include "quest1_nv12_spv.h"
#include "quest1_views.h"

enum { QCP_LEVELS = 3 }; //!< the copy and two halvings, for the inset that comes out smaller (see qcp_record)

static struct qnv_copy
{
	VkImage nv12; //!< vrcompositor's NV12 image
	VkImage img;  //!< the RGBA8 copy with its smaller levels, bound to the shared memory of its size
	VkImage out;  //!< what vrcompositor reads: the wide views with their insets, at its eye buffers' size (qcs.out)
	int mem;                   //!< the copy's memory (qcs.mem)
	VkImageView src, dst;      //!< the NV12 through the conversion, the copy as an attachment
	VkImageView all, out_view; //!< the copy with its levels, the output as an attachment
	VkFramebuffer fb, out_fb;
	VkDescriptorSet set, out_set;
	uint32_t w, h, ow, oh;
} qcp[128];
static atomic_int qcp_n;

static struct
{
	pthread_mutex_t lock;
	VkDevice device;
	bool tried, ok;
	VkSamplerYcbcrConversion conv;
	VkSampler sampler;
	VkDescriptorSetLayout dsl;
	VkPipelineLayout pl;
	VkRenderPass rp;
	VkPipeline pipe;
	VkSampler sampler2; //!< the inset pass's: linear, with levels
	VkDescriptorSetLayout dsl2;
	VkPipelineLayout pl2;
	VkPipeline pipe2;
	VkRenderPass rp_out; //!< the output's render pass: qcs.rp, or its sRGB twin (qcp_srgb_out)
	VkDescriptorPool pool;
	struct
	{
		VkDeviceMemory mem;
		VkDeviceSize size;
		uint32_t type;
		int users;
	} mem[4]; //!< the copies' and outputs' memories, one per size
	struct
	{
		uint32_t w, h;
		VkImage img;
		VkImageView view;
		VkFramebuffer fb;
		int mem, users;
	} out[4]; //!< the outputs, one per size, shared by the NV12 images of that size (qcp_out_get)
} qcs = {PTHREAD_MUTEX_INITIALIZER};

//! log2 of x >= 1 within 0.01 (0 below; the layer does without libm)
static float qcp_log2(float x)
{
	float e = 0;
	while (x >= 2.0f) {
		x *= 0.5f;
		e += 1.0f;
	}
	return x < 1.0f ? 0.0f : e + (x - 1.0f) * (1.4427f - 0.4427f * (x - 1.0f));
}

//! Whether vrcompositor takes the stream's frame as linear: with `steamvr/enableSimplifiedShaders` it draws through
//! its simplified (achromatic) distortion shaders, which sample the eye texture as linear light and encode it for
//! the display (2.2 ms of GPU a frame less than the full ones, docs/streaming.md "The distortion shader"); an RGBA8
//! output holding the stream's sRGB values then came out washed out. So the output is then an sRGB image, written
//! as linear light by nv12_inset.frag (LINEAR_OUT) and decoded by the sampler as vrcompositor reads it. Its own
//! format, not a view of another (mutable images lose UBWC compression: ~9 ms a frame, 2026-11-16). vrcompositor
//! reads the setting once at its start, as this is read once.
static bool qcp_srgb_out(void)
{
	static int v = -1;
	if (v >= 0)
		return v;
	v = 0;
	const char *home = getenv("HOME");
	char path[512];
	snprintf(path, sizeof(path), "%s/.local/share/Steam/config/steamvr.vrsettings", home ? home : "");
	FILE *f = fopen(path, "r");
	if (!f)
		return v;
	static char buf[65536];
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	const char *k = strstr(buf, "\"enableSimplifiedShaders\"");
	if (k) {
		k += strlen("\"enableSimplifiedShaders\"");
		while (*k == ' ' || *k == '\t' || *k == ':')
			k++;
		v = strncmp(k, "true", 4) == 0;
	}
	LOG("dma-buf: Steam Link VR frames for vrcompositor's %s shaders: %s output\n", v ? "simplified" : "full",
	    v ? "sRGB" : "RGBA8");
	return v;
}

//! nv12_inset.frag's push constants
struct qcp_inset
{
	float place[4]; //!< the inset over the wide view, in the atlas (u1 <= u0: none)
	float view[4];  //!< the inset's own view
	float lod;
};

//! vrlink's shared views (quest1_views.h) in vrcompositor, mapped once vrlink made them; NULL before.
static const struct q1_views *qcp_views_map(void)
{
	static const struct q1_views *_Atomic v;
	static _Atomic uint64_t tried_ns;
	const struct q1_views *m = atomic_load(&v);
	if (m || strcmp(program_invocation_short_name, "vrcompositor") != 0) // (not test_modifier's frames)
		return m;
	uint64_t t = now_ns();
	if (t - atomic_load(&tried_ns) < 1000000000ull) // vrlink makes them at its first frame
		return NULL;
	atomic_store(&tried_ns, t);
	int fd = open(Q1_VIEWS_PATH, O_RDONLY | O_CLOEXEC);
	struct stat st;
	void *p = fd >= 0 && fstat(fd, &st) == 0 && st.st_size >= (off_t)sizeof(*m)
	              ? mmap(NULL, sizeof(*m), PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
	if (fd >= 0)
		close(fd);
	if (p == MAP_FAILED)
		return NULL;
	const struct q1_views *expected = NULL;
	if (!atomic_compare_exchange_strong(&v, &expected, (const struct q1_views *)p)) {
		munmap(p, sizeof(*m));
		return expected;
	}
	LOG("dma-buf: Steam Link VR's foveated insets from %s\n", Q1_VIEWS_PATH);
	return p;
}

//! When vrlink last said it runs (quest1_views.h), 0 if never.
static uint64_t qcp_vrlink_alive_ns(void)
{
	const struct q1_views *v = qcp_views_map();
	return v ? __atomic_load_n(&v->alive_ns, __ATOMIC_RELAXED) : 0;
}

//! vrlink's views (quest1_views.h) in vrcompositor, or false when it published none.
static bool qcp_views(struct q1_views *out)
{
	const struct q1_views *v = qcp_views_map();
	if (!v)
		return false;
	for (int tries = 0; tries < 8; tries++) {
		uint32_t seq = __atomic_load_n(&v->seq, __ATOMIC_ACQUIRE);
		if (seq & 1)
			continue;
		memcpy(out, (const void *)v, sizeof(*out));
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (__atomic_load_n(&v->seq, __ATOMIC_RELAXED) == seq)
			return out->eyes == 2;
	}
	return false;
}

//! Call with qcs.lock held: the conversion, sampler, pipeline and pool, once per device.
static bool qcs_init(struct device *d, VkDevice device)
{
	if (qcs.tried)
		return qcs.ok && qcs.device == device;
	qcs.tried = true;
	qcs.device = device;
	VkFormatFeatureFlags feat = 0;
	uint64_t xfmt = qnv_external_format(d, device, &feat);
	if (!xfmt)
		return false;
	VkExternalFormatANDROID xf = {VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID, NULL, xfmt};
	VkSamplerYcbcrConversionCreateInfo cc = {VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO, &xf,
	                                         VK_FORMAT_UNDEFINED, VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,
	                                         VK_SAMPLER_YCBCR_RANGE_ITU_FULL, {0},
	                                         (feat & VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT)
	                                             ? VK_CHROMA_LOCATION_COSITED_EVEN : VK_CHROMA_LOCATION_MIDPOINT,
	                                         VK_CHROMA_LOCATION_MIDPOINT, VK_FILTER_NEAREST, VK_FALSE};
	if (feat & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT)
		cc.chromaFilter = VK_FILTER_LINEAR;
	VkResult r = d->CreateSamplerYcbcrConversion(device, &cc, NULL, &qcs.conv);
	VkSamplerYcbcrConversionInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO, NULL, qcs.conv};
	// the copy is the frame's size: each pixel is read where it is (the conversion wants its chroma filter here too)
	VkSamplerCreateInfo sc = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, &ci};
	sc.magFilter = sc.minFilter = cc.chromaFilter;
	sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	if (r == VK_SUCCESS)
		r = ((PFN_vkCreateSampler)d->gdpa(device, "vkCreateSampler"))(device, &sc, NULL, &qcs.sampler);
	VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
	                                  &qcs.sampler};
	VkDescriptorSetLayoutCreateInfo dc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b};
	if (r == VK_SUCCESS)
		r = d->CreateDescriptorSetLayout(device, &dc, NULL, &qcs.dsl);
	VkPipelineLayoutCreateInfo plc = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &qcs.dsl};
	if (r == VK_SUCCESS)
		r = d->CreatePipelineLayout(device, &plc, NULL, &qcs.pl);
	VkAttachmentDescription att = {0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	                               VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	                               VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL};
	VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription sub = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ref};
	VkSubpassDependency dep[2] = {
	    // after the copy's last readers (the frames before), and the decoder's frame (vrcompositor waited for it)
	    {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
	     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, 0},
	    // before vrcompositor reads the copy
	    {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT, 0},
	};
	VkRenderPassCreateInfo rc = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &att, 1, &sub, 2, dep};
	if (r == VK_SUCCESS)
		r = d->CreateRenderPass(device, &rc, NULL, &qcs.rp);
	qcs.rp_out = qcs.rp;
	if (r == VK_SUCCESS && qcp_srgb_out()) {
		att.format = VK_FORMAT_R8G8B8A8_SRGB;
		r = d->CreateRenderPass(device, &rc, NULL, &qcs.rp_out);
	}
	PFN_vkCreateShaderModule csm = (PFN_vkCreateShaderModule)d->gdpa(device, "vkCreateShaderModule");
	PFN_vkDestroyShaderModule dsm = (PFN_vkDestroyShaderModule)d->gdpa(device, "vkDestroyShaderModule");
	VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
	VkShaderModuleCreateInfo mv = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(nv12_copy_vert),
	                               nv12_copy_vert};
	VkShaderModuleCreateInfo mf = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(nv12_copy_frag),
	                               nv12_copy_frag};
	if (r == VK_SUCCESS)
		r = csm(device, &mv, NULL, &vs);
	if (r == VK_SUCCESS)
		r = csm(device, &mf, NULL, &fs);
	VkPipelineShaderStageCreateInfo st[2] = {
	    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"},
	    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"},
	};
	VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
	                                             VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
	VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1};
	VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineColorBlendAttachmentState ba = {0};
	ba.colorWriteMask = 0xf;
	VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = 1;
	cb.pAttachments = &ba;
	VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn};
	VkGraphicsPipelineCreateInfo gp = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	gp.stageCount = 2;
	gp.pStages = st;
	gp.pVertexInputState = &vi;
	gp.pInputAssemblyState = &ia;
	gp.pViewportState = &vp;
	gp.pRasterizationState = &rs;
	gp.pMultisampleState = &ms;
	gp.pColorBlendState = &cb;
	gp.pDynamicState = &ds;
	gp.layout = qcs.pl;
	gp.renderPass = qcs.rp;
	if (r == VK_SUCCESS)
		r = d->CreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, NULL, &qcs.pipe);
	if (fs)
		dsm(device, fs, NULL);
	fs = VK_NULL_HANDLE;
	// the inset pass (nv12_inset.frag): the copy through a linear sampler with its levels, the inset in push constants
	VkSamplerCreateInfo sc2 = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sc2.magFilter = sc2.minFilter = VK_FILTER_LINEAR;
	sc2.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	sc2.addressModeU = sc2.addressModeV = sc2.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sc2.maxLod = QCP_LEVELS;
	if (r == VK_SUCCESS)
		r = ((PFN_vkCreateSampler)d->gdpa(device, "vkCreateSampler"))(device, &sc2, NULL, &qcs.sampler2);
	b.pImmutableSamplers = &qcs.sampler2;
	if (r == VK_SUCCESS)
		r = d->CreateDescriptorSetLayout(device, &dc, NULL, &qcs.dsl2);
	VkPushConstantRange pr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(struct qcp_inset)};
	VkPipelineLayoutCreateInfo plc2 = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &qcs.dsl2, 1, &pr};
	if (r == VK_SUCCESS)
		r = d->CreatePipelineLayout(device, &plc2, NULL, &qcs.pl2);
	VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(nv12_inset_frag),
	                               nv12_inset_frag};
	if (r == VK_SUCCESS)
		r = csm(device, &mi, NULL, &fs);
	st[1].module = fs;
	VkBool32 linear_out = qcp_srgb_out();
	VkSpecializationMapEntry se = {0, 0, sizeof(linear_out)};
	VkSpecializationInfo si = {1, &se, sizeof(linear_out), &linear_out};
	st[1].pSpecializationInfo = &si;
	gp.layout = qcs.pl2;
	gp.renderPass = qcs.rp_out;
	if (r == VK_SUCCESS)
		r = d->CreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, NULL, &qcs.pipe2);
	if (vs)
		dsm(device, vs, NULL);
	if (fs)
		dsm(device, fs, NULL);
	VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256};
	VkDescriptorPoolCreateInfo pc = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL,
	                                 VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, 256, 1, &ps};
	if (r == VK_SUCCESS)
		r = ((PFN_vkCreateDescriptorPool)d->gdpa(device, "vkCreateDescriptorPool"))(device, &pc, NULL, &qcs.pool);
	qcs.ok = r == VK_SUCCESS;
	LOG("dma-buf: NV12 frames copied as RGBA for vrcompositor (chroma %s, filter %s): %d\n",
	    cc.xChromaOffset == VK_CHROMA_LOCATION_COSITED_EVEN ? "co-sited" : "midpoint",
	    cc.chromaFilter == VK_FILTER_LINEAR ? "linear" : "nearest", r);
	return qcs.ok;
}

static int qcp_find(VkImage nv12)
{
	if (!nv12 || !atomic_load(&qcp_n))
		return -1;
	int found = -1;
	pthread_mutex_lock(&qcs.lock);
	for (unsigned i = 0; i < sizeof(qcp) / sizeof(qcp[0]) && found < 0; i++)
		if (qcp[i].nv12 == nv12)
			found = (int)i;
	pthread_mutex_unlock(&qcs.lock);
	return found;
}

//! Call with qcs.lock held: an RGBA8 image bound to the shared memory of its size (one per size: see above).
static VkResult qcp_image(struct device *d, VkDevice device, uint32_t w, uint32_t h, uint32_t levels, VkFormat format,
                          VkImage *img, int *mem)
{
	VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ic.imageType = VK_IMAGE_TYPE_2D;
	ic.format = format;
	ic.extent = (VkExtent3D){w, h, 1};
	ic.mipLevels = levels;
	ic.arrayLayers = 1;
	ic.samples = VK_SAMPLE_COUNT_1_BIT;
	ic.tiling = VK_IMAGE_TILING_OPTIMAL;
	ic.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	           VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	*mem = -1;
	VkResult r = d->CreateImage(device, &ic, NULL, img);
	VkMemoryRequirements req = {0};
	if (r == VK_SUCCESS)
		d->GetImageMemoryRequirements(device, *img, &req);
	uint32_t type = UINT32_MAX;
	for (uint32_t t = 0; t < d->memprops.memoryTypeCount && type == UINT32_MAX; t++)
		if ((req.memoryTypeBits & (1u << t)) &&
		    (d->memprops.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
			type = t;
	if (r == VK_SUCCESS && type == UINT32_MAX)
		r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
	const int nmem = (int)(sizeof(qcs.mem) / sizeof(qcs.mem[0]));
	int m = 0;
	while (r == VK_SUCCESS && m < nmem && !(qcs.mem[m].mem && qcs.mem[m].size == req.size && qcs.mem[m].type == type))
		m++;
	if (r == VK_SUCCESS && m == nmem) {
		m = 0;
		while (m < nmem && qcs.mem[m].mem)
			m++;
		VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type};
		r = m == nmem ? VK_ERROR_OUT_OF_DEVICE_MEMORY : d->AllocateMemory(device, &ai, NULL, &qcs.mem[m].mem);
		if (r == VK_SUCCESS) {
			qcs.mem[m].size = req.size;
			qcs.mem[m].type = type;
		}
	}
	if (r == VK_SUCCESS)
		r = d->BindImageMemory(device, *img, qcs.mem[m].mem, 0);
	if (r == VK_SUCCESS) {
		qcs.mem[m].users++;
		*mem = m;
	}
	return r;
}

//! Call with qcs.lock held: one user less of a shared memory; the memory to free when it was the last.
static VkDeviceMemory qcp_unref(int m)
{
	VkDeviceMemory unused = VK_NULL_HANDLE;
	if (m >= 0 && !--qcs.mem[m].users) {
		unused = qcs.mem[m].mem;
		qcs.mem[m].mem = VK_NULL_HANDLE;
	}
	return unused;
}

//! Call with qcs.lock held: the output of that size, made at its first user. One image for all the NV12 images of a size,
//! not one each over a shared memory: vrcompositor late-latches the app's newest frame (its distortion pass reads the
//! frame slot the app's last submit filled, which can be newer than the frame its commands were recorded for, and the
//! pass copied), and the view it then read was of an output image the pass had not rendered: it read black, though
//! the memory held the copy (the driver keeps per-image state: UBWC). The stream flashed black on ~5 % of the frames
//! with the headset still, more under load (2026-11-12). With one image any view reads the pass's last output.
static VkResult qcp_out_get(struct device *d, VkDevice device, struct qnv_copy *c)
{
	const unsigned n = sizeof(qcs.out) / sizeof(qcs.out[0]);
	for (unsigned s = 0; s < n; s++)
		if (qcs.out[s].users && qcs.out[s].w == c->ow && qcs.out[s].h == c->oh) {
			qcs.out[s].users++;
			c->out = qcs.out[s].img;
			c->out_view = qcs.out[s].view;
			c->out_fb = qcs.out[s].fb;
			return VK_SUCCESS;
		}
	unsigned s = 0;
	while (s < n && qcs.out[s].users)
		s++;
	if (s == n)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	VkImage img = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkFramebuffer fb = VK_NULL_HANDLE;
	int mem = -1;
	const VkFormat format = qcp_srgb_out() ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
	VkResult r = qcp_image(d, device, c->ow, c->oh, 1, format, &img, &mem);
	VkImageViewCreateInfo vc = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D,
	                            format, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	if (r == VK_SUCCESS)
		r = d->CreateImageView(device, &vc, NULL, &view);
	VkFramebufferCreateInfo fc = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, qcs.rp_out, 1, &view, c->ow, c->oh, 1};
	if (r == VK_SUCCESS)
		r = ((PFN_vkCreateFramebuffer)d->gdpa(device, "vkCreateFramebuffer"))(device, &fc, NULL, &fb);
	if (r != VK_SUCCESS) {
		((PFN_vkDestroyFramebuffer)d->gdpa(device, "vkDestroyFramebuffer"))(device, fb, NULL);
		((PFN_vkDestroyImageView)d->gdpa(device, "vkDestroyImageView"))(device, view, NULL);
		((PFN_vkDestroyImage)d->gdpa(device, "vkDestroyImage"))(device, img, NULL);
		VkDeviceMemory m = qcp_unref(mem);
		if (m)
			d->FreeMemory(device, m, NULL);
		return r;
	}
	qcs.out[s].w = c->ow;
	qcs.out[s].h = c->oh;
	qcs.out[s].img = c->out = img;
	qcs.out[s].view = c->out_view = view;
	qcs.out[s].fb = c->out_fb = fb;
	qcs.out[s].mem = mem;
	qcs.out[s].users = 1;
	return VK_SUCCESS;
}

static void qcp_destroy(struct device *d, VkDevice device, const struct qnv_copy *c)
{
	PFN_vkFreeDescriptorSets fs = (PFN_vkFreeDescriptorSets)d->gdpa(device, "vkFreeDescriptorSets");
	PFN_vkDestroyFramebuffer df = (PFN_vkDestroyFramebuffer)d->gdpa(device, "vkDestroyFramebuffer");
	PFN_vkDestroyImageView dv = (PFN_vkDestroyImageView)d->gdpa(device, "vkDestroyImageView");
	PFN_vkDestroyImage di = (PFN_vkDestroyImage)d->gdpa(device, "vkDestroyImage");
	if (c->set)
		fs(device, qcs.pool, 1, &c->set);
	if (c->out_set)
		fs(device, qcs.pool, 1, &c->out_set);
	df(device, c->fb, NULL);
	dv(device, c->dst, NULL);
	dv(device, c->src, NULL);
	dv(device, c->all, NULL);
	di(device, c->img, NULL);
	pthread_mutex_lock(&qcs.lock);
	VkDeviceMemory m1 = qcp_unref(c->mem), m2 = VK_NULL_HANDLE;
	// the output, when this was its last user
	VkFramebuffer ofb = VK_NULL_HANDLE;
	VkImageView oview = VK_NULL_HANDLE;
	VkImage oimg = VK_NULL_HANDLE;
	for (unsigned s = 0; c->out && s < sizeof(qcs.out) / sizeof(qcs.out[0]); s++)
		if (qcs.out[s].users && qcs.out[s].img == c->out && !--qcs.out[s].users) {
			ofb = qcs.out[s].fb;
			oview = qcs.out[s].view;
			oimg = qcs.out[s].img;
			m2 = qcp_unref(qcs.out[s].mem);
			memset(&qcs.out[s], 0, sizeof(qcs.out[s]));
		}
	pthread_mutex_unlock(&qcs.lock);
	df(device, ofb, NULL);
	dv(device, oview, NULL);
	di(device, oimg, NULL);
	if (m1)
		d->FreeMemory(device, m1, NULL);
	if (m2)
		d->FreeMemory(device, m2, NULL);
}

//! The RGBA copy of vrcompositor's NV12 image `n` and its output (made at its first view): its index, -1 if they
//! cannot be made. The output is 1.5 times the frame: vrcompositor's eye buffers are 1500x1664 here, over the field
//! of a 1024x1024 wide view (Steam Link VR's for a Frame), and the inset keeps that much of its detail.
static int qcp_get(struct device *d, VkDevice device, int n)
{
	int k = qcp_find(qnv[n].image);
	if (k >= 0)
		return k;
	pthread_mutex_lock(&qcs.lock);
	unsigned i = 0;
	while (i < sizeof(qcp) / sizeof(qcp[0]) && qcp[i].nv12)
		i++;
	if (i == sizeof(qcp) / sizeof(qcp[0]) || !qcs_init(d, device)) {
		pthread_mutex_unlock(&qcs.lock);
		return -1;
	}
	struct qnv_copy c = {.nv12 = qnv[n].image, .mem = -1, .w = qnv[n].w, .h = qnv[n].h};
	c.ow = c.w * 3 / 2;
	c.oh = c.h * 3 / 2;
	VkResult r = qcp_image(d, device, c.w, c.h, QCP_LEVELS, VK_FORMAT_R8G8B8A8_UNORM, &c.img, &c.mem);
	if (r == VK_SUCCESS)
		r = qcp_out_get(d, device, &c);
	VkSamplerYcbcrConversionInfo cinfo = {VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO, NULL, qcs.conv};
	VkImageViewCreateInfo vc = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &cinfo, 0, c.nv12, VK_IMAGE_VIEW_TYPE_2D,
	                            VK_FORMAT_UNDEFINED, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	if (r == VK_SUCCESS)
		r = d->CreateImageView(device, &vc, NULL, &c.src);
	vc.pNext = NULL;
	vc.image = c.img;
	vc.format = VK_FORMAT_R8G8B8A8_UNORM;
	if (r == VK_SUCCESS)
		r = d->CreateImageView(device, &vc, NULL, &c.dst);
	vc.subresourceRange.levelCount = QCP_LEVELS;
	if (r == VK_SUCCESS)
		r = d->CreateImageView(device, &vc, NULL, &c.all);
	PFN_vkCreateFramebuffer cf = (PFN_vkCreateFramebuffer)d->gdpa(device, "vkCreateFramebuffer");
	VkFramebufferCreateInfo fc = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, qcs.rp, 1, &c.dst, c.w, c.h, 1};
	if (r == VK_SUCCESS)
		r = cf(device, &fc, NULL, &c.fb);
	PFN_vkAllocateDescriptorSets as = (PFN_vkAllocateDescriptorSets)d->gdpa(device, "vkAllocateDescriptorSets");
	VkDescriptorSetAllocateInfo sa = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, qcs.pool, 1, &qcs.dsl};
	if (r == VK_SUCCESS)
		r = as(device, &sa, &c.set);
	sa.pSetLayouts = &qcs.dsl2;
	if (r == VK_SUCCESS)
		r = as(device, &sa, &c.out_set);
	if (r == VK_SUCCESS) {
		VkDescriptorImageInfo ii[2] = {{VK_NULL_HANDLE, c.src, VK_IMAGE_LAYOUT_GENERAL},
		                               {VK_NULL_HANDLE, c.all, VK_IMAGE_LAYOUT_GENERAL}};
		VkWriteDescriptorSet w[2] = {
		    {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, c.set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii[0]},
		    {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, c.out_set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		     &ii[1]},
		};
		((PFN_vkUpdateDescriptorSets)d->gdpa(device, "vkUpdateDescriptorSets"))(device, 2, w, 0, NULL);
		qcp[i] = c;
		atomic_fetch_add(&qcp_n, 1);
	}
	pthread_mutex_unlock(&qcs.lock);
	if (r != VK_SUCCESS)
		qcp_destroy(d, device, &c);
	static atomic_int logs;
	if (atomic_fetch_add(&logs, 1) < 4 || r != VK_SUCCESS)
		LOG("dma-buf: RGBA copy %ux%u of NV12 image %p, output %ux%u: %d\n", c.w, c.h, (void *)c.nv12, c.ow, c.oh, r);
	return r == VK_SUCCESS ? (int)i : -1;
}

//! After vkDestroyImage of an NV12 image: its copy goes too (vrcompositor destroyed its views of it first).
static void qcp_forget(VkDevice device, VkImage nv12)
{
	int k = qcp_find(nv12);
	if (k < 0)
		return;
	pthread_mutex_lock(&qcs.lock);
	struct qnv_copy c = qcp[k];
	memset(&qcp[k], 0, sizeof(qcp[k]));
	atomic_fetch_sub(&qcp_n, 1);
	pthread_mutex_unlock(&qcs.lock);
	qcp_destroy(device_of(device), device, &c);
}

// The PC sends a black picture now and then: one frame (sometimes a few) with nothing but what its compositor draws
// last, the chaperone, the controllers and the laser, on black, cut in and out with no fade. It is in the PC's own
// mirror window (2026-11-12, 33 in 68 s of a capture; ~3% of the snapshots here, with or without the dashboard, the
// headset's tracking steady as the PC saw it), so it is not made here. The output is one image (qcp_out_get): without
// a copy it keeps the last picture, so a frame that is suddenly almost all black after a picture is not copied. A
// black that lasts is the app's own (a fade, a loading screen) and goes through after QCP_HOLD_MAX frames.
enum { QCP_HOLD_MAX = 4 };
static struct
{
	pthread_mutex_t lock;
	VkImage last; //!< the frame decided last (vrcompositor copies a frame again until there is a new one)
	bool last_held;
	int run;           //!< black frames in a row
	uint64_t picture;  //!< when the last frame with a picture came
	unsigned held, logged;
	uint64_t log_ns;
} qhold = {PTHREAD_MUTEX_INITIALIZER};
