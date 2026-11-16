// streambench: times, with Vulkan timestamps, the passes quest1_modifier.c (qcp_record) records for every Steam Link VR frame,
// and variants of them, on the headset's Adreno blob:
//   copy   NV12 1024x4096 (four 1024x1024 views stacked) -> RGBA8 level 0, through a YCbCr conversion sampler
//   mips   levels 1 and 2 (vkCmdBlitImage, or one fragment pass per level)
//   output 1536x6144 RGBA8, nv12_inset.frag, two draws scissored to each eye's wide-view rows
//   read   (reference only: vrcompositor's read of it) each wide view into a 1500x1664 RGBA8 target
// The NV12 source is a gralloc buffer (AHardwareBuffer, venus's format and usage as the layer) imported with the blob's
// external format, filled by the CPU with a noise gradient; SB_NOAHB=1 (or no AHB) makes an OPTIMAL
// G8_B8R8_2PLANE_420_UNORM image filled by a buffer copy instead.
// Variants (argv; default "A B C"; ITER=n iterations each, default 200, run interleaved):
//   A  the layer before 2026-11-16: full-image blits, 3 levels    B  blits over the two inset views' rows only
//   C  B, level 2 skipped when the inset lod < 1 (the layer since)   N  no mips (lower bound)
//   RA/RB/RC  A/B/C with one fragment pass per level (bands for B/C)
//   D  C with the copy pass over the inset rows only and the output reading the wide views from the NV12 image
//      (streambench_inset_nv12.frag); with 2+ variants the output's patches are compared with the first variant's
//   <v>o  any of them with the output as one render pass per eye band instead of one over the whole image
// Build (headset): glslangValidator -V -o nv12_copy.vert.spv nv12_copy.vert; same for nv12_copy.frag, nv12_inset.frag,
//   streambench_tex.frag, streambench_inset_nv12.frag (-> <name>.spv); gcc -O2 -o streambench streambench.c -ldl
// Run:   LD_LIBRARY_PATH=/usr/local/lib/quest1-vk:/opt/hybris/lib LD_PRELOAD=/opt/hybris/lib/libbionictls.so ./streambench A B C
// Timestamps include the GPU's other users (vrcompositor, Monado): look at the minimum and the median.
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CK(x) do { VkResult r_ = (x); if (r_) { fprintf(stderr, "%s:%d %s = %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)

#define FNS \
	F(vkEnumeratePhysicalDevices) F(vkGetPhysicalDeviceQueueFamilyProperties) F(vkCreateDevice) F(vkGetDeviceProcAddr) \
	F(vkGetPhysicalDeviceProperties) F(vkGetPhysicalDeviceMemoryProperties) F(vkEnumerateDeviceExtensionProperties) \
	F(vkGetPhysicalDeviceFormatProperties)
#define DFNS \
	F(vkGetDeviceQueue) F(vkCreateCommandPool) F(vkAllocateCommandBuffers) F(vkBeginCommandBuffer) F(vkEndCommandBuffer) \
	F(vkQueueSubmit) F(vkQueueWaitIdle) F(vkCreateFence) F(vkWaitForFences) F(vkResetFences) F(vkCreateQueryPool) \
	F(vkCmdResetQueryPool) F(vkCmdWriteTimestamp) F(vkGetQueryPoolResults) F(vkCreateImage) F(vkGetImageMemoryRequirements) \
	F(vkAllocateMemory) F(vkBindImageMemory) F(vkCreateImageView) F(vkCreateFramebuffer) F(vkCreateRenderPass) \
	F(vkCreateShaderModule) F(vkCreateGraphicsPipelines) F(vkCreatePipelineLayout) F(vkCreateDescriptorSetLayout) \
	F(vkCreateDescriptorPool) F(vkAllocateDescriptorSets) F(vkUpdateDescriptorSets) F(vkCreateSampler) \
	F(vkCreateSamplerYcbcrConversion) F(vkCmdBeginRenderPass) F(vkCmdEndRenderPass) F(vkCmdBindPipeline) \
	F(vkCmdBindDescriptorSets) F(vkCmdSetViewport) F(vkCmdSetScissor) F(vkCmdDraw) F(vkCmdBlitImage) \
	F(vkCmdPipelineBarrier) F(vkCmdPushConstants) F(vkCmdCopyBufferToImage) F(vkCmdCopyImageToBuffer) F(vkCreateBuffer) \
	F(vkGetBufferMemoryRequirements) F(vkBindBufferMemory) F(vkMapMemory) F(vkGetAndroidHardwareBufferPropertiesANDROID)
#define F(n) static PFN_##n n;
FNS DFNS
#undef F

static VkDevice dev;
static VkPhysicalDeviceMemoryProperties mp;

static void *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "cannot read %s\n", path);
		exit(1);
	}
	fseek(f, 0, SEEK_END);
	*len = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	void *p = malloc(*len);
	if (fread(p, 1, *len, f) != *len)
		exit(1);
	fclose(f);
	return p;
}

static VkShaderModule shader(const char *path)
{
	size_t len;
	void *code = slurp(path, &len);
	VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, len, code};
	VkShaderModule m;
	CK(vkCreateShaderModule(dev, &ci, NULL, &m));
	return m;
}

static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want)
{
	for (uint32_t t = 0; t < mp.memoryTypeCount; t++)
		if ((bits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & want) == want)
			return t;
	fprintf(stderr, "no memory type for 0x%x flags 0x%x\n", bits, want);
	exit(1);
}

