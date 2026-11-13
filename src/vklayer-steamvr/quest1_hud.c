// quest1_hud: SteamVR's head-locked overlays drawn apart (included by quest1_scanout.c after quest1_rpmerge.c;
// vrcompositor, scene mode: its own frames and, while Steam Link VR streams, the layer of overlays its distortion pass
// blends over the stream; the same buffers either way, the eye render's size).
//
// vrcompositor draws each overlay of an eye as a quad after the scene, with a 624-byte uniform update before the draw
// whose first two matrices are the overlay's model (overlay to tracking space) and the eye's view. A head-locked
// overlay (SteamVR's HUD: the volume, the IPD) is placed from the frame's own head pose, so it lands at the same place
// of every frame (tools/headset-dev's q1marker "head": 0 px of motion in vrcompositor's output while the head turned).
// Monado's re-projection then turns the whole frame by its prediction error (the render pose against the head pose at
// the display time): the world lands right, but what is fixed to the head moved by that error at every frame, and
// trembled whenever the head moved (user, 2026-11-13: "exactly the same trembling" as the stream).
//
// So those draws go into buffers of their own: a set of one per eye for each swapchain image (the frame's own, so it
// is never drawn into while Monado may still read it: with 3 sets, 1 frame in 7 found none free, as Monado may still
// hold three images while a fourth renders), the eye render's size, made at the first such draw (~80 MB). Monado draws
// them over the re-projected frame with the lens only, and only over the part of the panel they cover (Q1D_FRAME's
// hud and hud_rect).
//
// Which draws: a quad whose transform in the eye's space (view * model) is one of the previous frame's quads' while its
// model is not (the head moved and the overlay followed it). One whose model is the same is world-locked; while the
// head is still both are the same and a quad keeps what it was.
// "nohud" in /tmp/quest1-layer turns it off (read once a second).

#include <math.h>

#define QH_UBO_SIZE 624
#define QH_QUADS 16
#define QH_TOL 1e-4f

struct qh_quad
{
	float model[12], eye[12];
	bool head;
};
static struct
{
	struct qh_quad prev[QH_QUADS], cur[QH_QUADS];
	unsigned nprev, ncur;
} qh_eyes[2];
//! the last overlay-sized uniform update of each command buffer (the next draw's)
static struct
{
	VkCommandBuffer cb;
	float u[48];
	bool valid;
} qh_ubo[16];
static pthread_mutex_t qh_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic unsigned qh_stat_draws, qh_stat_fallbacks;

static bool qh_off(void)
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
		off = strstr(b, "nohud") != NULL;
	}
	return off;
}

//! vkCmdUpdateBuffer (quest1_compat.c): an overlay's uniforms are kept for its draw.
static void qh_note_update(VkCommandBuffer cb, VkDeviceSize size, const void *data)
{
	if (!qs_scene_w || size != QH_UBO_SIZE)
		return;
	pthread_mutex_lock(&qh_lock);
	int k = -1;
	for (int i = 0; i < 16 && k < 0; i++)
		if (qh_ubo[i].cb == cb)
			k = i;
	for (int i = 0; i < 16 && k < 0; i++)
		if (!qh_ubo[i].cb)
			k = i;
	static unsigned next;
	if (k < 0)
		k = (int)(next++ % 16);
	qh_ubo[k].cb = cb;
	memcpy(qh_ubo[k].u, data, sizeof(qh_ubo[k].u));
	qh_ubo[k].valid = true;
	pthread_mutex_unlock(&qh_lock);
}

//! An eye's passes begin: its quads of the frame before become the reference. Runs of the eye that draw no quad
//! (one in two at times) leave the reference as it is.
static void qh_eye_start(int eye)
{
	if (eye < 0 || eye > 1)
		return;
	pthread_mutex_lock(&qh_lock);
	if (qh_eyes[eye].ncur > 0) {
		memcpy(qh_eyes[eye].prev, qh_eyes[eye].cur, sizeof(qh_eyes[eye].cur));
		qh_eyes[eye].nprev = qh_eyes[eye].ncur;
		qh_eyes[eye].ncur = 0;
	}
	pthread_mutex_unlock(&qh_lock);
}

static bool qh_same(const float *a, const float *b)
{
	for (int i = 0; i < 12; i++)
		if (fabsf(a[i] - b[i]) > QH_TOL)
			return false;
	return true;
}

