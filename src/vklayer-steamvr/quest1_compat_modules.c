// quest1_compat_modules: shader modules (their code kept for retries, the identity distortion), layouts and render passes.
// Part of quest1_compat.c (included by it, in this order: one translation unit).

// The code of every live module (as given to the driver), for the pipeline retry of CreateGraphicsPipelines.
#define MOD_CODES 2048
static struct { VkShaderModule module; uint32_t *code; uint32_t words; } mod_codes[MOD_CODES];
static unsigned mod_code_next;

static void mod_code_put(VkShaderModule m, const uint32_t *code, uint32_t words)
{
	uint32_t *c = malloc(words * sizeof(uint32_t));
	if (!c)
		return;
	memcpy(c, code, words * sizeof(uint32_t));
	pthread_mutex_lock(&g_lock);
	unsigned slot = MOD_CODES;
	for (unsigned i = 0; i < MOD_CODES && slot == MOD_CODES; i++)
		if (!mod_codes[i].module)
			slot = i;
	if (slot == MOD_CODES) // full: replace the oldest entries round robin
		slot = mod_code_next++ % MOD_CODES;
	free(mod_codes[slot].code);
	mod_codes[slot].module = m;
	mod_codes[slot].code = c;
	mod_codes[slot].words = words;
	pthread_mutex_unlock(&g_lock);
}

//! A malloc'ed copy of a module's code, or NULL.
static uint32_t *mod_code_get(VkShaderModule m, uint32_t *words)
{
	uint32_t *c = NULL;
	pthread_mutex_lock(&g_lock);
	for (unsigned i = 0; i < MOD_CODES; i++)
		if (mod_codes[i].module == m && m && (c = malloc(mod_codes[i].words * sizeof(uint32_t)))) {
			memcpy(c, mod_codes[i].code, mod_codes[i].words * sizeof(uint32_t));
			*words = mod_codes[i].words;
			break;
		}
	pthread_mutex_unlock(&g_lock);
	return c;
}

static VKAPI_ATTR void VKAPI_CALL DestroyShaderModule(VkDevice device, VkShaderModule m, const VkAllocationCallbacks *alloc)
{
	pthread_mutex_lock(&g_lock);
	for (unsigned i = 0; i < MOD_CODES; i++)
		if (mod_codes[i].module == m && m) {
			free(mod_codes[i].code);
			mod_codes[i].module = VK_NULL_HANDLE;
			mod_codes[i].code = NULL;
		}
	pthread_mutex_unlock(&g_lock);
	device_of(device)->DestroyShaderModule(device, m, alloc);
}

// vrcompositor's distortion pass with overlays (distort_ps_layered, SteamVR 2.x: 7724 bytes, its FNV-1a below) reads
// the app's eye and the overlays' layer once per colour channel: six texture reads a pixel over both eyes' buffers
// (1500x1664 here; 2.7 to 5 ms an eye measured when that pass did the lens, 2026-11-02). With Monado doing the lens
// (/etc/quest1-warp-lens "1" or "2") driver_quest1's distortion is an identity and the three reads of each are at
// one place, so distort_layered_identity.frag reads each once and gives the same picture. The pass runs while
// Steam Link VR streams (quest1_scanout.c). Known versions: SteamVR 2.17.10 (the Frame's 0.3.0) and 2.19.3 (0.5.9,
// another uniform layout: the .frag's STEAMVR_2_19); any other version of SteamVR's shader is left as it is.
#include "quest1_distort_spv.h"
static const struct {
	size_t size;
	uint64_t fnv;
	const uint32_t *identity;
	size_t identity_size;
	const char *version;
} distort_layered[] = {
	{7724, 0x2151f5242dccf0a6ull, distort_layered_identity_frag, sizeof(distort_layered_identity_frag), "2.17"},
	{7896, 0xc854a56d1d36b547ull, distort_layered_identity_2_19_frag, sizeof(distort_layered_identity_2_19_frag),
	 "2.19"},
};

static bool distort_is_identity(void)
{
	static int on = -1;
	if (on < 0) {
		FILE *f = fopen("/etc/quest1-warp-lens", "r");
		int c = f ? fgetc(f) : 0;
		if (f)
			fclose(f);
		on = c == '1' || c == '2';
	}
	return on;
}

