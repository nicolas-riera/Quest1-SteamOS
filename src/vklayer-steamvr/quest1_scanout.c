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
#define QS_AHB_USAGE (0x100u /* GPU_SAMPLED_IMAGE */ | 0x200u /* GPU_COLOR_OUTPUT */ | 0x800u /* COMPOSER_OVERLAY */)

static struct
{
	int (*allocate)(const qs_ahb_desc *, AHardwareBuffer **);
	void (*describe)(const AHardwareBuffer *, qs_ahb_desc *);
	void (*release)(AHardwareBuffer *);
	const qs_native_handle *(*native_handle)(const AHardwareBuffer *);
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

// --- the right half in a buffer of its own --------------------------------------------------------
// vrcompositor draws each eye in its own render pass over its half of the display image. On the
// Adreno 540 blob, a render area at x = 1440 of a linear gralloc image comes out as copies of its
// first 320-px bin column, and is slow. So the pass of the right half renders into a 1440-wide
// image of its own at x = 0 instead (twin image view and framebuffer, viewports and scissors moved
// left), which the second display pipe scans out.

struct qs_twin
{
	uint64_t handle, twin; //!< app's image view / framebuffer -> ours on image2
	VkImage image;         //!< the app object's image (views) / unused (framebuffers)
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

//! Call with qs_twin_lock held: image2 of the direct swapchain image `image`, if it is one.
static VkImage qs_image2_of(VkImage image)
{
	for (unsigned s = 0; s < 4; s++)
		for (uint32_t i = 0; qs_direct_scs[s] && i < qs_direct_scs[s]->count; i++)
			if (qs_direct_scs[s]->images[i] == image)
				return qs_direct_scs[s]->image2[i];
	return VK_NULL_HANDLE;
}

static uint64_t qs_twin_find(struct qs_twin *t, unsigned n, uint64_t handle)
{
	for (unsigned i = 0; i < n; i++)
		if (t[i].handle == handle)
			return t[i].twin;
	return 0;
}

static VKAPI_ATTR VkResult VKAPI_CALL qs_CreateImageView(VkDevice device, const VkImageViewCreateInfo *ci,
                                                        const VkAllocationCallbacks *alloc, VkImageView *out)
{
	struct device *d = device_of(device);
	PFN_vkCreateImageView next = (PFN_vkCreateImageView)d->gdpa(device, "vkCreateImageView");
	VkResult r = next(device, ci, alloc, out);
	if (r != VK_SUCCESS)
		return r;
	pthread_mutex_lock(&qs_twin_lock);
	VkImage image2 = qs_image2_of(ci->image);
	if (image2 && qs_nviews < sizeof(qs_views) / sizeof(qs_views[0])) {
		VkImageViewCreateInfo c = *ci;
		c.image = image2;
		VkImageView twin;
		if (next(device, &c, alloc, &twin) == VK_SUCCESS)
			qs_views[qs_nviews++] = (struct qs_twin){(uint64_t)*out, (uint64_t)twin, ci->image};
	}
	pthread_mutex_unlock(&qs_twin_lock);
	return r;
}

static VKAPI_ATTR void VKAPI_CALL qs_DestroyImageView(VkDevice device, VkImageView view, const VkAllocationCallbacks *alloc)
{
	struct device *d = device_of(device);
	PFN_vkDestroyImageView next = (PFN_vkDestroyImageView)d->gdpa(device, "vkDestroyImageView");
	pthread_mutex_lock(&qs_twin_lock);
	for (unsigned i = 0; i < qs_nviews; i++)
		if (qs_views[i].handle == (uint64_t)view) {
			next(device, (VkImageView)qs_views[i].twin, alloc);
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
	VkResult r = next(device, ci, alloc, out);
	if (r != VK_SUCCESS || ci->attachmentCount != 1 || ci->width != 2 * QS_PANEL_W)
		return r;
	pthread_mutex_lock(&qs_twin_lock);
	uint64_t view2 = qs_twin_find(qs_views, qs_nviews, (uint64_t)ci->pAttachments[0]);
	if (view2 && qs_nfbs < sizeof(qs_fbs) / sizeof(qs_fbs[0])) {
		VkFramebufferCreateInfo c = *ci;
		VkImageView a = (VkImageView)view2;
		c.pAttachments = &a;
		c.width = QS_PANEL_W;
		VkFramebuffer twin;
		if (next(device, &c, alloc, &twin) == VK_SUCCESS)
			qs_fbs[qs_nfbs++] = (struct qs_twin){(uint64_t)*out, (uint64_t)twin, VK_NULL_HANDLE};
	}
	pthread_mutex_unlock(&qs_twin_lock);
	return r;
}

static VKAPI_ATTR void VKAPI_CALL qs_DestroyFramebuffer(VkDevice device, VkFramebuffer fb, const VkAllocationCallbacks *alloc)
{
	struct device *d = device_of(device);
	PFN_vkDestroyFramebuffer next = (PFN_vkDestroyFramebuffer)d->gdpa(device, "vkDestroyFramebuffer");
	pthread_mutex_lock(&qs_twin_lock);
	for (unsigned i = 0; i < qs_nfbs; i++)
		if (qs_fbs[i].handle == (uint64_t)fb) {
			next(device, (VkFramebuffer)qs_fbs[i].twin, alloc);
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

	VkImageCreateInfo ici2 = ici;
	ici2.extent.width = QS_PANEL_W;
	for (uint32_t i = 0; i < sc->count; i++) {
		sc->release_fence[i] = -1;
		sc->buffer_fd2[i] = -1;
		VkResult r = qs_import(sc, &ici, &sc->ahb[i], &sc->images[i], &sc->memory[i], &sc->buffer_fd[i],
		                       &sc->stride_px[i], &sc->ubwc[i]);
		if (r == VK_SUCCESS)
			r = qs_import(sc, &ici2, &sc->ahb2[i], &sc->image2[i], &sc->memory2[i], &sc->buffer_fd2[i],
			              &sc->stride2_px[i], &sc->ubwc2[i]);
		if (r != VK_SUCCESS) {
			LOG("direct scanout: image %u: %d\n", i, r);
			return r;
		}
	}
	qs_twins_register(sc);
	LOG("direct scanout: %u gralloc buffers %ux%u + %ux%u for the right half (stride %u px, %s)\n", sc->count,
	    ci->imageExtent.width, ci->imageExtent.height, QS_PANEL_W, ci->imageExtent.height, sc->stride_px[0],
	    sc->ubwc[0] ? "UBWC" : "linear");
	return VK_SUCCESS;
}

// debug: `runuser -u steamos -- touch /tmp/quest1-snap` writes the next frame to /tmp/quest1-snap.ppm
static void qs_snapshot(struct qd_swapchain *sc, uint32_t idx)
{
	if (unlink("/tmp/quest1-snap") != 0) // a trigger we cannot remove (root's, in sticky /tmp) is ignored
		return;
	uint32_t w = sc->ici.extent.width, h = sc->ici.extent.height, stride = sc->stride_px[idx] * 4;
	uint32_t stride2 = sc->stride2_px[idx] * 4;
	if (sc->ubwc[idx] || (sc->ahb2[idx] && sc->ubwc2[idx])) {
		LOG("direct scanout: snapshot of a UBWC buffer not supported\n");
		return;
	}
	// the right half from image2 when there is one (what the panels show)
	uint8_t *p = mmap(NULL, (size_t)stride * h, PROT_READ, MAP_SHARED, sc->buffer_fd[idx], 0);
	uint8_t *p2 = sc->ahb2[idx] ? mmap(NULL, (size_t)stride2 * h, PROT_READ, MAP_SHARED, sc->buffer_fd2[idx], 0) : MAP_FAILED;
	FILE *f = p != MAP_FAILED ? fopen("/tmp/quest1-snap.ppm.tmp", "wb") : NULL;
	if (f) {
		fprintf(f, "P6\n%u %u\n255\n", w, h);
		uint8_t *row = malloc(w * 3);
		for (uint32_t y = 0; y < h; y++) {
			for (uint32_t x = 0; x < w; x++) {
				const uint8_t *src = p2 != MAP_FAILED && x >= QS_PANEL_W ? p2 + (size_t)y * stride2 + (x - QS_PANEL_W) * 4
				                                                       : p + (size_t)y * stride + x * 4;
				memcpy(row + x * 3, src, 3);
			}
			fwrite(row, 1, w * 3, f);
		}
		free(row);
		fclose(f);
		rename("/tmp/quest1-snap.ppm.tmp", "/tmp/quest1-snap.ppm");
	}
	if (p != MAP_FAILED)
		munmap(p, (size_t)stride * h);
	if (p2 != MAP_FAILED)
		munmap(p2, (size_t)stride2 * h);
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
	} cbs[8];
	char label[QS_TS_SLOTS][64];
} qs_ts;
static pthread_mutex_t qs_ts_lock = PTHREAD_MUTEX_INITIALIZER;

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
static void qs_ts_frame(void)
{
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

static void qs_ts_begin_pass(VkCommandBuffer cb, const VkRect2D *area)
{
	if (qs_ts.state != 1)
		return;
	pthread_mutex_lock(&qs_ts_lock);
	int e = qs_ts_entry(cb, true);
	if (e >= 0) {
		if (qs_ts.cbs[e].count == 0)
			qs_ts_stamp(cb, e, "start");
		qs_ts.cbs[e].area = *area;
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
		char label[64];
		const VkRect2D *a = &qs_ts.cbs[e].area;
		snprintf(label, sizeof(label), "pass %ux%u@%d,%d, %u vertices", a->extent.width, a->extent.height, a->offset.x,
		         a->offset.y, qs_ts.cbs[e].indices);
		qs_ts_stamp(cb, e, label);
	}
	pthread_mutex_unlock(&qs_ts_lock);
}

#define QS_TRACE_NEXT(type, fn) \
	static type next; \
	if (!next) \
		next = (type)device_of(cb)->gdpa(device_of(cb)->handle, fn);

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
	qs_ts_begin_pass(cb, &a);
	bool eye_pass = a.extent.width == QS_PANEL_W && a.extent.height == QS_PANEL_H && a.offset.y == 0 &&
	                (a.offset.x == 0 || a.offset.x == QS_PANEL_W);
	pthread_mutex_lock(&qs_twin_lock);
	VkFramebuffer twin = eye_pass ? (VkFramebuffer)qs_twin_find(qs_fbs, qs_nfbs, (uint64_t)bi->framebuffer) : VK_NULL_HANDLE;
	pthread_mutex_unlock(&qs_twin_lock);
	if (twin) { // the left half renders fine where it is, the right half goes to image2
		if (a.offset.x == 0) {
			next(cb, bi, contents);
			return;
		}
		VkRenderPassBeginInfo b = *bi;
		b.framebuffer = twin;
		b.renderArea.offset.x = 0;
		next(cb, &b, contents);
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
	if (qs_trace_left > 0 && qs_trace_left--)
		LOG("trace: cb %p Dispatch %u %u %u\n", (void *)cb, x, y, z);
	next(cb, x, y, z);
}

static VKAPI_ATTR void VKAPI_CALL qs_CmdDispatchBase(VkCommandBuffer cb, uint32_t bx, uint32_t by, uint32_t bz,
                                                    uint32_t x, uint32_t y, uint32_t z)
{
	QS_TRACE_NEXT(PFN_vkCmdDispatchBase, "vkCmdDispatchBase")
	if (qs_trace_left > 0 && qs_trace_left--)
		LOG("trace: cb %p DispatchBase %u %u %u + %u %u %u\n", (void *)cb, bx, by, bz, x, y, z);
	next(cb, bx, by, bz, x, y, z);
}

static VKAPI_ATTR void VKAPI_CALL qs_CmdDraw(VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi)
{
	QS_TRACE_NEXT(PFN_vkCmdDraw, "vkCmdDraw")
	if (qs_trace_left > 0 && qs_trace_left--)
		LOG("trace: cb %p Draw %u x%u\n", (void *)cb, vc, ic);
	qs_ts_draw(cb, vc * ic);
	next(cb, vc, ic, fv, fi);
}

static VKAPI_ATTR void VKAPI_CALL qs_CmdDrawIndexed(VkCommandBuffer cb, uint32_t n, uint32_t ic, uint32_t fi, int32_t vo,
                                                   uint32_t fin)
{
	QS_TRACE_NEXT(PFN_vkCmdDrawIndexed, "vkCmdDrawIndexed")
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
	next(cb, src, dst, dep, nm, m, nb, b, ni, im);
}

static VKAPI_ATTR void VKAPI_CALL qs_CmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer buf, VkImage img, VkImageLayout l,
                                                         uint32_t n, const VkBufferImageCopy *r)
{
	QS_TRACE_NEXT(PFN_vkCmdCopyBufferToImage, "vkCmdCopyBufferToImage")
	if (qs_trace_left > 0 && qs_trace_left--)
		LOG("trace: cb %p CopyBufferToImage %p %ux%u\n", (void *)cb, (void *)img, n ? r[0].imageExtent.width : 0,
		    n ? r[0].imageExtent.height : 0);
	next(cb, buf, img, l, n, r);
}

static VKAPI_ATTR void VKAPI_CALL qs_CmdCopyImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d,
                                                 VkImageLayout dl, uint32_t n, const VkImageCopy *r)
{
	QS_TRACE_NEXT(PFN_vkCmdCopyImage, "vkCmdCopyImage")
	if (qs_trace_left > 0 && qs_trace_left--)
		LOG("trace: cb %p CopyImage %p -> %p %ux%u\n", (void *)cb, (void *)s, (void *)d, n ? r[0].extent.width : 0,
		    n ? r[0].extent.height : 0);
	next(cb, s, sl, d, dl, n, r);
}

static VKAPI_ATTR void VKAPI_CALL qs_CmdBlitImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d,
                                                 VkImageLayout dl, uint32_t n, const VkImageBlit *r, VkFilter f)
{
	QS_TRACE_NEXT(PFN_vkCmdBlitImage, "vkCmdBlitImage")
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
}

static VKAPI_ATTR void VKAPI_CALL qs_CmdPushConstants(VkCommandBuffer cb, VkPipelineLayout pl, VkShaderStageFlags st,
                                                     uint32_t off, uint32_t size, const void *v)
{
	QS_TRACE_NEXT(PFN_vkCmdPushConstants, "vkCmdPushConstants")
	if (qs_trace_left > 0 && qs_trace_left--)
		LOG("trace: cb %p PushConstants %u+%u\n", (void *)cb, off, size);
	next(cb, pl, st, off, size, v);
}

static PFN_vkVoidFunction qs_trace_proc(const char *name)
{
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
static uint32_t qs_mdp_format(const struct qd_swapchain *sc, uint32_t idx)
{
	bool bgra = sc->ici.format == VK_FORMAT_B8G8R8A8_UNORM || sc->ici.format == VK_FORMAT_B8G8R8A8_SRGB;
	if (sc->ubwc[idx])
		return MDP_RGBA_8888_UBWC; // no BGRA UBWC format: never happens with linear gralloc buffers
	return bgra ? MDP_BGRA_8888 : MDP_RGBA_8888;
}

// Completer thread, image `idx` rendered: put it on the panels. sc->mutex not held.
static void qs_commit(struct qd_swapchain *sc, uint32_t idx, uint64_t frame)
{
	qs_snapshot(sc, idx);
	qs_trace_arm();
	qs_ts_frame();
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
			// the right half comes from image2 (qs_CmdBeginRenderPass)
			bool own = hh == 1 && sc->ahb2[idx];
			l->src_rect = (struct mdp_rect){own ? 0 : hh * QS_PANEL_W, 0, QS_PANEL_W, sc->ici.extent.height};
			l->dst_rect = (struct mdp_rect){hh * QS_PANEL_W, 0, QS_PANEL_W, sc->ici.extent.height};
			l->buffer.width = own ? sc->stride2_px[idx] : sc->stride_px[idx];
			l->buffer.height = sc->ici.extent.height;
			l->buffer.format = qs_mdp_format(sc, idx);
			l->buffer.planes[0].fd = own ? sc->buffer_fd2[idx] : sc->buffer_fd[idx];
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
			if (!qs_flag_written) {
				FILE *f = fopen(QS_FLAG_FILE, "w");
				if (f) {
					fprintf(f, "%d\n", (int)getpid());
					fclose(f);
					qs_flag_written = true;
					LOG("direct scanout: on the panels (Monado pauses its compositing)\n");
				}
			}
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
	uint32_t slots = sizeof(sc->retire) / sizeof(sc->retire[0]);
	pthread_mutex_lock(&sc->mutex);
	while (!sc->stopping) {
		struct pollfd p[1 + 8 + QD_MAX_IMAGES];
		int kind[1 + 8 + QD_MAX_IMAGES]; // -1 wake, 0..7 retire slot, 100+i release of image i
		int n = 0;
		p[n] = (struct pollfd){sc->wake_fd[0], POLLIN, 0};
		kind[n++] = -1;
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
			if (kind[j] < 0) {
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
		if (sc->ahb[i])
			qs_ahb.release(sc->ahb[i]);
		if (sc->ahb2[i])
			qs_ahb.release(sc->ahb2[i]);
	}
	qs_twins_forget(sc);
	if (sc->wake_fd[0] >= 0) {
		close(sc->wake_fd[0]);
		close(sc->wake_fd[1]);
	}
	qs_remove_flag();
}
