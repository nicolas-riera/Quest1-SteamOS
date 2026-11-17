// quest1_compat_pipelines: pipeline creation: a refused pipeline retried, logged and dumped.
// Part of quest1_compat.c (included by it, in this order: one translation unit).

//! What a refused graphics pipeline asked for (logged on every failure: they are rare and vrcompositor then binds the
//! null pipeline and crashes, e.g. its camera roomview on 2026-11-03).
static void log_failed_pipeline(const VkGraphicsPipelineCreateInfo *c)
{
	LOG("failed graphics pipeline: flags 0x%x pNext %s, %u stages, layout %p, render pass %p subpass %u\n", c->flags,
	    c->pNext ? "set" : "none", c->stageCount, (void *)c->layout, (void *)c->renderPass, c->subpass);
	obj_desc_log("pipeline layout", (uint64_t)c->layout);
	pthread_mutex_lock(&g_lock);
	char sets[480] = "";
	for (unsigned i = 0; i < OBJ_DESCS; i++)
		if (obj_descs[i].handle == (uint64_t)c->layout && c->layout)
			snprintf(sets, sizeof(sets), "%s", obj_descs[i].text);
	pthread_mutex_unlock(&g_lock);
	for (char *p = strstr(sets, "0x"); p; p = strstr(p + 2, "0x"))
		obj_desc_log("set layout", strtoull(p, NULL, 16));
	obj_desc_log("render pass", (uint64_t)c->renderPass);
	for (uint32_t s = 0; s < c->stageCount; s++) {
		const VkPipelineShaderStageCreateInfo *st = &c->pStages[s];
		LOG("  stage 0x%x '%s' flags 0x%x pNext %s, specialization %u constants\n", st->stage, st->pName, st->flags,
		    st->pNext ? "set" : "none", st->pSpecializationInfo ? st->pSpecializationInfo->mapEntryCount : 0);
	}
	const VkPipelineVertexInputStateCreateInfo *vi = c->pVertexInputState;
	if (vi) {
		LOG("  vertex input: %u bindings, %u attributes\n", vi->vertexBindingDescriptionCount,
		    vi->vertexAttributeDescriptionCount);
		for (uint32_t i = 0; i < vi->vertexBindingDescriptionCount; i++)
			LOG("    binding %u stride %u rate %d\n", vi->pVertexBindingDescriptions[i].binding,
			    vi->pVertexBindingDescriptions[i].stride, vi->pVertexBindingDescriptions[i].inputRate);
		for (uint32_t i = 0; i < vi->vertexAttributeDescriptionCount; i++)
			LOG("    attribute loc %u binding %u format %d offset %u\n", vi->pVertexAttributeDescriptions[i].location,
			    vi->pVertexAttributeDescriptions[i].binding, vi->pVertexAttributeDescriptions[i].format,
			    vi->pVertexAttributeDescriptions[i].offset);
	}
	if (c->pInputAssemblyState)
		LOG("  topology %d restart %u\n", c->pInputAssemblyState->topology, c->pInputAssemblyState->primitiveRestartEnable);
	if (c->pTessellationState)
		LOG("  tessellation state set\n");
	if (c->pViewportState)
		LOG("  viewports %u scissors %u pNext %s\n", c->pViewportState->viewportCount, c->pViewportState->scissorCount,
		    c->pViewportState->pNext ? "set" : "none");
	const VkPipelineRasterizationStateCreateInfo *rs = c->pRasterizationState;
	if (rs)
		LOG("  raster: clamp %u discard %u polygon %d cull %d front %d bias %u width %.1f pNext %s\n", rs->depthClampEnable,
		    rs->rasterizerDiscardEnable, rs->polygonMode, rs->cullMode, rs->frontFace, rs->depthBiasEnable, rs->lineWidth,
		    rs->pNext ? "set" : "none");
	const VkPipelineMultisampleStateCreateInfo *ms = c->pMultisampleState;
	if (ms)
		LOG("  multisample: %d samples, sample shading %u (%.2f), mask %s, alpha to coverage %u, to one %u\n",
		    ms->rasterizationSamples, ms->sampleShadingEnable, ms->minSampleShading, ms->pSampleMask ? "set" : "none",
		    ms->alphaToCoverageEnable, ms->alphaToOneEnable);
	const VkPipelineDepthStencilStateCreateInfo *dss = c->pDepthStencilState;
	if (dss)
		LOG("  depth test %u write %u op %d bounds %u stencil %u (front fail %d pass %d depthfail %d op %d, back op %d)\n",
		    dss->depthTestEnable, dss->depthWriteEnable, dss->depthCompareOp, dss->depthBoundsTestEnable,
		    dss->stencilTestEnable, dss->front.failOp, dss->front.passOp, dss->front.depthFailOp, dss->front.compareOp,
		    dss->back.compareOp);
	const VkPipelineColorBlendStateCreateInfo *cb = c->pColorBlendState;
	if (cb) {
		LOG("  colour blend: logic op %u, %u attachments\n", cb->logicOpEnable, cb->attachmentCount);
		for (uint32_t i = 0; i < cb->attachmentCount; i++)
			LOG("    blend %u src %d/%d dst %d/%d op %d/%d mask 0x%x\n", cb->pAttachments[i].blendEnable,
			    cb->pAttachments[i].srcColorBlendFactor, cb->pAttachments[i].srcAlphaBlendFactor,
			    cb->pAttachments[i].dstColorBlendFactor, cb->pAttachments[i].dstAlphaBlendFactor,
			    cb->pAttachments[i].colorBlendOp, cb->pAttachments[i].alphaBlendOp, cb->pAttachments[i].colorWriteMask);
	}
	const VkPipelineDynamicStateCreateInfo *dyn = c->pDynamicState;
	if (dyn) {
		char buf[256] = "";
		size_t len = 0;
		for (uint32_t i = 0; i < dyn->dynamicStateCount && len < sizeof(buf) - 16; i++)
			len += snprintf(buf + len, sizeof(buf) - len, " %d", dyn->pDynamicStates[i]);
		LOG("  dynamic states:%s\n", buf);
	}
}