// The entry of distort_layered that ci is, or -1
static int distort_layered_version(const VkShaderModuleCreateInfo *ci)
{
	for (size_t v = 0; v < sizeof(distort_layered) / sizeof(distort_layered[0]); v++) {
		if (ci->codeSize != distort_layered[v].size)
			continue;
		uint64_t h = 0xcbf29ce484222325ull;
		const uint8_t *p = (const uint8_t *)ci->pCode;
		for (size_t i = 0; i < ci->codeSize; i++)
			h = (h ^ p[i]) * 0x100000001b3ull;
		if (h == distort_layered[v].fnv)
			return (int)v;
	}
	return -1;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *ci,
                                                         const VkAllocationCallbacks *alloc, VkShaderModule *out)
{
	VkShaderModuleCreateInfo rewritten, identity;
	uint32_t *code = NULL, words = 0;
	int v = distort_layered_version(ci);
	if (v >= 0 && distort_is_identity()) {
		identity = *ci;
		identity.pCode = distort_layered[v].identity;
		identity.codeSize = distort_layered[v].identity_size;
		ci = &identity;
		LOG("shader module: distort_ps_layered (SteamVR %s) with one read per texture (the distortion is an identity)\n",
		    distort_layered[v].version);
	}
	if (ci->codeSize >= 20 && ci->pCode[0] == 0x07230203 && !getenv("QUEST1_COMPAT_NO_SPIRV_REWRITE") &&
	    (code = rewrite_layer_builtins(ci->pCode, ci->codeSize / 4, &words))) {
		rewritten = *ci;
		rewritten.pCode = code;
		rewritten.codeSize = words * 4;
		ci = &rewritten;
		LOG("shader module: gl_Layer/gl_ViewportIndex turned into private variables\n");
	}
	const char *dbg = spirv_debug() ? getenv("QUEST1_COMPAT_SPIRV") : NULL;
	if (dbg && ci->codeSize >= 20 && ci->pCode[0] == 0x07230203) {
		unsigned n = atomic_fetch_add(&counter, 1);
		char caps[512] = "";
		size_t len = 0;
		const uint32_t *w = ci->pCode, words = ci->codeSize / 4;
		for (uint32_t i = 5; i < words;) {
			uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
			if (wc == 0)
				break;
			if (op == 17 && wc >= 2 && len < sizeof(caps) - 12) // OpCapability
				len += snprintf(caps + len, sizeof(caps) - len, " %u", w[i + 1]);
			if (op == 54) // OpFunction: the preamble is over
				break;
			i += wc;
		}
		LOG("shader module %u: %zu bytes, capabilities%s\n", n, ci->codeSize, caps);
		if (atoi(dbg) >= 2) {
			char path[64];
			mkdir("/tmp/quest1-spirv", 0777);
			snprintf(path, sizeof(path), "/tmp/quest1-spirv/%u.spv", n);
			FILE *f = fopen(path, "wb");
			if (f) {
				fwrite(ci->pCode, 1, ci->codeSize, f);
				fclose(f);
			}
		}
	}
	VkResult r = device_of(device)->CreateShaderModule(device, ci, alloc, out);
	if (r == VK_SUCCESS && ci->codeSize >= 20 && ci->pCode[0] == 0x07230203)
		mod_code_put(*out, ci->pCode, (uint32_t)(ci->codeSize / 4));
	free(code);
	if (dbg && r == VK_SUCCESS) {
		pthread_mutex_lock(&g_lock);
		spv_modules[spv_count++ % 1024] = (struct spv_entry){*out, atomic_load(&counter) - 1};
		pthread_mutex_unlock(&g_lock);
	}
	return r;
}

static int spv_index(VkShaderModule m)
{
	int r = -1;
	pthread_mutex_lock(&g_lock);
	for (unsigned i = 0; i < 1024; i++)
		if (spv_modules[i].module == m)
			r = (int)spv_modules[i].index;
	pthread_mutex_unlock(&g_lock);
	return r;
}

static void log_stages(const char *what, const VkPipelineShaderStageCreateInfo *st, uint32_t n)
{
	char buf[256] = "";
	size_t len = 0;
	for (uint32_t i = 0; i < n && len < sizeof(buf) - 32; i++)
		len += snprintf(buf + len, sizeof(buf) - len, " stage 0x%x=module %d", st[i].stage, spv_index(st[i].module));
	LOG("%s:%s\n", what, buf);
}

