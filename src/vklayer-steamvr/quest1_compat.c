// VK_LAYER_QUEST1_steamvr_compat: lets SteamVR's vrcompositor (and vrclient in apps) run on the
// Adreno 540 Qualcomm blob, which lacks some extensions it requires (docs/steamvr-port.md §8,
// docs/adreno-vk-extensions.md).
//
// - Timeline semaphores (VK_KHR_timeline_semaphore) are emulated, including OPAQUE_FD export and
//   import across processes: each timeline semaphore is a 64-bit counter in a memfd page, waited
//   on with a futex. The blob only knows binary semaphores, so queue submissions go through one
//   worker thread per device that resolves timeline waits on the CPU, submits, and a completion
//   thread bumps the counters once the GPU work (an internal fence) is done.
// - Extensions only required by name on vrcompositor's default path are advertised and then
//   removed from vkCreateDevice: VK_EXT_shader_viewport_index_layer, VK_EXT_extended_dynamic_state3
//   (their entry points become no-ops). The others the blob implements but hides are enabled with
//   device/qgl_config.txt instead.
//
// Active only with QUEST1_STEAMVR_COMPAT=1 (implicit layer manifest enable_environment).
// QUEST1_COMPAT_FAKE_EXTS overrides the advertised list (comma separated).
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define LAYER_NAME "VK_LAYER_QUEST1_steamvr_compat"
#define EXPORT __attribute__((visibility("default")))

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "[quest1_compat] ");
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}
#define LOG(...) logf_(__VA_ARGS__)

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

// --- advertised extensions ------------------------------------------------------------------------

static const char *default_fake[] = {
    VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
    VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME,
    VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
    // vrcompositor requires these; it never presents through a swapchain as a virtual display
    VK_KHR_PRESENT_ID_EXTENSION_NAME,
    VK_KHR_PRESENT_WAIT_EXTENSION_NAME,
};
static const uint32_t default_fake_spec[] = {
    VK_KHR_TIMELINE_SEMAPHORE_SPEC_VERSION,
    VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_SPEC_VERSION,
    VK_EXT_EXTENDED_DYNAMIC_STATE_3_SPEC_VERSION,
    VK_KHR_PRESENT_ID_SPEC_VERSION,
    VK_KHR_PRESENT_WAIT_SPEC_VERSION,
};
static char fake_names[16][VK_MAX_EXTENSION_NAME_SIZE];
static uint32_t fake_spec[16];
static uint32_t fake_count;
static pthread_once_t fake_once = PTHREAD_ONCE_INIT;

static void init_fake(void)
{
	const char *env = getenv("QUEST1_COMPAT_FAKE_EXTS");
	if (env == NULL) {
		for (unsigned i = 0; i < sizeof(default_fake) / sizeof(default_fake[0]); i++) {
			snprintf(fake_names[fake_count], VK_MAX_EXTENSION_NAME_SIZE, "%s", default_fake[i]);
			fake_spec[fake_count++] = default_fake_spec[i];
		}
		return;
	}
	char buf[1024];
	snprintf(buf, sizeof(buf), "%s", env);
	for (char *save, *t = strtok_r(buf, ",", &save); t && fake_count < 16; t = strtok_r(NULL, ",", &save)) {
		snprintf(fake_names[fake_count], VK_MAX_EXTENSION_NAME_SIZE, "%s", t);
		fake_spec[fake_count++] = 1;
	}
}

static bool is_fake(const char *name)
{
	pthread_once(&fake_once, init_fake);
	for (uint32_t i = 0; i < fake_count; i++)
		if (strcmp(fake_names[i], name) == 0)
			return true;
	return false;
}

// --- dispatch ----------------------------------------------------------------------------------------

static inline void *key_of(const void *dispatchable) { return *(void **)dispatchable; }

struct instance
{
	void *key;
	VkInstance handle;
	PFN_vkGetInstanceProcAddr gipa;
	PFN_vkDestroyInstance DestroyInstance;
	PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
	PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2;
	PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2;
	PFN_vkGetPhysicalDeviceExternalSemaphoreProperties GetPhysicalDeviceExternalSemaphoreProperties;
};

struct submit_job;

struct device
{
	void *key;
	VkDevice handle;
	PFN_vkGetDeviceProcAddr gdpa;
	PFN_vkDestroyDevice DestroyDevice;
	PFN_vkCreateSemaphore CreateSemaphore;
	PFN_vkDestroySemaphore DestroySemaphore;
	PFN_vkQueueSubmit QueueSubmit;
	PFN_vkQueueWaitIdle QueueWaitIdle;
	PFN_vkCreateShaderModule CreateShaderModule;
	PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines;
	PFN_vkCreateComputePipelines CreateComputePipelines;
	PFN_vkDeviceWaitIdle DeviceWaitIdle;
	PFN_vkCreateFence CreateFence;
	PFN_vkDestroyFence DestroyFence;
	PFN_vkWaitForFences WaitForFences;
	PFN_vkResetFences ResetFences;
	PFN_vkGetSemaphoreFdKHR GetSemaphoreFdKHR;
	PFN_vkImportSemaphoreFdKHR ImportSemaphoreFdKHR;

