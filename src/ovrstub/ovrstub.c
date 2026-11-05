// SPDX-License-Identifier: MIT
/*
 * ovrstub: stand-ins for the system_server binder services that Meta's Quest 1 tracking stack
 * (trackingservice, vrfocusserver, calibrationserver, cameramuxmodeservice...) expects, so that
 * stack can run natively under Holo without Android's Java framework.
 *
 * Plain C on the raw binder protocol (/dev/binder, protocol version 8): no libbinder, no Android
 * build system. Every service is one binder node. Replies are "no exception" followed by zeros,
 * which AIDL and the hand-written libbinder proxies decode as 0 / false / null / empty, plus a few
 * overrides:
 *   permission       checkPermission (code 1) -> granted
 *   vrpowermanager   registerClient (code 1) keeps the client binder and calls its oneway
 *                    IVrPowerManagerClient::onStateChange(int) (code 1) with the current state.
 *                    libsensorclientutils maps PowerState = 3 - state, with PowerState
 *                    {0 SLEEP, 1 STANDBY, 2 RUNNING}, so 1 = RUNNING, 2 = STANDBY, 3 = SLEEP.
 *                    SIGUSR1 -> broadcast RUNNING, SIGUSR2 -> broadcast SLEEP, SIGHUP -> STANDBY.
 *   VrApi            (vrapiserver's IVrApiService) setThreadPolicy (code 1): trackingservice asks it
 *                    for the scheduling class of its vision threads and aborts if the call fails
 *                    ("Failed to set thread priority for thread type 'Mapper'"). Applied here as
 *                    /system/etc/threadpolicy.conf describes it (vrapiserver's Android10Scheduler),
 *                    for threads of the calling process only. Everything else vrapiserver serves
 *                    (clocks, performance locks, display, thermal) gets the zero reply: nothing here
 *                    may change CPU/GPU frequencies.
 * Every incoming call is logged (service, code, caller pid/uid, interface token) so missing
 * behaviour shows up in the journal.
 *
 * Usage: ovrstub [-v] [-s state] [--skip name,name] [--only name,name] [--extra name,name] [--no-sched]
 * Runs as root; needs /system/bin/servicemanager on /dev/binder (context manager).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <linux/android/binder.h>

#define PACK(a, b, c, d) (((a) << 24) | ((b) << 16) | ((c) << 8) | (d))
#define PING_TRANSACTION PACK('_', 'P', 'N', 'G')
#define DUMP_TRANSACTION PACK('_', 'D', 'M', 'P')
#define SHELL_COMMAND_TRANSACTION PACK('_', 'C', 'M', 'D')
#define INTERFACE_TRANSACTION PACK('_', 'N', 'T', 'F')
#define SYSPROPS_TRANSACTION PACK('_', 'S', 'P', 'R')
#define SVC_MGR_ADD_SERVICE 3

enum kind { K_GENERIC, K_PERMISSION, K_VRPOWER, K_VRAPI, K_REMOTE };

struct svc {
	const char *name, *desc;
	int kind;
	int enabled;
};

/* Names from plat_service_contexts / the clients' getService strings, descriptors from the
 * clients' Bp* proxies (UTF-16 strings in the binaries). See docs/tracking-6dof.md. */
static struct svc svcs[] = {
	{"permission", "android.os.IPermissionController", K_PERMISSION, 1},
	{"appops", "com.android.internal.app.IAppOpsService", K_GENERIC, 1},
	{"vrpowermanager", "oculus.internal.power.IVrPowerManager", K_VRPOWER, 1},
	{"settingsproxy", "oculus.internal.ISettingsProxy", K_GENERIC, 1},
	{"OVRRemoteService", "oculus.internal.remote.IRemoteService", K_REMOTE, 1},
	{"power", "android.os.IPowerManager", K_GENERIC, 1},
	{"GatekeeperService", "oculus.internal.IGatekeeperService", K_GENERIC, 1},
	{"notificationProxy", "oculus.internal.INotificationProxy", K_GENERIC, 1},
	{"activity", "android.app.IActivityManager", K_GENERIC, 1},
	{"package_native", "android.content.pm.IPackageManagerNative", K_GENERIC, 1},
	{"vrfocushelper", "oculus.internal.IVrFocusHelper", K_GENERIC, 1},
	{"OculusWindowManager", "oculus.internal.IOculusWindowManager", K_GENERIC, 1},
	{"PreferencesAppInfoInquirer", "oculus.internal.IPreferencesAppInfoInquirer", K_GENERIC, 1},
	{"cm_wifi", "oculus.internal.ICMHeadsetRole", K_GENERIC, 1},
	{"MrSystemService", "oculus.internal.IMrSystemService", K_GENERIC, 1},
	{"TelemetryService", "oculus.internal.telemetry.ITelemetryService", K_GENERIC, 1},
	{"VrApi", "oculus.internal.IVrApiService", K_VRAPI, 1},
	/* trackingservice's inside-out engine connects to it while waking up; when it is missing the
	 * cleanup code in libossdk (RuntimeIpcManagerImpl dtor) dereferences a null broker and the
	 * service dies with SIGSEGV (seen live). Same finding as the Quest 2 port. */
	{"RuntimeIpcBroker", "oculus.internal.IRuntimeIpcBroker", K_GENERIC, 1},
	/* Optional (--extra): stand-ins for real daemons, only to probe trackingservice's startup
	 * while the real one cannot run (cameramuxmodeservice waits for the sensors HAL). */
	{"cameramuxmodeservice", "oculus.internal.ICameraMuxModeService", K_GENERIC, 0},
};
#define NSVC ((int)(sizeof(svcs) / sizeof(svcs[0])))