static VkDeviceMemory bind_image(VkImage img)
{
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(dev, img, &req);
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size,
	                           memtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
	VkDeviceMemory m;
	CK(vkAllocateMemory(dev, &ai, NULL, &m));
	CK(vkBindImageMemory(dev, img, m, 0));
	return m;
}

// the layer's images (qcp_image): RGBA8, optimal, sampled + attachment + transfer
static VkImage rgba_image(uint32_t w, uint32_t h, uint32_t levels)
{
	VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ic.imageType = VK_IMAGE_TYPE_2D;
	ic.format = VK_FORMAT_R8G8B8A8_UNORM;
	ic.extent = (VkExtent3D){w, h, 1};
	ic.mipLevels = levels;
	ic.arrayLayers = 1;
	ic.samples = VK_SAMPLE_COUNT_1_BIT;
	ic.tiling = VK_IMAGE_TILING_OPTIMAL;
	ic.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	           VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	VkImage img;
	CK(vkCreateImage(dev, &ic, NULL, &img));
	bind_image(img);
	return img;
}

static VkImageView rgba_view(VkImage img, uint32_t base, uint32_t count)
{
	VkImageViewCreateInfo vc = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D,
	                            VK_FORMAT_R8G8B8A8_UNORM, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, base, count, 0, 1}};
	VkImageView v;
	CK(vkCreateImageView(dev, &vc, NULL, &v));
	return v;
}

static VkFramebuffer framebuffer(VkRenderPass rp, VkImageView v, uint32_t w, uint32_t h)
{
	VkFramebufferCreateInfo fc = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 1, &v, w, h, 1};
	VkFramebuffer fb;
	CK(vkCreateFramebuffer(dev, &fc, NULL, &fb));
	return fb;
}

// --- the AHardwareBuffer side (as quest1_dmabuf.c / quest1_modifier.c)
typedef struct
{
	uint32_t width, height, layers, format;
	uint64_t usage;
	uint32_t stride, rfu0;
	uint64_t rfu1;
} ahb_desc;
#define AHB_FORMAT 0x23 // Y8Cb8Cr8_420: venus's NV12 with the video usage (QNV_AHB_FORMAT)
#define AHB_USAGE (3ull | (1ull << 8) | (1ull << 16)) // CPU read often, GPU sampled, video encode (QNV_AHB_USAGE)

static struct
{
	int (*allocate)(const ahb_desc *, void **);
	void (*describe)(void *, ahb_desc *);
	int (*lock)(void *, uint64_t, int32_t, const void *, void **);
	int (*unlock)(void *, int32_t *);
} ahb;

static int ahb_load(void)
{
	void *hc = dlopen("libhybris-common.so.1", RTLD_NOW | RTLD_NOLOAD);
	if (!hc)
		hc = dlopen("/opt/hybris/lib/libhybris-common.so.1", RTLD_NOW | RTLD_NOLOAD);
	if (!hc)
		return 0;
	void *(*adlopen)(const char *, int) = (void *(*)(const char *, int))dlsym(hc, "android_dlopen");
	void *(*adlsym)(void *, const char *) = (void *(*)(void *, const char *))dlsym(hc, "android_dlsym");
	void *nw = adlopen && adlsym ? adlopen("libnativewindow.so", RTLD_NOW) : NULL;
	if (!nw)
		return 0;
	*(void **)&ahb.allocate = adlsym(nw, "AHardwareBuffer_allocate");
	*(void **)&ahb.describe = adlsym(nw, "AHardwareBuffer_describe");
	*(void **)&ahb.lock = adlsym(nw, "AHardwareBuffer_lock");
	*(void **)&ahb.unlock = adlsym(nw, "AHardwareBuffer_unlock");
	return ahb.allocate && ahb.describe && ahb.lock && ahb.unlock;
}

enum { SRCW = 1024, SRCH = 4096, OW = 1536, OH = 6144, TW = 1500, TH = 1664, LEVELS = 3 };
static int tri(int x, int period, int amp) // triangle wave 0..amp
{
	x %= period;
	return (x < period / 2 ? x : period - x) * 2 * amp / period;
}

// noise gradient; SB_SMOOTH=1: smooth content instead (triangle waves, a few levels of noise), NV12 with stride SRCW
static void fill(uint8_t *p, size_t n)
{
	uint32_t s = 12345;
	if (getenv("SB_SMOOTH") && n >= (size_t)SRCW * SRCH * 3 / 2) {
		for (int y = 0; y < SRCH; y++)
			for (int x = 0; x < SRCW; x++) {
				s = s * 1664525u + 1013904223u;
				p[(size_t)y * SRCW + x] = (uint8_t)(20 + tri(x + 2 * y, 300, 200) + ((s >> 24) & 3));
			}
		for (int y = 0; y < SRCH / 2; y++)
			for (int x = 0; x < SRCW / 2; x++) {
				uint8_t *c = p + (size_t)SRCW * SRCH + (size_t)y * SRCW + 2 * x;
				c[0] = (uint8_t)(80 + tri(3 * x + y, 400, 90)); // Cb
				c[1] = (uint8_t)(80 + tri(x + 2 * y, 500, 90)); // Cr
			}
		return;
	}
	for (size_t i = 0; i < n; i++) {
		s = s * 1664525u + 1013904223u;
		p[i] = (uint8_t)(((i >> 5) & 0xff) + ((s >> 24) & 0x3f)); // gradient with noise
	}
}

struct qcp_inset { float place[4], view[4], lod; };