	// submission worker (all queues of the device, in call order)
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	struct submit_job *head, *tail;       //!< waiting to be submitted
	struct submit_job *done_head, *done_tail; //!< submitted, GPU completion pending
	uint32_t inflight;                     //!< jobs not completed yet
	bool stop;
	pthread_t worker, completer;
	pthread_mutex_t queue_mutex; //!< serialises real queue calls with the app's
	VkFence fence_pool[32];
	uint32_t fence_pool_count;

	// timeline signals of submitted, not yet completed jobs, each with a binary semaphore the GPU
	// also signals: a later wait on them from this process becomes a GPU wait (under mutex)
	struct timeline_op *pending[256];
	uint32_t npending;
	uint64_t stat_since, gpu_waits, cpu_waits, cpu_wait_ns;
};

#define MAX_OBJS 8
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct instance *g_instances[MAX_OBJS];
static struct device *g_devices[MAX_OBJS];

static struct instance *instance_of(const void *dispatchable)
{
	void *k = key_of(dispatchable);
	pthread_mutex_lock(&g_lock);
	struct instance *r = NULL;
	for (int i = 0; i < MAX_OBJS; i++)
		if (g_instances[i] && g_instances[i]->key == k)
			r = g_instances[i];
	pthread_mutex_unlock(&g_lock);
	return r;
}

static struct device *device_of(const void *dispatchable)
{
	void *k = key_of(dispatchable);
	pthread_mutex_lock(&g_lock);
	struct device *r = NULL;
	for (int i = 0; i < MAX_OBJS; i++)
		if (g_devices[i] && g_devices[i]->key == k)
			r = g_devices[i];
	pthread_mutex_unlock(&g_lock);
	return r;
}

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

static VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue queue, uint32_t n, const VkSubmitInfo *submits, VkFence fence)
{
	struct device *d = device_of(queue);
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

// --- physical device queries ------------------------------------------------------------------------

static VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(VkPhysicalDevice pd, const char *layer,
                                                                         uint32_t *count, VkExtensionProperties *props)
{
	if (layer && strcmp(layer, LAYER_NAME) == 0) {
		*count = 0;
		return VK_SUCCESS;
	}
	struct instance *in = instance_of(pd);
	uint32_t n = 0;
	VkResult r = in->EnumerateDeviceExtensionProperties(pd, layer, &n, NULL);
	if (r != VK_SUCCESS || layer != NULL)
		return in->EnumerateDeviceExtensionProperties(pd, layer, count, props);
	VkExtensionProperties *all = calloc(n + 16, sizeof(*all));
	in->EnumerateDeviceExtensionProperties(pd, NULL, &n, all);
	pthread_once(&fake_once, init_fake);
	for (uint32_t f = 0; f < fake_count; f++) {
		bool present = false;
		for (uint32_t i = 0; i < n; i++)
			present |= strcmp(all[i].extensionName, fake_names[f]) == 0;
		if (!present) {
			snprintf(all[n].extensionName, VK_MAX_EXTENSION_NAME_SIZE, "%s", fake_names[f]);
			all[n++].specVersion = fake_spec[f];
		}
	}
	if (!props) {
		*count = n;
		free(all);
		return VK_SUCCESS;
	}
	uint32_t c = *count < n ? *count : n;
	memcpy(props, all, c * sizeof(*all));
	*count = c;
	free(all);
	return c < n ? VK_INCOMPLETE : VK_SUCCESS;
}

static void fill_fake_features(VkPhysicalDeviceFeatures2 *f)
{
	for (VkBaseOutStructure *s = (VkBaseOutStructure *)f->pNext; s; s = s->pNext) {
		if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES)
			((VkPhysicalDeviceTimelineSemaphoreFeatures *)s)->timelineSemaphore = VK_TRUE;
		else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR)
			((VkPhysicalDevicePresentIdFeaturesKHR *)s)->presentId = VK_TRUE;
		else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR)
			((VkPhysicalDevicePresentWaitFeaturesKHR *)s)->presentWait = VK_TRUE;
		else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT) {
			VkPhysicalDeviceExtendedDynamicState3FeaturesEXT *e = (void *)s;
			void *next = e->pNext;
			memset(e, 0, sizeof(*e));
			e->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT;
			e->pNext = next;
		}
	}
}

// Structures the blob (or the Android loader) does not know: loader chain links, the debug messenger of
// the stripped VK_EXT_debug_utils, and the feature/property structures of the advertised-only extensions.
static bool is_unknown_struct(VkStructureType t)
{
	switch (t) {
	case VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO:
	case VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO:
	case VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT:
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES:
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES:
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT:
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_PROPERTIES_EXT:
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR:
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR: return true;
	default: return false;
	}
}

