// quest1_wsi_x11: X11 presentation for Vulkan apps on the Quest 1 Adreno blob (libhybris).
//
// The Android Vulkan loader under libhybris only has Android/Wayland WSI, so X11 clients (the Steam
// webhelper's ANGLE Vulkan backend, vkcube...) cannot present. This implements VK_KHR_xcb_surface,
// VK_KHR_xlib_surface and VK_KHR_swapchain for those surfaces with a CPU copy, like Mesa's software
// X11 WSI:
//   - swapchain images are ordinary optimal-tiled device images (plus TRANSFER_SRC usage);
//   - vkQueuePresentKHR submits a pre-recorded copy into a host-visible buffer (waiting on the
//     app's semaphores) and hands the image to a per-swapchain thread, which waits for the copy,
//     pushes the pixels to the window (MIT-SHM, or PutImage strips) and syncs with the X server;
//   - vkAcquireNextImageKHR returns an image whose copy and upload are done and signals the
//     semaphore/fence with an empty submission on the device's first queue.
// Queue submissions of the application go through a mutex because acquire submits on a queue
// the application does not pass in.
// Everything else (Wayland surfaces, other devices) passes through. Enabled by QUEST1_VKSHIM_WSI=1.
#define _GNU_SOURCE
#define VK_USE_PLATFORM_XCB_KHR
#define VK_USE_PLATFORM_XLIB_KHR
#include "quest1_wsi.h"

#include <xcb/shm.h>
#include <xcb/xcb.h>

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <time.h>

#define LOG(...) fprintf(stderr, "quest1_wsi: " __VA_ARGS__)

const char *const wsi_instance_exts[] = {VK_KHR_XCB_SURFACE_EXTENSION_NAME, VK_KHR_XLIB_SURFACE_EXTENSION_NAME};
const unsigned wsi_instance_ext_count = 2;

int wsi_enabled(void)
{
	static int v = -1;
	if (v < 0) {
		const char *e = getenv("QUEST1_VKSHIM_WSI");
		v = e && atoi(e) > 0;
	}
	return v;
}

static PFN_vkGetInstanceProcAddr gipa_down;
static VkInstance g_instance;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER; // object lists
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER; // queue external synchronization

void wsi_set_down(PFN_vkGetInstanceProcAddr gipa) { gipa_down = gipa; }
void wsi_instance_created(VkInstance instance)
{
	if (!g_instance)
		g_instance = instance;
}

// instance-level commands of the next element, for objects that are not ours
#define DOWN(type, name) ((type)gipa_down(g_instance, name))

// --- devices --------------------------------------------------------------------------------------

struct dev {
	VkDevice device;
	VkPhysicalDevice pd;
	VkPhysicalDeviceMemoryProperties mp;
	uint32_t qfam;
	VkQueue queue;
	PFN_vkGetDeviceProcAddr gdpa;
#define DEVFN(n) PFN_vk##n n
	DEVFN(DestroyDevice);
	DEVFN(GetDeviceQueue);
	DEVFN(QueueSubmit);
	DEVFN(CreateImage);
	DEVFN(DestroyImage);
	DEVFN(GetImageMemoryRequirements);
	DEVFN(BindImageMemory);
	DEVFN(CreateBuffer);
	DEVFN(DestroyBuffer);
	DEVFN(GetBufferMemoryRequirements);
	DEVFN(BindBufferMemory);
	DEVFN(AllocateMemory);
	DEVFN(FreeMemory);
	DEVFN(MapMemory);
	DEVFN(InvalidateMappedMemoryRanges);
	DEVFN(CreateCommandPool);
	DEVFN(DestroyCommandPool);
	DEVFN(AllocateCommandBuffers);
	DEVFN(BeginCommandBuffer);
	DEVFN(EndCommandBuffer);
	DEVFN(CmdPipelineBarrier);
	DEVFN(CmdCopyImageToBuffer);
	DEVFN(CreateFence);
	DEVFN(DestroyFence);
	DEVFN(WaitForFences);
	DEVFN(ResetFences);
	DEVFN(QueueWaitIdle);
#undef DEVFN
	struct dev *next;
};
static struct dev *devices;

// queue commands of the next element (the same for every device of the driver)
static PFN_vkQueueSubmit down_QueueSubmit;
static PFN_vkQueueWaitIdle down_QueueWaitIdle;
static PFN_vkQueueBindSparse down_QueueBindSparse;
static PFN_vkQueuePresentKHR down_QueuePresentKHR;

void wsi_device_created(VkInstance instance, VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, VkDevice device,
                        PFN_vkGetDeviceProcAddr gdpa)
{
	if (getenv("QUEST1_VKSHIM_DEBUG"))
		LOG("device %p created on physical device %p (instance %p)\n", (void *)device, (void *)pd,
		    (void *)instance);
	struct dev *d = calloc(1, sizeof(*d));
	d->device = device;
	d->pd = pd;
	d->gdpa = gdpa;
	((PFN_vkGetPhysicalDeviceMemoryProperties)gipa_down(instance, "vkGetPhysicalDeviceMemoryProperties"))(pd, &d->mp);
#define L(n) d->n = (PFN_vk##n)gdpa(device, "vk" #n)
	L(DestroyDevice);
	L(GetDeviceQueue);
	L(QueueSubmit);
	L(CreateImage);
	L(DestroyImage);
	L(GetImageMemoryRequirements);
	L(BindImageMemory);
	L(CreateBuffer);
	L(DestroyBuffer);
	L(GetBufferMemoryRequirements);
	L(BindBufferMemory);
	L(AllocateMemory);
	L(FreeMemory);
	L(MapMemory);
	L(InvalidateMappedMemoryRanges);
	L(CreateCommandPool);
	L(DestroyCommandPool);
	L(AllocateCommandBuffers);
	L(BeginCommandBuffer);
	L(EndCommandBuffer);
	L(CmdPipelineBarrier);
	L(CmdCopyImageToBuffer);
	L(CreateFence);
	L(DestroyFence);
	L(WaitForFences);
	L(ResetFences);
	L(QueueWaitIdle);
#undef L
	d->qfam = ci->queueCreateInfoCount ? ci->pQueueCreateInfos[0].queueFamilyIndex : 0;
	// ANGLE creates its queue with VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT when the device supports
	// protected memory; such queues are only returned by vkGetDeviceQueue2 with the same flags
	// (vkGetDeviceQueue gives NULL, and the Android loader then crashes on it)
	VkDeviceQueueCreateFlags qflags = ci->queueCreateInfoCount ? ci->pQueueCreateInfos[0].flags : 0;
	PFN_vkGetDeviceQueue2 gdq2 = (PFN_vkGetDeviceQueue2)gdpa(device, "vkGetDeviceQueue2");
	if (qflags && gdq2) {
		VkDeviceQueueInfo2 qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2, NULL, qflags, d->qfam, 0};
		gdq2(device, &qi, &d->queue);
	} else {
		d->GetDeviceQueue(device, d->qfam, 0, &d->queue);
	}
	if (getenv("QUEST1_VKSHIM_DEBUG"))
		LOG("presentation queue %p (family %u, flags 0x%x)\n", (void *)d->queue, d->qfam, qflags);
	pthread_mutex_lock(&g_lock);
	if (!down_QueueSubmit) {
		down_QueueSubmit = d->QueueSubmit;
		down_QueueWaitIdle = d->QueueWaitIdle;
		down_QueueBindSparse = (PFN_vkQueueBindSparse)gdpa(device, "vkQueueBindSparse");
		down_QueuePresentKHR = (PFN_vkQueuePresentKHR)gdpa(device, "vkQueuePresentKHR");
	}
	d->next = devices;
	devices = d;
	pthread_mutex_unlock(&g_lock);
}

