// quest1_rpmerge: merges vrcompositor's one-draw render passes (included by quest1_scanout.c, vrcompositor only).
//
// vrcompositor draws each layer of an eye (environment, overlays, controllers, laser...) in a render pass of its own.
// Before each draw it rewrites the draw's uniforms with vkCmdUpdateBuffer, between a fragment-shader-to-transfer
// barrier (the previous draw must have read them) and a transfer-to-vertex-shader one. Every draw is thus a render pass
// that loads and stores the whole 1500x1664 colour and depth images, after a full drain of the GPU: ~25 an eye.
// Measured 2026-11-08 with the passes merged (uniforms dropped): 13-14.3 -> 8.4-10 ms of GPU a frame, GPU busy
// 92 -> 69 %, 63 -> 72 fps.
//
// The layer keeps the render pass open across them instead:
// - The app's vkCmdEndRenderPass is held back. If the next render pass begins on the same framebuffer with a render
//   pass that loads what the open one stores (same formats, layouts and store ops), it continues the open one.
// - The uniform updates in between are absorbed: the data goes into a CPU copy of the buffer (rm_buf.shadow, kept for
//   every buffer that is only a uniform and transfer buffer, up to 64 KB). The buffer barriers that only cover such
//   buffers are dropped.
// - Before each draw in a merged pass, every bound descriptor set whose uniform-buffer descriptors read an updated
//   buffer is replaced by a copy (from a descriptor pool of the command buffer, reset when it is begun again) whose
//   descriptors point at a snapshot of the shadow in the command buffer's staging (the ub_* arenas of
//   quest1_compat.c, uniform-capable).
// - When the merged pass really ends (another framebuffer, a transfer, a dispatch, an image barrier, the end of the
//   command buffer...), the updated buffers get their latest contents with one copy between two barriers, and the
//   app's own descriptor sets are bound again.
// - A draw whose sets cannot be redirected (unknown set, storage or dynamic descriptor on an updated buffer, no shadow
//   for its range, pool exhausted) splits the pass: end, buffers updated, begin again with a loading render pass.
// "nomerge" in /tmp/quest1-layer turns it off (read once a second; from the next render pass).

#define RM_BUF_MAX (64 * 1024)
#define RM_SLOTS 8
#define RM_DIRTY 16
#define RM_SET_BUFS 8
#define RM_HASH 256
#define RM_SETS 4096
#define RM_LAYOUTS 256
#define RM_RPS 128
#define RM_CBS 32

static bool ub_alloc(struct device *d, VkCommandBuffer cb, VkDeviceSize size, VkBuffer *buf, VkDeviceSize *off,
                     void **ptr); // quest1_compat.c

static bool rm_active(void) { return qs_enabled(); }

static bool rm_off(void)
{
	static _Atomic uint64_t checked;
	static _Atomic bool off;
	uint64_t t = now_ns();
	if (t - checked > 1000000000ull) {
		checked = t;
		char b[64] = {0};
		FILE *f = fopen("/tmp/quest1-layer", "r");
		if (f) {
			b[fread(b, 1, sizeof(b) - 1, f)] = 0;
			fclose(f);
		}
		off = strstr(b, "nomerge") != NULL;
	}
	return off;
}

static pthread_mutex_t rm_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned rm_hash(uint64_t h) { return (unsigned)((h >> 4) * 2654435761u); }

// --- buffers that are only uniform/transfer buffers, with a CPU copy of what vkCmdUpdateBuffer wrote --------------

struct rm_buf
{
	VkBuffer b;
	VkDeviceSize size;
	uint8_t *shadow, *valid; //!< valid: a bit per byte written by an update
	struct rm_buf *next;
};
static struct rm_buf *rm_bufs[RM_HASH];

//! Under rm_lock.
static struct rm_buf *rm_buf_find(VkBuffer b)
{
	for (struct rm_buf *r = rm_bufs[rm_hash((uint64_t)b) % RM_HASH]; r; r = r->next)
		if (r->b == b)
			return r;
	return NULL;
}

static void rm_forget_buffer(VkBuffer b)
{
	if (!rm_active() || !b)
		return;
	pthread_mutex_lock(&rm_lock);
	for (struct rm_buf **p = &rm_bufs[rm_hash((uint64_t)b) % RM_HASH]; *p; p = &(*p)->next)
		if ((*p)->b == b) {
			struct rm_buf *r = *p;
			*p = r->next;
			free(r->shadow);
			free(r->valid);
			free(r);
			break;
		}
	pthread_mutex_unlock(&rm_lock);
}

