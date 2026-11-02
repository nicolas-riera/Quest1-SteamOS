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
`src/camtap/camoverlay.cpp` (build: `build_overlay.sh` -> `/usr/local/bin/q1-camoverlay`; installed with
`device/holo-passthrough.sh install`, which also writes the persistent camtap drop-in, `q1-camoverlay.service` wanted by
`quest1-steamvr.service`, and `/usr/local/bin/q1-passthrough on|off|toggle`). A SteamVR overlay that follows the HMD, 1.6 m
wide at 1 m, cameras 0 and 2 side by side (not eye-aligned, no depth correction), shown while `/tmp/quest1-passthrough` exists.
- Frame upload: `SetOverlayRaw` does not work here (vrclient's memory blocks are never released: "201 blocks already
  outstanding", frozen after ~200 frames, fd leak). The overlay writes a TGA to `/dev/shm` and calls `SetOverlayFromFile`
  (8 Hz default, `PT_HZ`; fds stable, no timeouts at 6 Hz; 15 Hz made the compositor time out). A Vulkan `SetOverlayTexture` was
  not tried (needs external-memory fd export on the Adreno blob).
- The overlay connects as a Background app after vrserver and vrcompositor run: an Overlay-type connection starts vrserver and
  the dashboard inside the service's cgroup (stopping the unit would kill them).

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

## Route (a), further static findings (2026-11-02)
- No public client and no SteamVR UI code sends `TrackedCamera_Enable` (msg 430, vrserver handler registered at 0xd9788). The dashboard
  JS only has `StartCameraTest/GetCameraTestFrame/SetCameraCompatibilityMode`; `AcquireVideoStreamingService` only talks to
  vrcompositor (flag byte, returns a handle) and then queries frame bounds (3 msgs).
- vrcompositor decides roomview per frame (0x15fcc0): settings snapshot byte +830 vs running state +837. Inputs: camera.enableCamera,
  camera.roomView (>0; >1 = 3D needs HMD prop 2079), chaperone "force roomview" (prop 2500), the ToggleRoomView action.
  The compositor mailbox has no handler for set_camera_roomview_*.
- The Steam Frame path is IVRCameraPassthroughInternal_001 + `VR_CameraPassthroughState` (25088 bytes, two per-camera-type blocks at
  +0x14 and +0x20e8, 0x1040-byte records): a provider writes camera frame dma-bufs and a spline distortion texture there; the
  compositor imports them (CompositorImportDmaBuf). That provider (Frame driver) is not on this device.
- Next: live gdb session (vrserver 0x119bf8, 0x118780; vrcompositor 0x15f9e0, 0x15fcc0) while triggering roomview, to see whether
  the legacy IVRCameraComponent path is ever used.

## Route (a) design: act as the Frame's camera provider (phase 1, offline reverse engineering, not built)
SteamVR 2.17's roomview/passthrough has no use for IVRCameraComponent. vrcompositor (CTrackedCamera) is a *consumer* of
`VR_CameraPassthroughState`; the Steam Frame's driver (not shipped here) is the *provider*, through
`IVRCameraPassthroughInternal_001` (vrserver hands it to drivers from its internal-interface list at 0x29832c; a driver also has
`IVRIPCResourceManagerClient_003`, in `openvr_driver.h`, with `ImportDmabuf`, `NewSharedVulkanImage`, `RefResource`, ...).
The gate hack (`QUEST1_CAMERA_GATE`, byte 24 + block queues) only made the compositor start roomview with no frames: its watchdog
aborted it repeatedly. It is now OFF by default.

### State layout (25088 = 0x6200 bytes; offsets from vrclient `CVRCameraPassthroughInternal` and vrcompositor 0x15f310/0x15f048)
- Two camera-type blocks (0x20d4 bytes) at +0x14 and +0x20e8. In a block (b = block start):
  - b+0: u32 generation/camera-set id (compositor resets its per-set state when it changes); b+4: u8 "type enabled" (abs 24 / 8428);
    b+5: u8; b+8: u32 record count N (<= 16 per camera); b+64: u8 "ready" (abs 0x54 / 0x2128).
  - b+0x30..: pose/timestamp data the compositor copies each frame (double at abs 0x41bc, 0x30 bytes at abs 0x41c4).
  - Pool of shared handles (u64 each) imported once at start (vt[296] of the compositor's texture manager, kinds 1 = frame images,
    2 = the two spline-distortion images, count up to 0x300 bytes): the provider registers its dma-buf images up front.
  - Records, camera c (0..1 within the block), index k: at b + c*0x1040 + k*0x104 + 0x44: `{u32 state; u8 desc[248]; u64 sharedHandle}`
    (260 bytes). state: 0 free, 1 = frame ready (provider), 2 = taken by the compositor, which frees older records back to 0.
    desc (248 bytes, memcpy'd by the compositor; written by `SetRecordDescriptor`, vrclient 0x1a4f30): three HmdMatrix34_t (poses /
    camera-to-head) plus floats (quaternion at +32..+44, flags at +224) and timestamps: exact field map still to do (live gdb or the
    Frame's descriptor from a dump).
- Provider API (vtable of CVRCameraPassthroughInternal, vrclient 0x60cc90): ImportFrame(rec, bool*, DmabufAttributes_t*) (0x1a4808:
  `ImportDmabuf` + `RefResource`, handle stored at rec+252), ReleaseFrame (0x1a4900: `UnrefResource`, clears the record),
  MarkRecord(type, cam, k) (0x1a4df8: sets state 1 and clears other pending ones), SetRecordDescriptor (0x1a4f30), GetRecordStates
  (0x1a4cd0), GetEnabled (0x1a4a78), Get/SetParams (0x1a49d8, 0x1a4bf8), GetCameraInfo (0x1a44b0).

### Who writes what
Provider (our driver, on the camtap ring at 30 Hz): once, pick DRM format + modifier (`GetDmabufFormats/Modifiers`), import N frame
images + 2 spline images, fill the handle pool, set block enabled/ready, then per frame: write pixels (gray -> RGBA, rectified or not),
write desc (head pose at capture, camera calibration, exposure), `MarkRecord`. The compositor does the rest (roomview shader,
depth-less projection, stencil, `camera.roomView`/`roomViewStyle` settings, ToggleRoomView action, chaperone auto-on, the
`m_bRoomViewStreaming` UI state). roomview is switched on by `camera.enableCamera` + `camera.roomView` > 0 (settings snapshot byte
vrcompositor+830 vs running +837) and the HMD props 2079 (depth projection), 2105 and 2122 (camera settings model, Frame = 1).
Spline distortion texture: a small float image (2 of them, one per camera type) sampled by the roomview shader; format not yet known
(read 0x15f048/0x15ec20 render setup, resource kind 2).

### dma-buf source
camtap (bionic, in trackingservice) already holds the frame; the driver maps the ring. The dma-buf must come from a heap the Adreno
stack can import: ION (`/dev/ion`, kernel 4.4, system/cma heap) or the gralloc HAL used for AHB swapchains (the Monado direct-scanout
path allocates them). The driver writes the pixels with the CPU (mmap) and imports the fd with `ImportDmabuf`.

### Risks
1. vrcompositor's `ImportDmabuf` needs `VK_EXT_external_memory_dma_buf` (+ modifiers) on the Adreno 540 blob: not available natively;
   the compat layer (libVkLayer_quest1_steamvr_compat) emulates external memory with AHB, so dma-buf import has to be added there.
   This is the main unknown.
2. Descriptor layout and spline texture format are not fully known; wrong values give a garbled view, not a crash, but the
   compositor watchdog aborted when roomview waited without frames, so a half-working provider must not leave it enabled.
3. The gate/roomview must stay off by default; the provider should only enable the type blocks while frames flow.
4. IVRCameraPassthroughInternal_001 may be refused to a non-Frame driver (the IVRBlockQueue lookup refused unknown versions).
5. Not eye-aligned (tracking cameras sit below/besides the eyes) and mono-per-camera: the shader assumes the Frame's geometry.

### Effort
Static map is about 70 % done. Remaining: descriptor/spline details (0.5 day, live gdb), driver provider + ION/gralloc dma-buf
(1 day), layer dma-buf import (1 to 2 days, the risk). About 3 days, with a real chance that vrcompositor's import path needs a
different approach (e.g. a shared AHB image via NewSharedVulkanImage + mappable).
