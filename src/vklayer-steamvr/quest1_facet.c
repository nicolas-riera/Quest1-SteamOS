// quest1_facet.c: what SteamVR's newer "Facet" compositor renderer needs and the Adreno 540 blob lacks.
// Included by quest1_compat.c (after quest1_display.c, whose qs_*/qd_* hooks it chains to).
//
// Facet (useFacetRenderer=true) hard-requires timeline semaphores (emulated in quest1_compat.c),
// VK_EXT_host_query_reset and VK_KHR_dynamic_rendering. Off by default: QUEST1_FACET=1 in the
// environment, or the file /etc/quest1-facet containing 1, turns the emulation on.
//
// - VK_EXT_host_query_reset: vkResetQueryPool becomes a one-shot command buffer with
//   vkCmdResetQueryPool, submitted on the first queue and not waited for (the queue is in order).
// - VK_KHR_dynamic_rendering: vkCmdBeginRendering/EndRendering become vkCmdBeginRenderPass/
//   EndRenderPass on render passes and framebuffers built (and cached) from the rendering info.
//   Pipelines created with VkPipelineRenderingCreateInfo get a render pass compatible with them.
//   The begin/create calls go through the next hook in the chain (qs_* when direct scanout is on),
//   so the per-eye redirection of quest1_scanout.c applies to Facet's eye passes too.
// - vkCmdBeginRenderPass2/EndRenderPass2 (what Facet actually uses on the headset, it never calls
//   vkCmdBeginRendering) go to the classic begin/end hooks, otherwise direct scanout is bypassed.
// - Dynamic states of VK_EXT_extended_dynamic_state(3) / sample_locations are dropped from pipelines
//   (the blob does not know them), their vkCmdSet* are logged no-ops.
//
// Credit: the idea of advertising these for Facet and the host-cached memory type comes from the
// Quest 2 SteamOS port (github.com/URFAVF3MBOY/quest2-steamos, commits d474e7c5 and 2a491fe1).

struct fx_node
{
	uint64_t h;
	struct fx_node *next;
	VkFormat fmt;
	VkSampleCountFlagBits samples; // images
	VkExtent3D ext;                // images
	uint32_t mips;                 // images
	uint64_t image;                // views
	VkImageAspectFlags aspect;     // views
	uint32_t base_mip;             // views
};

#define FX_HASH 4096
#define FX_RPS 64
#define FX_FBS 4096
#define FX_SLOTS 16

struct fx_rp
{
	VkRenderPass rp;
	uint8_t key[256];
	uint32_t keylen;
};

struct fx_fb
{
	VkFramebuffer fb;
	VkRenderPass rp;
	VkImageView views[9];
	uint32_t nviews, w, h, layers;
};

struct fx
{
	pthread_mutex_t lock;
	struct fx_node *images[FX_HASH], *views[FX_HASH];
	struct fx_rp rps[FX_RPS];
	uint32_t nrps;
	struct fx_fb *fbs[FX_FBS];
	uint32_t nfbs;
	// one-shot query resets
	VkCommandPool pool;
	VkCommandBuffer cb[FX_SLOTS];
	VkFence fence[FX_SLOTS];
	bool used[FX_SLOTS];
	uint32_t next_slot;
	VkQueue queue;
	bool reset_ok;
	uint64_t n_resets, n_rendering, n_dyn_dropped, n_dyn_set;
	bool warned_resolve, warned_multiview;
};

static int fx_level(void) // 0 off, 1 on, 2 on + trace of memory-related calls
{
	static int on = -1;
	if (on < 0) {
		on = 0;
		const char *e = getenv("QUEST1_FACET");
		if (e) {
			on = atoi(e);
		} else {
			FILE *f = fopen("/etc/quest1-facet", "r");
			if (f) {
				int c = fgetc(f);
				on = c >= '0' && c <= '9' ? c - '0' : 0;
				fclose(f);
			}
		}
	}
	return on;
}

static bool fx_enabled(void) { return fx_level() > 0; }

// next hook in the chain for a device function: ours for direct scanout (qs_*), else the blob's
static PFN_vkVoidFunction fx_next(struct device *d, const char *name)
{
	PFN_vkVoidFunction f = qd_proc(name);
	return f ? f : d->gdpa(d->handle, name);
}

static inline uint32_t fx_hash(uint64_t h) { return (uint32_t)((h >> 4) * 2654435761u) % FX_HASH; }

static struct fx_node *fx_find(struct fx_node **t, uint64_t h)
{
	for (struct fx_node *n = t[fx_hash(h)]; n; n = n->next)
		if (n->h == h)
			return n;
	return NULL;
}

static void fx_add(struct fx_node **t, struct fx_node *n)
{
	uint32_t i = fx_hash(n->h);
	n->next = t[i];
	t[i] = n;
}

static struct fx_node *fx_remove(struct fx_node **t, uint64_t h)
{
	for (struct fx_node **p = &t[fx_hash(h)]; *p; p = &(*p)->next)
		if ((*p)->h == h) {
			struct fx_node *n = *p;
			*p = n->next;
			return n;
		}
	return NULL;
}

