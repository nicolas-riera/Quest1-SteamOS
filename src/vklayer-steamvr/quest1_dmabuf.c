// quest1_dmabuf: Vulkan memory from a dma-buf the Adreno blob does not take (included by quest1_compat.c).
//
// The blob imports external memory as its own OPAQUE_FD or as gralloc buffers (AHardwareBuffer), not as any
// dma-buf: an ION buffer (the venus decoder's frames) given as OPAQUE_FD is refused with
// VK_ERROR_INVALID_EXTERNAL_HANDLE. Such an import, once refused, is made again here with a gralloc BLOB buffer
// (plain memory) around the same dma-buf: AHardwareBuffer_createFromHandle on the native handle of a small BLOB
// buffer of ours, with its fd and size replaced.
//
// The images bound to that memory must not be created as external ones: the blob refuses to bind an OPAQUE_FD image
// to AHB memory, and an AHB image wants a dedicated import of a buffer of its own format, whereas a plain image binds
// at any offset (a plane of a frame). So linear images created for OPAQUE_FD memory are created plain: vrclient's
// and vrcompositor's shared images are optimal, and linear OPAQUE_FD images come from the stream client's dma-buf
// import (quest1_angle_egl.c imports each plane of a frame with GL_EXT_memory_object_fd and linear tiling).
// Checked on the headset (2026-11-11): linear R8 and RG8 images bound at the planes' offsets of a venus NV12 frame
// read back exact, on a BLOB buffer and on a plain ION buffer made one this way.

typedef struct
{
	uint32_t width, height, layers, format;
	uint64_t usage;
	uint32_t stride, rfu0;
	uint64_t rfu1;
} qdb_desc;
typedef struct
{
	int version, numFds, numInts;
	int data[];
} qdb_handle;
#define QDB_FORMAT_BLOB 0x21
#define QDB_USAGE (0x3ull | 0x30ull | (1ull << 24)) // CPU read and write often, GPU data buffer
#define QDB_MODEL_SIZE 4096
#define QDB_CLONE 3 // AHARDWAREBUFFER_CREATE_FROM_HANDLE_METHOD_CLONE: the handle's fds are duplicated
// Qualcomm's private_handle_t after its two fds: magic, flags, width, height, unaligned width, unaligned height,
// format, buffer type, size, ...
enum { QDB_MAGIC = 0, QDB_WIDTH = 2, QDB_UWIDTH = 4, QDB_FORMAT = 6, QDB_SIZE = 8 };

static struct
{
	pthread_once_t once;
	int (*from_handle)(const qdb_desc *, const qdb_handle *, int, AHardwareBuffer **);
	void (*release)(AHardwareBuffer *);
	const qdb_handle *(*native_handle)(const AHardwareBuffer *);
	AHardwareBuffer *model; //!< a BLOB buffer of ours: its native handle is the model of the others
} qdb = {PTHREAD_ONCE_INIT};

static void qdb_load(void)
{
	// as qs_load (quest1_scanout.c): libhybris-common is already loaded with the hybris libvulkan
	void *hc = dlopen("libhybris-common.so.1", RTLD_NOW | RTLD_NOLOAD);
	if (!hc)
		hc = dlopen("/opt/hybris/lib/libhybris-common.so.1", RTLD_NOW | RTLD_NOLOAD);
	void *lookup = hc ? hc : RTLD_DEFAULT;
	void *(*adlopen)(const char *, int) = (void *(*)(const char *, int))dlsym(lookup, "android_dlopen");
	void *(*adlsym)(void *, const char *) = (void *(*)(void *, const char *))dlsym(lookup, "android_dlsym");
	void *nw = adlopen && adlsym ? adlopen("libnativewindow.so", RTLD_NOW) : NULL;
	int (*allocate)(const qdb_desc *, AHardwareBuffer **) = NULL;
	if (nw) {
		*(void **)&allocate = adlsym(nw, "AHardwareBuffer_allocate");
		*(void **)&qdb.from_handle = adlsym(nw, "AHardwareBuffer_createFromHandle");
		*(void **)&qdb.release = adlsym(nw, "AHardwareBuffer_release");
		*(void **)&qdb.native_handle = adlsym(nw, "AHardwareBuffer_getNativeHandle");
	}
	qdb_desc d = {QDB_MODEL_SIZE, 1, 1, QDB_FORMAT_BLOB, QDB_USAGE, 0, 0, 0};
	if (!allocate || !qdb.from_handle || !qdb.release || !qdb.native_handle || allocate(&d, &qdb.model) != 0) {
		LOG("dma-buf import: no gralloc BLOB buffers\n");
		qdb.model = NULL;
		return;
	}
	// a handle laid out as expected, or none of this
	const qdb_handle *h = qdb.native_handle(qdb.model);
	const int *ints = h->data + h->numFds;
	if (h->numFds != 2 || h->numInts <= QDB_SIZE || ints[QDB_MAGIC] != 0x676d736d || ints[QDB_FORMAT] != QDB_FORMAT_BLOB ||
	    ints[QDB_WIDTH] != QDB_MODEL_SIZE || ints[QDB_UWIDTH] != QDB_MODEL_SIZE || ints[QDB_SIZE] != QDB_MODEL_SIZE) {
		LOG("dma-buf import: gralloc handle not as expected (%d fds, %d ints)\n", h->numFds, h->numInts);
		qdb.release(qdb.model);
		qdb.model = NULL;
	}
}