// vrcompositor's camera roomview vertex shader (2026-11-03) samples images and samplers its pipeline layout does not give
// to the vertex stage (bindings 11/27 not in the set layout, sampler 26 fragment-only), in branches its uniforms never
// take here. The Steam Frame's driver accepts that; the Adreno 540 blob refuses the pipeline (-13) and vrcompositor then
// binds a null pipeline and crashes. So, when a pipeline is refused, its modules are rewritten for the layout: every
// image or sampler variable the layout lacks for that stage is removed, and what sampled or read it gives 0
// (OpConstantNull). Same for buffers: its headset view's vertex shader (2026-11-09, asked for by Remote Play's
// spectator) loads a uniform of a buffer (binding 0) its layout has not got, into a variable it never reads; the
// access chains into such a buffer are removed and their loads give 0. Returns a malloc'ed module or NULL (nothing to
// do, or a use this does not handle).
static uint32_t *null_missing_descriptors(const uint32_t *w, uint32_t words, uint64_t layout, uint32_t stage,
                                          uint32_t *out_words, unsigned *nnull)
{
	enum { NAME = 5, ENTRY = 15, DECORATE = 71, VARIABLE = 59, LOAD = 61, SAMPLED_IMAGE = 86, IMAGE = 100, COPY = 83 };
	enum { CONST_NULL = 46, FUNCTION = 54, FUNCTION_CALL = 57, DEC_BINDING = 33, DEC_SET = 34, UNIFORM_CONSTANT = 0 };
	enum { ACCESS_CHAIN = 65, IN_BOUNDS_ACCESS_CHAIN = 66, UNIFORM = 2, STORAGE_BUFFER = 12 };
	// MISSING: an image or sampler variable, DERIVED: loaded or combined from one, NULLED: read through one (now 0),
	// MISSING_BUF: a buffer variable, CHAIN: a pointer into one
	enum { MISSING = 1, DERIVED = 2, NULLED = 3, MISSING_BUF = 4, CHAIN = 5 };
	uint32_t bound = w[3];
	if (bound == 0 || bound > (1u << 20))
		return NULL;
	uint32_t *set = malloc(bound * sizeof(uint32_t)), *bind = malloc(bound * sizeof(uint32_t));
	uint8_t *mark = calloc(bound, 1);
	uint32_t types[16], null_id[16], ntypes = 0, nmissing = 0, *o = NULL;
	bool ok = set && bind && mark;
	if (ok) {
		memset(set, 0xff, bound * sizeof(uint32_t));
		memset(bind, 0xff, bound * sizeof(uint32_t));
	}
	*nnull = 0;
	// pass 1: descriptor variables the layout lacks for this stage; what is loaded or sampled from them
	for (uint32_t i = 5; ok && i < words;) {
		uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
		if (wc == 0 || i + wc > words) {
			ok = false;
			break;
		}
		const uint32_t *in = &w[i];
		i += wc;
		if (op == DECORATE && wc >= 4 && in[1] < bound && (in[2] == DEC_SET || in[2] == DEC_BINDING)) {
			(in[2] == DEC_SET ? set : bind)[in[1]] = in[3];
		} else if (op == VARIABLE && wc >= 4 && in[2] < bound && set[in[2]] != UINT32_MAX &&
		           bind[in[2]] != UINT32_MAX && !layout_has(layout, set[in[2]], bind[in[2]], stage)) {
			if (in[3] == UNIFORM_CONSTANT)
				mark[in[2]] = MISSING;
			else if (in[3] == UNIFORM || in[3] == STORAGE_BUFFER)
				mark[in[2]] = MISSING_BUF;
			else { // push constants, ...: not handled
				ok = false;
				break;
			}
			nmissing++;
		} else if (nmissing && (op == ACCESS_CHAIN || op == IN_BOUNDS_ACCESS_CHAIN) && wc >= 4 && in[2] < bound &&
		           in[3] < bound && (mark[in[3]] == MISSING_BUF || mark[in[3]] == CHAIN)) {
			mark[in[2]] = CHAIN;
		} else if (nmissing && op == LOAD && wc >= 4 && in[2] < bound && in[3] < bound &&
		           (mark[in[3]] == MISSING_BUF || mark[in[3]] == CHAIN)) {
			mark[in[2]] = NULLED;
			uint32_t t = 0;
			while (t < ntypes && types[t] != in[1])
				t++;
			if (t == ntypes) {
				if (ntypes == 16) {
					ok = false;
					break;
				}
				types[ntypes++] = in[1];
			}
			(*nnull)++;
		} else if (nmissing && (op == LOAD || op == IMAGE) && wc >= 4 && in[2] < bound && in[3] < bound && mark[in[3]]) {
			mark[in[2]] = DERIVED;
		} else if (nmissing && op == SAMPLED_IMAGE && wc >= 5 && in[2] < bound && in[3] < bound && in[4] < bound &&
		           (mark[in[3]] || mark[in[4]])) {
			mark[in[2]] = DERIVED;
		} else if (nmissing && ((op >= 87 && op <= 99) || (op >= 103 && op <= 107) || (op >= 305 && op <= 315)) &&
		           wc >= 4 && in[2] < bound && in[3] < bound && mark[in[3]]) { // sample, fetch, gather, read, query
			mark[in[2]] = NULLED;
			uint32_t t = 0;
			while (t < ntypes && types[t] != in[1])
				t++;
			if (t == ntypes) {
				if (ntypes == 16) {
					ok = false;
					break;
				}
				types[ntypes++] = in[1];
			}
			(*nnull)++;
		} else if (nmissing && op == FUNCTION_CALL) {
			for (uint32_t k = 4; k < wc; k++)
				if (in[k] < bound && mark[in[k]] && mark[in[k]] != NULLED)
					ok = false; // an image passed to a function: not handled
		} else if (nmissing && op != NAME && op != DECORATE && op != ENTRY) {
			for (uint32_t k = 1; k < wc; k++) // a buffer pointer used otherwise (store, copy, array length): not handled
				if (in[k] < bound && (mark[in[k]] == MISSING_BUF || mark[in[k]] == CHAIN))
					ok = false;
		}
	}
	if (ok && nmissing) {
		o = malloc((words + 3 * ntypes) * sizeof(uint32_t));
		ok = o != NULL;
	}
	if (ok && nmissing) {
		memcpy(o, w, 5 * sizeof(uint32_t));
		for (uint32_t t = 0; t < ntypes; t++)
			null_id[t] = bound + t;
		o[3] = bound + ntypes;
		uint32_t n = 5;
		bool nulls_out = false;
		for (uint32_t i = 5; i < words;) {
			uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
			const uint32_t *in = &w[i];
			i += wc;
			if ((op == NAME || op == DECORATE) && wc >= 2 && in[1] < bound &&
			    (mark[in[1]] == MISSING || mark[in[1]] == MISSING_BUF))
				continue;
			if ((op == VARIABLE || op == LOAD || op == IMAGE || op == SAMPLED_IMAGE || op == ACCESS_CHAIN ||
			     op == IN_BOUNDS_ACCESS_CHAIN) && wc >= 3 && in[2] < bound &&
			    (mark[in[2]] == MISSING || mark[in[2]] == DERIVED || mark[in[2]] == MISSING_BUF || mark[in[2]] == CHAIN))
				continue;
			if (op == FUNCTION && !nulls_out) {
				for (uint32_t t = 0; t < ntypes; t++) {
					o[n++] = (3u << 16) | CONST_NULL;
					o[n++] = types[t];
					o[n++] = null_id[t];
				}
				nulls_out = true;
			}
			if (op == ENTRY) { // [model, id, name..., interface ids]: drop the removed variables
				uint32_t s = 3;
				while (s < wc && memchr(&in[s], 0, 4) == NULL)
					s++;
				s = s + 1 < wc ? s + 1 : wc;
				uint32_t start = n;
				memcpy(&o[n], in, s * sizeof(uint32_t));
				n += s;
				for (uint32_t k = s; k < wc; k++)
					if (in[k] >= bound || (mark[in[k]] != MISSING && mark[in[k]] != MISSING_BUF))
						o[n++] = in[k];
				o[start] = ((n - start) << 16) | ENTRY;
				continue;
			}
			// the instruction that gave the nulled value (in[2] is its result; an OpStore of it has it there too)
			bool producer = op == LOAD || (op >= 87 && op <= 99) || (op >= 103 && op <= 107) || (op >= 305 && op <= 315);
			if (producer && wc >= 3 && in[2] < bound && mark[in[2]] == NULLED) {
				uint32_t t = 0;
				while (types[t] != in[1])
					t++;
				o[n++] = (4u << 16) | COPY;
				o[n++] = in[1];
				o[n++] = in[2];
				o[n++] = null_id[t];
				continue;
			}
			memcpy(&o[n], in, wc * sizeof(uint32_t));
			n += wc;
		}
		*out_words = n;
	} else {
		free(o);
		o = NULL;
	}
	free(set);
	free(bind);
	free(mark);
	return o;
}

