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

#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
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
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define LAYER_NAME "VK_LAYER_QUEST1_steamvr_compat"
#define EXPORT __attribute__((visibility("default")))

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// the log also goes to /tmp/quest1-compat-<program>.log: when Steam starts SteamVR, stderr is a socket
static FILE *log_file;
static void log_open(void)
{
	char path[128];
	snprintf(path, sizeof(path), "/tmp/quest1-compat-%s.log", program_invocation_short_name);
	log_file = fopen(path, "a");
	if (log_file)
		setvbuf(log_file, NULL, _IOLBF, 0);
}

static void logf_(const char *fmt, ...)
{
	static pthread_once_t once = PTHREAD_ONCE_INIT;
	pthread_once(&once, log_open);
	FILE *file = log_file;
	va_list ap;
	va_start(ap, fmt);
	if (file) {
		va_list ap2;
		va_copy(ap2, ap);
		vfprintf(file, fmt, ap2);
		va_end(ap2);
	}
	fprintf(stderr, "[quest1_compat] ");
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}
#define LOG(...) logf_(__VA_ARGS__)

//! Highest real-time priority SteamVR's threads keep. vrcompositor makes its RenderThread and SignalThread SCHED_RR 99
//! (it has CAP_SYS_NICE, as on desktop Linux); its RenderThread works several ms a frame on cores 4-6. Monado's vsync
//! thread (warp and commit, FIFO 98) and the MDP's display kernel thread (FIFO 97) must run the moment they wake:
//! behind those threads they woke up to 6 ms late while Steam worked, and the view trembled (worn, 2026-11-08).
//! Above Meta's tracking threads (FIFO 48 and 49) and every normal thread still.
#define Q1_RT_MAX 50

//! The calling thread made real-time (SCHED_RR prio) if it is not already. The layer's own threads on the frame's path
//! (vrcompositor's submissions, their completion, the frame handed to Monado, its commits' fences) work well under
//! 1 ms a frame each but ran as normal threads: under Steam's load (load average ~10) a frame done in time could reach
//! Monado after its warp had started, and the panels showed the previous frame again (2026-11-09).
static void q1_realtime(const char *who, int prio)
{
	int policy;
	struct sched_param sp;
	if (strcmp(program_invocation_short_name, "vrcompositor") != 0 || // not the dashboard's or other clients'
	    (pthread_getschedparam(pthread_self(), &policy, &sp) == 0 && policy != SCHED_OTHER))
		return;
	sp.sched_priority = prio;
	int e = pthread_setschedparam(pthread_self(), SCHED_RR, &sp);
	LOG("%s: SCHED_RR %d: %s\n", who, prio, e ? strerror(e) : "ok");
}

//! The calling thread's real-time priority brought down to Q1_RT_MAX, once per thread. Called from the Vulkan entry
//! points SteamVR's real-time threads use.
static void q1_rt_cap(void)
{
	static __thread bool done;
	if (done)
		return;
	done = true;
	int policy;
	struct sched_param sp;
	if (pthread_getschedparam(pthread_self(), &policy, &sp) != 0 || policy == SCHED_OTHER ||
	    sp.sched_priority <= Q1_RT_MAX)
		return;
	int was = sp.sched_priority;
	sp.sched_priority = Q1_RT_MAX;
	char name[16] = "";
	pthread_getname_np(pthread_self(), name, sizeof(name));
	int e = pthread_setschedparam(pthread_self(), policy, &sp);
	LOG("%s: real-time priority %d -> %d: %s\n", name, was, Q1_RT_MAX, e ? strerror(e) : "ok");
}

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static bool fx_enabled(void); // quest1_facet.c

// --- advertised extensions ------------------------------------------------------------------------

static const char *default_fake[] = {
    VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
    VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME,
    VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
    // vrcompositor requires these; it never presents through a swapchain as a virtual display
    VK_KHR_PRESENT_ID_EXTENSION_NAME,
    VK_KHR_PRESENT_WAIT_EXTENSION_NAME,
    // without it vrcompositor's vblank thread has no event source and busy-loops a whole core
    VK_EXT_DISPLAY_CONTROL_EXTENSION_NAME,
};
static const uint32_t default_fake_spec[] = {
    VK_KHR_TIMELINE_SEMAPHORE_SPEC_VERSION,
    VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_SPEC_VERSION,
    VK_EXT_EXTENDED_DYNAMIC_STATE_3_SPEC_VERSION,
    VK_KHR_PRESENT_ID_SPEC_VERSION,
    VK_KHR_PRESENT_WAIT_SPEC_VERSION,
    VK_EXT_DISPLAY_CONTROL_SPEC_VERSION,
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
		if (fx_enabled()) { // SteamVR's Facet renderer (quest1_facet.c)
			snprintf(fake_names[fake_count], VK_MAX_EXTENSION_NAME_SIZE, "%s", VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
			fake_spec[fake_count++] = VK_KHR_DYNAMIC_RENDERING_SPEC_VERSION;
			snprintf(fake_names[fake_count], VK_MAX_EXTENSION_NAME_SIZE, "%s", VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME);
			fake_spec[fake_count++] = VK_EXT_HOST_QUERY_RESET_SPEC_VERSION;
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
	PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
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
	PFN_vkDestroyShaderModule DestroyShaderModule;
	PFN_vkCreateImage CreateImage;
	PFN_vkAllocateMemory AllocateMemory;
	PFN_vkGetImageSubresourceLayout GetImageSubresourceLayout;
	PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines;
	PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout;
	PFN_vkCreatePipelineLayout CreatePipelineLayout;
	PFN_vkCreateRenderPass CreateRenderPass;
	PFN_vkCreateComputePipelines CreateComputePipelines;
	PFN_vkDeviceWaitIdle DeviceWaitIdle;
	PFN_vkGetQueryPoolResults GetQueryPoolResults;
	PFN_vkCreateFence CreateFence;
	PFN_vkDestroyFence DestroyFence;
	PFN_vkWaitForFences WaitForFences;
	PFN_vkResetFences ResetFences;
	PFN_vkGetSemaphoreFdKHR GetSemaphoreFdKHR;
	PFN_vkImportSemaphoreFdKHR ImportSemaphoreFdKHR;
	PFN_vkCmdUpdateBuffer CmdUpdateBuffer;
	PFN_vkCmdBindIndexBuffer CmdBindIndexBuffer;
	PFN_vkCmdCopyBuffer CmdCopyBuffer;
	PFN_vkBindBufferMemory BindBufferMemory;
	PFN_vkBindBufferMemory2 BindBufferMemory2;
	PFN_vkMapMemory MapMemory;
	PFN_vkUnmapMemory UnmapMemory;
	PFN_vkFreeMemory FreeMemory;
	PFN_vkCreateBuffer CreateBuffer;
	PFN_vkDestroyBuffer DestroyBuffer;
	PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
	PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
	PFN_vkFreeCommandBuffers FreeCommandBuffers;
	PFN_vkDestroyCommandPool DestroyCommandPool;
	PFN_vkBeginCommandBuffer BeginCommandBuffer;
	VkPhysicalDeviceMemoryProperties memprops; //!< for the vkCmdUpdateBuffer staging (ub_*)
	void *fx;              //!< Facet emulation state (quest1_facet.c)
	bool has_q0;           //!< first queue family of vkCreateDevice, for one-shot submissions
	uint32_t q0_family;

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
		else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
			VkPhysicalDeviceVulkan12Features *v = (void *)s;
			v->timelineSemaphore = VK_TRUE;
			v->hostQueryReset = fx_enabled();
		} else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES)
			((VkPhysicalDeviceVulkan13Features *)s)->dynamicRendering = fx_enabled();
		else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES)
			((VkPhysicalDeviceHostQueryResetFeatures *)s)->hostQueryReset = fx_enabled();
		else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES)
			((VkPhysicalDeviceDynamicRenderingFeatures *)s)->dynamicRendering = fx_enabled();
		else if (s->sType == 1000201000)
			memset((char *)s + sizeof(VkBaseOutStructure), 0, sizeof(VkBool32) * 2);
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
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES:
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES:
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES:
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES:
	case 1000201000: // PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_NV
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

//! QUEST1_FAKE_CPU_DEVICE=1: report the GPU as a CPU device. Mesa runs zink on X servers without
//! DRI3 (Xvfb) only through drisw, and there it only picks CPU devices (lavapipe); the rendering
//! still happens on the Adreno.
static void fake_device_type(VkPhysicalDeviceProperties *p)
{
	static int fake = -1;
	if (fake < 0)
		fake = getenv("QUEST1_FAKE_CPU_DEVICE") && atoi(getenv("QUEST1_FAKE_CPU_DEVICE")) == 1;
	if (fake)
		p->deviceType = VK_PHYSICAL_DEVICE_TYPE_CPU;
}

static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties(VkPhysicalDevice pd, VkPhysicalDeviceProperties *p)
{
	instance_of(pd)->GetPhysicalDeviceProperties(pd, p);
	fake_device_type(p);
}

static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p)
{
	struct instance *in = instance_of(pd);
	WITH_UNLINKED(p, in->GetPhysicalDeviceProperties2(pd, p));
	fake_device_type(&p->properties);
	for (VkBaseOutStructure *s = (VkBaseOutStructure *)p->pNext; s; s = s->pNext)
		if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES)
			((VkPhysicalDeviceTimelineSemaphoreProperties *)s)->maxTimelineSemaphoreValueDifference = UINT64_MAX;
		else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES)
			((VkPhysicalDeviceVulkan12Properties *)s)->maxTimelineSemaphoreValueDifference = UINT64_MAX;
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
#include "quest1_facet.c"


// --- fences ----------------------------------------------------------------------------------------------

