// test_readback: CPU read and write speed of each host-visible memory type (8 MB buffer, memcpy), and the kgsl cache
// mode the driver gave it (/sys/kernel/debug/kgsl/proc/<pid>/mem, 5th flag: '-' write-combine, 'b' write-back).
// Build on the headset: gcc -O2 -o test_readback test_readback.c -ldl
// Run like test_memtypes (as steamos, LD_LIBRARY_PATH=/usr/local/lib/quest1-vk:/opt/hybris/lib, LD_PRELOAD libbionictls).
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(void)
{
	void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
	if (!lib) { fprintf(stderr, "%s\n", dlerror()); return 1; }
	PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	VkInstance inst;
	if (((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &inst)) return 1;
	uint32_t n = 1;
	VkPhysicalDevice pd;
	((PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices"))(inst, &n, &pd);
	VkPhysicalDeviceMemoryProperties mp;
	((PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties"))(pd, &mp);
	float prio = 1;
	VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	VkDevice dev;
	if (((PFN_vkCreateDevice)gipa(inst, "vkCreateDevice"))(pd, &dci, NULL, &dev)) return 1;
	PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
	PFN_vkAllocateMemory alloc = (PFN_vkAllocateMemory)gdpa(dev, "vkAllocateMemory");
	PFN_vkMapMemory map = (PFN_vkMapMemory)gdpa(dev, "vkMapMemory");
	PFN_vkInvalidateMappedMemoryRanges inval = (PFN_vkInvalidateMappedMemoryRanges)gdpa(dev, "vkInvalidateMappedMemoryRanges");
	const size_t size = 8 << 20;
	char *host = malloc(size);
	memset(host, 1, size);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
		VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
		if (!(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
			continue;
		VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, size, i};
		VkDeviceMemory mem;
		void *p;
		if (alloc(dev, &mai, NULL, &mem) || map(dev, mem, 0, size, 0, &p)) {
			printf("type %u flags 0x%x: allocation or map failed\n", i, f);
			continue;
		}
		memset(p, 2, size);
		double t0 = now();
		for (int k = 0; k < 4; k++)
			memcpy(host, p, size);
		double t1 = now();
		for (int k = 0; k < 4; k++)
			memcpy(p, host, size);
		double t2 = now();
		VkMappedMemoryRange r = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, mem, 0, VK_WHOLE_SIZE};
		double t3 = now();
		inval(dev, 1, &r);
		double t4 = now();
		printf("type %u flags 0x%x: read %.0f MB/s, write %.0f MB/s, invalidate %.2f ms\n", i, f,
		       4 * 8 / (t1 - t0), 4 * 8 / (t2 - t1), (t4 - t3) * 1e3);
	}
	char cmd[128];
	snprintf(cmd, sizeof(cmd), "awk '$3 == 8388608 {print $5}' /sys/kernel/debug/kgsl/proc/%d/mem", getpid());
	printf("kgsl flags of the 8 MB allocations, in order:\n");
	fflush(stdout);
	system(cmd);
	return 0;
}