// Unlink the structures the blob does not know from an output chain, call down, relink.
#define WITH_UNLINKED(head_ptr, call)                                                                                  \
	do {                                                                                                           \
		VkBaseOutStructure *saved[16];                                                                         \
		VkBaseOutStructure *prevs[16];                                                                         \
		int ns = 0;                                                                                            \
		VkBaseOutStructure *prev = (VkBaseOutStructure *)(head_ptr);                                           \
		for (VkBaseOutStructure *s = prev->pNext; s && ns < 16;) {                                             \
			VkBaseOutStructure *next = s->pNext;                                                           \
			if (is_unknown_struct(s->sType)) {                                                             \
				saved[ns] = s;                                                                         \
				prevs[ns++] = prev;                                                                    \
				prev->pNext = next;                                                                    \
			} else {                                                                                       \
				prev = s;                                                                              \
			}                                                                                              \
			s = next;                                                                                      \
		}                                                                                                      \
		call;                                                                                                  \
		for (int i = ns - 1; i >= 0; i--) {                                                                    \
			saved[i]->pNext = prevs[i]->pNext;                                                             \
			prevs[i]->pNext = saved[i];                                                                    \
		}                                                                                                      \
	} while (0)

static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f)
{
	struct instance *in = instance_of(pd);
	WITH_UNLINKED(f, in->GetPhysicalDeviceFeatures2(pd, f));
	fill_fake_features(f);
}

static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p)
{
	struct instance *in = instance_of(pd);
	WITH_UNLINKED(p, in->GetPhysicalDeviceProperties2(pd, p));
	for (VkBaseOutStructure *s = (VkBaseOutStructure *)p->pNext; s; s = s->pNext)
		if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES)
			((VkPhysicalDeviceTimelineSemaphoreProperties *)s)->maxTimelineSemaphoreValueDifference = UINT64_MAX;
}

static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceExternalSemaphoreProperties(
    VkPhysicalDevice pd, const VkPhysicalDeviceExternalSemaphoreInfo *info, VkExternalSemaphoreProperties *out)
{
	const VkSemaphoreTypeCreateInfo *type = find_struct(info->pNext, VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO);
	if ((type && type->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE) ||
	    info->handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT) {
		// emulated: timeline semaphores share their counter as an OPAQUE_FD (a memfd)
		bool ok = info->handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT && type &&
		          type->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE;
		out->exportFromImportedHandleTypes = ok ? VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT : 0;
		out->compatibleHandleTypes = out->exportFromImportedHandleTypes;
		out->externalSemaphoreFeatures =
		    ok ? VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT : 0;
		return;
	}
	struct instance *in = instance_of(pd);
	VkPhysicalDeviceExternalSemaphoreInfo plain = *info;
	plain.pNext = NULL;
	in->GetPhysicalDeviceExternalSemaphoreProperties(pd, &plain, out);
}

// --- VK_EXT_debug_utils stubs (the extension is stripped at instance creation) ---------------------------

static VKAPI_ATTR VkResult VKAPI_CALL CreateDebugUtilsMessenger(VkInstance instance,
                                                                const VkDebugUtilsMessengerCreateInfoEXT *ci,
                                                                const VkAllocationCallbacks *alloc,
                                                                VkDebugUtilsMessengerEXT *out)
{
	*out = (VkDebugUtilsMessengerEXT)(uintptr_t)1;
	return VK_SUCCESS;
}

// every other debug_utils command returns void or VkResult: VK_SUCCESS is 0
static VKAPI_ATTR VkResult VKAPI_CALL debug_utils_noop(void) { return VK_SUCCESS; }

static PFN_vkVoidFunction debug_utils_stub(const char *name)
{
	if (!strstr(name, "DebugUtils"))
		return NULL;
	if (strcmp(name, "vkCreateDebugUtilsMessengerEXT") == 0)
		return (PFN_vkVoidFunction)CreateDebugUtilsMessenger;
	return (PFN_vkVoidFunction)debug_utils_noop;
}

// --- simulated direct-mode display for vrcompositor ----------------------------------------------------

#include "quest1_display.c"


// --- fences ----------------------------------------------------------------------------------------------

// Long timeouts (UINT64_MAX in particular) make the blob return VK_TIMEOUT at once: wait in steps.
static VKAPI_ATTR VkResult VKAPI_CALL WaitForFences(VkDevice device, uint32_t n, const VkFence *fences, VkBool32 all,
                                                    uint64_t timeout)
{
	struct device *d = device_of(device);
	const uint64_t step = 100000000ull; // 100 ms
	if (timeout <= step)
		return d->WaitForFences(device, n, fences, all, timeout);
	uint64_t deadline = timeout == UINT64_MAX ? UINT64_MAX : now_ns() + timeout;
	for (;;) {
		VkResult r = d->WaitForFences(device, n, fences, all, step);
		if (r != VK_TIMEOUT || (deadline != UINT64_MAX && now_ns() >= deadline))
			return r;
	}
}

