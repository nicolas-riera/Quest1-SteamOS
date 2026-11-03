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

## Route (a): feasibility results and the provider (2026-11-02, evening)
Tested live with `src/camtap/ptres_test.cpp` (read-only client, `build_probe.sh`; needs `PT_OVERLAY=1`: a Background client has no
compositor connection and every allocation fails):
- `IVRCameraPassthroughInternal_001` and `IVRIPCResourceManagerClient_003` are obtainable by a plain Overlay-type client: the provider
  does not have to be driver code.
- `GetDmabufFormats` returns false (no `VK_EXT_external_memory_dma_buf` on the blob): dma-buf import is dead, as expected.
- `NewSharedVulkanImage` (non-mappable, with or without renderable) and `NewSharedVulkanBuffer` work. `RefResource` +
  `ReceiveSharedFd` returns an fd whose target is `anon_inode:dmabuf`, and `mmap(RW)` of w*h*4 bytes works: the blob's opaque fd is a
  real dma-buf, so a provider can write pixels with the CPU. `mappable = true` returns true but a null handle (do not use).
  Still unknown: the memory layout (optimal tiling on the A540). If tiled, upload through a small Vulkan copy instead.
- Layout completed (vrcompositor 0x15f048): per block, spline handles at b+0x20c4 + 8*camera, frame handles at record+0xfc (record
  = {u32 state, desc[248], u64 handle}), 16 records per camera, 2 cameras per block, block 1 is used while roomview runs.
- `src/camtap/ptprovider.cpp` (built by `build_probe.sh`): the provider. Allocates N shared images per camera, publishes the pool,
  writes frames from the camtap ring (or `PT_PATTERN=1` synthetic gradient), fills a guessed descriptor, flips record states, enables
  the block byte only while frames flow and clears it on exit/stall. Not run yet (user was testing): first live run must use
  `PT_PATTERN=1 PT_DUMP=1` and watch `vrcompositor.txt` for "Failed to import the shared camera frame texture".
- Unknowns to settle live: image layout/format the compositor expects, the descriptor fields, the spline texture format, whether the
  state mutex must be taken, and which of the two blocks roomview reads.

## Roomview trigger and mesh (2026-11-02, late)
- Trigger: vrcompositor arms roomview only when the HMD reports `Prop_HasCamera_Bool` (the driver's `/etc/quest1-camera` = `props`
  sets HasCamera/AllowCameraToggle/NumCameras and creates the empty mesh queues; no camera component) and `camera.enableCamera` +
  `camera.roomView` are set; changing `camera.roomView` from a client (false then true, see `trackedcam_test` `PT_ROOMVIEW=1`) starts
  it. With `ptprovider` running the compositor then logs "Depth mesh block queues connection successful" and "Late initialization
  successful", takes and frees the provider's records (no import error), but draws nothing yet.
- Why nothing is drawn: the compositor builds the roomview geometry from the depth mesh and waits for it. It reads, through
  property containers on the block-queue headers, `/meshLayout/layoutId`, `/meshLayout/vertexCount`, `/meshLayout/indexCount`
  (queue `/xrservice/roomview3d/meshLayout`) and `/meshData/meshId`, `/meshData/layoutId`, `/meshData/meshIsUpToDate`
  (queue `.../meshData`), then calls its geometry builder (vrcompositor 0x15e300: vertex count x float3, index count x u32).
  So the driver (the only place that may write the server-only block queues) must publish a mesh: layout block once, data block
  with vertices (a plane or dome in head space) and indices, then set the header properties (IVRProperties on the block's header
  container). Block data layout and property types to confirm live.
- Forcing it in a gdb session (`$x27 = 1` at vrcompositor 0x9ee6c) is no longer needed.