static struct dev *find_dev(VkDevice device)
{
	pthread_mutex_lock(&g_lock);
	struct dev *d = devices;
	while (d && d->device != device)
		d = d->next;
	pthread_mutex_unlock(&g_lock);
	return d;
}

static VKAPI_ATTR void VKAPI_CALL hook_DestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc)
{
	pthread_mutex_lock(&g_lock);
	struct dev **pp = &devices, *d = NULL;
	while (*pp && (*pp)->device != device)
		pp = &(*pp)->next;
	if (*pp) {
		d = *pp;
		*pp = d->next;
	}
	pthread_mutex_unlock(&g_lock);
	if (!d) { // not created through us
		DOWN(PFN_vkDestroyDevice, "vkDestroyDevice")(device, alloc);
		return;
	}
	d->DestroyDevice(device, alloc);
	free(d);
}

static VKAPI_ATTR VkResult VKAPI_CALL hook_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f)
{
	pthread_mutex_lock(&q_lock);
	VkResult r = down_QueueSubmit(q, n, s, f);
	pthread_mutex_unlock(&q_lock);
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL hook_QueueWaitIdle(VkQueue q)
{
	pthread_mutex_lock(&q_lock);
	VkResult r = down_QueueWaitIdle(q);
	pthread_mutex_unlock(&q_lock);
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL hook_QueueBindSparse(VkQueue q, uint32_t n, const VkBindSparseInfo *b, VkFence f)
{
	pthread_mutex_lock(&q_lock);
	VkResult r = down_QueueBindSparse(q, n, b, f);
	pthread_mutex_unlock(&q_lock);
	return r;
}

// --- surfaces -------------------------------------------------------------------------------------

#define SURF_MAGIC 0x51315853u
struct surf {
	uint32_t magic;
	xcb_connection_t *conn;
	xcb_window_t win;
	struct surf *next;
};
static struct surf *surfaces;

static struct surf *find_surf(VkSurfaceKHR s)
{
	pthread_mutex_lock(&g_lock);
	struct surf *p = surfaces;
	while (p && (VkSurfaceKHR)(uintptr_t)p != s)
		p = p->next;
	pthread_mutex_unlock(&g_lock);
	return p;
}

static VkResult new_surface(xcb_connection_t *conn, xcb_window_t win, VkSurfaceKHR *out)
{
	if (!conn)
		return VK_ERROR_INITIALIZATION_FAILED;
	struct surf *s = calloc(1, sizeof(*s));
	if (!s)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	s->magic = SURF_MAGIC;
	s->conn = conn;
	s->win = win;
	pthread_mutex_lock(&g_lock);
	s->next = surfaces;
	surfaces = s;
	pthread_mutex_unlock(&g_lock);
	*out = (VkSurfaceKHR)(uintptr_t)s;
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL hook_CreateXcbSurfaceKHR(VkInstance inst, const VkXcbSurfaceCreateInfoKHR *ci,
                                                              const VkAllocationCallbacks *a, VkSurfaceKHR *out)
{
	return new_surface(ci->connection, ci->window, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL hook_CreateXlibSurfaceKHR(VkInstance inst, const VkXlibSurfaceCreateInfoKHR *ci,
                                                               const VkAllocationCallbacks *a, VkSurfaceKHR *out)
{
	static xcb_connection_t *(*get_xcb)(Display *);
	if (!get_xcb) {
		void *h = dlopen("libX11-xcb.so.1", RTLD_NOW | RTLD_LOCAL);
		get_xcb = h ? (xcb_connection_t * (*)(Display *)) dlsym(h, "XGetXCBConnection") : NULL;
		if (!get_xcb) {
			LOG("no XGetXCBConnection (libX11-xcb.so.1)\n");
			return VK_ERROR_INITIALIZATION_FAILED;
		}
	}
	return new_surface(get_xcb(ci->dpy), (xcb_window_t)ci->window, out);
}

static VKAPI_ATTR void VKAPI_CALL hook_DestroySurfaceKHR(VkInstance inst, VkSurfaceKHR surface,
                                                         const VkAllocationCallbacks *a)
{
	if (!surface)
		return;
	pthread_mutex_lock(&g_lock);
	struct surf **pp = &surfaces;
	while (*pp && (VkSurfaceKHR)(uintptr_t)*pp != surface)
		pp = &(*pp)->next;
	struct surf *s = *pp;
	if (s)
		*pp = s->next;
	pthread_mutex_unlock(&g_lock);
	if (s) {
		free(s);
		return;
	}
	((PFN_vkDestroySurfaceKHR)gipa_down(inst, "vkDestroySurfaceKHR"))(inst, surface, a);
}

static VKAPI_ATTR VkBool32 VKAPI_CALL hook_GetPhysicalDeviceXcbPresentationSupportKHR(VkPhysicalDevice pd, uint32_t qf,
                                                                                     xcb_connection_t *c,
                                                                                     xcb_visualid_t v)
{
	return VK_TRUE;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL hook_GetPhysicalDeviceXlibPresentationSupportKHR(VkPhysicalDevice pd, uint32_t qf,
                                                                                      Display *dpy, VisualID v)
{
	return VK_TRUE;
}

static VKAPI_ATTR VkResult VKAPI_CALL hook_GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice pd, uint32_t qf,
                                                                             VkSurfaceKHR surface, VkBool32 *sup)
{
	if (!find_surf(surface))
		return DOWN(PFN_vkGetPhysicalDeviceSurfaceSupportKHR, "vkGetPhysicalDeviceSurfaceSupportKHR")(pd, qf, surface,
		                                                                                               sup);
	*sup = VK_TRUE;
	return VK_SUCCESS;
}

// window size and depth; 0 on failure (window gone)
static int geometry(struct surf *s, uint32_t *w, uint32_t *h, uint8_t *depth)
{
	xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(s->conn, xcb_get_geometry(s->conn, s->win), NULL);
	if (!g)
		return 0;
	*w = g->width;
	*h = g->height;
	if (depth)
		*depth = g->depth;
	free(g);
	return 1;
}

static void fill_caps(struct surf *s, VkSurfaceCapabilitiesKHR *c)
{
	uint32_t w = 0, h = 0;
	memset(c, 0, sizeof(*c));
	c->minImageCount = 2;
	c->maxImageCount = 6;
	if (geometry(s, &w, &h, NULL)) {
		c->currentExtent = (VkExtent2D){w, h};
		c->minImageExtent = c->maxImageExtent = c->currentExtent;
	} else {
		c->currentExtent = (VkExtent2D){UINT32_MAX, UINT32_MAX};
		c->minImageExtent = (VkExtent2D){1, 1};
		c->maxImageExtent = (VkExtent2D){16384, 16384};
	}
	c->maxImageArrayLayers = 1;
	c->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	c->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	c->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR | VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
	c->supportedUsageFlags = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
	                         VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
}

static VKAPI_ATTR VkResult VKAPI_CALL hook_GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice pd,
                                                                                  VkSurfaceKHR surface,
                                                                                  VkSurfaceCapabilitiesKHR *caps)
{
	struct surf *s = find_surf(surface);
	if (!s)
		return DOWN(PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")(
		    pd, surface, caps);
	fill_caps(s, caps);
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL hook_GetPhysicalDeviceSurfaceCapabilities2KHR(
    VkPhysicalDevice pd, const VkPhysicalDeviceSurfaceInfo2KHR *info, VkSurfaceCapabilities2KHR *caps)
{
	struct surf *s = find_surf(info->surface);
	if (!s)
		return DOWN(PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR, "vkGetPhysicalDeviceSurfaceCapabilities2KHR")(
		    pd, info, caps);
	fill_caps(s, &caps->surfaceCapabilities);
	for (VkBaseOutStructure *p = caps->pNext; p; p = p->pNext) {
		if (p->sType == VK_STRUCTURE_TYPE_SHARED_PRESENT_SURFACE_CAPABILITIES_KHR)
			((VkSharedPresentSurfaceCapabilitiesKHR *)p)->sharedPresentSupportedUsageFlags = 0;
		else if (p->sType == VK_STRUCTURE_TYPE_SURFACE_PROTECTED_CAPABILITIES_KHR)
			((VkSurfaceProtectedCapabilitiesKHR *)p)->supportsProtected = VK_FALSE;
	}
	return VK_SUCCESS;
}

static const VkSurfaceFormatKHR formats[] = {
    {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
};
#define NFORMATS (sizeof(formats) / sizeof(formats[0]))

static VKAPI_ATTR VkResult VKAPI_CALL hook_GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice pd, VkSurfaceKHR surface,
                                                                             uint32_t *count, VkSurfaceFormatKHR *out)
{
	if (!find_surf(surface))
		return DOWN(PFN_vkGetPhysicalDeviceSurfaceFormatsKHR, "vkGetPhysicalDeviceSurfaceFormatsKHR")(pd, surface,
		                                                                                               count, out);
	if (!out) {
		*count = NFORMATS;
		return VK_SUCCESS;
	}
	uint32_t n = *count < NFORMATS ? *count : NFORMATS;
	memcpy(out, formats, n * sizeof(*out));
	*count = n;
	return n < NFORMATS ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL hook_GetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice pd,
                                                                              const VkPhysicalDeviceSurfaceInfo2KHR *info,
                                                                              uint32_t *count, VkSurfaceFormat2KHR *out)
{
	if (!find_surf(info->surface))
		return DOWN(PFN_vkGetPhysicalDeviceSurfaceFormats2KHR, "vkGetPhysicalDeviceSurfaceFormats2KHR")(pd, info,
		                                                                                                 count, out);
	if (!out) {
		*count = NFORMATS;
		return VK_SUCCESS;
	}
	uint32_t n = *count < NFORMATS ? *count : NFORMATS;
	for (uint32_t i = 0; i < n; i++)
		out[i].surfaceFormat = formats[i];
	*count = n;
	return n < NFORMATS ? VK_INCOMPLETE : VK_SUCCESS;
}

static const VkPresentModeKHR modes[] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR,
                                         VK_PRESENT_MODE_MAILBOX_KHR};
#define NMODES (sizeof(modes) / sizeof(modes[0]))

static VKAPI_ATTR VkResult VKAPI_CALL hook_GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice pd,
                                                                                  VkSurfaceKHR surface, uint32_t *count,
                                                                                  VkPresentModeKHR *out)
{
	if (!find_surf(surface))
		return DOWN(PFN_vkGetPhysicalDeviceSurfacePresentModesKHR, "vkGetPhysicalDeviceSurfacePresentModesKHR")(
		    pd, surface, count, out);
	if (!out) {
		*count = NMODES;
		return VK_SUCCESS;
	}
	uint32_t n = *count < NMODES ? *count : NMODES;
	memcpy(out, modes, n * sizeof(*out));
	*count = n;
	return n < NMODES ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL hook_GetPhysicalDevicePresentRectanglesKHR(VkPhysicalDevice pd,
                                                                                VkSurfaceKHR surface, uint32_t *count,
                                                                                VkRect2D *rects)
{
	struct surf *s = find_surf(surface);
	if (!s)
		return DOWN(PFN_vkGetPhysicalDevicePresentRectanglesKHR, "vkGetPhysicalDevicePresentRectanglesKHR")(
		    pd, surface, count, rects);
	if (!rects) {
		*count = 1;
		return VK_SUCCESS;
	}
	if (*count < 1)
		return VK_INCOMPLETE;
	uint32_t w = 0, h = 0;
	geometry(s, &w, &h, NULL);
	rects[0] = (VkRect2D){{0, 0}, {w, h}};
	*count = 1;
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL hook_GetDeviceGroupSurfacePresentModesKHR(VkDevice device, VkSurfaceKHR surface,
                                                                               VkDeviceGroupPresentModeFlagsKHR *m)
{
	if (!find_surf(surface)) {
		struct dev *d = find_dev(device);
		PFN_vkGetDeviceGroupSurfacePresentModesKHR f =
		    d ? (PFN_vkGetDeviceGroupSurfacePresentModesKHR)d->gdpa(device, "vkGetDeviceGroupSurfacePresentModesKHR")
		      : NULL;
		return f ? f(device, surface, m) : VK_ERROR_SURFACE_LOST_KHR;
	}
	*m = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
	return VK_SUCCESS;
}

// --- swapchains -----------------------------------------------------------------------------------

#define SC_MAGIC 0x51315343u
#define MAX_IMAGES 8
enum { IMG_FREE, IMG_ACQUIRED, IMG_QUEUED };

struct img {
	VkImage image;
	VkDeviceMemory mem;
	VkBuffer buf;
	VkDeviceMemory bufmem;
	void *map;
	VkCommandBuffer cmd;
	VkFence fence;
	int state;
};

struct swapchain {
	uint32_t magic;
	struct dev *d;
	struct surf *s;
	VkExtent2D extent;
	uint8_t depth;
	uint32_t n;
	struct img img[MAX_IMAGES];
	VkCommandPool pool;
	int coherent;
	VkDeviceSize bufsize;
	// X side
	xcb_gcontext_t gc;
	xcb_shm_seg_t shmseg;
	void *shmaddr;
	// presentation thread
	pthread_t thread;
	pthread_mutex_t m;
	pthread_cond_t cv;
	uint32_t fifo[MAX_IMAGES];
	uint32_t head, tail;
	uint32_t next_acquire;
	int quit, thread_started;
	int suboptimal, out_of_date;
	uint64_t frames;
	struct swapchain *nextsc;
};
static struct swapchain *swapchains;

static struct swapchain *find_sc(VkSwapchainKHR h)
{
	pthread_mutex_lock(&g_lock);
	struct swapchain *p = swapchains;
	while (p && (VkSwapchainKHR)(uintptr_t)p != h)
		p = p->nextsc;
	pthread_mutex_unlock(&g_lock);
	return p;
}

static int mem_type(struct dev *d, uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags *got)
{
	for (uint32_t i = 0; i < d->mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (d->mp.memoryTypes[i].propertyFlags & want) == want) {
			if (got)
				*got = d->mp.memoryTypes[i].propertyFlags;
			return (int)i;
		}
	return -1;
}

static void put_frame(struct swapchain *sc, struct img *im)
{
	xcb_connection_t *c = sc->s->conn;
	uint32_t w = sc->extent.width, h = sc->extent.height, stride = w * 4;
	if (sc->shmaddr) {
		memcpy(sc->shmaddr, im->map, (size_t)stride * h);
		xcb_shm_put_image(c, sc->s->win, sc->gc, w, h, 0, 0, w, h, 0, 0, sc->depth, XCB_IMAGE_FORMAT_Z_PIXMAP, 0,
		                  sc->shmseg, 0);
	} else {
		uint32_t maxreq = xcb_get_maximum_request_length(c) * 4u;
		uint32_t rows = (maxreq - 64) / stride;
		if (rows < 1)
			rows = 1;
		for (uint32_t y = 0; y < h; y += rows) {
			uint32_t nr = h - y < rows ? h - y : rows;
			xcb_put_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP, sc->s->win, sc->gc, w, nr, 0, y, 0, sc->depth,
			              nr * stride, (const uint8_t *)im->map + (size_t)y * stride);
		}
	}
	// round trip: the server is done with the pixels, and the window size is current
	uint32_t ww, wh;
	if (!geometry(sc->s, &ww, &wh, NULL))
		sc->out_of_date = 1;
	else if (ww != w || wh != h)
		sc->suboptimal = 1;
}

static void *present_thread(void *arg)
{
	struct swapchain *sc = arg;
	struct dev *d = sc->d;
	pthread_mutex_lock(&sc->m);
	for (;;) {
		while (sc->head == sc->tail && !sc->quit)
			pthread_cond_wait(&sc->cv, &sc->m);
		if (sc->head == sc->tail)
			break;
		uint32_t i = sc->fifo[sc->head % MAX_IMAGES];
		sc->head++;
		pthread_mutex_unlock(&sc->m);

		struct img *im = &sc->img[i];
		d->WaitForFences(d->device, 1, &im->fence, VK_TRUE, UINT64_MAX);
		if (!sc->coherent) {
			VkMappedMemoryRange r = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, im->bufmem, 0, VK_WHOLE_SIZE};
			d->InvalidateMappedMemoryRanges(d->device, 1, &r);
		}
		if (!sc->out_of_date)
			put_frame(sc, im);

		if (getenv("QUEST1_VKSHIM_DEBUG") && (sc->frames < 10 || sc->frames % 300 == 0)) {
			const uint32_t *px = im->map;
			uint32_t nz = 0, ns = 0;
			for (uint32_t y = 0; y < sc->extent.height; y += 16)
				for (uint32_t x = 0; x < sc->extent.width; x += 16, ns++)
					nz += px[y * sc->extent.width + x] != 0;
			LOG("window 0x%x frame %llu (image %u): pixel(center) 0x%08x, %u/%u samples non-zero%s%s\n", sc->s->win,
			    (unsigned long long)sc->frames, i, px[(sc->extent.height / 2) * sc->extent.width + sc->extent.width / 2],
			    nz, ns, sc->out_of_date ? " out-of-date" : "", sc->suboptimal ? " suboptimal" : "");
		}
		pthread_mutex_lock(&sc->m);
		im->state = IMG_FREE;
		sc->frames++;
		pthread_cond_broadcast(&sc->cv);
	}
	pthread_mutex_unlock(&sc->m);
	return NULL;
}

static void destroy_sc(struct swapchain *sc)
{
	struct dev *d = sc->d;
	if (sc->thread_started) {
		pthread_mutex_lock(&sc->m);
		sc->quit = 1;
		pthread_cond_broadcast(&sc->cv);
		pthread_mutex_unlock(&sc->m);
		pthread_join(sc->thread, NULL);
	}
	for (uint32_t i = 0; i < sc->n; i++) {
		struct img *im = &sc->img[i];
		if (im->fence) {
			d->WaitForFences(d->device, 1, &im->fence, VK_TRUE, UINT64_MAX);
			d->DestroyFence(d->device, im->fence, NULL);
		}
		if (im->image)
			d->DestroyImage(d->device, im->image, NULL);
		if (im->mem)
			d->FreeMemory(d->device, im->mem, NULL);
		if (im->buf)
			d->DestroyBuffer(d->device, im->buf, NULL);
		if (im->bufmem)
			d->FreeMemory(d->device, im->bufmem, NULL);
	}
	if (sc->pool)
		d->DestroyCommandPool(d->device, sc->pool, NULL);
	if (sc->shmaddr) {
		xcb_shm_detach(sc->s->conn, sc->shmseg);
		shmdt(sc->shmaddr);
	}
	if (sc->gc)
		xcb_free_gc(sc->s->conn, sc->gc);
	xcb_flush(sc->s->conn);
	pthread_mutex_destroy(&sc->m);
	pthread_cond_destroy(&sc->cv);
	free(sc);
}

static void setup_shm(struct swapchain *sc)
{
	xcb_connection_t *c = sc->s->conn;
	if (getenv("QUEST1_WSI_NO_SHM"))
		return;
	const xcb_query_extension_reply_t *ext = xcb_get_extension_data(c, &xcb_shm_id);
	if (!ext || !ext->present)
		return;
	size_t size = (size_t)sc->extent.width * sc->extent.height * 4;
	int id = shmget(IPC_PRIVATE, size, IPC_CREAT | 0600);
	if (id < 0)
		return;
	void *addr = shmat(id, NULL, 0);
	if (addr == (void *)-1) {
		shmctl(id, IPC_RMID, NULL);
		return;
	}
	xcb_shm_seg_t seg = xcb_generate_id(c);
	xcb_generic_error_t *err = xcb_request_check(c, xcb_shm_attach_checked(c, seg, id, 0));
	shmctl(id, IPC_RMID, NULL); // freed once both sides detach
	if (err) {
		free(err);
		shmdt(addr);
		return;
	}
	sc->shmseg = seg;
	sc->shmaddr = addr;
}

static VKAPI_ATTR VkResult VKAPI_CALL hook_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *ci,
                                                             const VkAllocationCallbacks *alloc, VkSwapchainKHR *out)
{
	struct dev *d = find_dev(device);
	struct surf *s = find_surf(ci->surface);
	if (!s || !d) {
		PFN_vkCreateSwapchainKHR f =
		    d ? (PFN_vkCreateSwapchainKHR)d->gdpa(device, "vkCreateSwapchainKHR") : NULL;
		return f ? f(device, ci, alloc, out) : VK_ERROR_INITIALIZATION_FAILED;
	}
	struct swapchain *old = ci->oldSwapchain ? find_sc(ci->oldSwapchain) : NULL;
	if (old) {
		pthread_mutex_lock(&old->m);
		old->out_of_date = 1; // retired: no more acquires
		pthread_mutex_unlock(&old->m);
	}

	struct swapchain *sc = calloc(1, sizeof(*sc));
	if (!sc)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	sc->magic = SC_MAGIC;
	sc->d = d;
	sc->s = s;
	sc->extent = ci->imageExtent;
	pthread_mutex_init(&sc->m, NULL);
	pthread_condattr_t ca;
	pthread_condattr_init(&ca);
	pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
	pthread_cond_init(&sc->cv, &ca);
	pthread_condattr_destroy(&ca);

	uint32_t ww = 0, wh = 0;
	if (!geometry(s, &ww, &wh, &sc->depth)) {
		destroy_sc(sc);
		return VK_ERROR_SURFACE_LOST_KHR;
	}
	sc->n = ci->minImageCount < 2 ? 2 : ci->minImageCount;
	if (sc->n > MAX_IMAGES)
		sc->n = MAX_IMAGES;

	VkResult r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
	VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, d->qfam};
	if (d->CreateCommandPool(device, &pci, NULL, &sc->pool) != VK_SUCCESS)
		goto fail;
	VkCommandBuffer cmds[MAX_IMAGES];
	VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, sc->pool,
	                                   VK_COMMAND_BUFFER_LEVEL_PRIMARY, sc->n};
	if (d->AllocateCommandBuffers(device, &cai, cmds) != VK_SUCCESS)
		goto fail;

	sc->bufsize = (VkDeviceSize)sc->extent.width * sc->extent.height * 4;
	sc->coherent = 1;
	for (uint32_t i = 0; i < sc->n; i++) {
		struct img *im = &sc->img[i];
		im->cmd = cmds[i];
		VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		if (ci->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) {
			ici.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
			for (const VkBaseInStructure *p = ci->pNext; p; p = p->pNext)
				if (p->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO)
					ici.pNext = p; // single struct, its pNext is not followed by us
		}
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = ci->imageFormat;
		ici.extent = (VkExtent3D){sc->extent.width, sc->extent.height, 1};
		ici.mipLevels = 1;
		ici.arrayLayers = ci->imageArrayLayers ? ci->imageArrayLayers : 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = ci->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		ici.sharingMode = ci->imageSharingMode;
		ici.queueFamilyIndexCount = ci->queueFamilyIndexCount;
		ici.pQueueFamilyIndices = ci->pQueueFamilyIndices;
		ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VkImageFormatListCreateInfo fl;
		if (ici.pNext) { // copy without its own chain
			fl = *(const VkImageFormatListCreateInfo *)ici.pNext;
			fl.pNext = NULL;
			ici.pNext = &fl;
		}
		if ((r = d->CreateImage(device, &ici, NULL, &im->image)) != VK_SUCCESS)
			goto fail;
		VkMemoryRequirements mr;
		d->GetImageMemoryRequirements(device, im->image, &mr);
		int t = mem_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
		if (t < 0)
			t = mem_type(d, mr.memoryTypeBits, 0, NULL);
		VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, (uint32_t)t};
		if ((r = d->AllocateMemory(device, &mai, NULL, &im->mem)) != VK_SUCCESS)
			goto fail;
		if ((r = d->BindImageMemory(device, im->image, im->mem, 0)) != VK_SUCCESS)
			goto fail;

		VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, sc->bufsize,
		                          VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
		if ((r = d->CreateBuffer(device, &bci, NULL, &im->buf)) != VK_SUCCESS)
			goto fail;
		d->GetBufferMemoryRequirements(device, im->buf, &mr);
		VkMemoryPropertyFlags got = 0;
		t = mem_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
		             &got);
		if (t < 0)
			t = mem_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &got);
		if (t < 0) {
			r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
			goto fail;
		}
		if (!(got & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
			sc->coherent = 0;
		mai.allocationSize = mr.size;
		mai.memoryTypeIndex = (uint32_t)t;
		if ((r = d->AllocateMemory(device, &mai, NULL, &im->bufmem)) != VK_SUCCESS)
			goto fail;
		if ((r = d->BindBufferMemory(device, im->buf, im->bufmem, 0)) != VK_SUCCESS)
			goto fail;
		if ((r = d->MapMemory(device, im->bufmem, 0, VK_WHOLE_SIZE, 0, &im->map)) != VK_SUCCESS)
			goto fail;
		VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT};
		if ((r = d->CreateFence(device, &fci, NULL, &im->fence)) != VK_SUCCESS)
			goto fail;

		// copy the presented image into the buffer, give it back in PRESENT_SRC layout
		VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		d->BeginCommandBuffer(im->cmd, &bi);
		VkImageMemoryBarrier ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		                           NULL,
		                           VK_ACCESS_MEMORY_WRITE_BIT,
		                           VK_ACCESS_TRANSFER_READ_BIT,
		                           VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                           VK_QUEUE_FAMILY_IGNORED,
		                           VK_QUEUE_FAMILY_IGNORED,
		                           im->image,
		                           {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
		d->CmdPipelineBarrier(im->cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
		                      NULL, 0, NULL, 1, &ib);
		VkBufferImageCopy reg = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
		                         {sc->extent.width, sc->extent.height, 1}};
		d->CmdCopyImageToBuffer(im->cmd, im->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im->buf, 1, &reg);
		ib.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		ib.dstAccessMask = 0;
		ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		ib.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		VkBufferMemoryBarrier bb = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		                            NULL,
		                            VK_ACCESS_TRANSFER_WRITE_BIT,
		                            VK_ACCESS_HOST_READ_BIT,
		                            VK_QUEUE_FAMILY_IGNORED,
		                            VK_QUEUE_FAMILY_IGNORED,
		                            im->buf,
		                            0,
		                            VK_WHOLE_SIZE};
		d->CmdPipelineBarrier(im->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                      VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 1, &bb, 1,
		                      &ib);
		if ((r = d->EndCommandBuffer(im->cmd)) != VK_SUCCESS)
			goto fail;
		im->state = IMG_FREE;
	}

	xcb_connection_t *c = s->conn;
	sc->gc = xcb_generate_id(c);
	xcb_create_gc(c, sc->gc, s->win, 0, NULL);
	setup_shm(sc);
	xcb_flush(c);
	if (pthread_create(&sc->thread, NULL, present_thread, sc) != 0) {
		r = VK_ERROR_INITIALIZATION_FAILED;
		goto fail;
	}
	sc->thread_started = 1;
	if (ww != sc->extent.width || wh != sc->extent.height)
		sc->suboptimal = 1;

	pthread_mutex_lock(&g_lock);
	sc->nextsc = swapchains;
	swapchains = sc;
	pthread_mutex_unlock(&g_lock);
	*out = (VkSwapchainKHR)(uintptr_t)sc;
	LOG("swapchain %ux%u on window 0x%x, %u images, format %d, present mode %d, depth %u, %s\n", sc->extent.width,
	    sc->extent.height, s->win, sc->n, ci->imageFormat, ci->presentMode, sc->depth,
	    sc->shmaddr ? "MIT-SHM" : "PutImage");
	return VK_SUCCESS;
