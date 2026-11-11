// libquest1_streamdrm.so: libdrm.so.2 for Steam's stream client, and for it only (it is found under that name in
// /usr/local/lib/quest1-streamclient, see quest1_streamclient.c).
//
// The client offers a video codec to the PC only when (1) libdrm shows it a display: a device with a primary node
// it can open, at least one CRTC, and a connected connector with a mode, an encoder and that CRTC; and (2) a V4L2
// decoder lists the codec. Without (1) it says "No supported video codec, will not stream video" and the picture
// stays black, although in VR it never draws on that display: the frames go to the SteamVR overlay through OpenGL.
// The headset's display is not a DRM one (/dev/dri is empty), so this library shows the client one display that
// does nothing: the panels' size and rate, on a node that is /dev/null. Everything that would draw on it fails.
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define EXPORT __attribute__((visibility("default")))

// libdrm's structures, as far as the client reads them (xf86drm.h, xf86drmMode.h)
struct device {
	char **nodes;
	int available_nodes;
	int bustype;
	void *businfo, *deviceinfo;
};
struct mode_info {
	uint32_t clock;
	uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
	uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
	uint32_t vrefresh, flags, type;
	char name[32];
};
struct res {
	int count_fbs;
	uint32_t *fbs;
	int count_crtcs;
	uint32_t *crtcs;
	int count_connectors;
	uint32_t *connectors;
	int count_encoders;
	uint32_t *encoders;
	uint32_t min_width, max_width, min_height, max_height;
};
struct connector {
	uint32_t connector_id, encoder_id, connector_type, connector_type_id, connection, mm_width, mm_height, subpixel;
	int count_modes;
	struct mode_info *modes;
	int count_props;
	uint32_t *props;
	uint64_t *prop_values;
	int count_encoders;
	uint32_t *encoders;
};
struct encoder {
	uint32_t encoder_id, encoder_type, crtc_id, possible_crtcs, possible_clones;
};
struct crtc {
	uint32_t crtc_id, buffer_id, x, y, width, height;
	int mode_valid;
	struct mode_info mode;
	int gamma_size;
};

enum { CRTC = 31, ENCODER = 32, CONNECTOR = 33, W = 1920, H = 1080, HZ = 72 };

static char node[] = "/dev/null";
static char *nodes[4] = {node, NULL, NULL, NULL};
static struct device the_device = {.nodes = nodes, .available_nodes = 1 /* the primary node */, .bustype = 2};
static uint32_t crtcs[1] = {CRTC}, encoders[1] = {ENCODER}, connectors[1] = {CONNECTOR};
static struct mode_info the_mode = {.clock = (W + 160) * (H + 40) * HZ / 1000,
                                    .hdisplay = W, .hsync_start = W + 48, .hsync_end = W + 80, .htotal = W + 160,
                                    .vdisplay = H, .vsync_start = H + 3, .vsync_end = H + 8, .vtotal = H + 40,
                                    .vrefresh = HZ, .type = 0x48 /* preferred, driver */, .name = "1920x1080"};

static int fail(void)
{
	errno = ENOSYS;
	return -1;
}

EXPORT int drmGetDevices2(uint32_t flags, struct device **devices, int max)
{
	(void)flags;
	if (devices && max > 0)
		devices[0] = &the_device;
	return 1;
}
EXPORT void drmFreeDevices(struct device **devices, int count)
{
	(void)devices;
	(void)count;
}

EXPORT struct res *drmModeGetResources(int fd)
{
	(void)fd;
	struct res *r = calloc(1, sizeof(*r));
	if (!r)
		return NULL;
	r->count_crtcs = r->count_connectors = r->count_encoders = 1;
	r->crtcs = crtcs;
	r->connectors = connectors;
	r->encoders = encoders;
	r->max_width = W;
	r->max_height = H;
	return r;
}
EXPORT struct connector *drmModeGetConnector(int fd, uint32_t id)
{
	(void)fd;
	struct connector *c = id == CONNECTOR ? calloc(1, sizeof(*c)) : NULL;
	if (!c)
		return NULL;
	c->connector_id = CONNECTOR;
	c->encoder_id = ENCODER;
	c->connector_type = 14; // eDP
	c->connector_type_id = 1;
	c->connection = 1; // connected
	c->count_modes = 1;
	c->modes = &the_mode;
	c->count_encoders = 1;
	c->encoders = encoders;
	return c;
}
EXPORT struct encoder *drmModeGetEncoder(int fd, uint32_t id)
{
	(void)fd;
	struct encoder *e = id == ENCODER ? calloc(1, sizeof(*e)) : NULL;
	if (!e)
		return NULL;
	e->encoder_id = ENCODER;
	e->crtc_id = CRTC;
	e->possible_crtcs = 1;
	return e;
}
EXPORT struct crtc *drmModeGetCrtc(int fd, uint32_t id)
{
	(void)fd;
	struct crtc *c = id == CRTC ? calloc(1, sizeof(*c)) : NULL;
	if (!c)
		return NULL;
	c->crtc_id = CRTC;
	c->width = W;
	c->height = H;
	c->mode_valid = 1;
	c->mode = the_mode;
	return c;
}
#define FREE(name) \
	EXPORT void name(void *p) \
	{ \
		free(p); \
	}
