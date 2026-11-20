// quest1_modifier_extfmt: venus's NV12 as an Android external format, sampled through a YCbCr conversion.
// Part of quest1_modifier.c (included by it, in this order: one translation unit).

// --- venus NV12 as an Android external format
//
// The blob's linear NV12 is not venus's (rows of the width rounded up to 128 bytes, UV plane after the rows rounded
// up to 32), and the blob samples venus NV12 only from a gralloc buffer of that layout, as an Android external
// format with a sampler YCbCr conversion (src/vdectest/vkahbtest.c). So for vrlink an NV12 image with the linear
// modifier is created as an external-format image (format UNDEFINED, VkExternalFormatANDROID); its dma-buf is
// imported as a gralloc NV12 buffer (the native handle of one of our own of the same size, with the fd replaced),
// dedicated to it; its YCbCr conversions and views take the external format instead of NV12.

#define QNV_AHB_FORMAT 0x23 // AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420: venus's NV12 with the video usage
// CPU read rarely, GPU sampled image, video encode, and Qualcomm gralloc's private "uncached" (bit 29, an
// AHardwareBuffer vendor bit, GRALLOC_USAGE_PRIVATE_UNCACHED). With "CPU read often" the frames were cached ION and
// venus's driver invalidated the whole 6 MB frame from the caches at each DQBUF: ~2.1 ms of vrlink's decoder thread a
// frame (ftrace, 2026-11-20), for memory only the GPU reads (the shim's flash check reads ~640 bytes of it, fine
// uncached). No CPU usage, or "rarely" alone, still gave cached buffers (tried: the same 2.4 ms); with bit 29 the
// frame reaches vrlink 0.2 ms after venus is done (p90 0.3). Steam Link 2D's frames are uncached for the same reason
// (quest1_streamdrm.c).
#define QNV_AHB_USAGE (2ull | (1ull << 8) | (1ull << 16) | (1ull << 29))

static struct
{
	VkImage image;
	uint32_t w, h;
	VkDeviceSize pitch, uv; //!< the caller's plane layouts
	uint64_t mod;           //!< the caller's modifier
	const uint8_t *map;     //!< vrcompositor's: the imported dma-buf, read-only, for qcp_hold (venus's layout)
	size_t map_len;
} qnv[128]; // (vrcompositor keeps the last sets of vrlink while it makes a new one at each decoder reset)
static atomic_int qnv_n;

static int qnv_find(VkImage image)
{
	if (!image || !atomic_load(&qnv_n))
		return -1;
	int found = -1;
	pthread_mutex_lock(&qdb_lock);
	for (unsigned i = 0; i < sizeof(qnv) / sizeof(qnv[0]) && found < 0; i++)
		if (qnv[i].image == image)
			found = (int)i;
	pthread_mutex_unlock(&qdb_lock);
	return found;
}

static void qnv_forget(VkImage image)
{
	if (!atomic_load(&qnv_n))
		return;
	const uint8_t *map = NULL;
	size_t len = 0;
	pthread_mutex_lock(&qdb_lock);
	for (unsigned i = 0; i < sizeof(qnv) / sizeof(qnv[0]); i++)
		if (qnv[i].image == image) {
			qnv[i].image = VK_NULL_HANDLE;
			map = qnv[i].map;
			len = qnv[i].map_len;
			qnv[i].map = NULL;
			atomic_fetch_sub(&qnv_n, 1);
		}
	pthread_mutex_unlock(&qdb_lock);
	if (map)
		munmap((void *)map, len);
}

// Images with a modifier and NV12 images are forgotten when destroyed: else only a new image that got the same
// handle took an entry back, and vrcompositor ran out of them after a few of vrlink's decoder resets (2026-11-11:
// "Unable to create VkImage", and vrlink waited for its texture forever).
static void qcp_forget(VkDevice device, VkImage nv12);

static VKAPI_ATTR void VKAPI_CALL DestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *alloc)
{
	struct device *d = device_of(device);
	if (image)
		qcp_forget(device, image); // (its RGBA copy's view of it goes first)
	((PFN_vkDestroyImage)d->gdpa(device, "vkDestroyImage"))(device, image, alloc);
	if (image)
		qdb_mod_forget(image);
}

