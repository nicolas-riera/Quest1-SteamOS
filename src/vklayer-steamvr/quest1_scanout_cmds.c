// quest1_scanout_cmds: the command hooks on the display image (render pass begin/end, viewports, draws, barriers, copies), with the HUD.
// Part of quest1_scanout.c (included by it, in this order: one translation unit with quest1_compat.c).

#include "quest1_hud.c"

static VKAPI_ATTR void VKAPI_CALL qs_CmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi,
                                                       VkSubpassContents contents)
{
	QS_TRACE_NEXT(PFN_vkCmdBeginRenderPass, "vkCmdBeginRenderPass")
	static PFN_vkCmdClearAttachments clear_attachments;
	if (!clear_attachments)
		clear_attachments = (PFN_vkCmdClearAttachments)device_of(cb)->gdpa(device_of(cb)->handle, "vkCmdClearAttachments");
	if (qs_trace_left > 0 && qs_trace_left--)
		LOG("trace: cb %p BeginRenderPass rp %p fb %p area %d,%d %ux%u clears %u\n", (void *)cb, (void *)bi->renderPass,
		    (void *)bi->framebuffer, bi->renderArea.offset.x, bi->renderArea.offset.y, bi->renderArea.extent.width,
		    bi->renderArea.extent.height, bi->clearValueCount);
	const VkRect2D a = bi->renderArea;
	qs_ts_begin_pass(cb, &a, (uint64_t)bi->renderPass);
	bool eye_pass = a.extent.width == QS_PANEL_W && a.extent.height == QS_PANEL_H && a.offset.y == 0 &&
	                (a.offset.x == 0 || a.offset.x == QS_PANEL_W);
	pthread_mutex_lock(&qs_twin_lock);
	const struct qs_twin *t = eye_pass ? qs_twin_find(qs_fbs, qs_nfbs, (uint64_t)bi->framebuffer) : NULL;
	int h = a.offset.x == QS_PANEL_W;
	VkFramebuffer twin = t ? (VkFramebuffer)t->twin[h] : VK_NULL_HANDLE;
	VkFramebuffer stream_fb = t ? (VkFramebuffer)t->stream[h] : VK_NULL_HANDLE;
	VkImage half_img = t ? (VkImage)t->img[h] : VK_NULL_HANDLE;
	pthread_mutex_unlock(&qs_twin_lock);
	VkRenderPassBeginInfo sb;
	if (qs_scene_w && !eye_pass) { // an eye's pass into the scene image: into the eye's gralloc buffer instead
		bool scene_fb = false;
		pthread_mutex_lock(&qs_scene_lock);
		for (unsigned i = 0; i < qs_nsfbs; i++)
			scene_fb |= qs_scene_fbs[i].fb == (uint64_t)bi->framebuffer;
		pthread_mutex_unlock(&qs_scene_lock);
		struct qd_swapchain *sc = NULL;
		int idx = -1;
		pthread_mutex_lock(&qs_cbst_lock);
		typeof(qs_cbst[0]) *e = scene_fb ? qs_cbst_get(cb, true) : NULL;
		bool start = e && !e->in_run;
		pthread_mutex_unlock(&qs_cbst_lock);
		// the frame's swapchain index, known before vrcompositor acquires it: the eyes render into its buffers, and the
		// head-locked overlays into its set (quest1_hud.c), also while streaming
		bool streaming = start && e->runs == 0 && qs_streaming();
		if (start && e->runs == 0)
			idx = qs_reserve(&sc);
		pthread_mutex_lock(&qs_cbst_lock);
		if (start) {
			e->in_run = true;
			e->eye = e->runs == 0 ? qs_first_eye : 1 - qs_first_eye;
			if (e->runs++ == 0) {
				e->idx = idx;
				e->stream = streaming;
			}
			// streaming: into vrcompositor's images, for its distortion pass (which then runs into the frame's buffers)
			e->redirect = e->idx >= 0 && !e->stream;
			e->hud_cleared = false;
		}
		int started_eye = start ? e->eye : -1;
		VkImage target = VK_NULL_HANDLE;
		if (e && e->redirect) {
			pthread_mutex_lock(&qs_twin_lock);
			sc = qs_direct_scs[0];
			if (sc && (uint32_t)e->idx < sc->count)
				target = sc->half_image[e->eye][e->idx];
			pthread_mutex_unlock(&qs_twin_lock);
		}
		pthread_mutex_unlock(&qs_cbst_lock);
		if (started_eye >= 0)
			qh_eye_start(started_eye);
		VkFramebuffer stw = target ? qs_scene_twin(device_of(cb)->handle, bi->framebuffer, target) : VK_NULL_HANDLE;
		if (scene_fb) {
			pthread_mutex_lock(&qs_cbst_lock);
			e = qs_cbst_get(cb, false);
			if (e && e->in_run) {
				e->cur_rp = (uint64_t)bi->renderPass;
				e->cur_fb = (uint64_t)(stw ? stw : bi->framebuffer);
				e->app_fb = (uint64_t)bi->framebuffer;
				e->area = bi->renderArea;
			}
			pthread_mutex_unlock(&qs_cbst_lock);
		}
		if (stw) {
			sb = *bi;
			sb.framebuffer = stw;
			bi = &sb;
		} else if (e && e->redirect) { // (cannot happen once the first pass of the eye had its twin)
			pthread_mutex_lock(&qs_cbst_lock);
			e->redirect = false;
			pthread_mutex_unlock(&qs_cbst_lock);
		}
	}
	if (twin && qs_scene_on) { // a distortion pass: its eye is in its gralloc buffer already, or gets copied there
		pthread_mutex_lock(&qs_cbst_lock);
		typeof(qs_cbst[0]) *e = qs_cbst_get(cb, false);
		bool rendered = e && e->in_run && e->redirect, run = e && e->in_run;
		int eye = e ? e->eye : -1, idx = e ? e->idx : -1;
		unsigned runs = e ? e->runs : 0;
		if (e)
			e->in_run = false;
		pthread_mutex_unlock(&qs_cbst_lock);
		if (rendered) {
			struct qd_swapchain *sc = qs_direct_scs[0];
			bool right = eye == h && sc && (uint32_t)idx < sc->count && sc->half_image[h][idx] == half_img;
			if (!right) {
				static int logged;
				if (logged++ < 4)
					LOG("direct scanout: scene mode: eye %d rendered into %d/%d, distortion pass for %d: learnt\n", eye,
					    eye, idx, h);
				qs_first_eye = runs == 1 ? h : 1 - h;
			}
			rm_flush(cb);
			pthread_mutex_lock(&qs_cbst_lock);
			qs_cbst_get(cb, true)->skipping = true;
			pthread_mutex_unlock(&qs_cbst_lock);
			return; // dropped: Monado's warp does the lens from the eye's render
		}
		if (half_img && stream_fb && qs_streaming()) { // vrlink's frame: the pass runs, stretched over the buffer
			rm_flush(cb);
			pthread_mutex_lock(&qs_cbst_lock);
			e = qs_cbst_get(cb, false);
			if (e)
				e->scene = 0;
			pthread_mutex_unlock(&qs_cbst_lock);
			VkRenderPassBeginInfo s = *bi;
			s.framebuffer = stream_fb;
			s.renderArea = (VkRect2D){{0, 0}, {qs_scene_w, qs_scene_h}};
			rm_begin(cb, &s, contents, true); // (never merged)
			next(cb, &s, contents);
			qs_set_scaled(cb, h ? QS_PANEL_W : 0);
			static int logged;
			if (!logged++)
				LOG("direct scanout: scene mode: Steam Link VR's frames, the distortion pass runs over the %ux%u buffers\n",
				    qs_scene_w, qs_scene_h);
			return;
		}
		if (half_img) { // (also notes the render size when it differs: qs_scene_copy)
			(void)run;
			rm_flush(cb);
			if (qs_scene_copy(cb, half_img))
				return;
		}
	}
	if (rm_begin(cb, bi, contents, eye_pass))
		return; // it continues the open render pass
	if (twin) { // the eye's own buffer (half_image[h]) at x = 0
		VkRenderPassBeginInfo b = *bi;
		b.framebuffer = twin;
		b.renderArea.offset.x = 0;
		next(cb, &b, contents);
		if (h)
			qs_set_shifted(cb, true);
		return;
	}
	// fallback without a twin: run the pass over the whole image
	VkRenderPass load = VK_NULL_HANDLE;
	if (contents == VK_SUBPASS_CONTENTS_INLINE && eye_pass && !getenv("QUEST1_DIRECT_NO_FULL_PASS"))
		load = qs_rp_load_variant(bi->renderPass);
	if (!load) {
		next(cb, bi, contents);
		return;
	}
	VkRenderPassBeginInfo full = *bi;
	full.renderPass = load;
	full.renderArea = (VkRect2D){{0, 0}, {2 * QS_PANEL_W, QS_PANEL_H}};
	next(cb, &full, contents);
	if (bi->clearValueCount > 0) {
		VkClearAttachment ca = {VK_IMAGE_ASPECT_COLOR_BIT, 0, bi->pClearValues[0]};
		VkClearRect cr = {a, 0, 1};
		clear_attachments(cb, 1, &ca, 1, &cr);
	}
}

