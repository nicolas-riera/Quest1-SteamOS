// q1chap: chaperone (boundary) setup utility for the Quest 1 SteamVR driver.
//
// Meta's tracking origin has no floor and the Frame build of SteamVR ships no room setup tool for
// aarch64, so this OpenVR utility does the job with IVRChaperoneSetup:
//
//   q1chap status                 calibration state, play area, bounds, HMD / controller poses, buttons
//   q1chap setup                  guided: controller on the floor + trigger (floor height), then the
//                                 trigger on every corner of the room, grip closes the polygon
//   q1chap default [w [d [floor]]]  rectangle of w x d metres (default 2.5 x 2.5) centred on the
//                                 tracking origin; floor = raw y of the floor (default 0)
//   q1chap clear                  remove the chaperone bounds
//   q1chap register               register as the SteamVR "Room Setup" tool in the dashboard
//
// Prompts are shown as HMD notifications (no fonts needed). Runs as the steamos user while SteamVR is up.
// Build: see build.sh (aarch64-linux-gnu-g++, links the device's libopenvr_api.so).
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include <openvr.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace vr;

static const float kWallHeight = 2.43f;

struct Vec2 {
	float x, z;
};

static IVRSystem *sys;
static IVRChaperone *chap;
static IVRChaperoneSetup *setup;
static VROverlayHandle_t overlay = k_ulOverlayHandleInvalid;

