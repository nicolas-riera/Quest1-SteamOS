// Meta's hand tracking for driver_quest1 (docs/hand-tracking.md): reads q1handsd's shared memory and turns Meta's
// 24-joint hand into SteamVR's 31-bone hand skeleton (IVRDriverInput skeleton components), with the pinch and palm
// gestures. No OpenXR here: controllers.cpp puts the result into the OpenXR local space.
#pragma once

#include <openvr_driver.h>

#include <cstdint>

namespace q1hands {

constexpr int kBones = 31; // OpenVR's hand skeleton, eBone_Count

struct Frame
{
	bool valid = false;   //!< q1handsd is running and fresh
	bool handsMode = false; //!< the system is in hand tracking mode (the controller constellation is off)
	bool tracked = false; //!< this hand has a pose
	int64_t sampleNs = 0; //!< CLOCK_MONOTONIC time of the hand sample
	bool headValid = false;
	double headRot[4] = {1, 0, 0, 0}; //!< w, x, y, z; the head at the same time, in libossdk's tracking space
	double headPos[3] = {};
	//! the skeleton's frame (wrist; fingers' model space as in SteamVR's hand skeleton), tracking space
	double rot[4] = {1, 0, 0, 0}; //!< w, x, y, z
	double pos[3] = {};
	vr::VRBoneTransform_t bones[kBones]; //!< parent-space, bone 0 (root) identity
	double pinchPoint[3] = {}; //!< midway between the thumb's and the index finger's tips, tracking space
	double palmNormal[3] = {}; //!< out of the palm, tracking space
	double palmCenter[3] = {};
	double pinchDist = 1;      //!< thumb tip to index tip, meters
	bool controllerInHand = false; //!< libossdk's own detection for this hand's controller
	bool controllerConnected = false;
};

//! The newest sample of hand `h` (0 left, 1 right).
bool Read(int h, Frame *f);

//! A hand holding nothing, fingers curled by 0..1 (thumb, index, middle, ring, pinky): for a controller's skeleton.
//! False until the skeleton resources are loaded.
bool CurledBones(int h, const float curl[5], vr::VRBoneTransform_t out[kBones]);

} // namespace q1hands
