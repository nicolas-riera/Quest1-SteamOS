// quest1_compat_submit: the queue submission worker: submits off the caller's thread, fences given back, vkQueueSubmit.
// Part of quest1_compat.c (included by it, in this order: one translation unit).

// --- queue submission worker -------------------------------------------------------------------------

struct timeline_op
{
	struct tsem *sem;
	uint64_t value;
	VkPipelineStageFlags stage; //!< waits: the app's stage mask
	VkSemaphore bin;            //!< signals: binary semaphore signaled along; waits: the one waited on
	bool consumed;              //!< signals: a later submission waits on bin
};

struct batch
{
	VkSubmitInfo info; // binary semaphores and command buffers only, arrays owned below
	VkSemaphore *wait_sems;
	VkPipelineStageFlags *wait_stages;
	VkCommandBuffer *cmds;
	VkSemaphore *signal_sems;
	struct timeline_op *waits, *signals;
	uint32_t nwaits, nsignals;
};

struct submit_job
{
	struct submit_job *next;
	VkQueue queue;
	struct batch *batches;
	uint32_t nbatches;
	VkFence app_fence;
	VkFence fence; // internal, when there are timeline signals or binary semaphores to recycle
	bool has_signals;
};

static void job_free(struct submit_job *j)
{
	for (uint32_t b = 0; b < j->nbatches; b++) {
		struct batch *x = &j->batches[b];
		for (uint32_t i = 0; i < x->nwaits; i++)
			tsem_put(x->waits[i].sem);
		for (uint32_t i = 0; i < x->nsignals; i++)
			tsem_put(x->signals[i].sem);
		free(x->wait_sems);
		free(x->wait_stages);
		free(x->cmds);
		free(x->signal_sems);
		free(x->waits);
		free(x->signals);
	}
	free(j->batches);
	free(j);
}

static struct submit_job *job_build(VkQueue queue, uint32_t n, const VkSubmitInfo *submits, VkFence fence)
{
	struct submit_job *j = calloc(1, sizeof(*j));
	j->queue = queue;
	j->app_fence = fence;
	j->nbatches = n;
	j->batches = calloc(n ? n : 1, sizeof(struct batch));
	for (uint32_t b = 0; b < n; b++) {
		const VkSubmitInfo *s = &submits[b];
		struct batch *x = &j->batches[b];
		const VkTimelineSemaphoreSubmitInfo *tl =
		    find_struct(s->pNext, VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO);
		// room for a binary semaphore per timeline wait and signal (see worker_main)
		x->wait_sems = calloc(s->waitSemaphoreCount + 1, sizeof(VkSemaphore));
		x->wait_stages = calloc(s->waitSemaphoreCount + 1, sizeof(VkPipelineStageFlags));
		x->waits = calloc(s->waitSemaphoreCount + 1, sizeof(struct timeline_op));
		x->signal_sems = calloc(2 * s->signalSemaphoreCount + 1, sizeof(VkSemaphore));
		x->signals = calloc(s->signalSemaphoreCount + 1, sizeof(struct timeline_op));
		x->cmds = calloc(s->commandBufferCount + 1, sizeof(VkCommandBuffer));
		memcpy(x->cmds, s->pCommandBuffers, s->commandBufferCount * sizeof(VkCommandBuffer));

		uint32_t nb = 0;
		for (uint32_t i = 0; i < s->waitSemaphoreCount; i++) {
			struct tsem *t = tsem_get(s->pWaitSemaphores[i]);
			if (t) {
				x->waits[x->nwaits].sem = t;
				x->waits[x->nwaits].stage = s->pWaitDstStageMask[i];
				x->waits[x->nwaits++].value =
				    tl && i < tl->waitSemaphoreValueCount ? tl->pWaitSemaphoreValues[i] : 0;
			} else {
				x->wait_sems[nb] = s->pWaitSemaphores[i];
				x->wait_stages[nb++] = s->pWaitDstStageMask[i];
			}
		}
		uint32_t ns = 0;
		for (uint32_t i = 0; i < s->signalSemaphoreCount; i++) {
			struct tsem *t = tsem_get(s->pSignalSemaphores[i]);
			if (t) {
				x->signals[x->nsignals].sem = t;
				x->signals[x->nsignals++].value =
				    tl && i < tl->signalSemaphoreValueCount ? tl->pSignalSemaphoreValues[i] : 0;
				j->has_signals = true;
			} else {
				x->signal_sems[ns++] = s->pSignalSemaphores[i];
			}
		}
		x->info = (VkSubmitInfo){VK_STRUCTURE_TYPE_SUBMIT_INFO};
		// other pNext structures (device group, protected...) are not used by SteamVR
		x->info.waitSemaphoreCount = nb;
		x->info.pWaitSemaphores = x->wait_sems;
		x->info.pWaitDstStageMask = x->wait_stages;
		x->info.commandBufferCount = s->commandBufferCount;
		x->info.pCommandBuffers = x->cmds;
		x->info.signalSemaphoreCount = ns;
		x->info.pSignalSemaphores = x->signal_sems;
	}
	return j;
}

