// test_fdleak: which Vulkan usage pattern leaves an anon_inode:sync_fence descriptor behind on the Quest 1's
// Adreno driver. Written 12 Nov 2026 to find vrcompositor's descriptor leak (about one per frame).
//
//   test_fdleak raw    [n]   the basic patterns (A .. RP) straight on the driver    (QUEST1_VKSHIM_LAYER=none)
//   test_fdleak layer  [n]   the same, plus H: a timeline semaphore signalled by a submit, which the
//                            compatibility layer emulates                           (QUEST1_STEAMVR_COMPAT=1)
//   test_fdleak rawnew [n]   AB0, AB, UW: fences never destroyed. With one more word: AB2 .. AB6 (work in flight)
//   test_fdleak rg     [n]   RG8, RG1, RW, RGD: fences used again after a reset, which is what the layer's pool does
//   test_fdleak ev     [n]   through the layer (QUEST1_STEAMVR_COMPAT=1 QUEST1_VKSHIM_LAYER=<the layer's .so>):
//                            rawnew's cases, then display events (EV2, EV). With one more word: AB2 .. AB6, EV4, EV3.
//                            With two: vrcompositor's frame only (EV5, 4 x n). With three: EV5, then events left
//                            behind after their vblank (EV6: 20, then 10 more 5.5 s later).
//                            The words themselves are not read, only how many: "test_fdleak ev 500 r5 evonly aged".
// Build on the headset: gcc -std=gnu11 -O1 -Wall -I <Vulkan-Headers>/include -o test_fdleak test_fdleak.c -ldl
// Run as steamos with LD_LIBRARY_PATH=/usr/local/lib/quest1-vk:/opt/hybris/lib LD_PRELOAD=/opt/hybris/lib/libbionictls.so
// It creates its own small GPU context and touches nothing else. Prints one CASE line per pattern: "sync_fence +N"
// is how many sync-fence descriptors the process gained over the n rounds, "all" the same for every descriptor.
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dirent.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x)                                                                                                       \
	do {                                                                                                           \
		VkResult r_ = (x);                                                                                     \
		if (r_ != VK_SUCCESS) {                                                                                \
			printf("FAIL line %d: %s = %d\n", __LINE__, #x, r_);                                           \
			fflush(stdout);                                                                                \
			exit(1);                                                                                       \
		}                                                                                                      \
	} while (0)

static PFN_vkGetInstanceProcAddr gipa;
#define FN(n) static PFN_##n n;
FN(vkCreateInstance)
FN(vkEnumeratePhysicalDevices)
FN(vkGetPhysicalDeviceProperties)
FN(vkGetPhysicalDeviceMemoryProperties)
FN(vkEnumerateDeviceExtensionProperties)
FN(vkCreateDevice)
FN(vkGetDeviceProcAddr)
FN(vkGetDeviceQueue)
FN(vkCreateCommandPool)
FN(vkAllocateCommandBuffers)
FN(vkBeginCommandBuffer)
FN(vkEndCommandBuffer)
FN(vkCmdFillBuffer)
FN(vkCreateBuffer)
FN(vkGetBufferMemoryRequirements)
FN(vkAllocateMemory)
FN(vkBindBufferMemory)
FN(vkCreateFence)
FN(vkDestroyFence)
FN(vkResetFences)
FN(vkWaitForFences)
FN(vkGetFenceStatus)
FN(vkCreateSemaphore)
FN(vkDestroySemaphore)
FN(vkQueueSubmit)
FN(vkQueueWaitIdle)
FN(vkGetSemaphoreFdKHR)
FN(vkWaitSemaphoresKHR)
#undef FN

static VkInstance inst;
static VkDevice dev;
static VkQueue queue;
static VkCommandBuffer cb; // fills a 4 KiB buffer: real GPU work, so the submit gets a GPU timestamp
static VkFence F;          // one reused fence, like the layer's pool
static bool have_fd_ext, layer_mode;
static int minus_one;      // R3: exports that returned -1 ("already signalled")
static VkSemaphore T;      // H: the timeline semaphore
static uint64_t tvalue;

//! Open descriptors of this process: sync fences, and all of them.
static int sync_fds(int *all)
{
	DIR *d = opendir("/proc/self/fd");
	struct dirent *e;
	int n = 0, a = 0;
	char p[64], b[160];
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		a++;
		snprintf(p, sizeof(p), "/proc/self/fd/%s", e->d_name);
		ssize_t l = readlink(p, b, sizeof(b) - 1);
		if (l > 0) {
			b[l] = 0;
			if (strstr(b, "sync_fence") || strstr(b, "sync_file"))
				n++;
		}
	}
	if (d)
		closedir(d);
	*all = a - 1; // not the directory handle itself
	return n;
}

