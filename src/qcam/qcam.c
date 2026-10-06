// qcam: grab frames from the 4 OV7251 tracking cameras of the Quest 1 on native Linux, through
// Meta's libsyncboss (sensor power/config/sync, SyncBoss MCU) and libqcameraoculushal (Qualcomm
// camera_v2 CSI/ISP "mini-driver"), both bionic libraries loaded with libhybris.
// See docs/tracking-6dof.md (Path O). Static analysis only: every prototype below is tagged
//   [V] = verified by disassembly (address given), [I] = inferred, to confirm on the device.
//
// Build: see build.sh (WSL cross build), or natively in Holo:
//   gcc -O2 -o qcam qcam.c -ldl -lpthread
// Run (root, after holo-android-blobs mounted /system):
//   LD_LIBRARY_PATH=/opt/hybris/lib LD_PRELOAD=/opt/hybris/lib/libbionictls.so ./qcam [options]
//   ./qcam --help
//
// Safety: only calls syncboss_init/deinit, the stream wait, and syncboss_camera_{probe,release,init,
// deinit,set_bpp,set_frame_rate,params,set_frame_tag_mode,start_streaming,stop_streaming,
// set_exposure_gain[_tag],decode_metadata}. It never calls any pairing, unpairing, firmware update,
// calibration, register-write or test-pattern function, and writes no partition and nothing under
// /persist: the only files written are the images in the output directory (default /tmp).
// Cleanup (stop streaming, stop/release sensors, camera deinit/release, syncboss_deinit) runs on
// normal exit, on errors, on SIGINT/SIGTERM and on the hard timeout (alarm). A second signal, a
// second alarm or a crash (SIGSEGV/SIGBUS/SIGABRT, e.g. a failed __android_log_assert in the camera
// libs) runs an emergency path that only stops streaming and powers the cameras down via SyncBoss.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ======================================================================================
 * Tunables (all overridable on the command line)
 * ====================================================================================== */
#define QCAM_MAX_CAMS 4             /* [V] libqcameraoculushal MAX_NUM_SENSORS = 4 (qcamera_open 0x3b50) */
#define QCAM_DEFAULT_BPP 8          /* [V] HAL default: persist.vendor.camera.bpp = 8 (libqcamerahal 0x82a4) */
#define QCAM_DEFAULT_NBUFS 4        /* [V] HAL uses (per-camera config) + 3 buffers (libqcamerahal 0x8338) */
#define QCAM_DEFAULT_PERIOD_US 33333 /* [V] MontereyCameraProvider::toFramePeriod 0x65604: 60 Hz mains ->
				      * 33333 us, 50 Hz -> 40000 us. Passed to syncboss_camera_set_frame_rate. */
#define QCAM_DEFAULT_TAG_MODE 1     /* [V] startCameras 0x65a9c: syncboss_camera_set_frame_tag_mode(h, 1) */
#define QCAM_DEQ_TIMEOUT_MS 100     /* HAL StreamingThread uses 33 ms (libqcamerahal 0x6a20) */
#define QCAM_DEFAULT_EXPOSURE_US 4000 /* [I] units: HAL writes exposureDuration[s] * 1e6 (0x3a4e8) */
#define QCAM_DEFAULT_GAIN 2.0       /* [I] HAL writes gain * 16 as u16 (fcvtzs #4, 0x3a500) */
#define QCAM_DEFAULT_HEADSET_TAG 1  /* [I] tag byte per camera; metadata tags seen: 1, 2, 4 (0x64ea8) */
#define QCAM_DEFAULT_TIMEOUT_S 30   /* hard timeout for the whole run */
#define QCAM_DEFAULT_FRAMESETS 3
#define QCAM_DEFAULT_SKIP 10        /* framesets dropped first (exposure settling) */

/* ======================================================================================
 * libqcameraoculushal ABI
 * ====================================================================================== */

/* Filled by qcamera_query_sensor_info() / extract_sensor_info (0x52cc, memset 0x8c bytes). [V] */
struct qcam_sensor_info {
	uint32_t width;       /* +0  capability +32 (640) */
	uint32_t height;      /* +4  capability +36 (480), WITHOUT the metadata line */
	uint32_t format;      /* +8  cam_format_t: 0x70 GREY/Y8, 0x71 Y10 (MIPI RAW10 packed), 0x72 Y12
			       *     (control_init_capability 0xa560 / 0xa808 / 0xa850) */
	uint32_t bpp;         /* +12 8 / 10 / 12 (jump table at 0x1c95) */
	uint32_t pixel_proc;  /* +16 0 for the Y formats */
	uint32_t code_bits;   /* +20 bits per code unit: 8 (8 bpp) or 40 (10 bpp = 4 px in 5 bytes) */
	uint32_t px_per_code; /* +24 code_bits / bpp: 1 or 4 */
	uint32_t packed;      /* +28 1 */
	uint32_t bayer;       /* +32 0 = mono */
	uint32_t sensor_id;   /* +36 set by qcamera_query_sensor_info (0x4054) */
	uint8_t rest[140 - 40];
	uint8_t slack[116];   /* safety margin, the lib only writes 140 bytes */
};
/* qcamera_start_sensor(s, dims, fmt, ...) takes dims = &info.width (u32 w,h) and fmt = &info.format
 * (28 bytes: format..bayer, copied to sensor+40). That is exactly what QCamera::StartCamera does:
 * x1 = this+0x28 (info+0), x2 = this+0x30 (info+8) (libqcamerahal 0x6608/0x6610). [V] */

