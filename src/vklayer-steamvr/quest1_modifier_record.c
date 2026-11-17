// quest1_modifier_record: each stream frame's copy and inset pass, recorded once (black frames from the PC held back).
// Part of quest1_modifier.c (included by it, in this order: one translation unit).

//! The share of NV12 image n's luma at black (512 samples spread over the frame), -1 if it is not mapped. Each
//! sample's cache line is cleaned and invalidated first: the decoder wrote the frame behind the CPU's cache.
static float qnv_dark(int n)
{
	pthread_mutex_lock(&qdb_lock);
	const uint8_t *y = qnv[n].map;
	uint32_t w = qnv[n].w, h = qnv[n].h;
	size_t len = qnv[n].map_len;
	pthread_mutex_unlock(&qdb_lock);
	size_t pitch = (w + 127u) & ~(size_t)127u; // venus's rows
	if (!y || !w || !h || pitch * h > len)
		return -1;
	enum { ROWS = 32, COLS = 16 };
	const volatile uint8_t *at[ROWS * COLS];
	for (unsigned r = 0; r < ROWS; r++)
		for (unsigned c = 0; c < COLS; c++)
			at[r * COLS + c] = y + (size_t)((2 * r + 1) * h / (2 * ROWS)) * pitch + (2 * c + 1) * w / (2 * COLS);
#if defined(__aarch64__)
	for (unsigned i = 0; i < ROWS * COLS; i++)
		__asm__ volatile("dc civac, %0" ::"r"(at[i]) : "memory");
	__asm__ volatile("dsb ish" ::: "memory");
#endif
	unsigned dark = 0;
	for (unsigned i = 0; i < ROWS * COLS; i++)
		dark += *at[i] <= 28; // video range: black is 16
	return (float)dark / (ROWS * COLS);
}

//! Whether the frame of NV12 image nv12 is one of the PC's black ones, to be left out (above).
static bool qcp_hold(VkImage nv12)
{
	pthread_mutex_lock(&qhold.lock);
	if (nv12 == qhold.last) {
		bool held = qhold.last_held;
		pthread_mutex_unlock(&qhold.lock);
		return held;
	}
	int n = qnv_find(nv12);
	float dark = n >= 0 ? qnv_dark(n) : -1;
	uint64_t now = now_ns();
	bool held = false;
	if (dark >= 0.8f) {
		held = qhold.run < QCP_HOLD_MAX && qhold.picture && now - qhold.picture < 500000000ull;
		qhold.run++;
		qhold.held += held;
	} else {
		qhold.run = 0;
		if (dark >= 0 && dark < 0.5f)
			qhold.picture = now;
	}
	qhold.last = nv12;
	qhold.last_held = held;
	if (qhold.held != qhold.logged && now - qhold.log_ns > 10000000000ull) {
		LOG("stream: %u black frames from the PC left out so far (the last picture stays)\n", qhold.held);
		qhold.logged = qhold.held;
		qhold.log_ns = now;
	}
	pthread_mutex_unlock(&qhold.lock);
	return held;
}

