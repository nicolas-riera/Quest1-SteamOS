# To do

Only what is left to do, roughly by priority. Done work leaves this list: it is described in the other docs
(`docs/*.md`) and in the commit log.

- Performance: confirm worn, controllers moving, dashboard and Steam in use. On the table since 2026-11-08 (vrcompositor's CPU
  and GPU overlap, docs/performance.md): 72 fps with the dashboard (with or without passthrough), 68-70 while it is
  used. GPU-bound now (~12.4 of 13.9 ms a frame). Levers left, all visible, so the user decides: achromatic distortion
  (vrcompositor's distortion pass is ~2 ms per eye), a lighter environment than Aurora (~4 ms; passthrough itself is
  cheaper), simpler controller models (~2 ms), the 4x MSAA of the dashboard layer. Also: the LSE emulation kernel
  (`build/perf/boot-native-holo-lse.img`, built 2026-11-08) to boot and measure; Meta's tracking threads starving
  when worn (Android pins syncboss to CPUs 0-3); memory (~500 MB available after boot; kswapd and loop0 busy while
  Steam is used).
- Passthrough: confirm in the headset the composite of the bottom and top cameras (2026-11-08: the upper half was the
  bottom camera's stretched edge). Then depth, so near things (the controllers, the hands) sit where they are: our own
  stereo (`stereo_depth.cpp`, off by default) is not reliable yet, see docs/passthrough.md "Depth from our own stereo".
  Meta's engine refuses our client (not pursued).
- Re-projection: confirm worn that nothing trembles with the head still and nothing stretches as it turns (every
  frame warped, each row at its own scan-out time, 2026-11-08, docs/direct-scanout.md).
- Brightness: confirm in the headset that both eyes match at low brightness (display-engine dimming, panels'
  calibration, 2026-11-08).
- Audio: confirm by ear that the crackle and the click at each Steam start are gone (the output now never closes,
  2026-11-08; Steam's audio thread real-time and the ADSP's receive thread at FIFO 87, 2026-11-08 night, docs/audio.md).
  If not: crackles at random, for a while, sometimes for good (user, 2026-11-02 to 05). PipeWire sees no xrun:
  below it (ADSP, codec) or memory pressure. Capture: `q1-audioring` (docs/audio.md). Also a click at each
  Steam start.
- After each put-on, ~10 s where every commit blocks ~10.7 ms in `MSMFB_ATOMIC_COMMIT` (a frame late each). Reproduced
  without wearing it (2026-11-08): `/run/quest1-standby` for 6 s, then panels on: 58 then 3 late commits in two 2 s windows.

## Tracking

- Controller connecting while Meta's stack starts (docs/tracking-6dof.md): confirm at boots (user), and with a
  controller woken up late (battery out and in while the stack starts).
- The room (playspace) came back a bit off at the first put-on after a boot, then right (user, 2026-11-09). The engine
  relocalized in the saved map 20 s after boot with the headset on the table, then logged "All submaps have bad map
  tracking" for ~50 s until it relocalized again worn. The headset's rest position in the map frame matched the day
  before within 1 cm (a yaw error would not show there). Maybe the saved anchors cover too little of the room (user):
  see how much of the room the map holds, and whether to persist the anchor again later in a session. 2026-11-09
  evening: the saved map holds 2 submaps (~4 MB, persisted on 2026-11-08 from one spot); the engine relocalizes in it,
  logs "All submaps have bad map tracking" seconds later, evicts the context and starts over, again and again, and
  while it tracks two contexts it skips map building, so the saved map never grows. Next (user: "as on Android, no
  constant delocalization"): clear the map with the user and walk the room before setting the playspace again; persist
  only once the room is covered, then again while tracking is good, so the saved map grows. 2026-11-10 00:53-00:54
  (user: "the tracking sneezed and I flew off", the headset pushed hard, desktop mode + a stream + passthrough, all 8
  cores busy and kswapd at 30 %): "Bad map tracking" on submap 0, "big search" relocalizations, both controllers
  "Lost position tracking, waiting for 6DoF reset". See whether a starved trackingservice makes it worse.
- Meta's tracking runs from boot, worn or not (trackingservice IOTcam ~50 % of a core, CtrllrCam ~11 %,
  2026-11-08). Do as Meta's OS (user, 2026-11-09): after power-on, tracking stays off until the headset is worn the
  first time; from then on it stays on, even when the headset is taken off. It stops only when the headset goes to
  sleep (standby), and after waking it starts again only once the headset is worn. Check that the map and the
  controllers come back fine after each start.

- Tracking loss: confirm with a real loss that the headset keeps its last position and that the
  controllers come back (user).
- The kernel logs "SPI transaction rejected by SyncBoss" / "Bad magic number 0xcacacaca" several times a
  minute: compare with Android, check whether IMU or controller data is lost.
- Frame rate drops while the controllers move fast (Meta's controller tracking threads work harder).

## Display

- 90 Hz mode, selectable from the refresh rate setting (low priority). Base: github.com/TheCez/quest1-90hz-orangefox.
- Per-panel lens decenter and roll from the calibration (`*_screen_offset.json`), once Meta's convention is known.
- Scene mode (`/etc/quest1-warp-lens` 2, docs/performance.md: GPU per frame ~9.7 -> ~6 ms, 1-sample on the 4x MSAA
  system layer since 2026-11-08): check it in the headset (sharpness, geometry, chromatic aberration, Steam's loading
  screen and the lock screen) before making it the default (user).
- The 4x MSAA system layer costs ~25 ms a frame in mode 0 (~37 fps): Steam's loading screen at each start. Force it to
  1 sample there too (the user allows no MSAA), or see why Steam's VR UI stayed "not ready" after many SteamVR
  restarts (2026-11-08: "SteamLoadingThrobber timed out after 120 seconds"; a Steam restart fixed it).
- Frames vrcompositor itself does not render (69-71 fps worn) repeat their content (docs/performance.md
  "Trembling: how to watch for it").
- Activating passthrough stalls vrcompositor ~0.3 s (user: "lags half a second", 2026-11-09 16:44:02,
  CTrackedCamera "Late initialization"): find what it builds then (pipelines? camera textures?) and do it at start.
- Steam's VR UI: confirm worn that Steam answers at once, that the view no longer trembles while it is used and that
  the headset no longer freezes (docs/performance.md).
- Steam's VR UI frame rate: ~10 fps while a page animates, any damage size. The GPU is the limit now (100 % busy):
  ~24 ms of GPU per Steam frame in the gaps vrcompositor leaves, for the full composite of the 1920x1080 page, its
  capture and readback. Find what costs that much (backdrop-filter blurs? the capture copy? ANGLE on this blob), or
  avoid the readback: frames from the GPU process to vrcompositor without the CPU (CEF OSR has no shared texture
  path in Steam on Linux: only CChromePainterSharedBuffer). A lower device scale is visible (user's call). Not
  RenderMode=Direct for Steam: it froze the headset (2026-11-08). Must leave room for streaming later (user).
- Steam's vrclient transfer ring: 256 MB, backed on first touch (kgsl-lazy), but Steam's webhelper cycles through it
  ("Ring Buffer Wrapped!") so all of it ends up pinned for a few 8 MB frames in flight. Drop the pages far behind the
  write position (kernel/kgsl-lazy.patch: unmap them and point the GPU back at the scratch page): ~200 MB.
- Steam's VR UI: check the right controller's Oculus button logo in the headset (user).
- Remote Play from the headset: smoother, "not perfect yet" (user, 2026-11-10; docs/streaming.md "The capture").
  Confirm in the headset: the VR view at 36 a second (Monado copies every second vsync, deployed at the next Monado
  restart), the dark blue screen with the headset off, no tearing, the colours. Then, fewer layers and more hardware
  (user): NV12 made on the GPU by Monado (a shader pass, 1.4 MB read back instead of 3.7 MB, no CPU conversion in
  q1screencast), and encode at 13-17 ms a frame in the stream (venus at its top clock 355 MHz; 8-9 ms on a quiet
  headset: the DDR bandwidth vote, `venus_bus_ddr`, or a firmware low-latency mode).
- Steam's VR overlay copy runs on llvmpipe (user, 2026-11-10: the CPU at ~100 % when the pointer moves over a Steam
  page): the pages render on the Adreno (webhelper GPU process, ANGLE Vulkan), but Steam's browser process copies
  each frame for the VR overlay with GLX on Mesa llvmpipe (`webhelper_vr.txt` "Loaded GL 4.5", docs/steam-gpu.md
  "Known issues"). Move that copy to the GPU.
- Confirm in the headset (user): System Monitor's GPU tile (src/ksysgpu). It showed 0 % while ksystemstats sent
  the right value as int64; it sends doubles now (2026-11-10).
- Remote Play from the headset: a gamepad plugged into the headset during a stream still gets its virtual gamepad
  and the PC's do not (src/rpspectator, local gamepads vs held uinput devices): confirm with real gamepads (user).
- SteamVR's global render resolution slider: remove it as on the Steam Frame, later (user, 2026-11-09), without the
  side effects found so far (docs/frame-mode.md).

## Passthrough

- Confirm worn that the camera image no longer shakes or tilts.
- Passthrough sometimes goes black and comes back with its fade-in, as if turned off and on at once (user,
  2026-11-10, with desktop mode and a stream open: the CPU was saturated). q1-ptprovider logged no disable and
  vrcompositor no roomview restart then: catch it with `PT_DUMP=1` (record states once a second) while it happens.
- q1-ptprovider composites the cameras on the CPU (~25 % of a core while passthrough shows, a third of it the
  memcpy of each composite into the shared image): do it on the GPU.
- The cameras as a real SteamVR camera (settings in SteamVR, used by the boundary drawing too), as on the Frame.
- Tint slider: check that it changes the colour.
- Camera power-up race at some boots (trackingservice "Timeout: (power: SLEEP) => (power: STANDBY)", 3DoF
  only), and udev helpers hanging ~3 min at the same boots (2026-11-08: `libinput-device-group` on event0 and
  `scsi_id` on sdd/sde, killed after 2 min 59 s): q1-hangwatch starts at sysinit now and logs D-state threads with
  their kernel stacks to the kernel log (pstore); read it at the next such boot.

## Boundary and room setup

- Leaving the room setup, Steam showed a "something went wrong" popup once.
- `ComputeInverseDistortion` returns identity while direct scanout distorts.
- Test Recenter and Clear playspace in the headset.
- Pairing Quest controllers from Steam's UI (`syncboss_input_pair`: never without the user's explicit go).

## Steam settings and Steam

- Timezone dropdown hidden (Steam shows it only under gamescope: `IN_GAMESCOPE` comes from steamui.so itself).
- Steam's first-run setup of the Frame: untested.
- The "+" menu's Desktop entry: confirm one click is enough with the laser (user).
- Desktop mode: lag and dashboard freezes while it is used (memory, llvmpipe; docs/desktop.md); the control bar's keyboard and
  close; mouse, scroll and keyboard in the headset; the Vapor look on a fresh user; install an app from Discover.
- Snapshots show q1desktop's overlay turned by 180 degrees while it is right in the headset: find why.

## Power

- The sleep animation shows inside the dashboard panel instead of full screen.
- Real sleep (suspend to RAM, `/etc/quest1-sleep` deep, opt-in): on resume Meta's tracking is lost, no sound,
  the proximity sensor wakes the headset while worn, the fan and power LED stay on.
- Restart from Steam: retest; if fine, drop the sshd `pam_systemd` workaround in `device/holo-perf.sh`.

## System

- SteamOS and SteamVR updates must keep working without undoing our changes.
- Slim the install: drop what this headset never uses (other controllers' render models, etc.).

## Streaming (after everything above, user 2026-11-08)

- Hardware video decoding for the whole system (user, 2026-11-09: "décodage vidéo matériel system wide, dont
  Chromium"). The venus decoder now answers FFmpeg's `*_v4l2m2m` decoders (`q1-hwdec`, docs/streaming.md "The
  decoder"), bit-exact, and VLC's launcher picks it for the files venus decodes. Left: the other programs.
  GStreamer's v4l2 decoders (they fall back by
  themselves, but set CAPTURE up after SOURCE_CHANGE: the library must start the firmware session itself first);
  Chromium decodes with VA-API only (Arch's build has no V4L2 decoder): a VA-API driver on top, or another build.
  The user's target (2026-11-10): the stream at 72 fps next to the dashboard and overlays, so everything on the
  hardware path and no CPU copy of the frames.

- Steam Link from a PC (docs/streaming.md, section 1): the picture shows, decoded by venus (user, 2026-11-11).
  Next: (1) performance: "ok" alone, falls apart as soon as anything else runs on the headset (user); the frames
  go through the CPU: into ANGLE with no copy (dma-buf import turned into AHardwareBuffer imports in the compat
  layer); (2) a change of the stream's size (the PC's game window minimised: 1920x1080 to 270x1080) kills the
  decoder and closes the stream; (3) the stream's sound stutters; the Vulkan/OpenGL semaphore is refused (glFinish
  fallback); (4) the user's list: 72 fps, microphone, gamepads, the PC's overlays, the headset's dashboard and not
  the PC's SteamVR one; then VR (vrlink).
- Reference for making Steam Link accept a Quest 1 (device identity, etc.):
  github.com/MisterAnderson91/quest1-steamlink-patcher (it patches Valve's Steam Link VR APK; we use it as a guide
  only, for SteamOS's native streaming, user 2026-11-09).

## Hardware

- Fan and heat (user, 2026-11-10): the fan runs but its speed does not seem to change, and the headset gets really
  hot after hours of use. Check what drives the fan here (Android's thermal engine and its fan control are not
  running on the native system?), read the temperatures under load (CPU, GPU, battery, skin), and make the fan
  follow them as Android does; check whether the kernel's thermal mitigation throttles the CPU/GPU meanwhile.
- Bluetooth: keyboard/mouse, audio (A2DP), Steam's and Plasma's Bluetooth settings, what Plasma's toggle does
  to the chip's power, a faster patch download (~11 s), the UART's power cost (in-band sleep).
- Power LED colour controlled by the user (low priority).
- USB OTG: keyboards, storage and other devices on the headset's port (low priority).

## Steam Frame features

- Passthrough toggle on a double click of a button the Touch controllers have (once passthrough is final).
- Performance overlay (fps, then temperatures and clocks).
- Frame rate limiter; GPU clock in the quick menu.

## Later

- Light overclocks within what other MSM8998 devices run stable (GPU, CPU), watching temperatures.
- Hand tracking.
- x86 games (FEX, Proton): very low priority.
- Other controllers: Windows Mixed Reality, Quest 2 (github.com/Juspertinry/q2ctrl-module). Not Quest 3 / Pro.
- The installer app (app/, installer/); later a Linux version that builds the image on the PC.
- While the headset's install system (the small Linux that builds the image on the headset) is building, the
  power LED blinks white <-> purple.
- Dual boot with a boot selector in the headset (very low priority).
