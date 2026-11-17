// quest1_compat_timeline: emulated timeline semaphores (counters in shared memory), vkCreateSemaphore and its kin.
// Part of quest1_compat.c (included by it, in this order: one translation unit).

// --- emulated timeline semaphores ------------------------------------------------------------------

struct shared_counter
{
	_Atomic uint64_t value;
	_Atomic uint32_t seq; // futex word, bumped on every signal
};

struct tsem
{
	VkSemaphore handle; // a real binary semaphore, never used by the GPU: just a unique handle
	struct shared_counter *sh;
	int fd; // memfd backing sh (exported as the "OPAQUE_FD")
	_Atomic int refs;
	struct tsem *next;
};

static pthread_mutex_t g_sem_lock = PTHREAD_MUTEX_INITIALIZER;
static struct tsem *g_sems;

static struct tsem *tsem_get(VkSemaphore s)
{
	pthread_mutex_lock(&g_sem_lock);
	struct tsem *t = g_sems;
	while (t && t->handle != s)
		t = t->next;
	if (t)
		atomic_fetch_add(&t->refs, 1);
	pthread_mutex_unlock(&g_sem_lock);
	return t;
}

static void tsem_put(struct tsem *t)
{
	if (atomic_fetch_sub(&t->refs, 1) == 1) {
		munmap(t->sh, sizeof(*t->sh));
		close(t->fd);
		free(t);
	}
}

static bool counter_map(int fd, struct shared_counter **out)
{
	void *p = mmap(NULL, sizeof(**out), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED)
		return false;
	*out = p;
	return true;
}

static void counter_signal(struct shared_counter *c, uint64_t v)
{
	uint64_t cur = atomic_load(&c->value);
	while (cur < v && !atomic_compare_exchange_weak(&c->value, &cur, v)) {
	}
	atomic_fetch_add(&c->seq, 1);
	syscall(SYS_futex, &c->seq, FUTEX_WAKE, INT_MAX, NULL, NULL, 0); // shared futex: no _PRIVATE
}

//! Wait until c->value >= v or the deadline (CLOCK_MONOTONIC ns, UINT64_MAX = forever) passes.
static bool counter_wait(struct shared_counter *c, uint64_t v, uint64_t deadline)
{
	for (;;) {
		uint32_t seq = atomic_load(&c->seq);
		if (atomic_load(&c->value) >= v)
			return true;
		uint64_t now = now_ns();
		if (now >= deadline)
			return false;
		uint64_t left = deadline - now;
		if (left > 10000000ull)
			left = 10000000ull; // re-check every 10 ms (signals from a dying process)
		struct timespec ts = {(time_t)(left / 1000000000ull), (long)(left % 1000000000ull)};
		syscall(SYS_futex, &c->seq, FUTEX_WAIT, seq, &ts, NULL, 0);
	}
}

// The blob returns VK_TIMEOUT at once for an UINT64_MAX timeout (it overflows converting it), so wait
// in finite steps.
static VkResult wait_fence(struct device *d, VkDevice device, VkFence fence)
{
	VkResult r;
	while ((r = d->WaitForFences(device, 1, &fence, VK_TRUE, 100000000ull)) == VK_TIMEOUT)
		;
	return r;
}

static const void *find_struct(const void *chain, VkStructureType type)
{
	for (const VkBaseInStructure *s = chain; s; s = s->pNext)
		if (s->sType == type)
			return s;
	return NULL;
}

// --- vkCreateSemaphore / vkDestroySemaphore / counters --------------------------------------------