## Roomview 3D mode and the mesh reads (2026-11-02, night)
- vrcompositor draws the roomview geometry only in the 3D mode: `camera.roomView = 2` AND HMD prop 2079
  (`Prop_SupportsRoomViewDepthProjection_Bool`); the driver's `props` mode now sets 2079 and 2500
  (`Prop_Hmd_ForceRoomViewOutsideChaperone_Bool`, `QUEST1_CAMERA_FORCE=0` to skip; it did not trigger roomview by itself without a
  playspace). A client toggles it with `trackedcam_test` `PT_ROOMVIEW=2` (SetInt32 0 then 2). Result with `ptprovider` running:
  "Depth mesh block queues connection successful", "Late initialization successful", records consumed.
- The mesh reads (vrcompositor 0x15fe98 and 0x160130) go through the block-queue interface object (vtable slot 0, queue handle in x1)
  with a std::string key per property ("/meshLayout/layoutId" ...): the header "properties" are block-queue header fields, not
  device properties. The key/value encoding still has to be read from vrserver's implementation (CVRBlockQueue_004 thunks at
  vrserver 0x77ec8.., manager vtable at 0x25d208..) to know what the driver must write.
- Without a mesh the roomview draws nothing. Next: read the manager's header-property write API, publish layout + data blocks
  (flat quad), verify with a synthetic pattern.

## Depth mesh protocol and the "show" params (2026-11-03)
Reversed from vrcompositor 0x15fcc0 (per-frame roomview update), 0x15e300 (geometry build) and vrclient CVRCameraPassthroughInternal.
- Block queue header "properties" are IVRPaths_001 values (`ReadPathBatch/WritePathBatch/StringToHandle/HandleToString`, vtable slots
  0..3; a driver gets it with `GetGenericInterface("IVRPaths_001")`; PathWrite_t/PathRead_t as in openvr_capi.h, the driver SDK header
  lacks them). Root = the queue handle (queue-wide) or the handle of an acquired block (per block), the key is a path string.
- Layout queue `/xrservice/roomview3d/meshLayout`, one block: buffer = `indexCount` 32-bit words (the compositor swaps the 2nd/3rd
  word of each triple), but drawn as `indexCount` **16-bit** indices: see "The 3D roomview works" below; block props `/meshLayout/layoutId` (u64, tag 3), `/meshLayout/vertexCount` (i32, tag 2),
  `/meshLayout/indexCount` (i32). The compositor rebuilds its GPU buffers only when layoutId changes.
- Data queue `/xrservice/roomview3d/meshData`: buffer = `vertexCount` x float3 vertices; block props `/meshData/meshId` (u64, must
  change for the compositor to re-read), `/meshData/layoutId` (u64, must equal the layout's), `/meshData/elementCount` (i32 = 3 x
  vertexCount), `/meshData/rawFromMeshPose` (HmdMatrix34_t, tag 20), `/meshData/posePredictionBaseTime` (double, tag 7);
  queue-wide (root = the queue handle) `/meshData/meshIsUpToDate` (bool, tag 4, must be 1).
- Write sequence in the driver (`camera.cpp` `CompositorGate::PublishMesh`, props mode): AcquireWriteOnlyBlock (IVRBlockQueue vtable
  slot 3), WritePathBatch per key on the block handle, ReleaseWriteOnlyBlock (slot 4); queues created with header size 1024. Every write
  returns no error. The mesh is a static 3 m sphere (inside view, `QUEST1_CAMERA_MESH_RADIUS`) for now.
- The "params" of IVRCameraPassthroughInternal (GetParams/SetParams) are 5 bytes at state+2 of the shared state. The compositor shows
  the roomview only while `state[2]` != 0 (GetParams(nullptr), checked each frame at vrcompositor 0x9ee6c: this is the "w27" that used
  to be forced in gdb) and reads `state[4]` into its snapshot byte +830 (compared with the running state +837). The Frame driver sets
  them; `ptprovider` now sets both while frames flow (`SetShow`). Also: a client has to hold `AcquireVideoStreamingService`
  (`trackedcam_test` with `PT_ROOMVIEW=2 PT_HOLD=<s>`) for the compositor to keep the depth mesh queues connected, and the provider must
  have been enabled for ~15 s before that (otherwise "Failed to start RoomView since there's no camera enabled").