static void rm_note_buffer(VkBuffer b, const VkBufferCreateInfo *ci)
{
	const VkBufferUsageFlags ok =
	    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	if (!rm_active())
		return;
	rm_forget_buffer(b); // a handle used again
	if (!(ci->usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) || (ci->usage & ~ok) || ci->size > RM_BUF_MAX)
		return;
	struct rm_buf *r = calloc(1, sizeof(*r));
	if (!r)
		return;
	r->b = b;
	r->size = ci->size;
	r->shadow = calloc(1, ci->size);
	r->valid = calloc(1, (ci->size + 7) / 8);
	if (!r->shadow || !r->valid) {
		free(r->shadow);
		free(r->valid);
		free(r);
		return;
	}
	pthread_mutex_lock(&rm_lock);
	unsigned h = rm_hash((uint64_t)b) % RM_HASH;
	r->next = rm_bufs[h];
	rm_bufs[h] = r;
	pthread_mutex_unlock(&rm_lock);
}

static void rm_buf_mark(struct rm_buf *r, VkDeviceSize off, VkDeviceSize size, bool valid)
{
	for (VkDeviceSize i = off; i < off + size && i < r->size; i++)
		if (valid)
			r->valid[i / 8] |= (uint8_t)(1u << (i % 8));
		else
			r->valid[i / 8] &= (uint8_t)~(1u << (i % 8));
}

static bool rm_buf_valid(const struct rm_buf *r, VkDeviceSize off, VkDeviceSize size)
{
	if (off + size > r->size)
		return false;
	for (VkDeviceSize i = off; i < off + size; i++)
		if (!(r->valid[i / 8] & (1u << (i % 8))))
			return false;
	return true;
}

//! A transfer into b that the layer does not see the data of (vkCmdCopyBuffer, vkCmdFillBuffer).
static void rm_buf_unknown(VkBuffer b, VkDeviceSize off, VkDeviceSize size)
{
	if (!rm_active())
		return;
	pthread_mutex_lock(&rm_lock);
	struct rm_buf *r = rm_buf_find(b);
	if (r)
		rm_buf_mark(r, off, size == VK_WHOLE_SIZE ? r->size - off : size, false);
	pthread_mutex_unlock(&rm_lock);
}

// --- descriptor set layouts and sets ----------------------------------------------------------------------------

struct rm_layout
{
	VkDescriptorSetLayout l;
	uint32_t n;
	struct
	{
		uint32_t binding, count;
		VkDescriptorType type;
	} b[32];
};
static struct rm_layout rm_layouts[RM_LAYOUTS];
static unsigned rm_nlayouts;

//! Under rm_lock.
static const struct rm_layout *rm_layout_find(VkDescriptorSetLayout l)
{
	for (unsigned i = 0; i < RM_LAYOUTS && i < rm_nlayouts; i++)
		if (rm_layouts[i].l == l)
			return &rm_layouts[i];
	return NULL;
}

static void rm_note_layout(VkDescriptorSetLayout l, const VkDescriptorSetLayoutCreateInfo *ci)
{
	if (!rm_active())
		return;
	pthread_mutex_lock(&rm_lock);
	struct rm_layout *e = (struct rm_layout *)rm_layout_find(l);
	if (!e)
		e = &rm_layouts[rm_nlayouts++ % RM_LAYOUTS];
	memset(e, 0, sizeof(*e));
	if (ci->bindingCount <= 32 && !ci->pNext) { // else unknown: its sets are never redirected
		e->l = l;
		e->n = ci->bindingCount;
		for (uint32_t i = 0; i < ci->bindingCount; i++) {
			e->b[i].binding = ci->pBindings[i].binding;
			e->b[i].count = ci->pBindings[i].descriptorCount;
			e->b[i].type = ci->pBindings[i].descriptorType;
		}
	}
	pthread_mutex_unlock(&rm_lock);
}

struct rm_set
{
	VkDescriptorSet s;
	VkDescriptorSetLayout l;
	bool bad; //!< contents not known
	uint32_t nbuf;
	struct
	{
		uint32_t binding, elem;
		VkDescriptorType type;
		VkBuffer b;
		VkDeviceSize off, range;
	} buf[RM_SET_BUFS];
};
static struct rm_set rm_sets[RM_SETS];

//! Under rm_lock: the entry of s (add: a free or reused one).
static struct rm_set *rm_set_find(VkDescriptorSet s, bool add)
{
	unsigned h = rm_hash((uint64_t)s);
	for (unsigned k = 0; k < 16; k++) {
		struct rm_set *e = &rm_sets[(h + k) % RM_SETS];
		if (e->s == s || (add && !e->s))
			return e;
	}
	return add ? &rm_sets[h % RM_SETS] : NULL; // evicts one: it is then unknown, and split on
}

static void rm_note_sets(const VkDescriptorSetAllocateInfo *ai, const VkDescriptorSet *sets)
{
	if (!rm_active())
		return;
	pthread_mutex_lock(&rm_lock);
	for (uint32_t i = 0; i < ai->descriptorSetCount; i++) {
		struct rm_set *e = rm_set_find(sets[i], true);
		memset(e, 0, sizeof(*e));
		e->s = sets[i];
		e->l = ai->pSetLayouts[i];
		e->bad = ai->pNext != NULL || !rm_layout_find(e->l);
	}
	pthread_mutex_unlock(&rm_lock);
}

static bool rm_buffer_type(VkDescriptorType t)
{
	return t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
	       t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}

