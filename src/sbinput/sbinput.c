// sbinput: read the Quest 1 Touch controllers (LCON) on native Linux through Meta's libsyncboss
// (bionic, via libhybris). See docs/controllers-native.md. Build (in Holo, one line):
//   gcc -O2 -o sbinput sbinput.c -I/opt/hybris/include -L/opt/hybris/lib -Wl,-rpath,/opt/hybris/lib
//       -lhybris-common -lpthread -lm
// Run: LD_PRELOAD=/opt/hybris/lib/libbionictls.so ./sbinput [seconds] [print interval] [options]
//   --haptic        short haptic pulse on each controller when it starts streaming
//   --amp N         haptic amplitude 0..255 (default 160)
//   --headset-imu   also enable the headset IMU (to compare time bases)
//   --no-fw-check   set SYNCBOSS_DISABLE_FW_VERSION_CHECK=1 (input data is dropped when the
//                   controller firmware differs from the version libsyncboss expects, 1.17.2)
//   --quiet         only print enumeration changes and the final summary
//
// Safety: only calls syncboss_init/deinit, imu_enable/disable, input_start/stop,
// input_enumerate, is_input_started, input_set_haptic and the stream wait. It never calls the
// pairing, unpairing, firmware update, calibration or LED/TX-power setters and writes no files.
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern void *android_dlopen(const char *filename, int flag);
extern void *android_dlsym(void *handle, const char *symbol);

/* ---- libsyncboss ABI (see docs/controllers-native.md) ---- */

struct sb_record { /* one entry of the library's stream history ring */
	uint64_t seq;
	uint32_t type;
	uint32_t pad;
	uint8_t data[96];
};

enum {
	SB_REC_HEADSET_IMU = 0,  /* pkt 0x50 */
	SB_REC_CTRL_IMU = 3,     /* pkt 0x8f, chunk 1 */
	SB_REC_CTRL_INPUT = 8,   /* pkt 0x8f, once per notification */
	SB_REC_CTRL_IMU_LOSS = 20,
	SB_REC_CTRL_ADC = 24,
};

struct sb_imu_event { /* type 0 (headset, id = 0) and type 3 (controller), 0x30 bytes */
	uint64_t id;
	uint64_t timestamp_us;
	uint32_t aux; /* headset: temperature (float bits); controller: 0 */
	float accel[3]; /* m/s^2 */
	float gyro[3];  /* rad/s */
	uint32_t pad;
};

struct sb_ctrl_input { /* type 8, 0x40 bytes, built by controller_process_notification() */
	uint64_t id;
	uint64_t timestamp_us; /* timestamp of the last IMU sample of the notification */
	uint8_t btn_ax, btn_by, btn_sys, btn_stick;
	uint8_t touch_ax, touch_by, touch_trigger, touch_stick, touch_thumbrest;
	uint8_t prox_ax, prox_by, prox_trigger, prox_stick, prox_thumbrest;
	int16_t cap_ax, cap_by, cap_trigger, cap_stick, cap_thumbrest; /* raw capsense */
	float stick_x, stick_y;          /* -1..1 */
	float grip_raw, trigger_raw;     /* 1.0 = released, 0.0 = fully pressed (HAL uses 1 - x) */
	uint8_t battery_percent;
	uint8_t pad[7];
};

struct sb_ctrl_imu_loss { /* type 20, 0x10 bytes */
	uint64_t id;
	uint32_t one;
	uint32_t missed; /* IMU samples missed before this one (2 ms nominal period) */
};

struct sb_input_device_info { /* syncboss_input_device_info_t, 0x148 bytes */
	uint64_t id;
	uint8_t connected;
	uint8_t asleep;
	uint8_t fw_up_to_date;
	uint8_t pad0;
	uint32_t type;    /* 2 = LCON (Quest 1 Touch) */
	uint32_t subtype; /* 1 = left, 2 = right, 3 = unconfigured, 4 = none */
	char desc[64];
	char serial[16];     /* assembly serial */
	char pcb_serial[16];
	char fw_version[64];
	char fw_expected[64];
	char imu_info[64];
	float accel_scale; /* g per LSB */
	float gyro_scale;  /* deg/s per LSB */
	uint32_t gyro_range_dps; /* constant 2000 */
	double battery_percent;
};