fail:
	LOG("swapchain creation failed (%d)\n", r);
	destroy_sc(sc);
	return r;
}

static VKAPI_ATTR void VKAPI_CALL hook_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR h,
                                                           const VkAllocationCallbacks *alloc)
{
	if (!h)
		return;
	pthread_mutex_lock(&g_lock);
	struct swapchain **pp = &swapchains;
	while (*pp && (VkSwapchainKHR)(uintptr_t)*pp != h)
		pp = &(*pp)->nextsc;
	struct swapchain *sc = *pp;
	if (sc)
		*pp = sc->nextsc;
	pthread_mutex_unlock(&g_lock);
	if (sc) {
		destroy_sc(sc);
		return;
	}
	struct dev *d = find_dev(device);
	PFN_vkDestroySwapchainKHR f = d ? (PFN_vkDestroySwapchainKHR)d->gdpa(device, "vkDestroySwapchainKHR") : NULL;
	if (f)
		f(device, h, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL hook_GetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR h, uint32_t *count,
                                                                VkImage *images)
{
	struct swapchain *sc = find_sc(h);
	if (!sc) {
		struct dev *d = find_dev(device);
		PFN_vkGetSwapchainImagesKHR f =
		    d ? (PFN_vkGetSwapchainImagesKHR)d->gdpa(device, "vkGetSwapchainImagesKHR") : NULL;
		return f ? f(device, h, count, images) : VK_ERROR_INITIALIZATION_FAILED;
	}
	if (!images) {
		*count = sc->n;
		return VK_SUCCESS;
	}
	uint32_t n = *count < sc->n ? *count : sc->n;
	for (uint32_t i = 0; i < n; i++)
		images[i] = sc->img[i].image;
	*count = n;
	return n < sc->n ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult acquire(struct swapchain *sc, uint64_t timeout, VkSemaphore sem, VkFence fence, uint32_t *index)
{
	struct timespec dl;
	clock_gettime(CLOCK_MONOTONIC, &dl);
	if (timeout != UINT64_MAX) {
		uint64_t ns = (uint64_t)dl.tv_nsec + timeout % 1000000000ull;
		dl.tv_sec += timeout / 1000000000ull + ns / 1000000000ull;
		dl.tv_nsec = ns % 1000000000ull;
	}
	pthread_mutex_lock(&sc->m);
	int found = -1;
	for (;;) {
		if (sc->out_of_date) {
			pthread_mutex_unlock(&sc->m);
			return VK_ERROR_OUT_OF_DATE_KHR;
		}
		for (uint32_t k = 0; k < sc->n; k++) {
			uint32_t i = (sc->next_acquire + k) % sc->n;
			if (sc->img[i].state == IMG_FREE) {
				found = (int)i;
				break;
			}
		}
		if (found >= 0)
			break;
		if (timeout == 0) {
			pthread_mutex_unlock(&sc->m);
			return VK_NOT_READY;
		}
		if (timeout == UINT64_MAX)
			pthread_cond_wait(&sc->cv, &sc->m);
		else if (pthread_cond_timedwait(&sc->cv, &sc->m, &dl) == ETIMEDOUT) {
			pthread_mutex_unlock(&sc->m);
			return VK_TIMEOUT;
		}
	}
	sc->img[found].state = IMG_ACQUIRED;
	sc->next_acquire = (uint32_t)found + 1;
	pthread_mutex_unlock(&sc->m);

	// the image is idle (its copy finished before it became free): just signal
	if (sem || fence) {
		VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
		si.signalSemaphoreCount = sem ? 1 : 0;
		si.pSignalSemaphores = &sem;
		pthread_mutex_lock(&q_lock);
		VkResult r = sc->d->QueueSubmit(sc->d->queue, 1, &si, fence);
		pthread_mutex_unlock(&q_lock);
		if (r != VK_SUCCESS)
			return r;
	}
	*index = (uint32_t)found;
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL hook_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR h, uint64_t timeout,
                                                              VkSemaphore sem, VkFence fence, uint32_t *index)
{
	struct swapchain *sc = find_sc(h);
	if (!sc) {
		struct dev *d = find_dev(device);
		PFN_vkAcquireNextImageKHR f = d ? (PFN_vkAcquireNextImageKHR)d->gdpa(device, "vkAcquireNextImageKHR") : NULL;
		return f ? f(device, h, timeout, sem, fence, index) : VK_ERROR_INITIALIZATION_FAILED;
	}
	return acquire(sc, timeout, sem, fence, index);
}

static VKAPI_ATTR VkResult VKAPI_CALL hook_AcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR *ai,
                                                               uint32_t *index)
{
	struct swapchain *sc = find_sc(ai->swapchain);
	if (!sc) {
		struct dev *d = find_dev(device);
		PFN_vkAcquireNextImage2KHR f =
		    d ? (PFN_vkAcquireNextImage2KHR)d->gdpa(device, "vkAcquireNextImage2KHR") : NULL;
		return f ? f(device, ai, index) : VK_ERROR_INITIALIZATION_FAILED;
	}
	return acquire(sc, ai->timeout, ai->semaphore, ai->fence, index);
}

static VKAPI_ATTR VkResult VKAPI_CALL hook_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pi)
{
	uint32_t ours = 0;
	for (uint32_t i = 0; i < pi->swapchainCount; i++)
		if (find_sc(pi->pSwapchains[i]))
			ours++;
	if (!ours) {
		pthread_mutex_lock(&q_lock);
		VkResult r = down_QueuePresentKHR(queue, pi);
		pthread_mutex_unlock(&q_lock);
		return r;
	}

	VkPipelineStageFlags stages[16];
	uint32_t nwait = pi->waitSemaphoreCount < 16 ? pi->waitSemaphoreCount : 16;
	for (uint32_t i = 0; i < nwait; i++)
		stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	int waited = 0;
	VkResult worst = VK_SUCCESS;
	for (uint32_t i = 0; i < pi->swapchainCount; i++) {
		struct swapchain *sc = find_sc(pi->pSwapchains[i]);
		VkResult r;
		if (!sc) {
			r = VK_ERROR_SURFACE_LOST_KHR; // mixing with driver swapchains is not supported
		} else {
			struct dev *d = sc->d;
			uint32_t idx = pi->pImageIndices[i];
			struct img *im = &sc->img[idx];
			VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
			if (!waited) {
				// later copies are ordered after this one by their ALL_COMMANDS barrier
				si.waitSemaphoreCount = nwait;
				si.pWaitSemaphores = pi->pWaitSemaphores;
				si.pWaitDstStageMask = stages;
				waited = 1;
			}
			si.commandBufferCount = 1;
			si.pCommandBuffers = &im->cmd;
			d->ResetFences(d->device, 1, &im->fence);
			pthread_mutex_lock(&q_lock);
			r = d->QueueSubmit(d->queue, 1, &si, im->fence);
			pthread_mutex_unlock(&q_lock);
			pthread_mutex_lock(&sc->m);
			if (r == VK_SUCCESS) {
				im->state = IMG_QUEUED;
				sc->fifo[sc->tail % MAX_IMAGES] = idx;
				sc->tail++;
				pthread_cond_broadcast(&sc->cv);
				r = sc->out_of_date ? VK_ERROR_OUT_OF_DATE_KHR : sc->suboptimal ? VK_SUBOPTIMAL_KHR : VK_SUCCESS;
			} else {
				im->state = IMG_FREE;
			}
			pthread_mutex_unlock(&sc->m);
		}
		if (pi->pResults)
			pi->pResults[i] = r;
		if (r < 0 && worst >= 0)
			worst = r;
		else if (r > 0 && worst == VK_SUCCESS)
			worst = r;
	}
	return worst;
}

