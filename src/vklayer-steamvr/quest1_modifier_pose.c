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
//
// SteamVR 2.19's vrcompositor writes that memory later: after it has bound those sets, just before it submits the
// frame, with the pose of vrlink's last submit at that moment. But the picture it draws is the one it took earlier in
// the frame (the layer copies it at vrcompositor's barrier on it, quest1_modifier_record.c): vrlink's last submit
// before that copy. When vrlink submitted in between, the frame has two candidates, the memory as it was at the bind
// (the pose written for the frame before) and as it is at the submit, and either may be the picture's: at each of 8
// steps of the pose sent to the PC, the picture (its horizon read in the decoded frame at the copy) went with the bind's
// pose 4 times and with the submit's 4 times, and every one of 353 pictures was vrlink's last submit before the copy
// (2026-11-17). The user felt it worn as the trembling back with 2.19. So the shim publishes the head pose of each
// submit with its time and frame buffer index (quest1_views.h q1_poses), the layer notes at the copy which submit the
// picture is (qsp_picture), and at the submit keeps whichever of the two snapshots has that pose's rotation
// (qsp_submit in quest1_rpmerge.c). When neither has it (vrlink submitted twice in the frame: the memory never held
// the picture's pose), the layer writes it into the snapshot itself: each eye's pose and its inverse, made from the
// picture's head pose and the eye's offset from the head (qsp_repose). "vrlink's last submit before the copy" was
// wrong for ~10 % of the pictures when vrlink submitted while vrcompositor took the frame; the picture is now told by
// its frame buffer, known for each NV12 image vrcompositor imports (qsp_image_imported: kcmp against vrlink's own
// fds of its buffers, which the shim publishes). Measured with the
// pose sent to the PC stepped by 1.5°: 0 of 112 pictures off (4 of 40 without the fix); with ±1° of random pitch at
// every pose read, 2.5 px rms left instead of 7.2. Without the shim's poses (an older shim) the bind's snapshot stays,
// as for 2.17.
enum { QSP_BYTES = 2800 };

static struct
{
	pthread_mutex_t lock;
	VkDeviceMemory host; //!< the memory vrcompositor writes the pose into
	const uint8_t *map;  //!< the layer's mapping of it, NULL when it could not map it
	unsigned copies, other; //!< sets bound with a snapshot, sets that read the memory but could not be copied
	unsigned bind, submit, moved, neither, unknown; //!< where each snapshot found its picture's pose (qsp_paired)
	double moved_deg, neither_deg;                  //!< how far off those not found were, summed
	uint64_t log_ns;
} qsp = {PTHREAD_MUTEX_INITIALIZER};
static unsigned qsp_guess, qsp_learnt, qsp_byfd; //!< pictures paired with the last submit, by the votes, by buffer

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
			LOG("stream: %u descriptor sets bound with a copy of the stream's pose in 60 s, %u left as they were; the "
			    "picture's pose was the bind's %u times, the submit's %u, made from them %u (%.2f deg away), not made "
			    "%u (%.2f deg off), the picture not known %u; pictures found by their buffer %u, by the votes %u, as "
			    "the last submit %u\n",
			    qsp.copies, qsp.other, qsp.bind, qsp.submit, qsp.moved, qsp.moved ? qsp.moved_deg / qsp.moved : 0.0,
			    qsp.neither, qsp.neither ? qsp.neither_deg / qsp.neither : 0.0, qsp.unknown, qsp_byfd, qsp_learnt,
			    qsp_guess);
		qsp_byfd = qsp_learnt = qsp_guess = 0;
		qsp.copies = qsp.other = qsp.bind = qsp.submit = qsp.moved = qsp.neither = qsp.unknown = 0;
		qsp.moved_deg = qsp.neither_deg = 0;
		qsp.log_ns = t;
	}
	pthread_mutex_unlock(&qsp.lock);
}

//! qsp_submit's count: where a snapshot found its picture's pose (0 the bind's, 1 the submit's, 2 made from one of
//! them (qsp_repose), 3 not found; dist: the nearer one's qsp_rot_dist; -1 the picture not known).
static void qsp_paired(int kind, float dist)
{
	// |R1 - R2|^2 = 4 (1 - cos a) ~ 2 a^2 (a square root without libm: the layer does not link it)
	float x = dist / 2, deg = x > 1 ? x : 1;
	for (int i = 0; i < 20; i++)
		deg = 0.5f * (deg + x / deg);
	deg *= 57.29578f;
	pthread_mutex_lock(&qsp.lock);
	qsp.bind += kind == 0;
	qsp.submit += kind == 1;
	qsp.moved += kind == 2;
	qsp.neither += kind == 3;
	qsp.unknown += kind < 0;
	if (kind == 2)
		qsp.moved_deg += deg;
	if (kind == 3)
		qsp.neither_deg += deg;
	pthread_mutex_unlock(&qsp.lock);
}

