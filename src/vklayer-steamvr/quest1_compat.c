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
static bool qdb_front(void);  // quest1_dmabuf.c

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
		if (qdb_front()) { // Steam Link VR's dma-buf frames (quest1_dmabuf.c)
			snprintf(fake_names[fake_count], VK_MAX_EXTENSION_NAME_SIZE, "%s",
			         VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
			fake_spec[fake_count++] = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_SPEC_VERSION;
			snprintf(fake_names[fake_count], VK_MAX_EXTENSION_NAME_SIZE, "%s",
			         VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
			fake_spec[fake_count++] = VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_SPEC_VERSION;
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
	PFN_vkGetPhysicalDeviceFormatProperties2 GetPhysicalDeviceFormatProperties2;
	PFN_vkGetPhysicalDeviceImageFormatProperties2 GetPhysicalDeviceImageFormatProperties2;
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
	PFN_vkGetFenceStatus GetFenceStatus;
	PFN_vkGetSemaphoreFdKHR GetSemaphoreFdKHR;
	PFN_vkImportSemaphoreFdKHR ImportSemaphoreFdKHR;
	PFN_vkCmdUpdateBuffer CmdUpdateBuffer;
	PFN_vkCmdBindIndexBuffer CmdBindIndexBuffer;
	PFN_vkCmdCopyBuffer CmdCopyBuffer;
	PFN_vkBindBufferMemory BindBufferMemory;
	PFN_vkBindBufferMemory2 BindBufferMemory2;
	PFN_vkBindImageMemory BindImageMemory;
	PFN_vkBindImageMemory2 BindImageMemory2;
	// venus NV12 as an external format (quest1_modifier.c)
	PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
	PFN_vkGetImageMemoryRequirements2 GetImageMemoryRequirements2, GetImageMemoryRequirements2KHR;
	PFN_vkCreateImageView CreateImageView;
	PFN_vkCreateSamplerYcbcrConversion CreateSamplerYcbcrConversion, CreateSamplerYcbcrConversionKHR;
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

// The rest of the layer, in this order (one translation unit: everything below is included here):
#include "quest1_compat_timeline.c"
#include "quest1_compat_submit.c"
#include "quest1_compat_physdev.c"

// --- simulated direct-mode display for vrcompositor ----------------------------------------------------

#include "quest1_display.c"
#include "quest1_facet.c"
#include "quest1_dmabuf.c"
#include "quest1_modifier.c"

#include "quest1_compat_sync.c"
#include "quest1_compat_spirv.c"
#include "quest1_compat_cmdbuf.c"
#include "quest1_compat_memory.c"
#include "quest1_compat_modules.c"
#include "quest1_compat_pipelines.c"
#include "quest1_compat_descriptors.c"
#include "quest1_compat_device.c"
#include "quest1_compat_instance.c"
