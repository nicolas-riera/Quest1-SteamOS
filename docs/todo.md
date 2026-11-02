# To do

Open items, roughly by priority. Done items move to the commit log.

## Performance
- vrcompositor, head still, panels on (2026-11-02): 22-28 fps in the morning, 36 with Monado's "early"
  ack, 45 with the layer's present waits returning at once. The limit is GPU time: the kgsl trace
  (adreno_cmdbatch_retired start/retire ticks) shows vrcompositor's one batch per frame running
  ~17.8 ms (gpu_busy_percentage under-reports, ~40 %). The layer's profiler (`touch
  /tmp/quest1-gputime`) splits it per eye: 12 scene passes at 1500x1664 (~5.6 ms, the controller
  models ~1.5 ms of it) and the distortion pass at 1440x1600 (~3.2 ms). Tried without useful gain:
  merging the scene passes (same time merged), dropping the distortion draws (-3 to -4 ms only),
  GpuSpeed.gpuSpeedRenderTargetScale (the scene stays 1500x1664), live vrcmd settings
  (enableSimplifiedShaders, overlay quality, skydome, supersampleScale: < 1 ms), SteamVR's realtime
  priority, GPU bus floors, Steam on the little cores. Left: lighter controller models, Facet, or
  ~45 fps with a clean 72 Hz re-projection. Never lower the resolution (user: it is already low). Tracking on all 8
  cores (it is pinned to the busy little cores) seemed to make the shaking worse: back to 0-3.
- SteamVR's Facet renderer (tip from a user, 2026-11-02) runs and draws correctly behind
  `/etc/quest1-facet` (off by default, `src/vklayer-steamvr/quest1_facet.c`): ~29-32 fps, eye passes
  6-7 ms each. Needs a fair A/B against the old renderer in one quiet window
  (`/root/facet-iter2.sh 0|1 <dir>`), with the dashboard shown (dropped stencil dynamic states).
  Leave it off unless it wins.

- User, 2026-11-02 (after the present-wait, UBWC warp and memory changes): "no more shaking, much
  better perf", but controllers are still not smooth (vrcompositor's frame rate), and the dashboard
  and Steam's UI lag.

## Audio
- Audio randomly turns crackly for a while, then recovers by itself (user, 2026-11-02).

## Display
- Monado's re-projection in Meta tracking mode deforms the image (user, 2026-11-01): turning the head
  left stretches the top of the view to the right, "soft". Sign is right (negating it is far worse).
  Suspects: q1trackd's angular velocity frame used for extrapolation, or a frame mismatch between
  the render pose and Monado's head pose. Live knobs in `/tmp/quest1-reproj` (`vel=0`, `delay=1`,
  `rx/ry/rz=`), per-axis delta and o-jitter in Monado's `direct_tick` log.
- Lens distortion and chromatic aberration are better but still off at the edges (user,
  2026-11-01). `q1grid` findings to port into driver_quest1 `ComputeDistortion` and Monado's warp.
- SteamVR popups (volume bar, IPD) show up too low in the view; the IPD popup has very strong
  chromatic aberration. Probably the same vertical error.
- Apply the per-panel calibration from `/persist/calibration/display` (decenter, roll, mura).
- 90 Hz mode, selectable from SteamVR's refresh rate setting (low priority). Base:
  github.com/TheCez/quest1-90hz-orangefox.

## Passthrough
- SteamVR's own camera view (roomview), fed by `ptprovider` (docs/passthrough.md): it starts (driver
  "props" mode + camera.roomView toggle) and takes our frames, but draws nothing until the driver
  publishes a depth mesh on the roomview3d block queues (key/value write API of vrserver's block
  queue manager to read). Then camera frames from the camtap ring instead of the test pattern.
  The head-locked overlay (`q1-camoverlay`) is shelved: the user wants SteamVR's integration.

## Boundary
- Test in the headset (after passthrough, needed to draw the boundary): the playspace buttons
  (fixed with Prop 11004), and relocalization after a reboot in the persisted map (`q1anchord`,
  docs/boundary.md), then Clear playspace.
- Pairing Quest controllers from the headset (Steam's UI). Pairing goes through SyncBoss
  (`syncboss_input_pair`): never call it without the user's explicit go.
- Controller connection state (user, 2026-11-02, low priority): a controller that loses power (battery
  removed) stays shown in SteamVR instead of turning disconnected; and right after the tracking
  service starts, controllers it has not seen yet show up inside the headset instead of staying
  "searching" until their first tracked pose.
- Controller battery levels no longer show in Steam's battery panel (user, 2026-11-02); they did before.
  Cause: in Meta tracking mode the level comes from q1trackd's shared memory, and q1trackd always
  writes -1 (unknown). Read it from libossdk's `IInputHub::enumerateAll` element (0xB8 bytes; the
  battery field's offset is still unknown, docs/tracking-ipc.md §3.3) and publish it there.

## Power
- Power button and sleep work (2026-11-02: short press = Steam's sleep, panels off and Meta's
  tracking asleep with its cameras; 1 s press = Steam's power menu). A few details are left (user):
  to list at the next test. Sleep is still a standby, not a kernel suspend.
- Restart and Shut down from Steam: retest with the PR #1 kernel (build-6 and later). If they work,
  drop the sshd `pam_systemd` workaround in `device/holo-perf.sh`.

## Steam and desktop
- The Steam Frame's desktop mode (KDE Plasma) from the "+" menu. It works in the Quest 2 port
  (github.com/URFAVF3MBOY/quest2-steamos), whose author allows us to use its code: base it on that.

## System
- SteamOS and SteamVR updates must keep working without undoing our changes (after the
  system and performance items above).
- Wi-Fi from Steam's menu and the Steam Frame's first-run setup: untested.
- Steam's VR keyboard: untested (SteamVR notifications already work).

## Streaming
- Steam Link streaming from a PC fails (2026-11-01): VR streaming leaves the PC's SteamVR waiting
  for the connection and the stream app on the headset closes; a 2D game started in cinema mode,
  its audio stuttered, then the headset's SteamVR crashed. Logs saved on the headset in
  `/root/stream-crash-20261101/`.
- Again 2026-11-02 20:29 (user): a SteamVR-tree process crashed (`/tmp/dumps/crash_20261102202929_2.dmp`,
  libllvm-qgl mapped), Steam shut down and restarted, vrserver quit with it and nothing restarts
  it, so the headset stays black. The stream's `streaming_client` (GL) can't share images with the
  compositor: libhybris GL lacks GL_EXT_memory_object(_fd) and GL_EXT_semaphore(_fd) (ASSERTs in
  sharedresource_linux.cpp), so the stream shows black. Logs in `/root/stream-crash-20261102/`.
  SteamVR now restarts by itself when it quits.

## Hardware
- Power LED: it must get its stock behaviour back (it stays solid yellow/orange). The front LED
  already lights while the cameras run. Later (low priority): let the user control it, e.g. from
  the quick menu; it turns off when the headset sleeps, except while charging.
- Bluetooth.
- USB OTG (low priority): keyboards, storage and other USB devices on the headset's port.

## Later
- Hand tracking.
- x86 games (FEX, Proton).
- Other controllers (low priority, together):
  - Windows Mixed Reality controllers (for fun);
  - Quest 2 controllers, based on github.com/Juspertinry/q2ctrl-module (only a left one to test;
    assume the right one works the same);
  - Quest 3 controllers, based on github.com/mlsplays/Touch3-on-Quest-2;
  - Quest Pro controllers: from scratch, maybe impossible.
- Windows installer that downloads SteamOS from Valve, reads the tracking software from the
  user's own headset, builds the image and flashes it (slot B), once everything works. Much later:
  dual boot with a boot selector shown in the headset at start-up (very low priority).