_Static_assert(sizeof(struct sb_record) == 112, "sb_record");
_Static_assert(sizeof(struct sb_imu_event) == 0x30, "imu");
_Static_assert(sizeof(struct sb_ctrl_input) == 0x40, "input");
_Static_assert(offsetof(struct sb_ctrl_input, stick_x) == 40, "stick");
_Static_assert(offsetof(struct sb_ctrl_input, grip_raw) == 48, "grip");
_Static_assert(offsetof(struct sb_ctrl_input, battery_percent) == 56, "battery");
_Static_assert(sizeof(struct sb_input_device_info) == 0x148, "devinfo");
_Static_assert(offsetof(struct sb_input_device_info, serial) == 0x54, "serial");
_Static_assert(offsetof(struct sb_input_device_info, accel_scale) == 0x134, "scale");
_Static_assert(offsetof(struct sb_input_device_info, battery_percent) == 0x140, "batt");

typedef int (*sb_init_t)(void **handle, const void *opts);
typedef int (*sb_handle_fn_t)(void *handle);
typedef int (*sb_wait_t)(void *handle, uint32_t timeout_ms, struct sb_record *out);
typedef int (*sb_enum_t)(void *handle, struct sb_input_device_info *out, int *inout_count, uint32_t *generation);
typedef int (*sb_started_t)(void *handle, bool *started);
typedef int (*sb_haptic_t)(void *handle, uint64_t id, uint8_t amplitude);

static sb_init_t sb_init;
static sb_handle_fn_t sb_deinit, imu_enable, imu_disable, input_start, input_stop;
static sb_wait_t sb_wait;
static sb_enum_t input_enumerate;
static sb_started_t is_input_started;
static sb_haptic_t set_haptic;

/* ---- state shared between the stream pump thread and main ---- */

