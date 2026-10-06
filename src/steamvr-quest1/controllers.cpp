// Touch controllers for driver_quest1.
//
// Monado (drivers/quest1) already exposes both Touch controllers as /interaction_profiles/oculus/
// touch_controller: orientation from each controller's IMU, position from an arm model hung off the
// head, buttons, trigger, squeeze, thumbstick and haptics. This file reads them through OpenXR
// actions on the driver's session and publishes them to SteamVR as two oculus_touch controllers
// with the Quest render models, so the game bindings written for Touch apply as is.
//
// Orientation: Monado's controller orientation is the IMU frame. The aim pose is
//   aim = yaw * imu * imuToAim
// imuToAim comes from a two-pose calibration (PollCalibration, saved in
// ~/.config/quest1-controllers.txt) or defaults to rotX(40 deg), Monado's rift_s convention for the
// same Touch controllers. yaw aligns the aim heading with the head on the first pose and whenever
// the system button (menu / Oculus) is held for 1.2 s.
// SteamVR's device ("raw") pose is the Oculus driver's controller origin; the Quest render models
// give the OpenXR aim/grip poses in that frame ("openxr_aim" / "openxr_grip", 60 degrees apart), so
//   raw = aim * rotX(60) * inverse(openxr_grip)
// which goes in the qDriverFromHead/vecDriverFromHead part of the DriverPose_t.
// Position: Monado's when it is tracked (6DoF), otherwise our own arm model hung below the head
// (Monado's 3DoF controllers sit in a different tracking origin than its HMD).
//
// Live knobs (read every ~0.5 s) in /tmp/quest1-ctrl: "<default pitch deg> <velocities 0|1> <debug 0|1>".

#include "controllers.h"

#include <pthread.h>
#include <sys/stat.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

using namespace vr;

static void CLog(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	VRDriverLog()->Log(buf);
}

// --- small rigid-transform helpers ---------------------------------------------------------------------

