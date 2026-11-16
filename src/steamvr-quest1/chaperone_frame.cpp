// See chaperone_frame.h.
#include "chaperone_frame.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mutex>

namespace
{
std::mutex mtx;
float eyeHeight = 1.65f;
bool anchored = false;   //!< the frame below is valid for the current wearing
bool wasWorn = false;
double lastWornS = 0;    //!< monotonic seconds of the last time the headset was worn
double offSinceS = 0;    //!< when it was taken off (0 = worn)
double yaw = 0;          //!< raw yaw correction: raw = Ry(yaw) * local + t
double t[3] = {0, 0, 0};
bool sixDof = false;
bool useStatic = false;  //!< /run/q1track/frame-state says "static" or "unknown": use Meta's world frame as is
double stateCheckS = 0;
bool roomShown = true;   //!< the chaperone is visible (collisionBounds alpha as the user has it)
bool restoredAlpha = false;
double badSinceS = 0;    //!< since when map-state has said "bad" (0 = not)

constexpr double kReanchorOffS = 20.0;
constexpr double kBadHideS = 4.0;     //!< "bad" map tracking hides the room only after this long (it comes in short bursts)
constexpr double kStateStaleS = 15.0; //!< q1anchord rewrites map-state every 3 s: older = q1anchord is not running

// SteamVR draws the chaperone (the boundary walls) from collisionBounds settings. To hide it while Meta's frame can't
// be trusted its alpha is set to 0 and the user's value is kept in a setting of ours, so that a vrserver that dies
// while the room is hidden does not leave the walls invisible for good: the next start restores it.
const char *kBounds = "collisionBounds", *kAlpha = "CollisionBoundsColorGammaA";
const char *kOurs = "driver_quest1", *kSaved = "chaperoneAlphaSaved";

void Log(const char *msg)
{
	if (vr::VRDriverLog())
		vr::VRDriverLog()->Log(msg);
}

void RestoreAlphaOnce()
{
	if (restoredAlpha || !vr::VRSettings())
		return;
	restoredAlpha = true;
	vr::EVRSettingsError err = vr::VRSettingsError_None;
	int saved = vr::VRSettings()->GetInt32(kOurs, kSaved, &err);
	if (err == vr::VRSettingsError_None && saved >= 0) {
		vr::VRSettings()->SetInt32(kBounds, kAlpha, saved, &err);
		vr::VRSettings()->SetInt32(kOurs, kSaved, -1, &err);
		Log("quest1: chaperone alpha restored after an interrupted hide\n");
	}
}

void ShowRoom(bool show)
{
	if (show == roomShown || !vr::VRSettings())
		return;
	vr::EVRSettingsError err = vr::VRSettingsError_None;
	if (!show) {
		int cur = vr::VRSettings()->GetInt32(kBounds, kAlpha, &err);
		if (err != vr::VRSettingsError_None)
			cur = 153; // SteamVR's default
		vr::VRSettings()->SetInt32(kOurs, kSaved, cur, &err);
		vr::VRSettings()->SetInt32(kBounds, kAlpha, 0, &err);
	} else {
		int saved = vr::VRSettings()->GetInt32(kOurs, kSaved, &err);
		if (err == vr::VRSettingsError_None && saved >= 0)
			vr::VRSettings()->SetInt32(kBounds, kAlpha, saved, &err);
		vr::VRSettings()->SetInt32(kOurs, kSaved, -1, &err);
	}
	roomShown = show;
	Log(show ? "quest1: chaperone shown (map tracking good)\n" : "quest1: chaperone hidden (map not tracking)\n");
}

//! "tracking" (or no q1anchord at all, or a stale file: nothing known, keep the old behaviour) shows the room.
bool MapTrusted(double now)
{
	char buf[16] = {};
	struct stat st;
	FILE *f = fopen("/run/q1track/map-state", "r");
	if (f) {
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		(void)n;
		fclose(f);
	}
	if (!buf[0] || stat("/run/q1track/map-state", &st) != 0 || time(nullptr) - st.st_mtime > kStateStaleS)
		return true;
	if (strncmp(buf, "tracking", 8) == 0) {
		badSinceS = 0;
		return true;
	}
	if (strncmp(buf, "bad", 3) == 0) {
		if (badSinceS == 0)
			badSinceS = now;
		return now - badSinceS < kBadHideS;
	}
	badSinceS = 0;
	return false; // unknown, relocalizing, lost
}

double Now()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

//! q1anchord (device/q1anchord.py) writes frame-state "static" once the saved map was the current one in this engine
//! session (or no map is saved yet), "unknown" until then, "anchor" when a map is saved but a worn wait of two
//! minutes found no relocalization. Meta's frame is used as is in static and unknown (no head anchoring: it would
//! put the room on the head at a moment the frame is not the saved one); map-state decides if the room is shown.
void CheckState(double now)
{
	if (now - stateCheckS < 1.0)
		return;
	stateCheckS = now;
	RestoreAlphaOnce();
	ShowRoom(MapTrusted(now));
	char buf[16] = {};
	FILE *f = fopen("/run/q1track/frame-state", "r");
	if (f) {
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		(void)n;
		fclose(f);
	}
	bool st = strncmp(buf, "static", 6) == 0 || strncmp(buf, "unknown", 7) == 0;
	if (st != useStatic) {
		useStatic = st;
		anchored = false; // leaving the static frame: anchor again at the next valid worn pose
	}
}

//! Panels off = headset not worn (Monado creates the file before blanking, removes it at unblank).
bool Worn() { return access("/tmp/quest1-panels-off", F_OK) != 0; }

void Rot(const XrQuaternionf &q, double x, double y, double z, double out[3])
{
	// v + 2w(u x v) + 2 u x (u x v)
	double ux = q.x, uy = q.y, uz = q.z, w = q.w;
	double cx = uy * z - uz * y, cy = uz * x - ux * z, cz = ux * y - uy * x;
	out[0] = x + 2 * (w * cx + uy * cz - uz * cy);
	out[1] = y + 2 * (w * cy + uz * cx - ux * cz);
	out[2] = z + 2 * (w * cz + ux * cy - uy * cx);
}
} // namespace