#define MAX_DEV 8
struct dev {
	uint64_t id;
	int subtype; /* from enumeration, 0 = unknown */
	struct sb_imu_event imu;
	struct sb_ctrl_input in;
	unsigned n_imu, n_in, n_missed;
	uint64_t first_imu_ts, last_imu_ts, max_imu_gap;
	int64_t min_host_minus_ts; /* host CLOCK_MONOTONIC us - device ts us, minimum = least latency */
	bool haptic_done;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct dev devs[MAX_DEV];
static int n_devs;
static unsigned type_counts[256];
static unsigned n_head_imu, n_timeouts;
static int64_t head_min_host_minus_ts = INT64_MAX;
static volatile bool pump_run = true;
static volatile sig_atomic_t interrupted;
static void *h;

static int64_t mono_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static double now_s(void) { return mono_us() / 1e6; }

static struct dev *dev_get(uint64_t id) /* lock held */
{
	for (int i = 0; i < n_devs; i++)
		if (devs[i].id == id)
			return &devs[i];
	if (n_devs == MAX_DEV)
		return NULL;
	struct dev *d = &devs[n_devs++];
	memset(d, 0, sizeof(*d));
	d->id = id;
	d->min_host_minus_ts = INT64_MAX;
	return d;
}

/* libsyncboss decodes raw SyncBoss packets (including the radio notifications and the
 * "devices changed" events) inside syncboss_wait_on_stream_data_exclusive(), in the caller's
 * thread: this loop must run continuously while input is started. */
static void *pump(void *arg)
{
	(void)arg;
	struct sb_record rec;
	while (pump_run) {
		int r = sb_wait(h, 100, &rec);
		if (r == -11) {
			n_timeouts++;
			continue;
		}
		if (r) {
			fprintf(stderr, "stream wait -> %d\n", r);
			usleep(10000);
			continue;
		}
		int64_t host = mono_us();
		pthread_mutex_lock(&lock);
		type_counts[rec.type & 0xff]++;
		if (rec.type == SB_REC_HEADSET_IMU) {
			struct sb_imu_event e;
			memcpy(&e, rec.data, sizeof(e));
			n_head_imu++;
			if (host - (int64_t)e.timestamp_us < head_min_host_minus_ts)
				head_min_host_minus_ts = host - (int64_t)e.timestamp_us;
		} else if (rec.type == SB_REC_CTRL_IMU) {
			struct sb_imu_event e;
			memcpy(&e, rec.data, sizeof(e));
			struct dev *d = dev_get(e.id);
			if (d) {
				if (d->n_imu) {
					uint64_t gap = e.timestamp_us - d->last_imu_ts;
					if (gap > d->max_imu_gap)
						d->max_imu_gap = gap;
				} else {
					d->first_imu_ts = e.timestamp_us;
				}
				d->last_imu_ts = e.timestamp_us;
				d->imu = e;
				d->n_imu++;
				if (host - (int64_t)e.timestamp_us < d->min_host_minus_ts)
					d->min_host_minus_ts = host - (int64_t)e.timestamp_us;
			}
		} else if (rec.type == SB_REC_CTRL_INPUT) {
			struct sb_ctrl_input in;
			memcpy(&in, rec.data, sizeof(in));
			struct dev *d = dev_get(in.id);
			if (d) {
				d->in = in;
				d->n_in++;
			}
		} else if (rec.type == SB_REC_CTRL_IMU_LOSS) {
			struct sb_ctrl_imu_loss l;
			memcpy(&l, rec.data, sizeof(l));
			struct dev *d = dev_get(l.id);
			if (d)
				d->n_missed += l.missed;
		}
		pthread_mutex_unlock(&lock);
	}
	return NULL;
}

static const char *type_str(uint32_t t)
{
	static const char *names[] = {"?", "?", "LCON", "?", "Jedi", "Starlet", "Ruby", "EXT_SYNC", "Raven"};
	return t < sizeof(names) / sizeof(names[0]) ? names[t] : "?";
}

static const char *subtype_str(int s)
{
	static const char *names[] = {"invalid", "left", "right", "unconfigured", "none"};
	return s >= 0 && s < 5 ? names[s] : "?";
}

static uint32_t enum_generation;
static char last_enum_sig[512];

static void enumerate(bool force_print)
{
	struct sb_input_device_info info[16];
	int count = 16;
	memset(info, 0, sizeof(info));
	int r = input_enumerate(h, info, &count, &enum_generation);
	if (r) {
		printf("syncboss_input_enumerate -> %d\n", r);
		return;
	}
	char sig[512];
	int n = 0;
	for (int i = 0; i < count && n < (int)sizeof(sig) - 40; i++)
		n += snprintf(sig + n, sizeof(sig) - n, "%016llx:%d:%d;", (unsigned long long)info[i].id,
			      info[i].connected, info[i].asleep);
	sig[n] = 0;
	pthread_mutex_lock(&lock);
	for (int i = 0; i < count; i++) {
		struct dev *d = dev_get(info[i].id);
		if (d)
			d->subtype = info[i].subtype;
	}
	pthread_mutex_unlock(&lock);
	if (!force_print && !strcmp(sig, last_enum_sig))
		return;
	strcpy(last_enum_sig, sig);
	printf("enumeration (generation %u): %d device(s)\n", enum_generation, count);
	for (int i = 0; i < count; i++) {
		struct sb_input_device_info *e = &info[i];
		printf("  %016llx %-5s %-5s %s%s fw=%s (expected %s%s) serial=%.16s desc=\"%.64s\" battery=%.0f%%"
		       " accel_scale=%g gyro_scale=%g\n",
		       (unsigned long long)e->id, type_str(e->type), subtype_str(e->subtype),
		       e->connected ? "connected" : "paired, not connected", e->asleep ? " asleep" : "",
		       e->fw_version[0] ? e->fw_version : "-", e->fw_expected[0] ? e->fw_expected : "-",
		       e->connected && !e->fw_up_to_date ? ", MISMATCH: input dropped unless --no-fw-check" : "",
		       e->serial, e->desc, e->battery_percent, e->accel_scale, e->gyro_scale);
	}
	fflush(stdout);
}

static void print_dev(const struct dev *d)
{
	const struct sb_ctrl_input *in = &d->in;
	const struct sb_imu_event *m = &d->imu;
	float trig = 1.0f - in->trigger_raw, grip = 1.0f - in->grip_raw;
	printf("%016llx %-5s imu=%u in=%u | btn %s%s%s%s| touch %s%s%s%s%s| prox %s%s%s%s| stick % .2f % .2f trig %.2f grip %.2f"
	       " bat %u%%\n",
	       (unsigned long long)d->id, subtype_str(d->subtype), d->n_imu, d->n_in,
	       in->btn_ax ? "AX " : "", in->btn_by ? "BY " : "", in->btn_sys ? "SYS " : "", in->btn_stick ? "STK " : "",
	       in->touch_ax ? "ax " : "", in->touch_by ? "by " : "", in->touch_trigger ? "trig " : "",
	       in->touch_stick ? "stk " : "", in->touch_thumbrest ? "rest " : "", in->prox_ax ? "ax " : "",
	       in->prox_by ? "by " : "", in->prox_trigger ? "trig " : "", in->prox_stick ? "stk " : "", in->stick_x,
	       in->stick_y, trig, grip, in->battery_percent);
	printf("    ts=%llu accel=[% 7.3f % 7.3f % 7.3f] |a|=%.3f gyro=[% 7.3f % 7.3f % 7.3f] cap=[%d %d %d %d]\n",
	       (unsigned long long)m->timestamp_us, m->accel[0], m->accel[1], m->accel[2],
	       sqrtf(m->accel[0] * m->accel[0] + m->accel[1] * m->accel[1] + m->accel[2] * m->accel[2]), m->gyro[0],
	       m->gyro[1], m->gyro[2], in->cap_ax, in->cap_by, in->cap_trigger, in->cap_stick);
}

static void haptic_pulse(uint64_t id, uint8_t amp, double seconds)
{
	/* simple haptics: register 0x97 = amplitude; refreshed like the HAL does, then 0 */
	double t0 = now_s();
	while (now_s() - t0 < seconds) {
		set_haptic(h, id, amp);
		usleep(50000);
	}
	int r = set_haptic(h, id, 0);
	printf("haptic pulse %016llx amp %u -> %d\n", (unsigned long long)id, amp, r);
}

static void on_signal(int sig)
{
	(void)sig;
	interrupted = 1;
}

int main(int argc, char **argv)
{
	double duration = 20, period = 0.5;
	bool haptic = false, headset_imu = false, quiet = false;
	int amp = 160, npos = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--haptic"))
			haptic = true;
		else if (!strcmp(argv[i], "--amp") && i + 1 < argc)
			amp = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--headset-imu"))
			headset_imu = true;
		else if (!strcmp(argv[i], "--no-fw-check"))
			setenv("SYNCBOSS_DISABLE_FW_VERSION_CHECK", "1", 1);
		else if (!strcmp(argv[i], "--quiet"))
			quiet = true;
		else if (argv[i][0] != '-' && npos == 0)
			duration = atof(argv[i]), npos++;
		else if (argv[i][0] != '-' && npos == 1)
			period = atof(argv[i]), npos++;
		else {
			fprintf(stderr, "usage: %s [seconds] [print interval] [--haptic] [--amp N] [--headset-imu]"
					" [--no-fw-check] [--quiet]\n", argv[0]);
			return 2;
		}
	}
	if (amp < 0)
		amp = 0;
	if (amp > 255)
		amp = 255;

