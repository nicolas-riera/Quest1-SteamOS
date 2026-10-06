// quest1_display: a simulated direct-mode display for vrcompositor (included by quest1_compat.c).
//
// vrcompositor always renders its distortion pass into the swapchain of a direct-mode display
// (CHmdWindowVulkanWSI), even when the driver offers IVRVirtualDisplay. The Android loader has no
// VK_KHR_display at all, so this provides one: a display named "Valve Corporation Quest1" (the
// prefix vrcompositor matches), one mode, one plane, a surface and a swapchain whose images are
// exportable (OPAQUE_FD). On vkQueuePresentKHR the finished image goes to driver_quest1 in vrserver
// over a unix socket (the memory fds once, then image indices); the driver blits it into its
// OpenXR swapchains for Monado and hands the image back.
//
// Socket protocol (struct qd_msg, both directions):
//   layer -> driver  QD_SWAPCHAIN: image description + one fd per image (SCM_RIGHTS)
//   layer -> driver  QD_PRESENT:   index of a finished image (in TRANSFER_SRC_OPTIMAL)
//   driver -> layer  QD_RELEASE:   the driver is done reading that image
//
// Env: QUEST1_DISPLAY_SIZE (default 2448x1360), QUEST1_DISPLAY_HZ (default 72),
//      QUEST1_DISPLAY_SOCKET (default $XDG_RUNTIME_DIR/quest1-display.sock)
#include <sys/socket.h>
#include <sys/un.h>

#include "quest1_display_proto.h"

#define QD_DISPLAY ((VkDisplayKHR)(uintptr_t)0x51d1500d)
#define QD_MODE ((VkDisplayModeKHR)(uintptr_t)0x51d1500e)
#define QD_NAME "Valve Corporation Quest1 (virtual)"

static uint32_t qd_width = 2448, qd_height = 1360, qd_mhz = 72000;
static pthread_once_t qd_once = PTHREAD_ONCE_INIT;

static void qd_init(void)
{
	const char *s = getenv("QUEST1_DISPLAY_SIZE");
	if (s)
		sscanf(s, "%ux%u", &qd_width, &qd_height);
	if ((s = getenv("QUEST1_DISPLAY_HZ")))
		qd_mhz = (uint32_t)(atof(s) * 1000);
}

