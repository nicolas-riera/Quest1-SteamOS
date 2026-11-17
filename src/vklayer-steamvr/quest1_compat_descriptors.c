// quest1_compat_descriptors: buffers and descriptor sets, followed for the render pass merge.
// Part of quest1_compat.c (included by it, in this order: one translation unit).

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
