// quest1_display: a simulated direct-mode display for vrcompositor (included by quest1_compat.c).
//
// vrcompositor always renders its distortion pass into the swapchain of a direct-mode display
// (CHmdWindowVulkanWSI), even when the driver offers IVRVirtualDisplay. The Android loader has no
// VK_KHR_display at all, so this provides one: a display named "Valve Corporation Quest1" (the
// prefix vrcompositor matches), one mode, one plane, a surface and a swapchain whose images are
// exportable (OPAQUE_FD). On vkQueuePresentKHR the finished image goes to driver_quest1 in vrserver
// over a unix socket (the memory fds once, then image indices); the driver blits it into its
// OpenXR swapchains for Monado and hands the image back.
//
// Socket protocol (struct qd_msg, both directions):
//   layer -> driver  QD_SWAPCHAIN: image description + one fd per image (SCM_RIGHTS)
//   layer -> driver  QD_PRESENT:   index of a finished image (in TRANSFER_SRC_OPTIMAL)
//   driver -> layer  QD_RELEASE:   the driver is done reading that image
//
// Env: QUEST1_DISPLAY_SIZE (default 2448x1360), QUEST1_DISPLAY_HZ (default 72),
//      QUEST1_DISPLAY_SOCKET (default $XDG_RUNTIME_DIR/quest1-display.sock)
#include <sys/socket.h>
#include <sys/un.h>
#include <vulkan/vulkan_android.h>
typedef struct AHardwareBuffer AHardwareBuffer;

#include "quest1_display_proto.h"
#include "quest1_direct_proto.h"

#define QD_DISPLAY ((VkDisplayKHR)(uintptr_t)0x51d1500d)
#define QD_MODE ((VkDisplayModeKHR)(uintptr_t)0x51d1500e)
#define QD_NAME "Valve Corporation Quest1 (virtual)"

static uint32_t qd_width = 2448, qd_height = 1360, qd_mhz = 72000;
//! How long a present waits for the driver to publish the next frame's pose (QD_POSED). If it returns before,
//! vrcompositor may render that frame with the previous pose while Monado's warp re-projects it from the new one: a
//! frame of head motion off, the whole image trembled (2026-11-09). 4 ms ran out 6-9 times a second with the
//! driver's reply arriving late (this layer's reply thread and Monado's IPC thread were normal threads), still now
//! and then once those were real-time (Monado answering xrLocateSpace up to 16 ms late): a late frame (its content
//! repeated, its rotation right) is better than a wrong warp.
#define QD_POSE_WAIT_NS 8000000

//! the vblank whose display-event fences were signaled last: vrcompositor starts a frame there
static _Atomic uint64_t qd_frame_tick;

// frame timing, logged every 5 s: where vrcompositor's frame period goes
static struct
{
	uint64_t since, last_return, n;
	uint64_t app, acquire, gpu, posed; //!< summed ns
	_Atomic uint64_t present_wait, present_waits; //!< vkWaitForPresentKHR (another thread)
	_Atomic uint64_t wait_depth; //!< summed: latest presented id minus the id waited for
	//! presents returned before the driver had published the next frame's pose (QD_POSE_WAIT_NS ran out): vrcompositor
	//! may render that frame with the previous pose while Monado's warp takes the new one for it
	uint64_t pose_timeouts;
	uint64_t pose_wait_max; //!< ns
} qd_stats;

//! async present: time from vkQueuePresentKHR to the image being rendered (summed, completer thread)
static _Atomic uint64_t qd_stats_gpu_latency;

static void qd_stats_gpu(uint64_t latency_ns) { atomic_fetch_add(&qd_stats_gpu_latency, latency_ns); }

static void qd_stats_present(uint64_t entry, uint64_t gpu_done, uint64_t posed_done)
{
	if (qd_stats.last_return)
		qd_stats.app += entry - qd_stats.last_return;
	qd_stats.gpu += gpu_done - entry;
	qd_stats.posed += posed_done - gpu_done;
	if (posed_done - gpu_done > qd_stats.pose_wait_max)
		qd_stats.pose_wait_max = posed_done - gpu_done;
	qd_stats.n++;
	qd_stats.last_return = posed_done;
	if (!qd_stats.since) {
		qd_stats.since = posed_done;
	} else if (posed_done - qd_stats.since > 5000000000ull) {
		double n = (double)qd_stats.n;
		uint64_t waits = atomic_exchange(&qd_stats.present_waits, 0);
		uint64_t wait_ns = atomic_exchange(&qd_stats.present_wait, 0);
		uint64_t depth = atomic_exchange(&qd_stats.wait_depth, 0);
		LOG("display: %.1f fps; per frame: vrcompositor %.2f ms (of which acquire %.2f), gpu wait %.2f, "
		    "pose wait %.2f (max %.2f, ran out %llu), present-to-rendered %.2f, present wait %.2f (%.2f frames behind)\n",
		    n * 1e9 / (posed_done - qd_stats.since), qd_stats.app / n / 1e6, qd_stats.acquire / n / 1e6,
		    qd_stats.gpu / n / 1e6, qd_stats.posed / n / 1e6, qd_stats.pose_wait_max / 1e6,
		    (unsigned long long)qd_stats.pose_timeouts,
		    atomic_exchange(&qd_stats_gpu_latency, 0) / n / 1e6, waits ? wait_ns / (double)waits / 1e6 : 0.0,
		    waits ? depth / (double)waits : 0.0);
		uint64_t keep = qd_stats.last_return;
		qd_stats.since = qd_stats.last_return = keep;
		qd_stats.n = qd_stats.app = qd_stats.acquire = qd_stats.gpu = qd_stats.posed = 0;
		qd_stats.pose_timeouts = qd_stats.pose_wait_max = 0;
	}
}

// QUEST1_DISPLAY_TRACE=N: log 120 display calls, after the first N-1, with their time (ms within the
// second) and thread
static void qd_trace(const char *what)
{
	static atomic_uint n;
	const char *e = getenv("QUEST1_DISPLAY_TRACE");
	if (!e)
		return;
	unsigned skip = atoi(e) > 1 ? (unsigned)atoi(e) - 1 : 0, i = atomic_fetch_add(&n, 1);
	if (i < skip || i >= skip + 120)
		return;
	uint64_t t = now_ns();
	LOG("trace %-12s %8.3f ms (vblank phase %6.3f ms) tid %ld\n", what, (t % 1000000000ull) / 1e6,
	    (t % (1000000000000ull / qd_mhz)) / 1e6, (long)syscall(SYS_gettid));
}
static pthread_once_t qd_once = PTHREAD_ONCE_INIT;

static void qd_init(void)
{
	const char *s = getenv("QUEST1_DISPLAY_SIZE");
	if (s)
		sscanf(s, "%ux%u", &qd_width, &qd_height);
	if ((s = getenv("QUEST1_DISPLAY_HZ")))
		qd_mhz = (uint32_t)(atof(s) * 1000);
}

// The rest, in this order (one translation unit with quest1_compat.c):
#include "quest1_display_queries.c"
#include "quest1_display_surfaces.c"
#include "quest1_display_swapchains.c"
#include "quest1_display_present.c"
#include "quest1_display_frametrace.c"
#include "quest1_display_vblank.c"
#include "quest1_display_dispatch.c"
