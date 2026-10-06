// test_timeline: exercises the layer's timeline semaphore emulation across two processes.
// Parent exports a timeline semaphore (OPAQUE_FD), a forked child imports it, CPU-waits for 5,
// then submits GPU work that waits on 5 and signals 10; the parent signals 5 from a GPU submit
// and waits for 10. Run with QUEST1_STEAMVR_COMPAT=1 and the layer manifest on VK_LAYER_PATH /
// VK_ADD_IMPLICIT_LAYER_PATH (works on any Vulkan driver, e.g. lavapipe in WSL), or on the headset
// through quest1_vkshim (its directory first on LD_LIBRARY_PATH). Like SteamVR, it dlopens
// libvulkan.so.1 and resolves everything through vkGetInstanceProcAddr.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x)                                                                                                       \
	do {                                                                                                           \
		VkResult r_ = (x);                                                                                     \
		if (r_ != VK_SUCCESS) {                                                                                \
			fprintf(stderr, "[%d] %s = %d\n", getpid(), #x, r_);                                           \
			exit(1);                                                                                       \
		}                                                                                                      \
	} while (0)

static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
#define FN(n) static PFN_##n n;
FN(vkCreateInstance)
FN(vkDestroyInstance)
FN(vkEnumeratePhysicalDevices)
FN(vkGetPhysicalDeviceFeatures2)
FN(vkCreateDevice)
FN(vkDestroyDevice)
FN(vkGetDeviceQueue)
FN(vkGetDeviceProcAddr)
FN(vkCreateSemaphore)
FN(vkDestroySemaphore)
FN(vkQueueSubmit)
FN(vkQueueWaitIdle)
#undef FN

struct ctx
{
	VkInstance inst;
	VkDevice dev;
	VkQueue queue;
	PFN_vkGetSemaphoreFdKHR getFd;
	PFN_vkImportSemaphoreFdKHR importFd;
	PFN_vkWaitSemaphoresKHR wait;
	PFN_vkGetSemaphoreCounterValueKHR value;
};

static void init(struct ctx *c)
{
	void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
	if (!lib) {
		fprintf(stderr, "%s\n", dlerror());
		exit(1);
	}
	vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
	vkCreateInstance = (PFN_vkCreateInstance)vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	CHECK(vkCreateInstance(&ici, NULL, &c->inst));
#define FN(n) n = (PFN_##n)vkGetInstanceProcAddr(c->inst, #n);
	FN(vkDestroyInstance)
	FN(vkEnumeratePhysicalDevices)
	FN(vkGetPhysicalDeviceFeatures2)
	FN(vkCreateDevice)
	FN(vkDestroyDevice)
	FN(vkGetDeviceQueue)
	FN(vkGetDeviceProcAddr)
	FN(vkCreateSemaphore)
	FN(vkDestroySemaphore)
	FN(vkQueueSubmit)
	FN(vkQueueWaitIdle)
#undef FN
	uint32_t n = 1;
	VkPhysicalDevice pd;
	vkEnumeratePhysicalDevices(c->inst, &n, &pd);

	VkPhysicalDeviceTimelineSemaphoreFeatures tf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
	VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &tf};
	vkGetPhysicalDeviceFeatures2(pd, &f2);
	if (!tf.timelineSemaphore) {
		fprintf(stderr, "timelineSemaphore feature not reported\n");
		exit(1);
	}
	const char *exts[] = {"VK_KHR_timeline_semaphore", "VK_KHR_external_semaphore_fd"};
	float prio = 1;
	VkDeviceQueueCreateInfo q = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	q.queueCount = 1;
	q.pQueuePriorities = &prio;
	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &tf};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &q;
	dci.enabledExtensionCount = 2;
	dci.ppEnabledExtensionNames = exts;
	CHECK(vkCreateDevice(pd, &dci, NULL, &c->dev));
	vkGetDeviceQueue(c->dev, 0, 0, &c->queue);
	c->getFd = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(c->dev, "vkGetSemaphoreFdKHR");
	c->importFd = (PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(c->dev, "vkImportSemaphoreFdKHR");
	c->wait = (PFN_vkWaitSemaphoresKHR)vkGetDeviceProcAddr(c->dev, "vkWaitSemaphoresKHR");
	c->value = (PFN_vkGetSemaphoreCounterValueKHR)vkGetDeviceProcAddr(c->dev, "vkGetSemaphoreCounterValueKHR");
}