// Long timeouts (UINT64_MAX in particular) make the blob return VK_TIMEOUT at once: wait in steps.
static VKAPI_ATTR VkResult VKAPI_CALL WaitForFences(VkDevice device, uint32_t n, const VkFence *fences, VkBool32 all,
                                                    uint64_t timeout)
{
	struct device *d = device_of(device);
	q1_rt_cap();
	const uint64_t step = 100000000ull; // 100 ms
	ft("waitfences", n ? (int64_t)(uint64_t)fences[0] : 0);
	if (timeout <= step) {
		VkResult r0 = d->WaitForFences(device, n, fences, all, timeout);
		ft("waited", r0);
		return r0;
	}
	uint64_t deadline = timeout == UINT64_MAX ? UINT64_MAX : now_ns() + timeout;
	for (;;) {
		VkResult r = d->WaitForFences(device, n, fences, all, step);
		if (r != VK_TIMEOUT || (deadline != UINT64_MAX && now_ns() >= deadline)) {
			ft("waited", r);
			return r;
		}
	}
}

// --- query results --------------------------------------------------------------------------------------
// vrcompositor reads its GPU timestamps with VK_QUERY_RESULT_WAIT_BIT right after it submits a frame, so its render
// thread sleeps until the GPU has finished that frame (the blob polls with usleep) and only then records the next
// one: CPU and GPU never overlap (2026-11-08: render thread 55 % of its time in that usleep, ~5 ms of CPU + ~12 ms
// of GPU a frame in series, 55 fps). The timestamps only feed its frame timing statistics. So a waiting read
// returns at once: the results if they are ready, else the last ones read for the same queries, while a helper
// thread waits for the real ones and keeps them for the next call (they are one frame old). The first read of a
// set of queries still waits. "qrwait" in /tmp/quest1-layer (read once a second) restores the plain wait.
#define QR_SLOTS 16
#define QR_MAX 1024
struct qr_slot
{
	VkDevice device;
	VkQueryPool pool;
	uint32_t first, count;
	VkDeviceSize stride;
	VkQueryResultFlags flags;
	size_t size;
	bool valid, pending;
	uint8_t data[QR_MAX];
};
static struct qr_slot qr_slots[QR_SLOTS];
static unsigned qr_next;
static pthread_mutex_t qr_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qr_cond = PTHREAD_COND_INITIALIZER;
static PFN_vkGetQueryPoolResults qr_real;
static bool qr_thread_started;
static _Atomic uint64_t qr_stat_cached, qr_stat_ready, qr_stat_waited;

static bool qr_plain_wait(void)
{
	static _Atomic uint64_t checked;
	static _Atomic bool plain;
	uint64_t t = now_ns();
	if (t - checked > 1000000000ull) {
		checked = t;
		char b[64] = {0};
		FILE *f = fopen("/tmp/quest1-layer", "r");
		if (f) {
			b[fread(b, 1, sizeof(b) - 1, f)] = 0;
			fclose(f);
		}
		plain = strstr(b, "qrwait") != NULL;
	}
	return plain;
}

static void *qr_thread(void *arg)
{
	(void)arg;
	pthread_setname_np(pthread_self(), "q1-queryresults");
	q1_rt_cap(); // started from the RenderThread: its priority is inherited
	uint8_t buf[QR_MAX];
	pthread_mutex_lock(&qr_lock);
	for (;;) {
		struct qr_slot *s = NULL;
		for (unsigned i = 0; i < QR_SLOTS && !s; i++)
			if (qr_slots[i].pending)
				s = &qr_slots[i];
		if (!s) {
			pthread_cond_wait(&qr_cond, &qr_lock);
			continue;
		}
		struct qr_slot q = *s;
		pthread_mutex_unlock(&qr_lock);
		VkResult r = qr_real(q.device, q.pool, q.first, q.count, q.size, buf, q.stride, q.flags);
		pthread_mutex_lock(&qr_lock);
		s->pending = false;
		if (r == VK_SUCCESS && s->pool == q.pool && s->first == q.first && s->size == q.size) {
			memcpy(s->data, buf, q.size);
			s->valid = true;
		}
	}
	return NULL;
}

static VKAPI_ATTR VkResult VKAPI_CALL GetQueryPoolResults(VkDevice device, VkQueryPool pool, uint32_t first,
                                                          uint32_t count, size_t size, void *data, VkDeviceSize stride,
                                                          VkQueryResultFlags flags)
{
	struct device *d = device_of(device);
	static _Atomic uint64_t qr_logged;
	uint64_t t = now_ns();
	if (t - qr_logged > 10000000000ull) {
		qr_logged = t;
		LOG("query results in 10 s: %llu ready, %llu from the last frame, %llu waited for\n",
		    (unsigned long long)atomic_exchange(&qr_stat_ready, 0), (unsigned long long)atomic_exchange(&qr_stat_cached, 0),
		    (unsigned long long)atomic_exchange(&qr_stat_waited, 0));
	}
	if (!(flags & VK_QUERY_RESULT_WAIT_BIT) || (flags & VK_QUERY_RESULT_PARTIAL_BIT) || size > QR_MAX ||
	    qr_plain_wait())
		return d->GetQueryPoolResults(device, pool, first, count, size, data, stride, flags);
	VkResult r = d->GetQueryPoolResults(device, pool, first, count, size, data, stride,
	                                    flags & ~VK_QUERY_RESULT_WAIT_BIT);
	pthread_mutex_lock(&qr_lock);
	struct qr_slot *s = NULL;
	for (unsigned i = 0; i < QR_SLOTS && !s; i++) {
		struct qr_slot *c = &qr_slots[i];
		if (c->device == device && c->pool == pool && c->first == first && c->count == count &&
		    c->stride == stride && c->flags == flags && c->size == size)
			s = c;
	}
	if (r == VK_SUCCESS) {
		if (s) {
			memcpy(s->data, data, size);
			s->valid = true;
		}
		pthread_mutex_unlock(&qr_lock);
		qr_stat_ready++;
		return r;
	}
	if (r != VK_NOT_READY || !s || !s->valid) {
		if (!s) { // remember these queries for the next frames
			s = &qr_slots[qr_next++ % QR_SLOTS];
			while (s->pending) { // the helper thread is reading into it: take another one
				pthread_mutex_unlock(&qr_lock);
				sched_yield();
				pthread_mutex_lock(&qr_lock);
			}
			*s = (struct qr_slot){device, pool, first, count, stride, flags, size, false, false, {0}};
		}
		pthread_mutex_unlock(&qr_lock);
		r = d->GetQueryPoolResults(device, pool, first, count, size, data, stride, flags);
		pthread_mutex_lock(&qr_lock);
		if (r == VK_SUCCESS && s->pool == pool && s->first == first && s->size == size) {
			memcpy(s->data, data, size);
			s->valid = true;
		}
		pthread_mutex_unlock(&qr_lock);
		qr_stat_waited++;
		return r;
	}
	memcpy(data, s->data, size);
	if (!s->pending) {
		s->pending = true;
		if (!qr_thread_started) {
			qr_real = d->GetQueryPoolResults;
			pthread_t th;
			qr_thread_started = pthread_create(&th, NULL, qr_thread, NULL) == 0;
			if (qr_thread_started)
				pthread_detach(th);
		}
		pthread_cond_signal(&qr_cond);
	}
	pthread_mutex_unlock(&qr_lock);
	qr_stat_cached++;
	return VK_SUCCESS;
}

// --- shader modules -------------------------------------------------------------------------------------
//! QUEST1_COMPAT_SPIRV set and not empty or "0" (steamvr-q1 and the frame-mode unit export it empty).
static bool spirv_debug(void)
{
	const char *e = getenv("QUEST1_COMPAT_SPIRV");
	return e && *e && strcmp(e, "0") != 0;
}

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

// Camera frames for vrcompositor's roomview (src/camtap/ptprovider): the provider allocates RGBA8 shared images and writes
// their memory with the CPU. With optimal tiling the Adreno blob compresses such images (UBWC), so raw bytes decode to
// blocks of noise. The provider marks them with an otherwise unused usage bit (input attachment); here they lose the
// mark and get linear tiling (no compression), and their row pitch is logged. In vrcompositor only, where those
// images are made: ANGLE (Steam's stream client) asks that usage for every texture of its own.
#define Q1_CPU_IMAGE_USAGE VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT

static bool cpu_image(const VkImageCreateInfo *ci)
{
	return (ci->usage & Q1_CPU_IMAGE_USAGE) && ci->format == VK_FORMAT_R8G8B8A8_UNORM &&
	       ci->tiling == VK_IMAGE_TILING_OPTIMAL && ci->mipLevels == 1 && ci->arrayLayers == 1 &&
	       ci->imageType == VK_IMAGE_TYPE_2D && !strcmp(program_invocation_short_name, "vrcompositor");
}

// Failed image creations and allocations are logged (the first 40): vrcompositor only says "CreateTexture - Unable to
// allocate memory".
static unsigned fail_logs;

// --- 32-bit indices bound as 16-bit ----------------------------------------------------------------
// vrcompositor uploads the room setup's meshes (boundary circle, drawn outline, drawing area) with their indices as
// 32-bit values, then binds those buffers with VK_INDEX_TYPE_UINT16 and the 32-bit index count. Read as 16-bit, every
// other index is 0: half the triangles ran to the mesh's first vertex (a fan of rays from one floor point). Small
// meshes come with vkCmdUpdateBuffer, big ones (a large drawn area) with vkCmdCopyBuffer from a mapped staging
// buffer. The (buffer, offset) ranges last filled with 32-bit shaped indices (every odd 16-bit word 0) are bound as
// VK_INDEX_TYPE_UINT32 instead. A real 16-bit index list with every second index 0 is not a mesh anyone draws.
#define U32_IBS 256
static struct { uint64_t buf, off; } u32_ibs[U32_IBS];
static pthread_mutex_t u32_lock = PTHREAD_MUTEX_INITIALIZER;
// where buffers are bound and memory is mapped, to read a copy's source
#define U32_BUFS 4096
static struct { uint64_t buf, mem, off; } u32_bufs[U32_BUFS];
static unsigned u32_buf_next;
#define U32_MAPS 1024
static struct { uint64_t mem; char *ptr; uint64_t off; } u32_maps[U32_MAPS];

