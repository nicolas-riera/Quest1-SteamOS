// quest1_modifier_export: NV12 memory made here and exported as a dma-buf (vrlink's frame buffers).
// Part of quest1_modifier.c (included by it, in this order: one translation unit).

// --- NV12 memory made here and exported as a dma-buf (vrlink's frame buffers)

static struct
{
	VkDeviceMemory mem;
	AHardwareBuffer *b; //!< ours: its fd is the dma-buf
	unsigned w, h;
} qnv_exp[128];
static atomic_int qnv_nexp;

// vrlink sets its 16 frame buffers up in its decoder thread, which takes no packet buffer back meanwhile, and gives
// up when the decoder holds all 16 (220 ms at 72 fps). A gralloc buffer of 6 MB took 13 to 66 ms to make, 1.1 s for
// the 16 (2026-11-11); the Frame's driver takes a few. So the buffers vrlink frees stay here for its next set (it
// sets them up again at each reset of its decoder), and 16 of its size are made in the background as soon as it
// asks about NV12, before the PC streams.
#define QNV_POOL 32
#define QNV_PREWARM_N 16
#define QNV_PREWARM_W 1024 // vrlink's atlas for the Quest 1 identity: four 1024x1024 views
#define QNV_PREWARM_H 4096
static struct
{
	AHardwareBuffer *b;
	unsigned w, h;
} qnv_pool[QNV_POOL];

static AHardwareBuffer *qnv_pool_take(unsigned w, unsigned h)
{
	AHardwareBuffer *b = NULL;
	pthread_mutex_lock(&qdb_lock);
	for (unsigned k = 0; k < QNV_POOL && !b; k++)
		if (qnv_pool[k].b && qnv_pool[k].w == w && qnv_pool[k].h == h) {
			b = qnv_pool[k].b;
			qnv_pool[k].b = NULL;
		}
	pthread_mutex_unlock(&qdb_lock);
	return b;
}

static void qnv_pool_put(AHardwareBuffer *b, unsigned w, unsigned h)
{
	unsigned k = 0;
	pthread_mutex_lock(&qdb_lock);
	while (k < QNV_POOL && qnv_pool[k].b)
		k++;
	if (k < QNV_POOL) {
		qnv_pool[k].b = b;
		qnv_pool[k].w = w;
		qnv_pool[k].h = h;
	}
	pthread_mutex_unlock(&qdb_lock);
	if (k == QNV_POOL)
		qdb.release(b);
}