- Layer snapshots taken before the params were set showed only SteamVR's default orange background (the mesh was not read:
  vrcompositor 0x15fdd4, the read section, needs byte +835 which needs state[2]). Drawing with the params set: see todo.
- `ptprovider` `PT_PATTERN=2` draws a 32 px checkerboard (corner marked) to check the shared image memory layout; `PT_PATTERN=1` the gradient.
- More gates found (2026-11-03 pm): per frame the compositor (0x9ee6c..0x9ee8c) needs byte +3421 of its frame state, copied from the 8th
  argument of 0x63920, = `camera.forceSceneAlphaBlending` or app flag bit 19; with the setting true it still read 0 here. Forcing it in
  gdb (`$x27 = 1` at 0x9ee6c) made the compositor build and upload the sphere (1225 vertices, 6912 indices) but a layer snapshot with the
  pattern provider still showed only SteamVR's default scene: the draw has further conditions not found yet (candidates: roomViewStyle,
  stencil/chaperone bounds, byte +833/+864 in 0x15fcc0). `trackedcam_test` has `PT_BLEND=1` to set the setting.

## Switching the roomview on: the system layer mailbox (2026-11-03, evening)
- vrcompositor's system layer (CSystemLayer::RegisterMailboxHandlers) listens on the mailbox `vrcompositor_systemlayer` for JSON
  messages `{"type": ..., ...}`: `set_camera_roomview_enabled`, `set_camera_roomview_enabled_forsceneapps`,
  `toggle_camera_roomview_forsceneapps`, the playspace commands (`set_playspace_mode`, `commit_playspace_bounds_drawing`, ...),
  `set_scene_alpha_target`, `fade_to_color`... SteamVR's web UI sends them through vrserver's web socket (127.0.0.1:27062,
  "mailbox_send <target> <json>"), which answers 401 without its token; a native client uses vrclient's `IVRMailbox_001`
  (RegisterMailbox, UndoRegisterMailbox, SendMessage, ReadMessage): `src/camtap/vrmailbox`.