static struct fx *fx_get(struct device *d)
{
	static pthread_mutex_t once = PTHREAD_MUTEX_INITIALIZER;
	pthread_mutex_lock(&once);
	if (!d->fx) {
		struct fx *x = calloc(1, sizeof(*x));
		pthread_mutex_init(&x->lock, NULL);
		d->fx = x;
	}
	pthread_mutex_unlock(&once);
	return d->fx;
}

// --- image / view tracking (formats and extents for render passes and framebuffers) ----------------------

static VKAPI_ATTR VkResult VKAPI_CALL fx_CreateImage(VkDevice device, const VkImageCreateInfo *ci,
                                                    const VkAllocationCallbacks *alloc, VkImage *out)
{
	struct device *d = device_of(device);
	VkResult r = ((PFN_vkCreateImage)fx_next(d, "vkCreateImage"))(device, ci, alloc, out);
	if (r == VK_SUCCESS) {
		struct fx *x = fx_get(d);
		struct fx_node *n = calloc(1, sizeof(*n));
		n->h = (uint64_t)*out;
		n->fmt = ci->format;
		n->samples = ci->samples;
		n->ext = ci->extent;
		n->mips = ci->mipLevels;
		pthread_mutex_lock(&x->lock);
		fx_add(x->images, n);
		pthread_mutex_unlock(&x->lock);
	}
	return r;
}

