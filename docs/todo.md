# To do

Only what is left to do, roughly by priority. Done work leaves this list: it is described in the other docs
(`docs/*.md`) and in the commit log.

- Performance (docs/performance.md): GPU-bound, ~12.4 of 13.9 ms a frame. Levers left, all visible, so the user
  decides: achromatic distortion (vrcompositor's distortion pass is ~2 ms per eye), a lighter environment than Aurora
  (~4 ms; passthrough itself is cheaper), simpler controller models (~2 ms), the 4x MSAA of the dashboard layer.
  Also: the LSE emulation kernel (`build/perf/boot-native-holo-lse.img`, built 2026-11-08) to boot and measure;
  Meta's tracking threads starving when worn (Android pins syncboss to CPUs 0-3); memory (~500 MB available after
  boot; kswapd and loop0 busy while Steam is used).
- Passthrough depth, so near things (the controllers, the hands) sit where they are: our own stereo
  (`stereo_depth.cpp`, off by default) is not reliable yet, see docs/passthrough.md "Depth from our own stereo".
  Meta's engine refuses our client (not pursued).
- Audio: a click at each Steam start (user, 2026-11-12: still there; the crackle is gone). The output no longer
  closes between sounds (docs/audio.md), so find what Steam does at its start: a sound of its own, or a stream it
  opens and closes.
## Tracking

- The room comes back a bit off at the first put-on after a boot: the engine relocalizes in the saved map, logs "All
  submaps have bad map tracking" seconds later, evicts the context and starts over, so the saved map (2 submaps, from
  one spot) never grows (docs/boundary.md "The room after a boot"). Next (user: "as on Android, no constant
  delocalization"): clear the map with the user and walk the room before setting the playspace again; persist only
  once the room is covered, then again while tracking is good. See also whether a starved trackingservice makes it
  worse (2026-11-10: "the tracking sneezed and I flew off" with all 8 cores busy).
- Meta's tracking runs from boot, worn or not (trackingservice IOTcam ~50 % of a core, CtrllrCam ~11 %,
  2026-11-08). Do as Meta's OS (user, 2026-11-09): after power-on, tracking stays off until the headset is worn the
  first time; from then on it stays on, even when the headset is taken off. It stops only when the headset goes to
  sleep (standby), and after waking it starts again only once the headset is worn. Check that the map and the
  controllers come back fine after each start.
- The kernel logs "SPI transaction rejected by SyncBoss" / "Bad magic number 0xcacacaca" several times a
  minute: compare with Android, check whether IMU or controller data is lost.
- Controllers, more tests (user, 2026-11-12): one woken up late (battery out and in while Meta's stack starts) must
  still connect (docs/tracking-6dof.md); after a real tracking loss the headset keeps its last position and the
  controllers come back.
- Sometimes, as the headset is put on, the floor's direction seems uncertain and is seen to adjust (user,
  2026-11-12; not remembered from Meta's OS). To investigate: gravity/orientation at the first poses after wake,
  and whether the room's floor comes from the relocalization above.

## Display

- 90 Hz mode, selectable from the refresh rate setting (low priority). Base: github.com/TheCez/quest1-90hz-orangefox.
- Per-panel lens decenter and roll from the calibration (`*_screen_offset.json`), once Meta's convention is known.
- Scene mode (`/etc/quest1-warp-lens` 2, docs/performance.md) as the default in the image: the user checked it in
  the headset (2026-11-12). It also takes away the 4x MSAA system layer's ~25 ms a frame of mode 0 (Steam's loading
  screen at ~37 fps).