// --- shader modules -------------------------------------------------------------------------------------
// QUEST1_COMPAT_SPIRV=1 logs the capabilities of every module (the Adreno compiler crashes on some),
// =2 also writes them to /tmp/quest1-spirv/<n>.spv

static atomic_uint counter;

struct spv_entry
{
	VkShaderModule module;
	unsigned index;
};
static struct spv_entry spv_modules[1024];
static unsigned spv_count;

// The Adreno compiler chokes on gl_Layer / gl_ViewportIndex outside geometry shaders: it crashes on
// vertex shaders that write them (SPV_EXT_shader_viewport_index_layer, which this layer only
// advertises) and fails fragment shaders that read gl_Layer. vrcompositor's distortion pass writes
// gl_Layer from a uniform, always 0 for its single-layer swapchain, and its fragment shader reads it
// back. So turn those built-ins into Private variables: written ones become no-ops, read ones read 0.
// The capabilities they needed (ShaderViewportIndexLayerEXT, and Geometry when nothing else uses it)
// are dropped. Returns a malloc'ed module (words in *out_words) or NULL when there is nothing to do.
static uint32_t *rewrite_layer_builtins(const uint32_t *w, uint32_t words, uint32_t *out_words)
{
	enum { CAP = 17, EXT = 10, ENTRY = 15, DECORATE = 71, TYPE_POINTER = 32, VARIABLE = 59 };
	enum { STORAGE_INPUT = 1, STORAGE_OUTPUT = 3, STORAGE_PRIVATE = 6 };
	enum { BUILTIN = 11, BI_PRIMITIVE_ID = 7, BI_LAYER = 9, BI_VIEWPORT_INDEX = 10 };
	enum { CAP_GEOMETRY = 2, CAP_VIEWPORT_INDEX_LAYER = 5254 };
	struct { uint32_t id, ptr, storage; } vars[8];
	struct { uint32_t ptr, storage, newptr, zero; } ptrs[8];
	uint32_t nvars = 0, nptrs = 0, decorated[8], ndec = 0;
	bool primitive_id = false;

	// pass 1: the Layer/ViewportIndex built-ins declared as Input or Output variables
	for (uint32_t i = 5; i < words;) {
		uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
		if (wc == 0 || i + wc > words)
			return NULL;
		if (op == DECORATE && wc >= 4 && w[i + 2] == BUILTIN) {
			if ((w[i + 3] == BI_LAYER || w[i + 3] == BI_VIEWPORT_INDEX) && ndec < 8)
				decorated[ndec++] = w[i + 1];
			primitive_id |= w[i + 3] == BI_PRIMITIVE_ID;
		}
		if (op == VARIABLE && (w[i + 3] == STORAGE_INPUT || w[i + 3] == STORAGE_OUTPUT))
			for (uint32_t k = 0; k < ndec; k++)
				if (decorated[k] == w[i + 2] && nvars < 8) {
					vars[nvars].id = w[i + 2];
					vars[nvars].ptr = w[i + 1];
					vars[nvars++].storage = w[i + 3];
					bool known = false;
					for (uint32_t p = 0; p < nptrs; p++)
						known |= ptrs[p].ptr == w[i + 1];
					if (!known && nptrs < 8) {
						ptrs[nptrs].ptr = w[i + 1];
						ptrs[nptrs++].storage = w[i + 3];
					}
				}
		i += wc;
	}
	if (nvars == 0)
		return NULL;

	// per pointer type: a new pointer + a zero constant (8 words); per variable: an initialiser word
	uint32_t *o = malloc((words + 8 * nptrs + nvars) * sizeof(uint32_t));
	memcpy(o, w, 5 * sizeof(uint32_t));
	uint32_t n = 5, bound = w[3];
	for (uint32_t p = 0; p < nptrs; p++) {
		ptrs[p].newptr = bound++;
		ptrs[p].zero = ptrs[p].storage == STORAGE_INPUT ? bound++ : 0;
	}
	o[3] = bound;
	bool keep_interface = w[1] >= 0x10400; // SPIR-V 1.4+ lists Private variables in the interface too
	bool reads = false;
	for (uint32_t v = 0; v < nvars; v++)
		reads |= vars[v].storage == STORAGE_INPUT;
	bool drop_geometry = reads && !primitive_id;

	for (uint32_t i = 5; i < words;) {
		uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
		const uint32_t *ins = &w[i];
		i += wc;
		int var = -1;
		for (uint32_t v = 0; v < nvars; v++)
			if ((op == DECORATE && ins[1] == vars[v].id) || (op == VARIABLE && ins[2] == vars[v].id))
				var = (int)v;

		if (op == CAP && (ins[1] == CAP_VIEWPORT_INDEX_LAYER || (ins[1] == CAP_GEOMETRY && drop_geometry)))
			continue;
		if (op == EXT && strcmp((const char *)&ins[1], "SPV_EXT_shader_viewport_index_layer") == 0)
			continue;
		if (op == DECORATE && var >= 0)
			continue;
		if (op == ENTRY && !keep_interface) {
			// [model, id, name (nul-terminated string words), interface ids...]
			uint32_t s = 3;
			while (s < wc && memchr(&ins[s], 0, 4) == NULL)
				s++;
			s++; // the word holding the terminator
			uint32_t start = n;
			memcpy(&o[n], ins, s * sizeof(uint32_t));
			n += s;
			for (uint32_t k = s; k < wc; k++) {
				bool drop = false;
				for (uint32_t v = 0; v < nvars; v++)
					drop |= ins[k] == vars[v].id;
				if (!drop)
					o[n++] = ins[k];
			}
			o[start] = ((n - start) << 16) | ENTRY;
			continue;
		}
		if (op == VARIABLE && var >= 0) {
			for (uint32_t p = 0; p < nptrs; p++)
				if (ptrs[p].ptr == ins[1]) {
					o[n++] = ((ptrs[p].zero ? 5u : 4u) << 16) | VARIABLE;
					o[n++] = ptrs[p].newptr;
					o[n++] = ins[2];
					o[n++] = STORAGE_PRIVATE;
					if (ptrs[p].zero)
						o[n++] = ptrs[p].zero; // initialiser: what reading gl_Layer gives
				}
			continue;
		}
		memcpy(&o[n], ins, wc * sizeof(uint32_t));
		n += wc;
		if (op == TYPE_POINTER)
			for (uint32_t p = 0; p < nptrs; p++)
				if (ptrs[p].ptr == ins[1]) {
					o[n++] = (4u << 16) | TYPE_POINTER;
					o[n++] = ptrs[p].newptr;
					o[n++] = STORAGE_PRIVATE;
					o[n++] = ins[3];
					if (ptrs[p].zero) { // OpConstant <int type> <id> 0
						o[n++] = (4u << 16) | 43;
						o[n++] = ins[3];
						o[n++] = ptrs[p].zero;
						o[n++] = 0;
					}
				}
	}
	*out_words = n;
	return o;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *ci,
                                                         const VkAllocationCallbacks *alloc, VkShaderModule *out)
{
	VkShaderModuleCreateInfo rewritten;
	uint32_t *code = NULL, words = 0;
	if (ci->codeSize >= 20 && ci->pCode[0] == 0x07230203 && !getenv("QUEST1_COMPAT_NO_SPIRV_REWRITE") &&
	    (code = rewrite_layer_builtins(ci->pCode, ci->codeSize / 4, &words))) {
		rewritten = *ci;
		rewritten.pCode = code;
		rewritten.codeSize = words * 4;
		ci = &rewritten;
		LOG("shader module: gl_Layer/gl_ViewportIndex turned into private variables\n");
	}
	const char *dbg = getenv("QUEST1_COMPAT_SPIRV");
	if (dbg && ci->codeSize >= 20 && ci->pCode[0] == 0x07230203) {
		unsigned n = atomic_fetch_add(&counter, 1);
		char caps[512] = "";
		size_t len = 0;
		const uint32_t *w = ci->pCode, words = ci->codeSize / 4;
		for (uint32_t i = 5; i < words;) {
			uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
			if (wc == 0)
				break;
			if (op == 17 && wc >= 2 && len < sizeof(caps) - 12) // OpCapability
				len += snprintf(caps + len, sizeof(caps) - len, " %u", w[i + 1]);
			if (op == 54) // OpFunction: the preamble is over
				break;
			i += wc;
		}
		LOG("shader module %u: %zu bytes, capabilities%s\n", n, ci->codeSize, caps);
		if (atoi(dbg) >= 2) {
			char path[64];
			mkdir("/tmp/quest1-spirv", 0777);
			snprintf(path, sizeof(path), "/tmp/quest1-spirv/%u.spv", n);
			FILE *f = fopen(path, "wb");
			if (f) {
				fwrite(ci->pCode, 1, ci->codeSize, f);
				fclose(f);
			}
		}
	}
	VkResult r = device_of(device)->CreateShaderModule(device, ci, alloc, out);
	free(code);
	if (dbg && r == VK_SUCCESS) {
		pthread_mutex_lock(&g_lock);
		spv_modules[spv_count++ % 1024] = (struct spv_entry){*out, atomic_load(&counter) - 1};
		pthread_mutex_unlock(&g_lock);
	}
	return r;
}

