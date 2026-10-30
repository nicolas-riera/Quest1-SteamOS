// Battery status for driver_quest1 (see battery.h).
//
// Headset: the kernel's power supply class (/sys/class/power_supply/battery/{capacity,status}, the
// same battery UPower reports). Steam's VR UI does not show the system (UPower) battery while VR runs;
// it shows /user/head's DeviceBatteryPercentage_Float instead, like on the Steam Frame.
// Touch controllers: Monado owns SyncBoss; its quest1 driver reports each controller's battery level
// (input records / enumeration) through xrt_device::get_battery_status, which libmonado exposes
// (mnd_root_get_device_battery_status). libmonado is loaded with dlopen; without it, or with a Monado
// whose controllers don't report a battery, the controllers just have no battery status.
//
// Polled every 10 s on a thread; properties are only written when they change.

#include "battery.h"

#include <dlfcn.h>
#include <pthread.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

using namespace vr;

namespace {

void BLog(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	VRDriverLog()->Log(buf);
}

struct Status
{
	bool present = false;
	bool charging = false;
	float charge = 0; // 0..1

	bool operator==(const Status &o) const
	{
		return present == o.present && (!present || (charging == o.charging && fabsf(charge - o.charge) < 0.005f));
	}
	bool operator!=(const Status &o) const { return !(*this == o); }
};

bool ReadLine(const char *path, char *buf, size_t size)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	bool ok = fgets(buf, (int)size, f) != nullptr;
	fclose(f);
	if (ok)
		buf[strcspn(buf, "\r\n")] = 0;
	return ok;
}

Status ReadHeadset()
{
	Status s;
	char cap[32], status[32];
	if (!ReadLine("/sys/class/power_supply/battery/capacity", cap, sizeof(cap)) ||
	    !ReadLine("/sys/class/power_supply/battery/status", status, sizeof(status)))
		return s;
	s.present = true;
	s.charge = fminf(fmaxf(atoi(cap) / 100.0f, 0.0f), 1.0f);
	// "Full" or "Not charging" while plugged in still counts as on external power
	bool plugged = false;
	for (const char *supply : {"usb", "dc", "pc_port", "main"}) {
		char path[96], online[8];
		snprintf(path, sizeof(path), "/sys/class/power_supply/%s/online", supply);
		if (ReadLine(path, online, sizeof(online)) && atoi(online) > 0)
			plugged = true;
	}
	s.charging = !strcmp(status, "Charging") || (plugged && strcmp(status, "Discharging") != 0);
	return s;
}

// --- libmonado (monado.h, API 1.x) ---

typedef struct mnd_root mnd_root_t;
typedef int mnd_result_t; // MND_SUCCESS = 0

struct Monado
{
	void *lib = nullptr;
	bool failed = false;
	void (*getVersion)(uint32_t *, uint32_t *, uint32_t *) = nullptr;
	mnd_result_t (*create)(mnd_root_t **) = nullptr;
	void (*destroy)(mnd_root_t **) = nullptr;
	mnd_result_t (*fromRole)(mnd_root_t *, const char *, int32_t *) = nullptr;
	mnd_result_t (*battery)(mnd_root_t *, uint32_t, bool *, bool *, float *) = nullptr;
	mnd_root_t *root = nullptr;
	double nextConnect = 0;
	int failures = 0;

	bool Load()
	{
		if (lib || failed)
			return lib != nullptr;
		for (const char *name : {"libmonado.so", "/opt/monado/lib/libmonado.so"}) {
			lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
			if (lib)
				break;
		}
		if (lib) {
			getVersion = (decltype(getVersion))dlsym(lib, "mnd_api_get_version");
			create = (decltype(create))dlsym(lib, "mnd_root_create");
			destroy = (decltype(destroy))dlsym(lib, "mnd_root_destroy");
			fromRole = (decltype(fromRole))dlsym(lib, "mnd_root_get_device_from_role");
			battery = (decltype(battery))dlsym(lib, "mnd_root_get_device_battery_status");
		}
		uint32_t major = 0, minor = 0, patch = 0;
		if (lib && getVersion)
			getVersion(&major, &minor, &patch);
		if (!lib || !create || !destroy || !fromRole || !battery || major != 1) {
			BLog("quest1: battery: no usable libmonado (%s), controllers without battery status\n",
			     lib ? "missing symbols or API version" : dlerror());
			if (lib)
				dlclose(lib);
			lib = nullptr;
			failed = true;
			return false;
		}
		return true;
	}

	void Disconnect()
	{
		if (root)
			destroy(&root);
		root = nullptr;
	}