//! A pipeline still refused after retry_pipeline: its modules for spirv-dis, /tmp/quest1-failed-pipeline-<stage>.spv
static void dump_failed_modules(const VkGraphicsPipelineCreateInfo *c)
{
	for (uint32_t s = 0; s < c->stageCount; s++) {
		uint32_t words = 0;
		uint32_t *code = mod_code_get(c->pStages[s].module, &words);
		char path[64];
		snprintf(path, sizeof(path), "/tmp/quest1-failed-pipeline-%x.spv", c->pStages[s].stage);
		FILE *f = code ? fopen(path, "wb") : NULL;
		if (f) {
			fwrite(code, 4, words, f);
			fclose(f);
			LOG("  module of stage 0x%x written to %s\n", c->pStages[s].stage, path);
		}
		free(code);
	}
}

//! Modules (by hash of their code) that a pipeline using them was refused for, and then accepted once rewritten for its
//! layout: the next pipeline built from such a module is created rewritten straight away. A refused compile costs ~30 ms
//! on the Adreno and vrcompositor's camera roomview builds a dozen of them in a row at its first activation (a 0.5 s stall
//! of the view, 2026-11-17: half of it was the refused attempts).
static uint64_t fixed_hashes[64];
static unsigned fixed_count;

static uint64_t code_hash(const uint32_t *w, uint32_t words)
{
	uint64_t h = 1469598103934665603ull;
	for (uint32_t i = 0; i < words; i++)
		h = (h ^ w[i]) * 1099511628211ull;
	return h ? h : 1;
}

