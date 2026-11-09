// quest1_scanout: direct scanout of vrcompositor's display swapchain on the Quest 1 panels
// (QUEST1_DIRECT_SCANOUT=1; included by quest1_display.c after struct qd_swapchain).
//
// vrcompositor does the lens distortion itself (driver_quest1's ComputeDistortion) into a
// 2880x1600 swapchain whose images are gralloc buffers the display engine can read. A present
// commits the finished image on /dev/fb0 (MSMFB_ATOMIC_COMMIT, one DMA pipe per panel, as Monado's
// comp_window_mdp.c does). No copy and no second distortion pass: Monado pauses its compositing
// while /tmp/quest1-scanout names this process, and keeps the panels on while the headset is worn.
//
// Image life cycle: app (acquired) -> rendering -> committed (driver_owned) until the commit's
// release fence signals, i.e. once a later commit has replaced it on the panels. The retire fence
// of a commit signals when it is on screen: vkWaitForPresentKHR returns then.
#include <dlfcn.h>
#include <linux/fb.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <vulkan/vulkan_android.h>

#include "quest1_direct_proto.h"

#define __user
#define GC msm_mdp_GC
#include <linux/msm_mdp_ext.h>
#undef GC

#define QS_FLAG_FILE "/tmp/quest1-scanout"
#define QS_PANEL_W 1440
#define QS_PANEL_H 1600
#define QS_PRIV_FLAGS_UBWC_ALIGNED 0x08000000

// NDK AHardwareBuffer ABI (libnativewindow, loaded through libhybris)
typedef struct
{
	uint32_t width, height, layers, format;
	uint64_t usage;
	uint32_t stride, rfu0;
	uint64_t rfu1;
} qs_ahb_desc;
typedef struct
{
	int version, numFds, numInts;
	int data[];
} qs_native_handle;
#define QS_AHB_FORMAT_R8G8B8A8_UNORM 1
// VENDOR_0 makes Qualcomm's gralloc allocate UBWC (compressed) buffers: vrcompositor's distortion pass, whose output
// these are, costs ~2.3 ms per eye instead of ~2.75 into linear buffers (2026-11-06, panels on, dashboard shown; Monado's
// warp found the same). On tiled surfaces the Adreno stores B8G8R8A8 as R8G8B8A8 (the BGRA swap applies to linear
// memory only), so mdss scans them out as MDP_RGBA_8888_UBWC and Monado samples them without a red/blue swizzle.
#define QS_AHB_USAGE                                                                                                   \
	(0x100u /* GPU_SAMPLED_IMAGE */ | 0x200u /* GPU_COLOR_OUTPUT */ | 0x800u /* COMPOSER_OVERLAY */ |              \
	 0x10000000u /* VENDOR_0: UBWC */)

static struct
{
	int (*allocate)(const qs_ahb_desc *, AHardwareBuffer **);
	void (*describe)(const AHardwareBuffer *, qs_ahb_desc *);
	void (*release)(AHardwareBuffer *);
	const qs_native_handle *(*native_handle)(const AHardwareBuffer *);
	int (*send_handle)(const AHardwareBuffer *, int);
} qs_ahb;
static int qs_fb = -1, qs_blank = -1;
static bool qs_flag_written;

static bool qs_enabled(void)
{
	static int on = -1;
	if (on < 0)
		on = getenv("QUEST1_DIRECT_SCANOUT") && atoi(getenv("QUEST1_DIRECT_SCANOUT")) == 1;
	return on;
}

// Scene mode (/etc/quest1-warp-lens "2", 2026-11-08): Monado's warp does the lens distortion, its chromatic aberration and
// the re-projection in one pass straight from vrcompositor's eye render, as Meta's TimeWarp does. vrcompositor renders
// each eye into a scene image (its render size, 1500x1664) and resamples it into the eye's panel image with its
// distortion pass (~2 ms an eye, memory traffic: as much when the distortion is an identity), which Monado's warp then
// resamples again. Here the eye's passes render straight into the eye's gralloc buffer, of the scene's size (a twin
// of vrcompositor's framebuffer with that buffer as its colour attachment, through an sRGB view), and the distortion
// pass is dropped. The frame's swapchain image is only acquired after the first eye is recorded: the index is
// reserved when that eye's first pass begins (qs_reserve) and that acquire returns it. Monado's lens warp costs
// ~0.9 ms a vsync more than its plain one.
// The buffers are made with the display, before vrcompositor creates its render targets: their size is the one seen
// last time (QS_SCENE_SIZE_FILE, written when a distortion pass reads a scene image of another size: SteamVR's render
// resolution changed; until vrcompositor starts again the eye renders are copied, as when no index was free).
#define QS_SCENE_SIZE_FILE ".cache/quest1-scene-size" // in $HOME
static uint32_t qs_scene_w, qs_scene_h; // 0: off
static bool qs_scene_on;                // asked for (qs_scene_w may still be 0: size unknown)
static int qs_first_eye = 1;            // the half vrcompositor renders first (its first distortion pass: x = 1440)

static void qs_scene_size_path(char *out, size_t n)
{
	const char *home = getenv("HOME");
	snprintf(out, n, "%s/%s", home ? home : "/tmp", QS_SCENE_SIZE_FILE);
}

static void qs_scene_mode_read(void)
{
	static bool done;
	if (done)
		return;
	done = true;
	FILE *f = fopen("/etc/quest1-warp-lens", "r");
	int c = f ? fgetc(f) : 0;
	if (f)
		fclose(f);
	if (c != '2')
		return;
	qs_scene_on = true;
	unsigned w = 0, h = 0;
	char path[256];
	qs_scene_size_path(path, sizeof(path));
	f = fopen(path, "r");
	if (!f || fscanf(f, "%u %u", &w, &h) != 2 || w < QS_PANEL_W / 2 || h < QS_PANEL_H / 2 || w > 4096 || h > 4096)
		LOG("direct scanout: scene mode: render size not known yet (%s): from vrcompositor's next start\n", path);
	else {
		qs_scene_w = w;
		qs_scene_h = h;
		LOG("direct scanout: scene mode: the eyes render %ux%u straight into Monado's buffers, it does the lens\n", w, h);
	}
	if (f)
		fclose(f);
}

// vrcompositor's colour render targets (vkCreateImage, quest1_compat.c), its views of them and framebuffers on them
static struct { uint64_t image; uint32_t w, h; } qs_scene_imgs[32];
static unsigned qs_nscene;
static struct { uint64_t view; VkImageViewCreateInfo ci; } qs_scene_views[32];
static unsigned qs_nsviews;
#define QS_SFB_ATT 4
static struct
{
	uint64_t fb;
	VkFramebufferCreateInfo ci;
	VkImageView att[QS_SFB_ATT];
	uint32_t color; //!< which attachment is the scene image
	VkImageViewCreateInfo vci;
} qs_scene_fbs[16];
static unsigned qs_nsfbs;
// their twins on the gralloc buffers, made when first used
static struct { uint64_t fb, half, view, twin; } qs_stwins[64];
static unsigned qs_nstwins;
static pthread_mutex_t qs_scene_lock = PTHREAD_MUTEX_INITIALIZER;

// The system layer in 4x MSAA: SteamVR's dashboard asks for it (with the camera roomview) while Steam's VR UI is
// loading, in the OOBE and on the lock screen (systemui.js, "systemlayer-msaa-enable-override"). Its eyes then render
// into 4x images that only the distortion pass reads, resolving the samples itself. Scene mode drops that pass and
// needs 1-sample targets for the eyes to render into its buffers, so there vrcompositor's 4x attachments, render passes
// and pipelines are made 1-sample (quest1_compat.c): no anti-aliasing on those screens, as on the others. Without it
// (2026-11-08) the distortion pass ran into the scene-sized buffers, Monado distorted that again: 9 fps and the
// controllers drawn wrong. (With RenderMode=Direct the 4x layer also costs ~25 ms of GPU a frame.)
static bool qs_scene_single_sample(VkSampleCountFlagBits samples)
{
	if (samples == VK_SAMPLE_COUNT_1_BIT || !qs_enabled())
		return false;
	qs_scene_mode_read();
	return qs_scene_on;
}