- The draw gate (vrcompositor 0x9ee5c..0x9ee8c, byte +835 = b1581 && w27): w27 = show (state[2], from the provider) && either
  - scene alpha (+3421, from `camera.forceSceneAlphaBlending` or the app's alpha-blend flag), only while a scene app submits
    frames (an app that only calls WaitGetPoses does not count: `src/camtap/vrscene_idle`); or
  - system layer byte +1580, copied each frame from the shared byte 2889 while camera.enableCamera && camera.roomView > 0
    (byte this+0x1572). With no playspace set up (construction mode, shared float +636 > 0), 1580 follows
    `camera.enableConstructRoomView`, which `set_camera_roomview_enabled` sets (and saves): the boundary-drawing passthrough.
    Otherwise `..._forsceneapps` stores the roomview state in 2889.
- With the props driver, ptprovider (pattern), `trackedcam_test PT_ROOMVIEW=2 PT_HOLD=` and `set_camera_roomview_enabled`, the
  compositor logs "Augmented passthrough geometry updated" and reaches the "Cam: Room View" draw (0x161ec4), then crashes: the
  roomview pipeline failed to build on the Adreno blob (vkCreateGraphicsPipelines -13 in the compat layer's log) and
  0x2bdcb4 calls vkCmdBindPipeline with a null pipeline. Next: dump its two modules (`/etc/quest1-spirv-dump`, compat layer)
  and make the blob accept them.
- `camera.enableConstructRoomView` stays saved: until the pipeline works it crash-loops vrcompositor at every start. Undo the
  test settings with `/root/pt-undo.sh` (stops SteamVR, removes `/etc/quest1-camera`, drops enableConstructRoomView and
  forceSceneAlphaBlending, roomView 0, starts SteamVR).

## The roomview pipeline on the Adreno 540 (2026-11-03, night)
- vrcompositor's roomview vertex shader samples image 11 + sampler 27 (a screen-space warp, `_m12 == 1`) and image 10 + sampler 26
  (the spline distortion, `_m7 == 2`), but its pipeline layout has no 11/27 and gives 25/26 to the fragment stage only. The blob
  refuses the pipeline (-13), vrcompositor binds VK_NULL_HANDLE and crashes in vkCmdBindPipeline (vrcompositor 0x2bdcb4).
- The compat layer now records set and pipeline layouts and module code; a refused graphics pipeline is logged in detail and retried
  with its modules rewritten: image/sampler variables the layout lacks for that stage are removed and their sample/fetch/query
  results become OpConstantNull (`null_missing_descriptors`). Checked offline with `src/vklayer-steamvr/pipetest`
  (`PT_VRC_LAYOUT=1` reproduces vrcompositor's layout; `QUEST1_VKSHIM_LAYER` picks a test build of the layer).
- Live: 4 roomview pipelines retried and built, no crash, and the roomview replaces SteamVR's environment (green: the shader reads
  the camera image as YUV, G = Y, B = Cb, R = Cr, minus 0.5019 on chroma; black outside the camera's bounds).
- Next: frames as YUV (gray -> R = B = 128), real camera frames, then the record descriptor fields the vertex shader's uniforms
  come from (`_m3` world-to-camera, `_m4` focal/centre, `_m5` fisheye coefficients with `_m7` = 1, `_m15/_m16` bounds), and
  whether the 2D mode (roomView 1) draws without the depth mesh.

## It draws: SteamVR's own camera view with the real cameras (2026-11-03, late)
What the provider (`ptprovider`) must give vrcompositor's roomview, found step by step with layer snapshots and gdb dumps of the
roomview uniforms (they live at camera state + 192, 528 bytes; written per frame at vrcompositor 0x161b00..0x161cf8):
- Frame images: RGBA8 holding Y'CbCr (R = Cr, G = Y, B = Cb, chroma centred on 0.5019), written by the CPU. The Adreno blob
  compresses optimal RGBA8 images (UBWC), so raw bytes came out as blocks of colour: ptprovider marks its images with an unused
  usage bit (input attachment, via NewSharedVulkanImage's extra usage flags) and the compat layer creates those linear (row pitch
  2560, logged as "CPU-written image").
- Block header +12/+16: image width and height (u32): the shader divides the projected pixel by them (`_m6`).
- Record descriptor: +16..+28 fx, fy, cx, cy in pixels; +32..+44 Kannala-Brandt k1..k4 (all zero = pinhole); +224 spline flag
  (0: fisheye only); +204..+216 the valid image area in texture coordinates (u0, v0, u1, v1; zero = everything black); +48 the
  camera pose as a 3x4 matrix with OpenCV camera axes, inverted by vrcompositor into `_m3`. Meta's DeviceFromCamera
  (camera_calibration_v2.json, Fisheye62: the first four radial coefficients are used) is in the OpenVR head frame (x right,
  y up, -z forward) and is what works.
- `camera.roomView = 1` (2D) draws the full view correctly with no depth mesh. The 3D mode (2): see the next section.
- Switching it on: props driver mode, ptprovider, a client holding AcquireVideoStreamingService, `set_camera_roomview_enabled`
  on the system layer mailbox. Test scripts: `src/camtap/roomview-test/` (pt-live.sh: ROOMVIEW=1|2, PATTERN=0 real cameras,
  KEEP=1 leaves it on; pt-undo.sh restores the default settings).

## The 3D roomview works (2026-11-03, night)
SteamVR's dashboard offers "Room View: On" = the 3D mode for this HMD (it declares SupportsInHMDRoomSetup like the Frame; the
options are [Off, On = 3D], or [Off, 2D, 3D] / [Off, On = 2D] on other HMDs), and its eye button (dashboard right section) shows
while `camera.enableCamera` and `camera.roomView` > 0. So the 3D mode is the one to make work. What it needed:
- **Mesh publish race**: the compositor drops a data block whose layoutId differs from the layout it has read so far, and it may read
  the data queue first: the driver publishes both blocks every second (`CompositorGate::Run`).
- **16-bit indices**: vrcompositor 0x15e300 allocates and uploads `indexCount` 32-bit words (swapping the 2nd and 3rd word of each
  triple, its winding flip), but the draw binds them as 16-bit indices (`indexCount` of them). With u32 indices every other index read
  as 0: triangle fans to vertex 0 (vertical streaks, half the view black, a plain quad drew nothing). The driver packs the indices in
  pairs and swaps the words of each triple beforehand, so at most 65536 vertices.
- **Culling**: triangles must face the head; the other winding is culled.
- **Mesh frame**: the vertices go to the GPU as they are; in the vertex shader `p = _m0 * v` with `_m0` identity (rawFromMeshPose
  identity, posePredictionBaseTime 0), `gl_Position = _m2 * _m1 * p` (`_m1` head -> eye), and the camera projection
  `c = _m3 * p`, `_m3` = the record's +48 inverted, times a matrix from the head pose (identity while the head is still: the
  6DoF correction between capture and display). So for now the mesh is in head space: a 3 m sphere around the head.
- **One camera per eye**: vrcompositor 0x160a80 picks, per eye, the camera whose pose is nearest to the eye. It reads the poses from
  the records' **+96** (mapped with marked matrices: +48 of record 0/1 -> its inverses at camera state +0x410/+0x450, +96 -> +0x83c/
  +0x86c) and the eye from `head -> eye`, so +96 = DeviceFromCamera in the head frame. It also needs params byte 1 (`state[3]`,
  snapshot +829): otherwise both eyes take eye 0's camera. ptprovider now sets both; gdb shows camera 0 for the left eye, 1 for the
  right. +144 is presumably the head pose at capture (ptprovider gives it the raw head pose).