// the driver returns VK_TIMEOUT at once for UINT64_MAX: wait in steps
static void wait_reset(VkFence f, bool reset)
{
	VkResult r;
	while ((r = vkWaitForFences(dev, 1, &f, VK_TRUE, 100000000ull)) == VK_TIMEOUT)
		;
	CHECK(r);
	if (reset)
		CHECK(vkResetFences(dev, 1, &f));
}

static void submit(uint32_t nw, const VkSemaphore *w, uint32_t ns, const VkSemaphore *s, bool work, VkFence f)
{
	VkPipelineStageFlags st[2] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.waitSemaphoreCount = nw;
	si.pWaitSemaphores = w;
	si.pWaitDstStageMask = st;
	si.commandBufferCount = work ? 1 : 0;
	si.pCommandBuffers = &cb;
	si.signalSemaphoreCount = ns;
	si.pSignalSemaphores = s;
	CHECK(vkQueueSubmit(queue, 1, &si, f));
}

static VkSemaphore sem_new(bool exportable)
{
	VkExportSemaphoreCreateInfo e = {VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
	e.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
	VkSemaphoreCreateInfo ci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	if (exportable)
		ci.pNext = &e;
	VkSemaphore s;
	CHECK(vkCreateSemaphore(dev, &ci, NULL, &s));
	return s;
}

static void case_A(void)
{
	VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence f;
	CHECK(vkCreateFence(dev, &fci, NULL, &f));
	submit(0, NULL, 0, NULL, true, f);
	wait_reset(f, false);
	vkDestroyFence(dev, f, NULL);
}

static void case_B(void)
{
	submit(0, NULL, 0, NULL, true, F);
	wait_reset(F, true);
}

static void case_C(void)
{
	VkSemaphore s = sem_new(false);
	submit(0, NULL, 1, &s, true, F);
	wait_reset(F, true);
	vkDestroySemaphore(dev, s, NULL);
}

static void case_C2(void)
{
	VkSemaphore s = sem_new(true);
	submit(0, NULL, 1, &s, true, F);
	wait_reset(F, true);
	vkDestroySemaphore(dev, s, NULL);
}

static void case_D(void)
{
	VkSemaphore s = sem_new(false);
	submit(0, NULL, 1, &s, true, F);
	wait_reset(F, true);
	submit(1, &s, 0, NULL, true, F);
	wait_reset(F, true);
	vkDestroySemaphore(dev, s, NULL);
}

static void case_E(void)
{
	VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence f;
	CHECK(vkCreateFence(dev, &fci, NULL, &f));
	CHECK(vkQueueSubmit(queue, 0, NULL, f));
	wait_reset(f, false);
	vkDestroyFence(dev, f, NULL);
}

static void case_R1(void)
{
	VkSemaphore s = sem_new(false);
	submit(0, NULL, 1, &s, true, F);
	wait_reset(F, true);
	submit(1, &s, 0, NULL, false, F); // a batch with no commands that only waits on it
	wait_reset(F, true);
	vkDestroySemaphore(dev, s, NULL);
}

static void case_R3(void)
{
	VkSemaphore s = sem_new(true);
	submit(0, NULL, 1, &s, true, F);
	wait_reset(F, true);
	VkSemaphoreGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
	gi.semaphore = s;
	gi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
	int fd = -2;
	CHECK(vkGetSemaphoreFdKHR(dev, &gi, &fd));
	if (fd >= 0)
		close(fd);
	else
		minus_one++;
	vkDestroySemaphore(dev, s, NULL);
}

static void case_H(void)
{
	uint64_t v = ++tvalue;
	VkTimelineSemaphoreSubmitInfo tl = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
	tl.signalSemaphoreValueCount = 1;
	tl.pSignalSemaphoreValues = &v;
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &tl};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cb;
	si.signalSemaphoreCount = 1;
	si.pSignalSemaphores = &T;
	CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
	VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
	wi.semaphoreCount = 1;
	wi.pSemaphores = &T;
	wi.pValues = &v;
	CHECK(vkWaitSemaphoresKHR(dev, &wi, 5000000000ull));
}