// --- debug tracing (QUEST1_VKSHIM_TRACE=1): how the app writes to swapchain images ----------------

static int is_sc_image(VkImage img)
{
	int r = 0;
	pthread_mutex_lock(&g_lock);
	for (struct swapchain *sc = swapchains; sc && !r; sc = sc->nextsc)
		for (uint32_t i = 0; i < sc->n; i++)
			r |= sc->img[i].image == img;
	pthread_mutex_unlock(&g_lock);
	return r;
}

#define MAX_TRACKED 64
static VkImageView sc_views[MAX_TRACKED];
static VkFramebuffer sc_fbs[MAX_TRACKED];
static unsigned n_views, n_fbs;
static int tracked(void *arr, unsigned n, uint64_t h)
{
	for (unsigned i = 0; i < n; i++)
		if (((uint64_t *)arr)[i] == h)
			return 1;
	return 0;
}

static PFN_vkVoidFunction trace_down(VkDevice dev, const char *name)
{
	struct dev *d = dev ? find_dev(dev) : devices;
	return d ? d->gdpa(d->device, name) : NULL;
}

static VKAPI_ATTR VkResult VKAPI_CALL trace_CreateImageView(VkDevice dev, const VkImageViewCreateInfo *ci,
                                                            const VkAllocationCallbacks *a, VkImageView *out)
{
	VkResult r = ((PFN_vkCreateImageView)trace_down(dev, "vkCreateImageView"))(dev, ci, a, out);
	if (r == VK_SUCCESS && is_sc_image(ci->image) && n_views < MAX_TRACKED) {
		sc_views[n_views++] = *out;
		LOG("trace: view %p of swapchain image %p, format %d\n", (void *)*out, (void *)ci->image, ci->format);
	}
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL trace_CreateFramebuffer(VkDevice dev, const VkFramebufferCreateInfo *ci,
                                                              const VkAllocationCallbacks *a, VkFramebuffer *out)
{
	VkResult r = ((PFN_vkCreateFramebuffer)trace_down(dev, "vkCreateFramebuffer"))(dev, ci, a, out);
	for (uint32_t i = 0; r == VK_SUCCESS && i < ci->attachmentCount; i++)
		if (tracked(sc_views, n_views, (uint64_t)ci->pAttachments[i]) && n_fbs < MAX_TRACKED) {
			sc_fbs[n_fbs++] = *out;
			LOG("trace: framebuffer %p %ux%u, attachment %u/%u is a swapchain view, flags 0x%x\n", (void *)*out,
			    ci->width, ci->height, i, ci->attachmentCount, ci->flags);
		}
	return r;
}

static unsigned long trace_rp, trace_blit, trace_copy, trace_clear, trace_resolve;
static void trace_tick(void)
{
	static unsigned long last;
	unsigned long t = trace_rp + trace_blit + trace_copy + trace_clear + trace_resolve;
	if (t / 200 != last / 200 || t < 5)
		LOG("trace: into swapchain images: %lu render passes, %lu blits, %lu copies, %lu clears, %lu resolves\n",
		    trace_rp, trace_blit, trace_copy, trace_clear, trace_resolve);
	last = t;
}

static VKAPI_ATTR void VKAPI_CALL trace_CmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi,
                                                           VkSubpassContents c)
{
	static PFN_vkCmdBeginRenderPass f;
	if (!f)
		f = (PFN_vkCmdBeginRenderPass)trace_down(NULL, "vkCmdBeginRenderPass");
	if (tracked(sc_fbs, n_fbs, (uint64_t)bi->framebuffer)) {
		trace_rp++;
		trace_tick();
	}
	f(cb, bi, c);
}