//! Whether the quad about to be drawn by cb in `eye` is head-locked (consumes the noted uniforms). If so, `rect` gets
//! the part of the eye's buffer it covers (u0, v0, u1, v1; the quad's corners at +-0.5 of its model, as SteamVR's
//! overlays are drawn), for Monado to blend only there.
static bool qh_classify(VkCommandBuffer cb, int eye, float rect[4])
{
	float u[48];
	bool have = false;
	pthread_mutex_lock(&qh_lock);
	for (unsigned i = 0; i < 16 && !have; i++)
		if (qh_ubo[i].cb == cb && qh_ubo[i].valid) {
			memcpy(u, qh_ubo[i].u, sizeof(u));
			qh_ubo[i].valid = false;
			have = true;
		}
	pthread_mutex_unlock(&qh_lock);
	// row-major 4x4 matrices: model and view, both affine (to float precision), then the projection
	bool affine = have;
	for (int i = 0; i < 4 && affine; i++)
		affine = fabsf(u[12 + i] - (i == 3)) < 1e-4f && fabsf(u[28 + i] - (i == 3)) < 1e-4f;
	if (!affine)
		return false;
	struct qh_quad q;
	memcpy(q.model, u, sizeof(q.model));
	const float *v = u + 16;
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 4; c++)
			q.eye[r * 4 + c] = v[r * 4] * u[c] + v[r * 4 + 1] * u[4 + c] + v[r * 4 + 2] * u[8 + c] + (c == 3 ? v[r * 4 + 3] : 0);
	pthread_mutex_lock(&qh_lock);
	const struct qh_quad *pe = NULL, *pm = NULL;
	for (unsigned i = 0; i < qh_eyes[eye].nprev; i++) {
		if (!pe && qh_same(qh_eyes[eye].prev[i].eye, q.eye))
			pe = &qh_eyes[eye].prev[i];
		if (!pm && qh_same(qh_eyes[eye].prev[i].model, q.model))
			pm = &qh_eyes[eye].prev[i];
	}
	q.head = pe && !pm ? true : pe && pm ? pe->head : false;
	if (qh_eyes[eye].ncur < QH_QUADS)
		qh_eyes[eye].cur[qh_eyes[eye].ncur++] = q;
	pthread_mutex_unlock(&qh_lock);
	if (!q.head)
		return false;
	// the corners in the eye's buffer: projection * eye; the viewport is flipped (row 0 at the top of the view)
	const float *P = u + 32;
	rect[0] = rect[1] = 1e9f;
	rect[2] = rect[3] = -1e9f;
	for (int k = 0; k < 4; k++) {
		float c[4] = {k & 1 ? 0.5f : -0.5f, k & 2 ? 0.5f : -0.5f, 0, 1}, e[4], clip[4];
		for (int r = 0; r < 3; r++)
			e[r] = q.eye[r * 4] * c[0] + q.eye[r * 4 + 1] * c[1] + q.eye[r * 4 + 3];
		e[3] = 1;
		for (int r = 0; r < 4; r++)
			clip[r] = P[r * 4] * e[0] + P[r * 4 + 1] * e[1] + P[r * 4 + 2] * e[2] + P[r * 4 + 3] * e[3];
		if (clip[3] < 1e-3f) { // a corner behind the eye: the whole buffer
			rect[0] = rect[1] = 0;
			rect[2] = rect[3] = 1;
			return true;
		}
		float x = 0.5f + 0.5f * clip[0] / clip[3], y = 0.5f - 0.5f * clip[1] / clip[3];
		rect[0] = fminf(rect[0], x);
		rect[1] = fminf(rect[1], y);
		rect[2] = fmaxf(rect[2], x);
		rect[3] = fmaxf(rect[3], y);
	}
	return true;
}

