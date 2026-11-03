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
- Light, safe overclocks (user, 2026-11-03): GPU (A540 max 710 MHz on the Quest 1) and CPU OPPs a
  step above stock, only within ranges other MSM8998 devices run stable, watching tsens and the
  fan. Kernel DT/OPP tables, so a new boot image (fastboot boot only).

- User, 2026-11-02 (after the present-wait, UBWC warp and memory changes): "no more shaking, much
  better perf", but controllers are still not smooth (vrcompositor's frame rate), and the dashboard
  and Steam's UI lag.
- The controllers cost ~10 fps (2026-11-03, headset on a table, dashboard shown, no re-projection):
  ~46 fps while SteamVR draws no controller, 34-36 fps with both drawn (vrcompositor's GPU batch
  20 -> 25-27 ms). The layer's profiler shows each visible controller as a ~0.6 ms pass (9792
  vertices, the body) plus 6 small passes (~0.1 ms each, the buttons), per eye: ~5 ms a frame.
  Monado's build, achromatic distortion and the vm settings made no difference (bench in
  /root/bench-20261103/). Next: why each render model component is its own render pass, and a
  lighter render model (could also fix the Oculus button texture below).
- Frame rate drops while the controllers move fast, until they stop (user, 2026-11-03): Meta's
  controller tracking threads (CtrllrCam, CtrlTracking, SCHED_FIFO on cores 0-3) work harder. In
  part (only in part) the laser pointer passing over Steam UI elements: their hover animations cost
  frames (user).
- Re-projection off when the headset is put on, then swinging slowly into place while fresh frames
  sit right (user, 2026-11-03): Monado's warp averaged the device-to-LOCAL rotation `o` at 5 % a
  frame and never reset it, but driver_quest1's LOCAL space is created at the head's yaw with each
  SteamVR start. Now seeded again per layer connection and when far for 8 frames (comp_window_mdp.c);
  to confirm in the headset after a SteamVR restart.
- Slim the install: drop what this headset never uses (other controllers' render models, SteamVR
  Home if present, etc.): less storage, memory and load time (user, 2026-11-03).
- Steam's UI and the dashboard lag, and so does Steam's VR keyboard (it is Steam's: it types fine,
  the lag is Steam's UI). Faster Steam fixes both (user, 2026-11-03).
- LSE atomics emulation (kernel/lse_emul.c): ~100,000 traps/s while SteamVR runs (vrserver ~38k,
  vrcompositor ~29k, steamwebhelper ~24k), 0.8 us each on a big core and 1.6 us on a little one,
  against ~20 ns native: ~10-15 % of one core. Make the emulation itself faster (trap path, hook
  lookup, decode). Not binary rewriting: patched Valve binaries break with Steam updates and
  can't be shipped (user, 2026-11-03).
- systemd-journald spends ~8 % of a core on trackingservice's log spam ("failed to extrapolate
  controller tracking state" every 2 s, CONSTELLATION status), and kswapd up to ~40 %.
- The kernel logs "SPI transaction rejected by SyncBoss" / "Bad magic number 0xcacacaca" several
  times a minute (2026-11-03): check against Android, and whether IMU or controller data is lost.
- Lens distortion in Monado's warp (`/etc/quest1-warp-lens` = 1, off by default): vrcompositor hands
  over undistorted eyes, Monado does distortion, chromatic aberration and re-projection in one pass
  like Meta's TimeWarp. Geometry right since Meta's lens centre (2026-11-03), but vrcompositor's
  distortion pass costs the same as a copy (3.6 ms per eye, bandwidth) and the warp then runs at
  every vsync with 3 samples: more GPU in total. Compare sharpness with the user once perf allows.

## Steam settings
- The timezone dropdown (Settings, next to the 24-hour clock) is hidden: Steam shows it only when
  `IN_GAMESCOPE && IS_STEAMOS` (steamui library.js, export XM). Our Steam runs under Xvfb, not gamescope. The user
  wants it (2026-11-03); meanwhile the system timezone is set with timedatectl (Europe/Paris). Find what sets
  IN_GAMESCOPE and whether it can be set without gamescope's other paths; check IS_STEAMOS too (os-release is now
  ID=steamos, device/holo-identity.sh).

## Audio
- Audio randomly turns crackly for a while, then recovers by itself, sometimes stays stuck crackly
  (user, 2026-11-02/03). PipeWire sees no xrun at all (pw-top ERR 0, logged every 2 s for 20 min):
  not the graph. Seen with memory pressure (kswapd 50 %, swappiness raised to 100). Capture:
  `q1-audioring` (runtime unit on the headset) keeps the last 90 s of the sink monitor and the ALSA
  status every 20 ms; `systemctl kill -s USR1 q1-audioring` dumps them to /root/audioring-<time>/
  when the user hears it. Clean monitor + crackle = below PipeWire (ADSP, codec).
  Capture 2026-11-03 18:07 in `/root/audioring-180736/` (crackle during and after a SteamVR crash
  loop: load 10-13, kswapd ~46 %, 0.7 GB available; `extra-state.txt` has vmstat and top). Fix later.