/* Calls whose first return value must be non-zero (int32/bool 1 after the status). Codes come
 * from the clients' Bp* proxies (transact immediates). */
static const struct ret1 {
	const char *svc;
	uint32_t code;
	const char *what;
} ret1[] = {
	{"permission", 1, "checkPermission -> granted"},
	/* vrfocusserver ConnectionManager::initWindowLayoutObserver retries every 500 ms until true */
	{"OculusWindowManager", 1, "registerWindowLayoutObserver -> true"},
	{"OculusWindowManager", 3, "registerSystemKeyEventHandler -> true"},
	{"OculusWindowManager", 4, "unregisterSystemKeyEventHandler -> true"},
};

static int verbose;
static int no_sched; /* --no-sched: log setThreadPolicy, do not apply it */
static int bfd = -1;
static volatile sig_atomic_t sig_state; /* pending power state from a signal, 0 = none */
static int then_state; /* --then N: one second after the first client registers, broadcast state N */
static volatile sig_atomic_t then_pending, tick;
static int remote_model = 2; /* RemoteStatus model field, --model N */
static int power_state = 1;             /* 1 RUNNING, 2 STANDBY, 3 SLEEP */

#define MAXCLIENTS 32
static struct client {
	uint32_t handle;
	int used, notified;
} clients[MAXCLIENTS];