static VKAPI_ATTR void VKAPI_CALL qs_CmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport *v)
{
	QS_TRACE_NEXT(PFN_vkCmdSetViewport, "vkCmdSetViewport")
	if (qs_trace_left > 0 && qs_trace_left--)
		for (uint32_t i = 0; i < n; i++)
			LOG("trace: cb %p SetViewport %u: %.1f,%.1f %.1fx%.1f\n", (void *)cb, first + i, v[i].x, v[i].y,
			    v[i].width, v[i].height);
	if (n <= 4 && qs_is_shifted(cb)) {
		VkViewport s[4];
		for (uint32_t i = 0; i < n; i++) {
			s[i] = v[i];
			s[i].x -= QS_PANEL_W;
		}
		next(cb, first, n, s);
		return;
	}
	int off = n <= 4 && qs_scene_w ? qs_scaled_off(cb) : -1;
	if (off >= 0) {
		const float sx = (float)qs_scene_w / QS_PANEL_W, sy = (float)qs_scene_h / QS_PANEL_H;
		VkViewport s[4];
		for (uint32_t i = 0; i < n; i++) {
			s[i] = v[i];
			s[i].x = (v[i].x - off) * sx;
			s[i].y = v[i].y * sy;
			s[i].width = v[i].width * sx;
			s[i].height = v[i].height * sy;
		}
		next(cb, first, n, s);
		return;
	}
	next(cb, first, n, v);
}

