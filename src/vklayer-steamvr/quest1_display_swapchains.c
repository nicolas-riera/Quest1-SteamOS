// quest1_display_swapchains: display swapchains: Monado's connection, creation and destruction (direct scanout: quest1_scanout.c).
// Part of quest1_display.c (included by it, in this order: one translation unit).

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
	VkFence fences[QD_MAX_IMAGES]; //!< async present: signaled when the image is rendered

	// async present (default; QUEST1_DISPLAY_SYNC=1 waits in vkQueuePresentKHR instead): presented
	// images still rendering, handed to the driver in order by the completer thread (under mutex)
	struct
	{
		uint32_t index;
		uint64_t frame, t_present;
	} pend[8];
	uint32_t pend_head, pend_tail;
	pthread_t completer;
	bool completer_running, stopping, sync_present;

	pthread_mutex_t mutex;
	pthread_cond_t cond;
	bool app_owned[QD_MAX_IMAGES], driver_owned[QD_MAX_IMAGES];
	uint64_t frame_tick[QD_MAX_IMAGES]; //!< qd_frame_tick when the image was acquired
	uint64_t posed_frame;               //!< last frame whose QD_POSED came back
	// present wait: the VkPresentIdKHR of each frame, and the last frame the driver latched
	struct
	{
		uint64_t frame, id;
	} present_ids[64];
	uint64_t displayed_id, displayed_ns;
	uint32_t next;
	uint32_t reserved1; //!< index + 1 the next acquire returns (scene mode, qs_reserve), 0: none

	// direct scanout (quest1_scanout.c, QUEST1_DIRECT_SCANOUT=1): gralloc images committed on /dev/fb0
	bool direct;
	// Monado's socket (quest1_direct_proto.h): when connected it commits the frames (and re-projects
	// them), otherwise this layer commits them itself
	int mon_sock;
	uint64_t mon_retry_ns;
	struct
	{
		uint64_t frame; //!< 0: empty
		float q[4];
		int64_t time_ns;
	} pose[64]; //!< the render pose of each frame (from QD_POSED, which announces the next frame's)
	int release_fence[QD_MAX_IMAGES]; //!< of the commit that showed the image; -1 when none
	// each half of the display image (one eye, one panel) is drawn into a gralloc buffer of its own,
	// see qs_CmdBeginRenderPass; images[] are plain images that are never rendered to
	AHardwareBuffer *half_ahb[2][QD_MAX_IMAGES];
	VkImage half_image[2][QD_MAX_IMAGES];
	VkDeviceMemory half_memory[2][QD_MAX_IMAGES];
	int half_fd[2][QD_MAX_IMAGES]; //!< owned by the AHardwareBuffer
	uint32_t half_stride_px[2][QD_MAX_IMAGES];
	bool half_ubwc[2][QD_MAX_IMAGES];
	// head-locked overlays drawn apart (quest1_hud.c): Q1D_HUD_SETS sets of a buffer per eye, made at the first one
	AHardwareBuffer *hud_ahb[Q1D_HUD_SETS][2];
	VkImage hud_image[Q1D_HUD_SETS][2];
	VkDeviceMemory hud_memory[Q1D_HUD_SETS][2];
	int hud_fd[Q1D_HUD_SETS][2];
	uint32_t hud_stride_px[Q1D_HUD_SETS][2];
	bool hud_ubwc[Q1D_HUD_SETS][2];
	bool hud_made, hud_failed;
	int hud_sent_sock;              //!< 1 + the Monado socket the buffers went to, 0 none
	uint32_t hud_of[QD_MAX_IMAGES]; //!< Q1D_FRAME's hud for the image's frame (1 + set, eyes << 8), 0 none
	float hud_rect[QD_MAX_IMAGES][2][4]; //!< Q1D_FRAME's hud_rect for the image's frame
	struct
	{
		int fd;
		uint64_t frame;
	} retire[8];                     //!< commits not on screen yet
	uint32_t retire_head, retire_tail;
	int wake_fd[2];
	pthread_t fence_thread;
	bool fence_thread_running;
	uint64_t frame;
	int sock;
	pthread_t reader;
	bool reader_running;

	// device entry points (below this layer)
	PFN_vkDestroyImage DestroyImage;
	PFN_vkFreeMemory FreeMemory;
	PFN_vkDestroyCommandPool DestroyCommandPool;
};