static void qs_scene_note(VkImage image, const VkImageCreateInfo *ci)
{
	const VkImageUsageFlags want = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	if (!qs_enabled() || (ci->usage & want) != want || ci->samples != VK_SAMPLE_COUNT_1_BIT || ci->mipLevels != 1 ||
	    ci->arrayLayers != 1 || ci->imageType != VK_IMAGE_TYPE_2D ||
	    (ci->format != VK_FORMAT_R8G8B8A8_SRGB && ci->format != VK_FORMAT_R8G8B8A8_UNORM) || ci->extent.width < 1024)
		return;
	pthread_mutex_lock(&qs_scene_lock);
	qs_scene_imgs[qs_nscene++ % 32] = (typeof(qs_scene_imgs[0])){(uint64_t)image, ci->extent.width, ci->extent.height};
	pthread_mutex_unlock(&qs_scene_lock);
}

//! Call with qs_scene_lock held: the size of a noted render target, 0 if it is none.
static uint32_t qs_scene_img_size(uint64_t image, uint32_t *h)
{
	for (unsigned k = 0; k < 32 && k < qs_nscene; k++)
		if (qs_scene_imgs[k].image == image) {
			*h = qs_scene_imgs[k].h;
			return qs_scene_imgs[k].w;
		}
	return 0;
}

// per command buffer: the scene image last made SHADER_READ_ONLY (a distortion pass's source, for the copy), the
// eye being rendered (run), and whether a dropped distortion pass is being recorded
static struct
{
	VkCommandBuffer cb;
	uint64_t scene;
	uint32_t w, h;
	bool skipping;
	bool in_run, redirect; //!< an eye's passes are being recorded; into its gralloc buffer
	int eye, idx;          //!< that eye's half, the frame's reserved swapchain index (-1: none)
	unsigned runs;         //!< eyes begun in this recording
} qs_cbst[16];
static pthread_mutex_t qs_cbst_lock = PTHREAD_MUTEX_INITIALIZER;

//! Call with qs_cbst_lock held.
static typeof(qs_cbst[0]) *qs_cbst_get(VkCommandBuffer cb, bool add)
{
	for (unsigned i = 0; i < 16; i++)
		if (qs_cbst[i].cb == cb)
			return &qs_cbst[i];
	if (!add)
		return NULL;
	static unsigned next;
	typeof(qs_cbst[0]) *e = &qs_cbst[next++ % 16];
	memset(e, 0, sizeof(*e));
	e->cb = cb;
	e->idx = -1;
	return e;
}

//! vkBeginCommandBuffer (quest1_compat.c): a new recording.
static void qs_scene_cb_begin(VkCommandBuffer cb)
{
	if (!qs_scene_w)
		return;
	pthread_mutex_lock(&qs_cbst_lock);
	typeof(qs_cbst[0]) *e = qs_cbst_get(cb, false);
	if (e) {
		memset(e, 0, sizeof(*e));
		e->cb = cb;
		e->idx = -1;
	}
	pthread_mutex_unlock(&qs_cbst_lock);
}

static bool qs_skipping(VkCommandBuffer cb)
{
	if (!qs_scene_w)
		return false;
	pthread_mutex_lock(&qs_cbst_lock);
	typeof(qs_cbst[0]) *e = qs_cbst_get(cb, false);
	bool r = e && e->skipping;
	pthread_mutex_unlock(&qs_cbst_lock);
	return r;
}

static void qs_remove_flag(void)
{
	if (qs_flag_written)
		unlink(QS_FLAG_FILE);
	qs_flag_written = false;
}

static bool qs_load(void)
{
	if (qs_ahb.allocate)
		return true;
	// libhybris-common comes in as a local dependency of the hybris libvulkan: not visible through
	// RTLD_DEFAULT, so take a handle on the already loaded copy (a second copy would be a second linker).
	void *hc = dlopen("libhybris-common.so.1", RTLD_NOW | RTLD_NOLOAD);
	if (!hc)
		hc = dlopen("/opt/hybris/lib/libhybris-common.so.1", RTLD_NOW | RTLD_NOLOAD);
	void *lookup = hc ? hc : RTLD_DEFAULT;
	void *(*adlopen)(const char *, int) = (void *(*)(const char *, int))dlsym(lookup, "android_dlopen");
	void *(*adlsym)(void *, const char *) = (void *(*)(void *, const char *))dlsym(lookup, "android_dlsym");
	void *nw = adlopen && adlsym ? adlopen("libnativewindow.so", RTLD_NOW) : NULL;
	if (!nw) {
		LOG("direct scanout: no libhybris libnativewindow\n");
		return false;
	}
	*(void **)&qs_ahb.allocate = adlsym(nw, "AHardwareBuffer_allocate");
	*(void **)&qs_ahb.describe = adlsym(nw, "AHardwareBuffer_describe");
	*(void **)&qs_ahb.release = adlsym(nw, "AHardwareBuffer_release");
	*(void **)&qs_ahb.native_handle = adlsym(nw, "AHardwareBuffer_getNativeHandle");
	*(void **)&qs_ahb.send_handle = adlsym(nw, "AHardwareBuffer_sendHandleToUnixSocket");
	if (!qs_ahb.allocate || !qs_ahb.describe || !qs_ahb.release || !qs_ahb.native_handle) {
		LOG("direct scanout: AHardwareBuffer functions missing\n");
		qs_ahb.allocate = NULL;
		return false;
	}
	qs_fb = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	qs_blank = open("/sys/class/graphics/fb0/show_blank_event", O_RDONLY | O_CLOEXEC);
	if (qs_fb < 0) {
		LOG("direct scanout: /dev/fb0: %s\n", strerror(errno));
		qs_ahb.allocate = NULL;
		return false;
	}
	atexit(qs_remove_flag);
	return true;
}

//! The panels are powered (Monado switches them off while the headset is not worn).
static bool qs_panels_on(void)
{
	// Monado creates it before powering the panels down: a commit then would wedge mdss
	if (access("/tmp/quest1-panels-off", F_OK) == 0)
		return false;
	char buf[64];
	ssize_t n = qs_blank >= 0 ? pread(qs_blank, buf, sizeof(buf) - 1, 0) : -1;
	if (n <= 0)
		return true;
	buf[n] = 0;
	return strstr(buf, "= 1") != NULL;
}

//! A gralloc buffer of ici's size, imported in Vulkan as an image created from ici.
static VkResult qs_import(struct qd_swapchain *sc, const VkImageCreateInfo *ici, AHardwareBuffer **ahb, VkImage *image,
                          VkDeviceMemory *memory, int *fd, uint32_t *stride_px, bool *ubwc)
{
	struct device *d = sc->d;
	VkDevice device = sc->device;
	PFN_vkGetAndroidHardwareBufferPropertiesANDROID get_props =
	    (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)d->gdpa(device, "vkGetAndroidHardwareBufferPropertiesANDROID");
	PFN_vkCreateImage create_image = (PFN_vkCreateImage)d->gdpa(device, "vkCreateImage");
	PFN_vkAllocateMemory allocate = (PFN_vkAllocateMemory)d->gdpa(device, "vkAllocateMemory");
	PFN_vkBindImageMemory bind = (PFN_vkBindImageMemory)d->gdpa(device, "vkBindImageMemory");

	qs_ahb_desc desc = {ici->extent.width, ici->extent.height, 1, QS_AHB_FORMAT_R8G8B8A8_UNORM, QS_AHB_USAGE};
	int e = qs_ahb.allocate(&desc, ahb);
	if (e != 0) {
		LOG("direct scanout: AHardwareBuffer_allocate: %d\n", e);
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}
	qs_ahb.describe(*ahb, &desc);
	const qs_native_handle *nh = qs_ahb.native_handle(*ahb);
	*fd = nh->data[0]; // gralloc private_handle_t: fd, fd_metadata, magic, flags
	*ubwc = ((uint32_t)nh->data[3] & QS_PRIV_FLAGS_UBWC_ALIGNED) != 0;
	*stride_px = desc.stride;

	VkAndroidHardwareBufferFormatPropertiesANDROID fp = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
	VkAndroidHardwareBufferPropertiesANDROID props = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID, &fp};
	VkResult r = get_props(device, *ahb, &props);
	if (r == VK_SUCCESS)
		r = create_image(device, ici, NULL, image);
	if (r != VK_SUCCESS)
		return r;
	VkImportAndroidHardwareBufferInfoANDROID imp = {VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
	imp.buffer = *ahb;
	VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &imp};
	ded.image = *image;
	VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded};
	mai.allocationSize = props.allocationSize;
	mai.memoryTypeIndex = (uint32_t)__builtin_ctz(props.memoryTypeBits);
	r = allocate(device, &mai, NULL, memory);
	if (r == VK_SUCCESS)
		r = bind(device, *image, *memory, 0);
	return r;
}