void Quest1FrameInit(float h)
{
	std::lock_guard<std::mutex> l(mtx);
	eyeHeight = h;
}

void Quest1FrameUpdate(const XrPosef &head, bool positionTracked)
{
	std::lock_guard<std::mutex> l(mtx);
	sixDof = positionTracked;
	double now = Now();
	CheckState(now);
	bool worn = Worn();
	if (worn) {
		if (!wasWorn && offSinceS != 0 && now - offSinceS > kReanchorOffS)
			anchored = false;
		offSinceS = 0;
		lastWornS = now;
	} else if (wasWorn || offSinceS == 0) {
		offSinceS = now;
	}
	wasWorn = worn;
	if (anchored || useStatic || !positionTracked || !worn || access("/tmp/quest1-no-anchor", F_OK) == 0)
		return;

	// forward direction on the floor plane, also when looking up or down (then the up vector
	// points forward / backward)
	double f[3], u[3];
	Rot(head.orientation, 0, 0, -1, f);
	Rot(head.orientation, 0, 1, 0, u);
	double s = f[1] < 0 ? 1 : -1;
	double hx = f[0] + s * u[0], hz = f[2] + s * u[2];
	if (hx * hx + hz * hz < 1e-4)
		return;
	// Ry(a) maps (0,0,-1) to (-sin a, 0, -cos a): a0 = atan2(-hx, -hz); raw yaw = -a0
	yaw = -atan2(-hx, -hz);
	double c = cos(yaw), sn = sin(yaw);
	const double px = head.position.x, pz = head.position.z;
	// Ry(yaw) * (x, y, z) = (c x + sn z, y, -sn x + c z)
	t[0] = -(c * px + sn * pz);
	t[1] = eyeHeight - head.position.y;
	t[2] = -(-sn * px + c * pz);
	anchored = true;
}

void Quest1FrameApply(vr::DriverPose_t &pose)
{
	std::lock_guard<std::mutex> l(mtx);
	bool anch = sixDof && anchored && !useStatic;
	double a = anch ? yaw : 0;
	pose.qWorldFromDriverRotation.w = cos(a / 2);
	pose.qWorldFromDriverRotation.x = 0;
	pose.qWorldFromDriverRotation.y = sin(a / 2);
	pose.qWorldFromDriverRotation.z = 0;
	if (anch) {
		for (int i = 0; i < 3; i++)
			pose.vecWorldFromDriverTranslation[i] = t[i];
	} else {
		pose.vecWorldFromDriverTranslation[0] = pose.vecWorldFromDriverTranslation[2] = 0;
		pose.vecWorldFromDriverTranslation[1] = eyeHeight;
	}
}