/* Returned by qcamera_dequeue (pointer = calloc(0x50) container + 8, get_qframe_container 0x49a8). [V] */
struct qcam_frame {
	int64_t ts_sec;        /* +0  v4l2_buffer.timestamp.tv_sec  (mm_stream_read_msm_frame 0xc608) */
	int64_t ts_nsec;       /* +8  v4l2_buffer.timestamp.tv_usec * 1000 (0xc61c). CLOCK_MONOTONIC
				*     (msm_isp_get_timestamp: ktime_get_ts, vt_enable off) */
	const void *fmt;       /* +16 -> sensor+40 (the 28-byte format block) */
	uint32_t sequence;     /* +24 v4l2_buffer.sequence (0xc5fc -> mm_buf+24) */
	uint32_t _pad0;
	uint64_t _zero[2];     /* +32 */
	uint8_t *data;         /* +48 mapped pixels (mm_buf+560), buffer base = metadata line first [I] */
	int32_t buffer_fd;     /* +56 ION/dma-buf fd (mm_buf+552); libqcamerahal reads qframe+0x38 [V] */
	int32_t _pad1;
	uint32_t poison;       /* +64 0 while dequeued; enqueue asserts on it */
};
/* The container's first 8 bytes (= ((void **)frame)[-1]) point to the driver's mm buffer
 * (592-byte entry, get_bufs 0x4e64): +4 index, +552 fd, +560 vaddr, +568 length (u32). [V] */
#define QCAM_MMBUF_LEN_OFF 568

typedef void *(*qc_open_t)(int bpp_mode);                 /* 0x3a9c [V] 0 = 8 bpp, 1 = 10 bpp, else abort */
typedef int (*qc_num_sensors_t)(void *hal);               /* 0x3c18 [V] returns u8 (mask 0xff) */
typedef void *(*qc_get_sensor_t)(void *hal, int idx);     /* 0x3cb8 [V] NULL + errno on failure */
typedef int (*qc_query_info_t)(void *sensor, struct qcam_sensor_info *out); /* 0x402c [V] */
typedef void (*qc_query_dims_t)(int bpp, int add_meta_line, uint32_t *w, uint32_t *h); /* 0x4064 [V]
					* bpp must be 8 or 10 (else abort in control_get_capability) */
typedef int (*qc_start_sensor_t)(void *sensor, const uint32_t *dims, const void *fmt28, int nbufs,
				 const int *buf_fds /* NULL: the lib allocates ION buffers itself */,
				 int add_meta_line);       /* 0x40f0 [V] 0 / -1 */
typedef struct qcam_frame *(*qc_dequeue_t)(void *sensor, int timeout_ms); /* 0x4a78 [V] NULL = timeout */
typedef int (*qc_enqueue_t)(void *sensor, struct qcam_frame *f);          /* 0x4ae4 [V] frees f */
typedef int (*qc_stop_sensor_t)(void *sensor);            /* 0x4654 [V] */
typedef void (*qc_release_sensor_t)(void *sensor);        /* 0x3f3c [V] asserts if still started */
typedef void (*qc_close_t)(void *hal);                    /* 0x3ba8 [V] asserts if sensors still held */
typedef int (*qc_get_fd_t)(void *sensor);                 /* 0x48fc [V] pollable fd */
typedef const char *(*qc_telemetry_t)(void *sensor);      /* 0x4b6c [V] string or NULL */

/* ======================================================================================
 * libsyncboss ABI (docs/syncboss-imu.md, docs/controllers-native.md)
 * ====================================================================================== */
struct sb_record {
	uint64_t seq;
	uint32_t type;
	uint32_t pad;
	uint8_t data[96];
};
enum { SB_REC_CAM_SHUTTER = 1, SB_REC_DISPLAY = 2, SB_REC_CAM_SHUTTER_V2 = 14 };
/* type 14, 0x28 bytes, process_camera_shutter_v2 0x7e18 [V]; the HAL reads capture time at
 * record+24 (= data+8, in us) and the 4-bit frame-sync id at record+36 (= data+20)
 * (FrameTimeStamper::frameSyncHandler 0x64abc/0x64ac0) [V] */
struct sb_shutter_v2 {
	uint8_t b0;            /* data+0  (raw packet byte 0, meaning unknown) */
	uint8_t _pad[7];
	uint64_t timestamp_us; /* data+8  SyncBoss time (same clock as the IMU timestamps) */
	uint32_t u16;          /* data+16 (raw packet bytes 9..12, unknown) */
	uint8_t sync_id;       /* data+20 frame-sync sequence id (HAL uses & 0xf) */
};

/* camera_set_exposure_gain_tag_internal 0x657c [V]: 4 x 6-byte entries -> MCU packet 0x36 */
struct sb_cam_egt {
	uint16_t exposure; /* [I] microseconds */
	uint16_t gain;     /* [I] gain * 16 */
	uint8_t tag;       /* [I] frame tag this setting applies to */
	uint8_t pad;
};

/* syncboss_camera_decode_metadata output (syncboss_ap_frame_metadata_t, 0x64b0) [V offsets] */
struct sb_frame_meta {
	uint16_t exposure; /* (line[6] << 8 | line[7]) * params.u16@6  -> HAL: / 1e6 = seconds */
	uint8_t gain_x16;  /* line[3]                                  -> HAL: * 0.0625 */
	uint8_t b3;        /* line[0x59] (unknown) */
	uint8_t tag;       /* line[0x52] & 0xf (0 if line[0x50] & 2) -> 1, 2, 4 = frame type */
	uint8_t sync_id;   /* line[0x52] >> 4: matches sb_shutter_v2.sync_id & 0xf */
	uint8_t pad[10];
};
/* (10 bpp: same fields at the RAW10-packed offsets 3, 7, 8, 0x6f, 0x64, 0x66) */

typedef int (*sb_init_t)(void **handle, const void *opts);
typedef int (*sb_h_t)(void *h);                                  /* deinit, camera_release (0x58f0) */
typedef int (*sb_probe_t)(void *h, uint8_t *cam_mask);           /* 0x5888 [V] cmd 0x28, 1-byte reply */
typedef int (*sb_u8_t)(void *h, uint8_t v);                      /* camera_init 0x5958 (n cams, pkt 0x2e),
								  * deinit 0x59e0 (n, cmd 0x2f),
								  * start_streaming 0x5a58 (n, pkt 0x2c),
								  * stop_streaming 0x5ae0 (n, cmd 0x2d),
								  * set_bpp 0x5ef8 (data 0x8c),
								  * set_frame_tag_mode 0x66bc (data 0x8d) [V] */
