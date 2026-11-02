# Passthrough (camera view) on the Quest 1

## Pipeline
- `src/camtap/camtap.c` (LD_PRELOAD in Meta's `android-trackingservice`, drop-in
  `/run/systemd/system/android-trackingservice.service.d/camtap.conf`: `Environment=LD_PRELOAD=/data/local/tmp/libq1camtap.so`)
  copies the 4 tracking cameras (640x480 gray, 30 Hz) into `/dev/shm/quest1-cameras` (ring, see
  `src/steamvr-quest1/quest1_camera_ring.h`) only while a reader touches `/dev/shm/quest1-cameras.want` (<3 s old). Frames
  flow only while the panels' TE runs (headset worn).
- With camtap loaded, head and both controllers stay valid (~490 poses/s, trackingservice ~93 % CPU worn, no rise).
- The driver (`camera.cpp`) offers an `IVRCameraComponent` (cameras 0 and 2, stacked, rotated upright, 480x1280 RGBX).

## Route (b), works now: head-locked overlay
`src/camtap/camoverlay.cpp` (build: `build_overlay.sh`, installed as `/usr/local/bin/q1-camoverlay`, script
`/usr/local/bin/q1-passthrough on|off|toggle`): a SteamVR overlay that follows the HMD, 1.6 m wide at 1 m, with cameras 0 and 2
side by side (not eye-aligned, no depth correction). Shown while `/tmp/quest1-passthrough` exists. Verified: overlay visible
in `vrcmd --overlays` at 960x640 and the ring delivers 30 Hz; the image itself was not looked at yet.

## Route (a), SteamVR's own camera path: findings
- `AcquireVideoStreamingService` is answered by **vrcompositor** (CTrackedCamera), not vrserver. It needs
  1. the "camera enabled" byte of the shared state `VR_CameraPassthroughState` (25088-byte `/dev/shm/u<uid>-Shm_*` file, byte 24):
     nothing in vrserver sets it for a non-Frame HMD (the Frame driver does);
  2. the block queues `/xrservice/roomview3d/meshData` and `/meshLayout` (the Frame's depth-mesh producer).
- `camera.cpp` now does both from the HMD's Activate (`CompositorGate`): maps the state file and sets byte 24, creates the queues
  with `IVRBlockQueue_004` (vrserver has no `_005`; asking for an unknown version crashes; Create needs header size >= 16; called
  through the vtable by hand because the compiler turns calls on an abstract class into `__cxa_pure_virtual`).
  Result: Acquire returns OK ("Depth mesh block queues connection successful").
- Still missing: nothing sends the `TrackedCamera_Enable` message (id 430) to vrserver, so the driver's component is never called
  and the client's frame buffers (`VR_TrackedCameraStream_<n>`) never exist (`GetVideoStreamFrameBuffer` -> InvalidHandle).
  The compositor's roomview probably sends it, but we found no trigger (`vrcmd --compositorcmd set_camera_roomview_*` has no
  handler). Unknowns: the depth-mesh block format, and whether the compositor can import the camera frame texture here.
- Test client: `src/camtap/trackedcam_test` (`PT_ROOMVIEW=1` toggles `camera.roomView`).