static void rm_note_writes(uint32_t nw, const VkWriteDescriptorSet *w, uint32_t nc, const VkCopyDescriptorSet *c)
{
	if (!rm_active())
		return;
	pthread_mutex_lock(&rm_lock);
	for (uint32_t i = 0; i < nw; i++) {
		struct rm_set *e = rm_set_find(w[i].dstSet, false);
		if (!e || e->bad)
			continue;
		const struct rm_layout *l = rm_layout_find(e->l);
		uint32_t count = 0;
		for (uint32_t k = 0; l && k < l->n; k++)
			if (l->b[k].binding == w[i].dstBinding)
				count = l->b[k].count;
		if (w[i].dstArrayElement + w[i].descriptorCount > count) { // runs over into the next binding
			e->bad = true;
			continue;
		}
		if (!rm_buffer_type(w[i].descriptorType))
			continue;
		for (uint32_t j = 0; j < w[i].descriptorCount && !e->bad; j++) {
			uint32_t elem = w[i].dstArrayElement + j, k;
			for (k = 0; k < e->nbuf; k++)
				if (e->buf[k].binding == w[i].dstBinding && e->buf[k].elem == elem)
					break;
			if (k == RM_SET_BUFS) {
				e->bad = true;
				break;
			}
			if (k == e->nbuf)
				e->nbuf++;
			const VkDescriptorBufferInfo *bi = &w[i].pBufferInfo[j];
			e->buf[k].binding = w[i].dstBinding;
			e->buf[k].elem = elem;
			e->buf[k].type = w[i].descriptorType;
			e->buf[k].b = bi->buffer;
			e->buf[k].off = bi->offset;
			e->buf[k].range = bi->range;
		}
	}
	for (uint32_t i = 0; i < nc; i++) { // copies from other sets: not followed
		struct rm_set *e = rm_set_find(c[i].dstSet, false);
		if (e)
			e->bad = true;
	}
	pthread_mutex_unlock(&rm_lock);
}

static void rm_note_template_write(VkDescriptorSet s)
{
	if (!rm_active())
		return;
	pthread_mutex_lock(&rm_lock);
	struct rm_set *e = rm_set_find(s, false);
	if (e)
		e->bad = true;
	pthread_mutex_unlock(&rm_lock);
}

// --- render passes ------------------------------------------------------------------------------------------------

struct rm_rp
{
	VkRenderPass rp, load; //!< load: the same, loading every attachment (to begin again after a split)
	uint32_t n, ncol;
	bool ds;
	VkAttachmentDescription att[4];
};
static struct rm_rp rm_rps[RM_RPS];
static unsigned rm_nrps;

//! Under rm_lock.
static const struct rm_rp *rm_rp_find(VkRenderPass rp)
{
	for (unsigned i = 0; i < RM_RPS && i < rm_nrps; i++)
		if (rm_rps[i].rp == rp)
			return &rm_rps[i];
	return NULL;
}

