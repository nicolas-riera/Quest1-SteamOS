// Touch controllers for driver_quest1: read from Monado through OpenXR actions, exposed to SteamVR
// as two oculus_touch controllers (input profile {quest1}/input/quest1_touch_profile.json).
#pragma once

#include <openvr_driver.h>
#include <openxr/openxr.h>

#include <functional>

struct ControllerXr
{
	PFN_xrGetInstanceProcAddr getProcAddr = nullptr;
	XrInstance instance = XR_NULL_HANDLE;
	XrSession session = XR_NULL_HANDLE;
	XrSpace local = XR_NULL_HANDLE; //!< the space the HMD pose is reported in
	std::function<XrTime()> now;    //!< current time in XrTime
	float headHeight = 1.65f;       //!< vecWorldFromDriverTranslation[1] of the HMD
};

class Quest1Controllers
{
public:
	//! Creates the action set, attaches it to the session, adds both devices to SteamVR and
	//! starts the update thread. False if OpenXR refused the actions (no controllers then).
	bool Init(const ControllerXr &xr);
	void Shutdown();
	//! vrserver events from IServerTrackedDeviceProvider::RunFrame (haptics).
	void OnEvent(const vr::VREvent_t &e);
};
