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
//
// This file has the setup (the gralloc allocator, the scene mode, image import); the rest is in parts it includes at
// its end, in this order: quest1_scanout_twins.c (each half of the display image as an image of its own),
// quest1_scanout_images.c (the swapchain's images, snapshots), quest1_scanout_trace.c (debug traces, GPU timestamps),
// quest1_scanout_passes.c (render passes: the merge, the scene's twins), quest1_scanout_cmds.c (the command hooks, the
// HUD) and quest1_scanout_commit.c (the frames to the panels through Monado). One translation unit: split for reading,
// the binary is the same (checked: byte for byte, 2026-11-17).
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

// Steam Link VR (vrlink): vrcompositor draws its frames in the distortion pass only (quest1_modifier.c copies them as
// RGBA for it), which scene mode drops: the panels showed only what the eye passes drew (the headset's own dashboard,
// over black, 2026-11-11). So while a frame of vrlink came in the last second, the eyes render into vrcompositor's own
// images and its distortion pass runs into the eye's buffer: its distortion is an identity here (driver_quest1), so
// stretched from the panel's size to the buffer's (viewports, scissors) it gives what an eye render would have been.
// That holds while vrlink runs, not only while its frames come: with the headset's dashboard open it submitted none
// for seconds, the layer went back to scene mode, and the stream went black behind the dashboard (2026-11-12).
static _Atomic uint64_t qs_stream_ns;
static uint64_t qcp_vrlink_alive_ns(void); // quest1_modifier.c

static bool qs_streaming(void)
{
	uint64_t t = atomic_load(&qs_stream_ns), now = now_ns(), alive = qcp_vrlink_alive_ns();
	return (t && now - t < 1000000000ull) || (alive && now - alive < 2500000000ull);
}

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
	bool stream;           //!< the frame is rendered while streaming (into vrcompositor's images)
	unsigned runs;         //!< eyes begun in this recording
	// the eye's latest pass into its gralloc buffer (quest1_hud.c goes out of it and back)
	uint64_t cur_rp, cur_fb, app_fb; //!< the app's render pass, the twin framebuffer, the app's framebuffer
	VkRect2D area;
	bool hud_cleared; //!< the eye's buffer of head-locked overlays was cleared in this run
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

// The rest, in this order (one translation unit: quest1_compat.c includes quest1_display.c, which includes this):
#include "quest1_scanout_twins.c"
#include "quest1_scanout_images.c"
#include "quest1_scanout_trace.c"
#include "quest1_scanout_passes.c"
#include "quest1_scanout_cmds.c"
#include "quest1_scanout_commit.c"