typedef int (*sb_u32_t)(void *h, uint32_t v);                    /* set_frame_rate 0x5df4: period us,
								  * data 0x89 [V] */
typedef int (*sb_params_t)(void *h, void *out /* >= 104 bytes */); /* 0x622c [V] */
typedef int (*sb_expgain_t)(void *h, const uint16_t *exp4, const uint16_t *gain4, uint8_t ncams); /* 0x5b58 [V] */
typedef int (*sb_expgaintag_t)(void *h, const struct sb_cam_egt *cfg4); /* 0x6574 / 0x66b4 [V] */
typedef int (*sb_decode_t)(const void *line, struct sb_frame_meta *out, const void *params); /* 0x64b0 [V] */
typedef int (*sb_wait_t)(void *h, uint32_t timeout_ms, struct sb_record *out);

/* ======================================================================================
 * Globals
 * ====================================================================================== */
static struct {
	qc_open_t open;
	qc_num_sensors_t num_sensors;
	qc_get_sensor_t get_sensor;
	qc_query_info_t query_info;
	qc_query_dims_t query_dims;
	qc_start_sensor_t start_sensor;
	qc_dequeue_t dequeue;
	qc_enqueue_t enqueue;
	qc_stop_sensor_t stop_sensor;
	qc_release_sensor_t release_sensor;
	qc_close_t close;
	qc_get_fd_t get_fd;
	qc_telemetry_t telemetry;
} qc;

static struct {
	sb_init_t init;
	sb_h_t deinit;
	sb_wait_t wait;
	sb_probe_t cam_probe;
	sb_h_t cam_release;
	sb_u8_t cam_init, cam_deinit, start_streaming, stop_streaming, set_bpp, set_tag_mode;
	sb_u32_t set_frame_rate;
	sb_params_t params;
	sb_expgain_t set_exp_gain;
	sb_expgaintag_t set_exp_gain_tag, set_ctrl_exp_gain_tag;
	sb_decode_t decode_meta;
} sb;

static struct {
	void *sb;              /* syncboss handle */
	bool cam_probed;       /* syncboss_camera_probe succeeded -> cameras powered */
	bool cam_inited;       /* syncboss_camera_init sent */
	bool sb_streaming;     /* syncboss_camera_start_streaming sent */
	uint8_t sb_ncams;
	void *hal;
	int nsensors;
	void *sensor[QCAM_MAX_CAMS];
	bool started[QCAM_MAX_CAMS];
	struct qcam_frame *held[QCAM_MAX_CAMS]; /* dequeued, not yet returned */
	pthread_t pump;
	bool pump_started;
} st;

static volatile sig_atomic_t stop_req;
static volatile sig_atomic_t signal_count;
static volatile sig_atomic_t in_cleanup;
static volatile sig_atomic_t in_emergency;
static volatile bool pump_run = true;

/* shutter ring (pump thread -> main) */
#define SHUTTER_RING 64
static pthread_mutex_t shutter_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
	uint64_t ts_us;
	uint8_t id, b0;
	uint32_t u16;
	uint64_t mono_ns; /* CLOCK_MONOTONIC when the record was popped */
} shutter[SHUTTER_RING];
static unsigned shutter_head, shutter_count;
static unsigned rec_counts[256];

static uint64_t mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

#define LOG(...) do { fprintf(stderr, "qcam: " __VA_ARGS__); fflush(stderr); } while (0)

/* ======================================================================================
 * Cleanup paths
 * ====================================================================================== */

/* async-signal context, last resort: power the sensors down through SyncBoss only. */
static void emergency(const char *why)
{
	if (in_emergency)
		_exit(4);
	in_emergency = 1;
	static const char msg[] = "qcam: emergency cleanup\n";
	(void)!write(2, msg, sizeof msg - 1);
	(void)!write(2, why, strlen(why));
	(void)!write(2, "\n", 1);
	alarm(3); /* if a syncboss call deadlocks (mutex held by the interrupted thread) -> _exit(4) */
	if (st.sb) {
		if (st.sb_streaming && sb.stop_streaming)
			sb.stop_streaming(st.sb, st.sb_ncams);
		if (st.cam_inited && sb.cam_deinit)
			sb.cam_deinit(st.sb, st.sb_ncams);
		if (st.cam_probed && sb.cam_release)
			sb.cam_release(st.sb);
	}
	_exit(3);
}

static void on_signal(int sig)
{
	if (sig == SIGSEGV || sig == SIGBUS || sig == SIGABRT || sig == SIGILL || sig == SIGFPE) {
		emergency(sig == SIGABRT ? "SIGABRT (library assert?)" : "fatal signal");
		return;
	}
	if (in_emergency) /* alarm fired during the emergency path */
		_exit(4);
	signal_count++;
	stop_req = 1;
	if (signal_count >= 2 || in_cleanup) {
		emergency(sig == SIGALRM ? "timeout" : "second signal");
		return;
	}
	if (sig == SIGALRM) {
		static const char msg[] = "qcam: hard timeout, stopping\n";
		(void)!write(2, msg, sizeof msg - 1);
	}
	alarm(10); /* give the normal cleanup 10 s, then the next SIGALRM goes to emergency() */
}

static void install_signals(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_NODEFER;
	int sigs[] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGALRM, SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE };
	for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
		sigaction(sigs[i], &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
}

