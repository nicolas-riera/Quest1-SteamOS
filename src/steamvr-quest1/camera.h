// Tracking cameras of the Quest 1 as the HMD's SteamVR camera (IVRCameraComponent_003), so that
// SteamVR's camera features (settings "Camera", IVRTrackedCamera clients) see the outside world.
// Frames come from the shared-memory ring of quest1_camera_ring.h (or a test pattern); the driver
// never touches SyncBoss or the camera hardware itself. See docs/passthrough.md.
//
// Off unless enabled: QUEST1_CAMERA=ring|test (environment of vrserver), or the first word of
// /etc/quest1-camera. Other knobs (environment): QUEST1_CAMERA_RING, QUEST1_CAMERA_IDS,
// QUEST1_CAMERA_FORMAT, QUEST1_CAMERA_GAMMA, QUEST1_CAMERA_AUTOGAIN, QUEST1_CAMERA_ROT,
// QUEST1_CAMERA_CALIB, QUEST1_CAMERA_HFOV, QUEST1_CAMERA_IMAGE_OFFSET, QUEST1_CAMERA_QUEUE.
#pragma once

#include <openvr_driver.h>

#include <functional>

//! From the HMD's Activate: sets the camera properties on the HMD's container (when enabled).
//! headPose returns the HMD's current pose (stamped into each frame as m_RawTrackedDevicePose); poseAt the head's pose in
//! the raw tracking space at a CLOCK_MONOTONIC time (for the passthrough provider: quest1_camera_ring.h, q1cam_poses).
void Quest1CameraAttach(vr::PropertyContainerHandle_t container, std::function<vr::DriverPose_t()> headPose,
                        std::function<bool(int64_t monoNs, vr::HmdMatrix34_t &raw)> poseAt);

//! For the HMD's GetComponent(IVRCameraComponent_Version): the component, or nullptr when disabled.
vr::IVRCameraComponent *Quest1CameraComponent();

//! From the HMD's Deactivate: stops the stream and forgets the container.
void Quest1CameraDetach();
