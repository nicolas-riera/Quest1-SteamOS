// quest1_scanout_twins: the twin views, framebuffers and per-command-buffer state of the display image's two halves.
// Part of quest1_scanout.c (included by it, in this order: one translation unit with quest1_compat.c).

// --- each half in a buffer of its own -------------------------------------------------------------
// vrcompositor draws each eye in its own render pass over its half of the display image. On the
// Adreno 540 blob, a render area at x = 1440 of a linear gralloc image comes out as copies of its
// first 320-px bin column, and rendering into the 2880-wide linear image is slow for either half.
// So each eye's pass renders into a 1440-wide image of its own at x = 0 instead (twin image views
// and framebuffers; the right half's viewports and scissors move left), one per display pipe. The
// app's own 2880-wide images only serve as handles.

struct qs_twin
{
	uint64_t handle;    //!< app's image view / framebuffer
	uint64_t twin[2];   //!< ours on half_image[0] / half_image[1]
	uint64_t img[2];    //!< half_image[0] / [1]
	uint64_t stream[2]; //!< framebuffers: the whole buffer of scene mode (qs_streaming)
};
static struct qs_twin qs_views[64], qs_fbs[64];
static unsigned qs_nviews, qs_nfbs;
static struct qd_swapchain *qs_direct_scs[4];
static pthread_mutex_t qs_twin_lock = PTHREAD_MUTEX_INITIALIZER;

static void qs_twins_register(struct qd_swapchain *sc)
{
	pthread_mutex_lock(&qs_twin_lock);
	for (unsigned i = 0; i < 4; i++)
		if (!qs_direct_scs[i]) {
			qs_direct_scs[i] = sc;
			break;
		}
	pthread_mutex_unlock(&qs_twin_lock);
}

static void qs_twins_forget(struct qd_swapchain *sc)
{
	pthread_mutex_lock(&qs_twin_lock);
	for (unsigned i = 0; i < 4; i++)
		if (qs_direct_scs[i] == sc)
			qs_direct_scs[i] = NULL;
	pthread_mutex_unlock(&qs_twin_lock);
}

//! Call with qs_twin_lock held: the half images of the direct swapchain image `image`, if it is one.
static bool qs_halves_of(VkImage image, VkImage halves[2])
{
	for (unsigned s = 0; s < 4; s++)
		for (uint32_t i = 0; qs_direct_scs[s] && i < qs_direct_scs[s]->count; i++)
			if (qs_direct_scs[s]->images[i] == image) {
				halves[0] = qs_direct_scs[s]->half_image[0][i];
				halves[1] = qs_direct_scs[s]->half_image[1][i];
				return true;
			}
	return false;
}

//! Call with qs_twin_lock held.
static const struct qs_twin *qs_twin_find(const struct qs_twin *t, unsigned n, uint64_t handle)
{
	for (unsigned i = 0; i < n; i++)
		if (t[i].handle == handle)
			return &t[i];
	return NULL;
}

static VKAPI_ATTR VkResult VKAPI_CALL qs_CreateImageView(VkDevice device, const VkImageViewCreateInfo *ci,
                                                        const VkAllocationCallbacks *alloc, VkImageView *out)
{
	struct device *d = device_of(device);
	PFN_vkCreateImageView next = (PFN_vkCreateImageView)d->gdpa(device, "vkCreateImageView");
	PFN_vkDestroyImageView destroy = (PFN_vkDestroyImageView)d->gdpa(device, "vkDestroyImageView");
	VkResult r = next(device, ci, alloc, out);
	if (r != VK_SUCCESS)
		return r;
	pthread_mutex_lock(&qs_twin_lock);
	VkImage halves[2];
	if (qs_halves_of(ci->image, halves) && qs_nviews < sizeof(qs_views) / sizeof(qs_views[0])) {
		struct qs_twin t = {(uint64_t)*out, {0, 0}, {(uint64_t)halves[0], (uint64_t)halves[1]}};
		for (int h = 0; h < 2; h++) {
			VkImageViewCreateInfo c = *ci;
			c.image = halves[h];
			VkImageView v = VK_NULL_HANDLE;
			if (next(device, &c, alloc, &v) == VK_SUCCESS)
				t.twin[h] = (uint64_t)v;
		}
		if (t.twin[0] && t.twin[1])
			qs_views[qs_nviews++] = t;
		else
			for (int h = 0; h < 2; h++)
				if (t.twin[h])
					destroy(device, (VkImageView)t.twin[h], alloc);
	}
	pthread_mutex_unlock(&qs_twin_lock);
	uint32_t sh = 0;
	pthread_mutex_lock(&qs_scene_lock);
	if (qs_scene_w && qs_scene_img_size((uint64_t)ci->image, &sh))
		qs_scene_views[qs_nsviews++ % 32] = (typeof(qs_scene_views[0])){(uint64_t)*out, *ci};
	pthread_mutex_unlock(&qs_scene_lock);
	return r;
}