static VKAPI_ATTR void VKAPI_CALL qs_CmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D *r)
{
	QS_TRACE_NEXT(PFN_vkCmdSetScissor, "vkCmdSetScissor")
	if (qs_trace_left > 0 && qs_trace_left--)
		for (uint32_t i = 0; i < n; i++)
			LOG("trace: cb %p SetScissor %u: %d,%d %ux%u\n", (void *)cb, first + i, r[i].offset.x, r[i].offset.y,
			    r[i].extent.width, r[i].extent.height);
	if (n <= 4 && qs_is_shifted(cb)) {
		VkRect2D s[4];
		for (uint32_t i = 0; i < n; i++) {
			s[i] = r[i];
			s[i].offset.x = s[i].offset.x >= QS_PANEL_W ? s[i].offset.x - QS_PANEL_W : 0;
		}
		next(cb, first, n, s);
		return;
	}
	int off = n <= 4 && qs_scene_w ? qs_scaled_off(cb) : -1;
	if (off >= 0) {
		VkRect2D s[4];
		for (uint32_t i = 0; i < n; i++) {
			int x0 = r[i].offset.x - off, x1 = x0 + (int)r[i].extent.width;
			int y0 = r[i].offset.y, y1 = y0 + (int)r[i].extent.height;
			x0 = x0 < 0 ? 0 : (int)((int64_t)x0 * qs_scene_w / QS_PANEL_W);
			y0 = y0 < 0 ? 0 : (int)((int64_t)y0 * qs_scene_h / QS_PANEL_H);
			x1 = x1 <= 0 ? 0 : (int)(((int64_t)x1 * qs_scene_w + QS_PANEL_W - 1) / QS_PANEL_W);
			y1 = y1 <= 0 ? 0 : (int)(((int64_t)y1 * qs_scene_h + QS_PANEL_H - 1) / QS_PANEL_H);
			s[i] = (VkRect2D){{x0, y0}, {(uint32_t)(x1 > x0 ? x1 - x0 : 0), (uint32_t)(y1 > y0 ? y1 - y0 : 0)}};
		}
		next(cb, first, n, s);
		return;
	}
	next(cb, first, n, r);
}

