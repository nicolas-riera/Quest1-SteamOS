// quest1_compat_device: device creation and destruction: the dispatch table, queues, the worker.
// Part of quest1_compat.c (included by it, in this order: one translation unit).

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
	const char **exts = calloc(ci->enabledExtensionCount + 4, sizeof(char *));
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
	// direct scanout imports gralloc buffers (quest1_scanout.c), and so does a dma-buf import (quest1_dmabuf.c)
	if (!have_ahb)
		exts[ne++] = VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME;
	if (!have_foreign)
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
	GET(GetFenceStatus);
	GET(GetSemaphoreFdKHR);
	GET(ImportSemaphoreFdKHR);
	GET(CmdUpdateBuffer);
	GET(CmdBindIndexBuffer);
	GET(CmdCopyBuffer);
	GET(BindBufferMemory);
	GET(BindBufferMemory2);
	if (!d->BindBufferMemory2)
		d->BindBufferMemory2 = (PFN_vkBindBufferMemory2)gdpa(*out, "vkBindBufferMemory2KHR");
	GET(BindImageMemory);
	GET(BindImageMemory2);
	if (!d->BindImageMemory2)
		d->BindImageMemory2 = (PFN_vkBindImageMemory2)gdpa(*out, "vkBindImageMemory2KHR");
	GET(GetImageMemoryRequirements);
	GET(GetImageMemoryRequirements2);
	d->GetImageMemoryRequirements2KHR =
	    (PFN_vkGetImageMemoryRequirements2)gdpa(*out, "vkGetImageMemoryRequirements2KHR");
	GET(CreateImageView);
	GET(CreateSamplerYcbcrConversion);
	d->CreateSamplerYcbcrConversionKHR =
	    (PFN_vkCreateSamplerYcbcrConversion)gdpa(*out, "vkCreateSamplerYcbcrConversionKHR");
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
	qnv_prewarm(); // vrlink's frame buffers, seconds before the PC streams (quest1_modifier.c)
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
	qd_events_device_gone(d);
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
	I("vkDestroyFence", qd_DestroyFence)
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
	I("vkBindImageMemory", BindImageMemory)
	I("vkBindImageMemory2", BindImageMemory2)
	I("vkBindImageMemory2KHR", BindImageMemory2)
	I("vkGetImageSubresourceLayout", GetImageSubresourceLayout)
	I("vkGetMemoryFdPropertiesKHR", GetMemoryFdProperties)
	I("vkGetMemoryFdKHR", GetMemoryFd)
	I("vkGetImageDrmFormatModifierPropertiesEXT", GetImageDrmFormatModifierProperties)
	I("vkGetImageMemoryRequirements", GetImageMemoryRequirements)
	I("vkGetImageMemoryRequirements2", GetImageMemoryRequirements2)
	I("vkGetImageMemoryRequirements2KHR", GetImageMemoryRequirements2KHR)
	I("vkCreateImageView", CreateImageView)
	I("vkDestroyImage", DestroyImage)
	I("vkCmdPipelineBarrier", CmdPipelineBarrier) // (quest1_modifier.c, then quest1_scanout.c's when on)
	I("vkCreateSamplerYcbcrConversion", CreateSamplerYcbcrConversion)
	I("vkCreateSamplerYcbcrConversionKHR", CreateSamplerYcbcrConversionKHR)
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
