// SteamVR's raw tracking space for driver_quest1 (meta 6DoF mode), anchored at every put-on.
//
// Meta's tracking has no persistent map here: its world origin is wherever the headset was when the
// stack started. So that the floor and the boundary saved in chaperone_info.vrchap stay valid after a
// reboot, the driver defines the raw space itself: each time the headset is put on (panels on,
// after at least 20 s off, or at start-up), the raw origin is the head's position on the floor
// (x and z of the head, y = QUEST1_EYE_HEIGHT above the floor) and raw -z is where the head looks.
// Putting the headset on at the centre of the play area, facing the front, gives the same raw space as
// when the room was set up. In 3DoF mode (position not tracked) nothing is anchored: y = eye height.
#pragma once

#include <openvr_driver.h>
#include <openxr/openxr.h>

//! Eye height above the floor in the raw space (1.65 m, QUEST1_EYE_HEIGHT).
void Quest1FrameInit(float eyeHeight);
//! Head pose in the OpenXR local space, called for each head pose; positionTracked = 6DoF.
void Quest1FrameUpdate(const XrPosef &head, bool positionTracked);
//! Sets qWorldFromDriverRotation and vecWorldFromDriverTranslation of a pose in the local space.
void Quest1FrameApply(vr::DriverPose_t &pose);