	//! Status of the controller in role "left"/"right"; false if Monado can't be asked now.
	bool Query(const char *role, Status &out, double now)
	{
		out = Status();
		if (!Load())
			return false;
		if (!root) {
			if (now < nextConnect)
				return false;
			if (create(&root) != 0) {
				root = nullptr;
				nextConnect = now + 30; // Monado not running / restarting
				return false;
			}
		}
		int32_t index = -1;
		if (fromRole(root, role, &index) != 0 || index < 0)
			return true; // no such controller
		bool present = false, charging = false;
		float charge = 0;
		mnd_result_t r = battery(root, (uint32_t)index, &present, &charging, &charge);
		if (r == 0) {
			failures = 0;
			out.present = present;
			out.charging = charging;
			out.charge = fminf(fmaxf(charge, 0.0f), 1.0f);
			return true;
		}
		// MND_ERROR_OPERATION_FAILED: an IPC failure (Monado restarted) or a Monado whose device has
		// no battery status; both look the same, so reconnect, less and less often (each libmonado
		// connection is a Monado client)
		Disconnect();
		nextConnect = now + std::min(60.0 * (1 << std::min(failures, 4)), 600.0);
		failures++;
		return false;
	}
};

class Poller
{
public:
	void Attach(Quest1BatteryDevice which, PropertyContainerHandle_t c)
	{
		std::lock_guard<std::mutex> lock(mutex);
		container[which] = c;
		sent[which] = Status();
		known[which] = false;
		if (c != k_ulInvalidPropertyContainer && !thread.joinable()) {
			running = true;
			thread = std::thread([this] { Run(); });
		}
		cv.notify_all(); // publish at once
	}

	void Shutdown()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			running = false;
			cv.notify_all();
		}
		if (thread.joinable())
			thread.join();
		monado.Disconnect();
	}

private:
	static double Now()
	{
		return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	void Run()
	{
		pthread_setname_np(pthread_self(), "quest1 battery");
		std::unique_lock<std::mutex> lock(mutex);
		while (running) {
			lock.unlock();
			Status s[Quest1Battery_Count];
			bool valid[Quest1Battery_Count] = {true, false, false};
			s[Quest1Battery_Head] = ReadHeadset();
			double now = Now();
			valid[Quest1Battery_Left] = monado.Query("left", s[Quest1Battery_Left], now);
			valid[Quest1Battery_Right] = monado.Query("right", s[Quest1Battery_Right], now);
			lock.lock();
			for (int i = 0; i < Quest1Battery_Count; i++) {
				if (container[i] == k_ulInvalidPropertyContainer || !valid[i] || (known[i] && s[i] == sent[i]))
					continue;
				Publish((Quest1BatteryDevice)i, s[i]);
			}
			cv.wait_for(lock, std::chrono::seconds(10));
		}
	}

	//! Called with the mutex held.
	void Publish(Quest1BatteryDevice which, const Status &s)
	{
		static const char *names[] = {"headset", "left controller", "right controller"};
		PropertyContainerHandle_t c = container[which];
		CVRPropertyHelpers *p = VRProperties();
		if (s.present) {
			p->SetFloatProperty(c, Prop_DeviceBatteryPercentage_Float, s.charge);
			p->SetBoolProperty(c, Prop_DeviceIsCharging_Bool, s.charging);
		}
		p->SetBoolProperty(c, Prop_DeviceProvidesBatteryStatus_Bool, s.present);
		if (!known[which] || s.present != sent[which].present || s.charging != sent[which].charging ||
		    (int)lroundf(s.charge * 100) != (int)lroundf(sent[which].charge * 100)) {
			if (s.present)
				BLog("quest1: battery: %s %d%%%s\n", names[which], (int)lroundf(s.charge * 100),
				     s.charging ? ", charging" : "");
			else
				BLog("quest1: battery: %s: no battery status\n", names[which]);
		}
		sent[which] = s;
		known[which] = true;
	}

	std::mutex mutex;
	std::condition_variable cv;
	std::thread thread;
	bool running = false;
	PropertyContainerHandle_t container[Quest1Battery_Count] = {k_ulInvalidPropertyContainer,
	                                                            k_ulInvalidPropertyContainer,
	                                                            k_ulInvalidPropertyContainer};
	Status sent[Quest1Battery_Count];
	bool known[Quest1Battery_Count] = {};
	Monado monado; // used by the poll thread only (and Shutdown after it stopped)
};

Poller &ThePoller()
{
	static Poller poller;
	return poller;
}

} // namespace

void Quest1BatteryAttach(Quest1BatteryDevice which, PropertyContainerHandle_t container)
{
	ThePoller().Attach(which, container);
}

void Quest1BatteryShutdown()
{
	ThePoller().Shutdown();
}
