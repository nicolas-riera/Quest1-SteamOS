// quest1_display_vblank: vblank events (VK_EXT_display_control) and present waits.
// Part of quest1_display.c (included by it, in this order: one translation unit).

// --- vblank events (VK_EXT_display_control) ---------------------------------------------------------------
// vrcompositor paces its frames on vkRegisterDisplayEventEXT fences and vkGetSwapchainCounterEXT. The
// simulated display ticks on the panels' real vsync, which Monado publishes (struct q1d_vsync; driver_quest1's
// GetTimeSinceLastVsync reads it too), and at the mode's rate on CLOCK_MONOTONIC when there is none (no Monado, panels
// off); a thread signals the registered fences
// at every tick (an empty submission, the blob cannot signal a fence from the CPU otherwise). It signals the fence
// once the GPU has done the work queued before it, often after the tick: tried on 2026-11-08, signalling the
// fences from the CPU at the tick (the layer answering vkWaitForFences/vkGetFenceStatus on them) cut the
// present-to-rendered time from ~17 to ~14 ms but cost 1-1.5 fps (69.6 against 71.0, alternated A/B): vrcompositor's
// running start then leaves its render thread less slack before the next vblank.

//
// The fences come from a small pool (2026-11-12, descriptor leak). vrcompositor's vblank thread registers two display
// events a frame. The first it waits for at once: the blob answers VK_TIMEOUT immediately for a fence nothing has
// been submitted with yet, and vrcompositor then never names that fence in a wait again. That it does not destroy it
// either is inferred, not observed (frame trace of 2026-11-12: the blob never handed the address of such a fence out
// again, 0 of 182, and did so for every other fence, 194 of 194; the unpatched layer did not see vkDestroyFence).
// The tick below submits the abandoned fence all the same, and on this blob a fence submitted behind work in flight
// holds an anon_inode:sync_fence descriptor until it is destroyed or reset (test_fdleak AB2, AB5, RG8): one descriptor
// a frame, RLIMIT_NOFILE (524288) after 2 h 15 min, and vrcompositor dies with "Too many open files".
// So the layer keeps these fences: a slot goes back into use once the application has let go of it (vkDestroyFence,
// or a wait of up to 100 ms that timed out before the tick had submitted it) and the fence has signalled; it is
// reset then. Pool fences remain allocated until device destruction, but reuse changes their event identity and
// signal state; compatibility relies on this vrcompositor not using abandoned or age-reclaimed handles again.
// The application's allocator, if it passes one, is not used for them. When no slot can be
// had, a fence of its own as before.
// "evnopool" in /tmp/quest1-layer (read once a second) or QUEST1_EVENT_FENCE_POOL_OFF: no pool.
#define QD_EV_POOL 16
#define QD_EV_AGE_NS 5000000000ull
struct qd_evslot
{
	struct device *d;
	VkFence fence;  //!< VK_NULL_HANDLE: slot not made yet
	bool held;      //!< the application has it: registered, neither destroyed nor abandoned
	bool pending;   //!< registered, the tick has not submitted it yet
	bool submitted; //!< submitted since its last reset: usable again once signalled, after a reset
	uint64_t submitted_at; //!< when the tick submitted it
};

struct qd_event
{
	struct device *d;
	VkQueue queue;
	VkFence fence;
	struct qd_evslot *slot; //!< NULL: a fence of its own, not from the pool
};
static struct qd_event qd_events[16];
static uint32_t qd_event_count;
static pthread_mutex_t qd_event_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct qd_evslot qd_evpool[QD_EV_POOL]; //!< under qd_event_mutex
static _Atomic uint32_t qd_evpool_slots;        //!< slots made so far (0: nothing to look up)
static struct
{
	uint64_t since;
	unsigned registered, pooled, abandoned, destroyed, aged;
} qd_evstat; //!< under qd_event_mutex

static bool qd_evpool_on(void)
{
	static _Atomic uint64_t checked;
	static _Atomic bool on = true;
	if (getenv("QUEST1_EVENT_FENCE_POOL_OFF"))
		return false;
	uint64_t t = now_ns();
	if (t - checked > 1000000000ull) {
		checked = t;
		char b[64] = {0};
		FILE *f = fopen("/tmp/quest1-layer", "r");
		if (f) {
			b[fread(b, 1, sizeof(b) - 1, f)] = 0;
			fclose(f);
		}
		bool v = strstr(b, "evnopool") == NULL;
		if (v != on)
			LOG("display: event fences %s\n", v ? "from the pool" : "NOT from the pool (evnopool)");
		on = v;
	}
	return on;
}