static void rm_note_rp(struct device *d, VkRenderPass rp, const VkRenderPassCreateInfo *ci)
{
	if (!rm_active() || ci->pNext || ci->subpassCount != 1 || ci->attachmentCount == 0 || ci->attachmentCount > 4)
		return;
	const VkSubpassDescription *sp = &ci->pSubpasses[0];
	if (sp->inputAttachmentCount || sp->pResolveAttachments || sp->preserveAttachmentCount)
		return;
	struct rm_rp e = {rp, VK_NULL_HANDLE, ci->attachmentCount, sp->colorAttachmentCount, sp->pDepthStencilAttachment != NULL};
	VkAttachmentDescription load[4];
	bool loads = true;
	for (uint32_t i = 0; i < ci->attachmentCount; i++) {
		e.att[i] = load[i] = ci->pAttachments[i];
		if (load[i].loadOp != VK_ATTACHMENT_LOAD_OP_LOAD || load[i].stencilLoadOp == VK_ATTACHMENT_LOAD_OP_CLEAR)
			loads = false;
		load[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		if (load[i].stencilLoadOp == VK_ATTACHMENT_LOAD_OP_CLEAR)
			load[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		if (load[i].initialLayout == VK_IMAGE_LAYOUT_UNDEFINED)
			load[i].initialLayout = load[i].finalLayout;
	}
	if (loads)
		e.load = rp;
	else {
		VkRenderPassCreateInfo lci = *ci;
		lci.pAttachments = load;
		if (d->CreateRenderPass(d->handle, &lci, NULL, &e.load) != VK_SUCCESS)
			e.load = VK_NULL_HANDLE;
	}
	pthread_mutex_lock(&rm_lock);
	struct rm_rp *slot = (struct rm_rp *)rm_rp_find(rp);
	if (!slot)
		slot = &rm_rps[rm_nrps++ % RM_RPS]; // (an evicted variant is leaked: vrcompositor makes a few dozen)
	*slot = e;
	pthread_mutex_unlock(&rm_lock);
}

static void rm_forget_rp(struct device *d, VkRenderPass rp)
{
	if (!rm_active())
		return;
	pthread_mutex_lock(&rm_lock);
	struct rm_rp *e = (struct rm_rp *)rm_rp_find(rp);
	VkRenderPass load = e && e->load != rp ? e->load : VK_NULL_HANDLE;
	if (e)
		memset(e, 0, sizeof(*e));
	pthread_mutex_unlock(&rm_lock);
	if (load) {
		PFN_vkDestroyRenderPass destroy = (PFN_vkDestroyRenderPass)d->gdpa(d->handle, "vkDestroyRenderPass");
		destroy(d->handle, load, NULL);
	}
}

//! Whether `b` can continue the open pass `a`: it loads what `a` stores, into the same layouts.
static bool rm_continues(const struct rm_rp *a, const struct rm_rp *b)
{
	if (a->n != b->n || a->ncol != b->ncol || a->ds != b->ds)
		return false;
	for (uint32_t i = 0; i < a->n; i++) {
		const VkAttachmentDescription *x = &a->att[i], *y = &b->att[i];
		if (x->format != y->format || x->samples != y->samples || x->storeOp != y->storeOp ||
		    x->stencilStoreOp != y->stencilStoreOp || x->finalLayout != y->finalLayout ||
		    y->initialLayout != x->finalLayout || y->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ||
		    y->stencilLoadOp == VK_ATTACHMENT_LOAD_OP_CLEAR)
			return false;
	}
	return true;
}

// --- per command buffer -------------------------------------------------------------------------------------------

struct rm_slot
{
	VkDescriptorSet orig, cur; //!< the app's set, the one bound (its copy when redirected)
	VkPipelineLayout pl;
	unsigned ver; //!< rm_cb.ver when cur was made
	bool dyn;     //!< bound with dynamic offsets
};

struct rm_cb
{
	VkCommandBuffer cb;
	struct device *d;
	VkDescriptorPool pool;
	bool cand;     //!< the app's current render pass may go on into its next one
	bool deferred; //!< the app ended it, the layer did not
	bool dropped;  //!< barriers dropped since
	VkRenderPass rp, cont; //!< the open pass as begun, the app's latest in it
	VkFramebuffer fb;
	VkRect2D area;
	unsigned ndirty, ver;
	struct
	{
		VkBuffer b;
		VkDeviceSize lo, hi;
	} dirty[RM_DIRTY]; //!< updates absorbed in this pass
	struct rm_slot slot[RM_SLOTS];
};
static struct rm_cb rm_cbs[RM_CBS];
static _Atomic unsigned rm_stat_merged, rm_stat_splits, rm_stat_redirects;

static struct rm_cb *rm_get(VkCommandBuffer cb)
{
	if (!rm_active())
		return NULL;
	struct rm_cb *r = NULL;
	pthread_mutex_lock(&rm_lock);
	for (unsigned i = 0; i < RM_CBS && !r; i++)
		if (rm_cbs[i].cb == cb)
			r = &rm_cbs[i];
	pthread_mutex_unlock(&rm_lock);
	return r;
}

//! vkBeginCommandBuffer: the command buffer is not pending, its descriptor pool can be reset.
static void rm_cb_begin(struct device *d, VkCommandBuffer cb)
{
	if (!rm_active())
		return;
	pthread_mutex_lock(&rm_lock);
	struct rm_cb *c = NULL;
	for (unsigned i = 0; i < RM_CBS && !c; i++)
		if (rm_cbs[i].cb == cb)
			c = &rm_cbs[i];
	for (unsigned i = 0; i < RM_CBS && !c; i++)
		if (!rm_cbs[i].cb) {
			c = &rm_cbs[i];
			c->cb = cb;
			c->d = d;
			c->pool = VK_NULL_HANDLE;
		}
	pthread_mutex_unlock(&rm_lock);
	if (!c)
		return; // more command buffers than tracked: those keep their passes
	VkDescriptorPool pool = c->pool;
	memset(c, 0, sizeof(*c));
	c->cb = cb;
	c->d = d;
	c->pool = pool;
	if (pool) {
		PFN_vkResetDescriptorPool reset = (PFN_vkResetDescriptorPool)d->gdpa(d->handle, "vkResetDescriptorPool");
		reset(d->handle, pool, 0);
	}
}

//! The command buffer is freed (or its pool destroyed).
static void rm_cb_drop(struct device *d, VkCommandBuffer cb)
{
	if (!rm_active())
		return;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	pthread_mutex_lock(&rm_lock);
	for (unsigned i = 0; i < RM_CBS; i++)
		if (rm_cbs[i].cb == cb) {
			pool = rm_cbs[i].pool;
			memset(&rm_cbs[i], 0, sizeof(rm_cbs[i]));
		}
	pthread_mutex_unlock(&rm_lock);
	if (pool) {
		PFN_vkDestroyDescriptorPool destroy = (PFN_vkDestroyDescriptorPool)d->gdpa(d->handle, "vkDestroyDescriptorPool");
		destroy(d->handle, pool, NULL);
	}
}

#define RM_NEXT(type, fn) \
	static type fn; \
	if (!fn) \
		fn = (type)c->d->gdpa(c->d->handle, "vk" #fn);

//! The updated buffers get their latest contents; the app's sets are bound again. Outside a render pass.
static void rm_settle(struct rm_cb *c)
{
	RM_NEXT(PFN_vkCmdPipelineBarrier, CmdPipelineBarrier)
	RM_NEXT(PFN_vkCmdBindDescriptorSets, CmdBindDescriptorSets)
	VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
	                      VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
	if (c->dropped || c->ndirty)
		CmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
		                   NULL, 0, NULL);
	if (c->ndirty) {
		for (unsigned i = 0; i < c->ndirty; i++) {
			VkDeviceSize lo = c->dirty[i].lo, n = c->dirty[i].hi - lo;
			uint8_t tmp[RM_BUF_MAX];
			bool ok = false;
			pthread_mutex_lock(&rm_lock);
			struct rm_buf *r = rm_buf_find(c->dirty[i].b);
			if (r && rm_buf_valid(r, lo, n)) {
				memcpy(tmp, r->shadow + lo, n);
				ok = true;
			}
			pthread_mutex_unlock(&rm_lock);
			VkBuffer sb;
			VkDeviceSize soff;
			void *p;
			if (!ok)
				LOG("render pass merge: no copy of buffer %p [%llu, %llu): its update is lost\n", (void *)c->dirty[i].b,
				    (unsigned long long)lo, (unsigned long long)(lo + n));
			else if (ub_alloc(c->d, c->cb, n, &sb, &soff, &p)) {
				memcpy(p, tmp, n);
				VkBufferCopy bc = {soff, lo, n};
				c->d->CmdCopyBuffer(c->cb, sb, c->dirty[i].b, 1, &bc);
			} else {
				c->d->CmdUpdateBuffer(c->cb, c->dirty[i].b, lo, n, tmp);
			}
		}
		CmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
		                   NULL, 0, NULL);
	}
	for (unsigned i = 0; i < RM_SLOTS; i++) {
		struct rm_slot *s = &c->slot[i];
		if (s->orig && s->cur != s->orig) {
			CmdBindDescriptorSets(c->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pl, i, 1, &s->orig, 0, NULL);
			s->cur = s->orig;
		}
	}
	c->ndirty = 0;
	c->dropped = false;
}

//! The held-back end of the app's render pass, before a command that cannot go in it.
static void rm_close(struct rm_cb *c)
{
	RM_NEXT(PFN_vkCmdEndRenderPass, CmdEndRenderPass)
	c->deferred = false;
	c->cand = false;
	CmdEndRenderPass(c->cb);
	rm_settle(c);
}

//! Inside a merged pass, before a draw that cannot read the updates through copies: end, settle, begin again.
static void rm_split(struct rm_cb *c)
{
	RM_NEXT(PFN_vkCmdEndRenderPass, CmdEndRenderPass)
	RM_NEXT(PFN_vkCmdBeginRenderPass, CmdBeginRenderPass)
	pthread_mutex_lock(&rm_lock);
	const struct rm_rp *r = rm_rp_find(c->cont);
	VkRenderPass load = r ? r->load : VK_NULL_HANDLE;
	pthread_mutex_unlock(&rm_lock);
	CmdEndRenderPass(c->cb);
	rm_settle(c);
	rm_stat_splits++;
	if (!load) { // cannot happen: a pass is only continued by a loading one
		LOG("render pass merge: no loading render pass to split %p\n", (void *)c->cont);
		load = c->cont;
	}
	VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, load, c->fb, c->area, 0, NULL};
	CmdBeginRenderPass(c->cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
	c->rp = load;
}

static void rm_flush(VkCommandBuffer cb)
{
	struct rm_cb *c = rm_get(cb);
	if (c && c->deferred)
		rm_close(c);
}

//! vkCmdBeginRenderPass: true when it continues the open pass (the caller records nothing).
static bool rm_begin(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, VkSubpassContents contents, bool eye_pass)
{
	struct rm_cb *c = rm_get(cb);
	if (!c)
		return false;
	pthread_mutex_lock(&rm_lock);
	const struct rm_rp *r = rm_rp_find(bi->renderPass), *o = c->deferred ? rm_rp_find(c->rp) : NULL;
	bool cont = r && o && bi->framebuffer == c->fb && contents == VK_SUBPASS_CONTENTS_INLINE && rm_continues(o, r) &&
	            bi->renderArea.offset.x >= c->area.offset.x && bi->renderArea.offset.y >= c->area.offset.y &&
	            bi->renderArea.offset.x + bi->renderArea.extent.width <= c->area.offset.x + c->area.extent.width &&
	            bi->renderArea.offset.y + bi->renderArea.extent.height <= c->area.offset.y + c->area.extent.height;
	bool cand = r && r->load && contents == VK_SUBPASS_CONTENTS_INLINE && !eye_pass && !rm_off();
	pthread_mutex_unlock(&rm_lock);
	if (cont) {
		c->deferred = false;
		c->cont = bi->renderPass;
		rm_stat_merged++;
		return true;
	}
	if (c->deferred)
		rm_close(c);
	c->cand = cand;
	if (cand) {
		c->rp = c->cont = bi->renderPass;
		c->fb = bi->framebuffer;
		c->area = bi->renderArea;
	}
	return false;
}

//! vkCmdEndRenderPass: true when held back.
static bool rm_end(VkCommandBuffer cb)
{
	struct rm_cb *c = rm_get(cb);
	if (!c || !c->cand)
		return false;
	c->deferred = true;
	return true;
}

//! vkCmdPipelineBarrier: true to drop it (buffer barriers of uniform buffers between two passes being merged).
static bool rm_barrier(VkCommandBuffer cb, uint32_t nm, uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni)
{
	struct rm_cb *c = rm_get(cb);
	if (!c || !c->deferred)
		return false;
	bool drop = nm == 0 && ni == 0 && nb > 0;
	pthread_mutex_lock(&rm_lock);
	for (uint32_t i = 0; i < nb && drop; i++)
		drop = rm_buf_find(b[i].buffer) != NULL && b[i].srcQueueFamilyIndex == b[i].dstQueueFamilyIndex;
	pthread_mutex_unlock(&rm_lock);
	if (drop) {
		c->dropped = true;
		return true;
	}
	rm_close(c);
	return false;
}

//! vkCmdUpdateBuffer: always noted in the buffer's copy; true when absorbed (the caller records nothing).
static bool rm_update(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkDeviceSize size, const void *data)
{
	if (!rm_active())
		return false;
	pthread_mutex_lock(&rm_lock);
	struct rm_buf *r = rm_buf_find(b);
	if (r && off + size <= r->size) {
		memcpy(r->shadow + off, data, size);
		rm_buf_mark(r, off, size, true);
	}
	pthread_mutex_unlock(&rm_lock);
	struct rm_cb *c = rm_get(cb);
	if (!c || !c->deferred)
		return false;
	unsigned k;
	for (k = 0; k < c->ndirty && c->dirty[k].b != b; k++)
		;
	if (!r || off + size > r->size || k == RM_DIRTY) {
		rm_close(c);
		return false;
	}
	if (k == c->ndirty) {
		c->ndirty++;
		c->dirty[k].b = b;
		c->dirty[k].lo = off;
		c->dirty[k].hi = off + size;
	} else {
		c->dirty[k].lo = off < c->dirty[k].lo ? off : c->dirty[k].lo;
		c->dirty[k].hi = off + size > c->dirty[k].hi ? off + size : c->dirty[k].hi;
	}
	c->ver++;
	return true;
}

//! vkCmdBindDescriptorSets (graphics).
static void rm_bind(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout pl, uint32_t first, uint32_t n,
                    const VkDescriptorSet *sets, uint32_t nd)
{
	if (bp != VK_PIPELINE_BIND_POINT_GRAPHICS)
		return;
	struct rm_cb *c = rm_get(cb);
	if (!c)
		return;
	for (uint32_t i = 0; i < n && first + i < RM_SLOTS; i++)
		c->slot[first + i] = (struct rm_slot){sets[i], sets[i], pl, 0, nd > 0};
}

//! 1: the set reads an updated buffer through uniform-buffer descriptors only; 0: none; -1: cannot tell or redirect.
//! Under rm_lock.
static int rm_set_reads(const struct rm_cb *c, VkDescriptorSet s)
{
	const struct rm_set *e = rm_set_find(s, false);
	if (!e || e->bad)
		return -1;
	int r = 0;
	for (uint32_t i = 0; i < e->nbuf; i++)
		for (unsigned k = 0; k < c->ndirty; k++)
			if (e->buf[i].b == c->dirty[k].b) {
				if (e->buf[i].type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
					return -1;
				r = 1;
			}
	return r;
}

//! Binds a copy of slot i's set whose uniform buffers that were updated are snapshots of their contents now.
static bool rm_redirect(struct rm_cb *c, unsigned i)
{
	RM_NEXT(PFN_vkCmdBindDescriptorSets, CmdBindDescriptorSets)
	RM_NEXT(PFN_vkAllocateDescriptorSets, AllocateDescriptorSets)
	RM_NEXT(PFN_vkUpdateDescriptorSets, UpdateDescriptorSets)
	RM_NEXT(PFN_vkCreateDescriptorPool, CreateDescriptorPool)
	struct rm_slot *s = &c->slot[i];
	if (!c->pool) {
		VkDescriptorPoolSize sizes[11];
		for (unsigned t = 0; t < 11; t++)
			sizes[t] = (VkDescriptorPoolSize){(VkDescriptorType)t, 1024};
		VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 256, 11, sizes};
		if (CreateDescriptorPool(c->d->handle, &pci, NULL, &c->pool) != VK_SUCCESS) {
			c->pool = VK_NULL_HANDLE;
			return false;
		}
	}
	// what to copy and what to point elsewhere, read under the lock
	struct rm_set e;
	struct rm_layout l;
	pthread_mutex_lock(&rm_lock);
	const struct rm_set *pe = rm_set_find(s->orig, false);
	const struct rm_layout *pl = pe ? rm_layout_find(pe->l) : NULL;
	if (pe)
		e = *pe;
	if (pl)
		l = *pl;
	pthread_mutex_unlock(&rm_lock);
	if (!pe || !pl || e.bad)
		return false;
	VkDescriptorSet copy;
	VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, c->pool, 1, &e.l};
	if (AllocateDescriptorSets(c->d->handle, &ai, &copy) != VK_SUCCESS)
		return false;
	VkCopyDescriptorSet cp[32];
	uint32_t ncp = 0;
	for (uint32_t k = 0; k < l.n; k++) {
		if (l.b[k].count == 0)
			continue;
		if ((unsigned)l.b[k].type > VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)
			return false;
		cp[ncp++] = (VkCopyDescriptorSet){VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET, NULL, s->orig, l.b[k].binding, 0, copy,
		                                  l.b[k].binding, 0, l.b[k].count};
	}
	VkWriteDescriptorSet wr[RM_SET_BUFS];
	VkDescriptorBufferInfo bi[RM_SET_BUFS];
	uint32_t nwr = 0;
	for (uint32_t k = 0; k < e.nbuf; k++) {
		bool dirty = false;
		for (unsigned j = 0; j < c->ndirty; j++)
			dirty |= e.buf[k].b == c->dirty[j].b;
		if (!dirty)
			continue;
		VkDeviceSize off = e.buf[k].off, n = e.buf[k].range;
		uint8_t tmp[RM_BUF_MAX];
		bool ok = false;
		pthread_mutex_lock(&rm_lock);
		struct rm_buf *r = rm_buf_find(e.buf[k].b);
		if (r && n == VK_WHOLE_SIZE)
			n = off < r->size ? r->size - off : 0;
		if (r && n && rm_buf_valid(r, off, n)) {
			memcpy(tmp, r->shadow + off, n);
			ok = true;
		}
		pthread_mutex_unlock(&rm_lock);
		VkBuffer sb;
		VkDeviceSize soff;
		void *p;
		if (!ok || !ub_alloc(c->d, c->cb, n, &sb, &soff, &p))
			return false;
		memcpy(p, tmp, n);
		bi[nwr] = (VkDescriptorBufferInfo){sb, soff, n};
		wr[nwr] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, copy, e.buf[k].binding,
		                                 e.buf[k].elem, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &bi[nwr], NULL};
		nwr++;
	}
	UpdateDescriptorSets(c->d->handle, 0, NULL, ncp, cp); // copies first: writes are applied before copies
	UpdateDescriptorSets(c->d->handle, nwr, wr, 0, NULL);
	CmdBindDescriptorSets(c->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pl, i, 1, &copy, 0, NULL);
	s->cur = copy;
	s->ver = c->ver;
	rm_stat_redirects++;
	return true;
}

