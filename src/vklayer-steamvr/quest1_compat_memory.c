// quest1_compat_memory: memory allocation (vrclient's ring backed on first touch) and image creation.
// Part of quest1_compat.c (included by it, in this order: one translation unit).

/*
 * vrclient's Vulkan transfer ring (CVulkanVRRenderer, "Initialized Vulkan transfer ring buffer"): 256 MB of
 * host-visible memory in every OpenVR process that uses Vulkan (vrcompositor has its own copy of that
 * renderer). On KGSL that is pinned memory, allocated in full up front, and mostly never touched: 0 MB in the
 * dashboard's vrwebhelper and in overlay apps (q1desktop), ~50 MB in vrcompositor; only Steam's webhelper,
 * which uploads its CPU-drawn pages through it, fills it. A large allocation made from vrclient.so or
 * vrcompositor (the ring is their only one of 64 MB or more) asks the kernel to back it page by page at the
 * first CPU touch (kernel/kgsl-lazy.patch, IOCTL_KGSL_LAZY_NEXT): the ring is written by the CPU before the
 * GPU reads it. Steam's webhelper goes all the way round the ring: the kernel moves the pages 3/8 of the ring
 * behind the write position (96 MB of 256, long consumed by the GPU) to where it writes next, so the ring stays
 * backed by about that much instead of filling up. Without that kernel patch the ioctl fails and nothing changes.
 */
#define LAZY_MIN_SIZE (64ull << 20)

static bool called_from_vrclient(void)
{
	void *bt[16];
	int n = backtrace(bt, 16);
	for (int i = 1; i < n; i++) {
		Dl_info di;
		if (!dladdr(bt[i], &di) || !di.dli_fname)
			continue;
		const char *base = strrchr(di.dli_fname, '/');
		base = base ? base + 1 : di.dli_fname;
		if (!strcmp(base, "vrclient.so") || !strcmp(base, "vrcompositor"))
			return true;
	}
	return false;
}

static void lazy_hint(VkDeviceSize size)
{
	static atomic_int kgsl_fd = -2;
	struct { uint64_t size; } a = { size };
	int fd = atomic_load(&kgsl_fd);
	if (fd == -2) {
		int nfd = open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC), expected = -2;
		if (atomic_compare_exchange_strong(&kgsl_fd, &expected, nfd))
			fd = nfd;
		else {
			if (nfd >= 0)
				close(nfd);
			fd = expected;
		}
	}
	if (fd >= 0 && ioctl(fd, _IOW(0x09, 0x58, a), &a) == 0)
		LOG("vrclient's %llu MB ring backed on first touch\n", (unsigned long long)(size >> 20));
}

