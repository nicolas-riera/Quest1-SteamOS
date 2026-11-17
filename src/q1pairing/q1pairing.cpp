// q1pairing: the backend of Steam's Settings > Controllers > "Add controllers" page (docs/controller-pairing.md).
//
// Steam's UI talks to vrserver's web socket (CSteamVR_ControllerPairing_Request). vrserver does not pair anything
// itself: it forwards every request as a JSON message to the mailbox "frame_controller_pairing", which on a Steam
// Frame is served by Valve's frame_controller driver, and it expects the controller list as a "pairing_state"
// message in its mailbox "web_steam_mailbox". On the Quest 1 nobody serves "frame_controller_pairing", so this
// helper does, through vrclient's IVRMailbox (the same channel as src/camtap/vrmailbox).
//
// Requests handled (JSON {"type": ...}):
//   update_pairing_state | start_pairing {timeout_seconds} | stop_pairing | identify_controller {serial_number}
//   disconnect_controller {serial_number} | disconnect_all_controllers | unpair_controller {serial_number}
//   unpair_all_controllers | unpair_disconnected_controllers
// Answer: {"type":"pairing_state","num_controller_slots":2,"is_pairing":b,
//          "connected_controllers":[{"serial_number","role":"left"|"right"}],"paired_controllers":[{"serial_number"}]}
//
// The pairing itself is behind class Backend, the only code that can change a controller's pairing:
//   DryRunBackend  (the default) does nothing but log what it would do. QUEST1_PAIR_SIM=1 makes it fake a controller
//                  that connects 4 s after pairing starts (in memory only), to see the whole flow in Steam's page.
//   RealBackend    only when the file /etc/quest1/pairing-real exists and QUEST1_PAIR_DRYRUN is not 1: it runs the
//                  program in QUEST1_PAIR_CMD (default /usr/local/libexec/q1-pair-hal) as
//                      q1-pair-hal scan | pair <hex id> | unpair <hex id>
//                  That program is not written yet (it must ask the sensors HAL's IControllerManagementClient, see the
//                  doc); without it the real backend reports "failed" and changes nothing.
// This process never loads libsyncboss and never calls syncboss_*.
//
// Controller list: /run/q1track/controllers (written by fakelogd from the sensors HAL log; one
// "<16 hex id> <1 left | 2 right> <1 connected | 0 not yet>" line per paired controller), read only. Serial numbers are
// the driver's ("QUEST1-Touch-Left/Right", src/steamvr-quest1/controllers.cpp), so Steam's Identify matches.
//
// Build: see build.sh. Run: q1pairing [--once-file <json>] (started by driver_quest1, or by hand).
#include <openvr.h>

#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../steamvr-quest1/q1json.h"

using namespace vr;

// IVRMailbox_001 is not in the public SDK: the layout is the one Proton's vrclient bindings use (as in src/camtap/vrmailbox.cpp).
class IVRMailbox_001
{
public:
	virtual int RegisterMailbox(const char *name, uint64_t *handle) = 0;
	virtual int UndoRegisterMailbox(uint64_t handle) = 0;
	virtual int SendMessage(uint64_t handle, const char *address, const char *message) = 0;
	virtual int ReadMessage(uint64_t handle, char *buffer, uint32_t size, uint32_t *bytes) = 0;
};

static const char *k_requestMailbox = "frame_controller_pairing";
static const char *k_stateMailbox = "web_steam_mailbox";
static const char *k_controllerFile = "/run/q1track/controllers";
static const char *k_realMarker = "/etc/quest1/pairing-real";
static const int k_slots = 2;

static volatile sig_atomic_t g_quit;

static double Now()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void Log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "q1pairing: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

struct Ctl
{
	std::string hexId; //!< Meta's controller id: never printed (Log only says left/right)
	bool left = false;
	bool connected = false;
	std::string Serial() const { return left ? "QUEST1-Touch-Left" : "QUEST1-Touch-Right"; }
};

static std::vector<Ctl> ReadControllers()
{
	std::vector<Ctl> v;
	FILE *f = fopen(k_controllerFile, "r");
	if (!f)
		return v;
	char line[128];
	while (fgets(line, sizeof(line), f)) {
		char hex[32];
		int sub, conn = 1;
		if (sscanf(line, "%16s %d %d", hex, &sub, &conn) < 2 || strlen(hex) != 16 || (sub != 1 && sub != 2))
			continue;
		Ctl c;
		c.hexId = hex;
		c.left = sub == 1;
		c.connected = conn != 0;
		v.push_back(c);
	}
	fclose(f);
	return v;
}

//! The only place where a controller's pairing can change.
class Backend
{
public:
	virtual ~Backend() {}
	virtual const char *Name() const = 0;
	//! Pairing mode starts: begin looking for controllers that are in their pairing mode.
	virtual void StartScan(int timeoutSeconds) = 0;
	virtual void StopScan() = 0;
	//! Called every loop; fills `fake` with simulated controllers (dry run only).
	virtual void Poll(std::vector<Ctl> &fake) { (void)fake; }
	virtual void Unpair(const Ctl &c) = 0;
	virtual bool Simulated() const { return false; }
};