- 2026-11-03 22:30: the user recorded the Steam click sound crackling (phone recording, kept on the PC in
  Downloads/Mobile Devices, not in the repo): bursts of short high-frequency spikes on the clicks.

## Display
- Apply the per-panel calibration from `/persist/calibration/display` (decenter, roll, mura).
- 90 Hz mode, selectable from the headset refresh rate setting (the Frame's quick menu / SteamVR's
  setting) (low priority). Base: github.com/TheCez/quest1-90hz-orangefox.

- Steam's VR UI: the right controller's Oculus button uses the menu button's texture (SteamVR's
  Quest render model). Fix the texture someday, e.g. from another Quest controller's Oculus button,
  matching the menu button's look (user, 2026-11-03, low priority).

## Passthrough
- Goal (user, 2026-11-03): the cameras seen by SteamVR as a real camera, like on the Steam Frame:
  its settings adjustable in SteamVR, usable as normal passthrough and as the passthrough used to
  draw the boundary.
- 2026-11-03 night: SteamVR's 3D roomview works (docs/passthrough.md, "The 3D roomview works"): one
  bottom camera per eye, the image's edge stretched above the cameras' view, linear light (sRGB-encoded
  frames). Installed by `device/holo-passthrough.sh`: driver_quest1 in "props" mode
  (`/etc/quest1-camera`) and `q1-ptprovider.service` (with SteamVR; 7.5% of a core while shown,
  0.7% while hidden). Switched by vrcompositor itself: the dashboard's Room View (On = 3D) and eye
  button, the room setup (with no playspace it follows `camera.enableConstructRoomView`, set by
  `set_camera_roomview_enabled`). Test scripts in `src/camtap/roomview-test/` (`pt-undo.sh`
  restores the defaults; `pt-flow.sh`-style tests need no `trackedcam_test`).
- 2026-11-03 late, user feedback and fixes: eye button OK (Prop 2105); scale OK; shaking reduced (capture
  poses from Monado, frames without one dropped) - to recheck; left eye lower: fixed by the fisheye fit
  (to recheck); double click Menu/Oculus = toggle room view (vrcompositor bindings); Edge styles hidden and
  tint slider shown (props 2122 and 1062) - to check that the slider changes the colour.
- Later: depth from the bottom stereo pair (a per-frame depth mesh, like Meta's libmrengine) so near
  objects sit at their true depth (the fixed 3 m sphere and the 11 cm camera baseline make them wrong).

## Boundary
- Frame room setup (step 7) ran at ~3 fps: vrcompositor's "Playspace area SDF" texture (R8, linear) is 0x0
  until a boundary exists; the Adreno driver gives it 0 bytes of memory, whose allocation fails, so
  CreateTexture failed on every frame. Fixed 2026-11-03 in the compat layer (empty images made 1x1); to
  check in the headset: frame rate in step 7, floor and laser drawing, Accept.
  Also seen: its floor/boundary vertex shader samples bindings 10/26 and 11/27 that the pipeline layout
  lacks (Adreno: "Bind group info mismatches"); the layer's retry turns those reads into 0 (displacement
  offsets of the grid), created once per setup start, not per frame.
- 2026-11-04: with no chaperone_info.vrchap (e.g. after Clear playspace) vrserver had no universe, so no
  standing pose ("Cannot query GetRawZeroPoseToStandingAbsoluteTrackingPose"): no floor, no brush, Accept
  committed nothing. driver_quest1 now provides a default universe 2 (Prop_DriverProvidedChaperoneJson_String,
  standing = raw). Floor height adjust and drawing then work (user-tested).
  Left: the floor "circle" is a fan of lines converging on one floor point (photo by the user). Suspects:
  `playspace-floordetection-geo` (draw_range .4-10 m) fed by the Frame's depth/floor detection, which we do
  not provide (CTrackedCamera "Depth mesh block queues"), or the default play_area [0,0]. distort_tri_floor_gs
  (a geometry shader) samples bindings 11/27 like the passthrough reprojection VS. Accept not tried yet.
  2026-11-04 later: the user confirms huge graphical glitches: the setup geometry's lines do not behave (fan of
  rays from one floor point, varying lengths). Ruled out: the distortion LUT (the area uses geom_playspace_
  drawing_area_vs, no LUT) and the depth sphere (step 7 only adds the geom_bounds_color pipeline). Pipelines
  in use mapped to /opt/steamvr shader files by md5 of the compat layer's SPIR-V dumps. Next: with the user
  looking at the fan, remove the step 7 nodes one by one (tools/headset-dev/sg-hide.sh) and snapshot, then
  look at that node's vertex data / shader on the Adreno.
  Also wrong: ComputeInverseDistortion returns identity while direct scanout distorts (distort_geom_* shaders
  place geometry with the inverse LUT).
  LEFT ON THE HEADSET (2026-11-04, it rebooted into Android before cleanup): /etc/quest1-spirv-dump (remove
  it, logs and /tmp dumps every module) and the /root helpers.
  Lag: the compat layer's "display:" line shows vrcompositor at 23-35 fps with the room view (28-42 ms per
  frame, present-to-rendered 24-41 ms: GPU bound); Monado fills to 72 Hz by reprojection.
  Side note: steamvr-q1 exports QUEST1_COMPAT_SPIRV empty, and the layer checks getenv() != NULL, so the
  pipeline logging is always on (log noise).