static VKAPI_ATTR void VKAPI_CALL fx_DestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *alloc)
{
	struct device *d = device_of(device);
	struct fx *x = fx_get(d);
	pthread_mutex_lock(&x->lock);
	free(fx_remove(x->images, (uint64_t)image));
	pthread_mutex_unlock(&x->lock);
	((PFN_vkDestroyImage)fx_next(d, "vkDestroyImage"))(device, image, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL fx_CreateImageView(VkDevice device, const VkImageViewCreateInfo *ci,
                                                        const VkAllocationCallbacks *alloc, VkImageView *out)
{
	struct device *d = device_of(device);
	VkResult r = ((PFN_vkCreateImageView)fx_next(d, "vkCreateImageView"))(device, ci, alloc, out);
	if (r == VK_SUCCESS) {
		struct fx *x = fx_get(d);
		struct fx_node *n = calloc(1, sizeof(*n));
		n->h = (uint64_t)*out;
		n->fmt = ci->format;
		n->image = (uint64_t)ci->image;
		n->aspect = ci->subresourceRange.aspectMask;
		n->base_mip = ci->subresourceRange.baseMipLevel;
		pthread_mutex_lock(&x->lock);
		fx_add(x->views, n);
		pthread_mutex_unlock(&x->lock);
	}
	return r;
}

static VKAPI_ATTR void VKAPI_CALL fx_DestroyImageView(VkDevice device, VkImageView view, const VkAllocationCallbacks *alloc)
{
	struct device *d = device_of(device);
	struct fx *x = fx_get(d);
	PFN_vkDestroyFramebuffer destroy_fb = (PFN_vkDestroyFramebuffer)d->gdpa(device, "vkDestroyFramebuffer");
	pthread_mutex_lock(&x->lock);
	free(fx_remove(x->views, (uint64_t)view));
	// cached framebuffers over this view go with it
	for (uint32_t i = 0; i < x->nfbs;) {
		struct fx_fb *f = x->fbs[i];
		bool uses = false;
		for (uint32_t k = 0; k < f->nviews; k++)
			uses |= f->views[k] == view;
		if (uses) {
			PFN_vkDestroyFramebuffer df = (PFN_vkDestroyFramebuffer)fx_next(d, "vkDestroyFramebuffer");
			(df ? df : destroy_fb)(device, f->fb, NULL);
			free(f);
			x->fbs[i] = x->fbs[--x->nfbs];
		} else {
			i++;
		}
	}
	pthread_mutex_unlock(&x->lock);
	((PFN_vkDestroyImageView)fx_next(d, "vkDestroyImageView"))(device, view, alloc);
}

// --- dynamic rendering ----------------------------------------------------------------------------------

#define FX_NONE_LOAD 1000400000   // VK_ATTACHMENT_LOAD_OP_NONE_EXT
#define FX_NONE_STORE 1000301000  // VK_ATTACHMENT_STORE_OP_NONE

static VkImageLayout fx_layout(VkImageLayout l)
{
	switch ((int)l) {
	case 1000241000: // DEPTH_ATTACHMENT_OPTIMAL
	case 1000241002: // STENCIL_ATTACHMENT_OPTIMAL
		return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	case 1000241001: // DEPTH_READ_ONLY_OPTIMAL
	case 1000241003: // STENCIL_READ_ONLY_OPTIMAL
		return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	default: return l;
	}
}

struct fx_att
{
	VkFormat fmt;
	VkSampleCountFlagBits samples;
	VkAttachmentLoadOp load;
	VkAttachmentStoreOp store;
	VkImageLayout layout;
};

//! The render pass for `n` color attachments (fmt UNDEFINED = unused) and an optional depth/stencil
//! one. A pipeline's key leaves ops and layouts at fixed values: they do not matter for compatibility.
static VkRenderPass fx_get_rp(struct device *d, const struct fx_att *colors, uint32_t n, const struct fx_att *ds,
                              uint32_t view_mask)
{
	struct fx *x = fx_get(d);
	uint8_t key[256];
	uint32_t kl = 0;
	memcpy(key + kl, &n, 4), kl += 4;
	memcpy(key + kl, &view_mask, 4), kl += 4;
	for (uint32_t i = 0; i < n && kl + sizeof(struct fx_att) < sizeof(key); i++) {
		struct fx_att a = colors[i];
		memcpy(key + kl, &a, sizeof(a)), kl += sizeof(a);
	}
	if (ds)
		memcpy(key + kl, ds, sizeof(*ds)), kl += sizeof(*ds);
	if (kl > sizeof(key)) // cannot happen for n <= 8
		return VK_NULL_HANDLE;
	pthread_mutex_lock(&x->lock);
	for (uint32_t i = 0; i < x->nrps; i++)
		if (x->rps[i].keylen == kl && !memcmp(x->rps[i].key, key, kl)) {
			VkRenderPass rp = x->rps[i].rp;
			pthread_mutex_unlock(&x->lock);
			return rp;
		}
	pthread_mutex_unlock(&x->lock);

	VkAttachmentDescription desc[9];
	VkAttachmentReference refs[8], dref = {VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED};
	uint32_t na = 0;
	for (uint32_t i = 0; i < n; i++) {
		refs[i] = (VkAttachmentReference){VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
		if (colors[i].fmt == VK_FORMAT_UNDEFINED)
			continue;
		desc[na] = (VkAttachmentDescription){0, colors[i].fmt, colors[i].samples, colors[i].load, colors[i].store,
		                                     VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
		                                     colors[i].layout, colors[i].layout};
		refs[i].attachment = na;
		refs[i].layout = colors[i].layout;
		na++;
	}
	if (ds && ds->fmt != VK_FORMAT_UNDEFINED) {
		desc[na] = (VkAttachmentDescription){0, ds->fmt, ds->samples, ds->load, ds->store, ds->load, ds->store,
		                                     ds->layout, ds->layout};
		dref.attachment = na;
		dref.layout = ds->layout;
		na++;
	}
	VkSubpassDescription sp = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, n, refs, NULL,
	                           dref.attachment != VK_ATTACHMENT_UNUSED ? &dref : NULL, 0, NULL};
	VkRenderPassMultiviewCreateInfo mv = {VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO};
	mv.subpassCount = 1;
	mv.pViewMasks = &view_mask;
	mv.correlationMaskCount = 1;
	mv.pCorrelationMasks = &view_mask;
	VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, view_mask ? &mv : NULL, 0, na, desc, 1, &sp, 0, NULL};
	VkRenderPass rp = VK_NULL_HANDLE;
	VkResult r = ((PFN_vkCreateRenderPass)fx_next(d, "vkCreateRenderPass"))(d->handle, &rpci, NULL, &rp);
	if (r != VK_SUCCESS) {
		LOG("facet: vkCreateRenderPass failed: %d\n", r);
		return VK_NULL_HANDLE;
	}
	pthread_mutex_lock(&x->lock);
	if (x->nrps < FX_RPS) {
		memcpy(x->rps[x->nrps].key, key, kl);
		x->rps[x->nrps].keylen = kl;
		x->rps[x->nrps++].rp = rp;
	}
	pthread_mutex_unlock(&x->lock);
	return rp;
}

static VkFramebuffer fx_get_fb(struct device *d, VkRenderPass rp, const VkImageView *views, uint32_t nv, uint32_t w,
                               uint32_t h, uint32_t layers)
{
	struct fx *x = fx_get(d);
	pthread_mutex_lock(&x->lock);
	for (uint32_t i = 0; i < x->nfbs; i++) {
		struct fx_fb *f = x->fbs[i];
		if (f->rp == rp && f->nviews == nv && f->w == w && f->h == h && f->layers == layers &&
		    !memcmp(f->views, views, nv * sizeof(*views))) {
			VkFramebuffer fb = f->fb;
			pthread_mutex_unlock(&x->lock);
			return fb;
		}
	}
	pthread_mutex_unlock(&x->lock);
	VkFramebufferCreateInfo ci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, nv, views, w, h, layers};
	VkFramebuffer fb = VK_NULL_HANDLE;
	VkResult r = ((PFN_vkCreateFramebuffer)fx_next(d, "vkCreateFramebuffer"))(d->handle, &ci, NULL, &fb);
	if (r != VK_SUCCESS) {
		LOG("facet: vkCreateFramebuffer %ux%u failed: %d\n", w, h, r);
		return VK_NULL_HANDLE;
	}
	struct fx_fb *f = calloc(1, sizeof(*f));
	f->fb = fb;
	f->rp = rp;
	f->nviews = nv;
	memcpy(f->views, views, nv * sizeof(*views));
	f->w = w;
	f->h = h;
	f->layers = layers;
	pthread_mutex_lock(&x->lock);
	if (x->nfbs < FX_FBS)
		x->fbs[x->nfbs++] = f;
	pthread_mutex_unlock(&x->lock);
	return fb;
}