static int spv_index(VkShaderModule m)
{
	int r = -1;
	pthread_mutex_lock(&g_lock);
	for (unsigned i = 0; i < 1024; i++)
		if (spv_modules[i].module == m)
			r = (int)spv_modules[i].index;
	pthread_mutex_unlock(&g_lock);
	return r;
}

static void log_stages(const char *what, const VkPipelineShaderStageCreateInfo *st, uint32_t n)
{
	char buf[256] = "";
	size_t len = 0;
	for (uint32_t i = 0; i < n && len < sizeof(buf) - 32; i++)
		len += snprintf(buf + len, sizeof(buf) - len, " stage 0x%x=module %d", st[i].stage, spv_index(st[i].module));
	LOG("%s:%s\n", what, buf);
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t n,
                                                              const VkGraphicsPipelineCreateInfo *ci,
                                                              const VkAllocationCallbacks *alloc, VkPipeline *out)
{
	if (getenv("QUEST1_COMPAT_SPIRV"))
		for (uint32_t i = 0; i < n; i++)
			log_stages("graphics pipeline", ci[i].pStages, ci[i].stageCount);
	VkResult r = device_of(device)->CreateGraphicsPipelines(device, cache, n, ci, alloc, out);
	if (getenv("QUEST1_COMPAT_SPIRV"))
		LOG("  -> %d\n", r);
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t n,
                                                             const VkComputePipelineCreateInfo *ci,
                                                             const VkAllocationCallbacks *alloc, VkPipeline *out)
{
	if (getenv("QUEST1_COMPAT_SPIRV"))
		for (uint32_t i = 0; i < n; i++)
			log_stages("compute pipeline", &ci[i].stage, 1);
	VkResult r = device_of(device)->CreateComputePipelines(device, cache, n, ci, alloc, out);
	if (getenv("QUEST1_COMPAT_SPIRV"))
		LOG("  -> %d\n", r);
	return r;
}