//! The external format of venus NV12 gralloc buffers (and its features), from a small one; 0 if there is none.
static uint64_t qnv_external_format(struct device *d, VkDevice device, VkFormatFeatureFlags *features)
{
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	static uint64_t fmt;
	static VkFormatFeatureFlags feat;
	static bool tried;
	pthread_mutex_lock(&lock);
	if (!tried) {
		tried = true;
		pthread_once(&qdb.once, qdb_load);
		PFN_vkGetAndroidHardwareBufferPropertiesANDROID props_of = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)d->gdpa(
		    device, "vkGetAndroidHardwareBufferPropertiesANDROID");
		qdb_desc md = {64, 64, 1, QNV_AHB_FORMAT, QNV_AHB_USAGE, 0, 0, 0};
		AHardwareBuffer *b = NULL;
		if (props_of && qdb.allocate && qdb.allocate(&md, &b) == 0) {
			VkAndroidHardwareBufferFormatPropertiesANDROID fp = {
			    VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
			VkAndroidHardwareBufferPropertiesANDROID bp = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
			                                               &fp};
			if (props_of(device, b, &bp) == VK_SUCCESS) {
				fmt = fp.externalFormat;
				feat = fp.formatFeatures;
			}
			qdb.release(b);
		}
		LOG("dma-buf: venus NV12 is external format 0x%llx (features 0x%x)\n", (unsigned long long)fmt, feat);
	}
	pthread_mutex_unlock(&lock);
	if (features)
		*features = feat;
	return fmt;
}