//! The buffers, made once (any thread: Vulkan objects of the device). False if they cannot be.
static bool qh_make(struct qd_swapchain *sc)
{
	if (sc->hud_made)
		return true;
	if (sc->hud_failed)
		return false;
	sc->hud_failed = true;
	VkFormat formats[2] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB};
	VkImageFormatListCreateInfo fl = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, NULL, 2, formats};
	VkExternalMemoryImageCreateInfo emi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &fl};
	emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
	VkImageCreateInfo ici = sc->ici; // as the eyes' buffers (qs_create_images)
	ici.pNext = &emi;
	ici.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
	ici.format = VK_FORMAT_R8G8B8A8_UNORM;
	ici.extent.width = qs_scene_w;
	ici.extent.height = qs_scene_h;
	ici.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	for (int s = 0; s < Q1D_HUD_SETS && s < (int)sc->count; s++)
		for (int h = 0; h < 2; h++) {
			sc->hud_fd[s][h] = -1;
			VkResult r = qs_import(sc, &ici, &sc->hud_ahb[s][h], &sc->hud_image[s][h], &sc->hud_memory[s][h],
			                       &sc->hud_fd[s][h], &sc->hud_stride_px[s][h], &sc->hud_ubwc[s][h]);
			if (r != VK_SUCCESS) {
				LOG("head-locked overlays: buffer %d/%d: %d: they stay in the frame\n", s, h, r);
				return false;
			}
		}
	pthread_mutex_lock(&sc->mutex);
	sc->hud_made = true;
	sc->hud_failed = false;
	pthread_mutex_unlock(&sc->mutex);
	LOG("head-locked overlays: drawn apart, %u x 2 buffers %ux%u (%s)\n",
	    sc->count < Q1D_HUD_SETS ? sc->count : (uint32_t)Q1D_HUD_SETS, qs_scene_w, qs_scene_h,
	    sc->hud_ubwc[0][0] ? "UBWC" : "linear");
	return true;
}

//! Hands the buffers to Monado on socket fd. Call with sc->mutex held.
static bool qh_send_locked(struct qd_swapchain *sc, int fd)
{
	if (!sc->hud_made || sc->hud_sent_sock == fd + 1)
		return true;
	for (int s = 0; s < Q1D_HUD_SETS && s < (int)sc->count; s++)
		for (int h = 0; h < 2; h++) {
			struct q1d_msg m;
			memset(&m, 0, sizeof(m));
			m.type = Q1D_BUFFER;
			m.index = (uint32_t)s;
			m.half = 2 + (uint32_t)h;
			m.count = sc->count;
			m.width = qs_scene_w;
			m.height = qs_scene_h;
			m.stride_px = sc->hud_stride_px[s][h];
			m.ubwc = sc->hud_ubwc[s][h];
			if (send(fd, &m, sizeof(m), MSG_NOSIGNAL) != (ssize_t)sizeof(m) || qs_ahb.send_handle(sc->hud_ahb[s][h], fd) != 0)
				return false;
		}
	sc->hud_sent_sock = fd + 1;
	return true;
}

//! The load variant of render pass rp (the one rpmerge continues with, else the clear-less one), or rp.
static VkRenderPass qh_load_rp(VkRenderPass rp)
{
	pthread_mutex_lock(&rm_lock);
	const struct rm_rp *r = rm_rp_find(rp);
	VkRenderPass load = r ? r->load : VK_NULL_HANDLE;
	pthread_mutex_unlock(&rm_lock);
	if (!load)
		load = qs_rp_load_variant(rp);
	return load ? load : rp;
}