namespace {

struct Quat
{
	double w = 1, x = 0, y = 0, z = 0;
};
struct Vec
{
	double x = 0, y = 0, z = 0;
};
struct Xform
{
	Quat r;
	Vec t;
};

Quat Mul(const Quat &a, const Quat &b)
{
	return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
	        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}
Quat Conj(const Quat &q) { return {q.w, -q.x, -q.y, -q.z}; }
Vec Rotate(const Quat &q, const Vec &v)
{
	// v + 2w(u x v) + 2u x (u x v)
	Vec u{q.x, q.y, q.z};
	Vec c{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
	Vec cc{u.y * c.z - u.z * c.y, u.z * c.x - u.x * c.z, u.x * c.y - u.y * c.x};
	return {v.x + 2 * (q.w * c.x + cc.x), v.y + 2 * (q.w * c.y + cc.y), v.z + 2 * (q.w * c.z + cc.z)};
}
Quat RotX(double deg)
{
	double h = deg * M_PI / 360.0;
	return {cos(h), sin(h), 0, 0};
}
Xform Compose(const Xform &a, const Xform &b)
{
	Vec t = Rotate(a.r, b.t);
	return {Mul(a.r, b.r), {t.x + a.t.x, t.y + a.t.y, t.z + a.t.z}};
}
Xform Inverse(const Xform &a)
{
	Quat c = Conj(a.r);
	Vec t = Rotate(c, a.t);
	return {c, {-t.x, -t.y, -t.z}};
}

double MonoSeconds()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// --- OpenXR entry points ---------------------------------------------------------------------------------

#define CTRL_XR_FUNCS(X)                                                                                               \
	X(xrStringToPath)                                                                                              \
	X(xrPathToString)                                                                                              \
	X(xrCreateActionSet)                                                                                           \
	X(xrDestroyActionSet)                                                                                          \
	X(xrCreateAction)                                                                                              \
	X(xrSuggestInteractionProfileBindings)                                                                         \
	X(xrAttachSessionActionSets)                                                                                   \
	X(xrCreateActionSpace)                                                                                         \
	X(xrDestroySpace)                                                                                              \
	X(xrSyncActions)                                                                                               \
	X(xrGetActionStateBoolean)                                                                                     \
	X(xrGetActionStateFloat)                                                                                       \
	X(xrGetActionStateVector2f)                                                                                    \
	X(xrGetActionStatePose)                                                                                        \
	X(xrGetCurrentInteractionProfile)                                                                              \
	X(xrLocateSpace)                                                                                               \
	X(xrCreateReferenceSpace)                                                                                      \
	X(xrApplyHapticFeedback)                                                                                       \
	X(xrStopHapticFeedback)

#define CTRL_DECLARE(name) PFN_##name c##name = nullptr;
CTRL_XR_FUNCS(CTRL_DECLARE)

// --- actions ------------------------------------------------------------------------------------------------

enum Act
{
	ACT_SYSTEM,
	ACT_PRIMARY, // A (right) / X (left)
	ACT_PRIMARY_TOUCH,
	ACT_SECONDARY, // B / Y
	ACT_SECONDARY_TOUCH,
	ACT_TRIGGER,
	ACT_TRIGGER_TOUCH,
	ACT_SQUEEZE,
	ACT_STICK,
	ACT_STICK_CLICK,
	ACT_STICK_TOUCH,
	ACT_THUMBREST_TOUCH,
	ACT_GRIP_POSE,
	ACT_HAPTIC,
	ACT_COUNT
};

struct ActDesc
{
	const char *name;
	XrActionType type;
	const char *path[2]; // left, right
};

const ActDesc kActs[ACT_COUNT] = {
    {"system", XR_ACTION_TYPE_BOOLEAN_INPUT, {"/user/hand/left/input/menu/click", "/user/hand/right/input/system/click"}},
    {"primary", XR_ACTION_TYPE_BOOLEAN_INPUT, {"/user/hand/left/input/x/click", "/user/hand/right/input/a/click"}},
    {"primary_touch", XR_ACTION_TYPE_BOOLEAN_INPUT, {"/user/hand/left/input/x/touch", "/user/hand/right/input/a/touch"}},
    {"secondary", XR_ACTION_TYPE_BOOLEAN_INPUT, {"/user/hand/left/input/y/click", "/user/hand/right/input/b/click"}},
    {"secondary_touch", XR_ACTION_TYPE_BOOLEAN_INPUT,
     {"/user/hand/left/input/y/touch", "/user/hand/right/input/b/touch"}},
    {"trigger", XR_ACTION_TYPE_FLOAT_INPUT,
     {"/user/hand/left/input/trigger/value", "/user/hand/right/input/trigger/value"}},
    {"trigger_touch", XR_ACTION_TYPE_BOOLEAN_INPUT,
     {"/user/hand/left/input/trigger/touch", "/user/hand/right/input/trigger/touch"}},
    {"squeeze", XR_ACTION_TYPE_FLOAT_INPUT,
     {"/user/hand/left/input/squeeze/value", "/user/hand/right/input/squeeze/value"}},
    {"stick", XR_ACTION_TYPE_VECTOR2F_INPUT,
     {"/user/hand/left/input/thumbstick", "/user/hand/right/input/thumbstick"}},
    {"stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT,
     {"/user/hand/left/input/thumbstick/click", "/user/hand/right/input/thumbstick/click"}},
    {"stick_touch", XR_ACTION_TYPE_BOOLEAN_INPUT,
     {"/user/hand/left/input/thumbstick/touch", "/user/hand/right/input/thumbstick/touch"}},
    {"thumbrest_touch", XR_ACTION_TYPE_BOOLEAN_INPUT,
     {"/user/hand/left/input/thumbrest/touch", "/user/hand/right/input/thumbrest/touch"}},
    {"grip_pose", XR_ACTION_TYPE_POSE_INPUT, {"/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose"}},
    {"haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, {"/user/hand/left/output/haptic", "/user/hand/right/output/haptic"}},
};

// --- SteamVR input components -------------------------------------------------------------------------

enum Comp
{
	C_SYSTEM_CLICK,
	C_SYSTEM_TOUCH,
	C_PRIMARY_CLICK,
	C_PRIMARY_TOUCH,
	C_SECONDARY_CLICK,
	C_SECONDARY_TOUCH,
	C_TRIGGER_VALUE,
	C_TRIGGER_TOUCH,
	C_GRIP_VALUE,
	C_GRIP_TOUCH,
	C_STICK_X,
	C_STICK_Y,
	C_STICK_CLICK,
	C_STICK_TOUCH,
	C_THUMBREST_TOUCH,
	C_COUNT
};

// --- the SteamVR device ---------------------------------------------------------------------------------

class TouchController : public ITrackedDeviceServerDriver
{
public:
	explicit TouchController(bool left) : left(left) {}
	virtual ~TouchController() = default;

	const char *Serial() const { return left ? "QUEST1-Touch-Left" : "QUEST1-Touch-Right"; }

	EVRInitError Activate(uint32_t objectId) override
	{
		id = objectId;
		container = VRProperties()->TrackedDeviceToPropertyContainer(id);
		CVRPropertyHelpers *p = VRProperties();
		p->SetStringProperty(container, Prop_ModelNumber_String,
		                     left ? "Oculus Quest (Left Controller)" : "Oculus Quest (Right Controller)");
		p->SetStringProperty(container, Prop_SerialNumber_String, Serial());
		p->SetStringProperty(container, Prop_ManufacturerName_String, "Oculus");
		p->SetStringProperty(container, Prop_TrackingSystemName_String, "quest1");
		p->SetStringProperty(container, Prop_RenderModelName_String,
		                     left ? "oculus_quest_controller_left" : "oculus_quest_controller_right");
		p->SetStringProperty(container, Prop_RegisteredDeviceType_String,
		                     left ? "quest1/QUEST1-Touch-Left" : "quest1/QUEST1-Touch-Right");
		p->SetStringProperty(container, Prop_InputProfilePath_String, "{quest1}/input/quest1_touch_profile.json");
		p->SetStringProperty(container, Prop_ControllerType_String, "oculus_touch");
		p->SetInt32Property(container, Prop_ControllerRoleHint_Int32,
		                    left ? TrackedControllerRole_LeftHand : TrackedControllerRole_RightHand);
		p->SetBoolProperty(container, Prop_DeviceProvidesBatteryStatus_Bool, false);
		p->SetBoolProperty(container, Prop_DeviceCanPowerOff_Bool, false);
		p->SetBoolProperty(container, Prop_WillDriftInYaw_Bool, true);
		// legacy (pre-SteamVR Input) API
		p->SetInt32Property(container, Prop_Axis0Type_Int32, k_eControllerAxis_Joystick);
		p->SetInt32Property(container, Prop_Axis1Type_Int32, k_eControllerAxis_Trigger);
		p->SetInt32Property(container, Prop_Axis2Type_Int32, k_eControllerAxis_Trigger);
		p->SetUint64Property(container, Prop_SupportedButtons_Uint64,
		                     ButtonMaskFromId(k_EButton_System) | ButtonMaskFromId(k_EButton_ApplicationMenu) |
		                         ButtonMaskFromId(k_EButton_Grip) | ButtonMaskFromId(k_EButton_A) |
		                         ButtonMaskFromId(k_EButton_SteamVR_Touchpad) |
		                         ButtonMaskFromId(k_EButton_SteamVR_Trigger));

		IVRDriverInput *in = VRDriverInput();
		const char *ab = left ? "x" : "a", *by = left ? "y" : "b";
		char path[64];
		in->CreateBooleanComponent(container, "/input/system/click", &comp[C_SYSTEM_CLICK]);
		in->CreateBooleanComponent(container, "/input/system/touch", &comp[C_SYSTEM_TOUCH]);
		snprintf(path, sizeof(path), "/input/%s/click", ab);
		in->CreateBooleanComponent(container, path, &comp[C_PRIMARY_CLICK]);
		snprintf(path, sizeof(path), "/input/%s/touch", ab);
		in->CreateBooleanComponent(container, path, &comp[C_PRIMARY_TOUCH]);
		snprintf(path, sizeof(path), "/input/%s/click", by);
		in->CreateBooleanComponent(container, path, &comp[C_SECONDARY_CLICK]);
		snprintf(path, sizeof(path), "/input/%s/touch", by);
		in->CreateBooleanComponent(container, path, &comp[C_SECONDARY_TOUCH]);
		in->CreateScalarComponent(container, "/input/trigger/value", &comp[C_TRIGGER_VALUE], VRScalarType_Absolute,
		                          VRScalarUnits_NormalizedOneSided);
		in->CreateBooleanComponent(container, "/input/trigger/touch", &comp[C_TRIGGER_TOUCH]);
		in->CreateScalarComponent(container, "/input/grip/value", &comp[C_GRIP_VALUE], VRScalarType_Absolute,
		                          VRScalarUnits_NormalizedOneSided);
		in->CreateBooleanComponent(container, "/input/grip/touch", &comp[C_GRIP_TOUCH]);
		in->CreateScalarComponent(container, "/input/joystick/x", &comp[C_STICK_X], VRScalarType_Absolute,
		                          VRScalarUnits_NormalizedTwoSided);
		in->CreateScalarComponent(container, "/input/joystick/y", &comp[C_STICK_Y], VRScalarType_Absolute,
		                          VRScalarUnits_NormalizedTwoSided);
		in->CreateBooleanComponent(container, "/input/joystick/click", &comp[C_STICK_CLICK]);
		in->CreateBooleanComponent(container, "/input/joystick/touch", &comp[C_STICK_TOUCH]);
		in->CreateBooleanComponent(container, "/input/thumbrest/touch", &comp[C_THUMBREST_TOUCH]);
		in->CreateHapticComponent(container, "/output/haptic", &haptic);
		for (float &v : last)
			v = NAN; // send everything once
		activated = true;
		CLog("quest1: %s Touch controller activated (device %u)\n", left ? "left" : "right", id);
		return VRInitError_None;
	}

	void Deactivate() override
	{
		activated = false;
		id = k_unTrackedDeviceIndexInvalid;
	}
	void EnterStandby() override {}
	void *GetComponent(const char *) override { return nullptr; }
	void DebugRequest(const char *, char *response, uint32_t size) override
	{
		if (size)
			response[0] = 0;
	}
	DriverPose_t GetPose() override
	{
		std::lock_guard<std::mutex> lock(poseMutex);
		return lastPose;
	}

	//! Update thread: a component value; only changes reach vrserver.
	void Set(Comp c, float v)
	{
		if (!activated || last[c] == v)
			return;
		last[c] = v;
		if (c == C_TRIGGER_VALUE || c == C_GRIP_VALUE || c == C_STICK_X || c == C_STICK_Y)
			VRDriverInput()->UpdateScalarComponent(comp[c], v, 0);
		else
			VRDriverInput()->UpdateBooleanComponent(comp[c], v != 0, 0);
	}

	void Publish(const DriverPose_t &p)
	{
		{
			std::lock_guard<std::mutex> lock(poseMutex);
			lastPose = p;
		}
		if (activated)
			VRServerDriverHost()->TrackedDevicePoseUpdated(id, p, sizeof(p));
	}

	const bool left;
	std::atomic<bool> activated{false};
	uint32_t id = k_unTrackedDeviceIndexInvalid;
	PropertyContainerHandle_t container = k_ulInvalidPropertyContainer;
	VRInputComponentHandle_t haptic = k_ulInvalidInputComponentHandle;

private:
	VRInputComponentHandle_t comp[C_COUNT] = {};
	float last[C_COUNT];
	std::mutex poseMutex;
	DriverPose_t lastPose{};
};

// --- the OpenXR side ------------------------------------------------------------------------------------------

struct Hand
{
	//! aim = imu * imuToAim. Calibrated (see Calibrate) or rotX(pitchDeg), the rift_s convention.
	Quat imuToAim;
	bool calibrated = false;
	//! heading correction applied on top of Monado's orientation so the aim points where the head
	//! looked when it was last recentered (Monado aligns the IMU's -Z, not the aim)
	Quat yaw;
	bool aligned = false;
	double systemDownSince = 0;
	bool recenteredThisHold = false;
	double lastValid = -10;
	bool wasValid = false;
	Vec lastUp{0, 1, 0}; //!< world up in the IMU frame, for the calibration
	bool haveUp = false;
	Vec calForward, calDown; //!< calibration samples: up while pointing forward / down
	bool haveForward = false, haveDown = false;
};

struct State
{
	ControllerXr xr;
	XrActionSet set = XR_NULL_HANDLE;
	XrAction act[ACT_COUNT] = {};
	XrPath hand[2] = {};
	XrSpace grip[2] = {};
	XrSpace view = XR_NULL_HANDLE;
	TouchController *dev[2] = {};
	Hand hands[2];
	std::thread thread;
	std::atomic<bool> running{false};

	// knobs
	std::atomic<float> pitchDeg{40.0f}; //!< default aim = IMU frame pitched up by this much (rift_s: 40)
	std::atomic<bool> velocities{true};
	std::atomic<bool> debug{false};
	time_t calMtime = 0;

	void Loop();
	void UpdateHand(int h, double now, const XrPosef &head, bool headValid);
	void ReadKnobs();
	void PollCalibration();
	void LoadCalibration();
	void SaveCalibration();
	Quat ImuToAim(int h) { return hands[h].calibrated ? hands[h].imuToAim : RotX(pitchDeg); }
};

State *g_state = nullptr;

bool Check(XrResult r, const char *what)
{
	if (XR_FAILED(r)) {
		CLog("quest1: controllers: %s failed (%d)\n", what, (int)r);
		return false;
	}
	return true;
}

//! openxr_grip of the Quest render models, in the controller's raw frame (left; right mirrors x).
Xform GripInRaw(bool left)
{
	return {RotX(20.6), {left ? 0.007 : -0.007, -0.00182941, 0.1019482}};
}

//! The rotation about +Y (heading) of q: its forward (-Z) flattened.
Quat YawOf(const Quat &q)
{
	Vec f = Rotate(q, {0, 0, -1});
	double yaw = atan2(-f.x, -f.z);
	return {cos(yaw / 2), 0, sin(yaw / 2), 0};
}

Vec Normalize(const Vec &v)
{
	double n = sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
	return n > 1e-9 ? Vec{v.x / n, v.y / n, v.z / n} : Vec{0, 1, 0};
}
double Dot(const Vec &a, const Vec &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec Cross(const Vec &a, const Vec &b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

//! Rotation whose matrix has columns x, y, z (orthonormal, right-handed).
Quat FromBasis(const Vec &x, const Vec &y, const Vec &z)
{
	double m00 = x.x, m11 = y.y, m22 = z.z, tr = m00 + m11 + m22;
	Quat q;
	if (tr > 0) {
		double s = sqrt(tr + 1.0) * 2;
		q = {0.25 * s, (y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s};
	} else if (m00 > m11 && m00 > m22) {
		double s = sqrt(1.0 + m00 - m11 - m22) * 2;
		q = {(y.z - z.y) / s, 0.25 * s, (y.x + x.y) / s, (z.x + x.z) / s};
	} else if (m11 > m22) {
		double s = sqrt(1.0 + m11 - m00 - m22) * 2;
		q = {(z.x - x.z) / s, (y.x + x.y) / s, 0.25 * s, (z.y + y.z) / s};
	} else {
		double s = sqrt(1.0 + m22 - m00 - m11) * 2;
		q = {(x.y - y.x) / s, (z.x + x.z) / s, (z.y + y.z) / s, 0.25 * s};
	}
	double n = sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
	return {q.w / n, q.x / n, q.y / n, q.z / n};
}

std::string CalibrationPath()
{
	const char *home = getenv("HOME");
	return std::string(home ? home : "/tmp") + "/.config/quest1-controllers.txt";
}

void State::LoadCalibration()
{
	FILE *f = fopen(CalibrationPath().c_str(), "r");
	if (!f)
		return;
	char side[16];
	double w, x, y, z;
	while (fscanf(f, "%15s %lf %lf %lf %lf", side, &w, &x, &y, &z) == 5) {
		int h = !strcmp(side, "left") ? 0 : !strcmp(side, "right") ? 1 : -1;
		if (h < 0)
			continue;
		hands[h].imuToAim = {w, x, y, z};
		hands[h].calibrated = true;
		CLog("quest1: %s controller calibration loaded: imu->aim %.4f %.4f %.4f %.4f\n", side, w, x, y, z);
	}
	fclose(f);
}

void State::SaveCalibration()
{
	std::string path = CalibrationPath();
	std::string dir = path.substr(0, path.rfind('/'));
	mkdir(dir.c_str(), 0755);
	FILE *f = fopen(path.c_str(), "w");
	if (!f) {
		CLog("quest1: cannot write %s\n", path.c_str());
		return;
	}
	for (int h = 0; h < 2; h++)
		if (hands[h].calibrated)
			fprintf(f, "%s %.6f %.6f %.6f %.6f\n", h ? "right" : "left", hands[h].imuToAim.w, hands[h].imuToAim.x,
			        hands[h].imuToAim.y, hands[h].imuToAim.z);
	fclose(f);
	CLog("quest1: controller calibration saved to %s\n", path.c_str());
}

// Calibration of the IMU mounting, two static poses, both controllers at once:
//   echo forward > /tmp/quest1-ctrl-cal   (pointing straight ahead, level, no roll)
//   echo down > /tmp/quest1-ctrl-cal      (pointing at the floor, buttons facing forward)
//   echo reset > /tmp/quest1-ctrl-cal     (back to the default)
// In the aim frame, world up is +Y in the first pose and +Z in the second; gravity gives both in
// the IMU frame, hence the rotation.
void State::PollCalibration()
{
	struct stat st;
	if (stat("/tmp/quest1-ctrl-cal", &st) != 0 || st.st_mtime == calMtime)
		return;
	bool first = calMtime == 0;
	calMtime = st.st_mtime;
	if (first && time(nullptr) - st.st_mtime > 5)
		return; // a stale request from an earlier run
	char what[32] = "";
	FILE *f = fopen("/tmp/quest1-ctrl-cal", "r");
	if (!f || fscanf(f, "%31s", what) != 1) {
		if (f)
			fclose(f);
		return;
	}
	fclose(f);
	for (int h = 0; h < 2; h++) {
		Hand &hd = hands[h];
		const char *side = h ? "right" : "left";
		if (!strcmp(what, "reset")) {
			hd.calibrated = hd.haveForward = hd.haveDown = false;
			CLog("quest1: %s calibration reset\n", side);
			continue;
		}
		if (!hd.haveUp || MonoSeconds() - hd.lastValid > 0.5) {
			CLog("quest1: calibration '%s': no recent %s controller pose, skipped\n", what, side);
			continue;
		}
		if (!strcmp(what, "forward")) {
			hd.calForward = hd.lastUp;
			hd.haveForward = true;
		} else if (!strcmp(what, "down")) {
			hd.calDown = hd.lastUp;
			hd.haveDown = true;
		} else {
			continue;
		}
		CLog("quest1: calibration '%s' %s: up in IMU frame %+.3f %+.3f %+.3f\n", what, side, hd.lastUp.x, hd.lastUp.y,
		     hd.lastUp.z);
		if (hd.haveForward && hd.haveDown) {
			Vec y = Normalize(hd.calForward);
			double angle = acos(std::max(-1.0, std::min(1.0, Dot(y, Normalize(hd.calDown))))) * 57.2958;
			Vec zr = hd.calDown;
			double d = Dot(zr, y);
			Vec z = Normalize({zr.x - d * y.x, zr.y - d * y.y, zr.z - d * y.z});
			Vec x = Cross(y, z);
			hd.imuToAim = FromBasis(x, y, z);
			hd.calibrated = true;
			hd.aligned = false; // re-align the heading with the new aim
			hd.haveForward = hd.haveDown = false;
			CLog("quest1: %s controller calibrated: imu->aim %.4f %.4f %.4f %.4f (poses %.0f deg apart, "
			     "90 expected)\n",
			     side, hd.imuToAim.w, hd.imuToAim.x, hd.imuToAim.y, hd.imuToAim.z, angle);
		}
	}
	if (hands[0].calibrated || hands[1].calibrated || !strcmp(what, "reset"))
		SaveCalibration();
}

void State::ReadKnobs()
{
	FILE *f = fopen("/tmp/quest1-ctrl", "r");
	if (!f)
		return;
	float pitch = pitchDeg;
	int vel = velocities, dbg = debug;
	int n = fscanf(f, "%f %d %d", &pitch, &vel, &dbg);
	fclose(f);
	if (n < 1)
		return;
	if (pitch != pitchDeg || (vel != 0) != velocities || (dbg != 0) != debug)
		CLog("quest1: controllers: default pitch %.1f deg, velocities %s, debug %s\n", pitch, vel ? "on" : "off",
		     dbg ? "on" : "off");
	pitchDeg = pitch;
	velocities = vel != 0;
	debug = dbg != 0;
}

//! Hold the system button (menu / Oculus) this long to point the controller where the head looks.
constexpr double kRecenterHold = 1.2; // Monado recenters the IMU heading at 1 s, this comes after

void State::UpdateHand(int h, double now, const XrPosef &headPose, bool headValid)
{
	TouchController *d = dev[h];
	Hand &hd = hands[h];
	XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
	gi.subactionPath = hand[h];
	auto boolean = [&](Act a) -> float {
		gi.action = act[a];
		XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};
		return XR_SUCCEEDED(cxrGetActionStateBoolean(xr.session, &gi, &s)) && s.isActive && s.currentState ? 1.f : 0.f;
	};
	auto scalar = [&](Act a) -> float {
		gi.action = act[a];
		XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT};
		return XR_SUCCEEDED(cxrGetActionStateFloat(xr.session, &gi, &s)) && s.isActive ? s.currentState : 0.f;
	};
	float system = boolean(ACT_SYSTEM);
	d->Set(C_SYSTEM_CLICK, system);
	d->Set(C_PRIMARY_CLICK, boolean(ACT_PRIMARY));
	d->Set(C_PRIMARY_TOUCH, boolean(ACT_PRIMARY_TOUCH));
	d->Set(C_SECONDARY_CLICK, boolean(ACT_SECONDARY));
	d->Set(C_SECONDARY_TOUCH, boolean(ACT_SECONDARY_TOUCH));
	d->Set(C_TRIGGER_VALUE, scalar(ACT_TRIGGER));
	d->Set(C_TRIGGER_TOUCH, boolean(ACT_TRIGGER_TOUCH));
	float squeeze = scalar(ACT_SQUEEZE);
	d->Set(C_GRIP_VALUE, squeeze);
	d->Set(C_GRIP_TOUCH, squeeze > 0.05f ? 1.f : 0.f); // no capacitive sensor on the grip
	{
		gi.action = act[ACT_STICK];
		XrActionStateVector2f s{XR_TYPE_ACTION_STATE_VECTOR2F};
		bool ok = XR_SUCCEEDED(cxrGetActionStateVector2f(xr.session, &gi, &s)) && s.isActive;
		d->Set(C_STICK_X, ok ? s.currentState.x : 0.f);
		d->Set(C_STICK_Y, ok ? s.currentState.y : 0.f);
	}
	d->Set(C_STICK_CLICK, boolean(ACT_STICK_CLICK));
	d->Set(C_STICK_TOUCH, boolean(ACT_STICK_TOUCH));
	d->Set(C_THUMBREST_TOUCH, boolean(ACT_THUMBREST_TOUCH));

	// pose
	DriverPose_t p{};
	p.qWorldFromDriverRotation.w = 1;
	p.vecWorldFromDriverTranslation[1] = xr.headHeight;
	p.qRotation.w = 1;
	p.qDriverFromHeadRotation.w = 1;
	XrSpaceVelocity vel{XR_TYPE_SPACE_VELOCITY};
	XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
	loc.next = &vel;
	bool valid = XR_SUCCEEDED(cxrLocateSpace(grip[h], xr.local, xr.now(), &loc)) &&
	             (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
	if (valid) {
		hd.lastValid = now;
		Quat imu{loc.pose.orientation.w, loc.pose.orientation.x, loc.pose.orientation.y, loc.pose.orientation.z};
		hd.lastUp = Rotate(Conj(imu), {0, 1, 0});
		hd.haveUp = true;
		Quat imuToAim = ImuToAim(h);
		Quat head{headPose.orientation.w, headPose.orientation.x, headPose.orientation.y, headPose.orientation.z};

		// heading: on the first pose, then each time the system button is held
		if (system == 0) {
			hd.systemDownSince = 0;
			hd.recenteredThisHold = false;
		} else if (hd.systemDownSince == 0) {
			hd.systemDownSince = now;
		}
		bool hold = hd.systemDownSince > 0 && now - hd.systemDownSince > kRecenterHold && !hd.recenteredThisHold;
		if (headValid && (!hd.aligned || hold)) {
			Quat aimYaw = YawOf(Mul(imu, imuToAim));
			hd.yaw = Mul(YawOf(head), Conj(aimYaw));
			hd.aligned = true;
			hd.recenteredThisHold = hold;
			CLog("quest1: %s controller heading aligned with the head\n", d->left ? "left" : "right");
		}
		Quat ctrl = Mul(hd.yaw, imu); // the IMU frame, heading-corrected
		Quat aim = Mul(ctrl, imuToAim);

		// position: tracked by Monado if it can, else an arm model hung below the head: elbow
		// beside and below it (turning with the head's heading), forearm along the aim
		Vec pos{loc.pose.position.x, loc.pose.position.y, loc.pose.position.z};
		if (!(loc.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT)) {
			Vec hp{headValid ? headPose.position.x : 0, headValid ? headPose.position.y : 0,
			       headValid ? headPose.position.z : 0};
			Vec elbow = Rotate(YawOf(head), {d->left ? -0.17 : 0.17, -0.45, -0.05});
			Vec forearm = Rotate(aim, {0, 0, -0.30});
			pos = {hp.x + elbow.x + forearm.x, hp.y + elbow.y + forearm.y, hp.z + elbow.z + forearm.z};
		}
		p.qRotation = {ctrl.w, ctrl.x, ctrl.y, ctrl.z};
		p.vecPosition[0] = pos.x;
		p.vecPosition[1] = pos.y;
		p.vecPosition[2] = pos.z;
		// the arm-model point is the grip: grip = ctrl * imuToAim * rotX(60) and raw = grip * inverse(openxr_grip)
		Xform dfh = Compose({Mul(imuToAim, RotX(60.0)), {}}, Inverse(GripInRaw(d->left)));
		p.qDriverFromHeadRotation = {dfh.r.w, dfh.r.x, dfh.r.y, dfh.r.z};
		p.vecDriverFromHeadTranslation[0] = dfh.t.x;
		p.vecDriverFromHeadTranslation[1] = dfh.t.y;
		p.vecDriverFromHeadTranslation[2] = dfh.t.z;
		if (velocities && (vel.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT)) {
			Vec w = Rotate(hd.yaw, {vel.angularVelocity.x, vel.angularVelocity.y, vel.angularVelocity.z});
			p.vecAngularVelocity[0] = w.x;
			p.vecAngularVelocity[1] = w.y;
			p.vecAngularVelocity[2] = w.z;
		}
		p.poseIsValid = true;
		p.result = TrackingResult_Running_OK;

		if (debug) {
			static double lastDbg[2];
			if (now - lastDbg[h] > 1.0) {
				lastDbg[h] = now;
				Vec fwd = Rotate(aim, {0, 0, -1});
				CLog("quest1: %s imu-up %+.2f %+.2f %+.2f, aim heading %+.0f pitch %+.0f deg, grip %+.2f %+.2f "
				     "%+.2f, monado %+.2f %+.2f %+.2f\n",
				     d->left ? "L" : "R", hd.lastUp.x, hd.lastUp.y, hd.lastUp.z, atan2(-fwd.x, -fwd.z) * 57.2958,
				     asin(std::max(-1.0, std::min(1.0, fwd.y))) * 57.2958, pos.x, pos.y, pos.z,
				     loc.pose.position.x, loc.pose.position.y, loc.pose.position.z);
			}
		}
	} else {
		p.poseIsValid = false;
		p.result = TrackingResult_Running_OutOfRange;
	}
	p.willDriftInYaw = true;
	// asleep controllers stop reporting: show them as off after a second without a pose
	p.deviceIsConnected = now - hd.lastValid < 1.0;
	if (valid != hd.wasValid)
		CLog("quest1: %s controller %s\n", d->left ? "left" : "right", valid ? "tracking" : "lost");
	hd.wasValid = valid;
	d->Publish(p);
}

void State::Loop()
{
	pthread_setname_np(pthread_self(), "quest1 ctrl");
	bool wasFocused = true;
	XrPath profile[2] = {XR_NULL_PATH, XR_NULL_PATH};
	for (unsigned tick = 0; running; tick++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(4));
		double now = MonoSeconds();
		if (tick % 128 == 0)
			ReadKnobs();
		if (tick % 64 == 0)
			PollCalibration();

		XrActiveActionSet active{set, XR_NULL_PATH};
		XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
		si.countActiveActionSets = 1;
		si.activeActionSets = &active;
		XrResult r = cxrSyncActions(xr.session, &si);
		bool focused = r == XR_SUCCESS;
		if (focused != wasFocused)
			CLog("quest1: controllers: input %s (xrSyncActions %d)\n", focused ? "focused" : "not focused", (int)r);
		wasFocused = focused;

		if (tick % 256 == 0) {
			// log the interaction profile Monado picked for each hand
			for (int h = 0; h < 2; h++) {
				XrInteractionProfileState ps{XR_TYPE_INTERACTION_PROFILE_STATE};
				if (XR_SUCCEEDED(cxrGetCurrentInteractionProfile(xr.session, hand[h], &ps)) &&
				    ps.interactionProfile != profile[h]) {
					profile[h] = ps.interactionProfile;
					char name[XR_MAX_PATH_LENGTH] = "(none)";
					uint32_t n = 0;
					if (profile[h] != XR_NULL_PATH)
						cxrPathToString(xr.instance, profile[h], sizeof(name), &n, name);
					CLog("quest1: %s hand interaction profile: %s\n", h ? "right" : "left", name);
				}
			}
		}
		XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
		bool headValid = XR_SUCCEEDED(cxrLocateSpace(view, xr.local, xr.now(), &head)) &&
		                 (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
		for (int h = 0; h < 2; h++)
			UpdateHand(h, now, head.pose, headValid);
	}
}

} // namespace

bool Quest1Controllers::Init(const ControllerXr &xr)
{
	if (getenv("QUEST1_CONTROLLERS") && atoi(getenv("QUEST1_CONTROLLERS")) == 0) {
		CLog("quest1: controllers disabled (QUEST1_CONTROLLERS=0)\n");
		return false;
	}
	State *s = new State;
	s->xr = xr;
	if (const char *pitch = getenv("QUEST1_CTRL_PITCH"))
		s->pitchDeg = (float)atof(pitch);
#define CTRL_LOAD(name)                                                                                                \
	if (XR_FAILED(xr.getProcAddr(xr.instance, #name, (PFN_xrVoidFunction *)&c##name))) {                          \
		CLog("quest1: controllers: no %s\n", #name);                                                           \
		delete s;                                                                                              \
		return false;                                                                                          \
	}
	CTRL_XR_FUNCS(CTRL_LOAD)

	XrActionSetCreateInfo sci{XR_TYPE_ACTION_SET_CREATE_INFO};
	strcpy(sci.actionSetName, "steamvr_touch");
	strcpy(sci.localizedActionSetName, "SteamVR Touch controllers");
	if (!Check(cxrCreateActionSet(xr.instance, &sci, &s->set), "xrCreateActionSet")) {
		delete s;
		return false;
	}
	cxrStringToPath(xr.instance, "/user/hand/left", &s->hand[0]);
	cxrStringToPath(xr.instance, "/user/hand/right", &s->hand[1]);
	XrActionSuggestedBinding bindings[ACT_COUNT * 2];
	uint32_t nb = 0;
	for (int a = 0; a < ACT_COUNT; a++) {
		XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
		strcpy(ci.actionName, kActs[a].name);
		strcpy(ci.localizedActionName, kActs[a].name);
		ci.actionType = kActs[a].type;
		ci.countSubactionPaths = 2;
		ci.subactionPaths = s->hand;
		if (!Check(cxrCreateAction(s->set, &ci, &s->act[a]), kActs[a].name)) {
			cxrDestroyActionSet(s->set);
			delete s;
			return false;
		}
		for (int h = 0; h < 2; h++) {
			bindings[nb].action = s->act[a];
			cxrStringToPath(xr.instance, kActs[a].path[h], &bindings[nb].binding);
			nb++;
		}
	}
	XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
	cxrStringToPath(xr.instance, "/interaction_profiles/oculus/touch_controller", &sb.interactionProfile);
	sb.suggestedBindings = bindings;
	sb.countSuggestedBindings = nb;
	XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
	ai.countActionSets = 1;
	ai.actionSets = &s->set;
	if (!Check(cxrSuggestInteractionProfileBindings(xr.instance, &sb), "xrSuggestInteractionProfileBindings") ||
	    !Check(cxrAttachSessionActionSets(xr.session, &ai), "xrAttachSessionActionSets")) {
		cxrDestroyActionSet(s->set);
		delete s;
		return false;
	}
	for (int h = 0; h < 2; h++) {
		XrActionSpaceCreateInfo asci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
		asci.action = s->act[ACT_GRIP_POSE];
		asci.subactionPath = s->hand[h];
		asci.poseInActionSpace.orientation.w = 1;
		if (!Check(cxrCreateActionSpace(xr.session, &asci, &s->grip[h]), "xrCreateActionSpace")) {
			cxrDestroyActionSet(s->set);
			delete s;
			return false;
		}
	}

	XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	rsci.poseInReferenceSpace.orientation.w = 1;
	if (!Check(cxrCreateReferenceSpace(xr.session, &rsci, &s->view), "xrCreateReferenceSpace")) {
		cxrDestroyActionSet(s->set);
		delete s;
		return false;
	}
	s->LoadCalibration();

	for (int h = 0; h < 2; h++) {
		s->dev[h] = new TouchController(h == 0);
		if (!VRServerDriverHost()->TrackedDeviceAdded(s->dev[h]->Serial(), TrackedDeviceClass_Controller, s->dev[h]))
			CLog("quest1: TrackedDeviceAdded refused the %s controller\n", h ? "right" : "left");
	}
	s->running = true;
	s->thread = std::thread(&State::Loop, s);
	g_state = s;
	CLog("quest1: Touch controllers: OpenXR actions attached, aim pitch %.1f deg\n", (float)s->pitchDeg);
	return true;
}

void Quest1Controllers::Shutdown()
{
	State *s = g_state;
	if (!s)
		return;
	s->running = false;
	if (s->thread.joinable())
		s->thread.join();
	for (int h = 0; h < 2; h++)
		if (s->grip[h])
			cxrDestroySpace(s->grip[h]);
	if (s->view)
		cxrDestroySpace(s->view);
	if (s->set)
		cxrDestroyActionSet(s->set);
	// the devices belong to vrserver until the driver is unloaded
	for (TouchController *d : s->dev)
		delete d;
	g_state = nullptr;
	delete s;
}

void Quest1Controllers::OnEvent(const VREvent_t &e)
{
	State *s = g_state;
	if (!s || e.eventType != VREvent_Input_HapticVibration)
		return;
	const VREvent_HapticVibration_t &hv = e.data.hapticVibration;
	for (int h = 0; h < 2; h++) {
		if (!s->dev[h] || hv.containerHandle != s->dev[h]->container)
			continue;
		XrHapticActionInfo hai{XR_TYPE_HAPTIC_ACTION_INFO};
		hai.action = s->act[ACT_HAPTIC];
		hai.subactionPath = s->hand[h];
		if (hv.fAmplitude <= 0) {
			cxrStopHapticFeedback(s->xr.session, &hai);
			return;
		}
		XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
		v.amplitude = hv.fAmplitude > 1 ? 1 : hv.fAmplitude;
		v.duration = hv.fDurationSeconds > 0 ? (XrDuration)(hv.fDurationSeconds * 1e9) : XR_MIN_HAPTIC_DURATION;
		v.frequency = hv.fFrequency > 0 ? hv.fFrequency : XR_FREQUENCY_UNSPECIFIED;
		cxrApplyHapticFeedback(s->xr.session, &hai, (XrHapticBaseHeader *)&v);
		if (s->debug)
			CLog("quest1: haptic %s %.3f s amp %.2f freq %.0f\n", h ? "R" : "L", hv.fDurationSeconds, hv.fAmplitude,
			     hv.fFrequency);
		return;
	}
}