	void *lib = android_dlopen("libsyncboss.so", RTLD_NOW);
	if (!lib) {
		fprintf(stderr, "android_dlopen(libsyncboss.so) failed\n");
		return 1;
	}
	sb_init = (sb_init_t)android_dlsym(lib, "syncboss_init");
	sb_deinit = (sb_handle_fn_t)android_dlsym(lib, "syncboss_deinit");
	imu_enable = (sb_handle_fn_t)android_dlsym(lib, "syncboss_imu_enable");
	imu_disable = (sb_handle_fn_t)android_dlsym(lib, "syncboss_imu_disable");
	input_start = (sb_handle_fn_t)android_dlsym(lib, "syncboss_input_start");
	input_stop = (sb_handle_fn_t)android_dlsym(lib, "syncboss_input_stop");
	sb_wait = (sb_wait_t)android_dlsym(lib, "syncboss_wait_on_stream_data_exclusive");
	input_enumerate = (sb_enum_t)android_dlsym(lib, "syncboss_input_enumerate");
	is_input_started = (sb_started_t)android_dlsym(lib, "syncboss_is_input_started");
	set_haptic = (sb_haptic_t)android_dlsym(lib, "syncboss_input_set_haptic");
	if (!sb_init || !sb_wait || !input_start || !input_stop || !input_enumerate || !set_haptic) {
		fprintf(stderr, "missing symbols\n");
		return 1;
	}