- Test in the headset (after passthrough, needed to draw the boundary): the playspace buttons
  (fixed with Prop 11004), and relocalization after a reboot in the persisted map (`q1anchord`,
  docs/boundary.md), then Clear playspace.
- Pairing Quest controllers from the headset (Steam's UI). Pairing goes through SyncBoss
  (`syncboss_input_pair`): never call it without the user's explicit go.
- Controller connection state: a powered-off controller now turns disconnected within 0.3 s
  (tested 2026-11-03). Left to check at the next boot: right after the tracking service starts, a
  controller it has not seen yet should stay "searching" until its first tracked position.

## Power
- Power button and sleep work (2026-11-02: short press = Steam's sleep, panels off and Meta's
  tracking asleep with its cameras; 1 s press = Steam's power menu). Left (user, 2026-11-03):
  - the sleep animation shows inside the dashboard panel instead of full screen, and it is
    SteamOS's (deck-suspend-animation), the Frame's one if it has its own;
  - real sleep: suspend to RAM works (`/etc/quest1-sleep` deep, opt-in) but on resume Meta's
    tracking was lost (head spinning), the sound gone, the proximity sensor kept waking the headset
    while worn (SyncBoss "wakeup received", irq 271), and the fan and power LED stayed on.
    syncboss_spi.c (Meta's kernel) eats PROX_ON until a PROX_OFF when suspended while worn.
- Steam's own startup movie (each Steam start, also after a SteamVR restart) is SteamOS's: Steam
  picks it from its hardware type (5001 = Frame, read from /proc/device-tree: the Frame's DT says
  qcom,sm8650-deckard), not from -steamframe. The boot animation (q1splash) plays the Frame's.
- Restart from Steam: retest. If it works, drop the sshd `pam_systemd` workaround in
  `device/holo-perf.sh`.

## Steam and desktop
- Next after passthrough and the boundary (user, 2026-11-03): the Steam Frame's desktop mode (the
  Frame's KDE Plasma) from the "+" menu, also to load the headset hard and see what really needs
  optimizing. It works in the Quest 2 port (github.com/URFAVF3MBOY/quest2-steamos), whose author
  allows us to use its code: base it on how that port does it.

## System
- SteamOS and SteamVR updates must keep working without undoing our changes (after the
  system and performance items above).
- The Steam Frame's first-run setup: untested.

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
- Power LED: Meta's battery colours since 2026-11-03 (`device/holo-battery.sh`). Later (low
  priority): let the user control it, e.g. from the quick menu.
- Bluetooth: wanted (user, 2026-11-03), likely not hard, and easy to test (plenty of devices
  around the user).
- USB OTG (low priority): keyboards, storage and other USB devices on the headset's port.

## Steam Frame features (from a video walkthrough, user 2026-11-03)
- Passthrough toggle by double-clicking the Steam button: the Touch controllers have no Steam
  button, so probably a binding to add in the VR dashboard's bindings (once passthrough works).
- Performance overlay (4 levels: fps, then temperatures and clocks): missing here (user confirmed).
- Frame rate limiter; GPU clock in the quick menu. (Refresh rate: with the 90 Hz mode. Window
  curvature and wrist-docked windows are SteamVR's and already work.)

## Later
- Hand tracking.
- x86 games (FEX, Proton): very low priority, no chance with today's performance and memory.
- Other controllers (low priority, together):
  - Windows Mixed Reality controllers (for fun);
  - Quest 2 controllers, based on github.com/Juspertinry/q2ctrl-module (only a left one to test;
    assume the right one works the same).
  - Not Quest 3 / Pro controllers: they pair with ECDH, which the Quest 1's MCU firmware lacks;
    replacing that firmware risks a brick (Touch3-on-Quest-2's developer, 2026-11-03).
- Windows installer that downloads SteamOS from Valve, reads the tracking software from the
  user's own headset, builds the image and flashes it (slot B), once everything works. Much later:
  dual boot with a boot selector shown in the headset at start-up (very low priority).