// --- second round: what vrcompositor's frame does that the first round did not cover -----------------------
static void case_N(void)
{
	submit(0, NULL, 0, NULL, true, VK_NULL_HANDLE); // work, no fence, no semaphore
	vkQueueWaitIdle(queue);
}

static void case_N2(void)
{
	submit(0, NULL, 0, NULL, true, VK_NULL_HANDLE);
	submit(0, NULL, 0, NULL, true, F); // a fenced submit right behind it, as in a frame
	wait_reset(F, true);
}

static VkSemaphore S; // reused over the iterations, like an application's own semaphore

static void case_NS(void)
{
	submit(0, NULL, 1, &S, true, VK_NULL_HANDLE); // the layer's acquire: no fence, signals the app's semaphore
	submit(1, &S, 0, NULL, true, F);
	wait_reset(F, true);
}

static void case_NS2(void)
{
	VkSemaphore s = sem_new(false);
	submit(0, NULL, 1, &s, true, VK_NULL_HANDLE);
	vkQueueWaitIdle(queue);
	vkDestroySemaphore(dev, s, NULL);
}

static void case_S1(void)
{
	submit(0, NULL, 1, &S, true, F);
	wait_reset(F, true);
	submit(1, &S, 0, NULL, true, F);
	wait_reset(F, true);
}

static void case_BB(void)
{
	VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence f2;
	CHECK(vkCreateFence(dev, &fci, NULL, &f2));
	submit(0, NULL, 1, &S, true, f2); // back to back: the wait is queued before the signal has run
	submit(1, &S, 0, NULL, true, F);
	wait_reset(F, true);
	wait_reset(f2, false);
	vkDestroyFence(dev, f2, NULL);
}

static void case_F1(void)
{
	submit(0, NULL, 0, NULL, true, F);
	vkQueueWaitIdle(queue); // never waited for or asked about
	CHECK(vkResetFences(dev, 1, &F));
}

static void case_F2(void)
{
	submit(0, NULL, 0, NULL, true, F);
	while (vkGetFenceStatus(dev, F) == VK_NOT_READY)
		usleep(200);
	CHECK(vkResetFences(dev, 1, &F));
}

// --- third round: usage outside the rules, which an application may still do --------------------------------
static void case_NS3(void)
{
	submit(0, NULL, 1, &S, true, VK_NULL_HANDLE); // signalled again and again, nobody ever waits on it
	vkQueueWaitIdle(queue);
}

static void case_WSS(void)
{
	submit(1, &S, 1, &S, true, F); // one batch waits on S and signals S again
	wait_reset(F, true);
}

static void case_FR(void)
{
	submit(0, NULL, 0, NULL, true, F); // the fence is used again without a reset in between
	VkResult r;
	while ((r = vkWaitForFences(dev, 1, &F, VK_TRUE, 100000000ull)) == VK_TIMEOUT)
		;
}

static void case_RP(void)
{
	submit(0, NULL, 0, NULL, true, F);
	vkResetFences(dev, 1, &F); // reset while the work may still be running
	vkQueueWaitIdle(queue);
}

// --- fourth round: display-event fences, the way vrcompositor's vblank thread uses them ----------------------
#include <time.h>
static PFN_vkRegisterDisplayEventEXT regEvent;
static int ev_timeout, ev_success, ev_other;
static double ev_wait_ms_max;
static double ms_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static VkFence ev_register(void)
{
	VkDisplayEventInfoEXT info = {VK_STRUCTURE_TYPE_DISPLAY_EVENT_INFO_EXT};
	info.displayEvent = VK_DISPLAY_EVENT_TYPE_FIRST_PIXEL_OUT_EXT;
	VkFence f;
	CHECK(regEvent(dev, VK_NULL_HANDLE, &info, NULL, &f));
	return f;
}