static VkRenderPass rp;
static VkPipeline pipe_copy, pipe_inset, pipe_inset2, pipe_tex;
static VkPipelineLayout pl_copy, pl_inset, pl_inset2, pl_tex;
static VkDescriptorSet set_all2;
static VkImage rgba, out_img;
static VkFramebuffer fb_lv[LEVELS], fb_out, fb_tgt[2];
static VkDescriptorSet set_copy, set_all, set_lv[2], set_out;
static VkQueryPool qpool;
static VkImage src_img;
static const float wide[2][4] = {{0, 0.25f, 1, 0.5f}, {0, 0.75f, 1, 1}};
static const float place[2][4] = {{0.4f, 0.329f, 0.780f, 0.415f}, {0.2197f, 0.829f, 0.5996f, 0.915f}};
static float lod;

struct var { const char *name; int mip; int skip2; int band_out; int direct; };
// mip: -1 none, 0 blits full, 1 blits inset rows, 2 passes full, 3 passes inset rows
// direct: the copy pass renders the inset rows only, the output reads the wide views from the NV12 image
static const struct var vars[] = {
    {"A", 0, 0, 0}, {"B", 1, 0, 0}, {"C", 1, 1, 0}, {"D", 1, 1, 0, 1}, {"N", -1, 0, 0},
    {"RA", 2, 0, 0}, {"RB", 3, 0, 0}, {"RC", 3, 1, 0},
    {"Ao", 0, 0, 1}, {"Bo", 1, 0, 1}, {"Co", 1, 1, 1}, {"No", -1, 0, 1}, {"RCo", 3, 1, 1},
};

static void begin_rp(VkCommandBuffer cb, VkFramebuffer fb, VkRect2D area)
{
	VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, area, 0, NULL};
	vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
}

static void set_vp(VkCommandBuffer cb, uint32_t w, uint32_t h, VkRect2D sc)
{
	VkViewport v = {0, 0, (float)w, (float)h, 0, 1};
	vkCmdSetViewport(cb, 0, 1, &v);
	vkCmdSetScissor(cb, 0, 1, &sc);
}

static void bar(VkCommandBuffer cb, VkImageMemoryBarrier *b, uint32_t n, VkPipelineStageFlags s, VkPipelineStageFlags d)
{
	vkCmdPipelineBarrier(cb, s, d, 0, 0, NULL, 0, NULL, n, b);
}

// each inset view's rows of the frame, with the layer's margin (qcp_record's `band`): on a multiple of the smallest level's rows
static void band_of(int e, int *y0, int *y1)
{
	const int align = 1 << (LEVELS - 1);
	int a = (int)((wide[e][1] - (wide[e][3] - wide[e][1])) * SRCH) - 2 * align, b = (int)(wide[e][1] * SRCH) + 2 * align;
	*y0 = a < 0 ? 0 : a / align * align;
	*y1 = b > SRCH ? SRCH : (b + align - 1) / align * align;
}