//! Before a draw: inside a merged pass with absorbed updates, the bound sets that read them are redirected.
static void rm_draw(VkCommandBuffer cb)
{
	struct rm_cb *c = rm_get(cb);
	if (!c || !c->ndirty)
		return;
	for (unsigned i = 0; i < RM_SLOTS; i++) {
		struct rm_slot *s = &c->slot[i];
		if (!s->orig || (s->cur != s->orig && s->ver == c->ver))
			continue;
		pthread_mutex_lock(&rm_lock);
		int reads = rm_set_reads(c, s->orig);
		pthread_mutex_unlock(&rm_lock);
		if (reads == 0)
			continue;
		if (reads < 0 || s->dyn || !rm_redirect(c, i)) {
			rm_split(c); // settles every slot: nothing left to redirect
			return;
		}
	}
}

//! Logged every 5 s with the display statistics.
static void rm_stats(void)
{
	static uint64_t last;
	uint64_t t = now_ns();
	if (!rm_active() || t - last < 5000000000ull)
		return;
	last = t;
	unsigned m = atomic_exchange(&rm_stat_merged, 0), s = atomic_exchange(&rm_stat_splits, 0),
	         r = atomic_exchange(&rm_stat_redirects, 0);
	LOG("render pass merge: %u passes merged, %u splits, %u descriptor set copies in 5 s%s\n", m, s, r,
	    rm_off() ? " (off: nomerge)" : "");
}