static void case_EV(void)
{
	VkFence f = ev_register();
	double t = ms_now();
	VkResult r = vkWaitForFences(dev, 1, &f, VK_TRUE, 50000000ull); // 50 ms: more than three vblanks
	double d = ms_now() - t;
	if (d > ev_wait_ms_max)
		ev_wait_ms_max = d;
	if (r == VK_TIMEOUT)
		ev_timeout++;
	else if (r == VK_SUCCESS)
		ev_success++;
	else
		ev_other++;
	usleep(14000); // and the fence is left behind, never destroyed
}

static void case_EV2(void)
{
	VkFence f = ev_register();
	usleep(20000); // a vblank passes: the layer has submitted the fence
	wait_reset(f, false);
	vkDestroyFence(dev, f, NULL);
}

static void case_AB(void)
{
	VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence f;
	CHECK(vkCreateFence(dev, &fci, NULL, &f));
	CHECK(vkQueueSubmit(queue, 0, NULL, f));
	vkQueueWaitIdle(queue); // signalled, then left behind
}

static void case_AB0(void)
{
	VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence f;
	CHECK(vkCreateFence(dev, &fci, NULL, &f)); // never used, left behind
}

static int uw_timeout;
static double uw_ms_max;
static void case_UW(void)
{
	VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence f;
	CHECK(vkCreateFence(dev, &fci, NULL, &f));
	double t = ms_now();
	VkResult r = vkWaitForFences(dev, 1, &f, VK_TRUE, 20000000ull); // 20 ms on a fence nothing was submitted with
	double d = ms_now() - t;
	if (d > uw_ms_max)
		uw_ms_max = d;
	uw_timeout += r == VK_TIMEOUT;
	vkDestroyFence(dev, f, NULL);
}

// --- fifth round: a fence on an empty submit while real work is still in flight, then left behind --------------
static VkFence fresh_fence(void)
{
	VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence f;
	CHECK(vkCreateFence(dev, &fci, NULL, &f));
	return f;
}

static void case_AB2(void)
{
	VkFence f = fresh_fence();
	submit(0, NULL, 0, NULL, true, VK_NULL_HANDLE); // real work, no fence
	CHECK(vkQueueSubmit(queue, 0, NULL, f));         // the fence rides behind it
	vkQueueWaitIdle(queue);                          // never asked about, never destroyed
}

static void case_AB3(void)
{
	VkFence f = fresh_fence();
	submit(0, NULL, 0, NULL, true, VK_NULL_HANDLE);
	CHECK(vkQueueSubmit(queue, 0, NULL, f));
	vkQueueWaitIdle(queue);
	while (vkGetFenceStatus(dev, f) == VK_NOT_READY) // status asked once it is signalled, still not destroyed
		usleep(200);
}

static void case_AB4(void)
{
	VkFence f = fresh_fence();
	submit(0, NULL, 0, NULL, true, VK_NULL_HANDLE);
	CHECK(vkQueueSubmit(queue, 0, NULL, f));
	wait_reset(f, false); // waited for with success, still not destroyed
}

static void case_AB5(void)
{
	VkFence f = fresh_fence();
	submit(0, NULL, 0, NULL, true, VK_NULL_HANDLE);
	CHECK(vkQueueSubmit(queue, 0, NULL, f));
	vkQueueWaitIdle(queue);
	vkDestroyFence(dev, f, NULL); // destroyed without ever being asked about
}

static void case_AB6(void)
{
	VkFence f = fresh_fence();
	VkResult r = vkWaitForFences(dev, 1, &f, VK_TRUE, 20000000ull); // times out at once: nothing submitted yet
	(void)r;
	submit(0, NULL, 0, NULL, true, VK_NULL_HANDLE);
	CHECK(vkQueueSubmit(queue, 0, NULL, f));
	vkQueueWaitIdle(queue); // then left behind: the display-event sequence, without the layer
}