static VKAPI_ATTR void VKAPI_CALL fx_CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri)
{
	struct device *d = device_of(cb);
	struct fx *x = fx_get(d);
	x->n_rendering++;
	struct fx_att colors[8], ds = {VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	                                VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED};
	VkImageView views[9];
	VkClearValue clears[9];
	uint32_t nv = 0, n = ri->colorAttachmentCount < 8 ? ri->colorAttachmentCount : 8;
	uint32_t fw = ri->renderArea.offset.x + ri->renderArea.extent.width;
	uint32_t fh = ri->renderArea.offset.y + ri->renderArea.extent.height;
	bool known_extent = false;
	uint32_t ew = UINT32_MAX, eh = UINT32_MAX;

	for (uint32_t i = 0; i < n; i++) {
		const VkRenderingAttachmentInfo *a = &ri->pColorAttachments[i];
		colors[i] = (struct fx_att){VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT, 0, 0, 0};
		if (a->imageView == VK_NULL_HANDLE)
			continue;
		pthread_mutex_lock(&x->lock);
		struct fx_node *v = fx_find(x->views, (uint64_t)a->imageView);
		struct fx_node *im = v ? fx_find(x->images, v->image) : NULL;
		VkFormat fmt = v ? v->fmt : VK_FORMAT_UNDEFINED;
		VkSampleCountFlagBits sc = im ? im->samples : VK_SAMPLE_COUNT_1_BIT;
		if (im) {
			uint32_t m = v->base_mip;
			uint32_t w = im->ext.width >> m, h = im->ext.height >> m;
			ew = ew < (w ? w : 1) ? ew : (w ? w : 1);
			eh = eh < (h ? h : 1) ? eh : (h ? h : 1);
			known_extent = true;
		}
		pthread_mutex_unlock(&x->lock);
		if (a->resolveMode != VK_RESOLVE_MODE_NONE && !x->warned_resolve) {
			x->warned_resolve = true;
			LOG("facet: color resolve requested (mode %u): not emulated yet\n", a->resolveMode);
		}
		colors[i] = (struct fx_att){fmt, sc,
		                            a->loadOp == (VkAttachmentLoadOp)FX_NONE_LOAD ? VK_ATTACHMENT_LOAD_OP_LOAD : a->loadOp,
		                            a->storeOp == (VkAttachmentStoreOp)FX_NONE_STORE ? VK_ATTACHMENT_STORE_OP_STORE : a->storeOp,
		                            fx_layout(a->imageLayout)};
		views[nv] = a->imageView;
		clears[nv++] = a->clearValue;
	}
	const VkRenderingAttachmentInfo *dpt = ri->pDepthAttachment, *stc = ri->pStencilAttachment;
	if ((dpt && dpt->imageView) || (stc && stc->imageView)) {
		const VkRenderingAttachmentInfo *a = (dpt && dpt->imageView) ? dpt : stc;
		pthread_mutex_lock(&x->lock);
		struct fx_node *v = fx_find(x->views, (uint64_t)a->imageView);
		struct fx_node *im = v ? fx_find(x->images, v->image) : NULL;
		ds.fmt = v ? v->fmt : VK_FORMAT_UNDEFINED;
		ds.samples = im ? im->samples : VK_SAMPLE_COUNT_1_BIT;
		if (im) {
			uint32_t m = v->base_mip;
			uint32_t w = im->ext.width >> m, h = im->ext.height >> m;
			ew = ew < (w ? w : 1) ? ew : (w ? w : 1);
			eh = eh < (h ? h : 1) ? eh : (h ? h : 1);
			known_extent = true;
		}
		pthread_mutex_unlock(&x->lock);
		// load/store of the aspects that are present (a missing aspect keeps the other's ops)
		const VkRenderingAttachmentInfo *dd = dpt && dpt->imageView ? dpt : a;
		ds.load = dd->loadOp == (VkAttachmentLoadOp)FX_NONE_LOAD ? VK_ATTACHMENT_LOAD_OP_LOAD : dd->loadOp;
		ds.store = dd->storeOp == (VkAttachmentStoreOp)FX_NONE_STORE ? VK_ATTACHMENT_STORE_OP_STORE : dd->storeOp;
		ds.layout = fx_layout(a->imageLayout);
		views[nv] = a->imageView;
		clears[nv++] = a->clearValue;
	}
	if (ri->viewMask && !x->warned_multiview) {
		x->warned_multiview = true;
		LOG("facet: viewMask 0x%x (multiview render pass)\n", ri->viewMask);
	}
	VkRenderPass rp = fx_get_rp(d, colors, n, ds.fmt != VK_FORMAT_UNDEFINED ? &ds : NULL, ri->viewMask);
	if (known_extent) {
		fw = ew;
		fh = eh;
	}
	uint32_t layers = ri->viewMask ? 1 : (ri->layerCount ? ri->layerCount : 1);
	VkFramebuffer fb = rp ? fx_get_fb(d, rp, views, nv, fw, fh, layers) : VK_NULL_HANDLE;
	if (!fb) {
		LOG("facet: BeginRendering without a framebuffer\n");
		return;
	}
	if (fx_level() >= 2 && x->n_rendering < 400)
		LOG("facet: BeginRendering area %d,%d %ux%u layers %u mask 0x%x colors %u (fmt %d load %d store %d lay %d) depth fmt %d -> rp %p fb %p %ux%u%s\n",
		    ri->renderArea.offset.x, ri->renderArea.offset.y, ri->renderArea.extent.width, ri->renderArea.extent.height,
		    ri->layerCount, ri->viewMask, n, n ? (int)colors[0].fmt : -1, n ? (int)colors[0].load : -1,
		    n ? (int)colors[0].store : -1, n ? (int)colors[0].layout : -1, (int)ds.fmt, (void *)rp, (void *)fb, fw, fh,
		    known_extent ? "" : " (extent from area)");
	VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, ri->renderArea, nv, clears};
	((PFN_vkCmdBeginRenderPass)fx_next(d, "vkCmdBeginRenderPass"))(
	    cb, &bi, (ri->flags & VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT) ? VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS
	                                                                                : VK_SUBPASS_CONTENTS_INLINE);
}