static void Sleep(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

static HmdMatrix34_t Translation(float x, float y, float z)
{
	HmdMatrix34_t m = {};
	m.m[0][0] = m.m[1][1] = m.m[2][2] = 1;
	m.m[0][3] = x;
	m.m[1][3] = y;
	m.m[2][3] = z;
	return m;
}

// Prompt text: rendered with stb_truetype (https://github.com/nothings/stb, public domain, not vendored:
// put stb_truetype.h in /root/stb) from the system Adwaita font, shown on an HMD-locked overlay.
static std::vector<unsigned char> fontData;
static stbtt_fontinfo font;

static void DrawText(std::vector<unsigned char> &rgba, int W, int H, const std::string &text)
{
	// background card
	for (int i = 0; i < W * H; i++) {
		rgba[i * 4 + 0] = 20;
		rgba[i * 4 + 1] = 24;
		rgba[i * 4 + 2] = 36;
		rgba[i * 4 + 3] = 235;
	}
	if (fontData.empty())
		return;
	// greedy word wrap at pixel size 34
	const float px = 34.0f;
	const float sc = stbtt_ScaleForPixelHeight(&font, px);
	int asc, desc, gap;
	stbtt_GetFontVMetrics(&font, &asc, &desc, &gap);
	const int lineH = (int)((asc - desc + gap) * sc) + 4;
	auto advance = [&](const std::string &w) {
		float x = 0;
		for (char c : w) {
			int adv, lsb;
			stbtt_GetCodepointHMetrics(&font, (unsigned char)c, &adv, &lsb);
			x += adv * sc;
		}
		return x;
	};
	std::vector<std::string> lines;
	std::string cur, word;
	auto flush = [&]() {
		if (word.empty())
			return;
		std::string t = cur.empty() ? word : cur + " " + word;
		if (advance(t) > W - 40 && !cur.empty()) {
			lines.push_back(cur);
			cur = word;
		} else
			cur = t;
		word.clear();
	};
	for (char c : text) {
		if (c == ' ')
			flush();
		else
			word += c;
	}
	flush();
	if (!cur.empty())
		lines.push_back(cur);
	int y = 24 + (int)(asc * sc);
	for (auto &l : lines) {
		float x = 20;
		for (char c : l) {
			int adv, lsb, x0, y0, x1, y1;
			stbtt_GetCodepointHMetrics(&font, (unsigned char)c, &adv, &lsb);
			stbtt_GetCodepointBitmapBox(&font, (unsigned char)c, sc, sc, &x0, &y0, &x1, &y1);
			const int gw = x1 - x0, gh = y1 - y0;
			std::vector<unsigned char> bm((size_t)std::max(1, gw * gh));
			stbtt_MakeCodepointBitmap(&font, bm.data(), gw, gh, gw, sc, sc, (unsigned char)c);
			for (int gy = 0; gy < gh; gy++)
				for (int gx = 0; gx < gw; gx++) {
					const int ox = (int)x + x0 + gx, oy = y + y0 + gy;
					if (ox < 0 || ox >= W || oy < 0 || oy >= H)
						continue;
					const int a = bm[gy * gw + gx];
					for (int k = 0; k < 3; k++) {
						unsigned char &d = rgba[(oy * W + ox) * 4 + k];
						d = (unsigned char)((d * (255 - a) + 255 * a) / 255);
					}
				}
			x += adv * sc;
		}
		y += lineH;
	}
}

static void Notify(const char *text)
{
	printf("[q1chap] %s\n", text);
	fflush(stdout);
	IVROverlay *o = VROverlay();
	if (!o || overlay == k_ulOverlayHandleInvalid)
		return;
	const int W = 900, H = 260;
	std::vector<unsigned char> rgba((size_t)W * H * 4);
	DrawText(rgba, W, H, text);
	EVROverlayError e1 = o->SetOverlayRaw(overlay, rgba.data(), W, H, 4);
	o->SetOverlayWidthInMeters(overlay, 0.9f);
	HmdMatrix34_t m = Translation(0, -0.12f, -1.2f);
	o->SetOverlayTransformTrackedDeviceRelative(overlay, k_unTrackedDeviceIndex_Hmd, &m);
	EVROverlayError e2 = o->ShowOverlay(overlay);
	printf("[q1chap] overlay: raw %s show %s\n", o->GetOverlayErrorNameFromEnum(e1), o->GetOverlayErrorNameFromEnum(e2));
}

static bool Pose(TrackedDeviceIndex_t dev, TrackedDevicePose_t &p)
{
	TrackedDevicePose_t poses[k_unMaxTrackedDeviceCount];
	sys->GetDeviceToAbsoluteTrackingPose(TrackingUniverseRawAndUncalibrated, 0, poses, k_unMaxTrackedDeviceCount);
	p = poses[dev];
	return p.bPoseIsValid && p.bDeviceIsConnected;
}

static std::vector<TrackedDeviceIndex_t> Controllers()
{
	std::vector<TrackedDeviceIndex_t> v;
	for (TrackedDeviceIndex_t i = 1; i < k_unMaxTrackedDeviceCount; i++)
		if (sys->IsTrackedDeviceConnected(i) && sys->GetTrackedDeviceClass(i) == TrackedDeviceClass_Controller)
			v.push_back(i);
	return v;
}

static bool TriggerDown(TrackedDeviceIndex_t dev)
{
	VRControllerState_t s;
	if (!sys->GetControllerState(dev, &s, sizeof(s)))
		return false;
	return s.rAxis[1].x > 0.8f || (s.ulButtonPressed & ButtonMaskFromId(k_EButton_SteamVR_Trigger));
}

static bool GripDown(TrackedDeviceIndex_t dev)
{
	VRControllerState_t s;
	if (!sys->GetControllerState(dev, &s, sizeof(s)))
		return false;
	return s.rAxis[2].x > 0.8f || (s.ulButtonPressed & ButtonMaskFromId(k_EButton_Grip));
}

// Raw-space polygon (x,z) + raw floor height -> standing zero pose + walls, committed live.
static bool Commit(const std::vector<Vec2> &poly, float floorY)
{
	float minx = 1e9f, maxx = -1e9f, minz = 1e9f, maxz = -1e9f;
	for (auto &p : poly) {
		minx = std::min(minx, p.x);
		maxx = std::max(maxx, p.x);
		minz = std::min(minz, p.z);
		maxz = std::max(maxz, p.z);
	}
	const float cx = (minx + maxx) / 2, cz = (minz + maxz) / 2;
	setup->RevertWorkingCopy();
	HmdMatrix34_t standing = Translation(cx, floorY, cz);
	setup->SetWorkingStandingZeroPoseToRawTrackingPose(&standing);
	// seated: head position at the standing origin, eye height 1.2 m (only used by seated apps)
	HmdMatrix34_t seated = Translation(cx, floorY + 1.2f, cz);
	setup->SetWorkingSeatedZeroPoseToRawTrackingPose(&seated);
	setup->SetWorkingPlayAreaSize(std::max(0.5f, maxx - minx), std::max(0.5f, maxz - minz));
	std::vector<HmdQuad_t> quads;
	for (size_t i = 0; i < poly.size(); i++) {
		const Vec2 a = poly[i], b = poly[(i + 1) % poly.size()];
		HmdQuad_t q;
		const float ax = a.x - cx, az = a.z - cz, bx = b.x - cx, bz = b.z - cz;
		q.vCorners[0] = {{ax, 0, az}};
		q.vCorners[1] = {{ax, kWallHeight, az}};
		q.vCorners[2] = {{bx, kWallHeight, bz}};
		q.vCorners[3] = {{bx, 0, bz}};
		quads.push_back(q);
	}
	setup->SetWorkingCollisionBoundsInfo(quads.data(), (uint32_t)quads.size());
	if (!setup->CommitWorkingCopy(EChaperoneConfigFile_Live)) {
		fprintf(stderr, "q1chap: CommitWorkingCopy failed\n");
		return false;
	}
	return true;
}

static int Status()
{
	ChaperoneCalibrationState st = chap->GetCalibrationState();
	printf("calibration state %d (200=OK)\n", (int)st);
	HmdQuad_t rect;
	if (chap->GetPlayAreaRect(&rect))
		printf("play area rect: (%.2f,%.2f) (%.2f,%.2f) (%.2f,%.2f) (%.2f,%.2f)\n", rect.vCorners[0].v[0],
		       rect.vCorners[0].v[2], rect.vCorners[1].v[0], rect.vCorners[1].v[2], rect.vCorners[2].v[0],
		       rect.vCorners[2].v[2], rect.vCorners[3].v[0], rect.vCorners[3].v[2]);
	else
		printf("no play area\n");
	uint32_t n = 0;
	setup->GetLiveCollisionBoundsInfo(nullptr, &n);
	printf("live collision bounds quads: %u\n", n);
	const HmdMatrix34_t *z = nullptr;
	(void)z;
	HmdMatrix34_t m;
	setup->ReloadFromDisk(EChaperoneConfigFile_Live);
	if (setup->GetWorkingStandingZeroPoseToRawTrackingPose(&m))
		printf("standing zero in raw: (%.3f, %.3f, %.3f)\n", m.m[0][3], m.m[1][3], m.m[2][3]);
	for (TrackedDeviceIndex_t i = 0; i < k_unMaxTrackedDeviceCount; i++) {
		if (!sys->IsTrackedDeviceConnected(i))
			continue;
		TrackedDevicePose_t p;
		Pose(i, p);
		const auto &mm = p.mDeviceToAbsoluteTracking.m;
		printf("device %u class %d valid %d raw pos (%.3f, %.3f, %.3f)", i, (int)sys->GetTrackedDeviceClass(i),
		       p.bPoseIsValid, mm[0][3], mm[1][3], mm[2][3]);
		if (sys->GetTrackedDeviceClass(i) == TrackedDeviceClass_Controller) {
			VRControllerState_t s;
			sys->GetControllerState(i, &s, sizeof(s));
			printf(" buttons %llx trig %.2f grip %.2f", (unsigned long long)s.ulButtonPressed, s.rAxis[1].x,
			       s.rAxis[2].x);
		}
		printf("\n");
	}
	return 0;
}

static int Default(float w, float d, float floorY)
{
	std::vector<Vec2> poly = {{-w / 2, -d / 2}, {-w / 2, d / 2}, {w / 2, d / 2}, {w / 2, -d / 2}};
	if (!Commit(poly, floorY))
		return 1;
	printf("committed %.2f x %.2f m rectangle, floor at raw y=%.3f\n", w, d, floorY);
	return 0;
}

static int Clear()
{
	setup->RevertWorkingCopy();
	setup->HideWorkingSetPreview();
	setup->SetWorkingCollisionBoundsInfo(nullptr, 0);
	setup->SetWorkingPlayAreaSize(0.5f, 0.5f);
	setup->CommitWorkingCopy(EChaperoneConfigFile_Live);
	printf("bounds removed\n");
	return 0;
}

// Wait until a controller trigger goes down (rising edge); returns the device, or 0 on timeout.
static TrackedDeviceIndex_t WaitTrigger(int timeoutS, bool *gripOut)
{
	bool wasT = true, wasG = true; // require a release first
	for (int t = 0; t < timeoutS * 50; t++) {
		for (auto d : Controllers()) {
			bool tr = TriggerDown(d), gr = GripDown(d);
			if (tr && !wasT) {
				*gripOut = false;
				return d;
			}
			if (gr && !wasG) {
				*gripOut = true;
				return d;
			}
			wasT = tr;
			wasG = gr;
		}
		Sleep(20);
	}
	return 0;
}

static int Guided()
{
	bool grip = false;
	Notify("Room setup: put a controller flat on the floor and pull its trigger");
	TrackedDeviceIndex_t d = WaitTrigger(300, &grip);
	if (!d)
		return 2;
	TrackedDevicePose_t p;
	Pose(d, p);
	// the controller body sits ~1.5 cm above the surface it lies on
	const float floorY = p.mDeviceToAbsoluteTracking.m[1][3] - 0.015f;
	char msg[200];
	snprintf(msg, sizeof(msg), "Floor set (%.2f m). Now pull the trigger at each corner of your room, grip to finish", floorY);
	Notify(msg);
	std::vector<Vec2> poly;
	for (;;) {
		d = WaitTrigger(600, &grip);
		if (!d)
			return 2;
		if (grip) {
			if (poly.size() >= 3)
				break;
			Notify("Need at least 3 corners: keep pulling the trigger at the room corners");
			continue;
		}
		Pose(d, p);
		if (poly.size() >= 2 || poly.empty())
			poly.push_back({p.mDeviceToAbsoluteTracking.m[0][3], p.mDeviceToAbsoluteTracking.m[2][3]});
		else
			poly.push_back({p.mDeviceToAbsoluteTracking.m[0][3], p.mDeviceToAbsoluteTracking.m[2][3]});
		snprintf(msg, sizeof(msg), "Corner %zu added. Trigger: next corner, grip: finish (%zu corners)", poly.size(),
		         poly.size());
		if (poly.size() >= 3)
			Commit(poly, floorY); // live preview of the boundary so far
		Notify(msg);
	}
	if (!Commit(poly, floorY)) {
		Notify("Saving the boundary failed");
		return 1;
	}
	Notify("Boundary saved");
	Sleep(3000);
	return 0;
}

static int Register(const char *self)
{
	char exe[512];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (n <= 0)
		return 1;
	exe[n] = 0;
	(void)self;
	std::string dir = std::string(getenv("HOME") ? getenv("HOME") : "/tmp") + "/.config/q1chap";
	std::string cmd = "mkdir -p " + dir;
	if (system(cmd.c_str()))
		return 1;
	std::string path = dir + "/q1chap.vrmanifest";
	FILE *f = fopen(path.c_str(), "w");
	if (!f)
		return 1;
	fprintf(f,
	        "{\n \"source\": \"builtin\",\n \"applications\": [{\n"
	        "  \"app_key\": \"openvr.tool.steamvr_room_setup\",\n  \"launch_type\": \"binary\",\n"
	        "  \"binary_path_linux\": \"%s\",\n  \"arguments\": \"setup\",\n  \"is_dashboard_overlay\": false,\n"
	        "  \"strings\": {\"en_us\": {\"name\": \"Quest 1 room setup\"}}\n }]\n}\n",
	        exe);
	fclose(f);
	EVRApplicationError e = VRApplications()->AddApplicationManifest(path.c_str(), false);
	printf("AddApplicationManifest %s: %s\n", path.c_str(), VRApplications()->GetApplicationsErrorNameFromEnum(e));
	return e == VRApplicationError_None ? 0 : 1;
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "setup";
	EVRInitError err = VRInitError_None;
	sys = VR_Init(&err, VRApplication_Overlay);
	if (err != VRInitError_None) {
		fprintf(stderr, "q1chap: VR_Init: %s\n", VR_GetVRInitErrorAsEnglishDescription(err));
		return 1;
	}
	chap = VRChaperone();
	setup = VRChaperoneSetup();
	if (!chap || !setup) {
		fprintf(stderr, "q1chap: no chaperone interface\n");
		VR_Shutdown();
		return 1;
	}
	if (VROverlay())
		VROverlay()->CreateOverlay("q1.chaperone.setup", "Quest 1 room setup", &overlay);
	else
	{
		EVRInitError ie = VRInitError_None;
		VR_GetGenericInterface(IVROverlay_Version, &ie);
		fprintf(stderr, "q1chap: no overlay interface (%s); prompts on stdout only\n",
		        VR_GetVRInitErrorAsEnglishDescription(ie));
	}
	if (FILE *ff = fopen("/usr/share/fonts/Adwaita/AdwaitaSans-Regular.ttf", "rb")) {
		fseek(ff, 0, SEEK_END);
		fontData.resize(ftell(ff));
		fseek(ff, 0, SEEK_SET);
		if (fread(fontData.data(), 1, fontData.size(), ff) != fontData.size() ||
		    !stbtt_InitFont(&font, fontData.data(), 0))
			fontData.clear();
		fclose(ff);
	}
	int rc = 0;
	if (!strcmp(cmd, "status"))
		rc = Status();
	else if (!strcmp(cmd, "default"))
		rc = Default(argc > 2 ? (float)atof(argv[2]) : 2.5f, argc > 3 ? (float)atof(argv[3]) : 2.5f,
		             argc > 4 ? (float)atof(argv[4]) : 0.0f);
	else if (!strcmp(cmd, "clear"))
		rc = Clear();
	else if (!strcmp(cmd, "launch")) {
		EVRApplicationError e = VRApplications()->LaunchApplication("openvr.tool.steamvr_room_setup");
		printf("LaunchApplication: %s\n", VRApplications()->GetApplicationsErrorNameFromEnum(e));
		rc = e == VRApplicationError_None ? 0 : 1;
	} else if (!strcmp(cmd, "register"))
		rc = Register(argv[0]);
	else
		rc = Guided();
	if (overlay != k_ulOverlayHandleInvalid)
		VROverlay()->DestroyOverlay(overlay);
	VR_Shutdown();
	return rc;
}