// --- the other commands that cannot be recorded inside a render pass: they end the held-back one ----------------

#define RM_FLUSH_HOOK(name, params, args) \
	static VKAPI_ATTR void VKAPI_CALL rm_##name params \
	{ \
		static PFN_vk##name next; \
		if (!next) \
			next = (PFN_vk##name)device_of(cb)->gdpa(device_of(cb)->handle, "vk" #name); \
		rm_flush(cb); \
		next args; \
	}

RM_FLUSH_HOOK(CmdCopyImageToBuffer,
              (VkCommandBuffer cb, VkImage i, VkImageLayout l, VkBuffer b, uint32_t n, const VkBufferImageCopy *r),
              (cb, i, l, b, n, r))
RM_FLUSH_HOOK(CmdClearColorImage,
              (VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearColorValue *v, uint32_t n,
               const VkImageSubresourceRange *r),
              (cb, i, l, v, n, r))
RM_FLUSH_HOOK(CmdClearDepthStencilImage,
              (VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearDepthStencilValue *v, uint32_t n,
               const VkImageSubresourceRange *r),
              (cb, i, l, v, n, r))
RM_FLUSH_HOOK(CmdResolveImage,
              (VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n,
               const VkImageResolve *r),
              (cb, s, sl, d, dl, n, r))
RM_FLUSH_HOOK(CmdDispatchIndirect, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o), (cb, b, o))
RM_FLUSH_HOOK(CmdResetQueryPool, (VkCommandBuffer cb, VkQueryPool p, uint32_t f, uint32_t n), (cb, p, f, n))
RM_FLUSH_HOOK(CmdCopyQueryPoolResults,
              (VkCommandBuffer cb, VkQueryPool p, uint32_t f, uint32_t n, VkBuffer b, VkDeviceSize o, VkDeviceSize s,
               VkQueryResultFlags fl),
              (cb, p, f, n, b, o, s, fl))