static void record(VkCommandBuffer cb, const struct var *v)
{
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	CK(vkBeginCommandBuffer(cb, &bi));
	vkCmdResetQueryPool(cb, qpool, 0, 5);
	vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qpool, 0);
	// 1. copy
	VkRect2D full = {{0, 0}, {SRCW, SRCH}};
	for (int e = 0; e < (v->direct ? 2 : 1); e++) {
		VkRect2D a = full;
		if (v->direct) {
			int y0, y1;
			band_of(e, &y0, &y1);
			a = (VkRect2D){{0, y0}, {SRCW, (uint32_t)(y1 - y0)}};
		}
		begin_rp(cb, fb_lv[0], a);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_copy);
		set_vp(cb, SRCW, SRCH, a);
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_copy, 0, 1, &set_copy, 0, NULL);
		vkCmdDraw(cb, 3, 1, 0, 0);
		vkCmdEndRenderPass(cb);
	}
	vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, 1);
	// 2. mips
	uint32_t nlev = v->skip2 ? 2 : LEVELS;
	if (v->mip == 0 || v->mip == 1) {
		VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		                          VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
		                          VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, rgba,
		                          {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
		VkImageMemoryBarrier bl[2] = {b, b};
		bl[1].srcAccessMask = 0;
		bl[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		bl[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		bl[1].subresourceRange.baseMipLevel = 1;
		bl[1].subresourceRange.levelCount = LEVELS - 1;
		bar(cb, bl, 2, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		for (uint32_t l = 1; l < nlev; l++) {
			int32_t w = SRCW >> (l - 1), h = SRCH >> (l - 1);
			VkImageBlit r[2];
			uint32_t n = 1;
			r[0] = (VkImageBlit){{VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 0, 1}, {{0, 0, 0}, {w, h, 1}},
			                     {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1}, {{0, 0, 0}, {w / 2, h / 2, 1}}};
			if (v->mip == 1) {
				n = 2;
				for (int e = 0; e < 2; e++) {
					int b0, b1;
					band_of(e, &b0, &b1);
					int a0 = b0 >> (l - 1), a1 = b1 >> (l - 1), d0 = b0 >> l, d1 = b1 >> l;
					r[e] = (VkImageBlit){{VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 0, 1}, {{0, a0, 0}, {w, a1, 1}},
					                     {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1}, {{0, d0, 0}, {w / 2, d1, 1}}};
				}
			}
			vkCmdBlitImage(cb, rgba, VK_IMAGE_LAYOUT_GENERAL, rgba, VK_IMAGE_LAYOUT_GENERAL, n, r, VK_FILTER_LINEAR);
			b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			b.subresourceRange.baseMipLevel = l;
			if (l + 1 < nlev)
				bar(cb, &b, 1, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		}
	} else if (v->mip == 2 || v->mip == 3) {
		for (uint32_t l = 1; l < nlev; l++) {
			uint32_t w = SRCW >> l, h = SRCH >> l;
			vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_tex);
			vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_tex, 0, 1, &set_lv[l - 1], 0, NULL);
			float region[4] = {0, 0, 1, 1};
			vkCmdPushConstants(cb, pl_tex, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(region), region);
			if (v->mip == 2) {
				VkRect2D a = {{0, 0}, {w, h}};
				begin_rp(cb, fb_lv[l], a);
				set_vp(cb, w, h, a);
				vkCmdDraw(cb, 3, 1, 0, 0);
				vkCmdEndRenderPass(cb);
			} else {
				for (int e = 0; e < 2; e++) {
					int y0, y1;
					band_of(e, &y0, &y1);
					y0 >>= l;
					y1 >>= l;
					VkRect2D a = {{0, y0}, {w, (uint32_t)(y1 - y0)}};
					begin_rp(cb, fb_lv[l], a);
					vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_tex);
					vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_tex, 0, 1, &set_lv[l - 1], 0, NULL);
					vkCmdPushConstants(cb, pl_tex, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(region), region);
					set_vp(cb, w, h, a);
					vkCmdDraw(cb, 3, 1, 0, 0);
					vkCmdEndRenderPass(cb);
				}
			}
		}
	}
	VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL,
	                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
	                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
	                          VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, rgba,
	                          {VK_IMAGE_ASPECT_COLOR_BIT, 0, LEVELS, 0, 1}};
	bar(cb, &b, 1, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
	    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
	vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, 2);
	// 3. output
	VkRect2D rows[2];
	struct qcp_inset pc[2];
	for (int e = 0; e < 2; e++) {
		memcpy(pc[e].place, place[e], sizeof(pc[e].place));
		float iv[4] = {wide[e][0], wide[e][1] - (wide[e][3] - wide[e][1]), wide[e][2], wide[e][1]};
		memcpy(pc[e].view, iv, sizeof(pc[e].view));
		pc[e].lod = lod;
		int y0 = (int)(wide[e][1] * OH) - 2, y1 = (int)(wide[e][3] * OH + 0.5f) + 2;
		y0 = y0 < 0 ? 0 : y0;
		y1 = y1 > OH ? OH : y1;
		rows[e] = (VkRect2D){{0, y0}, {OW, (uint32_t)(y1 - y0)}};
	}
	VkRect2D oarea = {{0, 0}, {OW, OH}};
	if (!v->band_out)
		begin_rp(cb, fb_out, oarea);
	for (int e = 0; e < 2; e++) {
		if (v->band_out)
			begin_rp(cb, fb_out, rows[e]);
		VkPipelineLayout pl = v->direct ? pl_inset2 : pl_inset;
		if (!e || v->band_out) {
			vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, v->direct ? pipe_inset2 : pipe_inset);
			vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, v->direct ? &set_all2 : &set_all, 0,
			                        NULL);
		}
		set_vp(cb, OW, OH, rows[e]);
		vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc[e]), &pc[e]);
		vkCmdDraw(cb, 3, 1, 0, 0);
		if (v->band_out)
			vkCmdEndRenderPass(cb);
	}
	if (!v->band_out)
		vkCmdEndRenderPass(cb);
	vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, 3);
	// 4. vrcompositor's read, for reference
	for (int e = 0; e < 2; e++) {
		VkRect2D a = {{0, 0}, {TW, TH}};
		begin_rp(cb, fb_tgt[e], a);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_tex);
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl_tex, 0, 1, &set_out, 0, NULL);
		float region[4] = {wide[e][0], wide[e][1], wide[e][2], wide[e][3]};
		vkCmdPushConstants(cb, pl_tex, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(region), region);
		set_vp(cb, TW, TH, a);
		vkCmdDraw(cb, 3, 1, 0, 0);
		vkCmdEndRenderPass(cb);
	}
	vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, 4);
	CK(vkEndCommandBuffer(cb));
}

static int cmpd(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
	void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
	if (!lib) {
		fprintf(stderr, "dlopen: %s\n", dlerror());
		return 1;
	}
	PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
	PFN_vkCreateInstance createInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
	VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
	VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
	VkInstance inst;
	CK(createInstance(&ici, NULL, &inst));
#define F(n) n = (PFN_##n)gipa(inst, #n);
	FNS
#undef F
	uint32_t n = 1;
	VkPhysicalDevice pd;
	vkEnumeratePhysicalDevices(inst, &n, &pd);
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(pd, &props);
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	uint32_t nq = 8;
	VkQueueFamilyProperties qf[8];
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
	uint32_t fam = 0;
	while (fam < nq && !(qf[fam].queueFlags & VK_QUEUE_GRAPHICS_BIT))
		fam++;
	uint32_t validbits = qf[fam].timestampValidBits;
	uint32_t ne = 0;
	vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
	VkExtensionProperties *ep = calloc(ne, sizeof(*ep));
	vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ep);
	const char *want[] = {"VK_KHR_sampler_ycbcr_conversion", "VK_KHR_maintenance1", "VK_KHR_bind_memory2",
	                      "VK_KHR_get_memory_requirements2", "VK_KHR_external_memory", "VK_KHR_dedicated_allocation",
	                      "VK_EXT_queue_family_foreign", "VK_ANDROID_external_memory_android_hardware_buffer"};
	const char *use[8];
	uint32_t nu = 0;
	int have_ahb_ext = 0;
	for (unsigned i = 0; i < 8; i++)
		for (uint32_t k = 0; k < ne; k++)
			if (!strcmp(want[i], ep[k].extensionName)) {
				use[nu++] = want[i];
				have_ahb_ext |= i == 7;
			}
	printf("%s, timestamp period %.1f ns, %u valid bits, %u of the wanted extensions (AHB: %d)\n", props.deviceName,
	       props.limits.timestampPeriod, validbits, nu, have_ahb_ext);
	VkPhysicalDeviceSamplerYcbcrConversionFeatures yf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
	                                                     NULL, VK_TRUE};
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, fam, 1, &prio};
	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &yf, 0, 1, &qci, 0, NULL, nu, use};
	CK(vkCreateDevice(pd, &dci, NULL, &dev));
	PFN_vkGetDeviceProcAddr gdpa = vkGetDeviceProcAddr;