//! A slot for a new display event, NULL when none can be had. Under qd_event_mutex.
static struct qd_evslot *qd_evslot_take(struct device *d)
{
	struct qd_evslot *empty = NULL;
	for (int i = 0; i < QD_EV_POOL; i++) {
		struct qd_evslot *s = &qd_evpool[i];
		if (s->fence == VK_NULL_HANDLE) {
			if (!empty)
				empty = s;
			continue;
		}
		if (s->d != d || s->held || s->pending)
			continue;
		if (s->submitted) {
			// in flight until it has signalled; a reset fence holds no descriptor
			if (d->GetFenceStatus(d->handle, s->fence) != VK_SUCCESS)
				continue;
			if (d->ResetFences(d->handle, 1, &s->fence) != VK_SUCCESS)
				continue;
			s->submitted = false;
		}
		return s;
	}
	if (empty) {
		VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		VkFence f = VK_NULL_HANDLE;
		if (d->CreateFence(d->handle, &fci, NULL, &f) != VK_SUCCESS)
			return NULL;
		*empty = (struct qd_evslot){.d = d, .fence = f};
		if (atomic_fetch_add(&qd_evpool_slots, 1) == 0)
			LOG("display: event fences come from a pool of %d (descriptor leak, 2026-11-12)\n", QD_EV_POOL);
		return empty;
	}
	// Every slot is with the application. One it has held since its fence was submitted QD_EV_AGE_NS ago, and whose
	// fence has signalled, is taken back: that vblank is long past. It covers a fence left behind in a way other
	// than the one above (seen in test_fdleak EV5 when the tick fell between the registration and the wait: 4 slots
	// in about 5000 frames); without it 16 of those would end the pool and the leak would be back.
	uint64_t t = now_ns();
	for (int i = 0; i < QD_EV_POOL; i++) {
		struct qd_evslot *s = &qd_evpool[i];
		if (s->d != d || !s->held || s->pending || !s->submitted || t - s->submitted_at < QD_EV_AGE_NS)
			continue;
		if (d->GetFenceStatus(d->handle, s->fence) != VK_SUCCESS)
			continue;
		if (d->ResetFences(d->handle, 1, &s->fence) != VK_SUCCESS)
			continue;
		s->submitted = false;
		s->held = false;
		qd_evstat.aged++;
		return s;
	}
	return NULL;
}

//! Once a minute, what became of the display events. Under qd_event_mutex.
static void qd_evpool_count(bool pooled)
{
	uint64_t t = now_ns();
	qd_evstat.registered++;
	qd_evstat.pooled += pooled;
	if (!qd_evstat.since)
		qd_evstat.since = t;
	if (t - qd_evstat.since < 60000000000ull)
		return;
	unsigned held = 0;
	for (int i = 0; i < QD_EV_POOL; i++)
		held += qd_evpool[i].fence != VK_NULL_HANDLE && qd_evpool[i].held;
	LOG("display: event fences in %.0f s: %u registered, %u from the pool (%u slots, %u held now), %u abandoned "
		"after a wait that timed out, %u destroyed by the application, %u taken back after %d s\n",
		(t - qd_evstat.since) / 1e9, qd_evstat.registered, qd_evstat.pooled, (unsigned)atomic_load(&qd_evpool_slots),
		held, qd_evstat.abandoned, qd_evstat.destroyed, qd_evstat.aged, (int)(QD_EV_AGE_NS / 1000000000ull));
	qd_evstat.since = t;
	qd_evstat.registered = qd_evstat.pooled = qd_evstat.abandoned = qd_evstat.destroyed = qd_evstat.aged = 0;
}