RM_FLUSH_HOOK(CmdSetEvent, (VkCommandBuffer cb, VkEvent e, VkPipelineStageFlags m), (cb, e, m))
RM_FLUSH_HOOK(CmdResetEvent, (VkCommandBuffer cb, VkEvent e, VkPipelineStageFlags m), (cb, e, m))
RM_FLUSH_HOOK(CmdWaitEvents,
              (VkCommandBuffer cb, uint32_t n, const VkEvent *e, VkPipelineStageFlags s, VkPipelineStageFlags d,
               uint32_t nm, const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni,
               const VkImageMemoryBarrier *im),
              (cb, n, e, s, d, nm, m, nb, b, ni, im))
RM_FLUSH_HOOK(CmdExecuteCommands, (VkCommandBuffer cb, uint32_t n, const VkCommandBuffer *c), (cb, n, c))

static VKAPI_ATTR void VKAPI_CALL rm_CmdFillBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkDeviceSize s,
                                                  uint32_t v)
{
	static PFN_vkCmdFillBuffer next;
	if (!next)
		next = (PFN_vkCmdFillBuffer)device_of(cb)->gdpa(device_of(cb)->handle, "vkCmdFillBuffer");
	rm_flush(cb);
	rm_buf_unknown(b, o, s);
	next(cb, b, o, s, v);
}