static void cleanup(void)
{
	in_cleanup = 1;
	alarm(15); /* cleanup must finish; otherwise the alarm goes to emergency() */
	if (st.sb && st.sb_streaming) {
		int r = sb.stop_streaming(st.sb, st.sb_ncams);
		LOG("syncboss_camera_stop_streaming(%u) -> %d\n", st.sb_ncams, r);
		st.sb_streaming = false;
	}
	for (int i = 0; i < st.nsensors; i++) {
		if (st.held[i]) { /* control_free_bufs waits for every buffer to be back */
			qc.enqueue(st.sensor[i], st.held[i]);
			st.held[i] = NULL;
		}
		if (st.started[i]) {
			int r = qc.stop_sensor(st.sensor[i]);
			LOG("qcamera_stop_sensor(%d) -> %d\n", i, r);
			st.started[i] = false;
		}
	}
	for (int i = 0; i < st.nsensors; i++) {
		if (st.sensor[i]) {
			if (qc.telemetry) {
				const char *t = qc.telemetry(st.sensor[i]);
				if (t)
					LOG("cam %d telemetry: %s\n", i, t);
			}
			qc.release_sensor(st.sensor[i]);
			st.sensor[i] = NULL;
		}
	}
	if (st.hal) {
		qc.close(st.hal);
		st.hal = NULL;
		LOG("qcamera_close done\n");
	}
	if (st.sb && st.cam_inited) {
		int r = sb.cam_deinit(st.sb, st.sb_ncams);
		LOG("syncboss_camera_deinit(%u) -> %d\n", st.sb_ncams, r);
		st.cam_inited = false;
	}
	if (st.sb && st.cam_probed) {
		int r = sb.cam_release(st.sb);
		LOG("syncboss_camera_release -> %d\n", r);
		st.cam_probed = false;
	}
	if (st.pump_started) {
		pump_run = false;
		pthread_join(st.pump, NULL);
		st.pump_started = false;
	}
	if (st.sb) {
		sb.deinit(st.sb);
		st.sb = NULL;
		LOG("syncboss_deinit done\n");
	}
	alarm(0);
	in_cleanup = 0;
}

/* ======================================================================================
 * Library loading
 * ====================================================================================== */
extern void *android_dlopen(const char *filename, int flag) __attribute__((weak));
extern void *android_dlsym(void *handle, const char *symbol) __attribute__((weak));
static void *(*h_dlopen)(const char *, int);
static void *(*h_dlsym)(void *, const char *);

static int load_hybris(void)
{
	if (android_dlopen && android_dlsym) { /* linked with -lhybris-common */
		h_dlopen = android_dlopen;
		h_dlsym = android_dlsym;
		return 0;
	}
	const char *cand[] = { getenv("QCAM_HYBRIS_LIB"), "/opt/hybris/lib/libhybris-common.so.1",
			       "/opt/hybris/lib/libhybris-common.so", "libhybris-common.so.1",
			       "libhybris-common.so" };
	for (unsigned i = 0; i < sizeof cand / sizeof cand[0]; i++) {
		if (!cand[i])
			continue;
		void *l = dlopen(cand[i], RTLD_NOW | RTLD_GLOBAL);
		if (!l)
			continue;
		h_dlopen = (void *(*)(const char *, int))dlsym(l, "android_dlopen");
		h_dlsym = (void *(*)(void *, const char *))dlsym(l, "android_dlsym");
		if (h_dlopen && h_dlsym) {
			LOG("libhybris: %s\n", cand[i]);
			return 0;
		}
	}
	LOG("cannot load libhybris-common (set QCAM_HYBRIS_LIB or LD_LIBRARY_PATH=/opt/hybris/lib)\n");
	return -1;
}

static int missing;
static void *sym(void *lib, const char *name, bool required)
{
	void *p = h_dlsym(lib, name);
	if (!p && required) {
		LOG("missing symbol %s\n", name);
		missing++;
	}
	return p;
}

static int load_libs(void)
{
	if (load_hybris())
		return -1;
	void *lsb = h_dlopen("libsyncboss.so", RTLD_NOW);
	if (!lsb) {
		LOG("android_dlopen(libsyncboss.so) failed\n");
		return -1;
	}
	void *lqc = h_dlopen("libqcameraoculushal.so", RTLD_NOW);
	if (!lqc) {
		LOG("android_dlopen(libqcameraoculushal.so) failed\n");
		return -1;
	}
	sb.init = (sb_init_t)sym(lsb, "syncboss_init", true);
	sb.deinit = (sb_h_t)sym(lsb, "syncboss_deinit", true);
	sb.wait = (sb_wait_t)sym(lsb, "syncboss_wait_on_stream_data_exclusive", true);
	sb.cam_probe = (sb_probe_t)sym(lsb, "syncboss_camera_probe", true);
	sb.cam_release = (sb_h_t)sym(lsb, "syncboss_camera_release", true);
	sb.cam_init = (sb_u8_t)sym(lsb, "syncboss_camera_init", true);
	sb.cam_deinit = (sb_u8_t)sym(lsb, "syncboss_camera_deinit", true);
	sb.start_streaming = (sb_u8_t)sym(lsb, "syncboss_camera_start_streaming", true);
	sb.stop_streaming = (sb_u8_t)sym(lsb, "syncboss_camera_stop_streaming", true);
	sb.set_bpp = (sb_u8_t)sym(lsb, "syncboss_camera_set_bpp", true);
	sb.set_tag_mode = (sb_u8_t)sym(lsb, "syncboss_camera_set_frame_tag_mode", true);
	sb.set_frame_rate = (sb_u32_t)sym(lsb, "syncboss_camera_set_frame_rate", true);
	sb.params = (sb_params_t)sym(lsb, "syncboss_camera_params", false);
	sb.set_exp_gain = (sb_expgain_t)sym(lsb, "syncboss_camera_set_exposure_gain", false);
	sb.set_exp_gain_tag = (sb_expgaintag_t)sym(lsb, "syncboss_camera_set_exposure_gain_tag", false);
	sb.set_ctrl_exp_gain_tag =
		(sb_expgaintag_t)sym(lsb, "syncboss_camera_set_controller_exposure_gain_tag", false);
	sb.decode_meta = (sb_decode_t)sym(lsb, "syncboss_camera_decode_metadata", false);

	qc.open = (qc_open_t)sym(lqc, "qcamera_open", true);
	qc.num_sensors = (qc_num_sensors_t)sym(lqc, "qcamera_num_sensors", true);
	qc.get_sensor = (qc_get_sensor_t)sym(lqc, "qcamera_get_sensor", true);
	qc.query_info = (qc_query_info_t)sym(lqc, "qcamera_query_sensor_info", true);
	qc.query_dims = (qc_query_dims_t)sym(lqc, "qcamera_query_buffer_dimensions", true);
	qc.start_sensor = (qc_start_sensor_t)sym(lqc, "qcamera_start_sensor", true);
	qc.dequeue = (qc_dequeue_t)sym(lqc, "qcamera_dequeue", true);
	qc.enqueue = (qc_enqueue_t)sym(lqc, "qcamera_enqueue", true);
	qc.stop_sensor = (qc_stop_sensor_t)sym(lqc, "qcamera_stop_sensor", true);
	qc.release_sensor = (qc_release_sensor_t)sym(lqc, "qcamera_release_sensor", true);
	qc.close = (qc_close_t)sym(lqc, "qcamera_close", true);
	qc.get_fd = (qc_get_fd_t)sym(lqc, "qcamera_get_fd", false);
	qc.telemetry = (qc_telemetry_t)sym(lqc, "qcamera_get_telemetry_stats", false);
	return missing ? -1 : 0;
}