#include "quest1_scanout.c"

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
	// real-time like the rest of the frame's path: vrcompositor waits (at most 4 ms) for the QD_POSED this thread
	// receives; as a normal thread it woke late and the wait ran out 9-15 times in 5 s even idle (2026-11-09)
	q1_realtime("display: driver reply thread", Q1_RT_MAX);
	while (recv(sc->sock, &m, sizeof(m), 0) == sizeof(m)) {
		if (m.type == QD_POSED) {
			pthread_mutex_lock(&sc->mutex);
			sc->posed_frame = m.frame;
			if (m.pose_valid) {
				uint64_t next = m.frame + 1;
				sc->pose[next % 64].frame = next;
				memcpy(sc->pose[next % 64].q, m.pose, sizeof(m.pose));
				sc->pose[next % 64].time_ns = m.pose_time_ns;
			}
			pthread_cond_broadcast(&sc->cond);
			pthread_mutex_unlock(&sc->mutex);
			continue;
		}
		if (m.type == QD_DISPLAYED) {
			pthread_mutex_lock(&sc->mutex);
			// present ids only grow: the latched frame's id also covers the frames it replaced
			if (sc->present_ids[m.frame % 64].frame == m.frame && sc->present_ids[m.frame % 64].id > sc->displayed_id)
				sc->displayed_id = sc->present_ids[m.frame % 64].id;
			sc->displayed_ns = m.vblank_ns;
			pthread_cond_broadcast(&sc->cond);
			pthread_mutex_unlock(&sc->mutex);
			continue;
		}
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

static void qd_stats_gpu(uint64_t latency_ns);

//! Async present: hand each presented image to the driver once the GPU has rendered it, in order.
static void *qd_completer(void *arg)
{
	struct qd_swapchain *sc = arg;
	pthread_setname_np(pthread_self(), "q1 display done");
	q1_realtime("display: present thread", Q1_RT_MAX);
	pthread_mutex_lock(&sc->mutex);
	for (;;) {
		while (sc->pend_head == sc->pend_tail && !sc->stopping)
			pthread_cond_wait(&sc->cond, &sc->mutex);
		if (sc->pend_head == sc->pend_tail)
			break;
		uint32_t idx = sc->pend[sc->pend_head % 8].index;
		uint64_t frame = sc->pend[sc->pend_head % 8].frame, t_present = sc->pend[sc->pend_head % 8].t_present;
		pthread_mutex_unlock(&sc->mutex);

		wait_fence(sc->d, sc->device, sc->fences[idx]);
		sc->d->ResetFences(sc->device, 1, &sc->fences[idx]);
		qd_stats_gpu(now_ns() - t_present);

		if (sc->direct) {
			pthread_mutex_lock(&sc->mutex);
			sc->pend_head++;
			pthread_mutex_unlock(&sc->mutex);
			qs_commit(sc, idx, frame);
			pthread_mutex_lock(&sc->mutex);
			continue;
		}
		pthread_mutex_lock(&sc->mutex);
		sc->pend_head++;
		sc->app_owned[idx] = false;
		if (sc->sock >= 0) {
			struct qd_msg m = {QD_PRESENT};
			m.index = idx;
			m.frame = frame;
			m.flags = QD_PRESENT_POSED;
			m.vblank_ns = sc->frame_tick[idx];
			if (send(sc->sock, &m, sizeof(m), MSG_NOSIGNAL) == sizeof(m))
				sc->driver_owned[idx] = true;
		}
		pthread_cond_broadcast(&sc->cond);
	}
	pthread_mutex_unlock(&sc->mutex);
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
	sc->mon_sock = -1;
	sc->count = ci->minImageCount < 3 ? 3 : ci->minImageCount;
	// direct scanout: Monado holds the image on the panels and the one queued for the next vsync, so with 3 the
	// next acquire waited for a vsync (~1.5 ms a frame on average, 2026-11-06)
	if (qs_enabled() && sc->count < 4)
		sc->count = 4;
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
	for (uint32_t i = 0; i < QD_MAX_IMAGES; i++)
		fds[i] = sc->release_fence[i] = sc->half_fd[0][i] = sc->half_fd[1][i] = -1;
	sc->wake_fd[0] = sc->wake_fd[1] = -1;
	sc->direct = qs_enabled() && ci->imageExtent.width == 2 * QS_PANEL_W && qs_load();
	if (qs_enabled() && !sc->direct)
		LOG("direct scanout: off (display %ux%u, needs %ux1600)\n", ci->imageExtent.width,
		    ci->imageExtent.height, 2 * QS_PANEL_W);
	if (sc->direct)
		r = qs_create_images(sc, ci);
	for (uint32_t i = 0; i < sc->count && r == VK_SUCCESS && !sc->direct; i++) {
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
			                  sc->direct ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		}
		VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		r = d->CreateFence(device, &fci, NULL, &sc->fence);
		for (uint32_t i = 0; i < sc->count && r == VK_SUCCESS; i++)
			r = d->CreateFence(device, &fci, NULL, &sc->fences[i]);
	}
	if (r != VK_SUCCESS) {
		LOG("display swapchain creation failed: %d\n", r);
		qd_destroy(sc);
		return r;
	}

	sc->sock = qd_connect();
	// direct scanout: the driver only publishes poses, it never sees the images
	if (sc->sock >= 0 && !sc->direct && !qd_send_swapchain(sc, fds)) {
		LOG("sending the swapchain to the display receiver failed: %s\n", strerror(errno));
		close(sc->sock);
		sc->sock = -1;
	}
	for (uint32_t i = 0; i < sc->count; i++)
		if (fds[i] >= 0)
			close(fds[i]);
	if (sc->sock >= 0) {
		sc->reader_running = pthread_create(&sc->reader, NULL, qd_reader, sc) == 0;
	}
	const char *sync = getenv("QUEST1_DISPLAY_SYNC");
	sc->sync_present = sync && atoi(sync) != 0 && !sc->direct;
	if (sc->direct)
		qs_start(sc);
	if (!sc->sync_present)
		sc->completer_running = pthread_create(&sc->completer, NULL, qd_completer, sc) == 0;
	sc->sync_present = !sc->completer_running;

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
	if (sc->completer_running) {
		pthread_mutex_lock(&sc->mutex);
		sc->stopping = true;
		pthread_cond_broadcast(&sc->cond);
		pthread_mutex_unlock(&sc->mutex);
		pthread_join(sc->completer, NULL);
	}
	if (sc->fence_thread_running) {
		pthread_mutex_lock(&sc->mutex);
		sc->stopping = true;
		pthread_mutex_unlock(&sc->mutex);
		qs_wake(sc);
		pthread_join(sc->fence_thread, NULL);
	}
	for (uint32_t i = 0; i < QD_MAX_IMAGES; i++)
		if (sc->fences[i])
			sc->d->DestroyFence(sc->device, sc->fences[i], NULL);
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
	if (sc->direct) {
		for (uint32_t i = 0; i < sc->count; i++)
			for (int h = 0; h < 2; h++) {
				if (sc->half_image[h][i])
					sc->DestroyImage(sc->device, sc->half_image[h][i], NULL);
				if (sc->half_memory[h][i])
					sc->FreeMemory(sc->device, sc->half_memory[h][i], NULL);
			}
		for (int s = 0; s < Q1D_HUD_SETS; s++)
			for (int h = 0; h < 2; h++) {
				if (sc->hud_image[s][h])
					sc->DestroyImage(sc->device, sc->hud_image[s][h], NULL);
				if (sc->hud_memory[s][h])
					sc->FreeMemory(sc->device, sc->hud_memory[s][h], NULL);
			}
		qs_destroy(sc);
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
