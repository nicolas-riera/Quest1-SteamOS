// quest1_scanout_trace: debug traces of vrcompositor's render passes and GPU timestamps per pass.
// Part of quest1_scanout.c (included by it, in this order: one translation unit with quest1_compat.c).

// debug: `touch /tmp/quest1-rptrace` logs the render passes, viewports, scissors and dispatches
// recorded during the next frames (how vrcompositor draws into the display image)
static volatile int qs_trace_left;

static void qs_trace_arm(void)
{
	if (unlink("/tmp/quest1-rptrace") != 0)
		return;
	qs_trace_left = 3000;
}

// debug: `runuser -u steamos -- touch /tmp/quest1-gputime` logs the GPU time of each render pass
// recorded during the next frame (timestamps after each pass; the time also covers the commands
// between passes). Ticks of the Adreno always-on counter, 19.2 MHz.
#define QS_TS_SLOTS 1024
#define QS_TS_PER_CB 128
static struct
{
	VkDevice device;
	VkQueryPool pool;
	int state; //!< 0 idle, 1 instrumenting recordings, 2 waiting for the results
	unsigned frames, next_slot, ncbs;
	struct
	{
		VkCommandBuffer cb;
		unsigned first, count, indices;
		VkRect2D area;
		uint64_t rp;
	} cbs[8];
	char label[QS_TS_SLOTS][224];
} qs_ts;
static pthread_mutex_t qs_ts_lock = PTHREAD_MUTEX_INITIALIZER;
static void obj_desc_copy(uint64_t handle, char *out, size_t n); // quest1_compat.c

//! Call with qs_ts_lock held: the instrumented entry of `cb`, if any (or a new one when `add`).
static int qs_ts_entry(VkCommandBuffer cb, bool add)
{
	for (unsigned i = 0; i < qs_ts.ncbs; i++)
		if (qs_ts.cbs[i].cb == cb)
			return (int)i;
	if (!add || qs_ts.state != 1 || qs_ts.ncbs == sizeof(qs_ts.cbs) / sizeof(qs_ts.cbs[0]) ||
	    qs_ts.next_slot + QS_TS_PER_CB > QS_TS_SLOTS)
		return -1;
	struct device *d = device_of(cb);
	if (!qs_ts.pool) {
		PFN_vkCreateQueryPool create = (PFN_vkCreateQueryPool)d->gdpa(d->handle, "vkCreateQueryPool");
		VkQueryPoolCreateInfo ci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
		ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
		ci.queryCount = QS_TS_SLOTS;
		if (create(d->handle, &ci, NULL, &qs_ts.pool) != VK_SUCCESS)
			return -1;
		qs_ts.device = d->handle;
	}
	unsigned i = qs_ts.ncbs++;
	qs_ts.cbs[i].cb = cb;
	qs_ts.cbs[i].first = qs_ts.next_slot;
	qs_ts.cbs[i].count = 0;
	qs_ts.next_slot += QS_TS_PER_CB;
	PFN_vkCmdResetQueryPool reset = (PFN_vkCmdResetQueryPool)d->gdpa(d->handle, "vkCmdResetQueryPool");
	reset(cb, qs_ts.pool, qs_ts.cbs[i].first, QS_TS_PER_CB);
	return (int)i;
}

//! Call with qs_ts_lock held, outside a render pass for the first stamp of a command buffer.
static void qs_ts_stamp(VkCommandBuffer cb, int e, const char *label)
{
	if (e < 0 || qs_ts.cbs[e].count >= QS_TS_PER_CB)
		return;
	static PFN_vkCmdWriteTimestamp write;
	if (!write)
		write = (PFN_vkCmdWriteTimestamp)device_of(cb)->gdpa(device_of(cb)->handle, "vkCmdWriteTimestamp");
	unsigned slot = qs_ts.cbs[e].first + qs_ts.cbs[e].count++;
	write(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qs_ts.pool, slot);
	snprintf(qs_ts.label[slot], sizeof(qs_ts.label[slot]), "%s", label);
}