//! A gralloc BLOB buffer of size bytes on dma-buf fd (whose fd it duplicates), or NULL.
static AHardwareBuffer *qdb_wrap(int fd, uint32_t size)
{
	pthread_once(&qdb.once, qdb_load);
	if (!qdb.model)
		return NULL;
	const qdb_handle *m = qdb.native_handle(qdb.model);
	size_t bytes = sizeof(*m) + (size_t)(m->numFds + m->numInts) * sizeof(int);
	qdb_handle *h = malloc(bytes);
	if (!h)
		return NULL;
	memcpy(h, m, bytes);
	h->data[0] = fd; // the metadata fd stays the model's: the clone duplicates it
	int *ints = h->data + h->numFds;
	ints[QDB_WIDTH] = ints[QDB_UWIDTH] = ints[QDB_SIZE] = (int)size;
	qdb_desc d = {size, 1, 1, QDB_FORMAT_BLOB, QDB_USAGE, 0, 0, 0};
	AHardwareBuffer *b = NULL;
	int e = qdb.from_handle(&d, h, QDB_CLONE, &b);
	free(h);
	return e == 0 ? b : NULL;
}

//! A duplicate of the fd of ai's fd import (OPAQUE_FD or DMA_BUF), -1 if there is none. The blob closes the fd of
//! an import it refuses (as it would on success), so the retry below needs its own.
static int qdb_keep(const VkMemoryAllocateInfo *ai)
{
	const VkImportMemoryFdInfoKHR *imp = find_struct(ai->pNext, VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR);
	if (!imp || imp->fd < 0 ||
	    (imp->handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT &&
	     imp->handleType != VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT))
		return -1;
	return fcntl(imp->fd, F_DUPFD_CLOEXEC, 0);
}

//! vkAllocateMemory of an fd import the blob refused (fd: qdb_keep's duplicate, still the caller's to close): the
//! same dma-buf, as a gralloc buffer.
static VkResult qdb_import(struct device *d, VkDevice device, const VkMemoryAllocateInfo *ai, int fd,
                           const VkAllocationCallbacks *alloc, VkDeviceMemory *out)
{
	static int failed;
	off_t size = lseek(fd, 0, SEEK_END);
	PFN_vkGetAndroidHardwareBufferPropertiesANDROID props_of =
	    (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)d->gdpa(device, "vkGetAndroidHardwareBufferPropertiesANDROID");
	AHardwareBuffer *b = NULL;
	if (size > 0 && size <= INT32_MAX && (VkDeviceSize)size >= ai->allocationSize && props_of)
		b = qdb_wrap(fd, (uint32_t)size);
	if (!b) {
		if (failed++ < 4)
			LOG("dma-buf import of %llu bytes: not as a gralloc buffer either (fd of %lld bytes%s%s)\n",
			    (unsigned long long)ai->allocationSize, (long long)size,
			    props_of ? "" : ", no vkGetAndroidHardwareBufferPropertiesANDROID", qdb.model ? "" : ", no model buffer");
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;
	}
	VkAndroidHardwareBufferPropertiesANDROID props = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
	VkResult r = props_of(device, b, &props);
	if (r == VK_SUCCESS && props.memoryTypeBits) {
		VkImportAndroidHardwareBufferInfoANDROID ahb = {VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
		ahb.buffer = b;
		VkMemoryAllocateInfo m = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ahb};
		m.allocationSize = props.allocationSize;
		m.memoryTypeIndex = (uint32_t)__builtin_ctz(props.memoryTypeBits);
		r = d->AllocateMemory(device, &m, alloc, out);
	} else if (r == VK_SUCCESS) {
		r = VK_ERROR_INVALID_EXTERNAL_HANDLE;
	}
	qdb.release(b); // the memory holds its own reference
	static int logged;
	if (logged++ < 4 || r != VK_SUCCESS)
		LOG("dma-buf of %lld bytes imported as a gralloc buffer: %d\n", (long long)size, r);
	return r;
}

// --- the images of the planes
//
// ANGLE binds an image made on an OpenGL memory object at offset 0, whatever offset glTexStorageMem2DEXT is given. So
// quest1_angle_egl.c puts the plane's offset in a structure of ours chained to ANGLE's VkImageCreateInfo
// (glTexStorageMemFlags2DANGLE takes such a chain; same values there): the image is bound at that offset. The
// structure goes no further than this layer.
#define QDB_STRUCTURE_TYPE_PLANE_OFFSET ((VkStructureType)0x51310001)
typedef struct
{
	VkStructureType sType;
	const void *pNext;
	uint64_t offset;
} qdb_plane_offset;

