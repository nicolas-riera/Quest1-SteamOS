// quest1_compat_spirv: SPIR-V rewrites for the blob's compiler, and the CPU-written images.
// Part of quest1_compat.c (included by it, in this order: one translation unit).

// --- shader modules -------------------------------------------------------------------------------------
//! QUEST1_COMPAT_SPIRV set and not empty or "0" (steamvr-q1 and the frame-mode unit export it empty).
static bool spirv_debug(void)
{
	const char *e = getenv("QUEST1_COMPAT_SPIRV");
	return e && *e && strcmp(e, "0") != 0;
}

// QUEST1_COMPAT_SPIRV=1 logs the capabilities of every module (the Adreno compiler crashes on some),
// =2 also writes them to /tmp/quest1-spirv/<n>.spv

static atomic_uint counter;

struct spv_entry
{
	VkShaderModule module;
	unsigned index;
};
static struct spv_entry spv_modules[1024];
static unsigned spv_count;

// The Adreno compiler chokes on gl_Layer / gl_ViewportIndex outside geometry shaders: it crashes on
// vertex shaders that write them (SPV_EXT_shader_viewport_index_layer, which this layer only
// advertises) and fails fragment shaders that read gl_Layer. vrcompositor's distortion pass writes
// gl_Layer from a uniform, always 0 for its single-layer swapchain, and its fragment shader reads it
// back. So turn those built-ins into Private variables: written ones become no-ops, read ones read 0.
// The capabilities they needed (ShaderViewportIndexLayerEXT, and Geometry when nothing else uses it)
// are dropped. Returns a malloc'ed module (words in *out_words) or NULL when there is nothing to do.
static uint32_t *rewrite_layer_builtins(const uint32_t *w, uint32_t words, uint32_t *out_words)
{
	enum { CAP = 17, EXT = 10, ENTRY = 15, DECORATE = 71, TYPE_POINTER = 32, VARIABLE = 59 };
	enum { STORAGE_INPUT = 1, STORAGE_OUTPUT = 3, STORAGE_PRIVATE = 6 };
	enum { BUILTIN = 11, BI_PRIMITIVE_ID = 7, BI_LAYER = 9, BI_VIEWPORT_INDEX = 10 };
	enum { CAP_GEOMETRY = 2, CAP_VIEWPORT_INDEX_LAYER = 5254 };
	struct { uint32_t id, ptr, storage; } vars[8];
	struct { uint32_t ptr, storage, newptr, zero; } ptrs[8];
	uint32_t nvars = 0, nptrs = 0, decorated[8], ndec = 0;
	bool primitive_id = false;

	// pass 1: the Layer/ViewportIndex built-ins declared as Input or Output variables
	for (uint32_t i = 5; i < words;) {
		uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
		if (wc == 0 || i + wc > words)
			return NULL;
		if (op == DECORATE && wc >= 4 && w[i + 2] == BUILTIN) {
			if ((w[i + 3] == BI_LAYER || w[i + 3] == BI_VIEWPORT_INDEX) && ndec < 8)
				decorated[ndec++] = w[i + 1];
			primitive_id |= w[i + 3] == BI_PRIMITIVE_ID;
		}
		if (op == VARIABLE && (w[i + 3] == STORAGE_INPUT || w[i + 3] == STORAGE_OUTPUT))
			for (uint32_t k = 0; k < ndec; k++)
				if (decorated[k] == w[i + 2] && nvars < 8) {
					vars[nvars].id = w[i + 2];
					vars[nvars].ptr = w[i + 1];
					vars[nvars++].storage = w[i + 3];
					bool known = false;
					for (uint32_t p = 0; p < nptrs; p++)
						known |= ptrs[p].ptr == w[i + 1];
					if (!known && nptrs < 8) {
						ptrs[nptrs].ptr = w[i + 1];
						ptrs[nptrs++].storage = w[i + 3];
					}
				}
		i += wc;
	}
	if (nvars == 0)
		return NULL;

	// per pointer type: a new pointer + a zero constant (8 words); per variable: an initialiser word
	uint32_t *o = malloc((words + 8 * nptrs + nvars) * sizeof(uint32_t));
	memcpy(o, w, 5 * sizeof(uint32_t));
	uint32_t n = 5, bound = w[3];
	for (uint32_t p = 0; p < nptrs; p++) {
		ptrs[p].newptr = bound++;
		ptrs[p].zero = ptrs[p].storage == STORAGE_INPUT ? bound++ : 0;
	}
	o[3] = bound;
	bool keep_interface = w[1] >= 0x10400; // SPIR-V 1.4+ lists Private variables in the interface too
	bool reads = false;
	for (uint32_t v = 0; v < nvars; v++)
		reads |= vars[v].storage == STORAGE_INPUT;
	bool drop_geometry = reads && !primitive_id;

	for (uint32_t i = 5; i < words;) {
		uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
		const uint32_t *ins = &w[i];
		i += wc;
		int var = -1;
		for (uint32_t v = 0; v < nvars; v++)
			if ((op == DECORATE && ins[1] == vars[v].id) || (op == VARIABLE && ins[2] == vars[v].id))
				var = (int)v;

		if (op == CAP && (ins[1] == CAP_VIEWPORT_INDEX_LAYER || (ins[1] == CAP_GEOMETRY && drop_geometry)))
			continue;
		if (op == EXT && strcmp((const char *)&ins[1], "SPV_EXT_shader_viewport_index_layer") == 0)
			continue;
		if (op == DECORATE && var >= 0)
			continue;
		if (op == ENTRY && !keep_interface) {
			// [model, id, name (nul-terminated string words), interface ids...]
			uint32_t s = 3;
			while (s < wc && memchr(&ins[s], 0, 4) == NULL)
				s++;
			s++; // the word holding the terminator
			uint32_t start = n;
			memcpy(&o[n], ins, s * sizeof(uint32_t));
			n += s;
			for (uint32_t k = s; k < wc; k++) {
				bool drop = false;
				for (uint32_t v = 0; v < nvars; v++)
					drop |= ins[k] == vars[v].id;
				if (!drop)
					o[n++] = ins[k];
			}
			o[start] = ((n - start) << 16) | ENTRY;
			continue;
		}
		if (op == VARIABLE && var >= 0) {
			for (uint32_t p = 0; p < nptrs; p++)
				if (ptrs[p].ptr == ins[1]) {
					o[n++] = ((ptrs[p].zero ? 5u : 4u) << 16) | VARIABLE;
					o[n++] = ptrs[p].newptr;
					o[n++] = ins[2];
					o[n++] = STORAGE_PRIVATE;
					if (ptrs[p].zero)
						o[n++] = ptrs[p].zero; // initialiser: what reading gl_Layer gives
				}
			continue;
		}
		memcpy(&o[n], ins, wc * sizeof(uint32_t));
		n += wc;
		if (op == TYPE_POINTER)
			for (uint32_t p = 0; p < nptrs; p++)
				if (ptrs[p].ptr == ins[1]) {
					o[n++] = (4u << 16) | TYPE_POINTER;
					o[n++] = ptrs[p].newptr;
					o[n++] = STORAGE_PRIVATE;
					o[n++] = ins[3];
					if (ptrs[p].zero) { // OpConstant <int type> <id> 0
						o[n++] = (4u << 16) | 43;
						o[n++] = ins[3];
						o[n++] = ptrs[p].zero;
						o[n++] = 0;
					}
				}
	}
	*out_words = n;
	return o;
}

// Camera frames for vrcompositor's roomview (src/camtap/ptprovider): the provider allocates RGBA8 shared images and writes
// their memory with the CPU. With optimal tiling the Adreno blob compresses such images (UBWC), so raw bytes decode to
// blocks of noise. The provider marks them with an otherwise unused usage bit (input attachment); here they lose the
// mark and get linear tiling (no compression), and their row pitch is logged. In vrcompositor only, where those
// images are made: ANGLE (Steam's stream client) asks that usage for every texture of its own.
#define Q1_CPU_IMAGE_USAGE VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT

static bool cpu_image(const VkImageCreateInfo *ci)
{
	return (ci->usage & Q1_CPU_IMAGE_USAGE) && ci->format == VK_FORMAT_R8G8B8A8_UNORM &&
	       ci->tiling == VK_IMAGE_TILING_OPTIMAL && ci->mipLevels == 1 && ci->arrayLayers == 1 &&
	       ci->imageType == VK_IMAGE_TYPE_2D && !strcmp(program_invocation_short_name, "vrcompositor");
}

// Failed image creations and allocations are logged (the first 40): vrcompositor only says "CreateTexture - Unable to
// allocate memory".
static unsigned fail_logs;