static bool u32_shaped(const uint16_t *w, size_t n)
{
	if (n < 6)
		return false;
	bool nonzero = false;
	for (size_t i = 0; i + 1 < n; i += 2) {
		if (w[i + 1])
			return false;
		nonzero |= w[i] != 0;
	}
	return nonzero;
}

//! Under u32_lock: records whether (b, off) now holds 32-bit shaped indices.
static void u32_note(uint64_t b, uint64_t off, bool u32)
{
	int at = -1, free_at = -1;
	for (int i = 0; i < U32_IBS; i++) {
		if (u32_ibs[i].buf == b && u32_ibs[i].off == off)
			at = i;
		else if (!u32_ibs[i].buf && free_at < 0)
			free_at = i;
	}
	if (u32 && at < 0 && free_at >= 0) {
		u32_ibs[free_at].buf = b;
		u32_ibs[free_at].off = off;
	} else if (!u32 && at >= 0) {
		u32_ibs[at].buf = 0;
	}
}

// --- vkCmdUpdateBuffer through a staging buffer of the layer ---------------------------------------
// The blob gives every vkCmdUpdateBuffer a KGSL object of its own (4 KB, "vk_cmdbuffer") for the inline data and
// frees it when the command buffer is reset. vrcompositor makes ~18 a frame: ~940 allocations a second on its
// render thread, each an alloc ioctl, an mmap, an IOMMU map and an info ioctl, then the unmap and the free, while
// its frames are serial (CPU, then GPU: its CPU time is frame time). The layer copies the data into host-visible
// buffers of its own, kept per command buffer across resets, and records a vkCmdCopyBuffer from there: the same
// transfer write to the same range, so the app's barriers still cover it. A command buffer is never pending when it
// is begun again, freed or its pool destroyed (Vulkan's rules), so its staging is reused from the start at each
// vkBeginCommandBuffer. Command buffers the layer did not see allocated (its own) keep the blob's path.
#define UB_CHUNK (64 * 1024) // vkCmdUpdateBuffer's maximum size
#define UB_BUCKETS 512

struct ub_chunk
{
	VkBuffer buf;
	VkDeviceMemory mem;
	char *ptr;
	struct ub_chunk *next;
};

struct ub_arena
{
	VkCommandBuffer cb;
	VkCommandPool pool;
	struct device *d;
	struct ub_chunk *chunks, *cur; //!< cur: the chunk being filled
	VkDeviceSize pos;
	struct ub_arena *next;         //!< hash bucket
};

static struct ub_arena *ub_table[UB_BUCKETS];
static pthread_mutex_t ub_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned ub_hash(const void *cb) { return (unsigned)(((uintptr_t)cb >> 4) * 2654435761u) % UB_BUCKETS; }

static int ub_memory_type(struct device *d, uint32_t bits)
{
	const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	for (uint32_t i = 0; i < d->memprops.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (d->memprops.memoryTypes[i].propertyFlags & want) == want)
			return (int)i;
	return -1;
}

static struct ub_chunk *ub_chunk_new(struct device *d)
{
	VkDevice dev = d->handle;
	struct ub_chunk *c = calloc(1, sizeof(*c));
	VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = UB_CHUNK;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT; // and snapshots (rpmerge)
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (!c || d->CreateBuffer(dev, &bci, NULL, &c->buf) != VK_SUCCESS) {
		free(c);
		return NULL;
	}
	VkMemoryRequirements mr;
	d->GetBufferMemoryRequirements(dev, c->buf, &mr);
	int type = ub_memory_type(d, mr.memoryTypeBits);
	VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = type < 0 ? 0 : (uint32_t)type;
	void *p = NULL;
	if (type < 0 || d->AllocateMemory(dev, &mai, NULL, &c->mem) != VK_SUCCESS) {
		d->DestroyBuffer(dev, c->buf, NULL);
		free(c);
		return NULL;
	}
	if (d->BindBufferMemory(dev, c->buf, c->mem, 0) != VK_SUCCESS || d->MapMemory(dev, c->mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
		d->FreeMemory(dev, c->mem, NULL);
		d->DestroyBuffer(dev, c->buf, NULL);
		free(c);
		return NULL;
	}
	c->ptr = p;
	return c;
}

static void ub_arena_free(struct ub_arena *a)
{
	for (struct ub_chunk *c = a->chunks, *n; c; c = n) {
		n = c->next;
		a->d->UnmapMemory(a->d->handle, c->mem);
		a->d->DestroyBuffer(a->d->handle, c->buf, NULL);
		a->d->FreeMemory(a->d->handle, c->mem, NULL);
		free(c);
	}
	free(a);
}

//! Under ub_lock.
static struct ub_arena *ub_find(VkCommandBuffer cb)
{
	for (struct ub_arena *a = ub_table[ub_hash(cb)]; a; a = a->next)
		if (a->cb == cb)
			return a;
	return NULL;
}

//! Takes out of the table the arenas of cb (cb != NULL), of a pool (cb == NULL) or of a device (pool and cb NULL), and
//! frees them.
static void ub_drop(struct device *d, VkCommandPool pool, VkCommandBuffer cb)
{
	struct ub_arena *gone = NULL;
	pthread_mutex_lock(&ub_lock);
	for (unsigned i = cb ? ub_hash(cb) : 0; i < (cb ? ub_hash(cb) + 1 : UB_BUCKETS); i++)
		for (struct ub_arena **p = &ub_table[i]; *p;) {
			struct ub_arena *a = *p;
			if (a->d == d && (cb ? a->cb == cb : !pool || a->pool == pool)) {
				*p = a->next;
				a->next = gone;
				gone = a;
			} else {
				p = &a->next;
			}
		}
	pthread_mutex_unlock(&ub_lock);
	for (struct ub_arena *n; gone; gone = n) {
		n = gone->next;
		rm_cb_drop(gone->d, gone->cb);
		ub_arena_free(gone);
	}
}

static VKAPI_ATTR VkResult VKAPI_CALL AllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo *ai,
                                                             VkCommandBuffer *out)
{
	struct device *d = device_of(device);
	VkResult r = d->AllocateCommandBuffers(device, ai, out);
	if (r != VK_SUCCESS || getenv("QUEST1_BLOB_UPDATEBUFFER"))
		return r;
	for (uint32_t i = 0; i < ai->commandBufferCount; i++) {
		struct ub_arena *a = calloc(1, sizeof(*a));
		if (!a)
			continue;
		a->cb = out[i];
		a->pool = ai->commandPool;
		a->d = d;
		pthread_mutex_lock(&ub_lock);
		unsigned h = ub_hash(out[i]);
		a->next = ub_table[h];
		ub_table[h] = a;
		pthread_mutex_unlock(&ub_lock);
	}
	return r;
}

static VKAPI_ATTR void VKAPI_CALL FreeCommandBuffers(VkDevice device, VkCommandPool pool, uint32_t n,
                                                     const VkCommandBuffer *cbs)
{
	struct device *d = device_of(device);
	for (uint32_t i = 0; i < n; i++)
		if (cbs[i])
			ub_drop(d, pool, cbs[i]);
	d->FreeCommandBuffers(device, pool, n, cbs);
}

static VKAPI_ATTR void VKAPI_CALL DestroyCommandPool(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks *alloc)
{
	struct device *d = device_of(device);
	if (pool)
		ub_drop(d, pool, NULL);
	d->DestroyCommandPool(device, pool, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi)
{
	pthread_mutex_lock(&ub_lock);
	struct ub_arena *a = ub_find(cb);
	pthread_mutex_unlock(&ub_lock);
	if (a) {
		a->cur = a->chunks;
		a->pos = 0;
	}
	rm_cb_begin(device_of(cb), cb);
	qs_scene_cb_begin(cb);
	return device_of(cb)->BeginCommandBuffer(cb, bi);
}

//! size bytes of the command buffer's staging, 256-aligned (a uniform buffer offset), until it is begun again.
static bool ub_alloc(struct device *d, VkCommandBuffer cb, VkDeviceSize size, VkBuffer *buf, VkDeviceSize *off,
                     void **ptr)
{
	pthread_mutex_lock(&ub_lock);
	struct ub_arena *a = ub_find(cb);
	pthread_mutex_unlock(&ub_lock);
	if (!a || size > UB_CHUNK)
		return false;
	// recording is externally synchronized: the arena is this thread's until it ends
	a->pos = (a->pos + 255) & ~(VkDeviceSize)255;
	if (a->cur && a->pos + size > UB_CHUNK) {
		a->cur = a->cur->next;
		a->pos = 0;
	}
	if (!a->cur) {
		struct ub_chunk *c = ub_chunk_new(d);
		if (!c)
			return false;
		// appended after the chunks in use: a later begin starts again from the first
		struct ub_chunk **p = &a->chunks;
		while (*p)
			p = &(*p)->next;
		*p = c;
		a->cur = c;
		a->pos = 0;
	}
	*buf = a->cur->buf;
	*off = a->pos;
	*ptr = a->cur->ptr + a->pos;
	a->pos += size;
	return true;
}

//! Records data through the command buffer's staging, false if it has none (the caller uses the blob's path).
static bool ub_update(struct device *d, VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkDeviceSize size,
                      const void *data)
{
	VkBuffer sb;
	VkDeviceSize soff;
	void *p;
	if (!ub_alloc(d, cb, size, &sb, &soff, &p))
		return false;
	memcpy(p, data, size);
	VkBufferCopy r = {soff, off, size};
	d->CmdCopyBuffer(cb, sb, b, 1, &r);
	return true;
}

static VKAPI_ATTR void VKAPI_CALL CmdUpdateBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkDeviceSize size,
                                                  const void *data)
{
	if (rm_update(cb, b, off, size, data))
		return; // absorbed into a merged render pass (quest1_rpmerge.c)
	bool u32 = size % 4 == 0 && u32_shaped(data, size / 2);
	pthread_mutex_lock(&u32_lock);
	u32_note((uint64_t)b, off, u32);
	pthread_mutex_unlock(&u32_lock);
	struct device *d = device_of(cb);
	if (!ub_update(d, cb, b, off, size, data))
		d->CmdUpdateBuffer(cb, b, off, size, data);
}