static VKAPI_ATTR void VKAPI_CALL qs_DestroyImageView(VkDevice device, VkImageView view, const VkAllocationCallbacks *alloc)
{
	struct device *d = device_of(device);
	PFN_vkDestroyImageView next = (PFN_vkDestroyImageView)d->gdpa(device, "vkDestroyImageView");
	pthread_mutex_lock(&qs_twin_lock);
	for (unsigned i = 0; i < qs_nviews; i++)
		if (qs_views[i].handle == (uint64_t)view) {
			for (int h = 0; h < 2; h++)
				next(device, (VkImageView)qs_views[i].twin[h], alloc);
			qs_views[i] = qs_views[--qs_nviews];
			break;
		}
	pthread_mutex_unlock(&qs_twin_lock);
	next(device, view, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL qs_CreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo *ci,
                                                          const VkAllocationCallbacks *alloc, VkFramebuffer *out)
{
	struct device *d = device_of(device);
	PFN_vkCreateFramebuffer next = (PFN_vkCreateFramebuffer)d->gdpa(device, "vkCreateFramebuffer");
	PFN_vkDestroyFramebuffer destroy = (PFN_vkDestroyFramebuffer)d->gdpa(device, "vkDestroyFramebuffer");
	VkResult r = next(device, ci, alloc, out);
	if (r == VK_SUCCESS && qs_scene_w && ci->width == qs_scene_w && ci->height == qs_scene_h &&
	    ci->attachmentCount <= QS_SFB_ATT && !ci->pNext) { // on vrcompositor's scene image: kept for its twins
		pthread_mutex_lock(&qs_scene_lock);
		for (uint32_t a = 0; a < ci->attachmentCount; a++)
			for (unsigned k = 0; k < 32 && k < qs_nsviews; k++)
				if (qs_scene_views[k].view == (uint64_t)ci->pAttachments[a] && qs_nsfbs < 16) {
					typeof(qs_scene_fbs[0]) *e = &qs_scene_fbs[qs_nsfbs++];
					e->fb = (uint64_t)*out;
					e->ci = *ci;
					memcpy(e->att, ci->pAttachments, ci->attachmentCount * sizeof(VkImageView));
					e->ci.pAttachments = NULL;
					e->color = a;
					e->vci = qs_scene_views[k].ci;
					a = ci->attachmentCount;
					break;
				}
		pthread_mutex_unlock(&qs_scene_lock);
	}
	if (r != VK_SUCCESS || ci->attachmentCount != 1 || (ci->width != 2 * QS_PANEL_W && ci->width != QS_PANEL_W))
		return r;
	pthread_mutex_lock(&qs_twin_lock);
	const struct qs_twin *v = qs_twin_find(qs_views, qs_nviews, (uint64_t)ci->pAttachments[0]);
	if (v && qs_nfbs < sizeof(qs_fbs) / sizeof(qs_fbs[0])) {
		struct qs_twin t = {(uint64_t)*out, {0, 0}, {v->img[0], v->img[1]}, {0, 0}};
		for (int h = 0; h < 2; h++) {
			VkFramebufferCreateInfo c = *ci;
			VkImageView a = (VkImageView)v->twin[h];
			c.pAttachments = &a;
			c.width = QS_PANEL_W;
			VkFramebuffer fb = VK_NULL_HANDLE;
			if (next(device, &c, alloc, &fb) == VK_SUCCESS)
				t.twin[h] = (uint64_t)fb;
			if (qs_scene_w) { // (the buffers are the scene's size)
				c.width = qs_scene_w;
				c.height = qs_scene_h;
				fb = VK_NULL_HANDLE;
				if (next(device, &c, alloc, &fb) == VK_SUCCESS)
					t.stream[h] = (uint64_t)fb;
			}
		}
		if (t.twin[0] && t.twin[1])
			qs_fbs[qs_nfbs++] = t;
		else
			for (int h = 0; h < 2; h++) {
				if (t.twin[h])
					destroy(device, (VkFramebuffer)t.twin[h], alloc);
				if (t.stream[h])
					destroy(device, (VkFramebuffer)t.stream[h], alloc);
			}
	}
	pthread_mutex_unlock(&qs_twin_lock);
	return r;
}

static VKAPI_ATTR void VKAPI_CALL qs_DestroyFramebuffer(VkDevice device, VkFramebuffer fb, const VkAllocationCallbacks *alloc)
{
	struct device *d = device_of(device);
	PFN_vkDestroyFramebuffer next = (PFN_vkDestroyFramebuffer)d->gdpa(device, "vkDestroyFramebuffer");
	PFN_vkDestroyImageView destroy_view = (PFN_vkDestroyImageView)d->gdpa(device, "vkDestroyImageView");
	pthread_mutex_lock(&qs_scene_lock);
	for (unsigned i = 0; i < qs_nsfbs;)
		if (qs_scene_fbs[i].fb == (uint64_t)fb)
			qs_scene_fbs[i] = qs_scene_fbs[--qs_nsfbs];
		else
			i++;
	for (unsigned i = 0; i < qs_nstwins;)
		if (qs_stwins[i].fb == (uint64_t)fb) {
			next(device, (VkFramebuffer)qs_stwins[i].twin, alloc);
			destroy_view(device, (VkImageView)qs_stwins[i].view, alloc);
			qs_stwins[i] = qs_stwins[--qs_nstwins];
		} else
			i++;
	pthread_mutex_unlock(&qs_scene_lock);
	pthread_mutex_lock(&qs_twin_lock);
	for (unsigned i = 0; i < qs_nfbs; i++)
		if (qs_fbs[i].handle == (uint64_t)fb) {
			for (int h = 0; h < 2; h++) {
				next(device, (VkFramebuffer)qs_fbs[i].twin[h], alloc);
				if (qs_fbs[i].stream[h])
					next(device, (VkFramebuffer)qs_fbs[i].stream[h], alloc);
			}
			qs_fbs[i] = qs_fbs[--qs_nfbs];
			break;
		}
	pthread_mutex_unlock(&qs_twin_lock);
	next(device, fb, alloc);
}

// command buffers recording a render pass into a twin framebuffer: their viewports and scissors move
// left by QS_PANEL_W until the end of the pass
static VkCommandBuffer qs_shifted[8];
static pthread_mutex_t qs_shift_lock = PTHREAD_MUTEX_INITIALIZER;

static bool qs_is_shifted(VkCommandBuffer cb)
{
	bool r = false;
	pthread_mutex_lock(&qs_shift_lock);
	for (unsigned i = 0; i < 8; i++)
		r |= qs_shifted[i] == cb;
	pthread_mutex_unlock(&qs_shift_lock);
	return r;
}

static void qs_set_shifted(VkCommandBuffer cb, bool on)
{
	pthread_mutex_lock(&qs_shift_lock);
	for (unsigned i = 0; i < 8; i++)
		if (on ? !qs_shifted[i] : qs_shifted[i] == cb) {
			qs_shifted[i] = on ? cb : VK_NULL_HANDLE;
			break;
		}
	pthread_mutex_unlock(&qs_shift_lock);
}

// command buffers recording a distortion pass stretched over a scene-sized buffer (qs_streaming): their viewports
// and scissors move left by `off` and scale from the panel's size to the buffer's until the end of the pass
static struct
{
	VkCommandBuffer cb;
	int off;
} qs_scaled[8];

//! The x offset the viewports of `cb` move by, -1 if it is not stretched.
static int qs_scaled_off(VkCommandBuffer cb)
{
	int r = -1;
	pthread_mutex_lock(&qs_shift_lock);
	for (unsigned i = 0; i < 8; i++)
		if (qs_scaled[i].cb == cb)
			r = qs_scaled[i].off;
	pthread_mutex_unlock(&qs_shift_lock);
	return r;
}

static void qs_set_scaled(VkCommandBuffer cb, int off)
{
	pthread_mutex_lock(&qs_shift_lock);
	for (unsigned i = 0; i < 8; i++)
		if (off >= 0 ? !qs_scaled[i].cb : qs_scaled[i].cb == cb) {
			qs_scaled[i].cb = off >= 0 ? cb : VK_NULL_HANDLE;
			qs_scaled[i].off = off;
			break;
		}
	pthread_mutex_unlock(&qs_shift_lock);
}
