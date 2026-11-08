# To do

Only what is left to do, roughly by priority. Done work leaves this list: it is described in the other docs
(`docs/*.md`) and in the commit log.

## Before the next push (user, 2026-11-08)

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
- Playspace and spatial anchors: set the playspace up again (the one of 2026-11-08 was never written to disk, fixed
  since: docs/boundary.md), then confirm it comes back in place after a SteamVR restart, a reboot and putting the
  headset back on. If not, read `/var/log/quest1-map.log` (the engine's map events and q1anchord's states) right after.
- "View in theater mode" (dashboard) crashes vrcompositor (2026-11-08 13:00:37, minidump in /tmp/dumps): right after
  the compat layer logged a failed graphics pipeline (2 stages, R8G8B8A8_SRGB colour attachment, vertex stride 20,
  1 sample). SteamVR restarts by itself.
- Brightness: confirm in the headset that both eyes match at low brightness (display-engine dimming, panels'
  calibration, 2026-11-08).
- Audio: confirm by ear that the crackle and the click at each Steam start are gone (the output now never closes,
  2026-11-08, docs/audio.md). If not: crackles at random, for a while, sometimes for good (user, 2026-11-02 to 05). PipeWire sees no xrun:
  below it (ADSP, codec) or memory pressure. Capture: `q1-audioring` (docs/audio.md). Also a click at each
  Steam start.
- After each put-on, ~10 s where every commit blocks ~10.7 ms in `MSMFB_ATOMIC_COMMIT` (a frame late each). Reproduced
  without wearing it (2026-11-08): `/run/quest1-standby` for 6 s, then panels on: 58 then 3 late commits in two 2 s windows.

## Tracking

- Tracking loss: confirm with a real loss that the headset keeps its last position and that the
  controllers come back (user).
- The kernel logs "SPI transaction rejected by SyncBoss" / "Bad magic number 0xcacacaca" several times a
  minute: compare with Android, check whether IMU or controller data is lost.
- Frame rate drops while the controllers move fast (Meta's controller tracking threads work harder).

## Display

- 90 Hz mode, selectable from the refresh rate setting (low priority). Base: github.com/TheCez/quest1-90hz-orangefox.
- Per-panel lens decenter and roll from the calibration (`*_screen_offset.json`), once Meta's convention is known.
- Scene mode (`/etc/quest1-warp-lens` 2, docs/performance.md: GPU per frame ~9.7 -> ~6 ms): check it in the headset
  (sharpness, geometry, chromatic aberration, the dashboard and passthrough), then make it the default.
- Steam's VR UI: check the right controller's Oculus button logo in the headset (user).

## Passthrough

- Confirm worn: no more shake/tilt of the camera image (fixed 2026-11-07).
- The cameras as a real SteamVR camera (settings in SteamVR, used by the boundary drawing too), as on the Frame.
- Tint slider: check that it changes the colour.
- Camera power-up race at some boots (trackingservice "Timeout: (power: SLEEP) => (power: STANDBY)", 3DoF
  only), and udev helpers hanging ~3 min at the same boots: catch again with logs.

## Boundary and room setup

- Leaving the room setup, Steam showed a "something went wrong" popup once.
- `ComputeInverseDistortion` returns identity while direct scanout distorts.
- Test Recenter and Clear playspace in the headset.
- Pairing Quest controllers from Steam's UI (`syncboss_input_pair`: never without the user's explicit go).

## Steam settings and Steam

- Timezone dropdown hidden (Steam shows it only under gamescope: `IN_GAMESCOPE` comes from steamui.so itself).
- Steam's first-run setup of the Frame: untested.
- The "+" menu's Desktop entry: confirm one click is enough with the laser (user).
- Desktop mode: lag and dashboard freezes while it is used (memory, llvmpipe); the control bar's keyboard and
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

- Steam Link from a PC: hardware HEVC decode with msm_vidc (docs/streaming.md); the stream app's GL can't share
  images with the compositor (libhybris GL lacks GL_EXT_memory_object_fd / semaphore_fd).

## Hardware

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
- Dual boot with a boot selector in the headset (very low priority).