static void *qnv_prewarm_run(void *arg)
{
	(void)arg;
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	unsigned n = 0;
	for (; n < QNV_PREWARM_N; n++) {
		qdb_desc md = {QNV_PREWARM_W, QNV_PREWARM_H, 1, QNV_AHB_FORMAT, QNV_AHB_USAGE, 0, 0, 0};
		AHardwareBuffer *b = NULL;
		if (qdb.allocate(&md, &b) != 0)
			break;
		qnv_pool_put(b, QNV_PREWARM_W, QNV_PREWARM_H);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	LOG("dma-buf: %u NV12 %ux%u frame buffers made ahead in %.0f ms\n", n, QNV_PREWARM_W, QNV_PREWARM_H,
	    (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
	return NULL;
}

static void qnv_prewarm_once(void)
{
	pthread_once(&qdb.once, qdb_load);
	pthread_t t;
	if (qdb.allocate && pthread_create(&t, NULL, qnv_prewarm_run, NULL) == 0)
		pthread_detach(t);
}

//! vrlink's frame buffers, made in the background (once)
static void qnv_prewarm(void)
{
	static pthread_once_t once = PTHREAD_ONCE_INIT;
	if (qdb_vrlink())
		pthread_once(&once, qnv_prewarm_once);
}

//! vkAllocateMemory of exportable (dma-buf) memory for an NV12 image of ours: true if it was one (*r is the result).
static bool qnv_export(struct device *d, VkDevice device, const VkMemoryAllocateInfo *ai,
                       const VkAllocationCallbacks *alloc, VkDeviceMemory *out, VkResult *r)
{
	const VkExportMemoryAllocateInfo *ex = find_struct(ai->pNext, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
	const VkMemoryDedicatedAllocateInfo *ded = find_struct(ai->pNext, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
	int i = ex && (ex->handleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) && ded ? qnv_find(ded->image) : -1;
	if (i < 0)
		return false;
	pthread_once(&qdb.once, qdb_load);
	PFN_vkGetAndroidHardwareBufferPropertiesANDROID props_of =
	    (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)d->gdpa(device, "vkGetAndroidHardwareBufferPropertiesANDROID");
	qdb_desc md = {qnv[i].w, qnv[i].h, 1, QNV_AHB_FORMAT, QNV_AHB_USAGE, 0, 0, 0};
	AHardwareBuffer *b = props_of ? qnv_pool_take(qnv[i].w, qnv[i].h) : NULL;
	if (!b && (!props_of || !qdb.allocate || qdb.allocate(&md, &b) != 0)) {
		LOG("dma-buf: NV12 %ux%u: no gralloc buffer\n", qnv[i].w, qnv[i].h);
		*r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
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
	} else if (*r == VK_SUCCESS) {
		*r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}
	unsigned k = 0;
	if (*r == VK_SUCCESS) {
		pthread_mutex_lock(&qdb_lock);
		while (k < sizeof(qnv_exp) / sizeof(qnv_exp[0]) && qnv_exp[k].mem)
			k++;
		if (k < sizeof(qnv_exp) / sizeof(qnv_exp[0])) {
			qnv_exp[k].mem = *out;
			qnv_exp[k].b = b;
			qnv_exp[k].w = qnv[i].w;
			qnv_exp[k].h = qnv[i].h;
			atomic_fetch_add(&qnv_nexp, 1);
		}
		pthread_mutex_unlock(&qdb_lock);
		if (k == sizeof(qnv_exp) / sizeof(qnv_exp[0])) {
			d->FreeMemory(device, *out, alloc);
			*out = VK_NULL_HANDLE;
			*r = VK_ERROR_TOO_MANY_OBJECTS;
		}
	}
	if (*r != VK_SUCCESS)
		qdb.release(b);
	if (qdb_logs++ < 16 || *r != VK_SUCCESS)
		LOG("dma-buf: NV12 %ux%u exportable, a gralloc buffer of %llu bytes: %d\n", qnv[i].w, qnv[i].h,
		    (unsigned long long)props.allocationSize, *r);
	return true;
}

static AHardwareBuffer *qnv_exported(VkDeviceMemory m)
{
	AHardwareBuffer *b = NULL;
	if (!m || !atomic_load(&qnv_nexp))
		return NULL;
	pthread_mutex_lock(&qdb_lock);
	for (unsigned k = 0; k < sizeof(qnv_exp) / sizeof(qnv_exp[0]) && !b; k++)
		if (qnv_exp[k].mem == m)
			b = qnv_exp[k].b;
	pthread_mutex_unlock(&qdb_lock);
	return b;
}

//! After vkFreeMemory: our gralloc buffer of exported memory goes too.
static void qnv_free_memory(VkDeviceMemory m)
{
	if (m && atomic_load(&qnv_nimp)) {
		int fd = -1;
		pthread_mutex_lock(&qdb_lock);
		for (unsigned k = 0; k < sizeof(qnv_imp) / sizeof(qnv_imp[0]) && fd < 0; k++)
			if (qnv_imp[k].mem == m) {
				fd = qnv_imp[k].fd;
				qnv_imp[k].mem = VK_NULL_HANDLE;
				atomic_fetch_sub(&qnv_nimp, 1);
			}
		pthread_mutex_unlock(&qdb_lock);
		if (fd >= 0) {
			close(fd);
			return;
		}
	}
	if (!m || !atomic_load(&qnv_nexp))
		return;
	AHardwareBuffer *b = NULL;
	unsigned w = 0, h = 0;
	pthread_mutex_lock(&qdb_lock);
	for (unsigned k = 0; k < sizeof(qnv_exp) / sizeof(qnv_exp[0]) && !b; k++)
		if (qnv_exp[k].mem == m) {
			b = qnv_exp[k].b;
			w = qnv_exp[k].w;
			h = qnv_exp[k].h;
			qnv_exp[k].mem = VK_NULL_HANDLE;
			atomic_fetch_sub(&qnv_nexp, 1);
		}
	pthread_mutex_unlock(&qdb_lock);
	if (b)
		qnv_pool_put(b, w, h); // for vrlink's next set
}

static VKAPI_ATTR VkResult VKAPI_CALL GetMemoryFd(VkDevice device, const VkMemoryGetFdInfoKHR *gi, int *fd)
{
	struct device *d = device_of(device);
	if (gi->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT ||
	    gi->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT) {
		// an NV12 import: its dma-buf again, as either kind (an OPAQUE_FD one comes back to qnv_import)
		*fd = qnv_import_fd(gi->memory);
		if (*fd >= 0) {
			static atomic_int logs;
			if (atomic_fetch_add(&logs, 1) < 2)
				LOG("dma-buf: NV12 import exported again (handle type 0x%x)\n", gi->handleType);
			return VK_SUCCESS;
		}
	}
	if (gi->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) {
		AHardwareBuffer *b = qnv_exported(gi->memory);
		const qdb_handle *h = b ? qdb.native_handle(b) : NULL;
		*fd = h && h->numFds >= 1 ? fcntl(h->data[0], F_DUPFD_CLOEXEC, 0) : -1;
		if (*fd < 0)
			LOG("dma-buf: no fd for memory %p (not exportable here)\n", (void *)gi->memory);
		return *fd >= 0 ? VK_SUCCESS : VK_ERROR_TOO_MANY_OBJECTS;
	}
	PFN_vkGetMemoryFdKHR next = (PFN_vkGetMemoryFdKHR)d->gdpa(device, "vkGetMemoryFdKHR");
	return next ? next(device, gi, fd) : VK_ERROR_EXTENSION_NOT_PRESENT;
}

static VKAPI_ATTR void VKAPI_CALL GetImageMemoryRequirements(VkDevice device, VkImage image, VkMemoryRequirements *req)
{
	struct device *d = device_of(device);
	int i = qnv_find(image);
	if (i < 0) {
		d->GetImageMemoryRequirements(device, image, req);
		return;
	}
	// an external-format image has no requirements of its own before its gralloc buffer is bound: venus's frame
	// (VENUS_BUFFER_SIZE: planes of rows rounded up to 128 bytes, 32 and 16 lines, 4096 bytes, and the decoder's
	// metadata: 16 kB or 8 rows),
	// whatever layouts the caller gave (vrlink's are for UBWC)
	VkDeviceSize stride = (qnv[i].w + 127) & ~127u, y = stride * ((qnv[i].h + 31) & ~31u),
	             uv = stride * ((((qnv[i].h + 1) / 2) + 15) & ~15u), extra = 8 * stride > 16384 ? 8 * stride : 16384;
	req->size = (y + uv + 4096 + extra + 4095) & ~(VkDeviceSize)4095;
	req->alignment = 4096;
	req->memoryTypeBits = (uint32_t)((1ull << d->memprops.memoryTypeCount) - 1);
}

static void qnv_requirements2(VkDevice device, const VkImageMemoryRequirementsInfo2 *info, VkMemoryRequirements2 *req,
                              PFN_vkGetImageMemoryRequirements2 next)
{
	if (qnv_find(info->image) < 0) {
		next(device, info, req);
		return;
	}
	GetImageMemoryRequirements(device, info->image, &req->memoryRequirements);
	VkMemoryDedicatedRequirements *dr =
	    (VkMemoryDedicatedRequirements *)find_struct(req->pNext, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS);
	if (dr)
		dr->prefersDedicatedAllocation = dr->requiresDedicatedAllocation = VK_TRUE;
}

static VKAPI_ATTR void VKAPI_CALL GetImageMemoryRequirements2(VkDevice device, const VkImageMemoryRequirementsInfo2 *info,
                                                             VkMemoryRequirements2 *req)
{
	qnv_requirements2(device, info, req, device_of(device)->GetImageMemoryRequirements2);
}

static VKAPI_ATTR void VKAPI_CALL GetImageMemoryRequirements2KHR(VkDevice device,
                                                                const VkImageMemoryRequirementsInfo2 *info,
                                                                VkMemoryRequirements2 *req)
{
	struct device *d = device_of(device);
	qnv_requirements2(device, info, req,
	                  d->GetImageMemoryRequirements2KHR ? d->GetImageMemoryRequirements2KHR : d->GetImageMemoryRequirements2);
}

//! An NV12 conversion of vrlink's: the external format's.
static VkResult qnv_conversion(VkDevice device, const VkSamplerYcbcrConversionCreateInfo *ci,
                               const VkAllocationCallbacks *alloc, VkSamplerYcbcrConversion *out,
                               PFN_vkCreateSamplerYcbcrConversion next)
{
	struct device *d = device_of(device);
	VkFormatFeatureFlags feat = 0;
	uint64_t xfmt = 0;
	if (!qdb_front() || ci->format != VK_FORMAT_G8_B8R8_2PLANE_420_UNORM || !(xfmt = qnv_external_format(d, device, &feat))) {
		static atomic_int logs;
		if (qdb_front() && atomic_fetch_add(&logs, 1) < 4)
			LOG("dma-buf: YCbCr conversion of format %d left as it is\n", ci->format);
		return next(device, ci, alloc, out);
	}
	VkExternalFormatANDROID xf = {VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID, NULL, xfmt};
	VkSamplerYcbcrConversionCreateInfo c = *ci;
	c.pNext = &xf;
	c.format = VK_FORMAT_UNDEFINED;
	if (!(feat & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT))
		c.chromaFilter = VK_FILTER_NEAREST;
	VkResult r = next(device, &c, alloc, out);
	LOG("dma-buf: NV12 YCbCr conversion (model %d, range %d, offsets %d %d, filter %d) on the external format: %d\n",
	    ci->ycbcrModel, ci->ycbcrRange, ci->xChromaOffset, ci->yChromaOffset, c.chromaFilter, r);
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateSamplerYcbcrConversion(VkDevice device,
                                                                  const VkSamplerYcbcrConversionCreateInfo *ci,
                                                                  const VkAllocationCallbacks *alloc,
                                                                  VkSamplerYcbcrConversion *out)
{
	return qnv_conversion(device, ci, alloc, out, device_of(device)->CreateSamplerYcbcrConversion);
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateSamplerYcbcrConversionKHR(VkDevice device,
                                                                     const VkSamplerYcbcrConversionCreateInfo *ci,
                                                                     const VkAllocationCallbacks *alloc,
                                                                     VkSamplerYcbcrConversion *out)
{
	struct device *d = device_of(device);
	return qnv_conversion(device, ci, alloc, out,
	                      d->CreateSamplerYcbcrConversionKHR ? d->CreateSamplerYcbcrConversionKHR
	                                                         : d->CreateSamplerYcbcrConversion);
}