//! An image from ici in plain device-local memory.
static VkResult qs_plain_image(struct qd_swapchain *sc, const VkImageCreateInfo *ici, VkImage *image,
                               VkDeviceMemory *memory)
{
	struct device *d = sc->d;
	VkDevice device = sc->device;
	PFN_vkCreateImage create_image = (PFN_vkCreateImage)d->gdpa(device, "vkCreateImage");
	PFN_vkGetImageMemoryRequirements reqs =
	    (PFN_vkGetImageMemoryRequirements)d->gdpa(device, "vkGetImageMemoryRequirements");
	PFN_vkAllocateMemory allocate = (PFN_vkAllocateMemory)d->gdpa(device, "vkAllocateMemory");
	PFN_vkBindImageMemory bind = (PFN_vkBindImageMemory)d->gdpa(device, "vkBindImageMemory");
	VkResult r = create_image(device, ici, NULL, image);
	if (r != VK_SUCCESS)
		return r;
	VkMemoryRequirements mr;
	reqs(device, *image, &mr);
	// never rendered to nor read: they all share the first one's memory (18 MB each at 2880x1600, pinned)
	if (sc->memory[0] && memory != &sc->memory[0]) {
		*memory = VK_NULL_HANDLE; // freed with images[0]'s
		return bind(device, *image, sc->memory[0], 0);
	}
	VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = (uint32_t)__builtin_ctz(mr.memoryTypeBits);
	r = allocate(device, &mai, NULL, memory);
	if (r == VK_SUCCESS)
		r = bind(device, *image, *memory, 0);
	return r;
}

// --- each half in a buffer of its own -------------------------------------------------------------
// vrcompositor draws each eye in its own render pass over its half of the display image. On the
// Adreno 540 blob, a render area at x = 1440 of a linear gralloc image comes out as copies of its
// first 320-px bin column, and rendering into the 2880-wide linear image is slow for either half.
// So each eye's pass renders into a 1440-wide image of its own at x = 0 instead (twin image views
// and framebuffers; the right half's viewports and scissors move left), one per display pipe. The
// app's own 2880-wide images only serve as handles.

struct qs_twin
{
	uint64_t handle;  //!< app's image view / framebuffer
	uint64_t twin[2]; //!< ours on half_image[0] / half_image[1]
	uint64_t img[2];  //!< half_image[0] / [1]
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
		struct qs_twin t = {(uint64_t)*out, {0, 0}, {v->img[0], v->img[1]}};
		for (int h = 0; h < 2; h++) {
			VkFramebufferCreateInfo c = *ci;
			VkImageView a = (VkImageView)v->twin[h];
			c.pAttachments = &a;
			c.width = QS_PANEL_W;
			VkFramebuffer fb = VK_NULL_HANDLE;
			if (next(device, &c, alloc, &fb) == VK_SUCCESS)
				t.twin[h] = (uint64_t)fb;
		}
		if (t.twin[0] && t.twin[1])
			qs_fbs[qs_nfbs++] = t;
		else
			for (int h = 0; h < 2; h++)
				if (t.twin[h])
					destroy(device, (VkFramebuffer)t.twin[h], alloc);
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
			for (int h = 0; h < 2; h++)
				next(device, (VkFramebuffer)qs_fbs[i].twin[h], alloc);
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

// Images for a display swapchain of the panels' size: gralloc buffers imported in Vulkan, created
// as R8G8B8A8_UNORM with the swapchain's format allowed for views. Fills sc->images/memory.
static VkResult qs_create_images(struct qd_swapchain *sc, const VkSwapchainCreateInfoKHR *ci)
{
	struct device *d = sc->d;
	VkDevice device = sc->device;
	PFN_vkGetAndroidHardwareBufferPropertiesANDROID get_props =
	    (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)d->gdpa(device, "vkGetAndroidHardwareBufferPropertiesANDROID");
	if (!get_props) {
		LOG("direct scanout: VK_ANDROID_external_memory_android_hardware_buffer is not enabled\n");
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	// vrcompositor's views use the swapchain format: with B8G8R8A8 they store BGRA bytes in this RGBA
	// image, which the commit then declares to the display engine (qs_mdp_format)
	VkFormat formats[4] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB};
	uint32_t nformats = 2;
	if (ci->imageFormat == VK_FORMAT_B8G8R8A8_UNORM || ci->imageFormat == VK_FORMAT_B8G8R8A8_SRGB) {
		formats[nformats++] = VK_FORMAT_B8G8R8A8_UNORM;
		formats[nformats++] = VK_FORMAT_B8G8R8A8_SRGB;
	}
	VkImageFormatListCreateInfo fl = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
	fl.viewFormatCount = nformats;
	fl.pViewFormats = formats;
	VkExternalMemoryImageCreateInfo emi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &fl};
	emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
	VkImageCreateInfo ici = sc->ici;
	ici.pNext = &emi;
	ici.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
	ici.format = VK_FORMAT_R8G8B8A8_UNORM;
	sc->ici.flags = ici.flags; // the views vrcompositor makes use the swapchain format (sRGB)

	// the app's images: plain device memory, never rendered to (see qs_CmdBeginRenderPass)
	VkImageCreateInfo plain = ici;
	plain.pNext = &fl;
	VkImageCreateInfo half = ici;
	half.extent.width = QS_PANEL_W;
	half.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT; // snapshots of UBWC buffers are GPU copies (qs_snapshot)
	qs_scene_mode_read();
	if (qs_scene_w) { // scene mode: the eyes render into them (or are copied in)
		half.extent.width = qs_scene_w;
		half.extent.height = qs_scene_h;
		half.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	}
	for (uint32_t i = 0; i < sc->count; i++) {
		sc->release_fence[i] = -1;
		VkResult r = qs_plain_image(sc, &plain, &sc->images[i], &sc->memory[i]);
		for (int h = 0; h < 2 && r == VK_SUCCESS; h++) {
			sc->half_fd[h][i] = -1;
			r = qs_import(sc, &half, &sc->half_ahb[h][i], &sc->half_image[h][i], &sc->half_memory[h][i],
			              &sc->half_fd[h][i], &sc->half_stride_px[h][i], &sc->half_ubwc[h][i]);
		}
		if (r != VK_SUCCESS) {
			LOG("direct scanout: image %u: %d\n", i, r);
			return r;
		}
	}
	qs_twins_register(sc);
	LOG("direct scanout: %u x 2 gralloc buffers %ux%u, one per panel (stride %u px, %s)\n", sc->count, half.extent.width,
	    half.extent.height, sc->half_stride_px[0][0], sc->half_ubwc[0][0] ? "UBWC" : "linear");
	return VK_SUCCESS;
}

//! Debug only (it stalls): a UBWC half can't be read through its mapping, so the GPU copies image `idx`'s halves
//! (rendered, held for the panels, in vrcompositor's final PRESENT_SRC layout) into a linear buffer, R8G8B8A8 rows of
//! QS_PANEL_W pixels. Returns the mapping, NULL on failure; qs_snapshot_free releases it.
struct qs_snapcopy
{
	VkBuffer buf;
	VkDeviceMemory mem;
	uint8_t *ptr;
};

static void qs_snapshot_free(struct qd_swapchain *sc, struct qs_snapcopy *c)
{
	struct device *d = sc->d;
	if (c->ptr)
		d->UnmapMemory(sc->device, c->mem);
	if (c->buf)
		d->DestroyBuffer(sc->device, c->buf, NULL);
	if (c->mem)
		d->FreeMemory(sc->device, c->mem, NULL);
	memset(c, 0, sizeof(*c));
}

