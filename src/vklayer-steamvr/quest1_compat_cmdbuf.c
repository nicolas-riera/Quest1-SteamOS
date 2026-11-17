// quest1_compat_cmdbuf: 32-bit indices bound as 16-bit, vkCmdUpdateBuffer through the layer's staging, memory maps.
// Part of quest1_compat.c (included by it, in this order: one translation unit).

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

//! The memory buffer b is bound to and where in it (quest1_rpmerge.c: qsp_bind), false if not known.
static bool u32_buf_mem(VkBuffer b, uint64_t *mem, uint64_t *off)
{
	bool found = false;
	pthread_mutex_lock(&u32_lock);
	for (unsigned i = 0; i < U32_BUFS && !found; i++)
		if (u32_bufs[i].buf == (uint64_t)b) {
			*mem = u32_bufs[i].mem;
			*off = u32_bufs[i].off;
			found = true;
		}
	pthread_mutex_unlock(&u32_lock);
	return found;
}

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
	qh_note_update(cb, size, data); // quest1_hud.c
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
	uint64_t smem = 0;
	for (unsigned i = 0; i < U32_BUFS && n == 1 && r[0].size == QSP_BYTES && !smem; i++)
		if (u32_bufs[i].buf == (uint64_t)src)
			smem = u32_bufs[i].mem;
	pthread_mutex_unlock(&u32_lock);
	if (smem) // the stream's pose (quest1_modifier.c)
		qsp_note_copy(device_of(cb)->handle, (VkDeviceMemory)smem, r[0].size);
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
	qsp_free_memory(m); // (quest1_modifier.c)
	device_of(device)->FreeMemory(device, m, alloc);
	qnv_free_memory(m); // exported NV12 memory: our gralloc buffer
}