static VKAPI_ATTR void VKAPI_CALL qs_CmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z)
{
	QS_TRACE_NEXT(PFN_vkCmdDispatch, "vkCmdDispatch")
	rm_flush(cb);
	if (qs_trace_left > 0 && qs_trace_left--)
		LOG("trace: cb %p Dispatch %u %u %u\n", (void *)cb, x, y, z);
	next(cb, x, y, z);
}

static VKAPI_ATTR void VKAPI_CALL qs_CmdDispatchBase(VkCommandBuffer cb, uint32_t bx, uint32_t by, uint32_t bz,
                                                    uint32_t x, uint32_t y, uint32_t z)
{
	QS_TRACE_NEXT(PFN_vkCmdDispatchBase, "vkCmdDispatchBase")
	rm_flush(cb);
	if (qs_trace_left > 0 && qs_trace_left--)
		LOG("trace: cb %p DispatchBase %u %u %u + %u %u %u\n", (void *)cb, bx, by, bz, x, y, z);
	next(cb, bx, by, bz, x, y, z);
}

static VKAPI_ATTR void VKAPI_CALL qs_CmdDraw(VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi)
{
	QS_TRACE_NEXT(PFN_vkCmdDraw, "vkCmdDraw")
	if (qs_skipping(cb))
		return;
	rm_draw(cb);
	if (qs_trace_left > 0 && qs_trace_left--)
		LOG("trace: cb %p Draw %u x%u\n", (void *)cb, vc, ic);
	qs_ts_draw(cb, vc * ic);
	next(cb, vc, ic, fv, fi);
}

static VKAPI_ATTR void VKAPI_CALL qs_CmdDrawIndexed(VkCommandBuffer cb, uint32_t n, uint32_t ic, uint32_t fi, int32_t vo,
                                                   uint32_t fin)
{
	QS_TRACE_NEXT(PFN_vkCmdDrawIndexed, "vkCmdDrawIndexed")
	if (qs_skipping(cb))
		return;
	bool hud = qh_before_draw(cb, n);
	rm_draw(cb);
	if (qs_trace_left > 0 && qs_trace_left--)
		LOG("trace: cb %p DrawIndexed %u x%u first %u\n", (void *)cb, n, ic, fi);
	qs_ts_draw(cb, n * ic);
	next(cb, n, ic, fi, vo, fin);
	if (hud)
		qh_after_draw(cb);
}

static VKAPI_ATTR void VKAPI_CALL qs_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p)
{
	QS_TRACE_NEXT(PFN_vkCmdBindPipeline, "vkCmdBindPipeline")
	if (qs_trace_left > 0 && qs_trace_left--)
		LOG("trace: cb %p BindPipeline %s %p\n", (void *)cb, bp == VK_PIPELINE_BIND_POINT_COMPUTE ? "compute" : "graphics",
		    (void *)p);
	next(cb, bp, p);
}