static VkSemaphore timeline(struct ctx *c, uint64_t initial)
{
	VkSemaphoreTypeCreateInfo t = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
	t.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	t.initialValue = initial;
	VkExportSemaphoreCreateInfo e = {VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, &t};
	e.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
	VkSemaphoreCreateInfo ci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &e};
	VkSemaphore s;
	CHECK(vkCreateSemaphore(c->dev, &ci, NULL, &s));
	return s;
}

// an empty batch waiting on (wait_sem, wait_value) and signalling (sig_sem, sig_value)
static void submit(struct ctx *c, VkSemaphore wait_sem, uint64_t wait_value, VkSemaphore sig_sem, uint64_t sig_value)
{
	VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	VkTimelineSemaphoreSubmitInfo tl = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
	tl.waitSemaphoreValueCount = wait_sem ? 1 : 0;
	tl.pWaitSemaphoreValues = &wait_value;
	tl.signalSemaphoreValueCount = 1;
	tl.pSignalSemaphoreValues = &sig_value;
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &tl};
	si.waitSemaphoreCount = wait_sem ? 1 : 0;
	si.pWaitSemaphores = &wait_sem;
	si.pWaitDstStageMask = &stage;
	si.signalSemaphoreCount = 1;
	si.pSignalSemaphores = &sig_sem;
	CHECK(vkQueueSubmit(c->queue, 1, &si, VK_NULL_HANDLE));
}

static VkResult host_wait(struct ctx *c, VkSemaphore s, uint64_t v, uint64_t timeout_ns)
{
	VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
	wi.semaphoreCount = 1;
	wi.pSemaphores = &s;
	wi.pValues = &v;
	return c->wait(c->dev, &wi, timeout_ns);
}

int main(void)
{
	struct ctx parent;
	init(&parent);
	VkSemaphore s = timeline(&parent, 0);
	VkSemaphoreGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
	gi.semaphore = s;
	gi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
	int fd = -1;
	CHECK(parent.getFd(parent.dev, &gi, &fd));
	int keep = dup(fd); // survives exec-less fork; the CLOEXEC one too, but be explicit

	pid_t pid = fork();
	if (pid == 0) {
		struct ctx child;
		init(&child);
		VkSemaphore cs = timeline(&child, 0);
		VkImportSemaphoreFdInfoKHR ii = {VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
		ii.semaphore = cs;
		ii.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
		ii.fd = keep;
		CHECK(child.importFd(child.dev, &ii));
		CHECK(host_wait(&child, cs, 5, 5000000000ull));
		printf("child: saw 5 from the parent's GPU submit\n");
		submit(&child, cs, 5, cs, 10);
		vkQueueWaitIdle(child.queue);
		uint64_t v = 0;
		child.value(child.dev, cs, &v);
		printf("child: counter now %llu\n", (unsigned long long)v);
		vkDestroySemaphore(child.dev, cs, NULL);
		vkDestroyDevice(child.dev, NULL);
		fflush(stdout);
		_exit(v == 10 ? 0 : 2);
	}

	usleep(200000);
	submit(&parent, VK_NULL_HANDLE, 0, s, 5);
	VkResult r = host_wait(&parent, s, 10, 5000000000ull);
	int status = 0;
	waitpid(pid, &status, 0);
	printf("parent: wait for 10 -> %d, child exit %d\n", r, WEXITSTATUS(status));
	vkDestroySemaphore(parent.dev, s, NULL);
	vkDestroyDevice(parent.dev, NULL);
	vkDestroyInstance(parent.inst, NULL);
	bool ok = r == VK_SUCCESS && WIFEXITED(status) && WEXITSTATUS(status) == 0;
	printf(ok ? "PASS\n" : "FAIL\n");
	return ok ? 0 : 1;
}