static VkFence fence_take(struct device *d)
{
	VkFence f = VK_NULL_HANDLE;
	pthread_mutex_lock(&d->mutex);
	if (d->fence_pool_count)
		f = d->fence_pool[--d->fence_pool_count];
	pthread_mutex_unlock(&d->mutex);
	if (f == VK_NULL_HANDLE) {
		VkFenceCreateInfo ci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		d->CreateFence(d->handle, &ci, NULL, &f);
	}
	return f;
}

static void fence_give(struct device *d, VkFence f)
{
	d->ResetFences(d->handle, 1, &f);
	pthread_mutex_lock(&d->mutex);
	if (d->fence_pool_count < 32)
		d->fence_pool[d->fence_pool_count++] = f;
	else
		d->DestroyFence(d->handle, f, NULL);
	pthread_mutex_unlock(&d->mutex);
}

static void *worker_main(void *arg)
{
	struct device *d = arg;
	pthread_setname_np(pthread_self(), "q1compat submit");
	q1_realtime("compat: submit thread", Q1_RT_MAX);
	pthread_mutex_lock(&d->mutex);
	for (;;) {
		while (!d->head && !d->stop)
			pthread_cond_wait(&d->cond, &d->mutex);
		if (!d->head && d->stop)
			break;
		struct submit_job *j = d->head;
		d->head = j->next;
		if (!d->head)
			d->tail = NULL;
		pthread_mutex_unlock(&d->mutex);

		// Timeline waits: the blob cannot wait for a value on the GPU. When an earlier submission of
		// this process signals the value and is still in flight, wait on the binary semaphore it
		// signals along (once: a binary wait consumes it); otherwise wait on the CPU. A CPU wait
		// drains the GPU and costs a thread round trip, a GPU wait keeps the queues busy.
		bool owns_bins = false;
		for (uint32_t b = 0; b < j->nbatches; b++) {
			struct batch *x = &j->batches[b];
			for (uint32_t i = 0; i < x->nwaits; i++) {
				struct timeline_op *w = &x->waits[i];
				struct shared_counter *sh = w->sem->sh;
				pthread_mutex_lock(&d->mutex);
				struct timeline_op *src = NULL;
				if (atomic_load(&sh->value) < w->value)
					for (uint32_t k = 0; k < d->npending && !src; k++) {
						struct timeline_op *o = d->pending[k];
						if (o->sem->sh == sh && o->value >= w->value && !o->consumed)
							src = o;
					}
				if (src) {
					src->consumed = true;
					w->bin = src->bin;
					x->wait_sems[x->info.waitSemaphoreCount] = w->bin;
					x->wait_stages[x->info.waitSemaphoreCount++] = w->stage;
					owns_bins = true;
					d->gpu_waits++;
				}
				pthread_mutex_unlock(&d->mutex);
				if (!src && atomic_load(&sh->value) < w->value) {
					uint64_t t0 = now_ns();
					counter_wait(sh, w->value, UINT64_MAX);
					d->cpu_waits++;
					d->cpu_wait_ns += now_ns() - t0;
				}
			}
			// every timeline signal also signals a fresh binary semaphore, for later GPU waits
			for (uint32_t i = 0; i < x->nsignals; i++) {
				VkSemaphoreCreateInfo ci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
				if (d->CreateSemaphore(d->handle, &ci, NULL, &x->signals[i].bin) == VK_SUCCESS)
					x->signal_sems[x->info.signalSemaphoreCount++] = x->signals[i].bin;
			}
		}

		VkSubmitInfo infos[64];
		uint32_t n = j->nbatches < 64 ? j->nbatches : 64;
		for (uint32_t b = 0; b < n; b++)
			infos[b] = j->batches[b].info;
		bool fenced = j->has_signals || owns_bins;
		if (fenced)
			j->fence = fence_take(d);

		pthread_mutex_lock(&d->queue_mutex);
		VkResult r = d->QueueSubmit(j->queue, n, infos, fenced ? j->fence : j->app_fence);
		if (r == VK_SUCCESS && fenced && j->app_fence != VK_NULL_HANDLE)
			r = d->QueueSubmit(j->queue, 0, NULL, j->app_fence);
		pthread_mutex_unlock(&d->queue_mutex);
		if (r != VK_SUCCESS)
			LOG("vkQueueSubmit failed: %d\n", r);

		pthread_mutex_lock(&d->mutex);
		// the signals are on the GPU now: later submissions may wait on their binary semaphores
		for (uint32_t b = 0; b < n && r == VK_SUCCESS; b++)
			for (uint32_t i = 0; i < j->batches[b].nsignals; i++)
				if (j->batches[b].signals[i].bin != VK_NULL_HANDLE && d->npending < 256)
					d->pending[d->npending++] = &j->batches[b].signals[i];
		uint64_t now = now_ns();
		if (!d->stat_since) {
			d->stat_since = now;
		} else if (now - d->stat_since > 5000000000ull) {
			LOG("timeline waits in 5 s: %llu on the GPU, %llu on the CPU (%.2f ms each)\n",
			    (unsigned long long)d->gpu_waits, (unsigned long long)d->cpu_waits,
			    d->cpu_waits ? d->cpu_wait_ns / (double)d->cpu_waits / 1e6 : 0.0);
			d->stat_since = now;
			d->gpu_waits = d->cpu_waits = d->cpu_wait_ns = 0;
		}
		j->next = NULL;
		if (d->done_tail)
			d->done_tail->next = j;
		else
			d->done_head = j;
		d->done_tail = j;
		pthread_cond_broadcast(&d->cond);
	}
	pthread_mutex_unlock(&d->mutex);
	return NULL;
}