// --- device creation -----------------------------------------------------------------------------------

static VKAPI_ATTR void VKAPI_CALL noop_cmd(void) {}

static PFN_vkVoidFunction device_intercept(const char *name);

static VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci,
                                                   const VkAllocationCallbacks *alloc, VkDevice *out)
{
	VkLayerDeviceCreateInfo *chain = (VkLayerDeviceCreateInfo *)ci->pNext;
	while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO))
		chain = (VkLayerDeviceCreateInfo *)chain->pNext;
	if (!chain)
		return VK_ERROR_INITIALIZATION_FAILED;
	PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	PFN_vkGetDeviceProcAddr gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
	chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
	struct instance *in = instance_of(pd);
	PFN_vkCreateDevice next_create = (PFN_vkCreateDevice)gipa(in->handle, "vkCreateDevice");

	// remove the advertised-only extensions and their feature structures
	const char **exts = calloc(ci->enabledExtensionCount + 1, sizeof(char *));
	uint32_t ne = 0;
	for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
		if (is_fake(ci->ppEnabledExtensionNames[i]))
			LOG("emulating %s\n", ci->ppEnabledExtensionNames[i]);
		else
			exts[ne++] = ci->ppEnabledExtensionNames[i];
	}
	VkDeviceCreateInfo copy = *ci;
	copy.enabledExtensionCount = ne;
	copy.ppEnabledExtensionNames = exts;

	// vrcompositor asks for a HIGH/REALTIME global-priority queue, which the blob refuses to non-root
	// processes (VK_ERROR_NOT_PERMITTED): drop the priority request, keep default priority queues
	VkDeviceQueueCreateInfo queues[8];
	if (!getenv("QUEST1_COMPAT_KEEP_PRIORITY") && ci->queueCreateInfoCount <= 8) {
		for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++) {
			queues[i] = ci->pQueueCreateInfos[i];
			const VkBaseInStructure **link = (const VkBaseInStructure **)&queues[i].pNext;
			while (*link) {
				if ((*link)->sType == VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR) {
					LOG("dropping the global priority of queue family %u\n", queues[i].queueFamilyIndex);
					// unlinked from our copy when first in the chain; deeper, this edits the
					// caller's chain (vrcompositor passes the priority struct alone)
					*link = (*link)->pNext;
					break;
				}
				link = (const VkBaseInStructure **)&(*link)->pNext;
			}
		}
		copy.pQueueCreateInfos = queues;
	}

	VkResult r;
	WITH_UNLINKED(&copy, r = next_create(pd, &copy, alloc, out));
	free(exts);
	if (r != VK_SUCCESS)
		return r;

	struct device *d = calloc(1, sizeof(*d));
	d->key = key_of(*out);
	d->handle = *out;
	d->gdpa = gdpa;
#define GET(fn) d->fn = (PFN_vk##fn)gdpa(*out, "vk" #fn)
	GET(DestroyDevice);
	GET(CreateSemaphore);
	GET(DestroySemaphore);
	GET(QueueSubmit);
	GET(QueueWaitIdle);
	GET(CreateShaderModule);
	GET(CreateGraphicsPipelines);
	GET(CreateComputePipelines);
	GET(DeviceWaitIdle);
	GET(CreateFence);
	GET(DestroyFence);
	GET(WaitForFences);
	GET(ResetFences);
	GET(GetSemaphoreFdKHR);
	GET(ImportSemaphoreFdKHR);
