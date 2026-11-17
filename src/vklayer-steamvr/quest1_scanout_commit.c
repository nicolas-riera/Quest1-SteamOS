// quest1_scanout_commit: the frames to the panels: Monado commits them (or the layer itself), the fence thread, start and destroy.
// Part of quest1_scanout.c (included by it, in this order: one translation unit with quest1_compat.c).

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
static uint32_t qs_mdp_format(const struct qd_swapchain *sc, uint32_t idx, int half)
{
	bool bgra = sc->ici.format == VK_FORMAT_B8G8R8A8_UNORM || sc->ici.format == VK_FORMAT_B8G8R8A8_SRGB;
	if (sc->half_ubwc[half][idx])
		return MDP_RGBA_8888_UBWC; // stored as RGBA whatever the view's swap (QS_AHB_USAGE)
	return bgra ? MDP_BGRA_8888 : MDP_RGBA_8888;
}

// --- Monado commits the frames ---------------------------------------------------------------------------
// When Monado's MDP target listens on Q1D_SOCKET, the layer hands it the half buffers once and then
// only announces each finished frame; Monado puts it on the panels (and re-projects it to the head pose
// of the display time), tells when the frame is displayed and when it no longer reads an image. Without
// Monado (or when it goes away) the layer commits the frames itself.

//! Connect to Monado and hand it the buffers. Returns true when sc->mon_sock is set.
static bool qs_mon_connect(struct qd_swapchain *sc)
{
	if (getenv("QUEST1_DIRECT_NO_MONADO") || !qs_ahb.send_handle)
		return false;
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	struct sockaddr_un a = {AF_UNIX};
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", Q1D_SOCKET);
	if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
		if (fd >= 0)
			close(fd);
		return false;
	}
	bool ok = true;
	for (uint32_t i = 0; i < sc->count && ok; i++)
		for (int h = 0; h < 2 && ok; h++) {
			struct q1d_msg m;
			memset(&m, 0, sizeof(m));
			m.type = Q1D_BUFFER;
			m.index = i;
			m.half = (uint32_t)h;
			m.count = sc->count;
			m.width = qs_scene_w ? qs_scene_w : QS_PANEL_W;
			m.height = qs_scene_w ? qs_scene_h : sc->ici.extent.height;
			m.stride_px = sc->half_stride_px[h][i];
			// scene mode: vrcompositor's R8G8B8A8 (sRGB) eye renders
			m.bgra = !qs_scene_w && qs_mdp_format(sc, i, h) == MDP_BGRA_8888;
			m.ubwc = sc->half_ubwc[h][i];
			ok = send(fd, &m, sizeof(m), MSG_NOSIGNAL) == (ssize_t)sizeof(m) &&
			     qs_ahb.send_handle(sc->half_ahb[h][i], fd) == 0;
		}
	if (!ok) {
		LOG("direct scanout: handing the buffers to Monado failed: %s\n", strerror(errno));
		close(fd);
		return false;
	}
	pthread_mutex_lock(&sc->mutex);
	sc->mon_sock = fd;
	pthread_mutex_unlock(&sc->mutex);
	qs_wake(sc);
	LOG("direct scanout: Monado commits and re-projects the frames (%s)\n", Q1D_SOCKET);
	return true;
}

static void qs_write_flag(void)
{
	if (qs_flag_written)
		return;
	FILE *f = fopen(QS_FLAG_FILE, "w");
	if (f) {
		fprintf(f, "%d\n", (int)getpid());
		fclose(f);
		qs_flag_written = true;
		LOG("direct scanout: on the panels (Monado pauses its compositing)\n");
	}
}

//! Announce frame `frame` (image idx, finished) to Monado. False when it is not connected (any more).
static bool qs_mon_present(struct qd_swapchain *sc, uint32_t idx, uint64_t frame)
{
	struct q1d_msg m;
	memset(&m, 0, sizeof(m));
	m.type = Q1D_FRAME;
	m.index = idx;
	m.frame = frame;
	pthread_mutex_lock(&sc->mutex);
	if (sc->mon_sock < 0) {
		pthread_mutex_unlock(&sc->mutex);
		return false;
	}
	if (sc->pose[frame % 64].frame == frame) {
		m.pose_valid = 1;
		memcpy(m.pose, sc->pose[frame % 64].q, sizeof(m.pose));
		m.time_ns = sc->pose[frame % 64].time_ns;
	}
	// the frame's head-locked overlays drawn apart (quest1_hud.c): their buffers go first, once
	if (sc->hud_of[idx] && qh_send_locked(sc, sc->mon_sock)) {
		m.hud = sc->hud_of[idx];
		memcpy(m.hud_rect, sc->hud_rect[idx], sizeof(m.hud_rect));
	}
	sc->app_owned[idx] = false;
	sc->driver_owned[idx] = true; // until Monado's Q1D_RELEASE
	bool ok = send(sc->mon_sock, &m, sizeof(m), MSG_NOSIGNAL | MSG_DONTWAIT) == (ssize_t)sizeof(m);
	if (!ok) {
		LOG("direct scanout: Monado is gone (%s): committing ourselves\n", strerror(errno));
		close(sc->mon_sock);
		sc->mon_sock = -1;
		for (uint32_t i = 0; i < sc->count; i++)
			if (sc->release_fence[i] < 0)
				sc->driver_owned[i] = false;
		sc->app_owned[idx] = true; // qs_commit hands it over
	}
	pthread_cond_broadcast(&sc->cond);
	pthread_mutex_unlock(&sc->mutex);
	if (ok)
		qs_write_flag();
	return ok;
}