static VKAPI_ATTR VkResult VKAPI_CALL CreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo *ci,
                                                      const VkAllocationCallbacks *alloc, VkSemaphore *out)
{
	struct device *d = device_of(device);
	const VkSemaphoreTypeCreateInfo *type = find_struct(ci->pNext, VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO);
	bool timeline = type && type->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE;
	const VkExportSemaphoreCreateInfo *exp = find_struct(ci->pNext, VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO);

	// The blob gets a plain binary semaphore: no type info, and no OPAQUE_FD export it lacks.
	VkSemaphoreCreateInfo plain = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	plain.flags = ci->flags;
	VkExportSemaphoreCreateInfo sync_exp;
	if (!timeline && exp && (exp->handleTypes & VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT)) {
		sync_exp = *exp;
		sync_exp.pNext = NULL;
		sync_exp.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
		plain.pNext = &sync_exp;
	}
	VkResult r = d->CreateSemaphore(device, &plain, alloc, out);
	if (r != VK_SUCCESS || !timeline)
		return r;

	struct tsem *t = calloc(1, sizeof(*t));
	t->handle = *out;
	t->refs = 1;
	t->fd = memfd_create("quest1-timeline", MFD_CLOEXEC);
	if (t->fd < 0 || ftruncate(t->fd, 4096) != 0 || !counter_map(t->fd, &t->sh)) {
		LOG("cannot create a shared counter: %s\n", strerror(errno));
		if (t->fd >= 0)
			close(t->fd);
		free(t);
		d->DestroySemaphore(device, *out, alloc);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	atomic_store(&t->sh->value, type->initialValue);
	pthread_mutex_lock(&g_sem_lock);
	t->next = g_sems;
	g_sems = t;
	pthread_mutex_unlock(&g_sem_lock);
	return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroySemaphore(VkDevice device, VkSemaphore s, const VkAllocationCallbacks *alloc)
{
	struct device *d = device_of(device);
	pthread_mutex_lock(&g_sem_lock);
	for (struct tsem **p = &g_sems; *p; p = &(*p)->next) {
		if ((*p)->handle == s) {
			struct tsem *t = *p;
			*p = t->next;
			pthread_mutex_unlock(&g_sem_lock);
			tsem_put(t); // pending submissions keep their reference
			d->DestroySemaphore(device, s, alloc);
			return;
		}
	}
	pthread_mutex_unlock(&g_sem_lock);
	d->DestroySemaphore(device, s, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL GetSemaphoreCounterValue(VkDevice device, VkSemaphore s, uint64_t *v)
{
	(void)device;
	q1_rt_cap();
	struct tsem *t = tsem_get(s);
	if (!t)
		return VK_ERROR_UNKNOWN;
	*v = atomic_load(&t->sh->value);
	tsem_put(t);
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL SignalSemaphore(VkDevice device, const VkSemaphoreSignalInfo *info)
{
	(void)device;
	q1_rt_cap();
	struct tsem *t = tsem_get(info->semaphore);
	if (!t)
		return VK_ERROR_UNKNOWN;
	counter_signal(t->sh, info->value);
	tsem_put(t);
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL WaitSemaphores(VkDevice device, const VkSemaphoreWaitInfo *info, uint64_t timeout)
{
	(void)device;
	q1_rt_cap();
	uint64_t deadline = timeout == UINT64_MAX ? UINT64_MAX : now_ns() + timeout;
	bool any = info->flags & VK_SEMAPHORE_WAIT_ANY_BIT;
	for (;;) {
		uint32_t satisfied = 0;
		struct tsem *first_unsatisfied = NULL;
		uint32_t idx = 0;
		for (uint32_t i = 0; i < info->semaphoreCount; i++) {
			struct tsem *t = tsem_get(info->pSemaphores[i]);
			if (!t)
				return VK_ERROR_UNKNOWN;
			if (atomic_load(&t->sh->value) >= info->pValues[i]) {
				satisfied++;
				tsem_put(t);
			} else if (!first_unsatisfied) {
				first_unsatisfied = t;
				idx = i;
			} else {
				tsem_put(t);
			}
		}
		if ((any && satisfied > 0) || satisfied == info->semaphoreCount) {
			if (first_unsatisfied)
				tsem_put(first_unsatisfied);
			return VK_SUCCESS;
		}
		// with ANY, wake up regularly to look at the others too
		uint64_t until = any ? (now_ns() + 1000000ull < deadline ? now_ns() + 1000000ull : deadline) : deadline;
		bool ok = counter_wait(first_unsatisfied->sh, info->pValues[idx], until);
		tsem_put(first_unsatisfied);
		if (!ok && now_ns() >= deadline)
			return VK_TIMEOUT;
	}
}

static VKAPI_ATTR VkResult VKAPI_CALL GetSemaphoreFdKHR(VkDevice device, const VkSemaphoreGetFdInfoKHR *info, int *fd)
{
	struct device *d = device_of(device);
	struct tsem *t = tsem_get(info->semaphore);
	if (!t)
		return d->GetSemaphoreFdKHR ? d->GetSemaphoreFdKHR(device, info, fd) : VK_ERROR_INVALID_EXTERNAL_HANDLE;
	if (info->handleType != VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT) {
		tsem_put(t);
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;
	}
	*fd = fcntl(t->fd, F_DUPFD_CLOEXEC, 0);
	tsem_put(t);
	return *fd >= 0 ? VK_SUCCESS : VK_ERROR_TOO_MANY_OBJECTS;
}

static VKAPI_ATTR VkResult VKAPI_CALL ImportSemaphoreFdKHR(VkDevice device, const VkImportSemaphoreFdInfoKHR *info)
{
	struct device *d = device_of(device);
	struct tsem *t = tsem_get(info->semaphore);
	if (!t)
		return d->ImportSemaphoreFdKHR ? d->ImportSemaphoreFdKHR(device, info)
		                               : VK_ERROR_INVALID_EXTERNAL_HANDLE;
	if (info->handleType != VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT) {
		tsem_put(t);
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;
	}
	struct shared_counter *sh;
	if (!counter_map(info->fd, &sh)) {
		tsem_put(t);
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;
	}
	// The semaphore now shares the exporter's counter. Submissions in flight keep the old
	// mapping alive through their own references only for the tsem, so swap under the lock.
	pthread_mutex_lock(&g_sem_lock);
	struct shared_counter *old = t->sh;
	int old_fd = t->fd;
	t->sh = sh;
	t->fd = info->fd; // ownership transfers to the implementation on success
	pthread_mutex_unlock(&g_sem_lock);
	munmap(old, sizeof(*old));
	close(old_fd);
	tsem_put(t);
	return VK_SUCCESS;
}