static VKAPI_ATTR void VKAPI_CALL fx_CmdEndRendering(VkCommandBuffer cb)
{
	struct device *d = device_of(cb);
	((PFN_vkCmdEndRenderPass)fx_next(d, "vkCmdEndRenderPass"))(cb);
}

// Pipelines created for dynamic rendering: a compatible render pass in place of VkPipelineRenderingCreateInfo,
// and no dynamic states the blob does not know.
static bool fx_dyn_unknown(VkDynamicState s)
{
	return (int)s >= 1000267000 /* EDS1 .. */ && (int)s <= 1000267012 ? true
	       : ((int)s >= 1000455000 && (int)s <= 1000455050) /* EDS3 */ || (int)s == 1000143000 /* SAMPLE_LOCATIONS */ ||
	         (int)s == 1000377000 /* RASTERIZER_DISCARD_ENABLE etc. (EDS2) */ || (int)s == 1000377001 ||
	         (int)s == 1000377002 || (int)s == 1000377003 || (int)s == 1000377004 || (int)s == 1000352000 ||
	         (int)s == 1000346000;
}

static VKAPI_ATTR VkResult VKAPI_CALL fx_CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t n,
                                                                const VkGraphicsPipelineCreateInfo *ci,
                                                                const VkAllocationCallbacks *alloc, VkPipeline *out)
{
	struct device *d = device_of(device);
	struct fx *x = fx_get(d);
	VkGraphicsPipelineCreateInfo *c = calloc(n ? n : 1, sizeof(*c));
	VkPipelineDynamicStateCreateInfo *dyn = calloc(n ? n : 1, sizeof(*dyn));
	VkDynamicState **dynlists = calloc(n ? n : 1, sizeof(*dynlists));
	for (uint32_t i = 0; i < n; i++) {
		c[i] = ci[i];
		const VkPipelineRenderingCreateInfo *pr = find_struct(ci[i].pNext, VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO);
		if (pr && ci[i].renderPass == VK_NULL_HANDLE) {
			struct fx_att colors[8], ds = {VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_LOAD,
			                                VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
			uint32_t nc = pr->colorAttachmentCount < 8 ? pr->colorAttachmentCount : 8;
			VkSampleCountFlagBits sc = ci[i].pMultisampleState ? ci[i].pMultisampleState->rasterizationSamples
			                                                    : VK_SAMPLE_COUNT_1_BIT;
			for (uint32_t k = 0; k < nc; k++)
				colors[k] = (struct fx_att){pr->pColorAttachmentFormats[k], sc, VK_ATTACHMENT_LOAD_OP_LOAD,
				                            VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
			ds.fmt = pr->depthAttachmentFormat != VK_FORMAT_UNDEFINED ? pr->depthAttachmentFormat : pr->stencilAttachmentFormat;
			ds.samples = sc;
			c[i].renderPass = fx_get_rp(d, colors, nc, ds.fmt != VK_FORMAT_UNDEFINED ? &ds : NULL, pr->viewMask);
			c[i].subpass = 0;
		}
		if (ci[i].pDynamicState) {
			const VkPipelineDynamicStateCreateInfo *s = ci[i].pDynamicState;
			dynlists[i] = calloc(s->dynamicStateCount + 1, sizeof(VkDynamicState));
			uint32_t k = 0;
			for (uint32_t j = 0; j < s->dynamicStateCount; j++)
				if (!fx_dyn_unknown(s->pDynamicStates[j]))
					dynlists[i][k++] = s->pDynamicStates[j];
				else {
					x->n_dyn_dropped++;
					LOG("facet: pipeline dynamic state %d dropped\n", (int)s->pDynamicStates[j]);
				}
			dyn[i] = *s;
			dyn[i].dynamicStateCount = k;
			dyn[i].pDynamicStates = dynlists[i];
			c[i].pDynamicState = &dyn[i];
		}
	}
	VkResult r = d->CreateGraphicsPipelines(device, cache, n, c, alloc, out);
	for (uint32_t i = 0; i < n; i++)
		free(dynlists[i]);
	free(dynlists);
	free(dyn);
	free(c);
	return r;
}

// --- VK_EXT_host_query_reset -----------------------------------------------------------------------------

static bool fx_reset_init(struct device *d, struct fx *x)
{
	if (x->reset_ok)
		return true;
	if (!d->has_q0)
		return false;
	PFN_vkGetDeviceQueue gq = (PFN_vkGetDeviceQueue)d->gdpa(d->handle, "vkGetDeviceQueue");
	PFN_vkCreateCommandPool cp = (PFN_vkCreateCommandPool)d->gdpa(d->handle, "vkCreateCommandPool");
	PFN_vkAllocateCommandBuffers ac = (PFN_vkAllocateCommandBuffers)d->gdpa(d->handle, "vkAllocateCommandBuffers");
	gq(d->handle, d->q0_family, 0, &x->queue);
	VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
	                              VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, d->q0_family};
	if (cp(d->handle, &pi, NULL, &x->pool) != VK_SUCCESS)
		return false;
	VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, x->pool,
	                                  VK_COMMAND_BUFFER_LEVEL_PRIMARY, FX_SLOTS};
	if (ac(d->handle, &ai, x->cb) != VK_SUCCESS)
		return false;
	for (int i = 0; i < FX_SLOTS; i++) {
		VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		if (d->CreateFence(d->handle, &fi, NULL, &x->fence[i]) != VK_SUCCESS)
			return false;
	}
	x->reset_ok = true;
	return true;
}