static VKAPI_ATTR void VKAPI_CALL CmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t n,
                                                const VkBufferCopy *r)
{
	rm_flush(cb);
	for (uint32_t i = 0; i < n; i++)
		rm_buf_unknown(dst, r[i].dstOffset, r[i].size);
	pthread_mutex_lock(&u32_lock);
	const char *sp = NULL;
	for (unsigned i = 0; i < U32_BUFS && !sp; i++)
		if (u32_bufs[i].buf == (uint64_t)src)
			for (unsigned k = 0; k < U32_MAPS; k++)
				if (u32_maps[k].mem == u32_bufs[i].mem && u32_maps[k].ptr) {
					sp = u32_maps[k].ptr + u32_bufs[i].off - u32_maps[k].off;
					break;
				}
	for (uint32_t k = 0; k < n; k++)
		u32_note((uint64_t)dst, r[k].dstOffset,
		         sp && r[k].size % 4 == 0 && u32_shaped((const uint16_t *)(sp + r[k].srcOffset), r[k].size / 2));
	pthread_mutex_unlock(&u32_lock);
	device_of(cb)->CmdCopyBuffer(cb, src, dst, n, r);
}

static VKAPI_ATTR void VKAPI_CALL CmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkIndexType t)
{
	if (t == VK_INDEX_TYPE_UINT16) {
		static unsigned logs;
		pthread_mutex_lock(&u32_lock);
		for (int i = 0; i < U32_IBS; i++)
			if (u32_ibs[i].buf == (uint64_t)b && u32_ibs[i].off == off) {
				t = VK_INDEX_TYPE_UINT32;
				if (logs < 10) {
					logs++;
					LOG("index buffer %p+%llu holds 32-bit indices: bound as UINT32, not UINT16\n", (void *)b,
					    (unsigned long long)off);
				}
				break;
			}
		pthread_mutex_unlock(&u32_lock);
	}
	device_of(cb)->CmdBindIndexBuffer(cb, b, off, t);
}

static void u32_bind(VkBuffer b, VkDeviceMemory m, VkDeviceSize off)
{
	pthread_mutex_lock(&u32_lock);
	int at = -1;
	for (unsigned i = 0; i < U32_BUFS && at < 0; i++)
		if (u32_bufs[i].buf == (uint64_t)b)
			at = (int)i;
	if (at < 0)
		at = (int)(u32_buf_next++ % U32_BUFS);
	u32_bufs[at].buf = (uint64_t)b;
	u32_bufs[at].mem = (uint64_t)m;
	u32_bufs[at].off = off;
	pthread_mutex_unlock(&u32_lock);
}

static VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory(VkDevice device, VkBuffer b, VkDeviceMemory m, VkDeviceSize off)
{
	VkResult r = device_of(device)->BindBufferMemory(device, b, m, off);
	if (r == VK_SUCCESS)
		u32_bind(b, m, off);
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL BindBufferMemory2(VkDevice device, uint32_t n, const VkBindBufferMemoryInfo *bi)
{
	VkResult r = device_of(device)->BindBufferMemory2(device, n, bi);
	if (r == VK_SUCCESS)
		for (uint32_t i = 0; i < n; i++)
			u32_bind(bi[i].buffer, bi[i].memory, bi[i].memoryOffset);
	return r;
}

static void u32_map(VkDeviceMemory m, void *ptr, VkDeviceSize off)
{
	pthread_mutex_lock(&u32_lock);
	int at = -1, free_at = -1;
	for (int i = 0; i < U32_MAPS; i++) {
		if (u32_maps[i].mem == (uint64_t)m)
			at = i;
		else if (!u32_maps[i].mem && free_at < 0)
			free_at = i;
	}
	if (at < 0)
		at = free_at;
	if (at >= 0) {
		u32_maps[at].mem = ptr ? (uint64_t)m : 0;
		u32_maps[at].ptr = ptr;
		u32_maps[at].off = off;
	}
	pthread_mutex_unlock(&u32_lock);
}

static VKAPI_ATTR VkResult VKAPI_CALL MapMemory(VkDevice device, VkDeviceMemory m, VkDeviceSize off, VkDeviceSize size,
                                                VkMemoryMapFlags f, void **pp)
{
	VkResult r = device_of(device)->MapMemory(device, m, off, size, f, pp);
	if (r == VK_SUCCESS)
		u32_map(m, *pp, off);
	return r;
}

static VKAPI_ATTR void VKAPI_CALL UnmapMemory(VkDevice device, VkDeviceMemory m)
{
	u32_map(m, NULL, 0);
	device_of(device)->UnmapMemory(device, m);
}

static VKAPI_ATTR void VKAPI_CALL FreeMemory(VkDevice device, VkDeviceMemory m, const VkAllocationCallbacks *alloc)
{
	u32_map(m, NULL, 0);
	device_of(device)->FreeMemory(device, m, alloc);
}

/*
 * vrclient's Vulkan transfer ring (CVulkanVRRenderer, "Initialized Vulkan transfer ring buffer"): 256 MB of
 * host-visible memory in every OpenVR process that uses Vulkan (vrcompositor has its own copy of that
 * renderer). On KGSL that is pinned memory, allocated in full up front, and mostly never touched: 0 MB in the
 * dashboard's vrwebhelper and in overlay apps (q1desktop), ~50 MB in vrcompositor; only Steam's webhelper,
 * which uploads its CPU-drawn pages through it, fills it. A large allocation made from vrclient.so or
 * vrcompositor (the ring is their only one of 64 MB or more) asks the kernel to back it page by page at the
 * first CPU touch (kernel/kgsl-lazy.patch, IOCTL_KGSL_LAZY_NEXT): the ring is written by the CPU before the
 * GPU reads it. Without that kernel patch the ioctl fails and nothing changes.
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
	VkResult r = d->AllocateMemory(device, ai, alloc, out);
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
		VkResult r = d->CreateImage(device, ci, alloc, out);
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

// The code of every live module (as given to the driver), for the pipeline retry of CreateGraphicsPipelines.
#define MOD_CODES 2048
static struct { VkShaderModule module; uint32_t *code; uint32_t words; } mod_codes[MOD_CODES];
static unsigned mod_code_next;

static void mod_code_put(VkShaderModule m, const uint32_t *code, uint32_t words)
{
	uint32_t *c = malloc(words * sizeof(uint32_t));
	if (!c)
		return;
	memcpy(c, code, words * sizeof(uint32_t));
	pthread_mutex_lock(&g_lock);
	unsigned slot = MOD_CODES;
	for (unsigned i = 0; i < MOD_CODES && slot == MOD_CODES; i++)
		if (!mod_codes[i].module)
			slot = i;
	if (slot == MOD_CODES) // full: replace the oldest entries round robin
		slot = mod_code_next++ % MOD_CODES;
	free(mod_codes[slot].code);
	mod_codes[slot].module = m;
	mod_codes[slot].code = c;
	mod_codes[slot].words = words;
	pthread_mutex_unlock(&g_lock);
}

//! A malloc'ed copy of a module's code, or NULL.
static uint32_t *mod_code_get(VkShaderModule m, uint32_t *words)
{
	uint32_t *c = NULL;
	pthread_mutex_lock(&g_lock);
	for (unsigned i = 0; i < MOD_CODES; i++)
		if (mod_codes[i].module == m && m && (c = malloc(mod_codes[i].words * sizeof(uint32_t)))) {
			memcpy(c, mod_codes[i].code, mod_codes[i].words * sizeof(uint32_t));
			*words = mod_codes[i].words;
			break;
		}
	pthread_mutex_unlock(&g_lock);
	return c;
}

static VKAPI_ATTR void VKAPI_CALL DestroyShaderModule(VkDevice device, VkShaderModule m, const VkAllocationCallbacks *alloc)
{
	pthread_mutex_lock(&g_lock);
	for (unsigned i = 0; i < MOD_CODES; i++)
		if (mod_codes[i].module == m && m) {
			free(mod_codes[i].code);
			mod_codes[i].module = VK_NULL_HANDLE;
			mod_codes[i].code = NULL;
		}
	pthread_mutex_unlock(&g_lock);
	device_of(device)->DestroyShaderModule(device, m, alloc);
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
	const char *dbg = spirv_debug() ? getenv("QUEST1_COMPAT_SPIRV") : NULL;
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
	if (r == VK_SUCCESS && ci->codeSize >= 20 && ci->pCode[0] == 0x07230203)
		mod_code_put(*out, ci->pCode, (uint32_t)(ci->codeSize / 4));
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

// Descriptions of the descriptor set layouts, pipeline layouts and render passes created, by handle, so that a refused
// graphics pipeline can be logged with what it was built against (log_failed_pipeline).
#define OBJ_DESCS 1024
static struct { uint64_t handle; char text[480]; } obj_descs[OBJ_DESCS];
static unsigned obj_desc_next;

static void obj_desc_set(uint64_t handle, const char *text)
{
	pthread_mutex_lock(&g_lock);
	unsigned i = obj_desc_next++ % OBJ_DESCS;
	obj_descs[i].handle = handle;
	snprintf(obj_descs[i].text, sizeof(obj_descs[i].text), "%s", text);
	pthread_mutex_unlock(&g_lock);
}

static void obj_desc_copy(uint64_t handle, char *out, size_t n)
{
	pthread_mutex_lock(&g_lock);
	for (unsigned i = 0; i < OBJ_DESCS; i++)
		if (obj_descs[i].handle == handle && handle)
			snprintf(out, n, "%s", obj_descs[i].text);
	pthread_mutex_unlock(&g_lock);
}

static void obj_desc_log(const char *what, uint64_t handle)
{
	pthread_mutex_lock(&g_lock);
	for (unsigned i = 0; i < OBJ_DESCS; i++)
		if (obj_descs[i].handle == handle && handle)
			LOG("  %s %p: %s\n", what, (void *)handle, obj_descs[i].text);
	pthread_mutex_unlock(&g_lock);
}

// Which bindings each descriptor set layout has, for which stages, and the set layouts of each pipeline layout.
#define LAYOUTS 1024
static struct { uint64_t handle; uint32_t n; uint32_t binding[64], stages[64]; } set_layouts[LAYOUTS];
static struct { uint64_t handle; uint32_t n; uint64_t sets[8]; } pipe_layouts[LAYOUTS];
static unsigned set_layout_next, pipe_layout_next;

//! Whether the pipeline layout gives (set, binding) to the stage; true when the layouts are unknown.
static bool layout_has(uint64_t pl, uint32_t set, uint32_t binding, uint32_t stage)
{
	bool known = false, has = false;
	pthread_mutex_lock(&g_lock);
	for (unsigned i = 0; i < LAYOUTS; i++) {
		if (pipe_layouts[i].handle != pl || !pl)
			continue;
		if (set >= pipe_layouts[i].n) {
			known = true;
			break;
		}
		for (unsigned j = 0; j < LAYOUTS; j++)
			if (set_layouts[j].handle == pipe_layouts[i].sets[set] && set_layouts[j].handle) {
				known = true;
				for (uint32_t k = 0; k < set_layouts[j].n; k++)
					if (set_layouts[j].binding[k] == binding)
						has = (set_layouts[j].stages[k] & stage) != 0;
				break;
			}
		break;
	}
	pthread_mutex_unlock(&g_lock);
	return !known || has;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo *ci,
                                                                const VkAllocationCallbacks *alloc,
                                                                VkDescriptorSetLayout *out)
{
	VkResult r = device_of(device)->CreateDescriptorSetLayout(device, ci, alloc, out);
	if (r == VK_SUCCESS) {
		char buf[480];
		size_t len = (size_t)snprintf(buf, sizeof(buf), "flags 0x%x pNext %s:", ci->flags, ci->pNext ? "set" : "none");
		for (uint32_t i = 0; i < ci->bindingCount && len < sizeof(buf) - 40; i++) {
			const VkDescriptorSetLayoutBinding *b = &ci->pBindings[i];
			len += (size_t)snprintf(buf + len, sizeof(buf) - len, " %u:t%d x%u s0x%x%s", b->binding, b->descriptorType,
			                        b->descriptorCount, b->stageFlags, b->pImmutableSamplers ? "i" : "");
		}
		obj_desc_set((uint64_t)*out, buf);
		rm_note_layout(*out, ci);
		if (ci->bindingCount <= 64) {
			pthread_mutex_lock(&g_lock);
			unsigned s = set_layout_next++ % LAYOUTS;
			set_layouts[s].handle = (uint64_t)*out;
			set_layouts[s].n = ci->bindingCount;
			for (uint32_t i = 0; i < ci->bindingCount; i++) {
				set_layouts[s].binding[i] = ci->pBindings[i].binding;
				set_layouts[s].stages[i] = ci->pBindings[i].descriptorCount ? ci->pBindings[i].stageFlags : 0;
			}
			pthread_mutex_unlock(&g_lock);
		}
	}
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo *ci,
                                                           const VkAllocationCallbacks *alloc, VkPipelineLayout *out)
{
	VkResult r = device_of(device)->CreatePipelineLayout(device, ci, alloc, out);
	if (r == VK_SUCCESS) {
		char buf[480];
		size_t len = (size_t)snprintf(buf, sizeof(buf), "%u push ranges, sets:", ci->pushConstantRangeCount);
		for (uint32_t i = 0; i < ci->setLayoutCount && len < sizeof(buf) - 24; i++)
			len += (size_t)snprintf(buf + len, sizeof(buf) - len, " %p", (void *)ci->pSetLayouts[i]);
		obj_desc_set((uint64_t)*out, buf);
		if (ci->setLayoutCount <= 8) {
			pthread_mutex_lock(&g_lock);
			unsigned s = pipe_layout_next++ % LAYOUTS;
			pipe_layouts[s].handle = (uint64_t)*out;
			pipe_layouts[s].n = ci->setLayoutCount;
			for (uint32_t i = 0; i < ci->setLayoutCount; i++)
				pipe_layouts[s].sets[i] = (uint64_t)ci->pSetLayouts[i];
			pthread_mutex_unlock(&g_lock);
		}
	}
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass(VkDevice device, const VkRenderPassCreateInfo *ci,
                                                       const VkAllocationCallbacks *alloc, VkRenderPass *out)
{
	// scene mode: the 4x MSAA system layer made 1-sample (qs_scene_single_sample, quest1_scanout.c)
	VkRenderPassCreateInfo single;
	VkAttachmentDescription atts[8];
	bool msaa = false;
	for (uint32_t i = 0; i < ci->attachmentCount; i++)
		msaa |= qs_scene_single_sample(ci->pAttachments[i].samples);
	if (msaa && ci->attachmentCount <= 8) {
		single = *ci;
		memcpy(atts, ci->pAttachments, ci->attachmentCount * sizeof(atts[0]));
		for (uint32_t i = 0; i < ci->attachmentCount; i++)
			atts[i].samples = VK_SAMPLE_COUNT_1_BIT;
		single.pAttachments = atts;
		ci = &single;
	}
	VkResult r = device_of(device)->CreateRenderPass(device, ci, alloc, out);
	if (r == VK_SUCCESS)
		rm_note_rp(device_of(device), *out, ci);
	if (r == VK_SUCCESS) {
		char buf[480];
		size_t len = 0;
		len += (size_t)snprintf(buf + len, sizeof(buf) - len, "pNext %s",
		                        ci->pNext ? "set" : "none");
		for (const VkBaseInStructure *p = ci->pNext; p && len < sizeof(buf) - 24; p = p->pNext)
			len += (size_t)snprintf(buf + len, sizeof(buf) - len, " (sType %d)", p->sType);
		for (uint32_t i = 0; i < ci->attachmentCount && len < sizeof(buf) - 40; i++)
			len += (size_t)snprintf(buf + len, sizeof(buf) - len, ", att%u fmt %d x%d ld %d st %d sld %d sst %d lay %d>%d", i,
			                        ci->pAttachments[i].format, ci->pAttachments[i].samples, ci->pAttachments[i].loadOp,
			                        ci->pAttachments[i].storeOp, ci->pAttachments[i].stencilLoadOp,
			                        ci->pAttachments[i].stencilStoreOp, ci->pAttachments[i].initialLayout,
			                        ci->pAttachments[i].finalLayout);
		for (uint32_t i = 0; i < ci->subpassCount && len < sizeof(buf) - 60; i++) {
			const VkSubpassDescription *sp = &ci->pSubpasses[i];
			len += (size_t)snprintf(buf + len, sizeof(buf) - len, ", sub%u in %u col %u res %s ds %s", i,
			                        sp->inputAttachmentCount, sp->colorAttachmentCount,
			                        sp->pResolveAttachments ? "y" : "n", sp->pDepthStencilAttachment ? "y" : "n");
		}
		obj_desc_set((uint64_t)*out, buf);
	}
	return r;
}

//! What a refused graphics pipeline asked for (logged on every failure: they are rare and vrcompositor then binds the
//! null pipeline and crashes, e.g. its camera roomview on 2026-11-03).
static void log_failed_pipeline(const VkGraphicsPipelineCreateInfo *c)
{
	LOG("failed graphics pipeline: flags 0x%x pNext %s, %u stages, layout %p, render pass %p subpass %u\n", c->flags,
	    c->pNext ? "set" : "none", c->stageCount, (void *)c->layout, (void *)c->renderPass, c->subpass);
	obj_desc_log("pipeline layout", (uint64_t)c->layout);
	pthread_mutex_lock(&g_lock);
	char sets[480] = "";
	for (unsigned i = 0; i < OBJ_DESCS; i++)
		if (obj_descs[i].handle == (uint64_t)c->layout && c->layout)
			snprintf(sets, sizeof(sets), "%s", obj_descs[i].text);
	pthread_mutex_unlock(&g_lock);
	for (char *p = strstr(sets, "0x"); p; p = strstr(p + 2, "0x"))
		obj_desc_log("set layout", strtoull(p, NULL, 16));
	obj_desc_log("render pass", (uint64_t)c->renderPass);
	for (uint32_t s = 0; s < c->stageCount; s++) {
		const VkPipelineShaderStageCreateInfo *st = &c->pStages[s];
		LOG("  stage 0x%x '%s' flags 0x%x pNext %s, specialization %u constants\n", st->stage, st->pName, st->flags,
		    st->pNext ? "set" : "none", st->pSpecializationInfo ? st->pSpecializationInfo->mapEntryCount : 0);
	}
	const VkPipelineVertexInputStateCreateInfo *vi = c->pVertexInputState;
	if (vi) {
		LOG("  vertex input: %u bindings, %u attributes\n", vi->vertexBindingDescriptionCount,
		    vi->vertexAttributeDescriptionCount);
		for (uint32_t i = 0; i < vi->vertexBindingDescriptionCount; i++)
			LOG("    binding %u stride %u rate %d\n", vi->pVertexBindingDescriptions[i].binding,
			    vi->pVertexBindingDescriptions[i].stride, vi->pVertexBindingDescriptions[i].inputRate);
		for (uint32_t i = 0; i < vi->vertexAttributeDescriptionCount; i++)
			LOG("    attribute loc %u binding %u format %d offset %u\n", vi->pVertexAttributeDescriptions[i].location,
			    vi->pVertexAttributeDescriptions[i].binding, vi->pVertexAttributeDescriptions[i].format,
			    vi->pVertexAttributeDescriptions[i].offset);
	}
	if (c->pInputAssemblyState)
		LOG("  topology %d restart %u\n", c->pInputAssemblyState->topology, c->pInputAssemblyState->primitiveRestartEnable);
	if (c->pTessellationState)
		LOG("  tessellation state set\n");
	if (c->pViewportState)
		LOG("  viewports %u scissors %u pNext %s\n", c->pViewportState->viewportCount, c->pViewportState->scissorCount,
		    c->pViewportState->pNext ? "set" : "none");
	const VkPipelineRasterizationStateCreateInfo *rs = c->pRasterizationState;
	if (rs)
		LOG("  raster: clamp %u discard %u polygon %d cull %d front %d bias %u width %.1f pNext %s\n", rs->depthClampEnable,
		    rs->rasterizerDiscardEnable, rs->polygonMode, rs->cullMode, rs->frontFace, rs->depthBiasEnable, rs->lineWidth,
		    rs->pNext ? "set" : "none");
	const VkPipelineMultisampleStateCreateInfo *ms = c->pMultisampleState;
	if (ms)
		LOG("  multisample: %d samples, sample shading %u (%.2f), mask %s, alpha to coverage %u, to one %u\n",
		    ms->rasterizationSamples, ms->sampleShadingEnable, ms->minSampleShading, ms->pSampleMask ? "set" : "none",
		    ms->alphaToCoverageEnable, ms->alphaToOneEnable);
	const VkPipelineDepthStencilStateCreateInfo *dss = c->pDepthStencilState;
	if (dss)
		LOG("  depth test %u write %u op %d bounds %u stencil %u (front fail %d pass %d depthfail %d op %d, back op %d)\n",
		    dss->depthTestEnable, dss->depthWriteEnable, dss->depthCompareOp, dss->depthBoundsTestEnable,
		    dss->stencilTestEnable, dss->front.failOp, dss->front.passOp, dss->front.depthFailOp, dss->front.compareOp,
		    dss->back.compareOp);
	const VkPipelineColorBlendStateCreateInfo *cb = c->pColorBlendState;
	if (cb) {
		LOG("  colour blend: logic op %u, %u attachments\n", cb->logicOpEnable, cb->attachmentCount);
		for (uint32_t i = 0; i < cb->attachmentCount; i++)
			LOG("    blend %u src %d/%d dst %d/%d op %d/%d mask 0x%x\n", cb->pAttachments[i].blendEnable,
			    cb->pAttachments[i].srcColorBlendFactor, cb->pAttachments[i].srcAlphaBlendFactor,
			    cb->pAttachments[i].dstColorBlendFactor, cb->pAttachments[i].dstAlphaBlendFactor,
			    cb->pAttachments[i].colorBlendOp, cb->pAttachments[i].alphaBlendOp, cb->pAttachments[i].colorWriteMask);
	}
	const VkPipelineDynamicStateCreateInfo *dyn = c->pDynamicState;
	if (dyn) {
		char buf[256] = "";
		size_t len = 0;
		for (uint32_t i = 0; i < dyn->dynamicStateCount && len < sizeof(buf) - 16; i++)
			len += snprintf(buf + len, sizeof(buf) - len, " %d", dyn->pDynamicStates[i]);
		LOG("  dynamic states:%s\n", buf);
	}
}

// vrcompositor's camera roomview vertex shader (2026-11-03) samples images and samplers its pipeline layout does not give
// to the vertex stage (bindings 11/27 not in the set layout, sampler 26 fragment-only), in branches its uniforms never
// take here. The Steam Frame's driver accepts that; the Adreno 540 blob refuses the pipeline (-13) and vrcompositor then
// binds a null pipeline and crashes. So, when a pipeline is refused, its modules are rewritten for the layout: every
// image or sampler variable the layout lacks for that stage is removed, and what sampled or read it gives 0
// (OpConstantNull). Same for buffers: its headset view's vertex shader (2026-11-09, asked for by Remote Play's
// spectator) loads a uniform of a buffer (binding 0) its layout has not got, into a variable it never reads; the
// access chains into such a buffer are removed and their loads give 0. Returns a malloc'ed module or NULL (nothing to
// do, or a use this does not handle).
static uint32_t *null_missing_descriptors(const uint32_t *w, uint32_t words, uint64_t layout, uint32_t stage,
                                          uint32_t *out_words, unsigned *nnull)
{
	enum { NAME = 5, ENTRY = 15, DECORATE = 71, VARIABLE = 59, LOAD = 61, SAMPLED_IMAGE = 86, IMAGE = 100, COPY = 83 };
	enum { CONST_NULL = 46, FUNCTION = 54, FUNCTION_CALL = 57, DEC_BINDING = 33, DEC_SET = 34, UNIFORM_CONSTANT = 0 };
	enum { ACCESS_CHAIN = 65, IN_BOUNDS_ACCESS_CHAIN = 66, UNIFORM = 2, STORAGE_BUFFER = 12 };
	// MISSING: an image or sampler variable, DERIVED: loaded or combined from one, NULLED: read through one (now 0),
	// MISSING_BUF: a buffer variable, CHAIN: a pointer into one
	enum { MISSING = 1, DERIVED = 2, NULLED = 3, MISSING_BUF = 4, CHAIN = 5 };
	uint32_t bound = w[3];
	if (bound == 0 || bound > (1u << 20))
		return NULL;
	uint32_t *set = malloc(bound * sizeof(uint32_t)), *bind = malloc(bound * sizeof(uint32_t));
	uint8_t *mark = calloc(bound, 1);
	uint32_t types[16], null_id[16], ntypes = 0, nmissing = 0, *o = NULL;
	bool ok = set && bind && mark;
	if (ok) {
		memset(set, 0xff, bound * sizeof(uint32_t));
		memset(bind, 0xff, bound * sizeof(uint32_t));
	}
	*nnull = 0;
	// pass 1: descriptor variables the layout lacks for this stage; what is loaded or sampled from them
	for (uint32_t i = 5; ok && i < words;) {
		uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
		if (wc == 0 || i + wc > words) {
			ok = false;
			break;
		}
		const uint32_t *in = &w[i];
		i += wc;
		if (op == DECORATE && wc >= 4 && in[1] < bound && (in[2] == DEC_SET || in[2] == DEC_BINDING)) {
			(in[2] == DEC_SET ? set : bind)[in[1]] = in[3];
		} else if (op == VARIABLE && wc >= 4 && in[2] < bound && set[in[2]] != UINT32_MAX &&
		           bind[in[2]] != UINT32_MAX && !layout_has(layout, set[in[2]], bind[in[2]], stage)) {
			if (in[3] == UNIFORM_CONSTANT)
				mark[in[2]] = MISSING;
			else if (in[3] == UNIFORM || in[3] == STORAGE_BUFFER)
				mark[in[2]] = MISSING_BUF;
			else { // push constants, ...: not handled
				ok = false;
				break;
			}
			nmissing++;
		} else if (nmissing && (op == ACCESS_CHAIN || op == IN_BOUNDS_ACCESS_CHAIN) && wc >= 4 && in[2] < bound &&
		           in[3] < bound && (mark[in[3]] == MISSING_BUF || mark[in[3]] == CHAIN)) {
			mark[in[2]] = CHAIN;
		} else if (nmissing && op == LOAD && wc >= 4 && in[2] < bound && in[3] < bound &&
		           (mark[in[3]] == MISSING_BUF || mark[in[3]] == CHAIN)) {
			mark[in[2]] = NULLED;
			uint32_t t = 0;
			while (t < ntypes && types[t] != in[1])
				t++;
			if (t == ntypes) {
				if (ntypes == 16) {
					ok = false;
					break;
				}
				types[ntypes++] = in[1];
			}
			(*nnull)++;
		} else if (nmissing && (op == LOAD || op == IMAGE) && wc >= 4 && in[2] < bound && in[3] < bound && mark[in[3]]) {
			mark[in[2]] = DERIVED;
		} else if (nmissing && op == SAMPLED_IMAGE && wc >= 5 && in[2] < bound && in[3] < bound && in[4] < bound &&
		           (mark[in[3]] || mark[in[4]])) {
			mark[in[2]] = DERIVED;
		} else if (nmissing && ((op >= 87 && op <= 99) || (op >= 103 && op <= 107) || (op >= 305 && op <= 315)) &&
		           wc >= 4 && in[2] < bound && in[3] < bound && mark[in[3]]) { // sample, fetch, gather, read, query
			mark[in[2]] = NULLED;
			uint32_t t = 0;
			while (t < ntypes && types[t] != in[1])
				t++;
			if (t == ntypes) {
				if (ntypes == 16) {
					ok = false;
					break;
				}
				types[ntypes++] = in[1];
			}
			(*nnull)++;
		} else if (nmissing && op == FUNCTION_CALL) {
			for (uint32_t k = 4; k < wc; k++)
				if (in[k] < bound && mark[in[k]] && mark[in[k]] != NULLED)
					ok = false; // an image passed to a function: not handled
		} else if (nmissing && op != NAME && op != DECORATE && op != ENTRY) {
			for (uint32_t k = 1; k < wc; k++) // a buffer pointer used otherwise (store, copy, array length): not handled
				if (in[k] < bound && (mark[in[k]] == MISSING_BUF || mark[in[k]] == CHAIN))
					ok = false;
		}
	}
	if (ok && nmissing) {
		o = malloc((words + 3 * ntypes) * sizeof(uint32_t));
		ok = o != NULL;
	}
	if (ok && nmissing) {
		memcpy(o, w, 5 * sizeof(uint32_t));
		for (uint32_t t = 0; t < ntypes; t++)
			null_id[t] = bound + t;
		o[3] = bound + ntypes;
		uint32_t n = 5;
		bool nulls_out = false;
		for (uint32_t i = 5; i < words;) {
			uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
			const uint32_t *in = &w[i];
			i += wc;
			if ((op == NAME || op == DECORATE) && wc >= 2 && in[1] < bound &&
			    (mark[in[1]] == MISSING || mark[in[1]] == MISSING_BUF))
				continue;
			if ((op == VARIABLE || op == LOAD || op == IMAGE || op == SAMPLED_IMAGE || op == ACCESS_CHAIN ||
			     op == IN_BOUNDS_ACCESS_CHAIN) && wc >= 3 && in[2] < bound &&
			    (mark[in[2]] == MISSING || mark[in[2]] == DERIVED || mark[in[2]] == MISSING_BUF || mark[in[2]] == CHAIN))
				continue;
			if (op == FUNCTION && !nulls_out) {
				for (uint32_t t = 0; t < ntypes; t++) {
					o[n++] = (3u << 16) | CONST_NULL;
					o[n++] = types[t];
					o[n++] = null_id[t];
				}
				nulls_out = true;
			}
			if (op == ENTRY) { // [model, id, name..., interface ids]: drop the removed variables
				uint32_t s = 3;
				while (s < wc && memchr(&in[s], 0, 4) == NULL)
					s++;
				s = s + 1 < wc ? s + 1 : wc;
				uint32_t start = n;
				memcpy(&o[n], in, s * sizeof(uint32_t));
				n += s;
				for (uint32_t k = s; k < wc; k++)
					if (in[k] >= bound || (mark[in[k]] != MISSING && mark[in[k]] != MISSING_BUF))
						o[n++] = in[k];
				o[start] = ((n - start) << 16) | ENTRY;
				continue;
			}
			// the instruction that gave the nulled value (in[2] is its result; an OpStore of it has it there too)
			bool producer = op == LOAD || (op >= 87 && op <= 99) || (op >= 103 && op <= 107) || (op >= 305 && op <= 315);
			if (producer && wc >= 3 && in[2] < bound && mark[in[2]] == NULLED) {
				uint32_t t = 0;
				while (types[t] != in[1])
					t++;
				o[n++] = (4u << 16) | COPY;
				o[n++] = in[1];
				o[n++] = in[2];
				o[n++] = null_id[t];
				continue;
			}
			memcpy(&o[n], in, wc * sizeof(uint32_t));
			n += wc;
		}
		*out_words = n;
	} else {
		free(o);
		o = NULL;
	}
	free(set);
	free(bind);
	free(mark);
	return o;
}

//! A pipeline still refused after retry_pipeline: its modules for spirv-dis, /tmp/quest1-failed-pipeline-<stage>.spv
static void dump_failed_modules(const VkGraphicsPipelineCreateInfo *c)
{
	for (uint32_t s = 0; s < c->stageCount; s++) {
		uint32_t words = 0;
		uint32_t *code = mod_code_get(c->pStages[s].module, &words);
		char path[64];
		snprintf(path, sizeof(path), "/tmp/quest1-failed-pipeline-%x.spv", c->pStages[s].stage);
		FILE *f = code ? fopen(path, "wb") : NULL;
		if (f) {
			fwrite(code, 4, words, f);
			fclose(f);
			LOG("  module of stage 0x%x written to %s\n", c->pStages[s].stage, path);
		}
		free(code);
	}
}

//! Retry of a refused graphics pipeline with its modules rewritten for its layout (null_missing_descriptors).
static VkResult retry_pipeline(VkDevice device, VkPipelineCache cache, const VkGraphicsPipelineCreateInfo *c,
                               const VkAllocationCallbacks *alloc, VkPipeline *out)
{
	struct device *d = device_of(device);
	VkPipelineShaderStageCreateInfo st[8];
	VkShaderModule tmp[8] = {0};
	unsigned changed = 0, nulled = 0;
	if (c->stageCount > 8)
		return VK_ERROR_INITIALIZATION_FAILED;
	for (uint32_t s = 0; s < c->stageCount; s++) {
		st[s] = c->pStages[s];
		uint32_t words = 0, out_words = 0;
		unsigned nn = 0;
		uint32_t *code = mod_code_get(st[s].module, &words);
		uint32_t *fixed =
		    code ? null_missing_descriptors(code, words, (uint64_t)c->layout, st[s].stage, &out_words, &nn) : NULL;
		free(code);
		if (!fixed)
			continue;
		VkShaderModuleCreateInfo smci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		                                 .codeSize = out_words * 4, .pCode = fixed};
		VkResult r = d->CreateShaderModule(device, &smci, alloc, &tmp[s]);
		free(fixed);
		if (r == VK_SUCCESS) {
			st[s].module = tmp[s];
			changed++;
			nulled += nn;
		}
	}
	VkResult r = VK_ERROR_INITIALIZATION_FAILED;
	if (changed) {
		VkGraphicsPipelineCreateInfo c2 = *c;
		c2.pStages = st;
		r = d->CreateGraphicsPipelines(device, cache, 1, &c2, alloc, out);
		LOG("graphics pipeline retried with %u descriptor reads the layout lacks turned into 0 (%u stages): %d, "
		    "pipeline %p\n", nulled, changed, r, r == VK_SUCCESS ? (void *)*out : NULL);
	}
	for (uint32_t s = 0; s < c->stageCount; s++)
		if (tmp[s])
			d->DestroyShaderModule(device, tmp[s], alloc);
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t n,
                                                              const VkGraphicsPipelineCreateInfo *ci,
                                                              const VkAllocationCallbacks *alloc, VkPipeline *out)
{
	if (spirv_debug())
		for (uint32_t i = 0; i < n; i++)
			log_stages("graphics pipeline", ci[i].pStages, ci[i].stageCount);
	// scene mode: the 4x MSAA system layer made 1-sample (qs_scene_single_sample, quest1_scanout.c)
	VkGraphicsPipelineCreateInfo single[16];
	VkPipelineMultisampleStateCreateInfo ms[16];
	bool msaa = false;
	for (uint32_t i = 0; i < n; i++)
		msaa |= ci[i].pMultisampleState && qs_scene_single_sample(ci[i].pMultisampleState->rasterizationSamples);
	if (msaa && n <= 16) {
		for (uint32_t i = 0; i < n; i++) {
			single[i] = ci[i];
			if (ci[i].pMultisampleState && ci[i].pMultisampleState->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT) {
				ms[i] = *ci[i].pMultisampleState;
				ms[i].rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
				ms[i].sampleShadingEnable = VK_FALSE;
				single[i].pMultisampleState = &ms[i];
			}
		}
		ci = single;
	}
	VkResult r = device_of(device)->CreateGraphicsPipelines(device, cache, n, ci, alloc, out);
	if (spirv_debug())
		LOG("  -> %d, pipeline %p\n", r, n ? (void *)out[0] : NULL);
	if (r != VK_SUCCESS) {
		bool all = true;
		for (uint32_t i = 0; i < n; i++)
			if (!out[i]) {
				LOG("graphics pipeline %u of %u refused: %d\n", i, n, r);
				log_failed_pipeline(&ci[i]);
				if (retry_pipeline(device, cache, &ci[i], alloc, &out[i]) != VK_SUCCESS) {
					dump_failed_modules(&ci[i]);
					all = false;
				}
			}
		if (all)
			r = VK_SUCCESS;
	}
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t n,
                                                             const VkComputePipelineCreateInfo *ci,
                                                             const VkAllocationCallbacks *alloc, VkPipeline *out)
{
	if (spirv_debug())
		for (uint32_t i = 0; i < n; i++)
			log_stages("compute pipeline", &ci[i].stage, 1);
	VkResult r = device_of(device)->CreateComputePipelines(device, cache, n, ci, alloc, out);
	if (spirv_debug())
		LOG("  -> %d\n", r);
	return r;
}

// --- buffers and descriptor sets, followed for the render pass merge (quest1_rpmerge.c) -------------------

static VKAPI_ATTR VkResult VKAPI_CALL CreateBuffer(VkDevice device, const VkBufferCreateInfo *ci,
                                                   const VkAllocationCallbacks *alloc, VkBuffer *out)
{
	VkResult r = device_of(device)->CreateBuffer(device, ci, alloc, out);
	if (r == VK_SUCCESS)
		rm_note_buffer(*out, ci);
	return r;
}

static VKAPI_ATTR void VKAPI_CALL DestroyBuffer(VkDevice device, VkBuffer b, const VkAllocationCallbacks *alloc)
{
	rm_forget_buffer(b);
	device_of(device)->DestroyBuffer(device, b, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL AllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo *ai,
                                                             VkDescriptorSet *out)
{
	static PFN_vkAllocateDescriptorSets next;
	if (!next)
		next = (PFN_vkAllocateDescriptorSets)device_of(device)->gdpa(device, "vkAllocateDescriptorSets");
	VkResult r = next(device, ai, out);
	if (r == VK_SUCCESS)
		rm_note_sets(ai, out);
	return r;
}

static VKAPI_ATTR void VKAPI_CALL UpdateDescriptorSets(VkDevice device, uint32_t nw, const VkWriteDescriptorSet *w,
                                                      uint32_t nc, const VkCopyDescriptorSet *c)
{
	static PFN_vkUpdateDescriptorSets next;
	if (!next)
		next = (PFN_vkUpdateDescriptorSets)device_of(device)->gdpa(device, "vkUpdateDescriptorSets");
	rm_note_writes(nw, w, nc, c);
	next(device, nw, w, nc, c);
}

static VKAPI_ATTR void VKAPI_CALL UpdateDescriptorSetWithTemplate(VkDevice device, VkDescriptorSet s,
                                                                 VkDescriptorUpdateTemplate t, const void *data)
{
	static PFN_vkUpdateDescriptorSetWithTemplate next;
	if (!next)
		next = (PFN_vkUpdateDescriptorSetWithTemplate)device_of(device)->gdpa(device, "vkUpdateDescriptorSetWithTemplate");
	rm_note_template_write(s);
	next(device, s, t, data);
}

static VKAPI_ATTR void VKAPI_CALL UpdateDescriptorSetWithTemplateKHR(VkDevice device, VkDescriptorSet s,
                                                                    VkDescriptorUpdateTemplate t, const void *data)
{
	static PFN_vkUpdateDescriptorSetWithTemplate next;
	if (!next)
		next = (PFN_vkUpdateDescriptorSetWithTemplate)device_of(device)->gdpa(device,
		                                                                    "vkUpdateDescriptorSetWithTemplateKHR");
	rm_note_template_write(s);
	next(device, s, t, data);
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
	const char **exts = calloc(ci->enabledExtensionCount + 3, sizeof(char *));
	uint32_t ne = 0;
	bool have_ahb = false, have_foreign = false;
	for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
		const char *e = ci->ppEnabledExtensionNames[i];
		have_ahb |= strcmp(e, VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME) == 0;
		have_foreign |= strcmp(e, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME) == 0;
		if (is_fake(e))
			LOG("emulating %s\n", e);
		else
			exts[ne++] = e;
	}
	// direct scanout imports gralloc buffers (quest1_scanout.c)
	if (qs_enabled() && !have_ahb)
		exts[ne++] = VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME;
	if (qs_enabled() && !have_foreign)
		exts[ne++] = VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME;
	VkDeviceCreateInfo copy = *ci;
	copy.enabledExtensionCount = ne;
	copy.ppEnabledExtensionNames = exts;

	// vrcompositor asks for a HIGH/REALTIME global-priority queue, which the blob refuses to non-root
	// processes (VK_ERROR_NOT_PERMITTED): drop the priority request, keep default priority queues
	VkDeviceQueueCreateInfo queues[8];
	bool keep = getenv("QUEST1_COMPAT_KEEP_PRIORITY") != NULL;
	if (ci->queueCreateInfoCount <= 8) {
		for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++)
			queues[i] = ci->pQueueCreateInfos[i];
		copy.pQueueCreateInfos = queues;
	}
	if (!keep && ci->queueCreateInfoCount <= 8) {
		for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++) {
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
	}

	// Steam's CEF processes draw their VR pages through vrclient's Vulkan renderer in GPU batches of up to ~40 ms.
	// At the default priority (kgsl MED) they share vrcompositor's ring buffer, and vrcompositor waits behind
	// them and misses its frame. With LOW they go on the ring below, which vrcompositor's ring preempts
	// (Monado, root, is on the ring above both).
	VkDeviceQueueGlobalPriorityCreateInfoKHR low[8];
	const char *prog = program_invocation_short_name;
	if (!keep && ci->queueCreateInfoCount <= 8 && (!strcmp(prog, "steamwebhelper") || !strcmp(prog, "vrwebhelper"))) {
		PFN_vkEnumerateDeviceExtensionProperties enum_exts =
		    (PFN_vkEnumerateDeviceExtensionProperties)gipa(in->handle, "vkEnumerateDeviceExtensionProperties");
		uint32_t n = 0;
		VkExtensionProperties *props = NULL;
		bool have = false, enabled = false;
		if (enum_exts && enum_exts(pd, NULL, &n, NULL) == VK_SUCCESS && (props = calloc(n, sizeof(*props))) &&
		    enum_exts(pd, NULL, &n, props) >= 0)
			for (uint32_t i = 0; i < n; i++)
				have |= !strcmp(props[i].extensionName, VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME);
		free(props);
		for (uint32_t i = 0; i < ne; i++)
			enabled |= !strcmp(exts[i], VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME);
		if (have) {
			if (!enabled)
				exts[ne++] = VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME;
			copy.enabledExtensionCount = ne;
			for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++) {
				bool set = false;
				for (const VkBaseInStructure *p = queues[i].pNext; p; p = p->pNext)
					set |= p->sType == VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR;
				if (set)
					continue;
				low[i] = (VkDeviceQueueGlobalPriorityCreateInfoKHR){
				    VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR, queues[i].pNext,
				    VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR};
				queues[i].pNext = &low[i];
			}
			LOG("GPU priority LOW (below vrcompositor)\n");
		}
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
	if (ci->queueCreateInfoCount) {
		d->has_q0 = true;
		d->q0_family = ci->pQueueCreateInfos[0].queueFamilyIndex;
	}
#define GET(fn) d->fn = (PFN_vk##fn)gdpa(*out, "vk" #fn)
	GET(DestroyDevice);
	GET(CreateSemaphore);
	GET(DestroySemaphore);
	GET(QueueSubmit);
	GET(QueueWaitIdle);
	GET(CreateShaderModule);
	GET(DestroyShaderModule);
	GET(CreateImage);
	GET(AllocateMemory);
	GET(GetImageSubresourceLayout);
	GET(CreateGraphicsPipelines);
	GET(CreateDescriptorSetLayout);
	GET(CreatePipelineLayout);
	GET(CreateRenderPass);
	GET(CreateComputePipelines);
	GET(DeviceWaitIdle);
	GET(GetQueryPoolResults);
	GET(CreateFence);
	GET(DestroyFence);
	GET(WaitForFences);
	GET(ResetFences);
	GET(GetSemaphoreFdKHR);
	GET(ImportSemaphoreFdKHR);
	GET(CmdUpdateBuffer);
	GET(CmdBindIndexBuffer);
	GET(CmdCopyBuffer);
	GET(BindBufferMemory);
	GET(BindBufferMemory2);
	if (!d->BindBufferMemory2)
		d->BindBufferMemory2 = (PFN_vkBindBufferMemory2)gdpa(*out, "vkBindBufferMemory2KHR");
	GET(MapMemory);
	GET(UnmapMemory);
	GET(FreeMemory);
	GET(CreateBuffer);
	GET(DestroyBuffer);
	GET(GetBufferMemoryRequirements);
	GET(AllocateCommandBuffers);
	GET(FreeCommandBuffers);
	GET(DestroyCommandPool);
	GET(BeginCommandBuffer);
#undef GET
	((PFN_vkGetPhysicalDeviceMemoryProperties)gipa(in->handle, "vkGetPhysicalDeviceMemoryProperties"))(pd, &d->memprops);
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
	fx_destroy(d);
	ub_drop(d, VK_NULL_HANDLE, NULL);
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
	if (fx_level() >= 2 && (strstr(name, "Render") || strstr(name, "BeginQuery")))
		LOG("GetDeviceProcAddr %s\n", name);
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
	PFN_vkVoidFunction fxf = fx_proc(name);
	if (fxf)
		return fxf;
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
	I("vkDestroyShaderModule", DestroyShaderModule)
	I("vkCreateImage", CreateImage)
	I("vkAllocateMemory", AllocateMemory)
	I("vkWaitForFences", WaitForFences)
	I("vkCreateGraphicsPipelines", CreateGraphicsPipelines)
	I("vkCreateDescriptorSetLayout", CreateDescriptorSetLayout)
	I("vkCreatePipelineLayout", CreatePipelineLayout)
	I("vkCreateRenderPass", CreateRenderPass)
	I("vkCreateBuffer", CreateBuffer)
	I("vkDestroyBuffer", DestroyBuffer)
	I("vkAllocateDescriptorSets", AllocateDescriptorSets)
	I("vkUpdateDescriptorSets", UpdateDescriptorSets)
	I("vkUpdateDescriptorSetWithTemplate", UpdateDescriptorSetWithTemplate)
	I("vkUpdateDescriptorSetWithTemplateKHR", UpdateDescriptorSetWithTemplateKHR)
	I("vkCreateComputePipelines", CreateComputePipelines)
	I("vkDeviceWaitIdle", DeviceWaitIdle)
	I("vkGetQueryPoolResults", GetQueryPoolResults)
	I("vkCmdUpdateBuffer", CmdUpdateBuffer)
	I("vkCmdBindIndexBuffer", CmdBindIndexBuffer)
	I("vkCmdCopyBuffer", CmdCopyBuffer)
	I("vkBindBufferMemory", BindBufferMemory)
	I("vkBindBufferMemory2", BindBufferMemory2)
	I("vkBindBufferMemory2KHR", BindBufferMemory2)
	I("vkMapMemory", MapMemory)
	I("vkUnmapMemory", UnmapMemory)
	I("vkFreeMemory", FreeMemory)
	I("vkAllocateCommandBuffers", AllocateCommandBuffers)
	I("vkFreeCommandBuffers", FreeCommandBuffers)
	I("vkDestroyCommandPool", DestroyCommandPool)
	I("vkBeginCommandBuffer", BeginCommandBuffer)
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
	in->GetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)gipa(*out, "vkGetPhysicalDeviceProperties");
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
	PFN_vkVoidFunction fxi = fx_iproc(name);
	if (fxi)
		return fxi;
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
	I("vkGetPhysicalDeviceProperties", GetPhysicalDeviceProperties)
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