// --- Which submit of vrlink's the picture is, and its pose (quest1_views.h q1_poses)

static bool qsp_poses_fd; //!< the shim publishes vrlink's frame buffers' fds (fd[] in q1_poses)

static const struct q1_poses *qsp_poses(void)
{
	static _Atomic(const struct q1_poses *) p;
	static _Atomic uint64_t tried_ns;
	const struct q1_poses *m = atomic_load(&p);
	uint64_t t = now_ns();
	if (m || t - atomic_load(&tried_ns) < 1000000000ull)
		return m;
	atomic_store(&tried_ns, t);
	int fd = open(Q1_POSES_PATH, O_RDONLY | O_CLOEXEC);
	struct stat st;
	const size_t old_size = offsetof(struct q1_poses, fd); // a shim without the frame buffers' fds
	void *v = fd >= 0 && fstat(fd, &st) == 0 && st.st_size >= (off_t)old_size
	              ? mmap(NULL, sizeof(*m), PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
	qsp_poses_fd = v != MAP_FAILED && st.st_size >= (off_t)sizeof(*m);
	if (fd >= 0)
		close(fd);
	if (v == MAP_FAILED)
		return NULL;
	const struct q1_poses *expected = NULL;
	if (!atomic_compare_exchange_strong(&p, &expected, (const struct q1_poses *)v)) {
		munmap(v, sizeof(*m));
		return expected;
	}
	LOG("stream: each picture's pose from %s\n", Q1_POSES_PATH);
	return v;
}

//! A copy of entry k-th newest of vrlink's poses (0 the last), false if being rewritten or not there.
static bool qsp_pose_entry(const struct q1_poses *p, uint32_t n, uint32_t k, uint64_t *t, int32_t *frame, float pose[12])
{
	if (k >= Q1_POSES - 1 || k >= n) // (the oldest slot may be being rewritten)
		return false;
	uint32_t want = n - k;
	const __typeof__(p->p[0]) *e = &p->p[(want - 1) % Q1_POSES];
	if (__atomic_load_n(&e->seq, __ATOMIC_ACQUIRE) != want)
		return false;
	*t = e->t_ns;
	*frame = e->frame;
	memcpy(pose, e->pose, 12 * sizeof(float));
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	return __atomic_load_n(&e->seq, __ATOMIC_RELAXED) == want;
}

// Which of vrlink's frame buffers each NV12 image of vrcompositor's is. vrlink submits the same 16 buffers in the same
// order each time, with the index of the one to show; vrcompositor imports each once, so an image is always the same
// index. It is learnt from the copies: the buffer of vrlink's last submit before the copy, which it is for ~90 % of
// them (the rest: vrlink submitted while vrcompositor was taking the frame), counted until one index has most.
enum { QSP_IMAGES = 64, QSP_VOTES_MIN = 4 };
static struct
{
	VkImage image;
	uint64_t used_ns;
	uint16_t votes[16];
} qsp_img[QSP_IMAGES];

//! Called with qsp_picture's lock: the frame buffer image is learnt to be, -1 while not known; `last` is the buffer of
//! vrlink's last submit before the copy, counted for it.
static int qsp_image_frame(VkImage image, int32_t last, uint64_t now)
{
	unsigned k = 0, old = 0;
	while (k < QSP_IMAGES && qsp_img[k].image != image)
		k++;
	if (k == QSP_IMAGES) { // a new image: the slot used longest ago
		for (k = 0; k < QSP_IMAGES; k++)
			if (qsp_img[k].used_ns < qsp_img[old].used_ns)
				old = k;
		k = old;
		memset(&qsp_img[k], 0, sizeof(qsp_img[k]));
		qsp_img[k].image = image;
	}
	qsp_img[k].used_ns = now;
	// Halved whenever one count reaches 32, so that what was learnt fades: a new vrlink (the stream restarted) brings
	// new buffers, which vrcompositor may import at the handles of the old ones. Kept for ever, the old counts made
	// those images unknown for the whole session (0 found by their buffer, 2026-11-17).
	if (last >= 0 && last < 16 && ++qsp_img[k].votes[last] >= 32)
		for (int i = 0; i < 16; i++)
			qsp_img[k].votes[i] /= 2;
	int best = -1;
	unsigned top = 0, second = 0;
	for (int i = 0; i < 16; i++) {
		unsigned v = qsp_img[k].votes[i];
		if (v > top)
			second = top, top = v, best = i;
		else if (v > second)
			second = v;
	}
	return top >= QSP_VOTES_MIN && top >= 2 * second ? best : -1;
}

// Which of vrlink's frame buffers an image is, exactly: the shim publishes vrlink's process and its own fd of each
// buffer (the dma-buf it has the decoder write into, quest1_views.h), and an image vrcompositor imports is a dma-buf of
// that same file, which kcmp tells across the two processes (same user, no pid namespace between them). Checked at the
// import, and again once a second while not found (the import may come before vrlink handed the buffer to the
// decoder). The votes above stay for when it is not known (an older shim). With the votes alone half the pictures
// were still guessed as the last submit, ~10 % of them wrongly, and worn the stream trembled now and then, nearly all
// the time with the dashboard open (2026-11-17). (The handles vrlink submits name fds of vrcompositor's that are not
// the buffer: a shared memory file each.)
static struct
{
	VkImage image;
	int own;   //!< the layer's fd of its dma-buf, kept while its buffer is not known
	int frame; //!< which of vrlink's frame buffers it is, -1 while not known
	uint64_t tried_ns;
} qsp_imfd[QSP_IMAGES];
static unsigned qsp_imfd_next;
static pthread_mutex_t qsp_imfd_lock = PTHREAD_MUTEX_INITIALIZER;

//! Which of vrlink's frame buffers dma-buf fd `own` is, -1 if none (or no shim that tells).
static int qsp_frame_of(int own)
{
	const struct q1_poses *p = qsp_poses();
	if (!p || !qsp_poses_fd || own < 0)
		return -1;
	pid_t me = getpid(), vr = (pid_t)__atomic_load_n(&p->pid, __ATOMIC_RELAXED);
	for (int i = 0; vr > 0 && i < Q1_POSES; i++) {
		int f = __atomic_load_n(&p->fd[i], __ATOMIC_RELAXED);
		if (f >= 0 && syscall(SYS_kcmp, me, vr, 0 /* KCMP_FILE */, own, f) == 0)
			return i;
	}
	return -1;
}

//! quest1_modifier_extfmt.c: NV12 image `image` was imported from dma-buf fd `fd` (an fd of the layer's own).
static void qsp_image_imported(VkImage image, int fd)
{
	int own = fcntl(fd, F_DUPFD_CLOEXEC, 0), frame = qsp_frame_of(own);
	pthread_mutex_lock(&qsp_imfd_lock);
	unsigned k = 0;
	while (k < QSP_IMAGES && qsp_imfd[k].image != image)
		k++;
	if (k == QSP_IMAGES)
		k = qsp_imfd_next++ % QSP_IMAGES;
	if (qsp_imfd[k].image && qsp_imfd[k].own >= 0)
		close(qsp_imfd[k].own);
	qsp_imfd[k].image = image;
	qsp_imfd[k].frame = frame;
	qsp_imfd[k].own = frame < 0 ? own : -1;
	qsp_imfd[k].tried_ns = now_ns();
	pthread_mutex_unlock(&qsp_imfd_lock);
	if (frame >= 0 && own >= 0)
		close(own);
	LOG("stream: NV12 image %p imported: vrlink's frame buffer %d\n", (void *)image, frame);
}

//! The frame buffer image is, -1 if not known (tried again once a second).
static int qsp_image_known(VkImage image, uint64_t now)
{
	int r = -1;
	pthread_mutex_lock(&qsp_imfd_lock);
	for (unsigned k = 0; k < QSP_IMAGES; k++) {
		if (qsp_imfd[k].image != image)
			continue;
		if (qsp_imfd[k].frame < 0 && qsp_imfd[k].own >= 0 && now - qsp_imfd[k].tried_ns > 1000000000ull) {
			qsp_imfd[k].tried_ns = now;
			qsp_imfd[k].frame = qsp_frame_of(qsp_imfd[k].own);
			if (qsp_imfd[k].frame >= 0) {
				close(qsp_imfd[k].own);
				qsp_imfd[k].own = -1;
				LOG("stream: NV12 image %p: vrlink's frame buffer %d\n", (void *)image, qsp_imfd[k].frame);
			}
		}
		r = qsp_imfd[k].frame;
		break;
	}
	pthread_mutex_unlock(&qsp_imfd_lock);
	return r;
}

//! qcp_record: command buffer cb draws a picture of vrlink's: NV12 image `image`, copied now (fresh), or the one
//! copied last. Its pose: the last submit before now of that image's buffer (known by kcmp, else by the votes), else
//! the last submit before now.
static void qsp_picture(VkCommandBuffer cb, VkImage image, bool fresh)
{
	static pthread_mutex_t l = PTHREAD_MUTEX_INITIALIZER;
	static float last[12];
	static bool have;
	pthread_mutex_lock(&l);
	const struct q1_poses *p = fresh ? qsp_poses() : NULL;
	if (fresh)
		have = false;
	if (p) {
		uint64_t t = now_ns(), et;
		uint32_t n = __atomic_load_n(&p->n, __ATOMIC_ACQUIRE);
		struct
		{
			int32_t frame;
			float pose[12];
		} e[Q1_POSES];
		unsigned ne = 0;
		for (uint32_t k = 0; k < Q1_POSES - 1; k++) // newest first, up to now
			if (qsp_pose_entry(p, n, k, &et, &e[ne].frame, e[ne].pose) && et <= t)
				ne++;
		if (ne) {
			int known = qsp_image_known(image, t);
			unsigned pick = 0;
			while (known >= 0 && pick < ne && e[pick].frame != known)
				pick++;
			int f = qsp_image_frame(image, e[0].frame, t); // (counted either way)
			if (known >= 0 && pick < ne)
				qsp_byfd++;
			else {
				pick = 0;
				while (f >= 0 && pick < ne && e[pick].frame != f)
					pick++;
				if (pick == ne)
					pick = 0;
				qsp_guess += f < 0;
				qsp_learnt += f >= 0;
			}
			memcpy(last, e[pick].pose, sizeof(last));
			have = true;
		}
	}
	bool ok = have;
	float pose[12];
	memcpy(pose, last, sizeof(pose));
	pthread_mutex_unlock(&l);
	rm_picture(cb, ok ? pose : NULL);
}

// The 2800 bytes hold, per eye (from float QSP_EYE[e]), the eye's pose (column-major 4x4: the head pose moved by half
// the IPD), the same again, then its inverse twice; nothing else in them follows the pose (2026-11-17, two snapshots of
// different poses: only those 8 matrices' upper 3 rows differ).
static const int QSP_EYE[2] = {44, 156};

//! How far the left eye's pose in a 2800-byte snapshot is from head pose `head` (HmdMatrix34_t) in rotation: the sum
//! of squared differences.
static float qsp_rot_dist(const uint8_t *snap, const float head[12])
{
	float m[16];
	memcpy(m, snap + QSP_EYE[0] * sizeof(float), sizeof(m));
	float d = 0;
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			d += (m[c * 4 + r] - head[r * 4 + c]) * (m[c * 4 + r] - head[r * 4 + c]);
	return d;
}

//! Snapshot `dst` made for head pose `want` from snapshot `from` (made by vrcompositor for another submit of
//! vrlink's): each eye keeps its offset from the head, and goes with the head to `want`. False when the head pose
//! `from` was made for is not among vrlink's recent ones (its eye offset is then not known).
static bool qsp_repose(uint8_t *dst, const uint8_t *from, size_t sz, const float want[12])
{
	const struct q1_poses *p = qsp_poses();
	if (!p || sz < (size_t)(QSP_EYE[1] + 64) * sizeof(float))
		return false;
	uint32_t n = __atomic_load_n(&p->n, __ATOMIC_ACQUIRE);
	float had[12];
	bool found = false;
	for (uint32_t k = 0; k < Q1_POSES - 1 && !found; k++) {
		uint64_t et;
		int32_t f;
		found = qsp_pose_entry(p, n, k, &et, &f, had) && qsp_rot_dist(from, had) < 1e-6f;
	}
	if (!found)
		return false;
	if (dst != from)
		memcpy(dst, from, sz);
	for (int e = 0; e < 2; e++) {
		float m[16];
		memcpy(m, from + QSP_EYE[e] * sizeof(float), sizeof(m));
		// the eye's offset in the head's frame: had's rotation transposed times (eye position - head position)
		float dp[3] = {m[12] - had[3], m[13] - had[7], m[14] - had[11]}, off[3];
		for (int i = 0; i < 3; i++)
			off[i] = had[0 * 4 + i] * dp[0] + had[1 * 4 + i] * dp[1] + had[2 * 4 + i] * dp[2];
		float E[16] = {0}, I[16] = {0}; // column-major: the eye at `want`, and its inverse
		for (int r = 0; r < 3; r++) {
			for (int c = 0; c < 3; c++) {
				E[c * 4 + r] = want[r * 4 + c];
				I[c * 4 + r] = want[c * 4 + r]; // the rotation transposed
			}
			E[12 + r] = want[r * 4 + 3] + want[r * 4 + 0] * off[0] + want[r * 4 + 1] * off[1] + want[r * 4 + 2] * off[2];
		}
		for (int r = 0; r < 3; r++)
			I[12 + r] = -(I[0 * 4 + r] * E[12] + I[1 * 4 + r] * E[13] + I[2 * 4 + r] * E[14]);
		E[15] = I[15] = 1;
		float *o = (float *)(void *)dst + QSP_EYE[e];
		memcpy(o, E, sizeof(E));
		memcpy(o + 16, E, sizeof(E));
		memcpy(o + 32, I, sizeof(I));
		memcpy(o + 48, I, sizeof(I));
	}
	return true;
}