static VKAPI_ATTR void VKAPI_CALL qs_CmdEndRenderPass(VkCommandBuffer cb)
{
	QS_TRACE_NEXT(PFN_vkCmdEndRenderPass, "vkCmdEndRenderPass")
	if (qs_trace_left > 0 && qs_trace_left--)
		LOG("trace: cb %p EndRenderPass\n", (void *)cb);
	if (qs_skipping(cb)) { // a dropped distortion pass ends
		pthread_mutex_lock(&qs_cbst_lock);
		qs_cbst_get(cb, true)->skipping = false;
		pthread_mutex_unlock(&qs_cbst_lock);
		return;
	}
	if (rm_end(cb)) { // held back: the next pass may continue it
		qs_ts_end_pass(cb);
		return;
	}
	next(cb);
	qs_set_shifted(cb, false);
	qs_set_scaled(cb, -1);
	qs_ts_end_pass(cb);
}

static VKAPI_ATTR void VKAPI_CALL qs_CmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
                                                       VkDependencyFlags dep, uint32_t nm, const VkMemoryBarrier *m,
                                                       uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni,
                                                       const VkImageMemoryBarrier *im)
{
	QS_TRACE_NEXT(PFN_vkCmdPipelineBarrier, "vkCmdPipelineBarrier")
	if (qs_trace_left > 0 && qs_trace_left--) {
		LOG("trace: cb %p PipelineBarrier 0x%x -> 0x%x, %u mem %u buf %u img\n", (void *)cb, src, dst, nm, nb, ni);
		for (uint32_t i = 0; i < ni; i++)
			LOG("trace:   image %p layout %d -> %d access 0x%x -> 0x%x\n", (void *)im[i].image, im[i].oldLayout,
			    im[i].newLayout, im[i].srcAccessMask, im[i].dstAccessMask);
	}
	for (uint32_t i = 0; qs_scene_on && i < ni; i++) {
		if (im[i].oldLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
		    im[i].newLayout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
			continue;
		uint32_t sh = 0, sw;
		pthread_mutex_lock(&qs_scene_lock);
		sw = qs_scene_img_size((uint64_t)im[i].image, &sh);
		pthread_mutex_unlock(&qs_scene_lock);
		if (sw) {
			pthread_mutex_lock(&qs_cbst_lock);
			typeof(qs_cbst[0]) *e = qs_cbst_get(cb, true);
			e->scene = (uint64_t)im[i].image;
			e->w = sw;
			e->h = sh;
			pthread_mutex_unlock(&qs_cbst_lock);
		}
	}
	if (rm_barrier(cb, nm, nb, b, ni))
		return; // the uniform buffers it covers are read through copies
	next(cb, src, dst, dep, nm, m, nb, b, ni, im);
}

static VKAPI_ATTR void VKAPI_CALL qs_CmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer buf, VkImage img, VkImageLayout l,
                                                         uint32_t n, const VkBufferImageCopy *r)
{
	QS_TRACE_NEXT(PFN_vkCmdCopyBufferToImage, "vkCmdCopyBufferToImage")
	rm_flush(cb);
	if (qs_trace_left > 0 && qs_trace_left--)
		LOG("trace: cb %p CopyBufferToImage %p %ux%u\n", (void *)cb, (void *)img, n ? r[0].imageExtent.width : 0,
		    n ? r[0].imageExtent.height : 0);
	next(cb, buf, img, l, n, r);
}

static VKAPI_ATTR void VKAPI_CALL qs_CmdCopyImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d,
                                                 VkImageLayout dl, uint32_t n, const VkImageCopy *r)
{
	QS_TRACE_NEXT(PFN_vkCmdCopyImage, "vkCmdCopyImage")
	rm_flush(cb);
	if (qs_trace_left > 0 && qs_trace_left--)
		LOG("trace: cb %p CopyImage %p -> %p %ux%u\n", (void *)cb, (void *)s, (void *)d, n ? r[0].extent.width : 0,
		    n ? r[0].extent.height : 0);
	next(cb, s, sl, d, dl, n, r);
}

