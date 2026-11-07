# To do

Open items, roughly by priority. Done items move to the commit log.

## Performance
- Target (user, 2026-11-05): 72 fps with a usable Steam (the dashboard is very slow), with headroom left
  for streaming and desktop mode. Fewer translation/emulation layers is an avenue to weigh.
- 2026-11-06 night: 72 fps with the dashboard shown, head still, passthrough on (was 53): vkCmdUpdateBuffer
  staging, UBWC scanout buffers, 4 swapchain images (docs/direct-scanout.md). Measured on a table with the panels
  forced on; to confirm worn, with the controllers in view (they cost ~5 ms of GPU a frame, see below) and
  moving the head (Monado's warp). Next: the LSE trap path (do_undefinstr takes a global spinlock per trap and
  emulate_atomic does two exclusive loops: ~18 % of vrcompositor's render thread), the passthrough mesh pass
  (~1.4 ms per eye), the controller models.
- 2026-11-07: worn it was 37-46 fps (user: "un peu mieux"): ~18 ms of GPU with the controllers in view, each of
  their parts in a render pass of its own. vrcompositor now renders straight to memory (RenderMode=Direct, SteamVR
  only): 14.1 -> 9.3 ms a frame on the table, 72 fps with the dashboard. To measure worn again. The controller
  models stay as they are (user: no visible change); the ovrstub log flood (~850 kernel log lines a second with the
  controllers on) is gone.
- Memory (user, 2026-11-05: "faut bien gérer ce problème de saturation"): right after boot, with
  SteamVR up, only ~500 MB is available and ~550 MB is already swapped (zram). Per process (Pss + swap
  + KGSL): Steam's webhelpers ~1.65 GB (two of them with 270 + 185 MB of GPU memory), vrcompositor
  ~610 MB (470 MB GPU), vrwebhelper (dashboard) ~560 MB (274 MB GPU), steam ~130 MB; KGSL total
  1.26 GB. After ~12 stack restarts in one boot the headset fell to 12-15 fps (kswapd 35 %, GPU trace
  buffer allocation failing): reboot rather than restart the stack many times. Leads: Steam's
  gamepadui window rendered in Xvfb for nobody, CEF memory flags, vrcompositor's textures,
  extra_free_kbytes so kswapd starts before allocations stall.
  2026-11-05 evening: holo.img's loop device now uses direct I/O (335 MB of double page cache gone), and
  vrclient's 256 MB transfer ring (one per OpenVR Vulkan process: vrcompositor, the dashboard, Steam's
  webhelper, q1desktop, every game) is backed on first CPU touch (kernel/kgsl-lazy.patch). Next: zram
  with zstd/lz4 instead of lzo, Steam's webhelper ring (fully used: its CPU-drawn pages), the dashboard's
  and Steam's CEF processes.
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
  Measured 2026-11-06 (CDP, arrow keys every 150 ms on the library home): requestAnimationFrame falls from 60 to
  ~9 fps (median frame 100 ms, max ~470 ms). The renderer's main thread runs ~80 % of a big core (libcef + V8:
  the gamepadui's own work), the GPU process's VizCompositorThread ~70 % (half of it memcpy: the off-screen
  frames copied for the VR overlay), the browser process uploads them with Mesa GL (libgallium). Steam itself
  pins its webhelpers to CPUs 2-6 (two little, three big cores; `STEAM_DESKTOP_APP_AFFINITY_LIST` is read but
  unset), though the hot threads ran on the big ones. Leads: fewer copies on the overlay path (shared textures
  instead of the CPU fallback), less UI work (animations, blur).
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

## Tracking
- When the headset loses tracking it jumps to the floor at (0, 0, 0) instead of holding its last pose
  (user, 2026-11-05). Expected: keep the last position (rotation from the IMU only), like Meta's OS.
- When tracking comes back the controllers are drawn at once even while the cameras haven't seen them
  yet, instead of showing as "searching" until they are found (user, 2026-11-05).
- Spatial anchors work very badly (user, 2026-11-05 night): after a tracking loss the headset has a hard
  time finding where it was (relocalizing in the persisted map; q1anchord, /run/q1track/frame-state).

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
- 2026-11-05: crackling still there, coming and going (user). Memory pressure is constant now (see
  Performance): check the link with it first.