static VKAPI_ATTR void VKAPI_CALL fx_ResetQueryPool(VkDevice device, VkQueryPool pool, uint32_t first, uint32_t count)
{
	struct device *dv = device_of(device);
	struct fx *x = fx_get(dv);
	pthread_mutex_lock(&x->lock);
	if (!fx_reset_init(dv, x)) {
		pthread_mutex_unlock(&x->lock);
		LOG("facet: vkResetQueryPool unavailable (no queue)\n");
		return;
	}
	uint32_t s = x->next_slot++ % FX_SLOTS;
	if (x->used[s]) { // the slot's previous reset: wait for it (rare: 16 resets in flight)
		dv->WaitForFences(device, 1, &x->fence[s], VK_TRUE, UINT64_MAX);
		dv->ResetFences(device, 1, &x->fence[s]);
	}
	x->used[s] = true;
	PFN_vkResetCommandBuffer rc = (PFN_vkResetCommandBuffer)dv->gdpa(device, "vkResetCommandBuffer");
	PFN_vkBeginCommandBuffer bc = (PFN_vkBeginCommandBuffer)dv->gdpa(device, "vkBeginCommandBuffer");
	PFN_vkEndCommandBuffer ec = (PFN_vkEndCommandBuffer)dv->gdpa(device, "vkEndCommandBuffer");
	PFN_vkCmdResetQueryPool cr = (PFN_vkCmdResetQueryPool)dv->gdpa(device, "vkCmdResetQueryPool");
	rc(x->cb[s], 0);
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
	                               VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
	bc(x->cb[s], &bi);
	cr(x->cb[s], pool, first, count);
	ec(x->cb[s]);
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &x->cb[s];
	pthread_mutex_lock(&dv->queue_mutex);
	VkResult r = dv->QueueSubmit(x->queue, 1, &si, x->fence[s]);
	pthread_mutex_unlock(&dv->queue_mutex);
	if (r != VK_SUCCESS)
		LOG("facet: reset submit failed: %d\n", r);
	x->n_resets++;
	pthread_mutex_unlock(&x->lock);
}

// --- no-op extended dynamic state / sample locations --------------------------------------------------------


static void fx_destroy(struct device *d)
{
	struct fx *x = d->fx;
	if (!x)
		return;
	LOG("facet: %llu begin-rendering, %llu query resets, %u render passes, %u framebuffers\n",
	    (unsigned long long)x->n_rendering, (unsigned long long)x->n_resets, x->nrps, x->nfbs);
	d->fx = NULL; // the device's objects die with it
}

// --- host-cached + coherent memory type ---------------------------------------------------------------------
// The blob lists a host-visible + coherent type (flags 0x7) and a host-visible + coherent + cached one (0xf), but it
// only lets some buffers use the second: a buffer of 5388 bytes (types 0x19 = 0, 3, 4) has no cached type, and
// Facet fails with "AllocateMemory: Failed to determine a compatible memory type". Same fix as the Quest 2 port
// (quest2-steamos d474e7c5): one more type, the coherent type's flags + HOST_CACHED, backed by that coherent type.
// Coherence is real, only the cached hint is not: slower CPU reads, never wrong.

static uint32_t fx_mem_virt = UINT32_MAX, fx_mem_base;