class DryRunBackend : public Backend
{
public:
	const char *Name() const override { return "dry-run"; }
	void StartScan(int t) override
	{
		Log("DRY RUN: would scan for controllers in pairing mode for %d s (no syncboss call)", t);
		scanStart = Now();
		scanning = true;
		simDone = false;
	}
	void StopScan() override
	{
		if (scanning)
			Log("DRY RUN: would stop the scan");
		scanning = false;
	}
	void Poll(std::vector<Ctl> &fake) override
	{
		if (!sim || !scanning || simDone || Now() - scanStart < 4.0)
			return;
		simDone = true;
		Ctl c;
		c.hexId = "0000000000000000";
		c.left = true;
		c.connected = true;
		fake.push_back(c);
		Log("DRY RUN: simulated: the left controller answered the scan and 'paired'");
	}
	void Unpair(const Ctl &c) override { Log("DRY RUN: would unpair the %s controller (no syncboss call)", c.left ? "left" : "right"); }
	bool Simulated() const override { return sim; }
	bool sim = false;

private:
	double scanStart = 0;
	bool scanning = false, simDone = false;
};

class RealBackend : public Backend
{
public:
	const char *Name() const override { return "real"; }
	void StartScan(int t) override
	{
		scanning = true;
		scanStart = Now();
		timeout = t;
		Log("REAL: scanning for %d s through %s", t, Cmd());
	}
	void StopScan() override { scanning = false; }
	void Poll(std::vector<Ctl> &) override
	{
		if (!scanning || Now() - lastScan < 2.0)
			return;
		lastScan = Now();
		if (Now() - scanStart > timeout) {
			scanning = false;
			return;
		}
		// `scan` prints one 16-hex id per advertising controller; each one is paired with `pair <id>`.
		FILE *p = popen(Run("scan").c_str(), "r");
		if (!p) {
			Log("REAL: cannot run %s", Cmd());
			return;
		}
		char line[64];
		std::vector<std::string> ids;
		while (fgets(line, sizeof(line), p)) {
			std::string s(line);
			while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
				s.pop_back();
			if (s.size() == 16)
				ids.push_back(s);
		}
		pclose(p);
		for (auto &id : ids) {
			int rc = system(Run("pair " + id).c_str());
			Log("REAL: pair -> %d", rc);
		}
	}
	void Unpair(const Ctl &c) override
	{
		int rc = system(Run("unpair " + c.hexId).c_str());
		Log("REAL: unpair the %s controller -> %d", c.left ? "left" : "right", rc);
	}

private:
	static const char *Cmd()
	{
		const char *e = getenv("QUEST1_PAIR_CMD");
		return e && *e ? e : "/usr/local/libexec/q1-pair-hal";
	}
	static std::string Run(const std::string &args) { return std::string(Cmd()) + " " + args + " 2>/dev/null"; }
	bool scanning = false;
	double scanStart = 0, lastScan = 0;
	int timeout = 30;
};

static std::string Esc(const std::string &s)
{
	std::string o;
	for (char c : s) {
		if (c == '"' || c == '\\')
			o += '\\';
		if ((unsigned char)c >= 0x20)
			o += c;
	}
	return o;
}

static bool VrServerUp()
{
	// exact process name: not a pattern that could match this command line
	return system("pgrep -x vrserver >/dev/null 2>&1") == 0;
}

struct Pairing
{
	IVRMailbox_001 *mb = nullptr;
	uint64_t handle = 0;
	Backend *be = nullptr;
	bool pairing = false;
	double pairingUntil = 0;
	std::vector<Ctl> fake; //!< dry-run simulated controllers
	std::string lastState;
	double lastSend = 0, lastAlive = 0;

	std::vector<Ctl> All()
	{
		auto v = ReadControllers();
		for (auto &c : fake)
			v.push_back(c);
		return v;
	}

	std::string StateJson()
	{
		auto v = All();
		std::string s = "{\"type\":\"pairing_state\",\"num_controller_slots\":" + std::to_string(k_slots) +
				",\"is_pairing\":" + (pairing ? "true" : "false") + ",\"connected_controllers\":[";
		bool first = true;
		for (auto &c : v) {
			if (!c.connected)
				continue;
			s += std::string(first ? "" : ",") + "{\"serial_number\":\"" + Esc(c.Serial()) + "\",\"role\":\"" +
			     (c.left ? "left" : "right") + "\"}";
			first = false;
		}
		s += "],\"paired_controllers\":[";
		first = true;
		for (auto &c : v) {
			s += std::string(first ? "" : ",") + "{\"serial_number\":\"" + Esc(c.Serial()) + "\"}";
			first = false;
		}
		return s + "]}";
	}

	void SendState(bool force)
	{
		std::string s = StateJson();
		if (!force && s == lastState && Now() - lastSend < 5.0)
			return;
		lastState = s;
		lastSend = Now();
		int r = mb->SendMessage(handle, k_stateMailbox, s.c_str());
		Log("state -> %s (send %d)", s.c_str(), r);
	}

