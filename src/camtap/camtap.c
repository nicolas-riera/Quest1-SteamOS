// SPDX-License-Identifier: MIT
/*
 * libq1camtap: LD_PRELOAD shim for Meta's trackingservice (bionic) that copies the tracking
 * cameras' HEADSET frames into the shared-memory ring of quest1_camera_ring.h while Meta's
 * stack keeps owning SyncBoss and the cameras (docs/passthrough.md).
 *
 * Hook: libsensorclientutils.so exports
 *   OVR::TrackingService::Transforms::ScopedTransformImageData::ScopedTransformImageData(
 *       gsl::span<OSSDK::Sensors::v3::ImageData>, OVR::Sensors::FrameSet const&, map const&)
 * which trackingservice calls for every frameset it hands to its engines. The constructor turns
 * the FrameSet's frames into ImageData entries (0x70 bytes each) whose +104 member points to the
 * pixels (the HAL's shared memory, valid while the object lives, i.e. after the original
 * constructor returned and until it is destroyed). We call the original, then copy the pixels.
 * No second client of the sensors HAL, no change to exposure or frame rate, head tracking
 * untouched.
 *
 * Layouts (static analysis of UpdateFrame/ctor, system build 49845030443200410, aarch64):
 *   FrameSet: +0 u64 id/timestamp, frames at +0x18 + 0x60*i (count: u8 at +976)
 *     Frame(0x60): +0 u32 FrameType, +8 u32 pixel format, +12 u32 width, +16 u32 height,
 *                  +20 u32 stride?, +24 u32 byte offset in the ImageBuffer, ...
 *   ImageData (0x70): +0 u64, +8 u32 FrameType, +80.. dims, +96 u32 size, +104 u8* pixels.
 *
 * Knobs (environment of trackingservice): Q1CAMTAP_LOG=<path> (probe log), Q1CAMTAP_DUMP=<dir>
 * (write the first N framesets as PGM; test only), Q1CAMTAP_ALWAYS=1 (ignore the consumer
 * heartbeat). Normally frames are copied only while a consumer touches
 * /dev/shm/quest1-cameras.want at least every 3 s.
 *
 * Built like q1trackd: plain C, glibc headers, linked against the device's bionic (build.sh).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "../steamvr-quest1/quest1_camera_ring.h"

#define BIONIC_RTLD_NEXT ((void *)-1L)
#define SYM_CTOR \
	"_ZN3OVR15TrackingService10Transforms24ScopedTransformImageDataC1EN3gsl4spanIN5OSSDK7Sensors2v39ImageDataELm18446744073709551615EEERKNS_7Sensors8FrameSetERKNSt3__113unordered_mapINSA_9FrameTypeENSF_IjNSE_6vectorINSE_10shared_ptrINSA_11ImageBufferEEENSE_9allocatorISK_EEEENSE_4hashIjEENSE_8equal_toIjEENSL_INSE_4pairIKjSN_EEEEEENSO_ISG_EENSQ_ISG_EENSL_INSS_IKSG_SW_EEEEEE"

#define FS_FRAMES 0x18
#define FS_STRIDE 0x60
#define FS_COUNT 976
#define IMG_STRIDE 0x70

static int log_fd = -1;
static const char *dump_dir;
static int always;
static struct q1cam_ring_header *ring;
static size_t ring_size;
static int64_t want_checked_ns;
static int want_on;
static unsigned logged;

static int64_t now_ns(clockid_t c)
{
	struct timespec ts;
	clock_gettime(c, &ts);
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void lg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void lg(const char *fmt, ...)
{
	if (log_fd < 0)
		return;
	char b[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	if (n > 0)
		(void)!write(log_fd, b, (size_t)(n < (int)sizeof(b) ? n : (int)sizeof(b) - 1));
}

static void hex(const char *tag, const void *p, size_t n)
{
	char b[600];
	size_t o = (size_t)snprintf(b, sizeof(b), "%s", tag);
	for (size_t i = 0; i < n && o + 4 < sizeof(b); i++)
		o += (size_t)snprintf(b + o, sizeof(b) - o, "%02x%s", ((const uint8_t *)p)[i], (i & 3) == 3 ? " " : "");
	b[o++] = '\n';
	if (log_fd >= 0)
		(void)!write(log_fd, b, o);
}

__attribute__((constructor)) static void camtap_init(void)
{
	const char *l = getenv("Q1CAMTAP_LOG");
	if (l)
		log_fd = open(l, O_WRONLY | O_CREAT | O_APPEND, 0644);
	dump_dir = getenv("Q1CAMTAP_DUMP");
	always = getenv("Q1CAMTAP_ALWAYS") != NULL;
	lg("camtap: loaded in pid %d\n", getpid());
}

static int ring_open(uint32_t slot_bytes)
{
	if (ring)
		return 0;
	const char *path = Q1CAM_RING_DEFAULT_PATH;
	size_t size = (size_t)q1cam_ring_file_size(slot_bytes);
	int fd = open(path, O_RDWR | O_CREAT, 0644);
	if (fd < 0)
		return -1;
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return -1;
	}
	void *m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (m == MAP_FAILED)
		return -1;
	struct q1cam_ring_header *h = m;
	if (h->magic != Q1CAM_RING_MAGIC) {
		memset(h, 0, sizeof(*h));
		h->version = Q1CAM_RING_VERSION;
		h->header_size = sizeof(*h);
		h->slot_count = Q1CAM_RING_SLOTS;
		h->slot_bytes = slot_bytes;
		__atomic_store_n(&h->magic, Q1CAM_RING_MAGIC, __ATOMIC_RELEASE);
	}
	h->producer_pid = getpid();
	ring = h;
	ring_size = size;
	return 0;
}

static int consumer_wants(void)
{
	if (always)
		return 1;
	int64_t t = now_ns(CLOCK_MONOTONIC);
	if (t - want_checked_ns < 500000000)
		return want_on;
	want_checked_ns = t;
	struct stat st;
	want_on = stat("/dev/shm/quest1-cameras.want", &st) == 0 && (time(NULL) - st.st_mtime) <= 3;
	return want_on;
}

__attribute__((visibility("default"))) void tap_ctor(void *self, size_t n, void *span, const void *fs, const void *map) __asm__(SYM_CTOR);

typedef void (*ctor_fn)(void *, size_t, void *, const void *, const void *);
static ctor_fn real_ctor;

static void dump_pgm(unsigned idx, unsigned cam, const uint8_t *px, unsigned w, unsigned h, unsigned stride)
{
	char p[256];
	snprintf(p, sizeof(p), "%s/cam%u-%u.pgm", dump_dir, idx, cam);
	int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return;
	char hd[64];
	int n = snprintf(hd, sizeof(hd), "P5\n%u %u\n255\n", w, h);
	(void)!write(fd, hd, (size_t)n);
	for (unsigned y = 0; y < h; y++)
		(void)!write(fd, px + (size_t)y * stride, w);
	close(fd);
}

/* the original constructor, then a copy of the HEADSET pixels */
void tap_ctor(void *self, size_t n, void *span, const void *fs, const void *map)
{
	static unsigned calls;
	if (!real_ctor) {
		real_ctor = (ctor_fn)dlsym(BIONIC_RTLD_NEXT, SYM_CTOR);
		lg("camtap: real ctor %p\n", (void *)real_ctor);
	}
	if (calls++ < 3)
		lg("camtap: ctor call %u n=%zu\n", calls, n);
	real_ctor(self, n, span, fs, map);

	const uint8_t *f = fs;
	unsigned count = f[FS_COUNT];
	if (count > n)
		count = (unsigned)n;
	int probe = log_fd >= 0 && logged < 24;
	if (probe) {
		logged++;
		lg("ctor n=%zu count=%u fs=%p id=%llx\n", n, count, fs, (unsigned long long)*(const uint64_t *)f);
	}
	int wr = consumer_wants();
	if (!probe && !dump_dir && !wr)
		return;
	struct q1cam_ring_slot *slot = NULL;
	uint8_t *slot_px = NULL;
	unsigned ncam = 0;
	/* several engines' callbacks get the same frameset (and CONTROLLER ones): write each HEADSET
	 * frameset once, newest only */
	static int64_t last_ts;
	static int busy;
	int64_t ts = 0;
	for (unsigned i = 0; i < count && wr; i++) {
		const uint8_t *fr = f + FS_FRAMES + (size_t)FS_STRIDE * i;
		if (*(const uint32_t *)fr == Q1CAM_FRAME_HEADSET) {
			ts = *(const int64_t *)(fr + 72);
			break;
		}
	}
	int locked = 0;
	if (wr && ts && ts > __atomic_load_n(&last_ts, __ATOMIC_ACQUIRE) && !__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE)) {
		locked = 1;
		__atomic_store_n(&last_ts, ts, __ATOMIC_RELEASE);
	}
	if (locked && ring_open(Q1CAM_MAX_CAMS * 640 * 480) == 0) {
		uint32_t k = ring->write_count % ring->slot_count;
		slot = &ring->slot[k];
		slot_px = (uint8_t *)ring + ring->header_size + (size_t)k * ring->slot_bytes;
		slot->seq++; /* odd: being written */
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		slot->frame_type = 0;
		memset(slot->cam, 0, sizeof(slot->cam));
		slot->capture_mono_ns = 0;
	}

	static unsigned dumped;
	for (unsigned i = 0; i < count; i++) {
		const uint8_t *fr = f + FS_FRAMES + (size_t)FS_STRIDE * i;
		const uint8_t *id = (const uint8_t *)span + (size_t)IMG_STRIDE * i;
		if (probe) {
			hex("  hdr ", fr - 8, 8);
			hex("  frame ", fr, FS_STRIDE);
			hex("  imgdata ", id, IMG_STRIDE);
		}
		uint32_t type = *(const uint32_t *)fr; /* HAL FrameType: 2 HEADSET, 3 CONTROLLER */
		uint32_t cam = *(const uint32_t *)(fr - 8); /* camera index in the frameset */
		const uint8_t *px = *(const uint8_t *const *)(id + 104);
		uint32_t w = *(const uint32_t *)(fr + 4), h = *(const uint32_t *)(fr + 8);
		uint32_t stride = *(const uint32_t *)(fr + 12);
		if (dump_dir && dumped < 16 && px && type == Q1CAM_FRAME_HEADSET && w == 640 && h == 480) {
			dump_pgm(dumped / 4, cam, px, w, h, stride);
			dumped++;
		}
		if (slot && type == Q1CAM_FRAME_HEADSET && px && w == 640 && h == 480 && stride == 640 &&
		    ncam < Q1CAM_MAX_CAMS) {
			memcpy(slot_px + (size_t)ncam * 640 * 480, px, 640 * 480);
			double ex = *(const double *)(fr + 48), gn = *(const double *)(fr + 56);
			slot->cam[ncam] = (struct q1cam_ring_cam){1, cam < 4 ? cam : ncam, 640, 480, 640,
								   (uint32_t)(ncam * 640 * 480), (float)ex, (float)gn};
			if (!slot->capture_mono_ns)
				slot->capture_mono_ns = *(const int64_t *)(fr + 72);
			ncam++;
		}
	}
	if (slot) {
		slot->frame_type = ncam ? Q1CAM_FRAME_HEADSET : 0;
		slot->frameset = *(const uint64_t *)f;
		slot->arrival_mono_ns = now_ns(CLOCK_MONOTONIC);
		slot->ncams = ncam;
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		slot->seq++; /* even: stable */
		if (ncam) {
			__atomic_add_fetch(&ring->write_count, 1, __ATOMIC_RELEASE);
			syscall(SYS_futex, &ring->write_count, 1 /* FUTEX_WAKE */, 0x7fffffff, NULL, NULL, 0);
		}
	}
	if (locked)
		__atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
}