static void fx_mem_patch(VkPhysicalDeviceMemoryProperties *p)
{
	if (!fx_enabled())
		return;
	const VkMemoryPropertyFlags vc = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	uint32_t n = p->memoryTypeCount, base = n;
	for (uint32_t i = 0; i < n && base == n; i++)
		if ((p->memoryTypes[i].propertyFlags & (vc | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) == vc)
			base = i; // the first coherent type that is not cached
	if (base == n || n >= VK_MAX_MEMORY_TYPES)
		return;
	p->memoryTypes[n].propertyFlags = p->memoryTypes[base].propertyFlags | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
	p->memoryTypes[n].heapIndex = p->memoryTypes[base].heapIndex;
	p->memoryTypeCount = n + 1;
	if (fx_mem_virt != n)
		LOG("facet: advertising memory type %u (flags 0x%x) backed by type %u\n", n, p->memoryTypes[n].propertyFlags, base);
	fx_mem_virt = n;
	fx_mem_base = base;
}

static void fx_mem_req(VkMemoryRequirements *r)
{
	if (fx_mem_virt != UINT32_MAX && (r->memoryTypeBits & (1u << fx_mem_base)))
		r->memoryTypeBits |= 1u << fx_mem_virt;
}

static VKAPI_ATTR void VKAPI_CALL fx_GetPhysicalDeviceMemoryProperties(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties *p)
{
	struct instance *in = instance_of(pd);
	((PFN_vkGetPhysicalDeviceMemoryProperties)in->gipa(in->handle, "vkGetPhysicalDeviceMemoryProperties"))(pd, p);
	fx_mem_patch(p);
}

static VKAPI_ATTR void VKAPI_CALL fx_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties2 *p)
{
	struct instance *in = instance_of(pd);
	PFN_vkGetPhysicalDeviceMemoryProperties2 f = (PFN_vkGetPhysicalDeviceMemoryProperties2)in->gipa(in->handle, "vkGetPhysicalDeviceMemoryProperties2");
	if (!f)
		f = (PFN_vkGetPhysicalDeviceMemoryProperties2)in->gipa(in->handle, "vkGetPhysicalDeviceMemoryProperties2KHR");
	if (f)
		f(pd, p);
	else
		((PFN_vkGetPhysicalDeviceMemoryProperties)in->gipa(in->handle, "vkGetPhysicalDeviceMemoryProperties"))(pd, &p->memoryProperties);
	fx_mem_patch(&p->memoryProperties);
}

static VKAPI_ATTR void VKAPI_CALL fx_GetBufferMemoryRequirements(VkDevice device, VkBuffer b, VkMemoryRequirements *r)
{
	struct device *d = device_of(device);
	((PFN_vkGetBufferMemoryRequirements)d->gdpa(device, "vkGetBufferMemoryRequirements"))(device, b, r);
	fx_mem_req(r);
}

static VKAPI_ATTR void VKAPI_CALL fx_GetImageMemoryRequirements(VkDevice device, VkImage im, VkMemoryRequirements *r)
{
	struct device *d = device_of(device);
	((PFN_vkGetImageMemoryRequirements)d->gdpa(device, "vkGetImageMemoryRequirements"))(device, im, r);
	fx_mem_req(r);
}

static VKAPI_ATTR void VKAPI_CALL fx_GetBufferMemoryRequirements2(VkDevice device, const VkBufferMemoryRequirementsInfo2 *i,
                                                                 VkMemoryRequirements2 *r)
{
	struct device *d = device_of(device);
	PFN_vkGetBufferMemoryRequirements2 f = (PFN_vkGetBufferMemoryRequirements2)d->gdpa(device, "vkGetBufferMemoryRequirements2");
	if (!f)
		f = (PFN_vkGetBufferMemoryRequirements2)d->gdpa(device, "vkGetBufferMemoryRequirements2KHR");
	f(device, i, r);
	fx_mem_req(&r->memoryRequirements);
}

static VKAPI_ATTR void VKAPI_CALL fx_GetImageMemoryRequirements2(VkDevice device, const VkImageMemoryRequirementsInfo2 *i,
                                                                VkMemoryRequirements2 *r)
{
	struct device *d = device_of(device);
	PFN_vkGetImageMemoryRequirements2 f = (PFN_vkGetImageMemoryRequirements2)d->gdpa(device, "vkGetImageMemoryRequirements2");
	if (!f)
		f = (PFN_vkGetImageMemoryRequirements2)d->gdpa(device, "vkGetImageMemoryRequirements2KHR");
	f(device, i, r);
	fx_mem_req(&r->memoryRequirements);
}

static int fx_trace_left = 60;
#define FX_TRACE(...) do { if (fx_level() >= 2 && fx_trace_left > 0) { fx_trace_left--; LOG(__VA_ARGS__); } } while (0)

static VKAPI_ATTR VkResult VKAPI_CALL fx_AllocateMemory(VkDevice device, const VkMemoryAllocateInfo *ai,
                                                       const VkAllocationCallbacks *alloc, VkDeviceMemory *out)
{
	struct device *d = device_of(device);
	VkMemoryAllocateInfo copy;
	uint32_t asked = ai->memoryTypeIndex;
	if (fx_mem_virt != UINT32_MAX && asked == fx_mem_virt) {
		copy = *ai;
		copy.memoryTypeIndex = fx_mem_base;
		ai = &copy;
	}
	VkResult r = ((PFN_vkAllocateMemory)fx_next(d, "vkAllocateMemory"))(device, ai, alloc, out);
	if (r != VK_SUCCESS)
		FX_TRACE("facet: AllocateMemory size %llu type %u (asked %u) failed: %d\n", (unsigned long long)ai->allocationSize,
		         ai->memoryTypeIndex, asked, r);
	return r;
}