#define F(n) n = (PFN_##n)gdpa(dev, #n);
	DFNS
#undef F
	VkQueue q;
	vkGetDeviceQueue(dev, fam, 0, &q);
	VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, fam};
	VkCommandPool pool;
	CK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
	VkCommandBufferAllocateInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool,
	                                   VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
	VkCommandBuffer cb, setup;
	CK(vkAllocateCommandBuffers(dev, &cbi, &cb));
	CK(vkAllocateCommandBuffers(dev, &cbi, &setup));
	VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence fence;
	CK(vkCreateFence(dev, &fci, NULL, &fence));
	VkQueryPoolCreateInfo qpi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, 5};
	CK(vkCreateQueryPool(dev, &qpi, NULL, &qpool));

	// --- the NV12 source and its conversion sampler
	VkSamplerYcbcrConversion conv;
	VkImageView src_view;
	int is_ahb = 0;
	if (!getenv("SB_NOAHB") && have_ahb_ext && ahb_load()) {
		ahb_desc d = {SRCW, SRCH, 1, AHB_FORMAT, AHB_USAGE | 0x30, 0, 0, 0}; // + CPU write often, to fill it
		void *b = NULL;
		int e = ahb.allocate(&d, &b);
		if (e)
			printf("AHB allocate: %d, falling back\n", e);
		else {
			ahb.describe(b, &d);
			VkAndroidHardwareBufferFormatPropertiesANDROID fp = {
			    VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
			VkAndroidHardwareBufferPropertiesANDROID bp = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID, &fp};
			CK(vkGetAndroidHardwareBufferPropertiesANDROID(dev, b, &bp));
			printf("AHB %ux%u stride %u usage 0x%llx: external format 0x%llx, features 0x%x, size %llu\n", d.width,
			       d.height, d.stride, (unsigned long long)d.usage, (unsigned long long)fp.externalFormat, fp.formatFeatures,
			       (unsigned long long)bp.allocationSize);
			void *map = NULL;
			e = ahb.lock(b, 0x30, -1, NULL, &map);
			if (e || !map)
				printf("AHB lock for CPU write: %d, the buffer stays as allocated\n", e);
			else {
				fill(map, bp.allocationSize < (uint64_t)d.stride * SRCH * 3 / 2 ? bp.allocationSize
				                                                              : (size_t)d.stride * SRCH * 3 / 2);
				ahb.unlock(b, NULL);
			}
			VkExternalFormatANDROID xf = {VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID, NULL, fp.externalFormat};
			VkSamplerYcbcrConversionCreateInfo cc = {VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO, &xf,
			                                         VK_FORMAT_UNDEFINED, VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,
			                                         VK_SAMPLER_YCBCR_RANGE_ITU_FULL, {0},
			                                         (fp.formatFeatures & VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT)
			                                             ? VK_CHROMA_LOCATION_COSITED_EVEN : VK_CHROMA_LOCATION_MIDPOINT,
			                                         VK_CHROMA_LOCATION_MIDPOINT, VK_FILTER_NEAREST, VK_FALSE};
			if (fp.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT)
				cc.chromaFilter = VK_FILTER_LINEAR;
			CK(vkCreateSamplerYcbcrConversion(dev, &cc, NULL, &conv));
			VkExternalMemoryImageCreateInfo emi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &xf,
			                                       VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
			VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &emi};
			ic.imageType = VK_IMAGE_TYPE_2D;
			ic.format = VK_FORMAT_UNDEFINED;
			ic.extent = (VkExtent3D){SRCW, SRCH, 1};
			ic.mipLevels = ic.arrayLayers = 1;
			ic.samples = VK_SAMPLE_COUNT_1_BIT;
			ic.tiling = VK_IMAGE_TILING_OPTIMAL;
			ic.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
			CK(vkCreateImage(dev, &ic, NULL, &src_img));
			VkImportAndroidHardwareBufferInfoANDROID imp = {VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
			                                                NULL, b};
			VkMemoryDedicatedAllocateInfo da = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &imp, src_img, 0};
			VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &da, bp.allocationSize,
			                           (uint32_t)__builtin_ctz(bp.memoryTypeBits)};
			VkDeviceMemory m;
			CK(vkAllocateMemory(dev, &ai, NULL, &m));
			CK(vkBindImageMemory(dev, src_img, m, 0));
			is_ahb = 1;
		}
	}
	if (!is_ahb) {
		VkFormatProperties fp;
		vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, &fp);
		printf("G8_B8R8_2PLANE_420 optimal features 0x%x\n", fp.optimalTilingFeatures);
		VkSamplerYcbcrConversionCreateInfo cc = {VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO, NULL,
		                                         VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,
		                                         VK_SAMPLER_YCBCR_RANGE_ITU_FULL, {0}, VK_CHROMA_LOCATION_MIDPOINT,
		                                         VK_CHROMA_LOCATION_MIDPOINT, VK_FILTER_NEAREST, VK_FALSE};
		if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT)
			cc.chromaFilter = VK_FILTER_LINEAR;
		CK(vkCreateSamplerYcbcrConversion(dev, &cc, NULL, &conv));
		VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ic.imageType = VK_IMAGE_TYPE_2D;
		ic.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
		ic.extent = (VkExtent3D){SRCW, SRCH, 1};
		ic.mipLevels = ic.arrayLayers = 1;
		ic.samples = VK_SAMPLE_COUNT_1_BIT;
		ic.tiling = VK_IMAGE_TILING_OPTIMAL;
		ic.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		CK(vkCreateImage(dev, &ic, NULL, &src_img));
		bind_image(src_img);
		size_t bytes = (size_t)SRCW * SRCH * 3 / 2;
		VkBufferCreateInfo bc = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
		VkBuffer buf;
		CK(vkCreateBuffer(dev, &bc, NULL, &buf));
		VkMemoryRequirements req;
		vkGetBufferMemoryRequirements(dev, buf, &req);
		VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size,
		                           memtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
		VkDeviceMemory bm;
		CK(vkAllocateMemory(dev, &ai, NULL, &bm));
		CK(vkBindBufferMemory(dev, buf, bm, 0));
		void *map;
		CK(vkMapMemory(dev, bm, 0, VK_WHOLE_SIZE, 0, &map));
		fill(map, bytes);
		VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		CK(vkBeginCommandBuffer(setup, &bi));
		VkImageMemoryBarrier tb = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
		                           VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
		                           VK_QUEUE_FAMILY_IGNORED, src_img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
		bar(setup, &tb, 1, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkBufferImageCopy cr[2] = {
		    {0, 0, 0, {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1}, {0, 0, 0}, {SRCW, SRCH, 1}},
		    {(VkDeviceSize)SRCW * SRCH, 0, 0, {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1}, {0, 0, 0}, {SRCW / 2, SRCH / 2, 1}}};
		vkCmdCopyBufferToImage(setup, buf, src_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, cr);
		tb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		tb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		tb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		tb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		bar(setup, &tb, 1, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
		CK(vkEndCommandBuffer(setup));
		VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &setup};
		CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
		CK(vkQueueWaitIdle(q));
	}
	printf("NV12 source: %s\n", is_ahb ? "gralloc AHardwareBuffer, external format, imported (CPU-filled, no UBWC data)"
	                                   : "OPTIMAL G8_B8R8_2PLANE_420_UNORM image filled by a buffer copy");
	{
		VkSamplerYcbcrConversionInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO, NULL, conv};
		VkImageViewCreateInfo vc = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &ci, 0, src_img, VK_IMAGE_VIEW_TYPE_2D,
		                            is_ahb ? VK_FORMAT_UNDEFINED : VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, {0},
		                            {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
		CK(vkCreateImageView(dev, &vc, NULL, &src_view));
	}

	// --- samplers, layouts, render pass, pipelines (qcs_init)
	VkSamplerYcbcrConversionInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO, NULL, conv};
	VkSamplerCreateInfo sc = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, &sci};
	sc.magFilter = sc.minFilter = VK_FILTER_LINEAR; // (the layer uses the conversion's chroma filter: linear when offered)
	sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VkSampler samp_y, samp2;
	CK(vkCreateSampler(dev, &sc, NULL, &samp_y));
	VkSamplerCreateInfo sc2 = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sc2.magFilter = sc2.minFilter = VK_FILTER_LINEAR;
	sc2.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	sc2.addressModeU = sc2.addressModeV = sc2.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sc2.maxLod = LEVELS;
	CK(vkCreateSampler(dev, &sc2, NULL, &samp2));
	VkDescriptorSetLayoutBinding lb = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &samp_y};
	VkDescriptorSetLayoutCreateInfo dlc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &lb};
	VkDescriptorSetLayout dsl, dsl2;
	CK(vkCreateDescriptorSetLayout(dev, &dlc, NULL, &dsl));
	lb.pImmutableSamplers = &samp2;
	CK(vkCreateDescriptorSetLayout(dev, &dlc, NULL, &dsl2));
	VkPipelineLayoutCreateInfo plc = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl};
	CK(vkCreatePipelineLayout(dev, &plc, NULL, &pl_copy));
	VkPushConstantRange pr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(struct qcp_inset)};
	VkPipelineLayoutCreateInfo plc2 = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl2, 1, &pr};
	CK(vkCreatePipelineLayout(dev, &plc2, NULL, &pl_inset));
	VkPushConstantRange pr3 = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
	VkPipelineLayoutCreateInfo plc3 = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl2, 1, &pr3};
	CK(vkCreatePipelineLayout(dev, &plc3, NULL, &pl_tex));
	// variant D's output pass: the RGBA copy's levels (binding 0) and the NV12 frame (binding 1)
	VkDescriptorSetLayoutBinding lb3[2] = {lb, {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &samp_y}};
	VkDescriptorSetLayoutCreateInfo dlc3 = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, lb3};
	VkDescriptorSetLayout dsl3;
	CK(vkCreateDescriptorSetLayout(dev, &dlc3, NULL, &dsl3));
	VkPipelineLayoutCreateInfo plc4 = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl3, 1, &pr};
	CK(vkCreatePipelineLayout(dev, &plc4, NULL, &pl_inset2));

	VkAttachmentDescription att = {0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	                               VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	                               VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL};
	VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription sub = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ref};
	VkSubpassDependency dep[2] = {
	    {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
	     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, 0},
	    {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT, 0},
	};
	VkRenderPassCreateInfo rc = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &att, 1, &sub, 2, dep};
	CK(vkCreateRenderPass(dev, &rc, NULL, &rp));

	VkShaderModule vs = shader("nv12_copy.vert.spv"), fs_copy = shader("nv12_copy.frag.spv"),
	               fs_inset = shader("nv12_inset.frag.spv"), fs_tex = shader("streambench_tex.frag.spv");
	VkPipelineShaderStageCreateInfo st[2] = {
	    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"},
	    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs_copy, "main"},
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
	VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cbs.attachmentCount = 1;
	cbs.pAttachments = &ba;
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
	gp.pColorBlendState = &cbs;
	gp.pDynamicState = &ds;
	gp.renderPass = rp;
	gp.layout = pl_copy;
	CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &pipe_copy));
	st[1].module = fs_inset;
	gp.layout = pl_inset;
	CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &pipe_inset));
	st[1].module = fs_tex;
	gp.layout = pl_tex;
	CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &pipe_tex));
	st[1].module = shader("streambench_inset_nv12.frag.spv");
	gp.layout = pl_inset2;
	CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &pipe_inset2));

	// --- images
	rgba = rgba_image(SRCW, SRCH, LEVELS);
	out_img = rgba_image(OW, OH, 1);
	VkImage tgt[2] = {rgba_image(TW, TH, 1), rgba_image(TW, TH, 1)};
	VkImageView lv[LEVELS];
	for (uint32_t l = 0; l < LEVELS; l++) {
		lv[l] = rgba_view(rgba, l, 1);
		fb_lv[l] = framebuffer(rp, lv[l], SRCW >> l, SRCH >> l);
	}
	VkImageView all = rgba_view(rgba, 0, LEVELS), outv = rgba_view(out_img, 0, 1);
	fb_out = framebuffer(rp, outv, OW, OH);
	for (int e = 0; e < 2; e++)
		fb_tgt[e] = framebuffer(rp, rgba_view(tgt[e], 0, 1), TW, TH);
	VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8};
	VkDescriptorPoolCreateInfo dpc = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 8, 1, &ps};
	VkDescriptorPool dp;
	CK(vkCreateDescriptorPool(dev, &dpc, NULL, &dp));
	VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &dsl};
	CK(vkAllocateDescriptorSets(dev, &dai, &set_copy));
	dai.pSetLayouts = &dsl2;
	CK(vkAllocateDescriptorSets(dev, &dai, &set_all));
	CK(vkAllocateDescriptorSets(dev, &dai, &set_lv[0]));
	CK(vkAllocateDescriptorSets(dev, &dai, &set_lv[1]));
	CK(vkAllocateDescriptorSets(dev, &dai, &set_out));
	VkDescriptorImageInfo ii[5] = {{VK_NULL_HANDLE, src_view, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, all, VK_IMAGE_LAYOUT_GENERAL},
	                               {VK_NULL_HANDLE, lv[0], VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, lv[1], VK_IMAGE_LAYOUT_GENERAL},
	                               {VK_NULL_HANDLE, outv, VK_IMAGE_LAYOUT_GENERAL}};
	VkDescriptorSet sets[5] = {set_copy, set_all, set_lv[0], set_lv[1], set_out};
	VkWriteDescriptorSet wr[5];
	for (int i = 0; i < 5; i++)
		wr[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, sets[i], 0, 0, 1,
		                               VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii[i]};
	vkUpdateDescriptorSets(dev, 5, wr, 0, NULL);
	dai.pSetLayouts = &dsl3;
	CK(vkAllocateDescriptorSets(dev, &dai, &set_all2));
	VkWriteDescriptorSet wr2[2] = {
	    {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set_all2, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii[1]},
	    {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set_all2, 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii[0]}};
	vkUpdateDescriptorSets(dev, 2, wr2, 0, NULL);

	// the AHB image is used in GENERAL, as the layer's descriptor says (nothing transitions the decoder's frames)
	// the inset's level as the layer computes it
	{
		float iv[4] = {wide[0][0], wide[0][1] - (wide[0][3] - wide[0][1]), wide[0][2], wide[0][1]};
		float sx = (iv[2] - iv[0]) * SRCW / ((place[0][2] - place[0][0]) * OW);
		float sy = (iv[3] - iv[1]) * SRCH / ((place[0][3] - place[0][1]) * OH);
		float m = sx > sy ? sx : sy, e = 0;
		while (m >= 2.0f) {
			m *= 0.5f;
			e += 1.0f;
		}
		lod = m < 1.0f ? 0.0f : e + (m - 1.0f) * (1.4427f - 0.4427f * (m - 1.0f));
		printf("inset lod %.3f (sx %.3f sy %.3f)%s\n", lod, sx, sy, lod < 1.0f ? ": level 2 is never read" : "");
	}

	// --- the variants
	int iters = getenv("ITER") ? atoi(getenv("ITER")) : 200, warm = 20;
	const struct var *sel[16];
	VkCommandBuffer cbs_v[16];
	int nv = 0;
	for (int a = 1; a < argc || (a == 1 && nv == 0); a++) {
		const char *nm = a < argc ? argv[a] : NULL;
		static const char *def[] = {"A", "B", "C"};
		for (int dflt = 0; dflt < (nm ? 1 : 3); dflt++) {
			const char *s = nm ? nm : def[dflt];
			const struct var *f = NULL;
			for (unsigned i = 0; i < sizeof(vars) / sizeof(vars[0]); i++)
				if (!strcmp(vars[i].name, s))
					f = &vars[i];
			if (!f) {
				fprintf(stderr, "unknown variant %s\n", s);
				return 2;
			}
			sel[nv++] = f;
		}
	}
	for (int i = 0; i < nv; i++) {
		if (i)
			CK(vkAllocateCommandBuffers(dev, &cbi, &cbs_v[i]));
		else
			cbs_v[0] = cb;
		record(cbs_v[i], sel[i]);
	}
	double *t[16][5];
	for (int i = 0; i < nv; i++)
		for (int s = 0; s < 5; s++)
			t[i][s] = malloc(sizeof(double) * (size_t)iters);
	const double period = props.limits.timestampPeriod * 1e-6; // ms
	const uint64_t mask = validbits >= 64 ? ~0ull : ((1ull << validbits) - 1);
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int it = -warm; it < iters; it++)
		for (int i = 0; i < nv; i++) {
			VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cbs_v[i]};
			CK(vkResetFences(dev, 1, &fence));
			CK(vkQueueSubmit(q, 1, &si, fence));
			CK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 2000000000ull));
			uint64_t ts[5];
			CK(vkGetQueryPoolResults(dev, qpool, 0, 5, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
			if (it >= 0) {
				for (int s = 0; s < 4; s++)
					t[i][s][it] = (double)((ts[s + 1] - ts[s]) & mask) * period;
				t[i][4][it] = (double)((ts[3] - ts[0]) & mask) * period; // the layer's three passes
			}
		}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	printf("%d variants x %d iterations in %.1f s; ms, median (min); read = reference only\n", nv, iters,
	       (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9);
	printf("%-4s %-14s %-14s %-14s %-14s %-14s\n", "var", "copy", "mips", "output", "layer(1-3)", "read");
	for (int i = 0; i < nv; i++) {
		printf("%-4s", sel[i]->name);
		const int order[5] = {0, 1, 2, 4, 3};
		for (int k = 0; k < 5; k++) {
			double *a = t[i][order[k]];
			qsort(a, (size_t)iters, sizeof(double), cmpd);
			printf(" %6.3f (%5.3f)", a[iters / 2], a[0]);
		}
		printf("\n");
	}
	if (nv > 1) {
		// the picture: 64x64 patches of the output, each variant against the first (max abs difference per channel, 0-255)
		enum { P = 64, NP = 6 };
		const struct { const char *what; int x, y; } patch[NP] = {
		    {"eye0 wide", 100, 1700}, {"eye1 wide", 100, 4800}, {"eye0 inset", 800, 2250},
		    {"eye1 inset", 500, 5300}, {"eye0 inset edge", 640, 2040}, {"eye1 inset edge", 1000, 5110}};
		VkBufferCreateInfo bc = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, (size_t)NP * P * P * 4,
		                         VK_BUFFER_USAGE_TRANSFER_DST_BIT};
		VkBuffer buf;
		CK(vkCreateBuffer(dev, &bc, NULL, &buf));
		VkMemoryRequirements req;
		vkGetBufferMemoryRequirements(dev, buf, &req);
		VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size,
		                           memtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
		VkDeviceMemory bm;
		CK(vkAllocateMemory(dev, &ai, NULL, &bm));
		CK(vkBindBufferMemory(dev, buf, bm, 0));
		uint8_t *map;
		CK(vkMapMemory(dev, bm, 0, VK_WHOLE_SIZE, 0, (void **)&map));
		VkCommandBuffer rb;
		CK(vkAllocateCommandBuffers(dev, &cbi, &rb));
		VkBufferImageCopy cr[NP];
		for (int k = 0; k < NP; k++)
			cr[k] = (VkBufferImageCopy){(VkDeviceSize)k * P * P * 4, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
			                            {patch[k].x, patch[k].y, 0}, {P, P, 1}};
		VkCommandBufferBeginInfo rbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		CK(vkBeginCommandBuffer(rb, &rbi));
		vkCmdCopyImageToBuffer(rb, out_img, VK_IMAGE_LAYOUT_GENERAL, buf, NP, cr);
		CK(vkEndCommandBuffer(rb));
		static uint8_t keep[16][NP * P * P * 4];
		for (int i = 0; i < nv; i++) {
			VkSubmitInfo si[2] = {{VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cbs_v[i]},
			                      {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &rb}};
			CK(vkQueueSubmit(q, 2, si, VK_NULL_HANDLE));
			CK(vkQueueWaitIdle(q));
			memcpy(keep[i], map, sizeof(keep[i]));
		}
		for (int i = 1; i < nv; i++) {
			printf("max abs diff %s vs %s (R G B):", sel[i]->name, sel[0]->name);
			for (int k = 0; k < NP; k++) {
				int mx[3] = {0, 0, 0};
				for (int p = 0; p < P * P; p++)
					for (int c = 0; c < 3; c++) {
						int d = abs((int)keep[i][(size_t)(k * P * P + p) * 4 + c] - (int)keep[0][(size_t)(k * P * P + p) * 4 + c]);
						mx[c] = d > mx[c] ? d : mx[c];
					}
				printf("\n  %-16s %3d %3d %3d", patch[k].what, mx[0], mx[1], mx[2]);
			}
			printf("\n");
		}
	}
	return 0;
}