//! Before a draw (indexed, n indices): when it is a head-locked quad of an eye rendered into Monado's buffers, the
//! eye's render pass is ended and one into the eye's buffer of head-locked overlays begun (qh_after_draw goes back).
static bool qh_before_draw(VkCommandBuffer cb, uint32_t n)
{
	if (!qs_scene_w || n != 6 || qh_off())
		return false;
	pthread_mutex_lock(&qs_cbst_lock);
	typeof(qs_cbst[0]) *e = qs_cbst_get(cb, false);
	bool run = e && e->in_run && e->idx >= 0 && e->cur_fb && e->app_fb;
	int eye = run ? e->eye : -1, idx = run ? e->idx : -1;
	VkRenderPass rp = run ? (VkRenderPass)e->cur_rp : VK_NULL_HANDLE;
	VkFramebuffer app_fb = run ? (VkFramebuffer)e->app_fb : VK_NULL_HANDLE;
	VkRect2D area = run ? e->area : (VkRect2D){{0, 0}, {0, 0}};
	bool cleared = run && e->hud_cleared;
	pthread_mutex_unlock(&qs_cbst_lock);
	float rect[4];
	if (!run || eye < 0 || eye > 1 || !qh_classify(cb, eye, rect))
		return false;
	pthread_mutex_lock(&qs_twin_lock);
	struct qd_swapchain *sc = qs_direct_scs[0];
	pthread_mutex_unlock(&qs_twin_lock);
	if (!sc || (uint32_t)idx >= sc->count || sc->mon_sock < 0 || !qh_make(sc)) {
		qh_stat_fallbacks++;
		return false;
	}
	// the image's own set: it lives as long as the frame (Monado reads it until it releases the image)
	int set = idx < Q1D_HUD_SETS ? idx : -1;
	VkImage img = set >= 0 ? sc->hud_image[set][eye] : VK_NULL_HANDLE;
	VkFramebuffer fb = img ? qs_scene_twin(device_of(cb)->handle, app_fb, img) : VK_NULL_HANDLE;
	if (!fb) {
		qh_stat_fallbacks++;
		return false;
	}
	pthread_mutex_lock(&sc->mutex);
	float *r = sc->hud_rect[idx][eye];
	if (!(sc->hud_of[idx] & (1u << (8 + eye)))) {
		memcpy(r, rect, sizeof(rect[0]) * 4);
	} else { // the union with the eye's other head-locked quads
		r[0] = fminf(r[0], rect[0]);
		r[1] = fminf(r[1], rect[1]);
		r[2] = fmaxf(r[2], rect[2]);
		r[3] = fmaxf(r[3], rect[3]);
	}
	sc->hud_of[idx] = ((uint32_t)set + 1) | (sc->hud_of[idx] & 0x300) | (1u << (8 + eye));
	pthread_mutex_unlock(&sc->mutex);

	struct device *d = device_of(cb);
	static PFN_vkCmdEndRenderPass end;
	static PFN_vkCmdBeginRenderPass begin;
	static PFN_vkCmdClearAttachments clear;
	if (!end) {
		end = (PFN_vkCmdEndRenderPass)d->gdpa(d->handle, "vkCmdEndRenderPass");
		begin = (PFN_vkCmdBeginRenderPass)d->gdpa(d->handle, "vkCmdBeginRenderPass");
		clear = (PFN_vkCmdClearAttachments)d->gdpa(d->handle, "vkCmdClearAttachments");
	}
	end(cb);
	struct rm_cb *c = rm_get(cb);
	if (c && c->cand)
		rm_settle(c); // the uniform updates it absorbed go to their buffers: the draw reads them there
	VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, qh_load_rp(rp), fb, area, 0, NULL};
	begin(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
	if (!cleared) {
		VkClearAttachment ca = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {.float32 = {0, 0, 0, 0}}}};
		VkClearRect cr = {area, 0, 1};
		clear(cb, 1, &ca, 1, &cr);
		pthread_mutex_lock(&qs_cbst_lock);
		e = qs_cbst_get(cb, false);
		if (e)
			e->hud_cleared = true;
		pthread_mutex_unlock(&qs_cbst_lock);
	}
	qh_stat_draws++;
	return true;
}

//! After a draw qh_before_draw redirected: back into the eye's own render pass.
static void qh_after_draw(VkCommandBuffer cb)
{
	struct device *d = device_of(cb);
	static PFN_vkCmdEndRenderPass end;
	static PFN_vkCmdBeginRenderPass begin;
	if (!end) {
		end = (PFN_vkCmdEndRenderPass)d->gdpa(d->handle, "vkCmdEndRenderPass");
		begin = (PFN_vkCmdBeginRenderPass)d->gdpa(d->handle, "vkCmdBeginRenderPass");
	}
	pthread_mutex_lock(&qs_cbst_lock);
	typeof(qs_cbst[0]) *e = qs_cbst_get(cb, false);
	VkRenderPass rp = e ? (VkRenderPass)e->cur_rp : VK_NULL_HANDLE;
	VkFramebuffer fb = e ? (VkFramebuffer)e->cur_fb : VK_NULL_HANDLE;
	VkRect2D area = e ? e->area : (VkRect2D){{0, 0}, {0, 0}};
	pthread_mutex_unlock(&qs_cbst_lock);
	end(cb);
	VkRenderPass load = qh_load_rp(rp);
	VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, load, fb, area, 0, NULL};
	begin(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
	struct rm_cb *c = rm_get(cb);
	if (c && c->cand)
		c->rp = load; // as after a split (rm_split): the passes after it continue this one
}

//! Logged every 5 s with the display statistics.
static void qh_stats(void)
{
	static uint64_t last;
	uint64_t t = now_ns();
	if (t - last < 5000000000ull)
		return;
	last = t;
	unsigned n = atomic_exchange(&qh_stat_draws, 0), f = atomic_exchange(&qh_stat_fallbacks, 0);
	if (n || f)
		LOG("head-locked overlays: %u draws apart in 5 s, %u left in the frame (no buffer)\n", n, f);
}