// --- render pass 2 -----------------------------------------------------------------------------------------
// Facet records its passes with vkCmdBeginRenderPass2(KHR) / vkCmdEndRenderPass2(KHR). Direct scanout hooks only
// the classic begin/end (qs_CmdBeginRenderPass redirects each eye's pass into its own buffer), so these go to them.

static uint64_t fx_rp2_begins;

static VKAPI_ATTR void VKAPI_CALL fx_CmdBeginRenderPass2(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi,
                                                        const VkSubpassBeginInfo *sbi)
{
	struct device *d = device_of(cb);
	fx_rp2_begins++;
	if (fx_level() >= 2 && fx_rp2_begins < 60)
		LOG("facet: BeginRenderPass2 rp %p fb %p area %d,%d %ux%u clears %u\n", (void *)bi->renderPass, (void *)bi->framebuffer,
		    bi->renderArea.offset.x, bi->renderArea.offset.y, bi->renderArea.extent.width, bi->renderArea.extent.height,
		    bi->clearValueCount);
	((PFN_vkCmdBeginRenderPass)fx_next(d, "vkCmdBeginRenderPass"))(cb, bi, sbi ? sbi->contents : VK_SUBPASS_CONTENTS_INLINE);
}

static VKAPI_ATTR void VKAPI_CALL fx_CmdEndRenderPass2(VkCommandBuffer cb, const VkSubpassEndInfo *sei)
{
	struct device *d = device_of(cb);
	((PFN_vkCmdEndRenderPass)fx_next(d, "vkCmdEndRenderPass"))(cb);
}

static VKAPI_ATTR void VKAPI_CALL fx_CmdNextSubpass2(VkCommandBuffer cb, const VkSubpassBeginInfo *sbi, const VkSubpassEndInfo *sei)
{
	struct device *d = device_of(cb);
	((PFN_vkCmdNextSubpass)fx_next(d, "vkCmdNextSubpass"))(cb, sbi ? sbi->contents : VK_SUBPASS_CONTENTS_INLINE);
}

//! Entry points of the emulated extensions (only when the Facet knob is on).
static PFN_vkVoidFunction fx_proc(const char *name)
{
	if (!fx_enabled())
		return NULL;
#define F(n, fn) if (strcmp(name, "vk" n) == 0) return (PFN_vkVoidFunction)fn;
	F("CmdBeginRendering", fx_CmdBeginRendering)
	F("CmdBeginRenderingKHR", fx_CmdBeginRendering)
	F("CmdEndRendering", fx_CmdEndRendering)
	F("CmdEndRenderingKHR", fx_CmdEndRendering)
	F("ResetQueryPool", fx_ResetQueryPool)
	F("ResetQueryPoolEXT", fx_ResetQueryPool)
	F("CreateImage", fx_CreateImage)
	F("DestroyImage", fx_DestroyImage)
	F("CreateImageView", fx_CreateImageView)
	F("DestroyImageView", fx_DestroyImageView)
	F("CreateGraphicsPipelines", fx_CreateGraphicsPipelines)
	F("GetBufferMemoryRequirements", fx_GetBufferMemoryRequirements)
	F("GetImageMemoryRequirements", fx_GetImageMemoryRequirements)
	F("GetBufferMemoryRequirements2", fx_GetBufferMemoryRequirements2)
	F("GetBufferMemoryRequirements2KHR", fx_GetBufferMemoryRequirements2)
	F("GetImageMemoryRequirements2", fx_GetImageMemoryRequirements2)
	F("GetImageMemoryRequirements2KHR", fx_GetImageMemoryRequirements2)
	F("AllocateMemory", fx_AllocateMemory)
	F("CmdBeginRenderPass2", fx_CmdBeginRenderPass2)
	F("CmdBeginRenderPass2KHR", fx_CmdBeginRenderPass2)
	F("CmdEndRenderPass2", fx_CmdEndRenderPass2)
	F("CmdEndRenderPass2KHR", fx_CmdEndRenderPass2)
	F("CmdNextSubpass2", fx_CmdNextSubpass2)
	F("CmdNextSubpass2KHR", fx_CmdNextSubpass2)
#undef F
	return NULL;
}

static PFN_vkVoidFunction fx_iproc(const char *name)
{
	if (!fx_enabled())
		return NULL;
#define F(n, fn) if (strcmp(name, "vk" n) == 0) return (PFN_vkVoidFunction)fn;
	F("GetPhysicalDeviceMemoryProperties", fx_GetPhysicalDeviceMemoryProperties)
	F("GetPhysicalDeviceMemoryProperties2", fx_GetPhysicalDeviceMemoryProperties2)
	F("GetPhysicalDeviceMemoryProperties2KHR", fx_GetPhysicalDeviceMemoryProperties2)
#undef F
	return NULL;
}