static VkResult qnv_create_image(struct device *d, VkDevice device, const VkImageCreateInfo *ci,
                                 const VkImageDrmFormatModifierExplicitCreateInfoEXT *ex,
                                 const VkAllocationCallbacks *alloc, VkImage *out)
{
	qnv_prewarm();
	uint64_t xfmt = qnv_external_format(d, device, NULL);
	if (!xfmt || ex->pPlaneLayouts[0].offset != 0) {
		LOG("dma-buf: NV12 image %ux%u: %s\n", ci->extent.width, ci->extent.height,
		    xfmt ? "first plane not at 0" : "no external format");
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}
	VkExternalFormatANDROID xf = {VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID, NULL, xfmt};
	VkExternalMemoryImageCreateInfo emi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &xf,
	                                       VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
	VkImageCreateInfo c = *ci;
	c.pNext = &emi; // nothing of the caller's chain applies to this image (format lists, the modifier)
	c.flags = 0;
	c.format = VK_FORMAT_UNDEFINED;
	c.tiling = VK_IMAGE_TILING_OPTIMAL;
	c.usage = VK_IMAGE_USAGE_SAMPLED_BIT; // all an external format allows
	VkResult r = d->CreateImage(device, &c, alloc, out);
	if (r != VK_SUCCESS) {
		LOG("dma-buf: NV12 image %ux%u as external format 0x%llx: %d\n", ci->extent.width, ci->extent.height,
		    (unsigned long long)xfmt, r);
		return r;
	}
	qdb_take_offset(*out);
	qdb_mod_forget(*out);
	qnv_forget(*out);
	unsigned i = 0;
	pthread_mutex_lock(&qdb_lock);
	while (i < sizeof(qnv) / sizeof(qnv[0]) && qnv[i].image)
		i++;
	if (i < sizeof(qnv) / sizeof(qnv[0])) {
		qnv[i].image = *out;
		qnv[i].w = ci->extent.width;
		qnv[i].h = ci->extent.height;
		qnv[i].pitch = ex->pPlaneLayouts[0].rowPitch;
		qnv[i].uv = ex->pPlaneLayouts[1].offset;
		qnv[i].mod = ex->drmFormatModifier;
		atomic_fetch_add(&qnv_n, 1);
	}
	pthread_mutex_unlock(&qdb_lock);
	if (i == sizeof(qnv) / sizeof(qnv[0])) {
		LOG("dma-buf: NV12 image %ux%u: %u of them already\n", ci->extent.width, ci->extent.height, i);
		((PFN_vkDestroyImage)d->gdpa(device, "vkDestroyImage"))(device, *out, alloc);
		*out = VK_NULL_HANDLE;
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	if (qdb_logs++ < 16)
		LOG("dma-buf: NV12 image %p %ux%u (modifier 0x%llx, usage 0x%x flags 0x%x, pitch %llu, UV at %llu): external format\n",
		    (void *)*out, ci->extent.width, ci->extent.height, (unsigned long long)ex->drmFormatModifier, ci->usage, ci->flags,
		    (unsigned long long)ex->pPlaneLayouts[0].rowPitch, (unsigned long long)ex->pPlaneLayouts[1].offset);
	return VK_SUCCESS;
}

// The gralloc buffer whose handle an imported NV12 dma-buf takes as a model, and its description as gralloc gives it
// (its stride, ...: the handle is checked against it). Kept: making one for each import was a 6 MB buffer each
// time, while vrlink waits for vrcompositor to import its 16 frame buffers.
static AHardwareBuffer *qnv_model(unsigned w, unsigned h, uint64_t usage, qdb_desc *md)
{
	static struct
	{
		AHardwareBuffer *b;
		unsigned w, h;
		uint64_t usage;
		qdb_desc md;
	} models[4]; // (two usages are tried, for one size in practice)
	AHardwareBuffer *b = NULL;
	pthread_mutex_lock(&qdb_lock);
	unsigned k = 0;
	for (; k < 4 && models[k].b; k++)
		if (models[k].w == w && models[k].h == h && models[k].usage == usage) {
			b = models[k].b;
			*md = models[k].md;
			break;
		}
	if (!b && k < 4) {
		qdb_desc d = {w, h, 1, QNV_AHB_FORMAT, usage, 0, 0, 0};
		if (qdb.allocate(&d, &b) == 0) {
			if (qdb.describe)
				qdb.describe(b, &d);
			*md = d;
			models[k].b = b;
			models[k].w = w;
			models[k].h = h;
			models[k].usage = usage;
			models[k].md = d;
		} else {
			b = NULL;
		}
	}
	pthread_mutex_unlock(&qdb_lock);
	return b;
}

// The dma-buf of each NV12 import, for vkGetMemoryFdKHR: vrcompositor exports what it imported again ("prepare image
// for export", as a cross-process shared resource), which the blob cannot do for memory of a gralloc buffer; without
// it every frame vrlink submits is an invalid texture (2026-11-11).
static struct
{
	VkDeviceMemory mem;
	int fd;
} qnv_imp[128];
static atomic_int qnv_nimp;

static void qsp_image_imported(VkImage image, int fd); // quest1_modifier_pose.c

static void qnv_imported(VkDeviceMemory m, int fd)
{
	int own = fcntl(fd, F_DUPFD_CLOEXEC, 0);
	unsigned k = 0;
	pthread_mutex_lock(&qdb_lock);
	while (k < sizeof(qnv_imp) / sizeof(qnv_imp[0]) && qnv_imp[k].mem)
		k++;
	if (own >= 0 && k < sizeof(qnv_imp) / sizeof(qnv_imp[0])) {
		qnv_imp[k].mem = m;
		qnv_imp[k].fd = own;
		atomic_fetch_add(&qnv_nimp, 1);
		own = -1;
	}
	pthread_mutex_unlock(&qdb_lock);
	if (own >= 0)
		close(own);
}

//! A new fd of the dma-buf an NV12 import was made of, -1 if m is none.
static int qnv_import_fd(VkDeviceMemory m)
{
	int fd = -1;
	if (!m || !atomic_load(&qnv_nimp))
		return -1;
	pthread_mutex_lock(&qdb_lock);
	for (unsigned k = 0; k < sizeof(qnv_imp) / sizeof(qnv_imp[0]) && fd < 0; k++)
		if (qnv_imp[k].mem == m)
			fd = fcntl(qnv_imp[k].fd, F_DUPFD_CLOEXEC, 0);
	pthread_mutex_unlock(&qdb_lock);
	return fd;
}

//! vkAllocateMemory of a dma-buf for an NV12 image of ours (dedicated to it): true if it was one (*r is the result).
static bool qnv_import(struct device *d, VkDevice device, const VkMemoryAllocateInfo *ai, int fd,
                       const VkAllocationCallbacks *alloc, VkDeviceMemory *out, VkResult *r)
{
	const VkMemoryDedicatedAllocateInfo *ded = find_struct(ai->pNext, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
	int i = ded ? qnv_find(ded->image) : -1;
	if (i < 0)
		return false;
	*r = VK_ERROR_INVALID_EXTERNAL_HANDLE;
	pthread_once(&qdb.once, qdb_load);
	PFN_vkGetAndroidHardwareBufferPropertiesANDROID props_of =
	    (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)d->gdpa(device, "vkGetAndroidHardwareBufferPropertiesANDROID");
	AHardwareBuffer *b = NULL;
	off_t size = lseek(fd, 0, SEEK_END);
	// gralloc takes the buffer only when its size is exactly the one it gives that frame: with the video usage it is
	// venus's own (room for the decoder's metadata after the planes), without it just the two planes
	static const uint64_t usages[] = {QNV_AHB_USAGE, QNV_AHB_USAGE & ~(1ull << 16)};
	int sizes[2] = {-1, -1};
	for (unsigned u = 0; u < 2 && !b && props_of && qdb.allocate; u++) {
		qdb_desc md;
		AHardwareBuffer *model = qnv_model(qnv[i].w, qnv[i].h, usages[u], &md);
		if (!model)
			continue;
		const qdb_handle *m = qdb.native_handle(model);
		size_t bytes = sizeof(*m) + (size_t)(m->numFds + m->numInts) * sizeof(int);
		qdb_handle *h = m->numFds >= 1 && m->numInts > QDB_SIZE ? malloc(bytes) : NULL;
		if (h) {
			memcpy(h, m, bytes);
			const int *ints = h->data + h->numFds;
			sizes[u] = ints[QDB_SIZE];
			if (ints[QDB_SIZE] == size) {
				h->data[0] = fd; // the metadata fd stays the model's: the clone duplicates both
				int e = qdb.from_handle(&md, h, QDB_CLONE, &b);
				if (e != 0) {
					LOG("dma-buf: NV12 %ux%u: gralloc refuses the handle (%d, usage 0x%llx)\n", qnv[i].w, qnv[i].h, e,
					    (unsigned long long)usages[u]);
					b = NULL;
				}
			}
			free(h);
		}
	}
	if (!b) {
		LOG("dma-buf: NV12 %ux%u: a dma-buf of %lld bytes, gralloc's are %d and %d\n", qnv[i].w, qnv[i].h,
		    (long long)size, sizes[0], sizes[1]);
		return true;
	}
	VkAndroidHardwareBufferPropertiesANDROID props = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
	*r = props_of(device, b, &props);
	if (*r == VK_SUCCESS && props.memoryTypeBits) {
		VkMemoryDedicatedAllocateInfo dd = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, ded->image};
		VkImportAndroidHardwareBufferInfoANDROID imp = {VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, &dd, b};
		VkMemoryAllocateInfo mi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, props.allocationSize,
		                           (uint32_t)__builtin_ctz(props.memoryTypeBits)};
		*r = d->AllocateMemory(device, &mi, alloc, out);
		if (*r == VK_SUCCESS) {
			qnv_imported(*out, fd);
			qsp_image_imported(ded->image, fd); // which of vrlink's buffers it is (quest1_modifier_pose.c)
			// the frame's luma for qcp_hold
			void *m = mmap(NULL, (size_t)size, PROT_READ, MAP_SHARED, fd, 0);
			if (m != MAP_FAILED) {
				pthread_mutex_lock(&qdb_lock);
				if (qnv[i].image == ded->image && !qnv[i].map) {
					qnv[i].map = m;
					qnv[i].map_len = (size_t)size;
					m = MAP_FAILED;
				}
				pthread_mutex_unlock(&qdb_lock);
				if (m != MAP_FAILED)
					munmap(m, (size_t)size);
			}
		}
	} else if (*r == VK_SUCCESS) {
		*r = VK_ERROR_INVALID_EXTERNAL_HANDLE;
	}
	qdb.release(b);
	if (qdb_logs++ < 16 || *r != VK_SUCCESS)
		LOG("dma-buf: NV12 %ux%u (dma-buf of %lld bytes) imported as a gralloc buffer: %d\n", qnv[i].w, qnv[i].h,
		    (long long)size, *r);
	return true;
}