//! vkWaitForFences with a timeout of up to 100 ms on one fence. For a pool fence the tick has not submitted yet the
//! answer is given here, under the lock the tick submits under, so that "timed out before the tick" and the slot's
//! state cannot disagree (a slot wrongly left "held" would be lost, and with all 16 lost the leak is back).
//! For n=1 and timeout <=100 ms, a pending pool fence is polled with timeout zero under the event lock; this is
//! consistent with 20 reset-fence waits at 20 ms and 202 traced fast timeouts, but the trace records neither n nor
//! timeout and does not prove equivalence for every call. The application then
//! abandons the fence (see above): its slot is free once the tick has submitted it and it has signalled.
//! false: not such a fence, the caller waits as before.
static bool qd_event_wait_pending(struct device *d, VkFence fence, VkResult *r)
{
	if (!atomic_load(&qd_evpool_slots))
		return false;
	bool answered = false;
	pthread_mutex_lock(&qd_event_mutex);
	for (int i = 0; i < QD_EV_POOL; i++) {
		struct qd_evslot *s = &qd_evpool[i];
		if (s->fence != fence || s->d != d)
			continue;
		if (s->pending) {
			*r = d->WaitForFences(d->handle, 1, &fence, VK_TRUE, 0);
			if (*r == VK_TIMEOUT && s->held) {
				s->held = false;
				qd_evstat.abandoned++;
			}
			answered = true;
		}
		break;
	}
	pthread_mutex_unlock(&qd_event_mutex);
	return answered;
}

//! vkDestroyFence: a fence of the pool is kept, its slot is free once it has signalled.
static VKAPI_ATTR void VKAPI_CALL qd_DestroyFence(VkDevice device, VkFence fence, const VkAllocationCallbacks *alloc)
{
	struct device *d = device_of(device);
	ft("destroyfence", (int64_t)(uint64_t)fence);
	if (fence != VK_NULL_HANDLE && atomic_load(&qd_evpool_slots)) {
		bool ours = false;
		pthread_mutex_lock(&qd_event_mutex);
		for (int i = 0; i < QD_EV_POOL && !ours; i++) {
			struct qd_evslot *s = &qd_evpool[i];
			if (s->fence == fence && s->d == d) {
				ours = true;
				if (s->held) {
					s->held = false;
					qd_evstat.destroyed++;
				}
			}
		}
		pthread_mutex_unlock(&qd_event_mutex);
		if (ours)
			return;
	}
	d->DestroyFence(device, fence, alloc);
}

//! vkDestroyDevice: its events are dropped (the tick must not submit on it any more) and its pool fences destroyed.
static void qd_events_device_gone(struct device *d)
{
	pthread_mutex_lock(&qd_event_mutex);
	uint32_t k = 0;
	for (uint32_t i = 0; i < qd_event_count; i++)
		if (qd_events[i].d != d)
			qd_events[k++] = qd_events[i];
	qd_event_count = k;
	for (int i = 0; i < QD_EV_POOL; i++) {
		struct qd_evslot *s = &qd_evpool[i];
		if (s->fence == VK_NULL_HANDLE || s->d != d)
			continue;
		if (s->submitted)
			d->WaitForFences(d->handle, 1, &s->fence, VK_TRUE, 50000000ull);
		d->DestroyFence(d->handle, s->fence, NULL);
		*s = (struct qd_evslot){0};
	}
	pthread_mutex_unlock(&qd_event_mutex);
}
static pthread_once_t qd_vblank_once = PTHREAD_ONCE_INIT;
static _Atomic uint64_t qd_vblanks; //!< vblanks so far (vkGetSwapchainCounterEXT)

static uint64_t qd_period_ns(void) { return 1000000000000ull / qd_mhz; }

static uint64_t qd_vblank_count(void) { return atomic_load(&qd_vblanks); }

//! Monado's page with the panels' real vsync (quest1_direct_proto.h), NULL until mapped
static const struct q1d_vsync *qd_vsync_page;