//! Record the copy of NV12 image k's frame into its RGBA image (outside a render pass).
static void qcp_record(VkCommandBuffer cb, int k)
{
	struct device *d = device_of(cb);
	static PFN_vkCmdBeginRenderPass begin;
	static PFN_vkCmdEndRenderPass end;
	static PFN_vkCmdBindPipeline bind;
	static PFN_vkCmdBindDescriptorSets sets;
	static PFN_vkCmdSetViewport viewport;
	static PFN_vkCmdSetScissor scissor;
	static PFN_vkCmdDraw draw;
	static PFN_vkCmdBlitImage blit;
	static PFN_vkCmdPipelineBarrier barrier;
	static PFN_vkCmdPushConstants push;
	if (!draw) {
		VkDevice dev = d->handle;
		begin = (PFN_vkCmdBeginRenderPass)d->gdpa(dev, "vkCmdBeginRenderPass");
		end = (PFN_vkCmdEndRenderPass)d->gdpa(dev, "vkCmdEndRenderPass");
		bind = (PFN_vkCmdBindPipeline)d->gdpa(dev, "vkCmdBindPipeline");
		sets = (PFN_vkCmdBindDescriptorSets)d->gdpa(dev, "vkCmdBindDescriptorSets");
		viewport = (PFN_vkCmdSetViewport)d->gdpa(dev, "vkCmdSetViewport");
		scissor = (PFN_vkCmdSetScissor)d->gdpa(dev, "vkCmdSetScissor");
		blit = (PFN_vkCmdBlitImage)d->gdpa(dev, "vkCmdBlitImage");
		barrier = (PFN_vkCmdPipelineBarrier)d->gdpa(dev, "vkCmdPipelineBarrier");
		push = (PFN_vkCmdPushConstants)d->gdpa(dev, "vkCmdPushConstants");
		draw = (PFN_vkCmdDraw)d->gdpa(dev, "vkCmdDraw");
	}
	pthread_mutex_lock(&qcs.lock);
	struct qnv_copy c = qcp[k];
	pthread_mutex_unlock(&qcs.lock);
	if (!c.fb)
		return;
	if (qcp_hold(c.nv12)) {
		atomic_store(&qs_stream_ns, now_ns()); // still streaming
		return;
	}
	// Each frame is copied once, when vrcompositor takes it first. vrcompositor takes the frame it shows again at each
	// of its own frames until vrlink gives it a newer one, but by then vrlink may have given the buffer back to the
	// decoder, which has only 9: a copy made again later read the newer picture the decoder had written there since,
	// under the old frame's pose, ~65 ms of head motion off in 20 % of the frames (2026-11-13: the whole stream and the
	// HUD jumped whenever the head moved). The output keeps the first copy.
	static _Atomic uint64_t copied;
	if ((uint64_t)c.nv12 == atomic_load(&copied)) {
		atomic_store(&qs_stream_ns, now_ns());
		return;
	}
	atomic_store(&copied, (uint64_t)c.nv12);
	VkRect2D area = {{0, 0}, {c.w, c.h}};
	VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, qcs.rp, c.fb, area, 0, NULL};
	begin(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
	bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, qcs.pipe);
	VkViewport v = {0, 0, (float)c.w, (float)c.h, 0, 1};
	viewport(cb, 0, 1, &v);
	scissor(cb, 0, 1, &area);
	sets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, qcs.pl, 0, 1, &c.set, 0, NULL);
	draw(cb, 3, 1, 0, 0);
	end(cb);
	// The insets' places (vrlink, quest1_views.h) and the level each is read at in the output (its texels per output
	// pixel), before the levels are made: only what the inset pass reads is made.
	struct q1_views views;
	bool insets = qcp_views(&views);
	float lod[2] = {0, 0}, iv[2][4];
	for (int e = 0; insets && e < 2; e++) {
		const float *w = views.wide[e], *r = views.inset[e];
		// the inset is the view above its wide view, as large
		iv[e][0] = w[0], iv[e][1] = w[1] - (w[3] - w[1]), iv[e][2] = w[2], iv[e][3] = w[1];
		float sx = (iv[e][2] - iv[e][0]) * c.w / ((r[2] - r[0]) * c.ow),
		      sy = (iv[e][3] - iv[e][1]) * c.h / ((r[3] - r[1]) * c.oh);
		lod[e] = qcp_log2(sx > sy ? sx : sy);
	}
	// The smaller levels: Steam Link's foveated insets (a narrow field at 1024x1024) come out ~1.8 times smaller in
	// the output; read at one level, their detail would alias. Only the insets are read at those levels (the wide
	// views at the copy's own), and a level only if the inset's level reaches it (textureLod reads the two around
	// it): over the inset rows only and without the unread level, 0.57 ms of GPU a frame instead of 1.47 (median,
	// src/vklayer-steamvr/streambench.c, 2026-11-16).
	uint32_t levels = 1;
	for (int e = 0; insets && e < 2; e++) {
		uint32_t need = (uint32_t)lod[e] + 2; // levels floor(lod) and floor(lod) + 1
		if (need > levels)
			levels = need;
	}
	if (levels > QCP_LEVELS)
		levels = QCP_LEVELS;
	VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	                          VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
	                          VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, c.img,
	                          {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	if (levels > 1) {
		VkImageMemoryBarrier bl[2] = {b, b};
		bl[1].srcAccessMask = 0;
		bl[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		bl[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		bl[1].subresourceRange.baseMipLevel = 1;
		bl[1].subresourceRange.levelCount = QCP_LEVELS - 1;
		barrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
		        2, bl);
	}
	// each inset's rows, with a margin (the filter's taps stay inside: the blend is gone 1 % from the inset's edge),
	// on a multiple of the smallest level's rows
	const int32_t align = 1 << (QCP_LEVELS - 1);
	int32_t band[2][2];
	for (int e = 0; e < 2; e++) {
		int32_t y0 = insets ? (int32_t)(iv[e][1] * c.h) - 2 * align : 0, y1 = insets ? (int32_t)(iv[e][3] * c.h) + 2 * align : 0;
		y0 = y0 < 0 ? 0 : y0 / align * align;
		y1 = y1 > (int32_t)c.h ? (int32_t)c.h : (y1 + align - 1) / align * align;
		band[e][0] = y0, band[e][1] = y1;
	}
	for (uint32_t l = 1; l < levels; l++) {
		int32_t w = (int32_t)(c.w >> (l - 1));
		VkImageBlit r[2];
		uint32_t n = 0;
		for (int e = 0; e < 2; e++) {
			int32_t y0 = band[e][0] >> (l - 1), y1 = band[e][1] >> (l - 1);
			if (y1 <= y0)
				continue;
			r[n++] = (VkImageBlit){{VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 0, 1}, {{0, y0, 0}, {w, y1, 1}},
			                       {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1}, {{0, y0 / 2, 0}, {w / 2, y1 / 2, 1}}};
		}
		if (n)
			blit(cb, c.img, VK_IMAGE_LAYOUT_GENERAL, c.img, VK_IMAGE_LAYOUT_GENERAL, n, r, VK_FILTER_LINEAR);
		b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		b.subresourceRange.baseMipLevel = l;
		if (l + 1 < levels)
			barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
	}
	// before vrcompositor reads it: level 0 by the copy pass, the others by the blits
	b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
	b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
	b.subresourceRange.baseMipLevel = 0;
	b.subresourceRange.levelCount = QCP_LEVELS;
	barrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
	        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
	// What vrcompositor reads: each eye's half of the atlas, its inset blended into its wide view (nv12_inset.frag)
	VkRect2D out = {{0, 0}, {c.ow, c.oh}};
	VkRenderPassBeginInfo bo = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, qcs.rp_out, c.out_fb, out, 0, NULL};
	begin(cb, &bo, VK_SUBPASS_CONTENTS_INLINE);
	bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, qcs.pipe2);
	VkViewport vo = {0, 0, (float)c.ow, (float)c.oh, 0, 1};
	viewport(cb, 0, 1, &vo);
	sets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, qcs.pl2, 0, 1, &c.out_set, 0, NULL);
	for (int e = 0; e < 2; e++) {
		struct qcp_inset pc = {{0}};
		VkRect2D half = {{0, (int32_t)(e * c.oh / 2)}, {c.ow, c.oh / 2}};
		if (insets) {
			const float *w = views.wide[e];
			memcpy(pc.place, views.inset[e], sizeof(pc.place));
			memcpy(pc.view, iv[e], sizeof(pc.view));
			pc.lod = lod[e];
			// vrcompositor reads the wide view only (and a row around it, filtering)
			int32_t y0 = (int32_t)(w[1] * c.oh) - 2, y1 = (int32_t)(w[3] * c.oh + 0.5f) + 2;
			half.offset.y = y0 < 0 ? 0 : y0;
			half.extent.height = (uint32_t)((y1 > (int32_t)c.oh ? (int32_t)c.oh : y1) - half.offset.y);
		}
		scissor(cb, 0, 1, &half);
		push(cb, qcs.pl2, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
		draw(cb, 3, 1, 0, 0);
	}
	end(cb);
	atomic_store(&qs_stream_ns, now_ns()); // (quest1_scanout.c: vrcompositor's distortion pass shows it)
}