//! Once per displayed frame: arms the instrumentation, then logs the results.
static void rm_stats(void); // quest1_rpmerge.c
static void qh_stats(void); // quest1_hud.c

static void qs_ts_frame(void)
{
	rm_stats();
	qh_stats();
	pthread_mutex_lock(&qs_ts_lock);
	if (qs_ts.state == 0 && unlink("/tmp/quest1-gputime") == 0) {
		qs_ts.state = 1;
		qs_ts.frames = 0;
		qs_ts.ncbs = 0;
		qs_ts.next_slot = 0;
	} else if (qs_ts.state == 1 && ++qs_ts.frames >= 2) {
		qs_ts.state = 2;
		qs_ts.frames = 0;
	} else if (qs_ts.state == 2 && ++qs_ts.frames >= 8) {
		struct device *d = NULL;
		PFN_vkGetQueryPoolResults get = NULL;
		for (unsigned i = 0; i < qs_ts.ncbs; i++) {
			if (!get) {
				d = device_of(qs_ts.cbs[i].cb);
				get = (PFN_vkGetQueryPoolResults)d->gdpa(qs_ts.device, "vkGetQueryPoolResults");
			}
			uint64_t ts[QS_TS_PER_CB];
			unsigned n = qs_ts.cbs[i].count, first = qs_ts.cbs[i].first;
			if (n < 2 || get(qs_ts.device, qs_ts.pool, first, n, sizeof(ts), ts, sizeof(uint64_t),
			                 VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
				LOG("gputime: cb %p: %u stamps, no results\n", (void *)qs_ts.cbs[i].cb, n);
				continue;
			}
			LOG("gputime: cb %p: %.2f ms in %u passes\n", (void *)qs_ts.cbs[i].cb, (ts[n - 1] - ts[0]) / 19200.0, n - 1);
			for (unsigned k = 1; k < n; k++)
				LOG("gputime:   %6.3f ms %s\n", (ts[k] - ts[k - 1]) / 19200.0, qs_ts.label[first + k]);
		}
		qs_ts.state = 0;
	}
	pthread_mutex_unlock(&qs_ts_lock);
}

static void qs_ts_begin_pass(VkCommandBuffer cb, const VkRect2D *area, uint64_t rp)
{
	if (qs_ts.state != 1)
		return;
	pthread_mutex_lock(&qs_ts_lock);
	int e = qs_ts_entry(cb, true);
	if (e >= 0) {
		if (qs_ts.cbs[e].count == 0)
			qs_ts_stamp(cb, e, "start");
		qs_ts.cbs[e].area = *area;
		qs_ts.cbs[e].rp = rp;
		qs_ts.cbs[e].indices = 0;
	}
	pthread_mutex_unlock(&qs_ts_lock);
}

static void qs_ts_draw(VkCommandBuffer cb, uint32_t n)
{
	if (qs_ts.state != 1)
		return;
	pthread_mutex_lock(&qs_ts_lock);
	int e = qs_ts_entry(cb, false);
	if (e >= 0)
		qs_ts.cbs[e].indices += n;
	pthread_mutex_unlock(&qs_ts_lock);
}

static void qs_ts_end_pass(VkCommandBuffer cb)
{
	if (qs_ts.state != 1)
		return;
	pthread_mutex_lock(&qs_ts_lock);
	int e = qs_ts_entry(cb, false);
	if (e >= 0) {
		char label[224], rpd[160] = "";
		const VkRect2D *a = &qs_ts.cbs[e].area;
		obj_desc_copy(qs_ts.cbs[e].rp, rpd, sizeof(rpd));
		snprintf(label, sizeof(label), "pass %ux%u@%d,%d, %u vertices; %s", a->extent.width, a->extent.height,
		         a->offset.x, a->offset.y, qs_ts.cbs[e].indices, rpd);
		qs_ts_stamp(cb, e, label);
	}
	pthread_mutex_unlock(&qs_ts_lock);
}

#define QS_TRACE_NEXT(type, fn) \
	static type next; \
	if (!next) \
		next = (type)device_of(cb)->gdpa(device_of(cb)->handle, fn);
