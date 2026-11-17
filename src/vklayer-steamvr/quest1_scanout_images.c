// quest1_scanout_images: the display swapchain's images (gralloc buffers) and the layer's snapshots of them.
// Part of quest1_scanout.c (included by it, in this order: one translation unit with quest1_compat.c).

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