// images to bind at an offset, until they are bound
static struct
{
	VkImage image;
	VkDeviceSize offset;
} qdb_planes[64];
static atomic_int qdb_nplanes; //!< entries in use
static pthread_mutex_t qdb_lock = PTHREAD_MUTEX_INITIALIZER;

//! The offset image is to be bound at (once: the entry goes), 0 for any other image.
static VkDeviceSize qdb_take_offset(VkImage image)
{
	if (!atomic_load(&qdb_nplanes))
		return 0;
	VkDeviceSize off = 0;
	pthread_mutex_lock(&qdb_lock);
	for (unsigned i = 0; i < sizeof(qdb_planes) / sizeof(qdb_planes[0]); i++)
		if (qdb_planes[i].image == image) {
			off = qdb_planes[i].offset;
			qdb_planes[i].image = VK_NULL_HANDLE;
			atomic_fetch_sub(&qdb_nplanes, 1);
		}
	pthread_mutex_unlock(&qdb_lock);
	return off;
}

//! vkCreateImage: a linear image for OPAQUE_FD memory (a plane of a dma-buf, see above) is created plain, and
//! the offset it comes with is kept for its binding.
static VkResult qdb_create_image(struct device *d, VkDevice device, const VkImageCreateInfo *ci,
                                 const VkAllocationCallbacks *alloc, VkImage *out)
{
	const VkExternalMemoryImageCreateInfo *e = find_struct(ci->pNext, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
	const qdb_plane_offset *plane = find_struct(ci->pNext, QDB_STRUCTURE_TYPE_PLANE_OFFSET);
	bool plain = ci->tiling == VK_IMAGE_TILING_LINEAR && e &&
	             (e->handleTypes & (VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT | VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT));
	if (!plain && !plane) {
		VkResult r = d->CreateImage(device, ci, alloc, out);
		if (r == VK_SUCCESS)
			qdb_take_offset(*out); // the handle of an image destroyed before it was bound, if one was
		return r;
	}
	// those two out of the chain for the call (as WITH_UNLINKED does), then back
	VkBaseOutStructure *cut[2], *before[2];
	int nc = 0;
	VkBaseOutStructure *prev = (VkBaseOutStructure *)ci;
	for (VkBaseOutStructure *s = prev->pNext; s && nc < 2;) {
		VkBaseOutStructure *next = s->pNext;
		if ((plain && s->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO) ||
		    s->sType == QDB_STRUCTURE_TYPE_PLANE_OFFSET) {
			cut[nc] = s;
			before[nc++] = prev;
			prev->pNext = next;
		} else {
			prev = s;
		}
		s = next;
	}
	VkResult r = d->CreateImage(device, ci, alloc, out);
	for (int i = nc - 1; i >= 0; i--) {
		cut[i]->pNext = before[i]->pNext;
		before[i]->pNext = cut[i];
	}
	if (r != VK_SUCCESS || !plane)
		return r;
	qdb_take_offset(*out);
	pthread_mutex_lock(&qdb_lock);
	unsigned i = 0;
	while (i < sizeof(qdb_planes) / sizeof(qdb_planes[0]) && qdb_planes[i].image)
		i++;
	if (i < sizeof(qdb_planes) / sizeof(qdb_planes[0])) {
		qdb_planes[i].image = *out;
		qdb_planes[i].offset = plane->offset;
		atomic_fetch_add(&qdb_nplanes, 1);
	}
	pthread_mutex_unlock(&qdb_lock);
	if (i == sizeof(qdb_planes) / sizeof(qdb_planes[0])) {
		LOG("dma-buf plane at %llu: too many images waiting for their memory\n", (unsigned long long)plane->offset);
		((PFN_vkDestroyImage)d->gdpa(device, "vkDestroyImage"))(device, *out, alloc);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL BindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory,
                                                      VkDeviceSize offset)
{
	return device_of(device)->BindImageMemory(device, image, memory, offset + qdb_take_offset(image));
}

static VKAPI_ATTR VkResult VKAPI_CALL BindImageMemory2(VkDevice device, uint32_t n, const VkBindImageMemoryInfo *bi)
{
	struct device *d = device_of(device);
	if (!atomic_load(&qdb_nplanes) || n > 16)
		return d->BindImageMemory2(device, n, bi);
	VkBindImageMemoryInfo copy[16];
	for (uint32_t i = 0; i < n; i++) {
		copy[i] = bi[i];
		copy[i].memoryOffset += qdb_take_offset(bi[i].image);
	}
	return d->BindImageMemory2(device, n, copy);
}
