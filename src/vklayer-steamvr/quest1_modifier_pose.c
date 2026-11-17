// quest1_modifier_pose: the stream frame's pose as vrcompositor's distortion reads it: a snapshot per command buffer.
// Part of quest1_modifier.c (included by it, in this order: one translation unit).

// --- The stream frame's pose, as vrcompositor's distortion reads it
//
// vrcompositor re-projects vrlink's frame from the head pose the PC rendered it for to the panels' pose. It writes that
// pose (with the rest of its distortion's uniforms, 2800 bytes) into host memory at the start of each frame, and its
// distortion reads it there, through a uniform buffer in that memory, when the GPU runs the frame: one buffer for every
// frame. The GPU runs a frame well after vrcompositor recorded it (busy: ~12 of 14 ms), and by then vrcompositor has
// often written the next frame's pose there. So whenever the pose changed from one of vrlink's frames to the next, a
// frame went out with the picture of one and the pose of the other: the stream jumped by that change (2026-11-15, the
// headset still and the pose sent to the PC stepped by 1.5°: 10-20 ms after each step nearly every frame was the whole
// step off, tools/headset-dev/stepsnap.py). On a head, whose pose changes at every frame, the whole stream shook: the
// trembling. vrlink's frames themselves are right (each decoded picture moves with its own pose, same submit).
// The fix is the double buffering vrcompositor leaves out: when a command buffer binds a descriptor set that reads a
// uniform buffer in that memory, the layer binds a copy of the set whose descriptor reads a snapshot of it taken there,
// in the command buffer's own staging (quest1_rpmerge.c: qsp_bind). The memory is found as the source of
// vrcompositor's 2800-byte copy (it copies the same bytes into a device buffer too) and mapped by the layer
// (vrcompositor writes it through no mapping the layer sees). `nostreampose` in /tmp/quest1-layer turns it off.
enum { QSP_BYTES = 2800 };

static struct
{
	pthread_mutex_t lock;
	VkDeviceMemory host; //!< the memory vrcompositor writes the pose into
	const uint8_t *map;  //!< the layer's mapping of it, NULL when it could not map it
	unsigned copies, other; //!< sets bound with a snapshot, sets that read the memory but could not be copied
	uint64_t log_ns;
} qsp = {PTHREAD_MUTEX_INITIALIZER};

static bool qsp_off(void)
{
	static _Atomic uint64_t checked;
	static _Atomic bool off;
	uint64_t t = now_ns();
	if (t - checked > 1000000000ull) {
		checked = t;
		char b[256] = {0};
		FILE *f = fopen("/tmp/quest1-layer", "r");
		if (f) {
			b[fread(b, 1, sizeof(b) - 1, f)] = 0;
			fclose(f);
		}
		off = strstr(b, "nostreampose") != NULL;
	}
	return off;
}

//! vkCmdCopyBuffer (quest1_compat.c) in vrcompositor: a copy of `size` bytes out of memory `smem`.
static void qsp_note_copy(VkDevice device, VkDeviceMemory smem, VkDeviceSize size)
{
	static int vrc = -1;
	if (vrc < 0)
		vrc = strcmp(program_invocation_short_name, "vrcompositor") == 0;
	if (!vrc || size != QSP_BYTES || !smem)
		return;
	pthread_mutex_lock(&qsp.lock);
	if (qsp.host != smem) {
		void *p = NULL;
		if (device_of(device)->MapMemory(device, smem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS)
			p = NULL;
		qsp.host = smem;
		qsp.map = p;
		LOG("stream: vrcompositor's distortion reads the stream's pose from memory %p: %s\n", (void *)smem,
		    p ? "each frame gets a copy of its own" : "it cannot be mapped, left as it is");
	}
	pthread_mutex_unlock(&qsp.lock);
}

//! vkFreeMemory: the mapping goes with the memory.
static void qsp_free_memory(VkDeviceMemory m)
{
	pthread_mutex_lock(&qsp.lock);
	if (qsp.host == m) {
		qsp.host = VK_NULL_HANDLE;
		qsp.map = NULL;
	}
	pthread_mutex_unlock(&qsp.lock);
}

//! For quest1_rpmerge.c's qsp_bind: the pose's memory and the layer's mapping of it, false when off or not found.
static bool qsp_host(VkDeviceMemory *mem, const uint8_t **map)
{
	if (qsp_off())
		return false;
	pthread_mutex_lock(&qsp.lock);
	*mem = qsp.host;
	*map = qsp.map;
	pthread_mutex_unlock(&qsp.lock);
	return *mem && *map;
}

//! qsp_bind's count, logged once a minute while it acts.
static void qsp_count(bool copied)
{
	pthread_mutex_lock(&qsp.lock);
	qsp.copies += copied;
	qsp.other += !copied;
	uint64_t t = now_ns();
	if (t - qsp.log_ns > 60000000000ull) {
		if (qsp.log_ns)
			LOG("stream: %u descriptor sets bound with a copy of the stream's pose in 60 s, %u left as they were\n",
			    qsp.copies, qsp.other);
		qsp.copies = qsp.other = 0;
		qsp.log_ns = t;
	}
	pthread_mutex_unlock(&qsp.lock);
}