- **Above the cameras' view**: the fragment shader draws black where u is outside the record's bounds (+204..+216) and clamps
  the uv to them; with bounds of +-100 the sampler's clamp to edge stretches the image's edge instead, as Meta's passthrough
  does (Meta's comes from its depth mesh extrapolating past the stereo area).
- **Brightness**: the shader decodes Y' as full-range BT.601 then sRGB; the OV7251 values are linear, so ptprovider encodes them
  with the sRGB curve (they were shown about as L^2.2: too dark). The `_m9` tint 0.7 is vrcompositor's own fade (0x121604:
  0.7 under a condition, likely while the dashboard is open, as on the Frame), not a setting.
- Test scripts on the headset: `pt-live.sh` (KEEP=1 PATTERN=0 ROOMVIEW=2), `pt-ubo.sh <n>` (gdb dump of the uniforms and the
  3D flags +835/+836/+864, index count +772, camera of the eye), `pt-state.py` (prints/sets the shared state's bytes).

### How Meta's OS does it (MrServiceMonterey / libmrservice.so, libmrengine.so, mrsystemservice)
- The bottom camera pair (0 and 2), one per eye; a stereo depth mesh computed per frame (libmrengine "PassthroughMesh",
  `position = vec3(bearing.xy, sqrt(1 - |b|^2)) / invDepth`), drawn with `viewProj * model(capture pose)`: 6DoF reprojection from
  the capture pose. Texture coordinates per vertex through a precomputed distortion-delta texture.
- Where the stereo is not valid (top middle) the mesh is extrapolated (favor_foreground, temporal fg/bg filtering): the stretch.
- Brightness: frames arrive gamma-encoded and the shader undoes it (pow 1.4 after the grayscale gamma): about linear light out, no
  extra brightness or contrast stage. Exposure is the tracking's (shared histogram auto exposure), no separate passthrough exposure.
- Next for us: a depth mesh (at least a floor plane + a far sphere in tracking space, then stereo depth like Meta), so near objects
  line up; ptprovider as a service; the roomview following the dashboard switch instead of being forced on.