static void case_EV3(void)
{
	VkFence f = ev_register();
	VkResult r = vkWaitForFences(dev, 1, &f, VK_TRUE, 50000000ull);
	ev_timeout += r == VK_TIMEOUT;
	double until = ms_now() + 16; // keep work in flight over the next vblank, when the layer submits the fence
	while (ms_now() < until) {
		submit(0, NULL, 0, NULL, true, VK_NULL_HANDLE);
		usleep(300);
	}
	// the fence is left behind, as vrcompositor leaves it
}

static void case_EV4(void)
{
	VkFence f = ev_register();
	double until = ms_now() + 16;
	while (ms_now() < until) {
		submit(0, NULL, 0, NULL, true, VK_NULL_HANDLE);
		usleep(300);
	}
	wait_reset(f, false);
	vkDestroyFence(dev, f, NULL); // the well-behaved sequence under the same load
}

//! vrcompositor's frame (frame trace 2026-11-12): event a, waited for at once (times out), left behind; event b,
//! work over the next vblank, b waited for (success) and destroyed.
static void case_EV5(void)
{
	VkFence a = ev_register();
	VkResult r = vkWaitForFences(dev, 1, &a, VK_TRUE, 50000000ull);
	ev_timeout += r == VK_TIMEOUT;
	VkFence b = ev_register();
	double until = ms_now() + 16;
	while (ms_now() < until) {
		submit(0, NULL, 0, NULL, true, VK_NULL_HANDLE);
		usleep(300);
	}
	wait_reset(b, false);
	vkDestroyFence(dev, b, NULL);
}

//! A display event left behind after its vblank: never waited for, never destroyed.
static void case_EV6(void)
{
	(void)ev_register();
	double until = ms_now() + 16;
	while (ms_now() < until) {
		submit(0, NULL, 0, NULL, true, VK_NULL_HANDLE);
		usleep(300);
	}
}

// --- sixth round: a small set of fences used again and again (what a layer fix would do) ------------------------
#define RG_MAX 8
static VkFence rg[RG_MAX];
static int rg_i, rg_k = RG_MAX, rg_waited;

//! Slot k used again: wait until its last submit has signalled it, reset it, then the display-event sequence
//! (real work in flight with no fence, the fence on an empty submit behind it). Never destroyed.
static void case_RG(void)
{
	int k = rg_i++ % rg_k;
	if (rg[k] == VK_NULL_HANDLE) {
		rg[k] = fresh_fence();
	} else {
		int spins = 0;
		while (vkGetFenceStatus(dev, rg[k]) == VK_NOT_READY) {
			usleep(200);
			spins++;
		}
		rg_waited += spins > 0;
		CHECK(vkResetFences(dev, 1, &rg[k]));
	}
	submit(0, NULL, 0, NULL, true, VK_NULL_HANDLE);
	CHECK(vkQueueSubmit(queue, 0, NULL, rg[k]));
	usleep(1000);
}

//! The same with the slot's fence destroyed and created anew each time instead of reset.
static VkFence rgd[RG_MAX];
static int rgd_i;
static void case_RGD(void)
{
	int k = rgd_i++ % RG_MAX;
	if (rgd[k] != VK_NULL_HANDLE) {
		while (vkGetFenceStatus(dev, rgd[k]) == VK_NOT_READY)
			usleep(200);
		vkDestroyFence(dev, rgd[k], NULL);
	}
	rgd[k] = fresh_fence();
	submit(0, NULL, 0, NULL, true, VK_NULL_HANDLE);
	CHECK(vkQueueSubmit(queue, 0, NULL, rgd[k]));
	usleep(1000);
}

//! A fence that was reset and has nothing submitted with it: how does a 20 ms wait on it return? (a new one: a
//! timeout at once, case UW). It is left submitted for the next round.
static int rw_timeout, rw_notready;
static double rw_ms_max;
static void case_RW(void)
{
	VkFence f = rg[0];
	while (vkGetFenceStatus(dev, f) == VK_NOT_READY)
		usleep(200);
	CHECK(vkResetFences(dev, 1, &f));
	rw_notready += vkGetFenceStatus(dev, f) == VK_NOT_READY;
	double t = ms_now();
	VkResult r = vkWaitForFences(dev, 1, &f, VK_TRUE, 20000000ull);
	double dt = ms_now() - t;
	if (dt > rw_ms_max)
		rw_ms_max = dt;
	rw_timeout += r == VK_TIMEOUT;
	submit(0, NULL, 0, NULL, true, VK_NULL_HANDLE);
	CHECK(vkQueueSubmit(queue, 0, NULL, f));
}

