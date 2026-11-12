// quest1_hmd_pose.h: the head's pose for Steam Link VR, from driver_quest1 to vrlink (src/v4l2shim/quest1_vrlink.c).
//
// The HMD pose driver_quest1 gives SteamVR only changes when vrcompositor presents a frame, and has no velocity:
// vrcompositor then renders each frame with exactly the pose the layer hands Monado (driver_quest1.cpp,
// XrBackend::LocateAhead). vrlink reads that same pose (SteamVR's raw driver pose, IVRClientInternal), predicts it
// with its velocity and sends it to the PC, which renders its frames for it. Without a velocity, the PC got a pose
// that stood still between the headset's frames, then jumped, at uneven times (10 to 27 ms apart while the head
// turned, 2026-11-12): the PC's frames, and its spectator view, shook far more than the head (about 6 times, from a
// recorded head turn). So driver_quest1 also writes here, at 250 Hz, the head's current pose with its velocities,
// and vrlink is given that one instead: the PC predicts a smooth pose as it does for a Steam Frame, and the headset's
// own frames are unchanged.
#pragma once
#include <stdint.h>

#define Q1_HMD_POSE_PATH "/dev/shm/quest1-hmd-pose"

struct q1_hmd_pose
{
	uint32_t seq;       //!< odd while the pose below is written
	uint32_t valid;     //!< 1 when the pose below is tracked
	int64_t publish_ns; //!< CLOCK_MONOTONIC time of the driver's last HMD pose for SteamVR (TrackedDevicePoseUpdated)
	int64_t reader_ns;  //!< CLOCK_MONOTONIC, written by vrlink as it reads: the driver writes only while it is recent
	int64_t time_ns;    //!< CLOCK_MONOTONIC time the pose below is the head's
	double q[4];        //!< orientation w, x, y, z, in the driver's space (as DriverPose_t::qRotation)
	double pos[3];      //!< position, metres, driver's space
	double vel[3];      //!< linear velocity, m/s, driver's space
	//! angular velocity, rad/s, in the head's own frame: vrlink turns DriverPose_t::vecAngularVelocity by the head's
	//! orientation (the PC then predicted a nod as a roll, and the view leaned as the headset was lifted, 2026-11-12;
	//! the PC's poses fitted the head's turns with a cosine of 0.99 this way, against ~0 taken as the driver's space)
	double ang[3];
};