#undef GET
	pthread_mutex_init(&d->mutex, NULL);
	pthread_mutex_init(&d->queue_mutex, NULL);
	pthread_cond_init(&d->cond, NULL);
	pthread_create(&d->worker, NULL, worker_main, d);
	pthread_create(&d->completer, NULL, completer_main, d);

	pthread_mutex_lock(&g_lock);
	for (int i = 0; i < MAX_OBJS; i++)
		if (!g_devices[i]) {
			g_devices[i] = d;
			break;
		}
	pthread_mutex_unlock(&g_lock);
	return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc)
{
	struct device *d = device_of(device);
	drain(d);
	pthread_mutex_lock(&d->mutex);
	d->stop = true;
	pthread_cond_broadcast(&d->cond);
	pthread_mutex_unlock(&d->mutex);
	pthread_join(d->worker, NULL);
	pthread_join(d->completer, NULL);
	for (uint32_t i = 0; i < d->fence_pool_count; i++)
		d->DestroyFence(device, d->fence_pool[i], NULL);
	pthread_mutex_lock(&g_lock);
	for (int i = 0; i < MAX_OBJS; i++)
		if (g_devices[i] == d)
			g_devices[i] = NULL;
	pthread_mutex_unlock(&g_lock);
	PFN_vkDestroyDevice destroy = d->DestroyDevice;
	free(d);
	destroy(device, alloc);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char *name)
{
	PFN_vkVoidFunction f = device_intercept(name);
	if (!f)
		f = debug_utils_stub(name);
	if (!f)
		f = qd_proc(name);
	if (f)
		return f;
	struct device *d = device_of(device);
	f = d->gdpa(device, name);
	// entry points of the advertised-only extensions (EDS3) are no-ops: SteamVR only calls them
	// from the facet renderer and motion smoothing, both off
	if (!f && strncmp(name, "vkCmdSet", 8) == 0 && strstr(name, "EXT"))
		f = (PFN_vkVoidFunction)noop_cmd;
	return f;
}

static PFN_vkVoidFunction device_intercept(const char *name)
{
#define I(n, fn)                                                                                                       \
	if (strcmp(name, n) == 0)                                                                                      \
		return (PFN_vkVoidFunction)fn;
	I("vkGetDeviceProcAddr", GetDeviceProcAddr)
	I("vkDestroyDevice", DestroyDevice)
	I("vkCreateSemaphore", CreateSemaphore)
	I("vkDestroySemaphore", DestroySemaphore)
	I("vkGetSemaphoreCounterValue", GetSemaphoreCounterValue)
	I("vkGetSemaphoreCounterValueKHR", GetSemaphoreCounterValue)
	I("vkSignalSemaphore", SignalSemaphore)
	I("vkSignalSemaphoreKHR", SignalSemaphore)
	I("vkWaitSemaphores", WaitSemaphores)
	I("vkWaitSemaphoresKHR", WaitSemaphores)
	I("vkGetSemaphoreFdKHR", GetSemaphoreFdKHR)
	I("vkImportSemaphoreFdKHR", ImportSemaphoreFdKHR)
	I("vkQueueSubmit", QueueSubmit)
	I("vkQueueWaitIdle", QueueWaitIdle)
	I("vkCreateShaderModule", CreateShaderModule)
	I("vkWaitForFences", WaitForFences)
	I("vkCreateGraphicsPipelines", CreateGraphicsPipelines)
	I("vkCreateComputePipelines", CreateComputePipelines)
	I("vkDeviceWaitIdle", DeviceWaitIdle)
#undef I
	return NULL;
}

// --- instance ------------------------------------------------------------------------------------------

static PFN_vkVoidFunction instance_intercept(const char *name);

static VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *alloc,
                                                     VkInstance *out)
{
	VkLayerInstanceCreateInfo *chain = (VkLayerInstanceCreateInfo *)ci->pNext;
	while (chain &&
	       !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO))
		chain = (VkLayerInstanceCreateInfo *)chain->pNext;
	if (!chain)
		return VK_ERROR_INITIALIZATION_FAILED;
	PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
	PFN_vkCreateInstance next_create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");

	// The Android loader lists VK_EXT_debug_utils but fails vkCreateInstance with it
	// (VK_ERROR_EXTENSION_NOT_PRESENT): drop it, its entry points become no-ops (debug_utils_stub)
	const char **exts = calloc(ci->enabledExtensionCount + 1, sizeof(char *));
	uint32_t ne = 0;
	for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
		if (strcmp(ci->ppEnabledExtensionNames[i], VK_EXT_DEBUG_UTILS_EXTENSION_NAME) != 0)
			exts[ne++] = ci->ppEnabledExtensionNames[i];
	VkInstanceCreateInfo copy = *ci;
	copy.enabledExtensionCount = ne;
	copy.ppEnabledExtensionNames = exts;
	VkResult r;
	WITH_UNLINKED(&copy, r = next_create(&copy, alloc, out));
	free(exts);
	if (r != VK_SUCCESS) {
		LOG("vkCreateInstance failed: %d\n", r);
		return r;
	}

	struct instance *in = calloc(1, sizeof(*in));
	in->key = key_of(*out);
	in->handle = *out;
	in->gipa = gipa;
	in->DestroyInstance = (PFN_vkDestroyInstance)gipa(*out, "vkDestroyInstance");
	in->EnumerateDeviceExtensionProperties =
	    (PFN_vkEnumerateDeviceExtensionProperties)gipa(*out, "vkEnumerateDeviceExtensionProperties");
	in->GetPhysicalDeviceFeatures2 = (PFN_vkGetPhysicalDeviceFeatures2)gipa(*out, "vkGetPhysicalDeviceFeatures2");
	if (!in->GetPhysicalDeviceFeatures2)
		in->GetPhysicalDeviceFeatures2 =
		    (PFN_vkGetPhysicalDeviceFeatures2)gipa(*out, "vkGetPhysicalDeviceFeatures2KHR");
	in->GetPhysicalDeviceProperties2 =
	    (PFN_vkGetPhysicalDeviceProperties2)gipa(*out, "vkGetPhysicalDeviceProperties2");
	if (!in->GetPhysicalDeviceProperties2)
		in->GetPhysicalDeviceProperties2 =
		    (PFN_vkGetPhysicalDeviceProperties2)gipa(*out, "vkGetPhysicalDeviceProperties2KHR");
	in->GetPhysicalDeviceExternalSemaphoreProperties = (PFN_vkGetPhysicalDeviceExternalSemaphoreProperties)gipa(
	    *out, "vkGetPhysicalDeviceExternalSemaphoreProperties");
	if (!in->GetPhysicalDeviceExternalSemaphoreProperties)
		in->GetPhysicalDeviceExternalSemaphoreProperties = (PFN_vkGetPhysicalDeviceExternalSemaphoreProperties)gipa(
		    *out, "vkGetPhysicalDeviceExternalSemaphorePropertiesKHR");

	pthread_mutex_lock(&g_lock);
	for (int i = 0; i < MAX_OBJS; i++)
		if (!g_instances[i]) {
			g_instances[i] = in;
			break;
		}
	pthread_mutex_unlock(&g_lock);
	LOG("active\n");
	return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks *alloc)
{
	struct instance *in = instance_of(instance);
	pthread_mutex_lock(&g_lock);
	for (int i = 0; i < MAX_OBJS; i++)
		if (g_instances[i] == in)
			g_instances[i] = NULL;
	pthread_mutex_unlock(&g_lock);
	PFN_vkDestroyInstance destroy = in->DestroyInstance;
	free(in);
	destroy(instance, alloc);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char *name)
{
	PFN_vkVoidFunction f = instance_intercept(name);
	if (f)
		return f;
	f = device_intercept(name);
	if (!f)
		f = debug_utils_stub(name);
	if (!f)
		f = qd_proc(name);
	if (f)
		return f;
	struct instance *in = instance ? instance_of(instance) : NULL;
	return in ? in->gipa(instance, name) : NULL;
}

static PFN_vkVoidFunction instance_intercept(const char *name)
{
#define I(n, fn)                                                                                                       \
	if (strcmp(name, n) == 0)                                                                                      \
		return (PFN_vkVoidFunction)fn;
	I("vkGetInstanceProcAddr", GetInstanceProcAddr)
	I("vkCreateInstance", CreateInstance)
	I("vkDestroyInstance", DestroyInstance)
	I("vkCreateDevice", CreateDevice)
	I("vkEnumerateDeviceExtensionProperties", EnumerateDeviceExtensionProperties)
	I("vkGetPhysicalDeviceFeatures2", GetPhysicalDeviceFeatures2)
	I("vkGetPhysicalDeviceFeatures2KHR", GetPhysicalDeviceFeatures2)
	I("vkGetPhysicalDeviceProperties2", GetPhysicalDeviceProperties2)
	I("vkGetPhysicalDeviceProperties2KHR", GetPhysicalDeviceProperties2)
	I("vkGetPhysicalDeviceExternalSemaphoreProperties", GetPhysicalDeviceExternalSemaphoreProperties)
	I("vkGetPhysicalDeviceExternalSemaphorePropertiesKHR", GetPhysicalDeviceExternalSemaphoreProperties)
#undef I
	return NULL;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v)
{
	if (v->loaderLayerInterfaceVersion > 2)
		v->loaderLayerInterfaceVersion = 2;
	v->pfnGetInstanceProcAddr = GetInstanceProcAddr;
	v->pfnGetDeviceProcAddr = GetDeviceProcAddr;
	v->pfnGetPhysicalDeviceProcAddr = NULL;
	return VK_SUCCESS;
}
