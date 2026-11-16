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
- Microphone: noise suppression and echo cancellation (user, 2026-11-12: it picks up everything, from the speakers'
  sound to the fan's noise). E.g. PipeWire's echo-cancel module (WebRTC) between the speakers and the mic, as the
  default source.

## Tracking

- The room comes back a bit off at the first put-on after a boot: the engine relocalizes in the saved map, logs "All
  submaps have bad map tracking" seconds later, evicts the context and starts over, so the saved map (2 submaps, from
  one spot) never grows (docs/boundary.md "The room after a boot"). Next (user: "as on Android, no constant
  delocalization"): clear the map with the user and walk the room before setting the playspace again; persist only
  once the room is covered, then again while tracking is good. See also whether a starved trackingservice makes it
  worse (2026-11-10: "the tracking sneezed and I flew off" with all 8 cores busy).
- The playspace is not where it was drawn (user, 2026-11-12: a new boundary, the headset off and on, no longer in
  place, "the space anchors work really badly"; again on 2026-11-13 after a reboot), and the engine opens new
  submaps instead of finding the saved ones (user). The boundary shifts mostly after a reset of the tracking, e.g.
  when the tracking service is restarted (user, 2026-11-13): test the playspace across stack restarts first. Found
  2026-11-12 (docs/boundary.md "Tracking starved of CPU"):
  - Meta's tracking is pinned to CPUs 0-3 as on Android (~3 cores' worth: IOTcam ~1.2, controllers ~0.85), but
    here vrserver, the vrwebhelpers, Steam and q1apps run there too: the four little cores at 100 %, the engine
    logging "Map tracking did not run" up to once a second, "Bad map tracking" 30-50 times a minute, falling to 3DoF,
    and while it tracks two contexts it skips map building. With everything else moved to CPUs 4-7 (a live
    `taskset`, lost at each restart): 0-1 "Bad map tracking" a minute worn, no new context at a put-on, and the
    boundary was in place after that put-on. Make it permanent (an `AllowedCPUs`/`CPUAffinity` for everything but the
    `android-*` units: Steam's own 2-6 pinning of its webhelpers must not bring them back to 2-3), then test the
    playspace over several sessions and reboots.
  - The playspace drawn at 21:40 was drawn while the tracking was failing (the frame may have slid under it).
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

- 90 Hz mode, selectable from the refresh rate setting (low priority). Base: github.com/TheCez/quest1-90hz-orangefox,
  and a Reddit write-up (r/OculusQuest "I got a Quest 1 running at a real 90 Hz", Juspertinry's boot image and module):
  only the device tree changes on Meta's kernel (`qcom,mdss-dsi-panel-framerate`, `max-refresh-rate` and the timing
  bytes scaled ~1.67x); the check that it really runs at 90 is the panel clock against the DSI bit clock
  (`pclk0_clk_src` rate = `dynamic_bitclk` / 6, 133.0056 MHz at 90), after a short blank to resync it. **Not every
  panel takes it**: Samsung's (`qcom,mdss_dsi_sdc_lightman_video` in `mdss_mdp.panel` on the command line, the dev
  headset's) does, AUO's stays at 72. So the setting shows 90 only on a panel known to work, and only once the kernel
  reports the rate, with a fall back to 72 if not.
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
- SteamVR's playspace setup (user, 2026-11-12): the controllers' models are wrong there (seen before elsewhere too: a
  dark disc where the ring should be), and it runs poorly.

## Steam settings and Steam

- Timezone dropdown hidden (Steam shows it only under gamescope: `IN_GAMESCOPE` comes from steamui.so itself).
- Steam's first-run setup of the Frame: untested.
- SteamVR restarted alone (Steam kept running) breaks Steam's VR UI: steamwebhelper's GPU process takes GPU page faults when
  vrcompositor stops (2026-11-11 17:50 and 17:51, two restarts) and the dashboard then shows its lower half empty and loses
  its bottom bar (restarting Steam brought it back, user). SteamVR alone should restart cleanly.
- A setting in the VR settings' Oculus Quest category, on by default: ignore the Oculus and Menu buttons while the
  controller moves (they are easy to press by mistake on Touch controllers; Meta's OS and the Rift S, same controllers,
  do the same).
- Steam's compatibility rating defaults to SteamOS's rating instead of the Steam Frame's (user, 2026-11-15).

## Power

- The sleep animation shows inside the dashboard panel instead of full screen.
- Real sleep (suspend to RAM, `/etc/quest1-sleep` deep, opt-in): on resume Meta's tracking is lost, no sound,
  the proximity sensor wakes the headset while worn, the fan and power LED stay on.
- Restart from Steam: with the OS started by `fastboot boot` it restarts into Android; test it once the OS is
  flashed (user), then drop the sshd `pam_systemd` workaround in `device/holo-perf.sh` if it works.
- Charging while off (user, 2026-11-16): plugged in while off, the headset boots into a charging-only mode (the
  bootloader starts the boot image in charger mode: `androidboot.mode=charger` on the command line, Meta's OS then
  shows its battery screen and charges). Once SteamOS is flashed, that boot would start the whole of SteamOS. The
  initramfs has to see charger mode and do what Meta's OS does instead, ideally with Meta's own charger from the
  stock image (its charger binary / healthd, screen off or the battery animation, power button to boot), rather than
  our own.

## System

- SteamOS and SteamVR updates must keep working without undoing our changes.
- Slim the install: drop what this headset never uses (other controllers' render models, etc.).
- Valve's `steamos-log-submitter` (uploads logs to Valve, ~34 MB of RAM): check whether it runs on the headset and
  what it sends; the Quest 2 port masks it.
- The telemetry HAL's loop (docs/performance.md, 2026-11-11 heat): once an event cannot go to ovrstub's
  TelemetryService it retries with no pause (a core at 100 %, ~50000 log lines/s, fakelogd at 90 %). Stopping the HAL
  froze the tracking: make the stand-in accept the events, or find what it fails on.
- Storage hang: twice on 2026-11-12 (and earlier) the headset stopped answering (ping fine, sshd's banner timing out,
  a build or a test running); only a reboot helped, and the journal is not kept. Keep the journal (or pstore), then
  read q1-hangwatch's D-state stacks.
- A GPU page fault in a steamwebhelper (2026-11-12 20:28, during a stream) took vrcompositor's device with it
  (`vkQueueSubmit` -4): SteamVR restarted by itself. Again at 21:48, same fault address: the layer's "q1 display
  done" thread spun at 93 % in its fence wait for 5 s, then VK_ERROR_DEVICE_LOST and vrcompositor aborted.

## Streaming

- Steam Link VR (docs/streaming.md, section 2): a new picture each frame, a sharp centre (user, 2026-11-12: "it's
  sharp") and no trembling any more (2026-11-16, blind A/B worn: "it doesn't tremble at all any more"), but it flashes black
  when the headset is loaded (the dashboard open), its latency is still a bit high (user, 2026-11-16) and it runs
  below 72 fps.
  - Latency: measure it (the head's pose to the picture's pose on screen) and find where it goes: the PC's ~90 ms
    prediction says how far ahead it must guess.
  - Now that the stream's pose is right, check whether the 40 ms low-pass of the angular velocity vrlink gets (an
    interim against the trembling, 2026-11-13) still helps or only adds swim (blind A/B worn).
  - Position: vrlink submits no depth, so a position error of the PC's prediction is not corrected at all.
  - On a system that had run for hours, the stream was 16.6 px rms off the world in Monado's output (2.5-3 px fresh).
    Follow it over a long run (`mjit.py` every few minutes, with vrcompositor's fps, fds and memory) to find which of
    these takes it there (vrcompositor's `sync_fence` leak is fixed: PR #4, display-event fences from a pool):
    - The headset slowing down over time: 64-71 fps at the start, 55 fps 90 minutes later, ~380 MB available, swap
      in use, load ~14.
  - Black flashes (user, 2026-11-12): vrlink logs no decoder reset outside the session's start; not found yet.
  - Frame rate: the layer's inset pass costs ~2.5 ms of GPU (58-60 fps against ~69 with the dashboard open).
    vrcompositor has the Frame's own way, free: `distort_ps_achromatic_se2` blends the inset in its distortion pass,
    but here it draws with `distort_ps_layered` (chromatic, overlays in the pass), with `enableAchromaticDistortion`
    true in steamvr.vrsettings. Find what picks the shader (its settings block reads achromatic off at run time).
  - Start of a session: the headset stalls for seconds (Monado's re-projection too), the system swaps ~150-250 MB
    as vrlink and vrcompositor allocate (it was ~700 MB before the copies shared their memory). Lock Monado's (and
    the tracking's) memory, and find what else allocates then.
  Also: the PC's overlays show only when the PC's SteamVR started after
  vrlink; stopping a session from the headset (`StopStreamingSession`) does nothing and vrlink hangs on its way out.
  With the USB cable in, vrlink and the PC send the stream over the USB network too (each datagram on each link: about
  a core on the headset, 2026-11-12). Hiding usb0 from vrlink (getifaddrs, and dropping what it sends there) made it
  never connect ("Timed out getting information from the server"): the PC reached it over USB first.
- Steam Link from a PC, 2D (docs/streaming.md, section 1): it works well (user, 2026-11-12); more performance is
  welcome: the kernel emulates atomic instructions for Steam's programs (`emulate_atomic` in the profiles), the
  Vulkan/OpenGL semaphore is refused (glFinish fallback).
- Hardware video decoding for the whole system (user, 2026-11-09: "hardware video decoding system wide, Chromium
  included"). FFmpeg's `*_v4l2m2m` decoders and VLC use venus (`q1-hwdec`, docs/streaming.md "The decoder"). Left:
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
- x86 games (FEX, Proton): very low priority. Ruby / RPG Maker games (mkxp, OneShot) close at once on ARM64 Proton
  (the Steam Frame too): github.com/URFAVF3MBOY/quest2-steamos (code reuse allowed, credit it) puts the 17 bytes of
  x86 `_isatty` code Ruby looks for back into Proton's `ucrtbase.dll` on a game's first start (its docs/PROTON.md).
- Other controllers: Windows Mixed Reality, Quest 2 (github.com/Juspertinry/q2ctrl-module).
- Quest 3 controllers (Touch Plus): github.com/Noculus-VR/TouchPlus-on-Quest-1 (user, 2026-11-13) pairs them with the
  Quest 1 by patching `libsyncboss.so` (no MCU or firmware change). Not finished: wait until it is, then see what our
  stack (q1trackd, libossdk) needs.
- The installer app (app/, installer/); later a Linux version that builds the image on the PC.
- While the headset's install system (the small Linux that builds the image on the headset) is building, the
  power LED blinks white <-> purple.
- Dual boot with a boot selector in the headset (very low priority).