	/* same options as sbimu: [2] = 1 (interface), [3] = 1 telemetry off (else init blocks on
	 * Android's hwservicemanager) */
	uint8_t opts[16] = {0, 0, 1, 1};
	int r = sb_init(&h, opts);
	printf("syncboss_init -> %d handle=%p\n", r, h);
	if (r)
		return 1;
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	pthread_t thr;
	pthread_create(&thr, NULL, pump, NULL);

	if (headset_imu && imu_enable)
		printf("syncboss_imu_enable -> %d\n", imu_enable(h));

	bool started = false;
	if (is_input_started && !is_input_started(h, &started))
		printf("MCU beacon mode before start: %s\n", started ? "yes (already running?)" : "no");
	r = input_start(h); /* asynchronous: queues MCU command 0x85 (start beacon mode) */
	printf("syncboss_input_start -> %d\n", r);
	bool input_on = r == 0;
	for (int i = 0; input_on && is_input_started && i < 20; i++) {
		usleep(100000);
		if (!is_input_started(h, &started) && started)
			break;
	}
	if (is_input_started)
		printf("MCU beacon mode: %s\n", started ? "on" : "NOT on");
	enumerate(true);
	printf("press a button on each controller to wake it up\n");
	fflush(stdout);

	double t0 = now_s(), last_print = t0, last_enum = t0;
	while (!interrupted && now_s() - t0 < duration) {
		usleep(20000);
		double t = now_s();
		if (t - last_enum >= 1.0) {
			last_enum = t;
			enumerate(false);
		}
		if (haptic && input_on) {
			uint64_t todo = 0;
			pthread_mutex_lock(&lock);
			for (int i = 0; i < n_devs; i++)
				if (!devs[i].haptic_done && devs[i].n_in > 50) {
					devs[i].haptic_done = true;
					todo = devs[i].id;
					break;
				}
			pthread_mutex_unlock(&lock);
			if (todo)
				haptic_pulse(todo, (uint8_t)amp, 0.3);
		}
		if (!quiet && t - last_print >= period) {
			last_print = t;
			pthread_mutex_lock(&lock);
			printf("t=%5.2f\n", t - t0);
			for (int i = 0; i < n_devs; i++)
				if (devs[i].n_imu || devs[i].n_in)
					print_dev(&devs[i]);
			pthread_mutex_unlock(&lock);
			fflush(stdout);
		}
	}
	double elapsed = now_s() - t0;

	pthread_mutex_lock(&lock);
	int nd = n_devs;
	uint64_t ids[MAX_DEV];
	for (int i = 0; i < nd; i++)
		ids[i] = devs[i].id;
	pthread_mutex_unlock(&lock);
	for (int i = 0; i < nd; i++)
		set_haptic(h, ids[i], 0);
	if (input_on)
		printf("syncboss_input_stop -> %d\n", input_stop(h));
	usleep(300000); /* keep pumping while beacon mode stops */
	pump_run = false;
	pthread_join(thr, NULL);
	if (headset_imu && imu_disable)
		printf("syncboss_imu_disable -> %d\n", imu_disable(h));

	printf("summary over %.1f s (stream timeouts %u):\n", elapsed, n_timeouts);
	for (int i = 0; i < n_devs; i++) {
		struct dev *d = &devs[i];
		double span = d->n_imu > 1 ? (d->last_imu_ts - d->first_imu_ts) / 1e6 : 0;
		printf("  %016llx %-5s imu %u (%.0f Hz by device ts), input %u, missed %u, max imu gap %llu us,"
		       " min(host-ts) %lld us\n",
		       (unsigned long long)d->id, subtype_str(d->subtype), d->n_imu, span > 0 ? (d->n_imu - 1) / span : 0,
		       d->n_in, d->n_missed, (unsigned long long)d->max_imu_gap,
		       d->min_host_minus_ts == INT64_MAX ? 0LL : (long long)d->min_host_minus_ts);
	}
	if (n_head_imu)
		printf("  headset imu %u, min(host-ts) %lld us (same value as controllers => same time base)\n",
		       n_head_imu, (long long)head_min_host_minus_ts);
	for (int i = 0; i < 256; i++)
		if (type_counts[i])
			printf("  record type %d: %u\n", i, type_counts[i]);
	if (sb_deinit)
		sb_deinit(h);
	return 0;
}