// Descriptions of the descriptor set layouts, pipeline layouts and render passes created, by handle, so that a refused
// graphics pipeline can be logged with what it was built against (log_failed_pipeline).
#define OBJ_DESCS 1024
static struct { uint64_t handle; char text[480]; } obj_descs[OBJ_DESCS];
static unsigned obj_desc_next;

static void obj_desc_set(uint64_t handle, const char *text)
{
	pthread_mutex_lock(&g_lock);
	unsigned i = obj_desc_next++ % OBJ_DESCS;
	obj_descs[i].handle = handle;
	snprintf(obj_descs[i].text, sizeof(obj_descs[i].text), "%s", text);
	pthread_mutex_unlock(&g_lock);
}

static void obj_desc_copy(uint64_t handle, char *out, size_t n)
{
	pthread_mutex_lock(&g_lock);
	for (unsigned i = 0; i < OBJ_DESCS; i++)
		if (obj_descs[i].handle == handle && handle)
			snprintf(out, n, "%s", obj_descs[i].text);
	pthread_mutex_unlock(&g_lock);
}

static void obj_desc_log(const char *what, uint64_t handle)
{
	pthread_mutex_lock(&g_lock);
	for (unsigned i = 0; i < OBJ_DESCS; i++)
		if (obj_descs[i].handle == handle && handle)
			LOG("  %s %p: %s\n", what, (void *)handle, obj_descs[i].text);
	pthread_mutex_unlock(&g_lock);
}

// Which bindings each descriptor set layout has, for which stages, and the set layouts of each pipeline layout.
#define LAYOUTS 1024
static struct { uint64_t handle; uint32_t n; uint32_t binding[64], stages[64]; } set_layouts[LAYOUTS];
static struct { uint64_t handle; uint32_t n; uint64_t sets[8]; } pipe_layouts[LAYOUTS];
static unsigned set_layout_next, pipe_layout_next;

