// sbimu: read the headset IMU on native Linux through Meta's libsyncboss (bionic, via libhybris).
// See docs/syncboss-imu.md. Build (in Holo):
//   gcc -O2 -o sbimu sbimu.c -I/opt/hybris/include -L/opt/hybris/lib -Wl,-rpath,/opt/hybris/lib -lhybris-common
// Run: LD_PRELOAD=/opt/hybris/lib/libbionictls.so ./sbimu [seconds] [print interval]
#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern void *android_dlopen(const char *filename, int flag);
extern void *android_dlsym(void *handle, const char *symbol);

struct sb_record {
	uint64_t seq;
	uint32_t type;
	uint32_t pad;
	uint8_t data[96];
};

struct sb_imu_event {
	uint64_t zero;
	uint64_t timestamp;
	uint32_t aux;
	float accel[3];
	float gyro[3];
	uint32_t pad;
};

typedef int (*sb_init_t)(void **handle, const void *opts);
typedef int (*sb_handle_fn_t)(void *handle);
typedef int (*sb_wait_t)(void *handle, uint32_t timeout_ms, struct sb_record *out);

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	double duration = argc > 1 ? atof(argv[1]) : 5;
	double period = argc > 2 ? atof(argv[2]) : 0.5; /* print interval, s */
	void *lib = android_dlopen("libsyncboss.so", RTLD_NOW);
	if (!lib) {
		fprintf(stderr, "android_dlopen(libsyncboss.so) failed\n");
		return 1;
	}
	sb_init_t sb_init = (sb_init_t)android_dlsym(lib, "syncboss_init");
	sb_handle_fn_t sb_deinit = (sb_handle_fn_t)android_dlsym(lib, "syncboss_deinit");
	sb_handle_fn_t imu_enable = (sb_handle_fn_t)android_dlsym(lib, "syncboss_imu_enable");
	sb_handle_fn_t imu_disable = (sb_handle_fn_t)android_dlsym(lib, "syncboss_imu_disable");
	sb_wait_t wait = (sb_wait_t)android_dlsym(lib, "syncboss_wait_on_stream_data_exclusive");
	if (!sb_init || !imu_enable || !wait) {
		fprintf(stderr, "missing symbols\n");
		return 1;
	}

	/* syncboss_init_options_t (16 bytes, defaults 00 00 01 00 ... at 0x24ab8):
	 * [1] skip firmware version check, [2] must be 1 (interface), [3] disable telemetry
	 * (telemetry init blocks forever waiting for Android's hwservicemanager), [8] log handler */
	uint8_t opts[16] = {0, 0, 1, 1};
	void *h = NULL;
	int r = sb_init(&h, opts);
	printf("syncboss_init -> %d handle=%p\n", r, h);
	if (r)
		return 1;
	r = imu_enable(h);
	printf("syncboss_imu_enable -> %d\n", r);

	unsigned counts[256] = {0};
	unsigned n_imu = 0, n_timeout = 0;
	uint64_t first_ts = 0, last_ts = 0;
	double t0 = now_s(), last_print = 0;
	struct sb_record rec;
	while (now_s() - t0 < duration) {
		r = wait(h, 100, &rec);
		if (r == -11) {
			n_timeout++;
			continue;
		}
		if (r) {
			printf("wait -> %d\n", r);
			break;
		}
		counts[rec.type & 0xff]++;
		if (rec.type != 0)
			continue;
		struct sb_imu_event e;
		memcpy(&e, rec.data, sizeof(e));
		if (!n_imu++)
			first_ts = e.timestamp;
		last_ts = e.timestamp;
		double t = now_s() - t0;
		if (t - last_print >= period) {
			last_print = t;
			printf("t=%5.2f ts=%llu aux=%u accel=[% 7.3f % 7.3f % 7.3f] |a|=%.3f gyro=[% 7.4f % 7.4f % 7.4f]\n", t,
			       (unsigned long long)e.timestamp, e.aux, e.accel[0], e.accel[1], e.accel[2],
			       sqrt(e.accel[0] * e.accel[0] + e.accel[1] * e.accel[1] + e.accel[2] * e.accel[2]),
			       e.gyro[0], e.gyro[1], e.gyro[2]);
			fflush(stdout);
		}
	}
	double elapsed = now_s() - t0;
	printf("imu samples: %u in %.2f s (%.0f Hz), timeouts %u, ts span %llu\n", n_imu, elapsed, n_imu / elapsed,
	       n_timeout, (unsigned long long)(last_ts - first_ts));
	for (int i = 0; i < 256; i++)
		if (counts[i])
			printf("  record type %d: %u\n", i, counts[i]);
	if (imu_disable)
		printf("syncboss_imu_disable -> %d\n", imu_disable(h));
	if (sb_deinit)
		sb_deinit(h);
	return 0;
}