static VKAPI_ATTR void VKAPI_CALL trace_CmdBlitImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d,
                                                     VkImageLayout dl, uint32_t n, const VkImageBlit *r, VkFilter f)
{
	static PFN_vkCmdBlitImage fn;
	if (!fn)
		fn = (PFN_vkCmdBlitImage)trace_down(NULL, "vkCmdBlitImage");
	if (is_sc_image(d)) {
		trace_blit++;
		trace_tick();
	}
	if (is_sc_image(s))
		LOG("trace: blit FROM swapchain image: src y %d..%d -> dst y %d..%d\n", r->srcOffsets[0].y,
		    r->srcOffsets[1].y, r->dstOffsets[0].y, r->dstOffsets[1].y);
	fn(cb, s, sl, d, dl, n, r, f);
}

static VKAPI_ATTR void VKAPI_CALL trace_CmdCopyImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d,
                                                     VkImageLayout dl, uint32_t n, const VkImageCopy *r)
{
	static PFN_vkCmdCopyImage fn;
	if (!fn)
		fn = (PFN_vkCmdCopyImage)trace_down(NULL, "vkCmdCopyImage");
	if (is_sc_image(d)) {
		trace_copy++;
		trace_tick();
	}
	if (is_sc_image(s))
		LOG("trace: copy FROM swapchain image: src (%d,%d) %ux%u -> dst (%d,%d)\n", r->srcOffset.x, r->srcOffset.y,
		    r->extent.width, r->extent.height, r->dstOffset.x, r->dstOffset.y);
	fn(cb, s, sl, d, dl, n, r);
}