static bool qs_snapshot_copy(struct qd_swapchain *sc, uint32_t idx, struct qs_snapcopy *c)
{
	struct device *d = sc->d;
	VkDevice dev = sc->device;
	uint32_t h = sc->ici.extent.height;
	VkDeviceSize half_bytes = (VkDeviceSize)QS_PANEL_W * h * 4;
	memset(c, 0, sizeof(*c));
	VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = 2 * half_bytes;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	if (d->CreateBuffer(dev, &bci, NULL, &c->buf) != VK_SUCCESS)
		return false;
	VkMemoryRequirements mr;
	d->GetBufferMemoryRequirements(dev, c->buf, &mr);
	int type = -1;
	for (uint32_t i = 0; i < d->memprops.memoryTypeCount && type < 0; i++)
		if ((mr.memoryTypeBits & (1u << i)) &&
		    (d->memprops.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
			type = (int)i;
	VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = (uint32_t)(type < 0 ? 0 : type);
	void *p = NULL;
	if (type < 0 || d->AllocateMemory(dev, &mai, NULL, &c->mem) != VK_SUCCESS ||
	    d->BindBufferMemory(dev, c->buf, c->mem, 0) != VK_SUCCESS) {
		qs_snapshot_free(sc, c);
		return false;
	}

	PFN_vkCreateCommandPool create_pool = (PFN_vkCreateCommandPool)d->gdpa(dev, "vkCreateCommandPool");
	PFN_vkEndCommandBuffer end = (PFN_vkEndCommandBuffer)d->gdpa(dev, "vkEndCommandBuffer");
	PFN_vkCmdPipelineBarrier barrier = (PFN_vkCmdPipelineBarrier)d->gdpa(dev, "vkCmdPipelineBarrier");
	PFN_vkCmdCopyImageToBuffer copy = (PFN_vkCmdCopyImageToBuffer)d->gdpa(dev, "vkCmdCopyImageToBuffer");
	VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pci.queueFamilyIndex = 0; // sc->queue's
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cb = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 1;
	VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	bool ok = create_pool(dev, &pci, NULL, &pool) == VK_SUCCESS;
	cai.commandPool = pool;
	ok = ok && d->AllocateCommandBuffers(dev, &cai, &cb) == VK_SUCCESS && d->CreateFence(dev, &fci, NULL, &fence) == VK_SUCCESS;
	if (ok) {
		VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		d->BeginCommandBuffer(cb, &bi);
		VkImageMemoryBarrier b[2];
		for (int k = 0; k < 2; k++) {
			b[k] = (VkImageMemoryBarrier){VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
			b[k].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			b[k].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			b[k].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			b[k].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			b[k].srcQueueFamilyIndex = b[k].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b[k].image = sc->half_image[k][idx];
			b[k].subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		}
		barrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, b);
		for (int k = 0; k < 2; k++) {
			VkBufferImageCopy r = {0};
			r.bufferOffset = (VkDeviceSize)k * half_bytes;
			r.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			r.imageExtent = (VkExtent3D){QS_PANEL_W, h, 1};
			copy(cb, sc->half_image[k][idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->buf, 1, &r);
		}
		for (int k = 0; k < 2; k++) {
			b[k].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			b[k].dstAccessMask = 0;
			b[k].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			b[k].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		}
		barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 2, b);
		ok = end(cb) == VK_SUCCESS;
	}
	if (ok) {
		VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
		si.commandBufferCount = 1;
		si.pCommandBuffers = &cb;
		pthread_mutex_lock(&d->queue_mutex);
		ok = d->QueueSubmit(sc->queue, 1, &si, fence) == VK_SUCCESS;
		pthread_mutex_unlock(&d->queue_mutex);
		ok = ok && d->WaitForFences(dev, 1, &fence, VK_TRUE, 2000000000ull) == VK_SUCCESS;
	}
	if (fence)
		d->DestroyFence(dev, fence, NULL);
	if (pool)
		d->DestroyCommandPool(dev, pool, NULL); // frees cb
	if (ok && d->MapMemory(dev, c->mem, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS) {
		c->ptr = p;
		return true;
	}
	qs_snapshot_free(sc, c);
	return false;
}

// debug: `runuser -u steamos -- touch /tmp/quest1-snap` writes the next frame to /tmp/quest1-snap.ppm
static void qs_snapshot(struct qd_swapchain *sc, uint32_t idx)
{
	if (unlink("/tmp/quest1-snap") != 0) // a trigger we cannot remove (root's, in sticky /tmp) is ignored
		return;
	uint32_t w = 2 * QS_PANEL_W, h = sc->ici.extent.height;
	uint32_t stride[2] = {sc->half_stride_px[0][idx] * 4, sc->half_stride_px[1][idx] * 4};
	uint8_t *p[2];
	struct qs_snapcopy copy = {0};
	bool ubwc = sc->half_ubwc[0][idx] || sc->half_ubwc[1][idx];
	if (ubwc) {
		if (!qs_snapshot_copy(sc, idx, &copy)) {
			LOG("direct scanout: snapshot: the GPU copy of the UBWC buffers failed%c", 10);
			return;
		}
		for (int k = 0; k < 2; k++) {
			stride[k] = QS_PANEL_W * 4;
			p[k] = copy.ptr + (size_t)k * stride[k] * h;
		}
	} else {
		for (int k = 0; k < 2; k++)
			p[k] = mmap(NULL, (size_t)stride[k] * h, PROT_READ, MAP_SHARED, sc->half_fd[k][idx], 0);
	}
	FILE *f = p[0] != MAP_FAILED && p[1] != MAP_FAILED ? fopen("/tmp/quest1-snap.ppm.tmp", "wb") : NULL;
	if (f) {
		fprintf(f, "P6%c%u %u%c255%c", 10, w, h, 10, 10);
		uint8_t *row = malloc(w * 3);
		// linear buffers hold what vrcompositor's views wrote: BGRA bytes through B8G8R8A8 views
		bool bgra = !ubwc && (sc->ici.format == VK_FORMAT_B8G8R8A8_UNORM || sc->ici.format == VK_FORMAT_B8G8R8A8_SRGB);
		for (uint32_t y = 0; y < h; y++) {
			for (uint32_t x = 0; x < w; x++) {
				int k = x >= QS_PANEL_W;
				const uint8_t *px = p[k] + (size_t)y * stride[k] + (x - k * QS_PANEL_W) * 4;
				row[x * 3] = px[bgra ? 2 : 0];
				row[x * 3 + 1] = px[1];
				row[x * 3 + 2] = px[bgra ? 0 : 2];
			}
			fwrite(row, 1, w * 3, f);
		}
		free(row);
		fclose(f);
		rename("/tmp/quest1-snap.ppm.tmp", "/tmp/quest1-snap.ppm");
	}
	if (ubwc)
		qs_snapshot_free(sc, &copy);
	else
		for (int k = 0; k < 2; k++)
			if (p[k] != MAP_FAILED)
				munmap(p[k], (size_t)stride[k] * h);
}

// debug: `touch /tmp/quest1-rptrace` logs the render passes, viewports, scissors and dispatches
// recorded during the next frames (how vrcompositor draws into the display image)
static volatile int qs_trace_left;

static void qs_trace_arm(void)
{
	if (unlink("/tmp/quest1-rptrace") != 0)
		return;
	qs_trace_left = 3000;
}

// debug: `runuser -u steamos -- touch /tmp/quest1-gputime` logs the GPU time of each render pass
// recorded during the next frame (timestamps after each pass; the time also covers the commands
// between passes). Ticks of the Adreno always-on counter, 19.2 MHz.
#define QS_TS_SLOTS 1024
#define QS_TS_PER_CB 128
static struct
{
	VkDevice device;
	VkQueryPool pool;
	int state; //!< 0 idle, 1 instrumenting recordings, 2 waiting for the results
	unsigned frames, next_slot, ncbs;
	struct
	{
		VkCommandBuffer cb;
		unsigned first, count, indices;
		VkRect2D area;
		uint64_t rp;
	} cbs[8];
	char label[QS_TS_SLOTS][224];
} qs_ts;
static pthread_mutex_t qs_ts_lock = PTHREAD_MUTEX_INITIALIZER;
static void obj_desc_copy(uint64_t handle, char *out, size_t n); // quest1_compat.c

//! Call with qs_ts_lock held: the instrumented entry of `cb`, if any (or a new one when `add`).
static int qs_ts_entry(VkCommandBuffer cb, bool add)
{
	for (unsigned i = 0; i < qs_ts.ncbs; i++)
		if (qs_ts.cbs[i].cb == cb)
			return (int)i;
	if (!add || qs_ts.state != 1 || qs_ts.ncbs == sizeof(qs_ts.cbs) / sizeof(qs_ts.cbs[0]) ||
	    qs_ts.next_slot + QS_TS_PER_CB > QS_TS_SLOTS)
		return -1;
	struct device *d = device_of(cb);
	if (!qs_ts.pool) {
		PFN_vkCreateQueryPool create = (PFN_vkCreateQueryPool)d->gdpa(d->handle, "vkCreateQueryPool");
		VkQueryPoolCreateInfo ci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
		ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
		ci.queryCount = QS_TS_SLOTS;
		if (create(d->handle, &ci, NULL, &qs_ts.pool) != VK_SUCCESS)
			return -1;
		qs_ts.device = d->handle;
	}
	unsigned i = qs_ts.ncbs++;
	qs_ts.cbs[i].cb = cb;
	qs_ts.cbs[i].first = qs_ts.next_slot;
	qs_ts.cbs[i].count = 0;
	qs_ts.next_slot += QS_TS_PER_CB;
	PFN_vkCmdResetQueryPool reset = (PFN_vkCmdResetQueryPool)d->gdpa(d->handle, "vkCmdResetQueryPool");
	reset(cb, qs_ts.pool, qs_ts.cbs[i].first, QS_TS_PER_CB);
	return (int)i;
}

//! Call with qs_ts_lock held, outside a render pass for the first stamp of a command buffer.
static void qs_ts_stamp(VkCommandBuffer cb, int e, const char *label)
{
	if (e < 0 || qs_ts.cbs[e].count >= QS_TS_PER_CB)
		return;
	static PFN_vkCmdWriteTimestamp write;
	if (!write)
		write = (PFN_vkCmdWriteTimestamp)device_of(cb)->gdpa(device_of(cb)->handle, "vkCmdWriteTimestamp");
	unsigned slot = qs_ts.cbs[e].first + qs_ts.cbs[e].count++;
	write(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qs_ts.pool, slot);
	snprintf(qs_ts.label[slot], sizeof(qs_ts.label[slot]), "%s", label);
}

//! Once per displayed frame: arms the instrumentation, then logs the results.
static void rm_stats(void); // quest1_rpmerge.c

static void qs_ts_frame(void)
{
	rm_stats();
	pthread_mutex_lock(&qs_ts_lock);
	if (qs_ts.state == 0 && unlink("/tmp/quest1-gputime") == 0) {
		qs_ts.state = 1;
		qs_ts.frames = 0;
		qs_ts.ncbs = 0;
		qs_ts.next_slot = 0;
	} else if (qs_ts.state == 1 && ++qs_ts.frames >= 2) {
		qs_ts.state = 2;
		qs_ts.frames = 0;
	} else if (qs_ts.state == 2 && ++qs_ts.frames >= 8) {
		struct device *d = NULL;
		PFN_vkGetQueryPoolResults get = NULL;
		for (unsigned i = 0; i < qs_ts.ncbs; i++) {
			if (!get) {
				d = device_of(qs_ts.cbs[i].cb);
				get = (PFN_vkGetQueryPoolResults)d->gdpa(qs_ts.device, "vkGetQueryPoolResults");
			}
			uint64_t ts[QS_TS_PER_CB];
			unsigned n = qs_ts.cbs[i].count, first = qs_ts.cbs[i].first;
			if (n < 2 || get(qs_ts.device, qs_ts.pool, first, n, sizeof(ts), ts, sizeof(uint64_t),
			                 VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
				LOG("gputime: cb %p: %u stamps, no results\n", (void *)qs_ts.cbs[i].cb, n);
				continue;
			}
			LOG("gputime: cb %p: %.2f ms in %u passes\n", (void *)qs_ts.cbs[i].cb, (ts[n - 1] - ts[0]) / 19200.0, n - 1);
			for (unsigned k = 1; k < n; k++)
				LOG("gputime:   %6.3f ms %s\n", (ts[k] - ts[k - 1]) / 19200.0, qs_ts.label[first + k]);
		}
		qs_ts.state = 0;
	}
	pthread_mutex_unlock(&qs_ts_lock);
}

static void qs_ts_begin_pass(VkCommandBuffer cb, const VkRect2D *area, uint64_t rp)
{
	if (qs_ts.state != 1)
		return;
	pthread_mutex_lock(&qs_ts_lock);
	int e = qs_ts_entry(cb, true);
	if (e >= 0) {
		if (qs_ts.cbs[e].count == 0)
			qs_ts_stamp(cb, e, "start");
		qs_ts.cbs[e].area = *area;
		qs_ts.cbs[e].rp = rp;
		qs_ts.cbs[e].indices = 0;
	}
	pthread_mutex_unlock(&qs_ts_lock);
}

static void qs_ts_draw(VkCommandBuffer cb, uint32_t n)
{
	if (qs_ts.state != 1)
		return;
	pthread_mutex_lock(&qs_ts_lock);
	int e = qs_ts_entry(cb, false);
	if (e >= 0)
		qs_ts.cbs[e].indices += n;
	pthread_mutex_unlock(&qs_ts_lock);
}

static void qs_ts_end_pass(VkCommandBuffer cb)
{
	if (qs_ts.state != 1)
		return;
	pthread_mutex_lock(&qs_ts_lock);
	int e = qs_ts_entry(cb, false);
	if (e >= 0) {
		char label[224], rpd[160] = "";
		const VkRect2D *a = &qs_ts.cbs[e].area;
		obj_desc_copy(qs_ts.cbs[e].rp, rpd, sizeof(rpd));
		snprintf(label, sizeof(label), "pass %ux%u@%d,%d, %u vertices; %s", a->extent.width, a->extent.height,
		         a->offset.x, a->offset.y, qs_ts.cbs[e].indices, rpd);
		qs_ts_stamp(cb, e, label);
	}
	pthread_mutex_unlock(&qs_ts_lock);
}

#define QS_TRACE_NEXT(type, fn) \
	static type next; \
	if (!next) \
		next = (type)device_of(cb)->gdpa(device_of(cb)->handle, fn);

#include "quest1_rpmerge.c"

// vrcompositor's distortion pass draws each eye in its own render pass whose render area is the
// eye's half of the display image. On the Adreno 540 blob, a render area at x = 1440 of a linear
// (gralloc) image comes out as copies of its first 320-px bin column. So those passes run over the
// whole image instead: a variant of the render pass that loads the attachments, and the clear the
// app asked for done with vkCmdClearAttachments on its own render area (its scissor already keeps
// the draws inside it).
struct qs_rp_variant
{
	VkRenderPass rp, load;
};
static struct qs_rp_variant qs_rp_variants[64];
static unsigned qs_rp_nvariants;
static pthread_mutex_t qs_rp_lock = PTHREAD_MUTEX_INITIALIZER;

static VKAPI_ATTR VkResult VKAPI_CALL qs_CreateRenderPass(VkDevice device, const VkRenderPassCreateInfo *ci,
                                                         const VkAllocationCallbacks *alloc, VkRenderPass *out)
{
	struct device *d = device_of(device);
	PFN_vkCreateRenderPass next = (PFN_vkCreateRenderPass)d->gdpa(device, "vkCreateRenderPass");
	VkResult r = next(device, ci, alloc, out);
	if (r != VK_SUCCESS || ci->attachmentCount == 0 || ci->attachmentCount > 4 || ci->subpassCount != 1)
		return r;
	VkAttachmentDescription att[4];
	bool clears = false;
	for (uint32_t i = 0; i < ci->attachmentCount; i++) {
		att[i] = ci->pAttachments[i];
		if (att[i].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR || att[i].loadOp == VK_ATTACHMENT_LOAD_OP_DONT_CARE) {
			clears |= att[i].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR;
			att[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		}
		if (att[i].stencilLoadOp == VK_ATTACHMENT_LOAD_OP_CLEAR)
			return r; // only colour clears are emulated
		// the image is in the final layout of the previous pass over it (the other eye's)
		if (att[i].initialLayout == VK_IMAGE_LAYOUT_UNDEFINED)
			att[i].initialLayout = att[i].finalLayout;
	}
	if (!clears)
		return r;
	VkRenderPassCreateInfo lci = *ci;
	lci.pAttachments = att;
	VkRenderPass load = VK_NULL_HANDLE;
	if (next(device, &lci, alloc, &load) != VK_SUCCESS)
		return r;
	pthread_mutex_lock(&qs_rp_lock);
	if (qs_rp_nvariants < sizeof(qs_rp_variants) / sizeof(qs_rp_variants[0]))
		qs_rp_variants[qs_rp_nvariants++] = (struct qs_rp_variant){*out, load};
	pthread_mutex_unlock(&qs_rp_lock);
	return r;
}

static VKAPI_ATTR void VKAPI_CALL qs_DestroyRenderPass(VkDevice device, VkRenderPass rp, const VkAllocationCallbacks *alloc)
{
	struct device *d = device_of(device);
	PFN_vkDestroyRenderPass next = (PFN_vkDestroyRenderPass)d->gdpa(device, "vkDestroyRenderPass");
	rm_forget_rp(d, rp);
	pthread_mutex_lock(&qs_rp_lock);
	for (unsigned i = 0; i < qs_rp_nvariants; i++)
		if (qs_rp_variants[i].rp == rp) {
			next(device, qs_rp_variants[i].load, alloc);
			qs_rp_variants[i] = qs_rp_variants[--qs_rp_nvariants];
			break;
		}
	pthread_mutex_unlock(&qs_rp_lock);
	next(device, rp, alloc);
}

static VkRenderPass qs_rp_load_variant(VkRenderPass rp)
{
	VkRenderPass load = VK_NULL_HANDLE;
	pthread_mutex_lock(&qs_rp_lock);
	for (unsigned i = 0; i < qs_rp_nvariants; i++)
		if (qs_rp_variants[i].rp == rp)
			load = qs_rp_variants[i].load;
	pthread_mutex_unlock(&qs_rp_lock);
	return load;
}

//! Scene mode: the index the next acquire of the direct swapchain returns, -1 if none is free now.
static int qs_reserve(struct qd_swapchain **out)
{
	pthread_mutex_lock(&qs_twin_lock);
	struct qd_swapchain *sc = qs_direct_scs[0];
	pthread_mutex_unlock(&qs_twin_lock);
	*out = sc;
	if (!sc)
		return -1;
	pthread_mutex_lock(&sc->mutex);
	int r = sc->reserved1 ? (int)sc->reserved1 - 1 : -1;
	for (uint32_t k = 0; k < sc->count && r < 0; k++) {
		uint32_t i = (sc->next + k) % sc->count;
		if (!sc->app_owned[i] && !sc->driver_owned[i])
			r = (int)i;
	}
	if (r >= 0)
		sc->reserved1 = (uint32_t)r + 1;
	pthread_mutex_unlock(&sc->mutex);
	return r;
}

//! Scene mode: the twin of scene framebuffer `fb` whose scene attachment is gralloc buffer `half` (made at first use).
static VkFramebuffer qs_scene_twin(VkDevice device, VkFramebuffer fb, VkImage half)
{
	VkFramebuffer twin = VK_NULL_HANDLE;
	pthread_mutex_lock(&qs_scene_lock);
	for (unsigned i = 0; i < qs_nstwins && !twin; i++)
		if (qs_stwins[i].fb == (uint64_t)fb && qs_stwins[i].half == (uint64_t)half)
			twin = (VkFramebuffer)qs_stwins[i].twin;
	int s = -1;
	for (unsigned i = 0; i < qs_nsfbs && s < 0; i++)
		if (qs_scene_fbs[i].fb == (uint64_t)fb)
			s = (int)i;
	if (twin || s < 0 || qs_nstwins == 64) {
		pthread_mutex_unlock(&qs_scene_lock);
		return twin;
	}
	struct device *d = device_of(device);
	PFN_vkCreateImageView create_view = (PFN_vkCreateImageView)d->gdpa(device, "vkCreateImageView");
	PFN_vkCreateFramebuffer create_fb = (PFN_vkCreateFramebuffer)d->gdpa(device, "vkCreateFramebuffer");
	PFN_vkDestroyImageView destroy_view = (PFN_vkDestroyImageView)d->gdpa(device, "vkDestroyImageView");
	VkImageViewCreateInfo vci = qs_scene_fbs[s].vci;
	vci.pNext = NULL;
	vci.image = half;
	VkImageView view = VK_NULL_HANDLE;
	VkImageView att[QS_SFB_ATT];
	memcpy(att, qs_scene_fbs[s].att, sizeof(att));
	VkFramebufferCreateInfo fci = qs_scene_fbs[s].ci;
	fci.pAttachments = att;
	if (create_view(device, &vci, NULL, &view) == VK_SUCCESS) {
		att[qs_scene_fbs[s].color] = view;
		if (create_fb(device, &fci, NULL, &twin) == VK_SUCCESS)
			qs_stwins[qs_nstwins++] = (typeof(qs_stwins[0])){(uint64_t)fb, (uint64_t)half, (uint64_t)view, (uint64_t)twin};
		else {
			destroy_view(device, view, NULL);
			twin = VK_NULL_HANDLE;
		}
	}
	pthread_mutex_unlock(&qs_scene_lock);
	if (!twin)
		LOG("direct scanout: scene mode: no twin of framebuffer %p on %p\n", (void *)fb, (void *)half);
	return twin;
}

//! Scene mode, at the start of a distortion pass into `half` whose eye was rendered into vrcompositor's own image (no
//! swapchain index was free): copies the scene image it made ready for the pass into `half` and marks the pass's
//! commands to be dropped. False (the pass runs) when no scene image of the size is known.
static bool qs_scene_copy(VkCommandBuffer cb, VkImage half)
{
	pthread_mutex_lock(&qs_cbst_lock);
	typeof(qs_cbst[0]) *e = qs_cbst_get(cb, false);
	VkImage scene = e ? (VkImage)e->scene : VK_NULL_HANDLE;
	uint32_t sw = e ? e->w : 0, sh = e ? e->h : 0;
	if (e)
		e->scene = 0;
	pthread_mutex_unlock(&qs_cbst_lock);
	if (scene && (sw != qs_scene_w || sh != qs_scene_h)) {
		static uint32_t saved_w, saved_h;
		if (sw != saved_w || sh != saved_h) { // the size for the next start
			saved_w = sw;
			saved_h = sh;
			char path[256], tmp[272];
			qs_scene_size_path(path, sizeof(path));
			snprintf(tmp, sizeof(tmp), "%s.tmp", path);
			FILE *f = fopen(tmp, "w");
			if (f) {
				fprintf(f, "%u %u\n", sw, sh);
				fclose(f);
				rename(tmp, path);
			}
			LOG("direct scanout: scene mode: vrcompositor renders the eyes at %ux%u (buffers %ux%u): saved in %s, "
			    "used from its next start\n", sw, sh, qs_scene_w, qs_scene_h, path);
		}
		return false;
	}
	if (!scene) {
		static int logged;
		if (!logged++)
			LOG("direct scanout: scene mode: no %ux%u scene image before a distortion pass, it runs (logged once)\n",
			    qs_scene_w, qs_scene_h);
		return false;
	}
	struct device *d = device_of(cb);
	static PFN_vkCmdPipelineBarrier barrier;
	static PFN_vkCmdCopyImage copy;
	if (!barrier) {
		barrier = (PFN_vkCmdPipelineBarrier)d->gdpa(d->handle, "vkCmdPipelineBarrier");
		copy = (PFN_vkCmdCopyImage)d->gdpa(d->handle, "vkCmdCopyImage");
	}
	const VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	VkImageMemoryBarrier b[2] = {
	    {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
	     VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	     VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, scene, all},
	    {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
	     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, half, all},
	};
	barrier(cb, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, b);
	const VkImageSubresourceLayers l = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	VkImageCopy r = {l, {0, 0, 0}, l, {0, 0, 0}, {qs_scene_w, qs_scene_h, 1}};
	copy(cb, scene, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, half, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
	b[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	b[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	b[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	b[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	b[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	b[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
	b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	b[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 2, b);
	pthread_mutex_lock(&qs_cbst_lock);
	qs_cbst_get(cb, true)->skipping = true;
	pthread_mutex_unlock(&qs_cbst_lock);
	return true;
}

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
		if (start && e->runs == 0)
			idx = qs_reserve(&sc);
		pthread_mutex_lock(&qs_cbst_lock);
		if (start) {
			e->in_run = true;
			e->eye = e->runs == 0 ? qs_first_eye : 1 - qs_first_eye;
			if (e->runs++ == 0)
				e->idx = idx;
			e->redirect = e->idx >= 0;
		}
		VkImage target = VK_NULL_HANDLE;
		if (e && e->redirect) {
			pthread_mutex_lock(&qs_twin_lock);
			sc = qs_direct_scs[0];
			if (sc && (uint32_t)e->idx < sc->count)
				target = sc->half_image[e->eye][e->idx];
			pthread_mutex_unlock(&qs_twin_lock);
		}
		pthread_mutex_unlock(&qs_cbst_lock);
		VkFramebuffer stw = target ? qs_scene_twin(device_of(cb)->handle, bi->framebuffer, target) : VK_NULL_HANDLE;
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
	rm_draw(cb);
	if (qs_trace_left > 0 && qs_trace_left--)
		LOG("trace: cb %p DrawIndexed %u x%u first %u\n", (void *)cb, n, ic, fi);
	qs_ts_draw(cb, n * ic);
	next(cb, n, ic, fi, vo, fin);
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

static void qs_wake(struct qd_swapchain *sc)
{
	if (sc->wake_fd[1] >= 0) {
		char c = 1;
		if (write(sc->wake_fd[1], &c, 1) < 0) {
		}
	}
}

//! Call with sc->mutex held: frame `frame` is (as good as) on screen.
static void qs_displayed_locked(struct qd_swapchain *sc, uint64_t frame)
{
	if (sc->present_ids[frame % 64].frame == frame && sc->present_ids[frame % 64].id > sc->displayed_id)
		sc->displayed_id = sc->present_ids[frame % 64].id;
	sc->displayed_ns = now_ns();
	pthread_cond_broadcast(&sc->cond);
}

//! The byte order vrcompositor wrote (see qs_create_images), as an MDP format.
static uint32_t qs_mdp_format(const struct qd_swapchain *sc, uint32_t idx, int half)
{
	bool bgra = sc->ici.format == VK_FORMAT_B8G8R8A8_UNORM || sc->ici.format == VK_FORMAT_B8G8R8A8_SRGB;
	if (sc->half_ubwc[half][idx])
		return MDP_RGBA_8888_UBWC; // stored as RGBA whatever the view's swap (QS_AHB_USAGE)
	return bgra ? MDP_BGRA_8888 : MDP_RGBA_8888;
}

// --- Monado commits the frames ---------------------------------------------------------------------------
// When Monado's MDP target listens on Q1D_SOCKET, the layer hands it the half buffers once and then
// only announces each finished frame; Monado puts it on the panels (and re-projects it to the head pose
// of the display time), tells when the frame is displayed and when it no longer reads an image. Without
// Monado (or when it goes away) the layer commits the frames itself.

//! Connect to Monado and hand it the buffers. Returns true when sc->mon_sock is set.
static bool qs_mon_connect(struct qd_swapchain *sc)
{
	if (getenv("QUEST1_DIRECT_NO_MONADO") || !qs_ahb.send_handle)
		return false;
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	struct sockaddr_un a = {AF_UNIX};
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", Q1D_SOCKET);
	if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
		if (fd >= 0)
			close(fd);
		return false;
	}
	bool ok = true;
	for (uint32_t i = 0; i < sc->count && ok; i++)
		for (int h = 0; h < 2 && ok; h++) {
			struct q1d_msg m;
			memset(&m, 0, sizeof(m));
			m.type = Q1D_BUFFER;
			m.index = i;
			m.half = (uint32_t)h;
			m.count = sc->count;
			m.width = qs_scene_w ? qs_scene_w : QS_PANEL_W;
			m.height = qs_scene_w ? qs_scene_h : sc->ici.extent.height;
			m.stride_px = sc->half_stride_px[h][i];
			// scene mode: vrcompositor's R8G8B8A8 (sRGB) eye renders
			m.bgra = !qs_scene_w && qs_mdp_format(sc, i, h) == MDP_BGRA_8888;
			m.ubwc = sc->half_ubwc[h][i];
			ok = send(fd, &m, sizeof(m), MSG_NOSIGNAL) == (ssize_t)sizeof(m) &&
			     qs_ahb.send_handle(sc->half_ahb[h][i], fd) == 0;
		}
	if (!ok) {
		LOG("direct scanout: handing the buffers to Monado failed: %s\n", strerror(errno));
		close(fd);
		return false;
	}
	pthread_mutex_lock(&sc->mutex);
	sc->mon_sock = fd;
	pthread_mutex_unlock(&sc->mutex);
	qs_wake(sc);
	LOG("direct scanout: Monado commits and re-projects the frames (%s)\n", Q1D_SOCKET);
	return true;
}

static void qs_write_flag(void)
{
	if (qs_flag_written)
		return;
	FILE *f = fopen(QS_FLAG_FILE, "w");
	if (f) {
		fprintf(f, "%d\n", (int)getpid());
		fclose(f);
		qs_flag_written = true;
		LOG("direct scanout: on the panels (Monado pauses its compositing)\n");
	}
}

//! Announce frame `frame` (image idx, finished) to Monado. False when it is not connected (any more).
static bool qs_mon_present(struct qd_swapchain *sc, uint32_t idx, uint64_t frame)
{
	struct q1d_msg m;
	memset(&m, 0, sizeof(m));
	m.type = Q1D_FRAME;
	m.index = idx;
	m.frame = frame;
	pthread_mutex_lock(&sc->mutex);
	if (sc->mon_sock < 0) {
		pthread_mutex_unlock(&sc->mutex);
		return false;
	}
	if (sc->pose[frame % 64].frame == frame) {
		m.pose_valid = 1;
		memcpy(m.pose, sc->pose[frame % 64].q, sizeof(m.pose));
		m.time_ns = sc->pose[frame % 64].time_ns;
	}
	sc->app_owned[idx] = false;
	sc->driver_owned[idx] = true; // until Monado's Q1D_RELEASE
	bool ok = send(sc->mon_sock, &m, sizeof(m), MSG_NOSIGNAL | MSG_DONTWAIT) == (ssize_t)sizeof(m);
	if (!ok) {
		LOG("direct scanout: Monado is gone (%s): committing ourselves\n", strerror(errno));
		close(sc->mon_sock);
		sc->mon_sock = -1;
		for (uint32_t i = 0; i < sc->count; i++)
			if (sc->release_fence[i] < 0)
				sc->driver_owned[i] = false;
		sc->app_owned[idx] = true; // qs_commit hands it over
	}
	pthread_cond_broadcast(&sc->cond);
	pthread_mutex_unlock(&sc->mutex);
	if (ok)
		qs_write_flag();
	return ok;
}

//! Fence thread, sc->mutex held: Monado's socket is readable.
static void qs_mon_read(struct qd_swapchain *sc)
{
	struct q1d_msg m;
	for (;;) {
		ssize_t n = recv(sc->mon_sock, &m, sizeof(m), MSG_DONTWAIT);
		if (n == (ssize_t)sizeof(m)) {
			if (m.type == Q1D_RELEASE && m.index < sc->count)
				sc->driver_owned[m.index] = false;
			else if (m.type == Q1D_DISPLAYED)
				qs_displayed_locked(sc, m.frame);
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			return;
		break;
	}
	LOG("direct scanout: Monado disconnected, committing ourselves again\n");
	close(sc->mon_sock);
	sc->mon_sock = -1;
	for (uint32_t i = 0; i < sc->count; i++)
		if (sc->release_fence[i] < 0)
			sc->driver_owned[i] = false;
	qs_displayed_locked(sc, sc->frame);
	pthread_cond_broadcast(&sc->cond);
}

// Completer thread, image `idx` rendered: put it on the panels. sc->mutex not held.
static void qs_commit(struct qd_swapchain *sc, uint32_t idx, uint64_t frame)
{
	qs_snapshot(sc, idx);
	qs_trace_arm();
	qs_ts_frame();
	// debug: /tmp/quest1-nomonado makes the layer commit the frames itself (A/B of Monado's presentation)
	bool nomonado = access("/tmp/quest1-nomonado", F_OK) == 0;
	if (nomonado && sc->mon_sock >= 0) {
		pthread_mutex_lock(&sc->mutex);
		close(sc->mon_sock);
		sc->mon_sock = -1;
		for (uint32_t i = 0; i < sc->count; i++)
			if (sc->release_fence[i] < 0 && i != idx)
				sc->driver_owned[i] = false;
		pthread_mutex_unlock(&sc->mutex);
		usleep(50000); // Monado must have dropped the panels before we commit
	}
	if (!nomonado && sc->mon_sock < 0 && now_ns() > sc->mon_retry_ns) {
		sc->mon_retry_ns = now_ns() + 2000000000ull;
		qs_mon_connect(sc);
	}
	if (sc->mon_sock >= 0 && qs_mon_present(sc, idx, frame))
		return;
	int release = -1, retire = -1;
	bool committed = false;
	if (qs_panels_on()) {
		struct mdp_input_layer layers[2];
		memset(layers, 0, sizeof(layers));
		const uint32_t pipes[2] = {64, 128}; // DMA0, DMA1: one per panel
		for (int hh = 0; hh < 2; hh++) {
			struct mdp_input_layer *l = &layers[hh];
			l->pipe_ndx = pipes[hh];
			l->alpha = 0xff;
			l->blend_op = BLEND_OP_OPAQUE;
			l->color_space = MDP_CSC_ITU_R_709;
			// each half from its own buffer (qs_CmdBeginRenderPass)
			l->src_rect = (struct mdp_rect){0, 0, QS_PANEL_W, sc->ici.extent.height};
			l->dst_rect = (struct mdp_rect){hh * QS_PANEL_W, 0, QS_PANEL_W, sc->ici.extent.height};
			l->buffer.width = sc->half_stride_px[hh][idx];
			l->buffer.height = sc->ici.extent.height;
			l->buffer.format = qs_mdp_format(sc, idx, hh);
			l->buffer.planes[0].fd = sc->half_fd[hh][idx];
			l->buffer.planes[0].stride = l->buffer.width * 4;
			l->buffer.plane_count = 1;
			l->buffer.fence = -1;
		}
		struct mdp_layer_commit c;
		memset(&c, 0, sizeof(c));
		c.version = MDP_COMMIT_VERSION_1_0;
		c.commit_v1.input_layers = layers;
		c.commit_v1.input_layer_cnt = 2;
		c.commit_v1.release_fence = -1;
		c.commit_v1.retire_fence = -1;
		if (ioctl(qs_fb, MSMFB_ATOMIC_COMMIT, &c) == 0) {
			committed = true;
			release = c.commit_v1.release_fence;
			retire = c.commit_v1.retire_fence;
			qs_write_flag();
		} else {
			static unsigned errors;
			if (errors++ < 5)
				LOG("direct scanout: MSMFB_ATOMIC_COMMIT: %s (layer errors %d %d)\n", strerror(errno),
				    layers[0].error_code, layers[1].error_code);
		}
	}

	pthread_mutex_lock(&sc->mutex);
	sc->app_owned[idx] = false;
	if (committed && release >= 0) {
		sc->driver_owned[idx] = true; // until the panels let go of it
		sc->release_fence[idx] = release;
	} else if (release >= 0) {
		close(release);
	}
	uint32_t slots = sizeof(sc->retire) / sizeof(sc->retire[0]);
	if (retire >= 0 && sc->retire_tail - sc->retire_head < slots) {
		sc->retire[sc->retire_tail % slots].fd = retire;
		sc->retire[sc->retire_tail % slots].frame = frame;
		sc->retire_tail++;
	} else {
		if (retire >= 0)
			close(retire);
		qs_displayed_locked(sc, frame); // panels off or no fence: nothing to wait for
	}
	pthread_cond_broadcast(&sc->cond);
	pthread_mutex_unlock(&sc->mutex);
	qs_wake(sc);
}

//! Waits on the commits' fences: retire -> displayed (present wait), release -> image free again.
static void *qs_fence_thread(void *arg)
{
	struct qd_swapchain *sc = arg;
	pthread_setname_np(pthread_self(), "q1 scanout");
	q1_realtime("scanout: fence thread", Q1_RT_MAX);
	uint32_t slots = sizeof(sc->retire) / sizeof(sc->retire[0]);
	pthread_mutex_lock(&sc->mutex);
	while (!sc->stopping) {
		struct pollfd p[2 + 8 + QD_MAX_IMAGES];
		int kind[2 + 8 + QD_MAX_IMAGES]; // -2 Monado, -1 wake, 0..7 retire slot, 100+i release of image i
		int n = 0;
		p[n] = (struct pollfd){sc->wake_fd[0], POLLIN, 0};
		kind[n++] = -1;
		if (sc->mon_sock >= 0) {
			p[n] = (struct pollfd){sc->mon_sock, POLLIN, 0};
			kind[n++] = -2;
		}
		for (uint32_t k = sc->retire_head; k != sc->retire_tail; k++) {
			if (sc->retire[k % slots].fd < 0)
				continue;
			p[n] = (struct pollfd){sc->retire[k % slots].fd, POLLIN, 0};
			kind[n++] = (int)(k % slots);
		}
		for (uint32_t i = 0; i < sc->count; i++)
			if (sc->release_fence[i] >= 0) {
				p[n] = (struct pollfd){sc->release_fence[i], POLLIN, 0};
				kind[n++] = 100 + (int)i;
			}
		pthread_mutex_unlock(&sc->mutex);
		int r = poll(p, n, 100);
		pthread_mutex_lock(&sc->mutex);
		if (r <= 0)
			continue;
		for (int j = 0; j < n; j++) {
			if (!p[j].revents)
				continue;
			if (kind[j] == -2) {
				if (sc->mon_sock >= 0)
					qs_mon_read(sc);
			} else if (kind[j] < 0) {
				char buf[16];
				if (read(sc->wake_fd[0], buf, sizeof(buf)) < 0) {
				}
			} else if (kind[j] >= 100) {
				uint32_t i = (uint32_t)(kind[j] - 100);
				close(sc->release_fence[i]);
				sc->release_fence[i] = -1;
				sc->driver_owned[i] = false;
			} else {
				close(sc->retire[kind[j]].fd);
				sc->retire[kind[j]].fd = -1;
				qs_displayed_locked(sc, sc->retire[kind[j]].frame);
			}
		}
		while (sc->retire_head != sc->retire_tail && sc->retire[sc->retire_head % slots].fd < 0)
			sc->retire_head++;
		pthread_cond_broadcast(&sc->cond);
	}
	pthread_mutex_unlock(&sc->mutex);
	return NULL;
}

static void qs_start(struct qd_swapchain *sc)
{
	if (pipe2(sc->wake_fd, O_CLOEXEC | O_NONBLOCK) != 0)
		sc->wake_fd[0] = sc->wake_fd[1] = -1;
	sc->fence_thread_running = pthread_create(&sc->fence_thread, NULL, qs_fence_thread, sc) == 0;
}

//! After the completer and the fence thread have stopped.
static void qs_destroy(struct qd_swapchain *sc)
{
	uint32_t slots = sizeof(sc->retire) / sizeof(sc->retire[0]);
	for (uint32_t k = sc->retire_head; k != sc->retire_tail; k++)
		if (sc->retire[k % slots].fd >= 0)
			close(sc->retire[k % slots].fd);
	for (uint32_t i = 0; i < QD_MAX_IMAGES; i++) {
		if (sc->release_fence[i] >= 0)
			close(sc->release_fence[i]);
		for (int h = 0; h < 2; h++)
			if (sc->half_ahb[h][i])
				qs_ahb.release(sc->half_ahb[h][i]);
	}
	qs_twins_forget(sc);
	pthread_mutex_lock(&qs_scene_lock);
	for (unsigned i = 0; i < qs_nstwins;) {
		bool mine = false;
		for (uint32_t k = 0; k < QD_MAX_IMAGES; k++)
			mine |= qs_stwins[i].half == (uint64_t)sc->half_image[0][k] || qs_stwins[i].half == (uint64_t)sc->half_image[1][k];
		if (mine) {
			((PFN_vkDestroyFramebuffer)sc->d->gdpa(sc->device, "vkDestroyFramebuffer"))(sc->device, (VkFramebuffer)qs_stwins[i].twin, NULL);
			((PFN_vkDestroyImageView)sc->d->gdpa(sc->device, "vkDestroyImageView"))(sc->device, (VkImageView)qs_stwins[i].view, NULL);
			qs_stwins[i] = qs_stwins[--qs_nstwins];
		} else
			i++;
	}
	pthread_mutex_unlock(&qs_scene_lock);
	if (sc->mon_sock >= 0)
		close(sc->mon_sock);
	if (sc->wake_fd[0] >= 0) {
		close(sc->wake_fd[0]);
		close(sc->wake_fd[1]);
	}
	qs_remove_flag();
}