static void run(const char *name, const char *desc, void (*fn)(void), int n)
{
	int a0, a1;
	int s0 = sync_fds(&a0);
	for (int i = 0; i < n; i++)
		fn();
	vkQueueWaitIdle(queue);
	usleep(300000); // the layer's completion thread tidies up after the submit
	int s1 = sync_fds(&a1);
	printf("CASE %-2s n=%d sync_fence %+d all %+d | %s\n", name, n, s1 - s0, a1 - a0, desc);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	bool only_new = argc > 1 && (strcmp(argv[1], "ev") == 0 || strcmp(argv[1], "rawnew") == 0);
	bool rg_mode = argc > 1 && strcmp(argv[1], "rg") == 0;
	bool r5 = argc > 3;
	bool ev_only = argc > 4;
	layer_mode = argc > 1 && (strcmp(argv[1], "layer") == 0 || strcmp(argv[1], "ev") == 0);
	int n = argc > 2 ? atoi(argv[2]) : 100;
	void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
	if (!lib) {
		printf("FAIL dlopen: %s\n", dlerror());
		return 1;
	}
	gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
	vkCreateInstance = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	CHECK(vkCreateInstance(&ici, NULL, &inst));
#define FN(x) x = (PFN_##x)gipa(inst, #x);
	FN(vkEnumeratePhysicalDevices)
	FN(vkGetPhysicalDeviceProperties)
	FN(vkGetPhysicalDeviceMemoryProperties)
	FN(vkEnumerateDeviceExtensionProperties)
	FN(vkCreateDevice)
	FN(vkGetDeviceProcAddr)
#undef FN
	uint32_t one = 1;
	VkPhysicalDevice pd;
	vkEnumeratePhysicalDevices(inst, &one, &pd);
	VkPhysicalDeviceProperties pp;
	vkGetPhysicalDeviceProperties(pd, &pp);

	static VkExtensionProperties ext[512];
	uint32_t next = 512;
	vkEnumerateDeviceExtensionProperties(pd, NULL, &next, ext);
	for (uint32_t i = 0; i < next; i++)
		if (strcmp(ext[i].extensionName, "VK_KHR_external_semaphore_fd") == 0)
			have_fd_ext = true;

	const char *exts[2];
	uint32_t nexts = 0;
	if (have_fd_ext || layer_mode)
		exts[nexts++] = "VK_KHR_external_semaphore_fd";
	if (layer_mode)
		exts[nexts++] = "VK_KHR_timeline_semaphore";
	VkPhysicalDeviceTimelineSemaphoreFeatures tf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
	tf.timelineSemaphore = VK_TRUE;
	float prio = 1;
	VkDeviceQueueCreateInfo q = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	q.queueCount = 1;
	q.pQueuePriorities = &prio;
	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	if (layer_mode)
		dci.pNext = &tf;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &q;
	dci.enabledExtensionCount = nexts;
	dci.ppEnabledExtensionNames = exts;
	CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
#define FN(x) x = (PFN_##x)vkGetDeviceProcAddr(dev, #x);
	FN(vkGetDeviceQueue)
	FN(vkCreateCommandPool)
	FN(vkAllocateCommandBuffers)
	FN(vkBeginCommandBuffer)
	FN(vkEndCommandBuffer)
	FN(vkCmdFillBuffer)
	FN(vkCreateBuffer)
	FN(vkGetBufferMemoryRequirements)
	FN(vkAllocateMemory)
	FN(vkBindBufferMemory)
	FN(vkCreateFence)
	FN(vkDestroyFence)
	FN(vkResetFences)
	FN(vkWaitForFences)
	FN(vkGetFenceStatus)
	FN(vkCreateSemaphore)
	FN(vkDestroySemaphore)
	FN(vkQueueSubmit)
	FN(vkQueueWaitIdle)
	FN(vkGetSemaphoreFdKHR)
	FN(vkWaitSemaphoresKHR)
#undef FN
	vkGetDeviceQueue(dev, 0, 0, &queue);
	printf("device: %s, mode %s, n=%d, VK_KHR_external_semaphore_fd %s, vkGetSemaphoreFdKHR %s\n", pp.deviceName,
	       layer_mode ? "layer" : "raw", n, have_fd_ext ? "listed" : "not listed", vkGetSemaphoreFdKHR ? "found" : "missing");

	// a command buffer with real work
	VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = 4096;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VkBuffer buf;
	CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(dev, buf, &mr);
	VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = mr.size;
	while (mai.memoryTypeIndex < 32 && !(mr.memoryTypeBits & (1u << mai.memoryTypeIndex)))
		mai.memoryTypeIndex++;
	VkDeviceMemory mem;
	CHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
	CHECK(vkBindBufferMemory(dev, buf, mem, 0));
	VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
	VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cai.commandPool = pool;
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 1;
	CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
	CHECK(vkBeginCommandBuffer(cb, &bi));
	vkCmdFillBuffer(cb, buf, 0, 4096, 0x51315131);
	CHECK(vkEndCommandBuffer(cb));
	VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	CHECK(vkCreateFence(dev, &fci, NULL, &F));

	// warm up, so one-off descriptors of the first submit are not counted against a pattern
	for (int i = 0; i < 3; i++)
		case_B();
	int all;
	printf("start: %d sync fences, %d descriptors\n", sync_fds(&all), all);

	if (rg_mode) {
		run("RG8", "8 fences used in turn: status, reset, work in flight, empty submit with the fence; never destroyed", case_RG, n);
		printf("RG8: %d of %d reuses had to wait for the fence to signal\n", rg_waited, n);
		rg_k = 1;
		rg_waited = 0;
		run("RG1", "as RG8 with a single fence", case_RG, n);
		printf("RG1: %d of %d reuses had to wait for the fence to signal\n", rg_waited, n);
		run("RW", "a reset fence with nothing submitted: 20 ms wait, then submitted again", case_RW, 20);
		printf("RW: %d of 20 waits timed out, longest %.3f ms; status not ready after the reset: %d of 20\n", rw_timeout, rw_ms_max, rw_notready);
		run("RGD", "8 slots, the slot's fence destroyed and created anew each time", case_RGD, n);
		run("AB2", "control: a new fence each time, never destroyed (the leak)", case_AB2, 100);
		vkQueueWaitIdle(queue);
		usleep(300000);
		printf("end: %d sync fences, %d descriptors\nRESULT done\n", sync_fds(&all), all);
		return 0;
	}
	if (!only_new) {
	run("A", "fence: create, submit with it, wait, destroy", case_A, n);
	run("B", "fence: one fence reused: submit, wait, reset", case_B, n);
	run("C", "semaphore signalled by a submit, never waited on, destroyed after the submit finished", case_C, n);
	run("D", "semaphore signalled, a second submit waits on it, destroyed after both finished", case_D, n);
	run("E", "fence on a submit with no batches (the vblank path): create, submit, wait, destroy", case_E, n);
	run("R1", "as C, but a batch with no commands waits on it before the destroy", case_R1, n);
	if (vkGetSemaphoreFdKHR && (have_fd_ext || layer_mode)) {
		run("C2", "as C, semaphore created exportable as a sync descriptor", case_C2, n);
		run("R3", "as C2, descriptor taken out with vkGetSemaphoreFdKHR and closed before the destroy", case_R3, n);
		printf("R3: %d of %d exports returned -1 (already signalled, nothing to close)\n", minus_one, n);
	}
	S = sem_new(false);
	run("N", "work submitted with no fence and no semaphore", case_N, n);
	run("N2", "as N, with a fenced submit right behind it", case_N2, n);
	run("NS", "no fence, signals a semaphore; the next submit waits on it (the layer's acquire, semaphore reused)", case_NS, n);
	run("NS2", "no fence, signals a semaphore nobody waits on; destroyed when the queue is idle", case_NS2, n);
	run("S1", "reused semaphore: signalled by one fenced submit, waited on by the next", case_S1, n);
	run("BB", "as S1, both submits queued back to back", case_BB, n);
	run("F1", "reused fence never waited for: submit, queue idle, reset", case_F1, n);
	run("F2", "reused fence polled with vkGetFenceStatus, then reset", case_F2, n);
	if (layer_mode && vkWaitSemaphoresKHR) {
		VkSemaphoreTypeCreateInfo t = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
		t.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		VkSemaphoreCreateInfo ci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &t};
		CHECK(vkCreateSemaphore(dev, &ci, NULL, &T));
		run("H", "timeline semaphore signalled by a submit (what the layer emulates), then waited for on the CPU", case_H, n);
	}
	submit(0, NULL, 1, &S, true, F); // leave S signalled for WSS
	wait_reset(F, true);
	run("WSS", "one batch both waits on a semaphore and signals it again", case_WSS, n);
	run("NS3", "semaphore signalled every time, never waited on (against the rules)", case_NS3, n);
	run("FR", "fence used for the next submit without a reset (against the rules)", case_FR, n);
	vkResetFences(dev, 1, &F);
	run("RP", "fence reset while its work may still run (against the rules)", case_RP, n);
	}
	if (only_new) {
		if (ev_only) {
		} else if (r5) {
			run("AB2", "work in flight, a new fence on an empty submit behind it, never asked about, never destroyed", case_AB2, n);
			run("AB3", "as AB2, vkGetFenceStatus once signalled, never destroyed", case_AB3, n);
			run("AB4", "as AB2, waited for with success, never destroyed", case_AB4, n);
			run("AB5", "as AB2, destroyed without being asked about", case_AB5, n);
			run("AB6", "wait that times out at once, then as AB2 (display-event sequence without the layer)", case_AB6, n);
		} else {
		run("AB0", "fence created, never used, never destroyed", case_AB0, n);
		run("AB", "fence signalled by a submit with no batches, never destroyed", case_AB, n);
		run("UW", "20 ms wait on a fence nothing was submitted with, then destroyed", case_UW, 20);
		printf("UW: %d of 20 timed out, longest wait %.3f ms\n", uw_timeout, uw_ms_max);
		}
		regEvent = (PFN_vkRegisterDisplayEventEXT)vkGetDeviceProcAddr(dev, "vkRegisterDisplayEventEXT");
		if (layer_mode && regEvent) {
			if (ev_only) {
				ev_timeout = 0;
				for (int k = 0; k < 4; k++)
					run("EV5", "vrcompositor's frame: event a waited at once and left behind, event b waited for and destroyed", case_EV5, n);
				printf("EV5: %d of %d first waits timed out\n", ev_timeout, 4 * n);
				if (argc > 5) {
					run("EV6", "display event left behind after its vblank, never waited for, never destroyed", case_EV6, 20);
					usleep(5500000);
					run("EV6", "the same, 5.5 s later", case_EV6, 10);
				}
			} else if (r5) {
				ev_timeout = 0;
				run("EV4", "display-event fence under load: registered, waited for with success, destroyed", case_EV4, n);
				run("EV3", "display-event fence under load: wait at once (times out), then left behind (vrcompositor's pattern)", case_EV3, n);
				printf("EV3: %d of %d waits timed out\n", ev_timeout, n);
			} else {
			run("EV2", "display-event fence: registered, waited for after the vblank, destroyed", case_EV2, n);
			run("EV", "display-event fence: registered, waited for at once (50 ms), then left behind (vrcompositor's pattern)", case_EV, n);
			printf("EV: waits: %d timed out, %d succeeded, %d other; longest wait %.3f ms\n", ev_timeout, ev_success, ev_other, ev_wait_ms_max);
			}
			usleep(300000);
			int a2;
			printf("EV: 0.3 s later: %d sync fences\n", sync_fds(&a2));
		}
	}
	printf("end: %d sync fences, %d descriptors\nRESULT done\n", sync_fds(&all), all);
	return 0;
}