static VKAPI_ATTR VkResult VKAPI_CALL AllocateMemory(VkDevice device, const VkMemoryAllocateInfo *ai,
                                                     const VkAllocationCallbacks *alloc, VkDeviceMemory *out)
{
	struct device *d = device_of(device);
	if (ai->allocationSize >= LAZY_MIN_SIZE && called_from_vrclient())
		lazy_hint(ai->allocationSize);
	int keep = qdb_keep(ai);
	// a dma-buf the blob does not know (quest1_dmabuf.c): an OPAQUE_FD one is tried there first, a DMA_BUF one never
	const VkImportMemoryFdInfoKHR *imp = find_struct(ai->pNext, VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR);
	// (an NV12 frame's OPAQUE_FD is the dma-buf vrcompositor exported again: GetMemoryFd)
	const VkMemoryDedicatedAllocateInfo *ded = find_struct(ai->pNext, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
	bool dmabuf = keep >= 0 && (imp->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT ||
	                            (ded && qnv_find(ded->image) >= 0));
	VkResult r;
	if (keep < 0 && qnv_export(d, device, ai, alloc, out, &r)) // an NV12 frame to export (quest1_modifier.c)
		return r;
	r = dmabuf ? VK_ERROR_INVALID_EXTERNAL_HANDLE : d->AllocateMemory(device, ai, alloc, out);
	if (r == VK_ERROR_INVALID_EXTERNAL_HANDLE && keep >= 0) {
		// an NV12 frame as a gralloc NV12 buffer, anything else as plain memory
		if (!dmabuf || !qnv_import(d, device, ai, keep, alloc, out, &r))
			r = qdb_import(d, device, ai, keep, alloc, out);
		if (r == VK_SUCCESS && dmabuf)
			close(imp->fd); // a successful import owns the fd (the blob closed the OPAQUE_FD one itself)
	}
	if (keep >= 0)
		close(keep);
	if (r != VK_SUCCESS && fail_logs < 40) {
		fail_logs++;
		LOG("vkAllocateMemory %llu bytes, memory type %u: %d\n", (unsigned long long)ai->allocationSize, ai->memoryTypeIndex, r);
	}
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateImage(VkDevice device, const VkImageCreateInfo *ci,
                                                  const VkAllocationCallbacks *alloc, VkImage *out)
{
	struct device *d = device_of(device);
	if (!cpu_image(ci)) {
		// vrcompositor sizes the "Playspace area SDF" texture from the boundary: 0x0 until one is drawn (room
		// setup). The Adreno driver accepts the image but reports 0 bytes of memory, whose allocation fails, so
		// vrcompositor's CreateTexture failed on every frame (room setup at ~3 fps). Made 1x1 instead.
		VkImageCreateInfo one;
		if (!ci->extent.width || !ci->extent.height || !ci->extent.depth) {
			static int logged;
			if (!logged++)
				LOG("vkCreateImage %ux%ux%u format %d: empty image made 1x1x1 (logged once)\n", ci->extent.width,
				    ci->extent.height, ci->extent.depth, ci->format);
			one = *ci;
			one.extent.width = one.extent.width ? one.extent.width : 1;
			one.extent.height = one.extent.height ? one.extent.height : 1;
			one.extent.depth = one.extent.depth ? one.extent.depth : 1;
			ci = &one;
		}
		// The room setup's "Playspace area SDF" texture is a linear R8 image written through a mapping and
		// sampled, but created with TRANSFER usage only. The Adreno blob then samples 0 everywhere (sdftest:
		// 261120 of 262144 texels wrong), i.e. "inside": the drawn area showed as whole purple squares. Linear
		// R8 images get SAMPLED usage (sdftest: all texels right).
		VkImageCreateInfo samp;
		if (ci->format == VK_FORMAT_R8_UNORM && ci->tiling == VK_IMAGE_TILING_LINEAR &&
		    !(ci->usage & VK_IMAGE_USAGE_SAMPLED_BIT)) {
			static int logged;
			if (!logged++)
				LOG("linear R8 image %ux%u: SAMPLED usage added (logged once)\n", ci->extent.width, ci->extent.height);
			samp = *ci;
			samp.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
			ci = &samp;
		}
		// scene mode: the 4x MSAA system layer made 1-sample (qs_scene_single_sample, quest1_scanout.c)
		VkImageCreateInfo single;
		if ((ci->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) &&
		    qs_scene_single_sample(ci->samples)) {
			LOG("scene mode: %ux%u format %d image of %d samples made 1-sample\n", ci->extent.width, ci->extent.height,
			    ci->format, ci->samples);
			single = *ci;
			single.samples = VK_SAMPLE_COUNT_1_BIT;
			ci = &single;
		}
		VkResult r = qdb_create_image(d, device, ci, alloc, out); // a plane of a dma-buf, or not (quest1_dmabuf.c)
		if (r == VK_SUCCESS)
			qs_scene_note(*out, ci); // scene mode: vrcompositor's eye render targets (quest1_scanout.c)
		if (r != VK_SUCCESS && fail_logs < 40) {
			fail_logs++;
			LOG("vkCreateImage %ux%ux%u format %d mips %u layers %u samples %d usage 0x%x flags 0x%x tiling %d: %d\n",
			    ci->extent.width, ci->extent.height, ci->extent.depth, ci->format, ci->mipLevels, ci->arrayLayers,
			    ci->samples, ci->usage, ci->flags, ci->tiling, r);
		}
		return r;
	}
	VkImageCreateInfo lin = *ci;
	lin.usage &= ~Q1_CPU_IMAGE_USAGE;
	lin.tiling = VK_IMAGE_TILING_LINEAR;
	VkResult r = d->CreateImage(device, &lin, alloc, out);
	if (r != VK_SUCCESS) {
		LOG("CPU-written image %ux%u: linear tiling refused (%d), optimal kept\n", ci->extent.width, ci->extent.height, r);
		lin.tiling = VK_IMAGE_TILING_OPTIMAL;
		return d->CreateImage(device, &lin, alloc, out);
	}
	VkImageSubresource sub = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT};
	VkSubresourceLayout l = {0};
	d->GetImageSubresourceLayout(device, *out, &sub, &l);
	LOG("CPU-written image %ux%u (usage 0x%x): linear, offset %llu row pitch %llu size %llu\n", ci->extent.width,
	    ci->extent.height, lin.usage, (unsigned long long)l.offset, (unsigned long long)l.rowPitch,
	    (unsigned long long)l.size);
	return r;
}