static void *completer_main(void *arg)
{
	struct device *d = arg;
	pthread_setname_np(pthread_self(), "q1compat done");
	q1_realtime("compat: completion thread", Q1_RT_MAX);
	pthread_mutex_lock(&d->mutex);
	for (;;) {
		while (!d->done_head && !d->stop)
			pthread_cond_wait(&d->cond, &d->mutex);
		if (!d->done_head && d->stop && !d->head)
			break;
		if (!d->done_head)
			continue;
		struct submit_job *j = d->done_head;
		d->done_head = j->next;
		if (!d->done_head)
			d->done_tail = NULL;
		pthread_mutex_unlock(&d->mutex);

		if (j->fence != VK_NULL_HANDLE) {
			wait_fence(d, d->handle, j->fence);
			for (uint32_t b = 0; b < j->nbatches; b++)
				for (uint32_t i = 0; i < j->batches[b].nsignals; i++)
					counter_signal(j->batches[b].signals[i].sem->sh, j->batches[b].signals[i].value);
			fence_give(d, j->fence);
		}
		// Binary semaphores: one this job signaled that nobody waits on is idle now; one a later
		// job consumed is destroyed by that job; the ones this job waited on are done.
		pthread_mutex_lock(&d->mutex);
		for (uint32_t b = 0; b < j->nbatches; b++) {
			struct batch *x = &j->batches[b];
			for (uint32_t i = 0; i < x->nsignals; i++) {
				struct timeline_op *o = &x->signals[i];
				for (uint32_t k = 0; k < d->npending; k++)
					if (d->pending[k] == o) {
						d->pending[k] = d->pending[--d->npending];
						break;
					}
				if (o->bin != VK_NULL_HANDLE && !o->consumed)
					d->DestroySemaphore(d->handle, o->bin, NULL);
			}
			for (uint32_t i = 0; i < x->nwaits; i++)
				if (x->waits[i].bin != VK_NULL_HANDLE)
					d->DestroySemaphore(d->handle, x->waits[i].bin, NULL);
		}
		pthread_mutex_unlock(&d->mutex);
		job_free(j);

		pthread_mutex_lock(&d->mutex);
		d->inflight--;
		pthread_cond_broadcast(&d->cond);
	}
	pthread_mutex_unlock(&d->mutex);
	return NULL;
}

static void ft(const char *what, int64_t arg); // quest1_display.c (frame trace)

static VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue queue, uint32_t n, const VkSubmitInfo *submits, VkFence fence)
{
	struct device *d = device_of(queue);
	q1_rt_cap();
	ft("submit", (int64_t)(uint64_t)fence);
	struct submit_job *j = job_build(queue, n, submits, fence);
	pthread_mutex_lock(&d->mutex);
	if (d->tail)
		d->tail->next = j;
	else
		d->head = j;
	d->tail = j;
	d->inflight++;
	pthread_cond_broadcast(&d->cond);
	pthread_mutex_unlock(&d->mutex);
	return VK_SUCCESS;
}

static void drain(struct device *d)
{
	pthread_mutex_lock(&d->mutex);
	while (d->inflight > 0)
		pthread_cond_wait(&d->cond, &d->mutex);
	pthread_mutex_unlock(&d->mutex);
}

static VKAPI_ATTR VkResult VKAPI_CALL QueueWaitIdle(VkQueue queue)
{
	struct device *d = device_of(queue);
	drain(d);
	pthread_mutex_lock(&d->queue_mutex);
	VkResult r = d->QueueWaitIdle(queue);
	pthread_mutex_unlock(&d->queue_mutex);
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL DeviceWaitIdle(VkDevice device)
{
	struct device *d = device_of(device);
	drain(d);
	return d->DeviceWaitIdle(device);
}
