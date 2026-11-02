// test_memtypes: prints the memory types and, for buffers of several usages, which types they may use.
// Cross-build: aarch64-linux-gnu-gcc -I<Vulkan-Headers>/include -o test_memtypes test_memtypes.c -ldl
// Run on the headset like test_timeline (LD_LIBRARY_PATH=/usr/local/lib/quest1-vk:/opt/hybris/lib, hybris env).
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
	void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
	if (!lib) { fprintf(stderr, "%s\n", dlerror()); return 1; }
	PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
	VkInstance inst;
	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	if (((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &inst)) return 1;
	PFN_vkEnumeratePhysicalDevices ep = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
	PFN_vkGetPhysicalDeviceMemoryProperties mpf = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
	PFN_vkCreateDevice cd = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
	uint32_t n = 1;
	VkPhysicalDevice pd;
	ep(inst, &n, &pd);
	VkPhysicalDeviceMemoryProperties mp;
	mpf(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		printf("type %u: flags 0x%x heap %u\n", i, mp.memoryTypes[i].propertyFlags, mp.memoryTypes[i].heapIndex);
	float prio = 1;
	VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qi};
	VkDevice dev;
	if (cd(pd, &dci, NULL, &dev)) return 2;
	PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
	PFN_vkCreateBuffer cb = (PFN_vkCreateBuffer)gdpa(dev, "vkCreateBuffer");
	PFN_vkGetBufferMemoryRequirements gr = (PFN_vkGetBufferMemoryRequirements)gdpa(dev, "vkGetBufferMemoryRequirements");
	uint32_t usages[] = {0x13, 0x1, 0x2, 0x10, 0x20, 0x40, 0x80, 0x100, 0x3, 0x1ff};
	for (unsigned u = 0; u < sizeof(usages) / sizeof(usages[0]); u++) {
		VkBufferCreateInfo bc = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 400, usages[u], VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
		VkBuffer b;
		if (cb(dev, &bc, NULL, &b)) { printf("usage 0x%x: create failed\n", usages[u]); continue; }
		VkMemoryRequirements mr;
		gr(dev, b, &mr);
		printf("buffer usage 0x%x: size %llu align %llu types 0x%x\n", usages[u], (unsigned long long)mr.size, (unsigned long long)mr.alignment, mr.memoryTypeBits);
	}
	return 0;
}