- A short click is heard each time Steam starts, even with the startup movie silenced (user, 2026-11-05:
  "on n'est pas censé entendre ça"). Find its source: the audio device being opened (pop), or a Steam UI sound.

## Display
- Apply the per-panel calibration from `/persist/calibration/display` (decenter, roll, mura).
- 90 Hz mode, selectable from the headset refresh rate setting (the Frame's quick menu / SteamVR's
  setting) (low priority). Base: github.com/TheCez/quest1-90hz-orangefox.

- Steam's VR UI: the right controller's Oculus button used the menu button's texture (SteamVR's
  Quest render model, user 2026-11-03). Fixed 2026-11-07: src/steamvr-quest1/mkrendermodel.py copies the model
  into driver_quest1's resources with the Quest 2 controller's logo on that button (holo-steamvr.sh runs it).
  Checked through IVRRenderModels; to look at in the headset (user).

## Passthrough
- 2026-11-07 (user: view frozen, camera LEDs off): Meta's tracking stopped at 09:58:39, ~3 min after boot
  (q1trackd 0 valid samples, status 85 -> 0), right after "ProcessMaybeDead vrguardianservice/vrruntimeservice" and
  "MapDB: Deleting 3 corrupted entrees". Restarting the stack failed at first: sb-free's per-descriptor readlink
  loop outlasted the sensors HAL's start timeout (fixed: one find). `holo-android-tracking meta-on` (Monado after
  the stack, as at boot) brought it back. Same boot: udev helpers hung ~3 min and were killed (v4l_id on
  /dev/video1 = msm_jpegdma, scsi_id on sdb/sdd, libinput-device-group on the power key's event0), so event0 kept
  root 0600 and quest1-powerbutton/volkeys crash-looped (fixed by hand: udevadm trigger --action=add). Find what
  blocks them at boot and whether it is linked to the tracking loss (first boot of the devpts kernel).
  That boot the cameras never powered up either (trackingservice: "Timeout: (power: SLEEP) => (power: STANDBY)"
  every second from 09:56:20, so 3DoF only; restarting the stack did not help). The next boot, same kernel, was
  clean (6DoF, no udev stall): a boot race in the camera drivers, to catch again with its logs.
- Shake/tilt (user, 2026-11-06: only the camera image, "almost nauseating"): not the kernel, the KGSL lazy rings, the capture
  poses or the frame order (docs/passthrough.md, what it is not); next: Meta's depth engine through MrSystemServiceIPCServer
  instead of our fixed mesh (user's call).
- Regression (user, 2026-11-05 night): passthrough shakes a lot and tends to tilt when the head moves.
  Broken by that night's memory work; suspects, in order: the KGSL lazy rings (first-touch page faults in
  vrcompositor's 256 MB ring, kernel/kgsl-lazy.patch and the compat layer's hint), zram zstd (slower
  readback of swapped pages), loop0 direct I/O, the fakelogd filter (something reading Meta's logs?).
  Bisect by booting without each (an image without kgsl-lazy first).
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
- The Steam Frame's room setup works (user, 2026-11-05; the Adreno fixes are in docs/boundary.md). Left:
  - Leaving the setup, Steam showed a "something went wrong" popup (Refresh cleared it); maybe memory.
  - Frame rate in step 7: ~30 fps after a clean boot, as outside the setup (see Performance).
  - ComputeInverseDistortion returns identity while direct scanout distorts (distort_geom_* shaders place
    geometry with the inverse LUT).
  - steamvr-q1 exports QUEST1_COMPAT_SPIRV empty, and the layer checks getenv() != NULL, so the pipeline
    logging is always on (log noise).
  - Test in the headset: Recenter and Clear playspace, and relocalization after a reboot in the persisted
    map (`q1anchord`, docs/boundary.md).
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
- The "+" menu's Desktop entry took several clicks (user, 2026-11-06). Found: a click during the first seconds of a
  launch signalled `q1desktop` before it had its SIGUSR1 handler and ended the desktop; a click while the last desktop
  was ending was dropped. Both fixed (docs/desktop.md); a click on the entry (DOM events) launches it once, and four
  launches 1 s apart give one desktop. Left: confirm with the laser in the headset (user).
- 2026-11-07, still several clicks with the laser (user): a click in the menu's first ~3 s was dropped by Steam while
  its icon loaders (bubblewrap) failed one after the other; the kernel now has multi-instance devpts. To confirm in
  the headset (icons in the "+" menu, one click). "Desktop" from Plasma's own menu no longer starts a nested desktop.
- Discover showed no apps and asked for flatpak (user, 2026-11-06): polkit refused Flatpak's system actions to steamos
  (SteamOS's rule wants wheel and an active seat session), so the Flathub catalogue was never deployed ("DeployAppstream
  not allowed"). Fixed in holo-frame-mode.sh's polkit rule; Discover lists Flathub's apps (checked on the desktop's
  screen). Left: install an app from it in the headset (user).
- Desktop mode launcher (user, 2026-11-05 night: failed to start again): kcminit outlived the desktop and held
  the launcher's lock; fixed 2026-11-06 (docs/desktop.md), 9 launch/close cycles and a double launch OK from a
  shell with Steam's environment. Left: confirm from Steam's "+" menu in the headset; once, the first launch
  after killing old leftovers hung 48 s (plasma_session up, kwin silent) and closed fine.
- Desktop mode (docs/desktop.md): the nested Plasma runs and shows in the dashboard (user, 2026-11-05: Dolphin
  started, the headset struggling). Open:
  - lag, and the whole dashboard freezing while the desktop is used (memory: ~2 GB swapped with Plasma and
    an app; llvmpipe threads competing with vrcompositor and the tracking);
  - start it from Steam's "+" menu (the Frame's entry is installed) and check the overlay key the dashboard
    ties to the running app (`valve.steam.desktopgame.<appid>`), the control bar's keyboard and close;
  - check the Vapor look (wallpaper, dark theme, Steam Deck logo) on a fresh user, and that the "+" menu shows
    only Desktop and user-installed apps;
  - mouse orientation, scroll and keyboard typing in the headset;
  - snapshots (/root/snap.sh) show q1desktop's overlay turned by 180 degrees while it is right in the headset
    (user, 2026-11-05; Steam's overlays are right in both): don't trust snapshots for that overlay's
    orientation, find why they differ.
- Steam crashed while the desktop was used (user, 2026-11-05 ~18:07) and its VR UI did not come back:
  vrdashboard asserts "stalled cross-thread pipe (pipe is disconnected)" (/tmp/dumps), the headset deep in swap.
  The dashboard's vrwebhelper (started by vrdashboard) died with it and vrdashboard never restarted it. The
  next OpenVR client to start (here q1desktop) started a new one in its own cgroup, so stopping that client
  killed the dashboard again. The stack must restart vrwebhelper (or the dashboard unit) when it dies.

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
- Bluetooth lost events (user, 2026-11-05: DualSense pairing failed, discovery hung): the UART's runtime PM
  suspended it 100 ms after each transfer and bytes the chip sent later were lost (no Inquiry Complete,
  Disconnection Complete or incoming connection). Fixed 2026-11-06 (quest1-bt keeps it on, docs/bluetooth.md):
  BR/EDR inquiries complete and find several times more devices. A DualSense pairs and works since AlwaysPairable (user, 2026-11-06). Left: the
  power cost (in-band sleep would let the UART sleep).
- Bluetooth (docs/bluetooth.md): up since 2026-11-05 evening (hci0 up, LE scans find devices). Left: pairing
  a keyboard/mouse, a gamepad, audio (A2DP); Steam's and Plasma's Bluetooth settings; `rfkill block bluetooth`
  cuts the chip's power (bt_power rfkill) under the attached hci0: check what Plasma's toggle does; a faster
  patch download (~11 s).
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
- The installer (app/, installer/) is under way: the Windows app, the headset builds the image in
  its micro Linux. Much later (user, 2026-11-04): a Linux version of the app, where the PC itself
  builds the image (build_image.py runs there, no scratch disk over USB) before flashing it.
- Much later: dual boot with a boot selector shown in the headset at start-up (very low priority).