- Activating passthrough freezes the view (the only freeze left in Steam's VR UI, user 2026-11-12): vrcompositor
  stalls ~0.3 s (2026-11-09 16:44:02, CTrackedCamera "Late initialization"): find what it builds then (pipelines?
  camera textures?) and do it at start.
- Steam's VR UI frame rate (user, 2026-11-12: SteamVR itself is very fluid, Steam is "usable" but needs more
  performance): ~10 fps while a page animates, any damage size. The GPU is the limit now (100 % busy):
  ~24 ms of GPU per Steam frame in the gaps vrcompositor leaves, for the full composite of the 1920x1080 page, its
  capture and readback. Find what costs that much (backdrop-filter blurs? the capture copy? ANGLE on this blob), or
  avoid the readback: frames from the GPU process to vrcompositor without the CPU (CEF OSR has no shared texture
  path in Steam on Linux: only CChromePainterSharedBuffer). A lower device scale is visible (user's call). Not
  RenderMode=Direct for Steam: it froze the headset (2026-11-08). Must leave room for streaming later (user).
- Steam's vrclient transfer ring: 256 MB, backed on first touch (kgsl-lazy), but Steam's webhelper cycles through it
  ("Ring Buffer Wrapped!") so all of it ends up pinned for a few 8 MB frames in flight. Drop the pages far behind the
  write position (kernel/kgsl-lazy.patch: unmap them and point the GPU back at the scratch page): ~200 MB.
- Steam's VR overlay copy runs on llvmpipe (user, 2026-11-10: the CPU at ~100 % when the pointer moves over a Steam
  page): the pages render on the Adreno (webhelper GPU process, ANGLE Vulkan), but Steam's browser process copies
  each frame for the VR overlay with GLX on Mesa llvmpipe (`webhelper_vr.txt` "Loaded GL 4.5", docs/steam-gpu.md
  "Known issues"). Move that copy to the GPU.
- SteamVR's global render resolution slider: remove it as on the Steam Frame, later (user, 2026-11-09), without the
  side effects found so far (docs/frame-mode.md).

## Passthrough

- Passthrough sometimes goes black and comes back with its fade-in, as if turned off and on at once (user,
  2026-11-10, with desktop mode and a stream open: the CPU was saturated). q1-ptprovider logged no disable and
  vrcompositor no roomview restart then: catch it with `PT_DUMP=1` (record states once a second) while it happens.
- q1-ptprovider composites the cameras on the CPU (~25 % of a core while passthrough shows, a third of it the
  memcpy of each composite into the shared image): do it on the GPU.
- The camera test in SteamVR's camera settings (passthrough) does not work (user, 2026-11-11).
- Camera power-up race at some boots (trackingservice "Timeout: (power: SLEEP) => (power: STANDBY)", 3DoF
  only), and udev helpers hanging ~3 min at the same boots (2026-11-08: `libinput-device-group` on event0 and
  `scsi_id` on sdd/sde, killed after 2 min 59 s): q1-hangwatch starts at sysinit now and logs D-state threads with
  their kernel stacks to the kernel log (pstore); read it at the next such boot.

## Boundary and room setup

- `ComputeInverseDistortion` returns identity while direct scanout distorts.
- Pairing Quest controllers from Steam's UI (`syncboss_input_pair`: never without the user's explicit go).

## Steam settings and Steam

- Timezone dropdown hidden (Steam shows it only under gamescope: `IN_GAMESCOPE` comes from steamui.so itself).
- Steam's first-run setup of the Frame: untested.
- SteamVR restarted alone (Steam kept running) breaks Steam's VR UI: steamwebhelper's GPU process takes GPU page faults when
  vrcompositor stops (2026-11-11 17:50 and 17:51, two restarts) and the dashboard then shows its lower half empty and loses
  its bottom bar (restarting Steam brought it back, user). SteamVR alone should restart cleanly.
- A setting in the VR settings' Oculus Quest category, on by default: ignore the Oculus and Menu buttons while the
  controller moves (they are easy to press by mistake on Touch controllers; Meta's OS and the Rift S, same controllers,
  do the same).

## Power

- The sleep animation shows inside the dashboard panel instead of full screen.
- Real sleep (suspend to RAM, `/etc/quest1-sleep` deep, opt-in): on resume Meta's tracking is lost, no sound,
  the proximity sensor wakes the headset while worn, the fan and power LED stay on.
- Restart from Steam: with the OS started by `fastboot boot` it restarts into Android; test it once the OS is
  flashed (user), then drop the sshd `pam_systemd` workaround in `device/holo-perf.sh` if it works.

## System