static void logf_(const char *fmt, ...)
{
	struct timespec ts;
	va_list ap;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	fprintf(stderr, "[%5ld.%03ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* ---- parcels ---- */
struct parcel {
	uint8_t data[2048];
	size_t len;
	binder_size_t offs[8];
	size_t noffs;
};

static void p_raw(struct parcel *p, const void *d, size_t n)
{
	size_t pad = (n + 3) & ~(size_t)3;
	if (p->len + pad > sizeof(p->data))
		return;
	memcpy(p->data + p->len, d, n);
	memset(p->data + p->len + n, 0, pad - n);
	p->len += pad;
}
static void p_i32(struct parcel *p, int32_t v) { p_raw(p, &v, 4); }
static void p_zeros(struct parcel *p, size_t n)
{
	while (n >= 4) {
		p_i32(p, 0);
		n -= 4;
	}
}
static void p_str16(struct parcel *p, const char *s)
{
	size_t n = strlen(s);
	uint16_t buf[256];
	if (n > 255)
		n = 255;
	for (size_t i = 0; i < n; i++)
		buf[i] = (unsigned char)s[i];
	buf[n] = 0;
	p_i32(p, (int32_t)n);
	p_raw(p, buf, (n + 1) * 2);
}
/* Parcel::writeInterfaceToken of Android 10: strict-mode policy, work source uid, descriptor */
static void p_token(struct parcel *p, const char *desc)
{
	p_i32(p, (int32_t)0x80000000); /* STRICT_MODE_PENALTY_GATHER */
	p_i32(p, -1);                  /* IPCThreadState::kUnsetWorkSource */
	p_str16(p, desc);
}
static void p_binder(struct parcel *p, uintptr_t ptr)
{
	struct flat_binder_object o;
	memset(&o, 0, sizeof(o));
	o.hdr.type = BINDER_TYPE_BINDER;
	o.flags = 0x7f | FLAT_BINDER_FLAG_ACCEPTS_FDS;
	o.binder = ptr;
	o.cookie = ptr;
	if (p->noffs < 8)
		p->offs[p->noffs++] = p->len;
	p_raw(p, &o, sizeof(o));
}

static void p_fd(struct parcel *p, int fd)
{
	struct flat_binder_object o;
	memset(&o, 0, sizeof(o));
	o.hdr.type = BINDER_TYPE_FD;
	o.flags = 0x7f;
	o.handle = (uint32_t)fd;
	if (p->noffs < 8)
		p->offs[p->noffs++] = p->len;
	p_raw(p, &o, sizeof(o));
}

struct rd {
	const uint8_t *p;
	size_t len, pos;
};
static int32_t r_i32(struct rd *r)
{
	int32_t v = 0;
	if (r->pos + 4 <= r->len)
		memcpy(&v, r->p + r->pos, 4);
	r->pos += 4;
	return v;
}
/* String16 -> ASCII (non-ASCII as '?'); returns 0 on a malformed string */
static int r_str16(struct rd *r, char *out, size_t outsz)
{
	int32_t n = r_i32(r);
	out[0] = 0;
	if (n < 0 || r->pos + (size_t)(n + 1) * 2 > r->len)
		return 0;
	size_t i;
	for (i = 0; i < (size_t)n && i + 1 < outsz; i++) {
		uint16_t c;
		memcpy(&c, r->p + r->pos + i * 2, 2);
		out[i] = c < 128 && c >= 32 ? (char)c : '?';
	}
	out[i] = 0;
	r->pos += (((size_t)(n + 1) * 2) + 3) & ~(size_t)3;
	return 1;
}

/* ---- binder plumbing ---- */
static int bwrite(const void *buf, size_t len)
{
	struct binder_write_read bwr;
	memset(&bwr, 0, sizeof(bwr));
	bwr.write_size = len;
	bwr.write_buffer = (uintptr_t)buf;
	while (ioctl(bfd, BINDER_WRITE_READ, &bwr) < 0) {
		if (errno != EINTR) {
			logf_("BINDER_WRITE_READ (write): %s", strerror(errno));
			return -1;
		}
	}
	return 0;
}

static void bc_ptr_cookie(uint32_t cmd, binder_uintptr_t ptr, binder_uintptr_t cookie)
{
	struct __attribute__((packed)) {
		uint32_t cmd;
		struct binder_ptr_cookie pc;
	} w = {cmd, {ptr, cookie}};
	bwrite(&w, sizeof(w));
}

static void bc_handle(uint32_t cmd, uint32_t handle)
{
	uint32_t w[2] = {cmd, handle};
	bwrite(w, sizeof(w));
}

static void bc_free_buffer(binder_uintptr_t buf)
{
	struct __attribute__((packed)) {
		uint32_t cmd;
		binder_uintptr_t buf;
	} w = {BC_FREE_BUFFER, buf};
	bwrite(&w, sizeof(w));
}

static int bc_txn(uint32_t cmd, uint32_t handle, uint32_t code, uint32_t flags, struct parcel *p)
{
	struct __attribute__((packed)) {
		uint32_t cmd;
		struct binder_transaction_data t;
	} w;
	memset(&w, 0, sizeof(w));
	w.cmd = cmd;
	w.t.target.handle = handle;
	w.t.code = code;
	w.t.flags = flags;
	w.t.data_size = p->len;
	w.t.offsets_size = p->noffs * sizeof(binder_size_t);
	w.t.data.ptr.buffer = (uintptr_t)p->data;
	w.t.data.ptr.offsets = (uintptr_t)p->offs;
	return bwrite(&w, sizeof(w));
}

static void notify_client(int i)
{
	struct parcel p = {0};
	p_token(&p, "oculus.internal.power.IVrPowerManagerClient");
	p_i32(&p, power_state);
	logf_("vrpowermanager: onStateChange(%d) -> client handle %u", power_state, clients[i].handle);
	bc_txn(BC_TRANSACTION, clients[i].handle, 1, TF_ONE_WAY | TF_ACCEPT_FDS, &p);
	clients[i].notified = 1;
}

static void add_client(uint32_t handle)
{
	int i, freeslot = -1;
	for (i = 0; i < MAXCLIENTS; i++) {
		if (clients[i].used && clients[i].handle == handle) {
			clients[i].notified = 0; /* re-registration: notify again */
			return;
		}
		if (!clients[i].used && freeslot < 0)
			freeslot = i;
	}
	if (freeslot < 0) {
		logf_("vrpowermanager: too many clients");
		return;
	}
	clients[freeslot].handle = handle;
	clients[freeslot].used = 1;
	clients[freeslot].notified = 0;
	/* keep the remote node alive after the transaction buffer is freed, and hear about its death */
	bc_handle(BC_INCREFS, handle);
	bc_handle(BC_ACQUIRE, handle);
	struct __attribute__((packed)) {
		uint32_t cmd;
		uint32_t handle;
		binder_uintptr_t cookie;
	} w = {BC_REQUEST_DEATH_NOTIFICATION, handle, (binder_uintptr_t)(freeslot + 1)};
	bwrite(&w, sizeof(w));
	logf_("vrpowermanager: client registered, handle %u (slot %d)", handle, freeslot);
	if (then_state)
		then_pending = 1;
}

static void drop_client(int i, const char *why)
{
	if (i < 0 || i >= MAXCLIENTS || !clients[i].used)
		return;
	logf_("vrpowermanager: dropping client handle %u (%s)", clients[i].handle, why);
	bc_handle(BC_RELEASE, clients[i].handle);
	bc_handle(BC_DECREFS, clients[i].handle);
	clients[i].used = 0;
}

static uint32_t first_handle(const struct binder_transaction_data *t, int *ok)
{
	const binder_size_t *offs = (const binder_size_t *)(uintptr_t)t->data.ptr.offsets;
	size_t n = t->offsets_size / sizeof(binder_size_t);
	*ok = 0;
	for (size_t i = 0; i < n; i++) {
		if (offs[i] + sizeof(struct flat_binder_object) > t->data_size)
			continue;
		const struct flat_binder_object *o =
			(const void *)((const uint8_t *)(uintptr_t)t->data.ptr.buffer + offs[i]);
		if (o->hdr.type == BINDER_TYPE_HANDLE || o->hdr.type == BINDER_TYPE_WEAK_HANDLE) {
			*ok = 1;
			return o->handle;
		}
	}
	return 0;
}

static const char *thread_name(pid_t pid, pid_t tid)
{
	static char name[32];
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/task/%d/comm", pid, tid);
	FILE *f = fopen(path, "r");
	name[0] = 0;
	if (f) {
		if (fgets(name, sizeof(name), f))
			name[strcspn(name, "\n")] = 0;
		fclose(f);
	}
	return name;
}

static void hexdump(const uint8_t *d, size_t n)
{
	char line[3 * 32 + 1];
	for (size_t i = 0; i < n; i += 32) {
		size_t k = 0;
		for (size_t j = i; j < n && j < i + 32; j++)
			k += snprintf(line + k, sizeof(line) - k, "%02x ", d[j]);
		logf_("    %04zx: %s", i, line);
	}
}

/* IVrApiService::setThreadPolicy(int policy, int priority, int tid). The policy enum follows the
 * THREAD_POLICIES order of /system/etc/threadpolicy.conf; vrapiserver ignores `priority` and
 * applies {sched policy, nice, rtprio, CPU set} as that file sets them (cpuset "11110000" =
 * CPUs 0-3). It always answers "ok", even when the policy is invalid or cannot be applied. */
static const struct thread_policy {
	const char *name;
	int sched, nice, rtprio;
	unsigned cpus; /* 0 = leave the affinity alone */
} thread_policies[] = {
	{"BACKGROUND", SCHED_OTHER, 19, 0, 0x0f},
	{"COMPUTE", SCHED_BATCH, 0, 0, 0},
	{"NORMAL", SCHED_OTHER, 0, 0, 0},
	{"ELEVATED", SCHED_FIFO, 0, 48, 0xf0},
	{"ISOCHRONOUS", SCHED_FIFO, 0, 1, 0x0f},
	{"LATENCY_CRITICAL", SCHED_FIFO, 0, 49, 0x0f},
};
#define NPOLICIES ((int)(sizeof(thread_policies) / sizeof(thread_policies[0])))

struct sched_attr_ { /* struct sched_attr (glibc has no wrapper) */
	uint32_t size, sched_policy;
	uint64_t sched_flags;
	int32_t sched_nice;
	uint32_t sched_priority;
	uint64_t sched_runtime, sched_deadline, sched_period;
};

static void set_thread_policy(pid_t caller, struct rd *r)
{
	int32_t policy = r_i32(r), prio = r_i32(r), tid = r_i32(r);
	char path[64];
	const char *res = "applied";
	snprintf(path, sizeof(path), "/proc/%d/task/%d", caller, tid);
	if (policy < 0 || policy >= NPOLICIES || prio < 0 || prio > 2) {
		res = "invalid, ignored";
	} else if (tid <= 0 || access(path, F_OK) != 0) {
		res = "not a thread of the caller, ignored";
	} else if (no_sched) {
		res = "not applied (--no-sched)";
	} else {
		const struct thread_policy *tp = &thread_policies[policy];
		struct sched_attr_ a = {.size = sizeof(a), .sched_policy = (uint32_t)tp->sched,
					.sched_nice = tp->nice, .sched_priority = (uint32_t)tp->rtprio};
		if (tp->cpus) {
			cpu_set_t set;
			CPU_ZERO(&set);
			for (int c = 0; c < 8; c++)
				if (tp->cpus & (1u << c))
					CPU_SET(c, &set);
			if (sched_setaffinity(tid, sizeof(set), &set) < 0)
				res = "sched_setaffinity failed";
		}
		if (syscall(SYS_sched_setattr, tid, &a, 0) < 0)
			res = strerror(errno);
	}
	logf_("VrApi: setThreadPolicy(%s, %d) for tid %d (%s) of pid %d: %s",
	      policy >= 0 && policy < NPOLICIES ? thread_policies[policy].name : "?", prio, tid,
	      thread_name(caller, tid), caller, res);
}


/* ---- OVRRemoteService: the controller list for trackingservice (oculus.internal.remote.IRemoteService) ----
 * Without it libossdk reports "None connected" although the sensors HAL sees both controllers.
 * Transaction codes and the RemoteStatus layout are from libremoteservice.so (Bp proxies and
 * RemoteStatus::writeToParcel). The controller ids come from /run/q1track/controllers, one
 * "<16 hex id> <subtype 1 left | 2 right>" line per controller, written by fakelogd from the
 * sensors HAL log (ids are never baked into the repo). The idea of answering
 * these calls and announcing controllers through the status callback comes from the Quest 2 port
 * (github.com/URFAVF3MBOY/quest2-steamos); this is an independent implementation. */
#define CTL_FILE "/run/q1track/controllers"
static struct rctl {
	int used;
	char sid[20];
	uint64_t id;
	int type; /* 0 left, 1 right */
	int announced;
	int fd_given; /* trackingservice asked for its event fd (getDeviceEventFd): it has the controller */
	int ticks;    /* seconds since the last onStatus */
	int evfd[2];
} rctl[2];
static uint32_t remote_cb; /* IRemoteServiceStatusCallback handle, 0 = none */

static void p_status(struct parcel *p, const struct rctl *c)
{
	p_i32(p, 1); /* parcelable present */
	p_str16(p, c->sid);  /* device id */
	p_str16(p, "0.0.0"); /* firmware */
	p_str16(p, "0x08");  /* hardware revision */
	p_str16(p, "unknown"); /* serial */
	p_raw(p, &(double){0.0}, 8);
	p_i32(p, 100); /* battery % */
	p_i32(p, 3);   /* state: connected, active */
	p_i32(p, 0);   /* error list: empty */
	p_i32(p, remote_model);
	p_i32(p, 1);   /* tracking: none (the tracker reports its own) */
	p_i32(p, 0);   /* brightness */
}

static void remote_reload(void)
{
	FILE *f = fopen(CTL_FILE, "r");
	char line[128];
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char hex[32];
		int sub;
		if (sscanf(line, "%16s %d", hex, &sub) != 2 || strlen(hex) != 16 || (sub != 1 && sub != 2))
			continue;
		struct rctl *c = &rctl[sub - 1];
		if (c->used && !strcmp(c->sid, hex))
			continue;
		memset(c, 0, sizeof(*c));
		c->used = 1;
		snprintf(c->sid, sizeof(c->sid), "%s", hex);
		c->id = strtoull(hex, NULL, 16);
		c->type = sub - 1;
		c->evfd[0] = c->evfd[1] = -1;
		socketpair(AF_UNIX, SOCK_SEQPACKET, 0, c->evfd);
		logf_("OVRRemoteService: controller %s (%s)", hex, sub == 1 ? "left" : "right");
	}
	fclose(f);
}

static void remote_announce(void)
{
	if (!remote_cb || power_state == 3)
		return;
	for (int i = 0; i < 2; i++) {
		if (!rctl[i].used)
			continue;
		/* trackingservice only takes a controller the sensors HAL already reports connected: when the
		 * onStatus comes first (the HAL connects them a second later), it ignores it. Repeat it every
		 * 3 s until trackingservice asks for the controller's event fd. */
		if (rctl[i].announced && (rctl[i].fd_given || ++rctl[i].ticks < 3))
			continue;
		rctl[i].ticks = 0;
		struct parcel p = {0};
		p_token(&p, "oculus.internal.remote.IRemoteServiceStatusCallback");
		p_i32(&p, rctl[i].type);
		p_status(&p, &rctl[i]);
		bc_txn(BC_TRANSACTION, remote_cb, 1, TF_ONE_WAY, &p);
		rctl[i].announced = 1;
		logf_("OVRRemoteService: onStatus(type %d) -> callback handle %u", rctl[i].type, remote_cb);
	}
}

static void remote_drain(void)
{
	char buf[512];
	for (int i = 0; i < 2; i++)
		if (rctl[i].used && rctl[i].evfd[0] >= 0)
			while (recv(rctl[i].evfd[0], buf, sizeof(buf), MSG_DONTWAIT) > 0)
				;
}

static void remote_txn(const struct binder_transaction_data *t, struct rd *r, struct parcel *reply)
{
	switch (t->code) {
	case 1: { /* getSupportedDeviceTypes -> vector<int> */
		int n = 0;
		for (int i = 0; i < 2; i++)
			n += rctl[i].used;
		p_i32(reply, n);
		for (int i = 0; i < 2; i++)
			if (rctl[i].used)
				p_i32(reply, rctl[i].type);
		logf_("OVRRemoteService: getSupportedDeviceTypes -> %d", n);
		break;
	}
	case 2: { /* getDeviceTypeDescription(int) */
		int ty = r_i32(r);
		p_str16(reply, ty == 0 ? "Left" : ty == 1 ? "Right" : "Unknown");
		break;
	}
	case 3: { /* getPairedDeviceStatus(int) -> RemoteStatus */
		int ty = r_i32(r);
		if (ty >= 0 && ty < 2 && rctl[ty].used)
			p_status(reply, &rctl[ty]);
		else
			p_i32(reply, 0);
		logf_("OVRRemoteService: getPairedDeviceStatus(%d) -> %s", ty, ty >= 0 && ty < 2 && rctl[ty].used ? "status" : "none");
		break;
	}
	case 26: { /* getDeviceEventFd(long id) -> ParcelFileDescriptor */
		uint32_t lo = (uint32_t)r_i32(r), hi = (uint32_t)r_i32(r);
		uint64_t id = ((uint64_t)hi << 32) | lo;
		int k = -1;
		for (int i = 0; i < 2; i++)
			if (rctl[i].used && rctl[i].id == id)
				k = i;
		logf_("OVRRemoteService: getDeviceEventFd(%016llx) -> %s", (unsigned long long)id, k >= 0 ? "socket" : "none");
		if (k >= 0 && rctl[k].evfd[1] >= 0) {
			rctl[k].fd_given = 1;
			p_i32(reply, 1); /* present */
			p_i32(reply, 0); /* no comm channel */
			p_fd(reply, rctl[k].evfd[1]);
		} else {
			p_i32(reply, 0);
		}
		break;
	}
	case 19: { /* registerStatusCallback */
		int ok;
		uint32_t h = first_handle(t, &ok);
		if (ok) {
			bc_handle(BC_INCREFS, h);
			bc_handle(BC_ACQUIRE, h);
			remote_cb = h;
			logf_("OVRRemoteService: status callback registered, handle %u", h);
		}
		break;
	}
	case 20:
		remote_cb = 0;
		break;
	default:
		logf_("OVRRemoteService: call %u answered ok (ignored)", t->code);
	}
}

static void handle_txn(const struct binder_transaction_data *t)
{
	int idx = (int)t->cookie - 1;
	struct parcel reply = {0};
	int oneway = t->flags & TF_ONE_WAY;
	struct rd r = {(const uint8_t *)(uintptr_t)t->data.ptr.buffer, t->data_size, 0};
	struct svc *s = idx >= 0 && idx < NSVC ? &svcs[idx] : NULL;
	const char *name = s ? s->name : "?";

	if (t->code == PING_TRANSACTION) {
		if (verbose)
			logf_("%s: ping from pid %d", name, t->sender_pid);
	} else if (t->code == INTERFACE_TRANSACTION) {
		p_str16(&reply, s ? s->desc : "");
	} else if (t->code == DUMP_TRANSACTION || t->code == SHELL_COMMAND_TRANSACTION ||
		   t->code == SYSPROPS_TRANSACTION) {
		/* nothing */
	} else {
		char desc[128];
		int32_t policy = r_i32(&r);
		int32_t ws = r_i32(&r);
		(void)policy;
		(void)ws;
		if (!r_str16(&r, desc, sizeof(desc)))
			strcpy(desc, "(no token)");
		logf_("%s: code %u%s from pid %d uid %d, %zu bytes, token %s%s", name, t->code,
		      oneway ? " (oneway)" : "", t->sender_pid, t->sender_euid, (size_t)t->data_size,
		      desc, s && strcmp(desc, s->desc) ? "  [UNEXPECTED DESCRIPTOR]" : "");
		if (verbose && r.pos < t->data_size)
			hexdump(r.p + r.pos, t->data_size - r.pos > 128 ? 128 : t->data_size - r.pos);

		p_i32(&reply, 0); /* binder::Status: no exception */
		int one = 0;
		for (size_t k = 0; s && k < sizeof(ret1) / sizeof(ret1[0]); k++)
			if (ret1[k].code == t->code && !strcmp(ret1[k].svc, s->name))
				one = 1;
		if (one) {
			p_i32(&reply, 1);
		} else if (s && s->kind == K_VRPOWER && t->code == 1) {
			int ok;
			uint32_t h = first_handle(t, &ok);
			if (ok)
				add_client(h);
			else
				logf_("vrpowermanager: registerClient without a binder");
		} else if (s && s->kind == K_REMOTE) {
			remote_txn(t, &r, &reply);
		} else if (s && s->kind == K_VRAPI && t->code == 1) {
			set_thread_policy(t->sender_pid, &r);
		} else if (s && s->kind == K_VRPOWER && t->code == 2) {
			int ok;
			uint32_t h = first_handle(t, &ok);
			for (int i = 0; ok && i < MAXCLIENTS; i++)
				if (clients[i].used && clients[i].handle == h)
					drop_client(i, "unregisterClient");
		}
		p_zeros(&reply, 60);
	}

	bc_free_buffer(t->data.ptr.buffer);
	if (!oneway)
		bc_txn(BC_REPLY, 0, 0, 0, &reply);
	for (int i = 0; i < MAXCLIENTS; i++)
		if (clients[i].used && !clients[i].notified)
			notify_client(i);
}

/* Process incoming commands. With want_reply, return after BR_REPLY (0) or a failure (-1). */
static int loop(int want_reply, int32_t *reply_status)
{
	uint8_t rbuf[4096];
	for (;;) {
		struct binder_write_read bwr;
		memset(&bwr, 0, sizeof(bwr));
		bwr.read_size = sizeof(rbuf);
		bwr.read_buffer = (uintptr_t)rbuf;
		if (ioctl(bfd, BINDER_WRITE_READ, &bwr) < 0) {
			if (errno == EINTR) {
				if (tick && !want_reply) {
					tick = 0;
					remote_reload();
					remote_drain();
					if (!then_pending)
						remote_announce();
				}
				if (sig_state && !want_reply) {
					power_state = sig_state;
					sig_state = 0;
					logf_("power state -> %d, notifying clients", power_state);
					for (int i = 0; i < MAXCLIENTS; i++)
						if (clients[i].used)
							notify_client(i);
				}
				continue;
			}
			logf_("BINDER_WRITE_READ (read): %s", strerror(errno));
			return -1;
		}
		uint8_t *p = rbuf, *end = rbuf + bwr.read_consumed;
		while (p + 4 <= end) {
			uint32_t cmd;
			memcpy(&cmd, p, 4);
			p += 4;
			switch (cmd) {
			case BR_NOOP:
			case BR_TRANSACTION_COMPLETE:
			case BR_SPAWN_LOOPER:
			case BR_OK:
				break;
			case BR_INCREFS:
			case BR_ACQUIRE:
			case BR_RELEASE:
			case BR_DECREFS: {
				struct binder_ptr_cookie pc;
				memcpy(&pc, p, sizeof(pc));
				p += sizeof(pc);
				if (cmd == BR_INCREFS)
					bc_ptr_cookie(BC_INCREFS_DONE, pc.ptr, pc.cookie);
				else if (cmd == BR_ACQUIRE)
					bc_ptr_cookie(BC_ACQUIRE_DONE, pc.ptr, pc.cookie);
				break;
			}
			case BR_TRANSACTION: {
				struct binder_transaction_data t;
				memcpy(&t, p, sizeof(t));
				p += sizeof(t);
				handle_txn(&t);
				break;
			}
			case BR_REPLY: {
				struct binder_transaction_data t;
				memcpy(&t, p, sizeof(t));
				p += sizeof(t);
				int32_t st = 0;
				if (t.flags & TF_STATUS_CODE)
					memcpy(&st, (void *)(uintptr_t)t.data.ptr.buffer, 4);
				else if (t.data_size >= 4)
					memcpy(&st, (void *)(uintptr_t)t.data.ptr.buffer, 4);
				bc_free_buffer(t.data.ptr.buffer);
				if (reply_status)
					*reply_status = st;
				if (want_reply)
					return 0;
				break;
			}
			case BR_DEAD_REPLY:
			case BR_FAILED_REPLY:
				logf_("%s", cmd == BR_DEAD_REPLY ? "BR_DEAD_REPLY" : "BR_FAILED_REPLY");
				if (want_reply)
					return -1;
				break;
			case BR_DEAD_BINDER: {
				binder_uintptr_t cookie;
				memcpy(&cookie, p, sizeof(cookie));
				p += sizeof(cookie);
				struct __attribute__((packed)) {
					uint32_t cmd;
					binder_uintptr_t cookie;
				} w = {BC_DEAD_BINDER_DONE, cookie};
				bwrite(&w, sizeof(w));
				drop_client((int)cookie - 1, "client died");
				break;
			}
			case BR_CLEAR_DEATH_NOTIFICATION_DONE:
				p += sizeof(binder_uintptr_t);
				break;
			case BR_ERROR: {
				int32_t e;
				memcpy(&e, p, 4);
				p += 4;
				logf_("BR_ERROR %d", e);
				break;
			}
			default:
				logf_("unhandled binder command 0x%x", cmd);
				p = end;
				break;
			}
		}
	}
}

static void on_signal(int sig)
{
	if (sig == SIGALRM) {
		tick = 1;
		if (then_pending) {
			then_pending = 0;
			sig_state = then_state;
		}
		alarm(1);
		return;
	}
	sig_state = sig == SIGUSR1 ? 1 : sig == SIGHUP ? 2 : 3;
}

static int in_list(const char *list, const char *name)
{
	size_t n = strlen(name);
	for (const char *p = list; p && *p;) {
		const char *c = strchr(p, ',');
		size_t l = c ? (size_t)(c - p) : strlen(p);
		if (l == n && !strncmp(p, name, n))
			return 1;
		p = c ? c + 1 : NULL;
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *skip = NULL, *only = NULL, *extra = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v"))
			verbose = 1;
		else if (!strcmp(argv[i], "-s") && i + 1 < argc)
			power_state = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--model") && i + 1 < argc)
			remote_model = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--then") && i + 1 < argc)
			then_state = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--skip") && i + 1 < argc)
			skip = argv[++i];
		else if (!strcmp(argv[i], "--only") && i + 1 < argc)
			only = argv[++i];
		else if (!strcmp(argv[i], "--extra") && i + 1 < argc)
			extra = argv[++i];
		else if (!strcmp(argv[i], "--no-sched"))
			no_sched = 1;
		else {
			fprintf(stderr, "usage: %s [-v] [-s 1|2|3] [--skip a,b] [--only a,b] [--extra a,b] [--no-sched]\n", argv[0]);
			return 2;
		}
	}
	setvbuf(stderr, NULL, _IOLBF, 0);

	bfd = open("/dev/binder", O_RDWR | O_CLOEXEC);
	if (bfd < 0) {
		perror("/dev/binder");
		return 1;
	}
	struct binder_version ver;
	if (ioctl(bfd, BINDER_VERSION, &ver) < 0 ||
	    ver.protocol_version != BINDER_CURRENT_PROTOCOL_VERSION) {
		fprintf(stderr, "binder protocol mismatch\n");
		return 1;
	}
	if (mmap(NULL, 1 << 20, PROT_READ, MAP_PRIVATE | MAP_NORESERVE, bfd, 0) == MAP_FAILED) {
		perror("binder mmap");
		return 1;
	}
	uint32_t maxthreads = 0;
	ioctl(bfd, BINDER_SET_MAX_THREADS, &maxthreads);

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal; /* no SA_RESTART: interrupt the binder read */
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGUSR2, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGALRM, &sa, NULL);
	alarm(1);

	uint32_t enter = BC_ENTER_LOOPER;
	bwrite(&enter, 4);

	int nreg = 0;
	for (int i = 0; i < NSVC; i++) {
		svcs[i].enabled = ((svcs[i].enabled && !only) || in_list(only, svcs[i].name) ||
				   in_list(extra, svcs[i].name)) && !in_list(skip, svcs[i].name);
		if (!svcs[i].enabled)
			continue;
		struct parcel p = {0};
		p_token(&p, "android.os.IServiceManager");
		p_str16(&p, svcs[i].name);
		p_binder(&p, (uintptr_t)(i + 1));
		p_i32(&p, 1); /* allowIsolated */
		p_i32(&p, 8); /* DUMP_FLAG_PRIORITY_DEFAULT */
		int32_t st = -1;
		if (bc_txn(BC_TRANSACTION, 0, SVC_MGR_ADD_SERVICE, TF_ACCEPT_FDS, &p) < 0 ||
		    loop(1, &st) < 0 || st != 0) {
			logf_("addService(%s) failed (status %d)", svcs[i].name, st);
			continue;
		}
		nreg++;
		if (verbose)
			logf_("registered %s (%s)", svcs[i].name, svcs[i].desc);
	}
	logf_("ovrstub: %d services registered, power state %d", nreg, power_state);
	return loop(0, NULL) < 0 ? 1 : 0;
}