static VKAPI_ATTR void VKAPI_CALL trace_CmdResolveImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d,
                                                        VkImageLayout dl, uint32_t n, const VkImageResolve *r)
{
	static PFN_vkCmdResolveImage fn;
	if (!fn)
		fn = (PFN_vkCmdResolveImage)trace_down(NULL, "vkCmdResolveImage");
	if (is_sc_image(d)) {
		trace_resolve++;
		trace_tick();
	}
	fn(cb, s, sl, d, dl, n, r);
}

static VKAPI_ATTR void VKAPI_CALL trace_CmdClearColorImage(VkCommandBuffer cb, VkImage img, VkImageLayout l,
                                                           const VkClearColorValue *c, uint32_t n,
                                                           const VkImageSubresourceRange *r)
{
	static PFN_vkCmdClearColorImage fn;
	if (!fn)
		fn = (PFN_vkCmdClearColorImage)trace_down(NULL, "vkCmdClearColorImage");
	if (is_sc_image(img)) {
		trace_clear++;
		trace_tick();
	}
	fn(cb, img, l, c, n, r);
}

// --- dispatch -------------------------------------------------------------------------------------

PFN_vkVoidFunction wsi_proc(const char *name)
{
	if (!wsi_enabled() || strncmp(name, "vk", 2) != 0)
		return NULL;
	static const struct {
		const char *name;
		PFN_vkVoidFunction fn;
	} table[] = {
#define H(n) {"vk" #n, (PFN_vkVoidFunction)hook_##n}
	    H(CreateXcbSurfaceKHR),
	    H(CreateXlibSurfaceKHR),
	    H(DestroySurfaceKHR),
	    H(GetPhysicalDeviceXcbPresentationSupportKHR),
	    H(GetPhysicalDeviceXlibPresentationSupportKHR),
	    H(GetPhysicalDeviceSurfaceSupportKHR),
	    H(GetPhysicalDeviceSurfaceCapabilitiesKHR),
	    H(GetPhysicalDeviceSurfaceCapabilities2KHR),
	    H(GetPhysicalDeviceSurfaceFormatsKHR),
	    H(GetPhysicalDeviceSurfaceFormats2KHR),
	    H(GetPhysicalDeviceSurfacePresentModesKHR),
	    H(GetPhysicalDevicePresentRectanglesKHR),
	    H(GetDeviceGroupSurfacePresentModesKHR),
	    H(CreateSwapchainKHR),
	    H(DestroySwapchainKHR),
	    H(GetSwapchainImagesKHR),
	    H(AcquireNextImageKHR),
	    H(AcquireNextImage2KHR),
	    H(QueuePresentKHR),
	    H(QueueSubmit),
	    H(QueueWaitIdle),
	    H(QueueBindSparse),
	    H(DestroyDevice),
#undef H
	};
	for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++)
		if (strcmp(name, table[i].name) == 0)
			return table[i].fn;
	static const struct {
		const char *name;
		PFN_vkVoidFunction fn;
	} trace[] = {
#define T(n) {"vk" #n, (PFN_vkVoidFunction)trace_##n}
	    T(CreateImageView), T(CreateFramebuffer), T(CmdBeginRenderPass), T(CmdBlitImage),
	    T(CmdCopyImage),    T(CmdResolveImage),   T(CmdClearColorImage),
#undef T
	};
	if (getenv("QUEST1_VKSHIM_TRACE"))
		for (unsigned i = 0; i < sizeof(trace) / sizeof(trace[0]); i++)
			if (strcmp(name, trace[i].name) == 0)
				return trace[i].fn;
	return NULL;
}