// --- display queries -------------------------------------------------------------------------------------

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetPhysicalDeviceDisplayPropertiesKHR(VkPhysicalDevice pd, uint32_t *count,
                                                                             VkDisplayPropertiesKHR *props)
{
	pthread_once(&qd_once, qd_init);
	if (!props) {
		*count = 1;
		return VK_SUCCESS;
	}
	if (*count < 1)
		return VK_INCOMPLETE;
	*count = 1;
	memset(props, 0, sizeof(*props));
	props->display = QD_DISPLAY;
	props->displayName = QD_NAME;
	props->physicalDimensions = (VkExtent2D){121, 68}; // mm, informative
	props->physicalResolution = (VkExtent2D){qd_width, qd_height};
	props->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetPhysicalDeviceDisplayProperties2KHR(VkPhysicalDevice pd, uint32_t *count,
                                                                              VkDisplayProperties2KHR *props)
{
	if (!props)
		return qd_GetPhysicalDeviceDisplayPropertiesKHR(pd, count, NULL);
	return qd_GetPhysicalDeviceDisplayPropertiesKHR(pd, count, &props->displayProperties);
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetPhysicalDeviceDisplayPlanePropertiesKHR(VkPhysicalDevice pd,
                                                                                  uint32_t *count,
                                                                                  VkDisplayPlanePropertiesKHR *props)
{
	if (!props) {
		*count = 1;
		return VK_SUCCESS;
	}
	if (*count < 1)
		return VK_INCOMPLETE;
	*count = 1;
	props->currentDisplay = QD_DISPLAY;
	props->currentStackIndex = 0;
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetPhysicalDeviceDisplayPlaneProperties2KHR(
    VkPhysicalDevice pd, uint32_t *count, VkDisplayPlaneProperties2KHR *props)
{
	if (!props)
		return qd_GetPhysicalDeviceDisplayPlanePropertiesKHR(pd, count, NULL);
	return qd_GetPhysicalDeviceDisplayPlanePropertiesKHR(pd, count, &props->displayPlaneProperties);
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetDisplayPlaneSupportedDisplaysKHR(VkPhysicalDevice pd, uint32_t plane,
                                                                           uint32_t *count, VkDisplayKHR *displays)
{
	if (plane != 0 || !displays) {
		*count = plane == 0;
		return VK_SUCCESS;
	}
	if (*count < 1)
		return VK_INCOMPLETE;
	*count = 1;
	displays[0] = QD_DISPLAY;
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetDisplayModePropertiesKHR(VkPhysicalDevice pd, VkDisplayKHR display,
                                                                   uint32_t *count, VkDisplayModePropertiesKHR *props)
{
	pthread_once(&qd_once, qd_init);
	if (display != QD_DISPLAY) {
		*count = 0;
		return VK_SUCCESS;
	}
	if (!props) {
		*count = 1;
		return VK_SUCCESS;
	}
	if (*count < 1)
		return VK_INCOMPLETE;
	*count = 1;
	props->displayMode = QD_MODE;
	props->parameters.visibleRegion = (VkExtent2D){qd_width, qd_height};
	props->parameters.refreshRate = qd_mhz;
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetDisplayModeProperties2KHR(VkPhysicalDevice pd, VkDisplayKHR display,
                                                                    uint32_t *count, VkDisplayModeProperties2KHR *props)
{
	if (!props)
		return qd_GetDisplayModePropertiesKHR(pd, display, count, NULL);
	return qd_GetDisplayModePropertiesKHR(pd, display, count, &props->displayModeProperties);
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_CreateDisplayModeKHR(VkPhysicalDevice pd, VkDisplayKHR display,
                                                            const VkDisplayModeCreateInfoKHR *ci,
                                                            const VkAllocationCallbacks *alloc, VkDisplayModeKHR *mode)
{
	*mode = QD_MODE;
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetDisplayPlaneCapabilitiesKHR(VkPhysicalDevice pd, VkDisplayModeKHR mode,
                                                                      uint32_t plane,
                                                                      VkDisplayPlaneCapabilitiesKHR *caps)
{
	pthread_once(&qd_once, qd_init);
	memset(caps, 0, sizeof(*caps));
	caps->supportedAlpha = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
	caps->minSrcExtent = caps->maxSrcExtent = caps->minDstExtent = caps->maxDstExtent =
	    (VkExtent2D){qd_width, qd_height};
	caps->maxSrcPosition = caps->maxDstPosition = (VkOffset2D){0, 0};
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetDisplayPlaneCapabilities2KHR(VkPhysicalDevice pd,
                                                                       const VkDisplayPlaneInfo2KHR *info,
                                                                       VkDisplayPlaneCapabilities2KHR *caps)
{
	return qd_GetDisplayPlaneCapabilitiesKHR(pd, info->mode, info->planeIndex, &caps->capabilities);
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_succeed(void) { return VK_SUCCESS; }

static VKAPI_ATTR VkResult VKAPI_CALL qd_unsupported(void) { return VK_ERROR_INITIALIZATION_FAILED; }

// RandR has no output for us: vrcompositor then enumerates the Vulkan displays
static VKAPI_ATTR VkResult VKAPI_CALL qd_GetRandROutputDisplayEXT(VkPhysicalDevice pd, void *dpy,
                                                                unsigned long output, VkDisplayKHR *display)
{
	*display = VK_NULL_HANDLE;
	return VK_SUCCESS;
}

// --- surfaces ----------------------------------------------------------------------------------------------

#define QD_MAX_SURFACES 4
static void *qd_surfaces[QD_MAX_SURFACES];

static bool qd_is_surface(VkSurfaceKHR s)
{
	pthread_mutex_lock(&g_lock);
	bool r = false;
	for (int i = 0; i < QD_MAX_SURFACES; i++)
		r |= s != VK_NULL_HANDLE && qd_surfaces[i] == (void *)(uintptr_t)s;
	pthread_mutex_unlock(&g_lock);
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_CreateDisplayPlaneSurfaceKHR(VkInstance instance,
                                                                    const VkDisplaySurfaceCreateInfoKHR *ci,
                                                                    const VkAllocationCallbacks *alloc,
                                                                    VkSurfaceKHR *surface)
{
	void *s = malloc(16);
	pthread_mutex_lock(&g_lock);
	int slot = -1;
	for (int i = 0; i < QD_MAX_SURFACES && slot < 0; i++)
		if (!qd_surfaces[i])
			slot = i;
	if (slot >= 0)
		qd_surfaces[slot] = s;
	pthread_mutex_unlock(&g_lock);
	if (slot < 0) {
		free(s);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	*surface = (VkSurfaceKHR)(uintptr_t)s;
	LOG("display surface created (%ux%u @ %.1f Hz)\n", qd_width, qd_height, qd_mhz / 1000.0);
	return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL qd_DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
                                                     const VkAllocationCallbacks *alloc)
{
	if (qd_is_surface(surface)) {
		pthread_mutex_lock(&g_lock);
		for (int i = 0; i < QD_MAX_SURFACES; i++)
			if (qd_surfaces[i] == (void *)(uintptr_t)surface)
				qd_surfaces[i] = NULL;
		pthread_mutex_unlock(&g_lock);
		free((void *)(uintptr_t)surface);
		return;
	}
	struct instance *in = instance_of(instance);
	PFN_vkDestroySurfaceKHR next = (PFN_vkDestroySurfaceKHR)in->gipa(instance, "vkDestroySurfaceKHR");
	if (next)
		next(instance, surface, alloc);
}

#define NEXT_INSTANCE_FN(pd, type, name) ((type)instance_of(pd)->gipa(instance_of(pd)->handle, name))

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice pd, uint32_t family,
                                                                          VkSurfaceKHR surface, VkBool32 *supported)
{
	if (qd_is_surface(surface)) {
		*supported = VK_TRUE;
		return VK_SUCCESS;
	}
	return NEXT_INSTANCE_FN(pd, PFN_vkGetPhysicalDeviceSurfaceSupportKHR, "vkGetPhysicalDeviceSurfaceSupportKHR")(
	    pd, family, surface, supported);
}

static void qd_fill_caps(VkSurfaceCapabilitiesKHR *c)
{
	pthread_once(&qd_once, qd_init);
	memset(c, 0, sizeof(*c));
	c->minImageCount = 3;
	c->maxImageCount = QD_MAX_IMAGES;
	c->currentExtent = c->minImageExtent = c->maxImageExtent = (VkExtent2D){qd_width, qd_height};
	c->maxImageArrayLayers = 1;
	c->supportedTransforms = c->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	c->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	c->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice pd,
                                                                               VkSurfaceKHR surface,
                                                                               VkSurfaceCapabilitiesKHR *caps)
{
	if (qd_is_surface(surface)) {
		qd_fill_caps(caps);
		return VK_SUCCESS;
	}
	return NEXT_INSTANCE_FN(pd, PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR,
	                        "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")(pd, surface, caps);
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetPhysicalDeviceSurfaceCapabilities2KHR(
    VkPhysicalDevice pd, const VkPhysicalDeviceSurfaceInfo2KHR *info, VkSurfaceCapabilities2KHR *caps)
{
	if (qd_is_surface(info->surface)) {
		qd_fill_caps(&caps->surfaceCapabilities);
		return VK_SUCCESS;
	}
	return NEXT_INSTANCE_FN(pd, PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR,
	                        "vkGetPhysicalDeviceSurfaceCapabilities2KHR")(pd, info, caps);
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetPhysicalDeviceSurfaceCapabilities2EXT(VkPhysicalDevice pd,
                                                                                VkSurfaceKHR surface,
                                                                                VkSurfaceCapabilities2EXT *caps)
{
	if (!qd_is_surface(surface))
		return VK_ERROR_SURFACE_LOST_KHR;
	VkSurfaceCapabilitiesKHR c;
	qd_fill_caps(&c);
	caps->minImageCount = c.minImageCount;
	caps->maxImageCount = c.maxImageCount;
	caps->currentExtent = c.currentExtent;
	caps->minImageExtent = c.minImageExtent;
	caps->maxImageExtent = c.maxImageExtent;
	caps->maxImageArrayLayers = c.maxImageArrayLayers;
	caps->supportedTransforms = c.supportedTransforms;
	caps->currentTransform = c.currentTransform;
	caps->supportedCompositeAlpha = c.supportedCompositeAlpha;
	caps->supportedUsageFlags = c.supportedUsageFlags;
	caps->supportedSurfaceCounters = VK_SURFACE_COUNTER_VBLANK_BIT_EXT;
	return VK_SUCCESS;
}

static const VkSurfaceFormatKHR qd_formats[] = {
    {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
};
#define QD_FORMAT_COUNT (sizeof(qd_formats) / sizeof(qd_formats[0]))

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice pd, VkSurfaceKHR surface,
                                                                          uint32_t *count, VkSurfaceFormatKHR *formats)
{
	if (!qd_is_surface(surface))
		return NEXT_INSTANCE_FN(pd, PFN_vkGetPhysicalDeviceSurfaceFormatsKHR,
		                        "vkGetPhysicalDeviceSurfaceFormatsKHR")(pd, surface, count, formats);
	if (!formats) {
		*count = QD_FORMAT_COUNT;
		return VK_SUCCESS;
	}
	uint32_t n = *count < QD_FORMAT_COUNT ? *count : QD_FORMAT_COUNT;
	memcpy(formats, qd_formats, n * sizeof(*formats));
	*count = n;
	return n < QD_FORMAT_COUNT ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetPhysicalDeviceSurfaceFormats2KHR(
    VkPhysicalDevice pd, const VkPhysicalDeviceSurfaceInfo2KHR *info, uint32_t *count, VkSurfaceFormat2KHR *formats)
{
	if (!qd_is_surface(info->surface))
		return NEXT_INSTANCE_FN(pd, PFN_vkGetPhysicalDeviceSurfaceFormats2KHR,
		                        "vkGetPhysicalDeviceSurfaceFormats2KHR")(pd, info, count, formats);
	if (!formats) {
		*count = QD_FORMAT_COUNT;
		return VK_SUCCESS;
	}
	uint32_t n = *count < QD_FORMAT_COUNT ? *count : QD_FORMAT_COUNT;
	for (uint32_t i = 0; i < n; i++)
		formats[i].surfaceFormat = qd_formats[i];
	*count = n;
	return n < QD_FORMAT_COUNT ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice pd,
                                                                               VkSurfaceKHR surface, uint32_t *count,
                                                                               VkPresentModeKHR *modes)
{
	static const VkPresentModeKHR all[] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR,
	                                       VK_PRESENT_MODE_IMMEDIATE_KHR};
	if (!qd_is_surface(surface))
		return NEXT_INSTANCE_FN(pd, PFN_vkGetPhysicalDeviceSurfacePresentModesKHR,
		                        "vkGetPhysicalDeviceSurfacePresentModesKHR")(pd, surface, count, modes);
	if (!modes) {
		*count = 3;
		return VK_SUCCESS;
	}
	uint32_t n = *count < 3 ? *count : 3;
	memcpy(modes, all, n * sizeof(*modes));
	*count = n;
	return n < 3 ? VK_INCOMPLETE : VK_SUCCESS;
}

// --- swapchains --------------------------------------------------------------------------------------------

struct qd_swapchain
{
	struct device *d;
	VkDevice device;
	VkQueue queue;
	uint32_t count;
	VkImageCreateInfo ici;
	VkFormat view_formats[4];
	uint32_t view_format_count;
	VkImage images[QD_MAX_IMAGES];
	VkDeviceMemory memory[QD_MAX_IMAGES];
	VkDeviceSize size[QD_MAX_IMAGES];
	VkCommandPool pool;
	VkCommandBuffer to_app[QD_MAX_IMAGES], to_driver[QD_MAX_IMAGES];
	VkFence fence;

	pthread_mutex_t mutex;
	pthread_cond_t cond;
	bool app_owned[QD_MAX_IMAGES], driver_owned[QD_MAX_IMAGES];
	uint32_t next;
	uint64_t frame;
	int sock;
	pthread_t reader;
	bool reader_running;

	// device entry points (below this layer)
	PFN_vkDestroyImage DestroyImage;
	PFN_vkFreeMemory FreeMemory;
	PFN_vkDestroyCommandPool DestroyCommandPool;
};

#define QD_MAX_SWAPCHAINS 4
static struct qd_swapchain *qd_swapchains[QD_MAX_SWAPCHAINS];

static struct qd_swapchain *qd_swapchain_of(VkSwapchainKHR s)
{
	pthread_mutex_lock(&g_lock);
	struct qd_swapchain *r = NULL;
	for (int i = 0; i < QD_MAX_SWAPCHAINS; i++)
		if (s != VK_NULL_HANDLE && qd_swapchains[i] == (void *)(uintptr_t)s)
			r = qd_swapchains[i];
	pthread_mutex_unlock(&g_lock);
	return r;
}

static int qd_connect(void)
{
	char path[108];
	const char *p = getenv("QUEST1_DISPLAY_SOCKET");
	if (p)
		snprintf(path, sizeof(path), "%s", p);
	else
		snprintf(path, sizeof(path), "%s/quest1-display.sock",
		         getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "/tmp");
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	struct sockaddr_un a = {AF_UNIX};
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
	if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
		LOG("no display receiver at %s (%s): frames are dropped\n", path, strerror(errno));
		if (fd >= 0)
			close(fd);
		return -1;
	}
	LOG("connected to the display receiver at %s\n", path);
	return fd;
}

static void *qd_reader(void *arg)
{
	struct qd_swapchain *sc = arg;
	struct qd_msg m;
	while (recv(sc->sock, &m, sizeof(m), 0) == sizeof(m)) {
		if (m.type != QD_RELEASE || m.index >= sc->count)
			continue;
		pthread_mutex_lock(&sc->mutex);
		sc->driver_owned[m.index] = false;
		pthread_cond_broadcast(&sc->cond);
		pthread_mutex_unlock(&sc->mutex);
	}
	// receiver gone: never wait for it again
	pthread_mutex_lock(&sc->mutex);
	close(sc->sock);
	sc->sock = -1;
	memset(sc->driver_owned, 0, sizeof(sc->driver_owned));
	pthread_cond_broadcast(&sc->cond);
	pthread_mutex_unlock(&sc->mutex);
	LOG("display receiver disconnected\n");
	return NULL;
}

static bool qd_send_swapchain(struct qd_swapchain *sc, const int *fds)
{
	struct qd_msg m = {QD_SWAPCHAIN};
	m.count = sc->count;
	m.width = sc->ici.extent.width;
	m.height = sc->ici.extent.height;
	m.format = sc->ici.format;
	m.usage = sc->ici.usage;
	m.flags = sc->ici.flags;
	m.view_format_count = sc->view_format_count;
	for (uint32_t i = 0; i < sc->view_format_count; i++)
		m.view_formats[i] = sc->view_formats[i];
	for (uint32_t i = 0; i < sc->count; i++)
		m.size[i] = sc->size[i];

	char ctrl[CMSG_SPACE(sizeof(int) * QD_MAX_IMAGES)];
	memset(ctrl, 0, sizeof(ctrl));
	struct iovec iov = {&m, sizeof(m)};
	struct msghdr h = {0};
	h.msg_iov = &iov;
	h.msg_iovlen = 1;
	h.msg_control = ctrl;
	h.msg_controllen = CMSG_SPACE(sizeof(int) * sc->count);
	struct cmsghdr *c = CMSG_FIRSTHDR(&h);
	c->cmsg_level = SOL_SOCKET;
	c->cmsg_type = SCM_RIGHTS;
	c->cmsg_len = CMSG_LEN(sizeof(int) * sc->count);
	memcpy(CMSG_DATA(c), fds, sizeof(int) * sc->count);
	return sendmsg(sc->sock, &h, MSG_NOSIGNAL) == (ssize_t)sizeof(m);
}

static void qd_record_barrier(struct qd_swapchain *sc, VkCommandBuffer cmd, VkImage image, VkImageLayout from,
                              VkImageLayout to)
{
	struct device *d = sc->d;
	PFN_vkBeginCommandBuffer begin = (PFN_vkBeginCommandBuffer)d->gdpa(sc->device, "vkBeginCommandBuffer");
	PFN_vkEndCommandBuffer end = (PFN_vkEndCommandBuffer)d->gdpa(sc->device, "vkEndCommandBuffer");
	PFN_vkCmdPipelineBarrier barrier = (PFN_vkCmdPipelineBarrier)d->gdpa(sc->device, "vkCmdPipelineBarrier");
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin(cmd, &bi);
	VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	b.oldLayout = from;
	b.newLayout = to;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
	end(cmd);
}

static void qd_destroy(struct qd_swapchain *sc);

static VKAPI_ATTR VkResult VKAPI_CALL qd_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *ci,
                                                          const VkAllocationCallbacks *alloc, VkSwapchainKHR *out)
{
	struct device *d = device_of(device);
	if (!qd_is_surface(ci->surface))
		return ((PFN_vkCreateSwapchainKHR)d->gdpa(device, "vkCreateSwapchainKHR"))(device, ci, alloc, out);

	struct qd_swapchain *sc = calloc(1, sizeof(*sc));
	sc->d = d;
	sc->device = device;
	sc->sock = -1;
	sc->count = ci->minImageCount < 3 ? 3 : ci->minImageCount;
	if (sc->count > QD_MAX_IMAGES)
		sc->count = QD_MAX_IMAGES;
	pthread_mutex_init(&sc->mutex, NULL);
	pthread_cond_init(&sc->cond, NULL);
#define DFN(name) PFN_vk##name name = (PFN_vk##name)d->gdpa(device, "vk" #name)
	DFN(CreateImage);
	DFN(GetImageMemoryRequirements);
	DFN(AllocateMemory);
	DFN(BindImageMemory);
	DFN(GetMemoryFdKHR);
	DFN(CreateCommandPool);
	DFN(AllocateCommandBuffers);
	DFN(GetDeviceQueue);
#undef DFN
	sc->DestroyImage = (PFN_vkDestroyImage)d->gdpa(device, "vkDestroyImage");
	sc->FreeMemory = (PFN_vkFreeMemory)d->gdpa(device, "vkFreeMemory");
	sc->DestroyCommandPool = (PFN_vkDestroyCommandPool)d->gdpa(device, "vkDestroyCommandPool");
	GetDeviceQueue(device, 0, 0, &sc->queue);

	// the images: what a WSI swapchain would make, plus exportable memory and TRANSFER_SRC for the driver
	VkExternalMemoryImageCreateInfo emi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
	emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	VkImageFormatListCreateInfo fl = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
	const VkImageFormatListCreateInfo *app_fl = find_struct(ci->pNext, VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO);
	if (app_fl && app_fl->viewFormatCount <= 4) {
		sc->view_format_count = app_fl->viewFormatCount;
		memcpy(sc->view_formats, app_fl->pViewFormats, app_fl->viewFormatCount * sizeof(VkFormat));
		fl.viewFormatCount = sc->view_format_count;
		fl.pViewFormats = sc->view_formats;
		emi.pNext = &fl;
	}
	VkImageCreateInfo *ici = &sc->ici;
	ici->sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici->flags = (ci->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
	ici->imageType = VK_IMAGE_TYPE_2D;
	ici->format = ci->imageFormat;
	ici->extent = (VkExtent3D){ci->imageExtent.width, ci->imageExtent.height, 1};
	ici->mipLevels = 1;
	ici->arrayLayers = ci->imageArrayLayers;
	ici->samples = VK_SAMPLE_COUNT_1_BIT;
	ici->tiling = VK_IMAGE_TILING_OPTIMAL;
	ici->usage = ci->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	ici->sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ici->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VkPhysicalDeviceMemoryProperties mp;
	struct instance *in = NULL;
	pthread_mutex_lock(&g_lock);
	for (int i = 0; i < MAX_OBJS && !in; i++)
		if (g_instances[i])
			in = g_instances[i];
	pthread_mutex_unlock(&g_lock);
	PFN_vkEnumeratePhysicalDevices enum_pd = (PFN_vkEnumeratePhysicalDevices)in->gipa(in->handle, "vkEnumeratePhysicalDevices");
	PFN_vkGetPhysicalDeviceMemoryProperties memprops =
	    (PFN_vkGetPhysicalDeviceMemoryProperties)in->gipa(in->handle, "vkGetPhysicalDeviceMemoryProperties");
	uint32_t npd = 1;
	VkPhysicalDevice pd;
	enum_pd(in->handle, &npd, &pd); // a single GPU
	memprops(pd, &mp);

	int fds[QD_MAX_IMAGES];
	VkResult r = VK_SUCCESS;
	for (uint32_t i = 0; i < sc->count && r == VK_SUCCESS; i++) {
		VkImageCreateInfo c = *ici;
		c.pNext = &emi;
		if ((r = CreateImage(device, &c, NULL, &sc->images[i])) != VK_SUCCESS)
			break;
		VkMemoryRequirements mr;
		GetImageMemoryRequirements(device, sc->images[i], &mr);
		uint32_t type = 0;
		for (uint32_t t = 0; t < mp.memoryTypeCount; t++)
			if ((mr.memoryTypeBits & (1u << t)) &&
			    (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
				type = t;
				break;
			}
		VkExportMemoryAllocateInfo ex = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
		ex.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
		VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &ex};
		ded.image = sc->images[i];
		VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded};
		mai.allocationSize = mr.size;
		mai.memoryTypeIndex = type;
		if ((r = AllocateMemory(device, &mai, NULL, &sc->memory[i])) != VK_SUCCESS)
			break;
		sc->size[i] = mr.size;
		if ((r = BindImageMemory(device, sc->images[i], sc->memory[i], 0)) != VK_SUCCESS)
			break;
		VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
		gi.memory = sc->memory[i];
		gi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
		fds[i] = -1;
		if ((r = GetMemoryFdKHR(device, &gi, &fds[i])) != VK_SUCCESS)
			break;
	}

	VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pci.queueFamilyIndex = 0;
	if (r == VK_SUCCESS)
		r = CreateCommandPool(device, &pci, NULL, &sc->pool);
	if (r == VK_SUCCESS) {
		VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		cai.commandPool = sc->pool;
		cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cai.commandBufferCount = sc->count;
		r = AllocateCommandBuffers(device, &cai, sc->to_app);
		if (r == VK_SUCCESS)
			r = AllocateCommandBuffers(device, &cai, sc->to_driver);
	}
	if (r == VK_SUCCESS) {
		// the app gets PRESENT_SRC images (contents are rewritten every frame); the driver TRANSFER_SRC
		for (uint32_t i = 0; i < sc->count; i++) {
			qd_record_barrier(sc, sc->to_app[i], sc->images[i], VK_IMAGE_LAYOUT_UNDEFINED,
			                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
			qd_record_barrier(sc, sc->to_driver[i], sc->images[i], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		}
		VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		r = d->CreateFence(device, &fci, NULL, &sc->fence);
	}
	if (r != VK_SUCCESS) {
		LOG("display swapchain creation failed: %d\n", r);
		qd_destroy(sc);
		return r;
	}

	sc->sock = qd_connect();
	if (sc->sock >= 0 && !qd_send_swapchain(sc, fds)) {
		LOG("sending the swapchain to the display receiver failed: %s\n", strerror(errno));
		close(sc->sock);
		sc->sock = -1;
	}
	for (uint32_t i = 0; i < sc->count; i++)
		close(fds[i]);
	if (sc->sock >= 0) {
		sc->reader_running = pthread_create(&sc->reader, NULL, qd_reader, sc) == 0;
	}

	pthread_mutex_lock(&g_lock);
	for (int i = 0; i < QD_MAX_SWAPCHAINS; i++)
		if (!qd_swapchains[i]) {
			qd_swapchains[i] = sc;
			break;
		}
	pthread_mutex_unlock(&g_lock);
	*out = (VkSwapchainKHR)(uintptr_t)sc;
	LOG("display swapchain: %u images %ux%u format %d usage 0x%x flags 0x%x\n", sc->count, ici->extent.width,
	    ici->extent.height, ici->format, ici->usage, ici->flags);
	return VK_SUCCESS;
}

static void qd_destroy(struct qd_swapchain *sc)
{
	if (sc->sock >= 0)
		shutdown(sc->sock, SHUT_RDWR);
	if (sc->reader_running)
		pthread_join(sc->reader, NULL);
	if (sc->fence)
		sc->d->DestroyFence(sc->device, sc->fence, NULL);
	if (sc->pool)
		sc->DestroyCommandPool(sc->device, sc->pool, NULL);
	for (uint32_t i = 0; i < sc->count; i++) {
		if (sc->images[i])
			sc->DestroyImage(sc->device, sc->images[i], NULL);
		if (sc->memory[i])
			sc->FreeMemory(sc->device, sc->memory[i], NULL);
	}
	free(sc);
}

static VKAPI_ATTR void VKAPI_CALL qd_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                       const VkAllocationCallbacks *alloc)
{
	struct qd_swapchain *sc = qd_swapchain_of(swapchain);
	if (!sc) {
		struct device *d = device_of(device);
		((PFN_vkDestroySwapchainKHR)d->gdpa(device, "vkDestroySwapchainKHR"))(device, swapchain, alloc);
		return;
	}
	pthread_mutex_lock(&g_lock);
	for (int i = 0; i < QD_MAX_SWAPCHAINS; i++)
		if (qd_swapchains[i] == sc)
			qd_swapchains[i] = NULL;
	pthread_mutex_unlock(&g_lock);
	sc->d->QueueWaitIdle(sc->queue);
	qd_destroy(sc);
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                             uint32_t *count, VkImage *images)
{
	struct qd_swapchain *sc = qd_swapchain_of(swapchain);
	if (!sc) {
		struct device *d = device_of(device);
		return ((PFN_vkGetSwapchainImagesKHR)d->gdpa(device, "vkGetSwapchainImagesKHR"))(device, swapchain, count,
		                                                                                images);
	}
	if (!images) {
		*count = sc->count;
		return VK_SUCCESS;
	}
	uint32_t n = *count < sc->count ? *count : sc->count;
	memcpy(images, sc->images, n * sizeof(VkImage));
	*count = n;
	return n < sc->count ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult qd_acquire(struct qd_swapchain *sc, uint64_t timeout, VkSemaphore semaphore, VkFence fence,
                           uint32_t *index)
{
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	if (timeout != UINT64_MAX) {
		uint64_t ns = deadline.tv_nsec + timeout % 1000000000ull;
		deadline.tv_sec += timeout / 1000000000ull + ns / 1000000000ull;
		deadline.tv_nsec = ns % 1000000000ull;
	}
	pthread_mutex_lock(&sc->mutex);
	int found = -1;
	for (;;) {
		for (uint32_t k = 0; k < sc->count && found < 0; k++) {
			uint32_t i = (sc->next + k) % sc->count;
			if (!sc->app_owned[i] && !sc->driver_owned[i])
				found = (int)i;
		}
		if (found >= 0 || timeout == 0)
			break;
		int e = timeout == UINT64_MAX ? pthread_cond_wait(&sc->cond, &sc->mutex)
		                              : pthread_cond_timedwait(&sc->cond, &sc->mutex, &deadline);
		if (e == ETIMEDOUT)
			break;
	}
	if (found < 0) {
		pthread_mutex_unlock(&sc->mutex);
		return timeout == 0 ? VK_NOT_READY : VK_TIMEOUT;
	}
	sc->app_owned[found] = true;
	sc->next = (found + 1) % sc->count;
	pthread_mutex_unlock(&sc->mutex);

	// the transition to PRESENT_SRC doubles as the acquire signal operation
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &sc->to_app[found];
	si.signalSemaphoreCount = semaphore != VK_NULL_HANDLE;
	si.pSignalSemaphores = &semaphore;
	VkResult r = QueueSubmit(sc->queue, 1, &si, fence);
	*index = (uint32_t)found;
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                           uint64_t timeout, VkSemaphore semaphore, VkFence fence,
                                                           uint32_t *index)
{
	struct qd_swapchain *sc = qd_swapchain_of(swapchain);
	if (!sc) {
		struct device *d = device_of(device);
		return ((PFN_vkAcquireNextImageKHR)d->gdpa(device, "vkAcquireNextImageKHR"))(device, swapchain, timeout,
		                                                                            semaphore, fence, index);
	}
	return qd_acquire(sc, timeout, semaphore, fence, index);
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_AcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR *info,
                                                            uint32_t *index)
{
	struct qd_swapchain *sc = qd_swapchain_of(info->swapchain);
	if (!sc) {
		struct device *d = device_of(device);
		return ((PFN_vkAcquireNextImage2KHR)d->gdpa(device, "vkAcquireNextImage2KHR"))(device, info, index);
	}
	return qd_acquire(sc, info->timeout, info->semaphore, info->fence, index);
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pi)
{
	VkResult result = VK_SUCCESS;
	bool waited = false;
	for (uint32_t s = 0; s < pi->swapchainCount; s++) {
		struct qd_swapchain *sc = qd_swapchain_of(pi->pSwapchains[s]);
		VkResult r;
		if (!sc) {
			struct device *d = device_of(queue);
			VkPresentInfoKHR one = *pi;
			one.swapchainCount = 1;
			one.pSwapchains = &pi->pSwapchains[s];
			one.pImageIndices = &pi->pImageIndices[s];
			one.pResults = NULL;
			if (waited)
				one.waitSemaphoreCount = 0;
			r = ((PFN_vkQueuePresentKHR)d->gdpa(d->handle, "vkQueuePresentKHR"))(queue, &one);
		} else {
			uint32_t idx = pi->pImageIndices[s];
			// wait for rendering, hand the image over in TRANSFER_SRC, then tell the driver
			VkPipelineStageFlags stages[16];
			for (uint32_t i = 0; i < pi->waitSemaphoreCount && i < 16; i++)
				stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
			si.waitSemaphoreCount = waited ? 0 : pi->waitSemaphoreCount;
			si.pWaitSemaphores = pi->pWaitSemaphores;
			si.pWaitDstStageMask = stages;
			si.commandBufferCount = 1;
			si.pCommandBuffers = &sc->to_driver[idx];
			r = QueueSubmit(queue, 1, &si, sc->fence);
			if (r == VK_SUCCESS) {
				wait_fence(sc->d, sc->device, sc->fence);
				sc->d->ResetFences(sc->device, 1, &sc->fence);
			}
			pthread_mutex_lock(&sc->mutex);
			sc->app_owned[idx] = false;
			if (sc->sock >= 0) {
				struct qd_msg m = {QD_PRESENT};
				m.index = idx;
				m.frame = ++sc->frame;
				if (send(sc->sock, &m, sizeof(m), MSG_NOSIGNAL) == sizeof(m))
					sc->driver_owned[idx] = true;
			}
			pthread_cond_broadcast(&sc->cond);
			pthread_mutex_unlock(&sc->mutex);
		}
		waited = true;
		if (pi->pResults)
			pi->pResults[s] = r;
		if (r != VK_SUCCESS && result == VK_SUCCESS)
			result = r;
	}
	return result;
}

// --- vblank events (VK_EXT_display_control) ---------------------------------------------------------------
// vrcompositor paces its frames on vkRegisterDisplayEventEXT fences and vkGetSwapchainCounterEXT. The
// simulated display ticks at the mode's rate on CLOCK_MONOTONIC; a thread signals the registered fences
// at every tick (an empty submission, the blob cannot signal a fence from the CPU otherwise).

struct qd_event
{
	struct device *d;
	VkQueue queue;
	VkFence fence;
};
static struct qd_event qd_events[16];
static uint32_t qd_event_count;
static pthread_mutex_t qd_event_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t qd_vblank_once = PTHREAD_ONCE_INIT;
static uint64_t qd_epoch_ns;

static uint64_t qd_period_ns(void) { return 1000000000000ull / qd_mhz; }

static uint64_t qd_vblank_count(void) { return (now_ns() - qd_epoch_ns) / qd_period_ns(); }

static void *qd_vblank_thread(void *arg)
{
	for (;;) {
		uint64_t next = qd_epoch_ns + (qd_vblank_count() + 1) * qd_period_ns();
		struct timespec ts = {(time_t)(next / 1000000000ull), (long)(next % 1000000000ull)};
		while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
			;
		pthread_mutex_lock(&qd_event_mutex);
		for (uint32_t i = 0; i < qd_event_count; i++) {
			struct qd_event *e = &qd_events[i];
			pthread_mutex_lock(&e->d->queue_mutex);
			e->d->QueueSubmit(e->queue, 0, NULL, e->fence);
			pthread_mutex_unlock(&e->d->queue_mutex);
		}
		qd_event_count = 0;
		pthread_mutex_unlock(&qd_event_mutex);
	}
	return NULL;
}

static void qd_vblank_start(void)
{
	pthread_once(&qd_once, qd_init);
	qd_epoch_ns = now_ns();
	pthread_t t;
	pthread_create(&t, NULL, qd_vblank_thread, NULL);
	pthread_detach(t);
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_RegisterDisplayEventEXT(VkDevice device, VkDisplayKHR display,
                                                               const VkDisplayEventInfoEXT *info,
                                                               const VkAllocationCallbacks *alloc, VkFence *fence)
{
	pthread_once(&qd_vblank_once, qd_vblank_start);
	struct device *d = device_of(device);
	VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkResult r = d->CreateFence(device, &fci, alloc, fence);
	if (r != VK_SUCCESS)
		return r;
	PFN_vkGetDeviceQueue get_queue = (PFN_vkGetDeviceQueue)d->gdpa(device, "vkGetDeviceQueue");
	VkQueue queue;
	get_queue(device, 0, 0, &queue);
	pthread_mutex_lock(&qd_event_mutex);
	if (qd_event_count < 16)
		qd_events[qd_event_count++] = (struct qd_event){d, queue, *fence};
	pthread_mutex_unlock(&qd_event_mutex);
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetSwapchainCounterEXT(VkDevice device, VkSwapchainKHR swapchain,
                                                              VkSurfaceCounterFlagBitsEXT counter, uint64_t *value)
{
	pthread_once(&qd_vblank_once, qd_vblank_start);
	*value = qd_vblank_count();
	return VK_SUCCESS;
}

// --- dispatch ----------------------------------------------------------------------------------------------

static PFN_vkVoidFunction qd_proc(const char *name)
{
	if (getenv("QUEST1_NO_DISPLAY"))
		return NULL;
#define Q(n) if (strcmp(name, "vk" #n) == 0) return (PFN_vkVoidFunction)qd_##n;
	// VK_KHR_display / display_properties2 / EXT_direct_mode_display / EXT_acquire_*_display
	Q(GetPhysicalDeviceDisplayPropertiesKHR)
	Q(GetPhysicalDeviceDisplayProperties2KHR)
	Q(GetPhysicalDeviceDisplayPlanePropertiesKHR)
	Q(GetPhysicalDeviceDisplayPlaneProperties2KHR)
	Q(GetDisplayPlaneSupportedDisplaysKHR)
	Q(GetDisplayModePropertiesKHR)
	Q(GetDisplayModeProperties2KHR)
	Q(CreateDisplayModeKHR)
	Q(GetDisplayPlaneCapabilitiesKHR)
	Q(GetDisplayPlaneCapabilities2KHR)
	Q(CreateDisplayPlaneSurfaceKHR)
	Q(GetRandROutputDisplayEXT)
	// surfaces and swapchains (pass through for the ones that are not ours)
	Q(DestroySurfaceKHR)
	Q(GetPhysicalDeviceSurfaceSupportKHR)
	Q(GetPhysicalDeviceSurfaceCapabilitiesKHR)
	Q(GetPhysicalDeviceSurfaceCapabilities2KHR)
	Q(GetPhysicalDeviceSurfaceCapabilities2EXT)
	Q(GetPhysicalDeviceSurfaceFormatsKHR)
	Q(GetPhysicalDeviceSurfaceFormats2KHR)
	Q(GetPhysicalDeviceSurfacePresentModesKHR)
	Q(CreateSwapchainKHR)
	Q(DestroySwapchainKHR)
	Q(GetSwapchainImagesKHR)
	Q(AcquireNextImageKHR)
	Q(AcquireNextImage2KHR)
	Q(QueuePresentKHR)
	Q(RegisterDisplayEventEXT)
	Q(GetSwapchainCounterEXT)
#undef Q
#define S(n, fn) if (strcmp(name, n) == 0) return (PFN_vkVoidFunction)fn;
	S("vkAcquireXlibDisplayEXT", qd_succeed)
	S("vkAcquireDrmDisplayEXT", qd_succeed)
	S("vkReleaseDisplayEXT", qd_succeed)
	S("vkDisplayPowerControlEXT", qd_succeed)
	S("vkWaitForPresentKHR", qd_succeed)
	S("vkGetDrmDisplayEXT", qd_unsupported)
	S("vkRegisterDeviceEventEXT", qd_unsupported)
#undef S
	return NULL;
}
