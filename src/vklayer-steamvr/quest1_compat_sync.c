// quest1_compat_sync: fence waits in steps and query results.
// Part of quest1_compat.c (included by it, in this order: one translation unit).

// --- fences ----------------------------------------------------------------------------------------------

// Long timeouts (UINT64_MAX in particular) make the blob return VK_TIMEOUT at once: wait in steps.
static VKAPI_ATTR VkResult VKAPI_CALL WaitForFences(VkDevice device, uint32_t n, const VkFence *fences, VkBool32 all,
                                                    uint64_t timeout)
{
	struct device *d = device_of(device);
	q1_rt_cap();
	const uint64_t step = 100000000ull; // 100 ms
	ft("waitfences", n ? (int64_t)(uint64_t)fences[0] : 0);
	if (timeout <= step) {
		VkResult r0;
		if (n != 1 || !qd_event_wait_pending(d, fences[0], &r0))
			r0 = d->WaitForFences(device, n, fences, all, timeout);
		ft("waited", r0);
		return r0;
	}
	uint64_t deadline = timeout == UINT64_MAX ? UINT64_MAX : now_ns() + timeout;
	for (;;) {
		VkResult r = d->WaitForFences(device, n, fences, all, step);
		if (r != VK_TIMEOUT || (deadline != UINT64_MAX && now_ns() >= deadline)) {
			ft("waited", r);
			return r;
		}
	}
}

// --- query results --------------------------------------------------------------------------------------
// vrcompositor reads its GPU timestamps with VK_QUERY_RESULT_WAIT_BIT right after it submits a frame, so its render
// thread sleeps until the GPU has finished that frame (the blob polls with usleep) and only then records the next
// one: CPU and GPU never overlap (2026-11-08: render thread 55 % of its time in that usleep, ~5 ms of CPU + ~12 ms
// of GPU a frame in series, 55 fps). The timestamps only feed its frame timing statistics. So a waiting read
// returns at once: the results if they are ready, else the last ones read for the same queries, while a helper
// thread waits for the real ones and keeps them for the next call (they are one frame old). The first read of a
// set of queries still waits. "qrwait" in /tmp/quest1-layer (read once a second) restores the plain wait.
#define QR_SLOTS 16
#define QR_MAX 1024
struct qr_slot
{
	VkDevice device;
	VkQueryPool pool;
	uint32_t first, count;
	VkDeviceSize stride;
	VkQueryResultFlags flags;
	size_t size;
	bool valid, pending;
	uint8_t data[QR_MAX];
};
static struct qr_slot qr_slots[QR_SLOTS];
static unsigned qr_next;
static pthread_mutex_t qr_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qr_cond = PTHREAD_COND_INITIALIZER;
static PFN_vkGetQueryPoolResults qr_real;
static bool qr_thread_started;
static _Atomic uint64_t qr_stat_cached, qr_stat_ready, qr_stat_waited;

static bool qr_plain_wait(void)
{
	static _Atomic uint64_t checked;
	static _Atomic bool plain;
	uint64_t t = now_ns();
	if (t - checked > 1000000000ull) {
		checked = t;
		char b[64] = {0};
		FILE *f = fopen("/tmp/quest1-layer", "r");
		if (f) {
			b[fread(b, 1, sizeof(b) - 1, f)] = 0;
			fclose(f);
		}
		plain = strstr(b, "qrwait") != NULL;
	}
	return plain;
}

