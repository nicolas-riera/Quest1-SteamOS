// quest1_display_present: acquire and present on the display swapchains.
// Part of quest1_display.c (included by it, in this order: one translation unit).

static VkResult qd_acquire(struct qd_swapchain *sc, uint64_t timeout, VkSemaphore semaphore, VkFence fence,
                           uint32_t *index)
{
	qd_trace("acquire");
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	if (timeout != UINT64_MAX) {
		uint64_t ns = deadline.tv_nsec + timeout % 1000000000ull;
		deadline.tv_sec += timeout / 1000000000ull + ns / 1000000000ull;
		deadline.tv_nsec = ns % 1000000000ull;
	}
	uint64_t t0 = now_ns();
	pthread_mutex_lock(&sc->mutex);
	int found = -1;
	if (sc->reserved1) { // scene mode rendered into this image's buffers before the acquire (qs_reserve)
		uint32_t r = sc->reserved1 - 1;
		sc->reserved1 = 0;
		if (!sc->app_owned[r] && !sc->driver_owned[r])
			found = (int)r;
	}
	for (; found < 0;) {
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
	sc->frame_tick[found] = atomic_load(&qd_frame_tick);
	sc->next = (found + 1) % sc->count;
	qd_stats.acquire += now_ns() - t0;
	pthread_mutex_unlock(&sc->mutex);
	if (qs_trace_left > 0 && qs_trace_left--)
		LOG("trace: acquire %d\n", found);
	ft("acquire", found);

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

//! VkPresentIdKHR of swapchain s in this present, 0 without one
static uint64_t qd_present_id(const VkPresentInfoKHR *pi, uint32_t s)
{
	for (const VkBaseInStructure *b = pi->pNext; b; b = b->pNext)
		if (b->sType == VK_STRUCTURE_TYPE_PRESENT_ID_KHR) {
			const VkPresentIdKHR *p = (const VkPresentIdKHR *)b;
			return p->pPresentIds && s < p->swapchainCount ? p->pPresentIds[s] : 0;
		}
	return 0;
}

//! Call with sc->mutex held.
static void qd_record_present_id(struct qd_swapchain *sc, uint64_t frame, uint64_t id)
{
	sc->present_ids[frame % 64].frame = frame;
	sc->present_ids[frame % 64].id = id;
}

//! Frame period of vrcompositor while the headset is not worn: 8 refresh periods, as Monado's MDP_IDLE_PERIOD_NS.
#define QD_IDLE_PERIOD_NS (8ull * 1000000000ull / 72)

static VKAPI_ATTR VkResult VKAPI_CALL qd_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pi)
{
	qd_trace("present");
	ft("present", 0);
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
			// The headset is not worn (Monado made /tmp/quest1-panels-off: the panels are off, no frame is shown):
			// vrcompositor is held here to about 9 frames a second, the rate Monado gives OpenXR apps then, instead
			// of rendering 72 a second for nobody (half a core and the GPU's work, measured 2026-11-10).
			static uint64_t idle_last; // one presenting thread
			if (access("/tmp/quest1-panels-off", F_OK) == 0) {
				uint64_t n = now_ns();
				if (n - idle_last < QD_IDLE_PERIOD_NS)
					usleep((useconds_t)((QD_IDLE_PERIOD_NS - (n - idle_last)) / 1000));
				idle_last = now_ns();
			}
			uint64_t t_entry = now_ns(), t_gpu;
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
			if (!sc->sync_present) {
				// async: the completer thread hands the image over once rendered; meanwhile the
				// driver publishes the next frame's pose now, so vrcompositor starts it at once
				r = QueueSubmit(queue, 1, &si, sc->fences[idx]);
				pthread_mutex_lock(&sc->mutex);
				uint64_t frame = ++sc->frame;
				qd_record_present_id(sc, frame, qd_present_id(pi, s));
				if (r == VK_SUCCESS && sc->sock >= 0) {
					struct qd_msg m = {QD_POSE};
					m.index = idx;
					m.frame = frame;
					if (send(sc->sock, &m, sizeof(m), MSG_NOSIGNAL) == sizeof(m)) {
						struct timespec dl;
						clock_gettime(CLOCK_REALTIME, &dl);
						dl.tv_nsec += QD_POSE_WAIT_NS;
						if (dl.tv_nsec >= 1000000000) {
							dl.tv_sec++;
							dl.tv_nsec -= 1000000000;
						}
						while (sc->posed_frame < frame && sc->sock >= 0 &&
						       pthread_cond_timedwait(&sc->cond, &sc->mutex, &dl) != ETIMEDOUT)
							;
						if (sc->posed_frame < frame)
							qd_stats.pose_timeouts++;
					}
				}
				if (r == VK_SUCCESS) {
					sc->pend[sc->pend_tail % 8].index = idx;
					sc->pend[sc->pend_tail % 8].frame = frame;
					sc->pend[sc->pend_tail % 8].t_present = t_entry;
					sc->pend_tail++;
				} else {
					sc->app_owned[idx] = false;
				}
				pthread_cond_broadcast(&sc->cond);
				pthread_mutex_unlock(&sc->mutex);
				qd_stats_present(t_entry, t_entry, now_ns());
				waited = true;
				if (pi->pResults)
					pi->pResults[s] = r;
				if (r != VK_SUCCESS && result == VK_SUCCESS)
					result = r;
				continue;
			}
			r = QueueSubmit(queue, 1, &si, sc->fence);
			if (r == VK_SUCCESS) {
				wait_fence(sc->d, sc->device, sc->fence);
				sc->d->ResetFences(sc->device, 1, &sc->fence);
			}
			t_gpu = now_ns();
			pthread_mutex_lock(&sc->mutex);
			sc->app_owned[idx] = false;
			qd_record_present_id(sc, sc->frame + 1, qd_present_id(pi, s));
			if (sc->sock >= 0) {
				struct qd_msg m = {QD_PRESENT};
				m.index = idx;
				m.frame = ++sc->frame;
				m.vblank_ns = sc->frame_tick[idx];
				if (send(sc->sock, &m, sizeof(m), MSG_NOSIGNAL) == sizeof(m)) {
					sc->driver_owned[idx] = true;
					// the driver publishes the pose of the next frame, which vrcompositor
					// must only read once we return: wait for it (bounded)
					struct timespec dl;
					clock_gettime(CLOCK_REALTIME, &dl);
					dl.tv_nsec += QD_POSE_WAIT_NS;
					if (dl.tv_nsec >= 1000000000) {
						dl.tv_sec++;
						dl.tv_nsec -= 1000000000;
					}
					while (sc->posed_frame < m.frame && sc->sock >= 0 &&
					       pthread_cond_timedwait(&sc->cond, &sc->mutex, &dl) != ETIMEDOUT)
						;
				}
			}
			pthread_cond_broadcast(&sc->cond);
			pthread_mutex_unlock(&sc->mutex);
			qd_stats_present(t_entry, t_gpu, now_ns());
			ft("presented", (int64_t)(t_gpu - t_entry));
		}
		waited = true;
		if (pi->pResults)
			pi->pResults[s] = r;
		if (r != VK_SUCCESS && result == VK_SUCCESS)
			result = r;
	}
	return result;
}