	void Handle(const std::string &msg)
	{
		q1json::Json j;
		q1json::JsonParser jp{msg.data(), msg.data() + msg.size()};
		if (!jp.Value(j) || j.type != q1json::Json::Obj) {
			Log("unreadable message: %.120s", msg.c_str());
			return;
		}
		const q1json::Json *t = j.Get("type");
		const std::string type = t && t->type == q1json::Json::Str ? t->str : "";
		const q1json::Json *sn = j.Get("serial_number");
		const std::string serial = sn && sn->type == q1json::Json::Str ? sn->str : "";
		Log("request: %s%s%s", type.c_str(), serial.empty() ? "" : " ", serial.c_str());
		if (type == "update_pairing_state") {
			SendState(true);
		} else if (type == "start_pairing") {
			const q1json::Json *to = j.Get("timeout_seconds");
			int sec = to && to->type == q1json::Json::Num ? (int)to->num : 30;
			if (sec < 5)
				sec = 5;
			if (sec > 120)
				sec = 120;
			pairing = true;
			pairingUntil = Now() + sec;
			be->StartScan(sec);
			SendState(true);
		} else if (type == "stop_pairing") {
			StopPairing();
		} else if (type == "unpair_controller" || type == "unpair_all_controllers" ||
			   type == "unpair_disconnected_controllers") {
			for (auto &c : All()) {
				bool match = type == "unpair_all_controllers" || (type == "unpair_controller" && c.Serial() == serial) ||
					     (type == "unpair_disconnected_controllers" && !c.connected);
				if (match)
					be->Unpair(c);
			}
			SendState(true);
		} else if (type == "identify_controller") {
			// a buzz through the driver (controllers.cpp, PollRequests): it reads this file and unlinks it
			const char *side = serial == "QUEST1-Touch-Left" ? "left" : serial == "QUEST1-Touch-Right" ? "right" : nullptr;
			FILE *f = side ? fopen("/run/q1pairing/identify", "w") : nullptr;
			if (f) {
				fputs(side, f);
				fclose(f);
			} else {
				Log("identify: cannot ask the driver (%s)", side ? "/run/q1pairing missing" : "unknown serial");
			}
		} else if (type == "disconnect_controller" || type == "disconnect_all_controllers") {
			// making a controller sleep needs the sensors HAL (syncboss_input_sleep): not wired up yet
			Log("%s: not implemented yet (needs the sensors HAL)", type.c_str());
			SendState(true);
		} else {
			Log("ignored request type '%s'", type.c_str());
		}
	}

	void StopPairing()
	{
		if (!pairing)
			return;
		pairing = false;
		be->StopScan();
		SendState(true);
	}

	void Loop()
	{
		static char buf[65536];
		while (!g_quit) {
			uint32_t n = 0;
			while (mb->ReadMessage(handle, buf, sizeof(buf) - 1, &n) == 0 && n) {
				buf[n < sizeof(buf) ? n : sizeof(buf) - 1] = 0;
				Handle(buf);
				n = 0;
			}
			be->Poll(fake);
			if (pairing && Now() > pairingUntil) {
				Log("pairing mode timed out");
				StopPairing();
			}
			SendState(false);
			if (Now() - lastAlive > 3.0) {
				lastAlive = Now();
				if (!VrServerUp()) {
					Log("vrserver is gone");
					return;
				}
			}
			usleep(50000);
		}
	}
};

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	signal(SIGTERM, [](int) { g_quit = 1; });
	signal(SIGINT, [](int) { g_quit = 1; });
	struct stat st;
	const char *dry = getenv("QUEST1_PAIR_DRYRUN");
	const bool real = stat(k_realMarker, &st) == 0 && !(dry && !strcmp(dry, "1"));
	DryRunBackend dryBackend;
	RealBackend realBackend;
	const char *sim = getenv("QUEST1_PAIR_SIM");
	dryBackend.sim = sim && !strcmp(sim, "1");
	Backend *be = real ? (Backend *)&realBackend : (Backend *)&dryBackend;
	Log("backend: %s%s", be->Name(), be->Simulated() ? " (simulated controller)" : "");

	while (!g_quit) {
		if (!VrServerUp()) {
			sleep(3);
			continue;
		}
		EVRInitError e = VRInitError_None;
		VR_Init(&e, VRApplication_Utility);
		if (e != VRInitError_None) {
			Log("VR_Init: %d, retrying", (int)e);
			sleep(5);
			continue;
		}
		auto *mb = (IVRMailbox_001 *)VR_GetGenericInterface("IVRMailbox_001", &e);
		if (!mb) {
			Log("no IVRMailbox_001: %d", (int)e);
			VR_Shutdown();
			sleep(5);
			continue;
		}
		Pairing p;
		p.mb = mb;
		p.be = be;
		int r = mb->RegisterMailbox(k_requestMailbox, &p.handle);
		Log("registered mailbox %s: %d", k_requestMailbox, r);
		p.SendState(true);
		p.Loop();
		mb->UndoRegisterMailbox(p.handle);
		VR_Shutdown();
	}
	return 0;
}