//! Whether the pipeline layout gives (set, binding) to the stage; true when the layouts are unknown.
static bool layout_has(uint64_t pl, uint32_t set, uint32_t binding, uint32_t stage)
{
	bool known = false, has = false;
	pthread_mutex_lock(&g_lock);
	for (unsigned i = 0; i < LAYOUTS; i++) {
		if (pipe_layouts[i].handle != pl || !pl)
			continue;
		if (set >= pipe_layouts[i].n) {
			known = true;
			break;
		}
		for (unsigned j = 0; j < LAYOUTS; j++)
			if (set_layouts[j].handle == pipe_layouts[i].sets[set] && set_layouts[j].handle) {
				known = true;
				for (uint32_t k = 0; k < set_layouts[j].n; k++)
					if (set_layouts[j].binding[k] == binding)
						has = (set_layouts[j].stages[k] & stage) != 0;
				break;
			}
		break;
	}
	pthread_mutex_unlock(&g_lock);
	return !known || has;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo *ci,
                                                                const VkAllocationCallbacks *alloc,
                                                                VkDescriptorSetLayout *out)
{
	VkResult r = device_of(device)->CreateDescriptorSetLayout(device, ci, alloc, out);
	if (r == VK_SUCCESS) {
		char buf[480];
		size_t len = (size_t)snprintf(buf, sizeof(buf), "flags 0x%x pNext %s:", ci->flags, ci->pNext ? "set" : "none");
		for (uint32_t i = 0; i < ci->bindingCount && len < sizeof(buf) - 40; i++) {
			const VkDescriptorSetLayoutBinding *b = &ci->pBindings[i];
			len += (size_t)snprintf(buf + len, sizeof(buf) - len, " %u:t%d x%u s0x%x%s", b->binding, b->descriptorType,
			                        b->descriptorCount, b->stageFlags, b->pImmutableSamplers ? "i" : "");
		}
		obj_desc_set((uint64_t)*out, buf);
		rm_note_layout(*out, ci);
		if (ci->bindingCount <= 64) {
			pthread_mutex_lock(&g_lock);
			unsigned s = set_layout_next++ % LAYOUTS;
			set_layouts[s].handle = (uint64_t)*out;
			set_layouts[s].n = ci->bindingCount;
			for (uint32_t i = 0; i < ci->bindingCount; i++) {
				set_layouts[s].binding[i] = ci->pBindings[i].binding;
				set_layouts[s].stages[i] = ci->pBindings[i].descriptorCount ? ci->pBindings[i].stageFlags : 0;
			}
			pthread_mutex_unlock(&g_lock);
		}
	}
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo *ci,
                                                           const VkAllocationCallbacks *alloc, VkPipelineLayout *out)
{
	VkResult r = device_of(device)->CreatePipelineLayout(device, ci, alloc, out);
	if (r == VK_SUCCESS) {
		char buf[480];
		size_t len = (size_t)snprintf(buf, sizeof(buf), "%u push ranges, sets:", ci->pushConstantRangeCount);
		for (uint32_t i = 0; i < ci->setLayoutCount && len < sizeof(buf) - 24; i++)
			len += (size_t)snprintf(buf + len, sizeof(buf) - len, " %p", (void *)ci->pSetLayouts[i]);
		obj_desc_set((uint64_t)*out, buf);
		if (ci->setLayoutCount <= 8) {
			pthread_mutex_lock(&g_lock);
			unsigned s = pipe_layout_next++ % LAYOUTS;
			pipe_layouts[s].handle = (uint64_t)*out;
			pipe_layouts[s].n = ci->setLayoutCount;
			for (uint32_t i = 0; i < ci->setLayoutCount; i++)
				pipe_layouts[s].sets[i] = (uint64_t)ci->pSetLayouts[i];
			pthread_mutex_unlock(&g_lock);
		}
	}
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass(VkDevice device, const VkRenderPassCreateInfo *ci,
                                                       const VkAllocationCallbacks *alloc, VkRenderPass *out)
{
	// scene mode: the 4x MSAA system layer made 1-sample (qs_scene_single_sample, quest1_scanout.c)
	VkRenderPassCreateInfo single;
	VkAttachmentDescription atts[8];
	bool msaa = false;
	for (uint32_t i = 0; i < ci->attachmentCount; i++)
		msaa |= qs_scene_single_sample(ci->pAttachments[i].samples);
	if (msaa && ci->attachmentCount <= 8) {
		single = *ci;
		memcpy(atts, ci->pAttachments, ci->attachmentCount * sizeof(atts[0]));
		for (uint32_t i = 0; i < ci->attachmentCount; i++)
			atts[i].samples = VK_SAMPLE_COUNT_1_BIT;
		single.pAttachments = atts;
		ci = &single;
	}
	VkResult r = device_of(device)->CreateRenderPass(device, ci, alloc, out);
	if (r == VK_SUCCESS)
		rm_note_rp(device_of(device), *out, ci);
	if (r == VK_SUCCESS) {
		char buf[480];
		size_t len = 0;
		len += (size_t)snprintf(buf + len, sizeof(buf) - len, "pNext %s",
		                        ci->pNext ? "set" : "none");
		for (const VkBaseInStructure *p = ci->pNext; p && len < sizeof(buf) - 24; p = p->pNext)
			len += (size_t)snprintf(buf + len, sizeof(buf) - len, " (sType %d)", p->sType);
		for (uint32_t i = 0; i < ci->attachmentCount && len < sizeof(buf) - 40; i++)
			len += (size_t)snprintf(buf + len, sizeof(buf) - len, ", att%u fmt %d x%d ld %d st %d sld %d sst %d lay %d>%d", i,
			                        ci->pAttachments[i].format, ci->pAttachments[i].samples, ci->pAttachments[i].loadOp,
			                        ci->pAttachments[i].storeOp, ci->pAttachments[i].stencilLoadOp,
			                        ci->pAttachments[i].stencilStoreOp, ci->pAttachments[i].initialLayout,
			                        ci->pAttachments[i].finalLayout);
		for (uint32_t i = 0; i < ci->subpassCount && len < sizeof(buf) - 60; i++) {
			const VkSubpassDescription *sp = &ci->pSubpasses[i];
			len += (size_t)snprintf(buf + len, sizeof(buf) - len, ", sub%u in %u col %u res %s ds %s", i,
			                        sp->inputAttachmentCount, sp->colorAttachmentCount,
			                        sp->pResolveAttachments ? "y" : "n", sp->pDepthStencilAttachment ? "y" : "n");
		}
		obj_desc_set((uint64_t)*out, buf);
	}
	return r;
}