FREE(drmModeFreeResources) FREE(drmModeFreeConnector) FREE(drmModeFreeEncoder) FREE(drmModeFreeCrtc)
FREE(drmModeFreePlaneResources) FREE(drmModeFreePlane) FREE(drmModeFreeObjectProperties) FREE(drmModeFreeProperty)
FREE(drmModeAtomicFree)

// nothing is ever drawn on this display
#define FAILS(name) \
	EXPORT int name(void) \
	{ \
		return fail(); \
	}
#define NOTHING(name) \
	EXPORT void *name(void) \
	{ \
		errno = ENOSYS; \
		return NULL; \
	}
FAILS(drmSetMaster) FAILS(drmSetClientCap) FAILS(drmModeSetCrtc) FAILS(drmModeAddFB2) FAILS(drmModeAddFB2WithModifiers)
FAILS(drmModeRmFB) FAILS(drmModeAtomicAddProperty) FAILS(drmModeAtomicCommit)
NOTHING(drmModeGetPlaneResources) NOTHING(drmModeGetPlane) NOTHING(drmModeObjectGetProperties)
NOTHING(drmModeGetProperty) NOTHING(drmModeAtomicAlloc)

// ANGLE's libGLESv2 is linked with libdrm for these two (the driver's name of a DRM device it is given): none here
NOTHING(drmGetVersion)
FREE(drmFreeVersion)

// --- "dumb" buffers: the client makes the decoder's frame buffers on its display device and hands them to the
// V4L2 decoder as dma-bufs. The venus decoder only takes ION buffers, so that is what they are here.
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct ion_allocation_data {
	size_t len, align;
	unsigned int heap_id_mask, flags;
	int handle;
};
struct ion_fd_data {
	int handle, fd;
};
#define ION_IOC_ALLOC _IOWR('I', 0, struct ion_allocation_data)
#define ION_IOC_FREE _IOWR('I', 1, int)
#define ION_IOC_SHARE _IOWR('I', 4, struct ion_fd_data)
#define ION_SYSTEM_HEAP_ID 25
#define ION_FLAG_CACHED 1

struct create_dumb {
	uint32_t height, width, bpp, flags, handle, pitch;
	uint64_t size;
};
struct prime_handle {
	uint32_t handle, flags;
	int32_t fd;
};
#define IOC_CREATE_DUMB _IOWR('d', 0xB2, struct create_dumb)
#define IOC_DESTROY_DUMB _IOWR('d', 0xB4, uint32_t)
#define IOC_PRIME_HANDLE_TO_FD _IOWR('d', 0x2d, struct prime_handle)
#define IOC_GEM_CLOSE _IOW('d', 0x09, uint64_t)

enum { NBUF = 64 };
static int bufs[NBUF]; // the ION fd of handle i + 1, 0 when free
static int ion = -1;

EXPORT int drmIoctl(int fd, unsigned long req, void *arg)
{
	(void)fd;
	if (req == IOC_CREATE_DUMB) {
		struct create_dumb *c = arg;
		if (ion < 0)
			ion = open("/dev/ion", O_RDONLY | O_CLOEXEC);
		c->pitch = c->width * ((c->bpp + 7) / 8);
		c->size = ((uint64_t)c->pitch * c->height + 4095) & ~4095ull;
		struct ion_allocation_data a = {.len = c->size, .align = 4096, .heap_id_mask = 1u << ION_SYSTEM_HEAP_ID,
		                                .flags = ION_FLAG_CACHED};
		int slot = 0;
		while (slot < NBUF && bufs[slot])
			slot++;
		if (ion < 0 || slot == NBUF || ioctl(ion, ION_IOC_ALLOC, &a) < 0)
			return -1;
		struct ion_fd_data f = {.handle = a.handle};
		int r = ioctl(ion, ION_IOC_SHARE, &f);
		ioctl(ion, ION_IOC_FREE, &a.handle); // the fd keeps the buffer
		if (r < 0)
			return -1;
		fcntl(f.fd, F_SETFD, FD_CLOEXEC);
		bufs[slot] = f.fd;
		c->handle = (uint32_t)slot + 1;
		if (getenv("QUEST1_STREAMDRM_TRACE"))
			fprintf(stderr, "quest1_streamdrm: buffer %u: %ux%u, %u bits, %llu bytes\n", c->handle, c->width, c->height,
			        c->bpp, (unsigned long long)c->size);
		return 0;
	}
	if (req == IOC_PRIME_HANDLE_TO_FD) {
		struct prime_handle *p = arg;
		if (!p->handle || p->handle > NBUF || !bufs[p->handle - 1]) {
			errno = ENOENT;
			return -1;
		}
		p->fd = fcntl(bufs[p->handle - 1], F_DUPFD_CLOEXEC, 0);
		return p->fd < 0 ? -1 : 0;
	}
	if (req == IOC_DESTROY_DUMB || req == IOC_GEM_CLOSE) {
		uint32_t h = *(uint32_t *)arg;
		if (h && h <= NBUF && bufs[h - 1]) {
			close(bufs[h - 1]);
			bufs[h - 1] = 0;
		}
		return 0;
	}
	fprintf(stderr, "quest1_streamdrm: drmIoctl 0x%lx: not on this display\n", req);
	return fail();
}