- SteamOS and SteamVR updates must keep working without undoing our changes.
- Slim the install: drop what this headset never uses (other controllers' render models, etc.).
- The telemetry HAL's loop (docs/performance.md, 2026-11-11 heat): once an event cannot go to ovrstub's
  TelemetryService it retries with no pause (a core at 100 %, ~50000 log lines/s, fakelogd at 90 %). Stopping the HAL
  froze the tracking: make the stand-in accept the events, or find what it fails on.

## Streaming

- Steam Link VR (docs/streaming.md, section 2): a new picture each frame and a sharp centre now (user, 2026-11-12:
  "c'est net"), but it trembles, flashes black now and then and runs below 72 fps.
  - Trembling: the stream only (the headset's own chaperone and dashboard are steady). Not Monado's re-projection:
    with it off everything trembled more (2026-11-12). Lead: the PC predicts each frame's display ~92 ms after it
    renders it (`vrlink_aftm_*.csv`: send 8 ms, network 5, decode 15, then the frame waits ~62 ms in vrlink for that
    predicted time), so its poses are predicted ~90 ms ahead. Find what makes the PC predict that far (what vrlink
    reports: `query_to_display`/`decode_to_display`, our frame timing, the "DecodeSlow" and "Mispredicted" flags).
  - Black flashes (user, 2026-11-12): vrlink logs no decoder reset outside the session's start; not found yet.
  - Frame rate: the layer's inset pass costs ~2.5 ms of GPU (58-60 fps against ~69 with the dashboard open).
    vrcompositor has the Frame's own way, free: `distort_ps_achromatic_se2` blends the inset in its distortion pass,
    but here it draws with `distort_ps_layered` (chromatic, overlays in the pass), with `enableAchromaticDistortion`
    true in steamvr.vrsettings. Find what picks the shader (its settings block reads achromatic off at run time).
  - Start of a session: the headset stalls for seconds (Monado's re-projection too), the system swaps ~150-250 MB
    as vrlink and vrcompositor allocate (it was ~700 MB before the copies shared their memory). Lock Monado's (and
    the tracking's) memory, and find what else allocates then.
  Also: vrcompositor leaks a sync_fence fd for each vrlink frame (~70 a second, limit 524288: ~2 h of session; they
  do not come through vkImportSemaphoreFdKHR). The PC's overlays show only when the PC's SteamVR started after
  vrlink; stopping a session from the headset (`StopStreamingSession`) does nothing and vrlink hangs on its way out.
- Steam Link from a PC, 2D (docs/streaming.md, section 1): it works well (user, 2026-11-12); more performance is
  welcome: the kernel emulates atomic instructions for Steam's programs (`emulate_atomic` in the profiles), the
  Vulkan/OpenGL semaphore is refused (glFinish fallback).
- Hardware video decoding for the whole system (user, 2026-11-09: "décodage vidéo matériel system wide, dont
  Chromium"). FFmpeg's `*_v4l2m2m` decoders and VLC use venus (`q1-hwdec`, docs/streaming.md "The decoder"). Left:
  GStreamer's v4l2 decoders (they fall back by themselves, but set CAPTURE up after SOURCE_CHANGE: the library must
  start the firmware session itself first); Chromium decodes with VA-API only (Arch's build has no V4L2 decoder): a
  VA-API driver on top, or another build. The user's target (2026-11-10): the stream at 72 fps next to the dashboard
  and overlays, so everything on the hardware path and no CPU copy of the frames.
- Wi-Fi power saving: off only while a lot of data goes through (a stream, a download), back on once it calms
  down (user, 2026-11-12). Today it is on at each boot; it was not the cause of the streams' problems.

## Hardware

- Heat after hours of use: read the thermal zones after a long session (stream, games) and check whether the
  kernel's thermal mitigation throttles the CPU/GPU meanwhile (the fan and the CPU clocks follow the temperature now,
  docs/performance.md).
- Bluetooth works (user, 2026-11-12): more tests and polish: keyboard/mouse, audio (A2DP), Steam's and Plasma's
  Bluetooth settings, what Plasma's toggle does to the chip's power, a faster patch download (~11 s), the UART's
  power cost (in-band sleep).
- Power LED colour controlled by the user (low priority).
- USB OTG: keyboards, storage and other devices on the headset's port (low priority).

## Steam Frame features

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