//! Fence thread, sc->mutex held: Monado's socket is readable.
static void qs_mon_read(struct qd_swapchain *sc)
{
	struct q1d_msg m;
	for (;;) {
		ssize_t n = recv(sc->mon_sock, &m, sizeof(m), MSG_DONTWAIT);
		if (n == (ssize_t)sizeof(m)) {
			if (m.type == Q1D_RELEASE && m.index < sc->count)
				sc->driver_owned[m.index] = false;
			else if (m.type == Q1D_DISPLAYED)
				qs_displayed_locked(sc, m.frame);
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			return;
		break;
	}
	LOG("direct scanout: Monado disconnected, committing ourselves again\n");
	close(sc->mon_sock);
	sc->mon_sock = -1;
	for (uint32_t i = 0; i < sc->count; i++)
		if (sc->release_fence[i] < 0)
			sc->driver_owned[i] = false;
	qs_displayed_locked(sc, sc->frame);
	pthread_cond_broadcast(&sc->cond);
}

// Completer thread, image `idx` rendered: put it on the panels. sc->mutex not held.
static void qs_commit(struct qd_swapchain *sc, uint32_t idx, uint64_t frame)
{
	qs_snapshot(sc, idx);
	qs_trace_arm();
	qs_ts_frame();
	// debug: /tmp/quest1-nomonado makes the layer commit the frames itself (A/B of Monado's presentation)
	bool nomonado = access("/tmp/quest1-nomonado", F_OK) == 0;
	if (nomonado && sc->mon_sock >= 0) {
		pthread_mutex_lock(&sc->mutex);
		close(sc->mon_sock);
		sc->mon_sock = -1;
		for (uint32_t i = 0; i < sc->count; i++)
			if (sc->release_fence[i] < 0 && i != idx)
				sc->driver_owned[i] = false;
		pthread_mutex_unlock(&sc->mutex);
		usleep(50000); // Monado must have dropped the panels before we commit
	}
	if (!nomonado && sc->mon_sock < 0 && now_ns() > sc->mon_retry_ns) {
		sc->mon_retry_ns = now_ns() + 2000000000ull;
		qs_mon_connect(sc);
	}
	if (sc->mon_sock >= 0 && qs_mon_present(sc, idx, frame))
		return;
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
			// each half from its own buffer (qs_CmdBeginRenderPass)
			l->src_rect = (struct mdp_rect){0, 0, QS_PANEL_W, sc->ici.extent.height};
			l->dst_rect = (struct mdp_rect){hh * QS_PANEL_W, 0, QS_PANEL_W, sc->ici.extent.height};
			l->buffer.width = sc->half_stride_px[hh][idx];
			l->buffer.height = sc->ici.extent.height;
			l->buffer.format = qs_mdp_format(sc, idx, hh);
			l->buffer.planes[0].fd = sc->half_fd[hh][idx];
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
			qs_write_flag();
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
	q1_realtime("scanout: fence thread", Q1_RT_MAX);
	uint32_t slots = sizeof(sc->retire) / sizeof(sc->retire[0]);
	pthread_mutex_lock(&sc->mutex);
	while (!sc->stopping) {
		struct pollfd p[2 + 8 + QD_MAX_IMAGES];
		int kind[2 + 8 + QD_MAX_IMAGES]; // -2 Monado, -1 wake, 0..7 retire slot, 100+i release of image i
		int n = 0;
		p[n] = (struct pollfd){sc->wake_fd[0], POLLIN, 0};
		kind[n++] = -1;
		if (sc->mon_sock >= 0) {
			p[n] = (struct pollfd){sc->mon_sock, POLLIN, 0};
			kind[n++] = -2;
		}
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
			if (kind[j] == -2) {
				if (sc->mon_sock >= 0)
					qs_mon_read(sc);
			} else if (kind[j] < 0) {
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
		for (int h = 0; h < 2; h++)
			if (sc->half_ahb[h][i])
				qs_ahb.release(sc->half_ahb[h][i]);
	}
	for (int s = 0; s < Q1D_HUD_SETS; s++)
		for (int h = 0; h < 2; h++)
			if (sc->hud_ahb[s][h])
				qs_ahb.release(sc->hud_ahb[s][h]);
	qs_twins_forget(sc);
	pthread_mutex_lock(&qs_scene_lock);
	for (unsigned i = 0; i < qs_nstwins;) {
		bool mine = false;
		for (uint32_t k = 0; k < QD_MAX_IMAGES; k++)
			mine |= qs_stwins[i].half == (uint64_t)sc->half_image[0][k] || qs_stwins[i].half == (uint64_t)sc->half_image[1][k];
		for (int k = 0; k < Q1D_HUD_SETS; k++)
			mine |= qs_stwins[i].half == (uint64_t)sc->hud_image[k][0] || qs_stwins[i].half == (uint64_t)sc->hud_image[k][1];
		if (mine) {
			((PFN_vkDestroyFramebuffer)sc->d->gdpa(sc->device, "vkDestroyFramebuffer"))(sc->device, (VkFramebuffer)qs_stwins[i].twin, NULL);
			((PFN_vkDestroyImageView)sc->d->gdpa(sc->device, "vkDestroyImageView"))(sc->device, (VkImageView)qs_stwins[i].view, NULL);
			qs_stwins[i] = qs_stwins[--qs_nstwins];
		} else
			i++;
	}
	pthread_mutex_unlock(&qs_scene_lock);
	if (sc->mon_sock >= 0)
		close(sc->mon_sock);
	if (sc->wake_fd[0] >= 0) {
		close(sc->wake_fd[0]);
		close(sc->wake_fd[1]);
	}
	qs_remove_flag();
}