static bool hash_known(uint64_t h)
{
	pthread_mutex_lock(&g_lock);
	bool k = false;
	for (unsigned i = 0; i < fixed_count && !k; i++)
		k = fixed_hashes[i] == h;
	pthread_mutex_unlock(&g_lock);
	return k;
}

static void hash_remember(uint64_t h)
{
	if (hash_known(h))
		return;
	pthread_mutex_lock(&g_lock);
	if (fixed_count < 64)
		fixed_hashes[fixed_count++] = h;
	pthread_mutex_unlock(&g_lock);
}

//! Retry of a refused graphics pipeline with its modules rewritten for its layout (null_missing_descriptors).
static VkResult retry_pipeline(VkDevice device, VkPipelineCache cache, const VkGraphicsPipelineCreateInfo *c,
                               const VkAllocationCallbacks *alloc, VkPipeline *out, bool quiet)
{
	struct device *d = device_of(device);
	VkPipelineShaderStageCreateInfo st[8];
	VkShaderModule tmp[8] = {0};
	unsigned changed = 0, nulled = 0;
	uint64_t hashes[8] = {0};
	if (c->stageCount > 8)
		return VK_ERROR_INITIALIZATION_FAILED;
	for (uint32_t s = 0; s < c->stageCount; s++) {
		st[s] = c->pStages[s];
		uint32_t words = 0, out_words = 0;
		unsigned nn = 0;
		uint32_t *code = mod_code_get(st[s].module, &words);
		if (code)
			hashes[s] = code_hash(code, words);
		if (quiet && code && !hash_known(hashes[s])) {
			free(code);
			continue;
		}
		uint32_t *fixed =
		    code ? null_missing_descriptors(code, words, (uint64_t)c->layout, st[s].stage, &out_words, &nn) : NULL;
		free(code);
		if (!fixed)
			continue;
		VkShaderModuleCreateInfo smci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		                                 .codeSize = out_words * 4, .pCode = fixed};
		VkResult r = d->CreateShaderModule(device, &smci, alloc, &tmp[s]);
		free(fixed);
		if (r == VK_SUCCESS) {
			st[s].module = tmp[s];
			changed++;
			nulled += nn;
		}
	}
	VkResult r = VK_ERROR_INITIALIZATION_FAILED;
	if (changed) {
		VkGraphicsPipelineCreateInfo c2 = *c;
		c2.pStages = st;
		r = d->CreateGraphicsPipelines(device, cache, 1, &c2, alloc, out);
		if (r == VK_SUCCESS)
			for (uint32_t s = 0; s < c->stageCount; s++)
				if (tmp[s] && hashes[s])
					hash_remember(hashes[s]);
		if (!quiet)
		LOG("graphics pipeline retried with %u descriptor reads the layout lacks turned into 0 (%u stages): %d, "
		    "pipeline %p\n", nulled, changed, r, r == VK_SUCCESS ? (void *)*out : NULL);
	}
	for (uint32_t s = 0; s < c->stageCount; s++)
		if (tmp[s])
			d->DestroyShaderModule(device, tmp[s], alloc);
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t n,
                                                              const VkGraphicsPipelineCreateInfo *ci,
                                                              const VkAllocationCallbacks *alloc, VkPipeline *out)
{
	if (spirv_debug())
		for (uint32_t i = 0; i < n; i++)
			log_stages("graphics pipeline", ci[i].pStages, ci[i].stageCount);
	// scene mode: the 4x MSAA system layer made 1-sample (qs_scene_single_sample, quest1_scanout.c)
	VkGraphicsPipelineCreateInfo single[16];
	VkPipelineMultisampleStateCreateInfo ms[16];
	bool msaa = false;
	for (uint32_t i = 0; i < n; i++)
		msaa |= ci[i].pMultisampleState && qs_scene_single_sample(ci[i].pMultisampleState->rasterizationSamples);
	if (msaa && n <= 16) {
		for (uint32_t i = 0; i < n; i++) {
			single[i] = ci[i];
			if (ci[i].pMultisampleState && ci[i].pMultisampleState->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT) {
				ms[i] = *ci[i].pMultisampleState;
				ms[i].rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
				ms[i].sampleShadingEnable = VK_FALSE;
				single[i].pMultisampleState = &ms[i];
			}
		}
		ci = single;
	}
	// a module that was refused before, and made to work by the rewrite: build it rewritten at once
	if (n == 1 && fixed_count && ci[0].stageCount <= 8 && retry_pipeline(device, cache, &ci[0], alloc, &out[0], true) == VK_SUCCESS)
		return VK_SUCCESS;
	VkResult r = device_of(device)->CreateGraphicsPipelines(device, cache, n, ci, alloc, out);
	if (spirv_debug())
		LOG("  -> %d, pipeline %p\n", r, n ? (void *)out[0] : NULL);
	if (r != VK_SUCCESS) {
		bool all = true;
		for (uint32_t i = 0; i < n; i++)
			if (!out[i]) {
				LOG("graphics pipeline %u of %u refused: %d\n", i, n, r);
				log_failed_pipeline(&ci[i]);
				if (retry_pipeline(device, cache, &ci[i], alloc, &out[i], false) != VK_SUCCESS) {
					dump_failed_modules(&ci[i]);
					all = false;
				}
			}
		if (all)
			r = VK_SUCCESS;
	}
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t n,
                                                             const VkComputePipelineCreateInfo *ci,
                                                             const VkAllocationCallbacks *alloc, VkPipeline *out)
{
	if (spirv_debug())
		for (uint32_t i = 0; i < n; i++)
			log_stages("compute pipeline", &ci[i].stage, 1);
	VkResult r = device_of(device)->CreateComputePipelines(device, cache, n, ci, alloc, out);
	if (spirv_debug())
		LOG("  -> %d\n", r);
	return r;
}