static void *qr_thread(void *arg)
{
	(void)arg;
	pthread_setname_np(pthread_self(), "q1-queryresults");
	q1_rt_cap(); // started from the RenderThread: its priority is inherited
	uint8_t buf[QR_MAX];
	pthread_mutex_lock(&qr_lock);
	for (;;) {
		struct qr_slot *s = NULL;
		for (unsigned i = 0; i < QR_SLOTS && !s; i++)
			if (qr_slots[i].pending)
				s = &qr_slots[i];
		if (!s) {
			pthread_cond_wait(&qr_cond, &qr_lock);
			continue;
		}
		struct qr_slot q = *s;
		pthread_mutex_unlock(&qr_lock);
		VkResult r = qr_real(q.device, q.pool, q.first, q.count, q.size, buf, q.stride, q.flags);
		pthread_mutex_lock(&qr_lock);
		s->pending = false;
		if (r == VK_SUCCESS && s->pool == q.pool && s->first == q.first && s->size == q.size) {
			memcpy(s->data, buf, q.size);
			s->valid = true;
		}
	}
	return NULL;
}

static VKAPI_ATTR VkResult VKAPI_CALL GetQueryPoolResults(VkDevice device, VkQueryPool pool, uint32_t first,
                                                          uint32_t count, size_t size, void *data, VkDeviceSize stride,
                                                          VkQueryResultFlags flags)
{
	struct device *d = device_of(device);
	static _Atomic uint64_t qr_logged;
	uint64_t t = now_ns();
	if (t - qr_logged > 10000000000ull) {
		qr_logged = t;
		LOG("query results in 10 s: %llu ready, %llu from the last frame, %llu waited for\n",
		    (unsigned long long)atomic_exchange(&qr_stat_ready, 0), (unsigned long long)atomic_exchange(&qr_stat_cached, 0),
		    (unsigned long long)atomic_exchange(&qr_stat_waited, 0));
	}
	if (!(flags & VK_QUERY_RESULT_WAIT_BIT) || (flags & VK_QUERY_RESULT_PARTIAL_BIT) || size > QR_MAX ||
	    qr_plain_wait())
		return d->GetQueryPoolResults(device, pool, first, count, size, data, stride, flags);
	VkResult r = d->GetQueryPoolResults(device, pool, first, count, size, data, stride,
	                                    flags & ~VK_QUERY_RESULT_WAIT_BIT);
	pthread_mutex_lock(&qr_lock);
	struct qr_slot *s = NULL;
	for (unsigned i = 0; i < QR_SLOTS && !s; i++) {
		struct qr_slot *c = &qr_slots[i];
		if (c->device == device && c->pool == pool && c->first == first && c->count == count &&
		    c->stride == stride && c->flags == flags && c->size == size)
			s = c;
	}
	if (r == VK_SUCCESS) {
		if (s) {
			memcpy(s->data, data, size);
			s->valid = true;
		}
		pthread_mutex_unlock(&qr_lock);
		qr_stat_ready++;
		return r;
	}
	if (r != VK_NOT_READY || !s || !s->valid) {
		if (!s) { // remember these queries for the next frames
			s = &qr_slots[qr_next++ % QR_SLOTS];
			while (s->pending) { // the helper thread is reading into it: take another one
				pthread_mutex_unlock(&qr_lock);
				sched_yield();
				pthread_mutex_lock(&qr_lock);
			}
			*s = (struct qr_slot){device, pool, first, count, stride, flags, size, false, false, {0}};
		}
		pthread_mutex_unlock(&qr_lock);
		r = d->GetQueryPoolResults(device, pool, first, count, size, data, stride, flags);
		pthread_mutex_lock(&qr_lock);
		if (r == VK_SUCCESS && s->pool == pool && s->first == first && s->size == size) {
			memcpy(s->data, data, size);
			s->valid = true;
		}
		pthread_mutex_unlock(&qr_lock);
		qr_stat_waited++;
		return r;
	}
	memcpy(data, s->data, size);
	if (!s->pending) {
		s->pending = true;
		if (!qr_thread_started) {
			qr_real = d->GetQueryPoolResults;
			pthread_t th;
			qr_thread_started = pthread_create(&th, NULL, qr_thread, NULL) == 0;
			if (qr_thread_started)
				pthread_detach(th);
		}
		pthread_cond_signal(&qr_cond);
	}
	pthread_mutex_unlock(&qr_lock);
	qr_stat_cached++;
	return VK_SUCCESS;
}