static VKAPI_ATTR void VKAPI_CALL rm_CmdDrawIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n,
                                                    uint32_t st)
{
	static PFN_vkCmdDrawIndirect next;
	if (!next)
		next = (PFN_vkCmdDrawIndirect)device_of(cb)->gdpa(device_of(cb)->handle, "vkCmdDrawIndirect");
	if (qs_skipping(cb))
		return;
	rm_draw(cb);
	next(cb, b, o, n, st);
}

static VKAPI_ATTR void VKAPI_CALL rm_CmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n,
                                                           uint32_t st)
{
	static PFN_vkCmdDrawIndexedIndirect next;
	if (!next)
		next = (PFN_vkCmdDrawIndexedIndirect)device_of(cb)->gdpa(device_of(cb)->handle, "vkCmdDrawIndexedIndirect");
	if (qs_skipping(cb))
		return;
	rm_draw(cb);
	next(cb, b, o, n, st);
}

static VKAPI_ATTR VkResult VKAPI_CALL rm_EndCommandBuffer(VkCommandBuffer cb)
{
	static PFN_vkEndCommandBuffer next;
	if (!next)
		next = (PFN_vkEndCommandBuffer)device_of(cb)->gdpa(device_of(cb)->handle, "vkEndCommandBuffer");
	rm_flush(cb);
	return next(cb);
}

static PFN_vkVoidFunction rm_proc(const char *name)
{
#define R(n) \
	if (strcmp(name, "vk" #n) == 0) \
		return (PFN_vkVoidFunction)rm_##n;
	R(CmdCopyImageToBuffer)
	R(CmdClearColorImage)
	R(CmdClearDepthStencilImage)
	R(CmdResolveImage)
	R(CmdDispatchIndirect)
	R(CmdResetQueryPool)
	R(CmdCopyQueryPoolResults)
	R(CmdSetEvent)
	R(CmdResetEvent)
	R(CmdWaitEvents)
	R(CmdExecuteCommands)
	R(CmdFillBuffer)
	R(CmdDrawIndirect)
	R(CmdDrawIndexedIndirect)
	R(EndCommandBuffer)
#undef R
	return NULL;
}