/* ======================================================================================
 * SyncBoss stream pump: logs camera shutter records (type 14 / 1)
 * ====================================================================================== */
static void *pump(void *arg)
{
	(void)arg;
	struct sb_record rec;
	while (pump_run) {
		int r = sb.wait(st.sb, 100, &rec);
		if (r == -11)
			continue;
		if (r) {
			usleep(10000);
			continue;
		}
		pthread_mutex_lock(&shutter_lock);
		rec_counts[rec.type & 0xff]++;
		if (rec.type == SB_REC_CAM_SHUTTER_V2) {
			struct sb_shutter_v2 s;
			memcpy(&s, rec.data, sizeof s);
			unsigned k = shutter_head++ % SHUTTER_RING;
			shutter[k].ts_us = s.timestamp_us;
			shutter[k].id = s.sync_id;
			shutter[k].b0 = s.b0;
			shutter[k].u16 = s.u16;
			shutter[k].mono_ns = mono_ns();
			shutter_count++;
		}
		pthread_mutex_unlock(&shutter_lock);
	}
	return NULL;
}

/* latest shutter record whose id matches (4 bits) */
static bool find_shutter(uint8_t id, uint64_t *ts_us, uint64_t *mono)
{
	bool found = false;
	pthread_mutex_lock(&shutter_lock);
	unsigned n = shutter_head < SHUTTER_RING ? shutter_head : SHUTTER_RING;
	for (unsigned i = 0; i < n; i++) {
		unsigned k = (shutter_head - 1 - i) % SHUTTER_RING;
		if ((shutter[k].id & 0xf) == (id & 0xf)) {
			*ts_us = shutter[k].ts_us;
			*mono = shutter[k].mono_ns;
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&shutter_lock);
	return found;
}

/* ======================================================================================
 * Image output
 * ====================================================================================== */
static uint32_t line_stride(const struct qcam_sensor_info *in)
{
	/* mm_stream_calc_offset_raw 0xd168 [V]: Y8 (0x70): (w + 15) & ~15;
	 * Y10 (0x71): ((w + 3) & ~3) * 5 / 4, then (+ 7) & ~7 (MIPI RAW10 packed) */
	if (in->bpp == 10)
		return (((in->width + 3) & ~3u) * 5 / 4 + 7) & ~7u;
	return (in->width + 15) & ~15u;
}

static int write_pgm(const char *path, const uint8_t *buf, const struct qcam_sensor_info *in,
		     uint32_t first_row, uint32_t rows, const char *comment)
{
	uint32_t stride = line_stride(in), w = in->width;
	FILE *f = fopen(path, "wb");
	if (!f) {
		LOG("open %s: %s\n", path, strerror(errno));
		return -1;
	}
	fprintf(f, "P5\n# %s\n%u %u\n255\n", comment, w, rows);
	uint8_t *line = malloc(w);
	for (uint32_t y = 0; y < rows; y++) {
		const uint8_t *src = buf + (size_t)(first_row + y) * stride;
		if (in->bpp == 10) /* 4 pixels = 4 MSB bytes + 1 byte of 2-bit LSBs: keep the MSBs */
			for (uint32_t x = 0; x < w; x++)
				line[x] = src[x + x / 4];
		else
			memcpy(line, src, w);
		fwrite(line, 1, w, f);
	}
	free(line);
	return fclose(f);
}

static void hexdump_line(const char *tag, const uint8_t *p, unsigned n)
{
	fprintf(stderr, "qcam: %s", tag);
	for (unsigned i = 0; i < n; i++)
		fprintf(stderr, " %02x", p[i]);
	fprintf(stderr, "\n");
}

/* ======================================================================================
 * main
 * ====================================================================================== */
static void usage(void)
{
	fprintf(stderr,
		"usage: qcam [options]\n"
		"  -n N            framesets to save (default %d)\n"
		"  -s N            framesets to drop first (default %d)\n"
		"  -o DIR          output directory (default /tmp) -> DIR/qcam_<cam>_<n>.pgm\n"
		"  -t S            hard timeout in seconds (default %d)\n"
		"  --stage N       1 load libs only, 2 + syncboss_init/qcamera_open (no camera power),\n"
		"                  3 + probe/init/get_sensor/info (powered, no streaming), 4 full (default)\n"
		"  --bpp 8|10      sensor bit depth (default %d)\n"
		"  --period US     frame period in us (default %d; 40000 for 50 Hz mains)\n"
		"  --tagmode N     syncboss_camera_set_frame_tag_mode value (default %d, -1 = skip)\n"
		"  --expmode M     tag (default, set_exposure_gain_tag), ctrl (+ controller tag call),\n"
		"                  plain (set_exposure_gain), none\n"
		"  --exp US        exposure in us (default %d)\n"
		"  --gain G        analog gain (default %.1f, sent as G*16)\n"
		"  --tag T         tag byte for the exposure settings (default %d)\n"
		"  --ctag T        tag byte for the controller exposure call (default 2)\n"
		"  --cexp US       controller-frame exposure (default 100)\n"
		"  --nbufs N       buffers per camera (default %d)\n"
		"  --keep-meta     keep the metadata line in the PGM (h+1 rows)\n"
		"  --meta-last     the metadata line is the last line instead of the first\n"
		"  --raw           also dump the whole buffer to DIR/qcam_<cam>_<n>.raw\n"
		"  --force         continue when SyncBoss and the camera HAL disagree on the camera count\n"
		"  --no-pump       do not read the SyncBoss stream (no shutter timestamps)\n",
		QCAM_DEFAULT_FRAMESETS, QCAM_DEFAULT_SKIP, QCAM_DEFAULT_TIMEOUT_S, QCAM_DEFAULT_BPP,
		QCAM_DEFAULT_PERIOD_US, QCAM_DEFAULT_TAG_MODE, QCAM_DEFAULT_EXPOSURE_US, QCAM_DEFAULT_GAIN,
		QCAM_DEFAULT_HEADSET_TAG, QCAM_DEFAULT_NBUFS);
}

int main(int argc, char **argv)
{
	int nsets = QCAM_DEFAULT_FRAMESETS, skip = QCAM_DEFAULT_SKIP, timeout_s = QCAM_DEFAULT_TIMEOUT_S;
	int stage = 4, bpp = QCAM_DEFAULT_BPP, period = QCAM_DEFAULT_PERIOD_US, tagmode = QCAM_DEFAULT_TAG_MODE;
	int exp_us = QCAM_DEFAULT_EXPOSURE_US, tag = QCAM_DEFAULT_HEADSET_TAG, ctag = 2, cexp_us = 100;
	int nbufs = QCAM_DEFAULT_NBUFS;
	double gain = QCAM_DEFAULT_GAIN;
	const char *outdir = "/tmp", *expmode = "tag";
	bool keep_meta = false, meta_last = false, raw = false, force = false, use_pump = true;

	enum { O_STAGE = 256, O_BPP, O_PERIOD, O_TAGMODE, O_EXPMODE, O_EXP, O_GAIN, O_TAG, O_CTAG, O_CEXP,
	       O_NBUFS, O_KEEPMETA, O_METALAST, O_RAW, O_FORCE, O_NOPUMP };
	static const struct option lo[] = {
		{ "stage", 1, 0, O_STAGE }, { "bpp", 1, 0, O_BPP }, { "period", 1, 0, O_PERIOD },
		{ "tagmode", 1, 0, O_TAGMODE }, { "expmode", 1, 0, O_EXPMODE }, { "exp", 1, 0, O_EXP },
		{ "gain", 1, 0, O_GAIN }, { "tag", 1, 0, O_TAG }, { "ctag", 1, 0, O_CTAG },
		{ "cexp", 1, 0, O_CEXP }, { "nbufs", 1, 0, O_NBUFS }, { "keep-meta", 0, 0, O_KEEPMETA },
		{ "meta-last", 0, 0, O_METALAST }, { "raw", 0, 0, O_RAW }, { "force", 0, 0, O_FORCE },
		{ "no-pump", 0, 0, O_NOPUMP }, { "help", 0, 0, 'h' }, { 0, 0, 0, 0 } };
	int c;
	while ((c = getopt_long(argc, argv, "n:s:o:t:h", lo, NULL)) != -1) {
		switch (c) {
		case 'n': nsets = atoi(optarg); break;
		case 's': skip = atoi(optarg); break;
		case 'o': outdir = optarg; break;
		case 't': timeout_s = atoi(optarg); break;
		case O_STAGE: stage = atoi(optarg); break;
		case O_BPP: bpp = atoi(optarg); break;
		case O_PERIOD: period = atoi(optarg); break;
		case O_TAGMODE: tagmode = atoi(optarg); break;
		case O_EXPMODE: expmode = optarg; break;
		case O_EXP: exp_us = atoi(optarg); break;
		case O_GAIN: gain = atof(optarg); break;
		case O_TAG: tag = atoi(optarg); break;
		case O_CTAG: ctag = atoi(optarg); break;
		case O_CEXP: cexp_us = atoi(optarg); break;
		case O_NBUFS: nbufs = atoi(optarg); break;
		case O_KEEPMETA: keep_meta = true; break;
		case O_METALAST: meta_last = true; break;
		case O_RAW: raw = true; break;
		case O_FORCE: force = true; break;
		case O_NOPUMP: use_pump = false; break;
		default: usage(); return c == 'h' ? 0 : 2;
		}
	}
	if (bpp != 8 && bpp != 10) {
		LOG("--bpp must be 8 or 10 (other values abort inside the camera libs)\n");
		return 2;
	}
	if (nbufs < 2 || nbufs > 16 || nsets < 1 || period < 1000 || exp_us < 1 || exp_us > 65535) {
		LOG("bad arguments\n");
		return 2;
	}
	install_signals();
	alarm(timeout_s);

	int rc = 1;
	if (load_libs())
		return 1;
	LOG("libraries loaded\n");
	if (stage <= 1) {
		LOG("stage 1 done\n");
		return 0;
	}

	/* 1. SyncBoss session. opts {0,0,1,1}: byte 2 = interface, byte 3 = telemetry off (sbimu). */
	uint8_t opts[16] = { 0, 0, 1, 1 };
	int r = sb.init(&st.sb, opts);
	LOG("syncboss_init -> %d handle=%p\n", r, st.sb);
	if (r || !st.sb) {
		st.sb = NULL;
		goto out;
	}
	if (use_pump && pthread_create(&st.pump, NULL, pump, NULL) == 0)
		st.pump_started = true;

	/* 2. Camera HAL (the sensors HAL opens it at construction, before the SyncBoss probe).
	 * control_init reads /sys/devices/virtual/misc/syncboss0/spi/control/num_cameras (device
	 * tree), enumerates /dev/media* ("msm_config", "msm_camera"), opens the config video node,
	 * disables the mm-qcamera-daemon path in the kernel and opens /dev/ion. */
	st.hal = qc.open(bpp == 10 ? 1 : 0);
	if (!st.hal) {
		LOG("qcamera_open failed\n");
		goto out;
	}
	st.nsensors = qc.num_sensors(st.hal) & 0xff;
	LOG("qcamera_open ok, %d sensors\n", st.nsensors);
	if (st.nsensors < 1 || st.nsensors > QCAM_MAX_CAMS)
		goto out;
	uint32_t bw = 0, bh = 0;
	qc.query_dims(bpp, 1, &bw, &bh);
	LOG("qcamera_query_buffer_dimensions(bpp %d, +meta) -> %ux%u\n", bpp, bw, bh);
	if (stage <= 2) {
		rc = 0;
		goto out;
	}

	/* 3. Power + init the sensors through the MCU (MontereyCameraProvider::cameraProbe 0x66104). */
	uint8_t mask = 0;
	r = sb.cam_probe(st.sb, &mask);
	LOG("syncboss_camera_probe -> %d mask=0x%02x\n", r, mask);
	if (r)
		goto out;
	st.cam_probed = true; /* the kernel snooped cmd 0x28 and powered the cameras */
	int ncams = __builtin_popcount(mask);
	if (ncams != st.nsensors) {
		LOG("SyncBoss reports %d cameras, camera HAL %d%s\n", ncams, st.nsensors,
		    force ? " (--force: continuing)" : "");
		if (!force || ncams == 0)
			goto out;
	}
	st.sb_ncams = (uint8_t)st.nsensors; /* HAL passes the camera-set size to start/stop/deinit */
	r = sb.set_bpp(st.sb, (uint8_t)bpp);
	LOG("syncboss_camera_set_bpp(%d) -> %d\n", bpp, r);
	r = sb.cam_init(st.sb, (uint8_t)ncams);
	LOG("syncboss_camera_init(%d) -> %d\n", ncams, r);
	if (r)
		goto out;
	st.cam_inited = true;

	struct qcam_sensor_info info[QCAM_MAX_CAMS];
	memset(info, 0, sizeof info);
	for (int i = 0; i < st.nsensors; i++) {
		st.sensor[i] = qc.get_sensor(st.hal, i);
		if (!st.sensor[i]) {
			LOG("qcamera_get_sensor(%d) failed: %s\n", i, strerror(errno));
			goto out;
		}
		qc.query_info(st.sensor[i], &info[i]);
		/* (qcamera_get_fd asserts -> abort when the sensor is not started: not called here) */
		LOG("cam %d: id %u %ux%u fmt 0x%x bpp %u code %u/%u packed %u stride %u\n", i,
		    info[i].sensor_id, info[i].width, info[i].height, info[i].format, info[i].bpp,
		    info[i].code_bits, info[i].px_per_code, info[i].packed, line_stride(&info[i]));
		if (!info[i].width || !info[i].height || info[i].width > 4096 || info[i].height > 4096 ||
		    (info[i].bpp != 8 && info[i].bpp != 10)) {
			LOG("unexpected sensor info, stopping\n");
			goto out;
		}
	}

	/* 4. Frame period (applyFramePeriod 0x65654), then read back the camera params. */
	r = sb.set_frame_rate(st.sb, (uint32_t)period);
	LOG("syncboss_camera_set_frame_rate(%d us) -> %d\n", period, r);
	uint8_t params[256];
	bool have_params = false;
	memset(params, 0, sizeof params);
	if (sb.params) {
		r = sb.params(st.sb, params);
		have_params = r == 0;
		LOG("syncboss_camera_params -> %d: sync_period %u, u16@6 %u, bpp %u\n", r,
		    *(uint32_t *)params, *(uint16_t *)(params + 6), params[28]);
		if (r == 0)
			hexdump_line("params[0..48]:", params, 48);
	}
	if (stage <= 3) {
		rc = 0;
		goto out;
	}

	/* 5. ISP streams (QCamera::StartCamera 0x6488): the lib allocates nbufs ION buffers. */
	for (int i = 0; i < st.nsensors; i++) {
		r = qc.start_sensor(st.sensor[i], &info[i].width, &info[i].format, nbufs, NULL, 1);
		LOG("qcamera_start_sensor(%d, nbufs %d) -> %d\n", i, nbufs, r);
		if (r)
			goto out;
		st.started[i] = true;
	}

	/* 6. Sensors start streaming (startCameras 0x65a94..0x65abc), then exposure (setTag). */
	if (tagmode >= 0) {
		r = sb.set_tag_mode(st.sb, (uint8_t)tagmode);
		LOG("syncboss_camera_set_frame_tag_mode(%d) -> %d\n", tagmode, r);
	}
	r = sb.start_streaming(st.sb, st.sb_ncams);
	LOG("syncboss_camera_start_streaming(%u) -> %d\n", st.sb_ncams, r);
	st.sb_streaming = true; /* stop it even if the call reported an error */
	if (r)
		goto out;

	uint16_t g16 = (uint16_t)(gain * 16.0 + 0.5);
	if (!strcmp(expmode, "tag") || !strcmp(expmode, "ctrl")) {
		struct sb_cam_egt cfg[4];
		memset(cfg, 0, sizeof cfg);
		for (int i = 0; i < 4; i++) {
			cfg[i].exposure = (uint16_t)exp_us;
			cfg[i].gain = g16;
			cfg[i].tag = (uint8_t)tag;
		}
		if (sb.set_exp_gain_tag) {
			r = sb.set_exp_gain_tag(st.sb, cfg);
			LOG("syncboss_camera_set_exposure_gain_tag(exp %d us, gain %u/16, tag %d) -> %d\n",
			    exp_us, g16, tag, r);
		}
		if (!strcmp(expmode, "ctrl") && sb.set_ctrl_exp_gain_tag) {
			for (int i = 0; i < 4; i++) {
				cfg[i].exposure = (uint16_t)cexp_us;
				cfg[i].tag = (uint8_t)ctag;
			}
			r = sb.set_ctrl_exp_gain_tag(st.sb, cfg);
			LOG("syncboss_camera_set_controller_exposure_gain_tag(exp %d us, tag %d) -> %d\n",
			    cexp_us, ctag, r);
		}
	} else if (!strcmp(expmode, "plain") && sb.set_exp_gain) {
		uint16_t e4[4], g4[4];
		for (int i = 0; i < 4; i++) {
			e4[i] = (uint16_t)exp_us;
			g4[i] = g16;
		}
		r = sb.set_exp_gain(st.sb, e4, g4, 4);
		LOG("syncboss_camera_set_exposure_gain(exp %d us, gain %u/16) -> %d\n", exp_us, g16, r);
	}

	/* 7. Capture loop. */
	mkdir(outdir, 0755);
	uint8_t *copy[QCAM_MAX_CAMS] = { 0 };
	size_t need[QCAM_MAX_CAMS];
	for (int i = 0; i < st.nsensors; i++) {
		need[i] = (size_t)line_stride(&info[i]) * (info[i].height + 1);
		copy[i] = malloc(need[i]);
	}
	int saved = 0, timeouts = 0;
	for (int set = 0; saved < nsets && !stop_req; set++) {
		struct qcam_frame fr[QCAM_MAX_CAMS];
		bool got[QCAM_MAX_CAMS] = { 0 };
		for (int i = 0; i < st.nsensors && !stop_req; i++) {
			struct qcam_frame *f = qc.dequeue(st.sensor[i], QCAM_DEQ_TIMEOUT_MS);
			if (!f) {
				timeouts++;
				continue;
			}
			st.held[i] = f;
			fr[i] = *f;
			size_t len = need[i];
			void *mm = ((void **)f)[-1];
			uint32_t mmlen = mm ? *(uint32_t *)((uint8_t *)mm + QCAM_MMBUF_LEN_OFF) : 0;
			if (mmlen && mmlen < len) {
				LOG("cam %d: buffer %u bytes < expected %zu, truncating copy\n", i, mmlen, len);
				len = mmlen;
				memset(copy[i], 0, need[i]);
			}
			if (f->data)
				memcpy(copy[i], f->data, len);
			got[i] = f->data != NULL;
			qc.enqueue(st.sensor[i], f);
			st.held[i] = NULL;
		}
		if (stop_req)
			break;
		if (set < skip) {
			if (timeouts > 20 * (st.nsensors + 1)) {
				LOG("too many dequeue timeouts (%d), giving up\n", timeouts);
				break;
			}
			continue;
		}
		bool any = false;
		for (int i = 0; i < st.nsensors; i++) {
			if (!got[i])
				continue;
			any = true;
			uint32_t meta_row = meta_last ? info[i].height : 0;
			const uint8_t *ml = copy[i] + (size_t)meta_row * line_stride(&info[i]);
			struct sb_frame_meta m;
			memset(&m, 0, sizeof m);
			int mr = -1;
			if (sb.decode_meta && have_params)
				mr = sb.decode_meta(ml, &m, params);
			uint64_t sh_us = 0, sh_mono = 0;
			bool sh = mr == 0 && find_shutter(m.sync_id, &sh_us, &sh_mono);
			char comment[256];
			snprintf(comment, sizeof comment,
				 "cam %d seq %u ts %lld.%09lld meta(rc %d) exp %u gain %u/16 tag %u id %u shutter_us %llu",
				 i, fr[i].sequence, (long long)fr[i].ts_sec, (long long)fr[i].ts_nsec, mr,
				 m.exposure, m.gain_x16, m.tag, m.sync_id, (unsigned long long)sh_us);
			printf("frameset %d %s%s\n", saved, comment, sh ? "" : " (no shutter match)");
			hexdump_line("meta line[0..15]:", ml, 16);
			char path[512];
			snprintf(path, sizeof path, "%s/qcam_%d_%d.pgm", outdir, i, saved);
			uint32_t first = keep_meta ? 0 : (meta_last ? 0 : 1);
			uint32_t rows = keep_meta ? info[i].height + 1 : info[i].height;
			if (write_pgm(path, copy[i], &info[i], first, rows, comment) == 0)
				printf("  wrote %s\n", path);
			if (raw) {
				snprintf(path, sizeof path, "%s/qcam_%d_%d.raw", outdir, i, saved);
				FILE *f = fopen(path, "wb");
				if (f) {
					fwrite(copy[i], 1, need[i], f);
					fclose(f);
					printf("  wrote %s (%zu bytes, stride %u)\n", path, need[i],
					       line_stride(&info[i]));
				}
			}
		}
		fflush(stdout);
		if (any)
			saved++;
		else if (timeouts > 20 * (st.nsensors + 1)) {
			LOG("too many dequeue timeouts (%d), giving up\n", timeouts);
			break;
		}
	}
	for (int i = 0; i < st.nsensors; i++)
		free(copy[i]);
	LOG("saved %d framesets, %d dequeue timeouts\n", saved, timeouts);
	rc = saved == nsets ? 0 : 1;

out:
	if (st.pump_started) {
		pthread_mutex_lock(&shutter_lock);
		LOG("syncboss records:");
		for (int i = 0; i < 256; i++)
			if (rec_counts[i])
				fprintf(stderr, " type%d=%u", i, rec_counts[i]);
		fprintf(stderr, " (shutter v2: %u)\n", shutter_count);
		pthread_mutex_unlock(&shutter_lock);
	}
	cleanup();
	if (stop_req && signal_count)
		LOG("interrupted\n");
	return rc;
}