static VKAPI_ATTR void VKAPI_CALL qs_CmdBlitImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d,
                                                 VkImageLayout dl, uint32_t n, const VkImageBlit *r, VkFilter f)
{
	QS_TRACE_NEXT(PFN_vkCmdBlitImage, "vkCmdBlitImage")
	rm_flush(cb);
	if (qs_trace_left > 0 && qs_trace_left--)
		LOG("trace: cb %p BlitImage %p -> %p\n", (void *)cb, (void *)s, (void *)d);
	next(cb, s, sl, d, dl, n, r, f);
}

static VKAPI_ATTR void VKAPI_CALL qs_CmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout pl,
                                                          uint32_t first, uint32_t n, const VkDescriptorSet *sets,
                                                          uint32_t nd, const uint32_t *dyn)
{
	QS_TRACE_NEXT(PFN_vkCmdBindDescriptorSets, "vkCmdBindDescriptorSets")
	if (qs_trace_left > 0 && qs_trace_left--)
		LOG("trace: cb %p BindDescriptorSets %u..%u\n", (void *)cb, first, first + n);
	next(cb, bp, pl, first, n, sets, nd, dyn);
	rm_bind(cb, bp, pl, first, n, sets, nd);
	qsp_bind(cb, bp, pl, first, n, sets, nd); // (the stream's pose, quest1_modifier.c)
}

static VKAPI_ATTR void VKAPI_CALL qs_CmdPushConstants(VkCommandBuffer cb, VkPipelineLayout pl, VkShaderStageFlags st,
                                                     uint32_t off, uint32_t size, const void *v)
{
	QS_TRACE_NEXT(PFN_vkCmdPushConstants, "vkCmdPushConstants")
	if (qs_trace_left > 0 && qs_trace_left--)
		LOG("trace: cb %p PushConstants %u+%u\n", (void *)cb, off, size);
	next(cb, pl, st, off, size, v);
}

static VKAPI_ATTR void VKAPI_CALL qs_CmdClearAttachments(VkCommandBuffer cb, uint32_t n, const VkClearAttachment *a,
                                                        uint32_t nr, const VkClearRect *r)
{
	QS_TRACE_NEXT(PFN_vkCmdClearAttachments, "vkCmdClearAttachments")
	if (!qs_skipping(cb))
		next(cb, n, a, nr, r);
}

static VKAPI_ATTR void VKAPI_CALL qs_CmdNextSubpass(VkCommandBuffer cb, VkSubpassContents c)
{
	QS_TRACE_NEXT(PFN_vkCmdNextSubpass, "vkCmdNextSubpass")
	if (!qs_skipping(cb))
		next(cb, c);
}

static PFN_vkVoidFunction qs_trace_proc(const char *name)
{
	if (strcmp(name, "vkCmdClearAttachments") == 0)
		return (PFN_vkVoidFunction)qs_CmdClearAttachments;
	if (strcmp(name, "vkCmdNextSubpass") == 0)
		return (PFN_vkVoidFunction)qs_CmdNextSubpass;
	PFN_vkVoidFunction f = rm_proc(name);
	if (f)
		return f;
#define T(n) if (strcmp(name, "vk" #n) == 0) return (PFN_vkVoidFunction)qs_##n;
	T(CmdEndRenderPass)
	T(CreateImageView)
	T(DestroyImageView)
	T(CreateFramebuffer)
	T(DestroyFramebuffer)
	T(CmdPipelineBarrier)
	T(CmdCopyBufferToImage)
	T(CmdCopyImage)
	T(CmdBlitImage)
	T(CmdBindDescriptorSets)
	T(CmdPushConstants)
	T(CmdBeginRenderPass)
	T(CmdSetViewport)
	T(CmdSetScissor)
	T(CmdDispatch)
	T(CmdDispatchBase)
	T(CmdDraw)
	T(CmdDrawIndexed)
	T(CmdBindPipeline)
	T(CreateRenderPass)
	T(DestroyRenderPass)
#undef T
	if (strcmp(name, "vkCmdDispatchBaseKHR") == 0)
		return (PFN_vkVoidFunction)qs_CmdDispatchBase;
	return NULL;
}