//! The panels' last vsync and period from Monado, false when there is none (no Monado, or the panels are off: no
//! vsync for 0.5 s).
static bool qd_real_vsync(int64_t *vsync, int64_t *period)
{
	if (!qd_vsync_page) {
		static uint64_t tried;
		uint64_t t = now_ns();
		if (tried && t - tried < 1000000000ull)
			return false;
		tried = t;
		int fd = open(Q1D_VSYNC_FILE, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			return false;
		struct stat st;
		void *m = MAP_FAILED;
		if (fstat(fd, &st) == 0 && st.st_size >= (off_t)sizeof(struct q1d_vsync)) // smaller: SIGBUS on read
			m = mmap(NULL, sizeof(struct q1d_vsync), PROT_READ, MAP_SHARED, fd, 0);
		close(fd);
		if (m == MAP_FAILED)
			return false;
		qd_vsync_page = m;
		LOG("display: vblanks follow the panels' vsync (%s)\n", Q1D_VSYNC_FILE);
	}
	// Monado writes it right at each vsync, often just when this thread reads it: retry, pausing after a few tries
	for (int i = 0; i < 200; i++) {
		if (i >= 20)
			nanosleep(&(struct timespec){0, 10000}, NULL);
		uint32_t s0 = __atomic_load_n(&qd_vsync_page->seq, __ATOMIC_ACQUIRE);
		if (s0 & 1)
			continue;
		int64_t v = __atomic_load_n(&qd_vsync_page->vsync_ns, __ATOMIC_RELAXED);
		int64_t p = __atomic_load_n(&qd_vsync_page->period_ns, __ATOMIC_RELAXED);
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (__atomic_load_n(&qd_vsync_page->seq, __ATOMIC_RELAXED) != s0)
			continue;
		if (v <= 0 || p < 12000000 || p > 16000000 || (int64_t)now_ns() - v > 500000000)
			return false;
		*vsync = v;
		*period = p;
		return true;
	}
	return false;
}

//! The calling thread made realtime if it is not already (SteamVR runs with CAP_SYS_NICE, as on desktop Linux).
//! vrcompositor's VulkanVblankThread (it calls vkGetSwapchainCounterEXT) polls its vblank fence then sleeps until
//! the next one with SCHED_OTHER: with the CPUs loaded (load ~11 on 8 cores: Steam, Meta's tracking) it woke up to
//! 7 ms late, its RenderThread then aimed at the vblank after and a frame was missed (~1 a second with the Steam
//! library open, 2026-11-08 frame trace). Its RenderThread is SCHED_RR too; all of them at Q1_RT_MAX (quest1_compat.c),
//! below Monado's vsync thread and the MDP's display kernel thread.
#define qd_realtime q1_realtime

static void *qd_vblank_thread(void *arg)
{
	qd_realtime("display: the layer's vblank thread", Q1_RT_MAX);
	int64_t period = (int64_t)qd_period_ns();
	int64_t last = (int64_t)now_ns();
	bool locked = false;
	for (;;) {
		int64_t vs, per, next;
		bool real = qd_real_vsync(&vs, &per);
		if (real) {
			// the first vsync of the panels at least half a period after the last tick
			period = per;
			int64_t from = last + per / 2;
			next = vs + (from > vs ? (from - vs + per - 1) / per : 0) * per;
		} else {
			next = last + period;
		}
		if (real != locked) {
			locked = real;
			LOG("display: vblanks %s\n", real ? "on the panels' vsync" : "on the layer's own clock (no vsync from Monado)");
		}
		int64_t now = (int64_t)now_ns();
		while (next + period <= now) // woke up late: those vblanks are gone
			next += period;
		struct timespec ts = {(time_t)(next / 1000000000ll), (long)(next % 1000000000ll)};
		while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
			;
		int64_t n = (next - last + period / 2) / period;
		atomic_fetch_add(&qd_vblanks, n > 0 ? (uint64_t)n : 1);
		last = next;
		ft_tick((uint64_t)next);
		pthread_mutex_lock(&qd_event_mutex);
		ft("tick", qd_event_count);
		if (qd_event_count)
			atomic_store(&qd_frame_tick, (uint64_t)next);
		for (uint32_t i = 0; i < qd_event_count; i++) {
			struct qd_event *e = &qd_events[i];
			pthread_mutex_lock(&e->d->queue_mutex);
			VkResult sr = e->d->QueueSubmit(e->queue, 0, NULL, e->fence);
			pthread_mutex_unlock(&e->d->queue_mutex);
			if (e->slot) {
				e->slot->pending = false;
				e->slot->submitted = sr == VK_SUCCESS;
				e->slot->submitted_at = now_ns();
			}
		}
		qd_event_count = 0;
		pthread_mutex_unlock(&qd_event_mutex);
	}
	return NULL;
}

static void qd_vblank_start(void)
{
	pthread_once(&qd_once, qd_init);
	atomic_store(&qd_vblanks, now_ns() / qd_period_ns());
	pthread_t t;
	pthread_create(&t, NULL, qd_vblank_thread, NULL);
	pthread_detach(t);
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_RegisterDisplayEventEXT(VkDevice device, VkDisplayKHR display,
                                                               const VkDisplayEventInfoEXT *info,
                                                               const VkAllocationCallbacks *alloc, VkFence *fence)
{
	qd_trace("event");
	pthread_once(&qd_vblank_once, qd_vblank_start);
	struct device *d = device_of(device);
	PFN_vkGetDeviceQueue get_queue = (PFN_vkGetDeviceQueue)d->gdpa(device, "vkGetDeviceQueue");
	VkQueue queue;
	get_queue(device, 0, 0, &queue);
	if (qd_evpool_on()) {
		pthread_mutex_lock(&qd_event_mutex);
		struct qd_evslot *s = qd_event_count < 16 ? qd_evslot_take(d) : NULL;
		if (s) {
			s->held = true;
			s->pending = true;
			qd_events[qd_event_count++] = (struct qd_event){d, queue, s->fence, s};
			*fence = s->fence;
			qd_evpool_count(true);
		}
		pthread_mutex_unlock(&qd_event_mutex);
		if (s) {
			ft("event", (int64_t)(uint64_t)*fence);
			return VK_SUCCESS;
		}
	}
	VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkResult r = d->CreateFence(device, &fci, alloc, fence);
	if (r != VK_SUCCESS)
		return r;
	ft("event", (int64_t)(uint64_t)*fence);
	pthread_mutex_lock(&qd_event_mutex);
	if (qd_event_count < 16)
		qd_events[qd_event_count++] = (struct qd_event){d, queue, *fence, NULL};
	qd_evpool_count(false);
	pthread_mutex_unlock(&qd_event_mutex);
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_GetSwapchainCounterEXT(VkDevice device, VkSwapchainKHR swapchain,
                                                              VkSurfaceCounterFlagBitsEXT counter, uint64_t *value)
{
	qd_trace("counter");
	ft("counter", 0);
	static __thread bool rt;
	if (!rt) {
		rt = true;
		qd_realtime("display: vrcompositor's vblank thread", Q1_RT_MAX);
	}
	pthread_once(&qd_vblank_once, qd_vblank_start);
	*value = qd_vblank_count();
	return VK_SUCCESS;
}

//! Present waits return at once: vrcompositor waits for the frame it has just presented (0 frames behind),
//! so waiting for it to be latched serializes its CPU, the GPU and the vsync (2026-11-02, A/B in the headset:
//! head still 19 -> 34 fps, head moving 18 -> 21 fps, "failed to wait for present" 250 -> 0 per 22 s). It is
//! still paced by its vblank fences and by the swapchain images the driver gives back. "pwwait" in
//! /tmp/quest1-layer (read once a second) restores the old wait.
static bool qd_present_wait_now(void)
{
	static _Atomic uint64_t checked;
	static _Atomic bool now = true;
	uint64_t t = now_ns();
	if (t - checked > 1000000000ull) {
		checked = t;
		char b[64] = {0};
		FILE *f = fopen("/tmp/quest1-layer", "r");
		if (f) {
			b[fread(b, 1, sizeof(b) - 1, f)] = 0;
			fclose(f);
		}
		now = strstr(b, "pwwait") == NULL;
	}
	return now;
}

static VKAPI_ATTR VkResult VKAPI_CALL qd_WaitForPresentKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t id,
                                                          uint64_t timeout)
{
	qd_trace("waitpresent");
	// vrcompositor times its frames on these returns: they must come when the frame is latched for
	// display (the driver's QD_DISPLAYED), at the real display's phase, not at once
	struct qd_swapchain *sc = qd_swapchain_of(swapchain);
	if (!sc || getenv("QUEST1_PRESENT_WAIT_NOW") || qd_present_wait_now())
		return VK_SUCCESS;
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	if (timeout != UINT64_MAX) {
		uint64_t ns = deadline.tv_nsec + timeout % 1000000000ull;
		deadline.tv_sec += timeout / 1000000000ull + ns / 1000000000ull;
		deadline.tv_nsec = ns % 1000000000ull;
	} else {
		deadline.tv_sec += 1; // the driver may never latch it (receiver gone, session idle)
	}
	uint64_t t0 = now_ns();
	VkResult r = VK_SUCCESS;
	pthread_mutex_lock(&sc->mutex);
	uint64_t latest = sc->present_ids[sc->frame % 64].id;
	qd_stats.wait_depth += latest > id ? latest - id : 0;
	while (sc->displayed_id < id && sc->sock >= 0 && !sc->stopping) {
		if (pthread_cond_timedwait(&sc->cond, &sc->mutex, &deadline) == ETIMEDOUT) {
			r = timeout == UINT64_MAX ? VK_SUCCESS : VK_TIMEOUT;
			break;
		}
	}
	pthread_mutex_unlock(&sc->mutex);
	qd_stats.present_wait += now_ns() - t0;
	qd_stats.present_waits++;
	return r;
}
