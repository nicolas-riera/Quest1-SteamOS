# To do

Only what is left to do, roughly by priority. Done work leaves this list: it is described in the other docs
(`docs/*.md`) and in the commit log.

## Before the next push (user, 2026-11-08)

- Performance: 72 fps (60 at least) with the headset worn, the controllers in view and the dashboard and Steam
  in use. SteamVR drops frames as soon as the dashboard (Steam's UI) is used: keep SteamVR's frame rate while
  Steam is in use, and make Steam as fast as possible without taking it from SteamVR (Steam lagging is
  acceptable). Leads (docs/direct-scanout.md, docs/steam-gpu.md):
  - the system layer (dashboard overlays) is rendered at 4x MSAA in vrcompositor, hard-coded; 1x would save a
    lot of GPU but is a quality trade-off: the user decides;
  - the LSE atomics trap path (kernel/lse_emul.c): ~17 % of vrcompositor's render thread, ~100,000 traps/s
    in all; make the trap path itself faster (no binary rewriting of Valve's files);
  - CPU placement: Meta's tracking threads starve when worn (position lost under load); Android pins
    syncboss to CPUs 0-3, here it runs on all eight;
  - the controller models: each part is a render pass of its own (~2.7 ms of GPU for both);
  - memory: ~500 MB available after boot with SteamVR, Steam's webhelpers ~1.65 GB;
  - Steam's UI: its renderer's main thread ~80 % of a big core, the VR overlay path copies frames with the CPU.
- Passthrough like Meta's: depth (near objects at their true distance) from Meta's own engine through
  MrSystemServiceIPCServer (user's call, 2026-11-06), instead of the fixed sphere.
- Re-projection: confirm in the headset that turning the head no longer lags (warp 8 ms before the vsync,
  display time 18 ms, 2026-11-07).
- Spatial anchors: confirm in the headset that the room comes back in place after a reboot and after putting the
  headset back on (set the playspace up again first: there is none since 2026-11-07). If not, read
  `/var/log/quest1-map.log` (the engine's map events and q1anchord's states) right after.
- Brightness: confirm in the headset that both eyes match at low brightness (display-engine dimming, panels'
  calibration, 2026-11-08).
- Audio crackles at random, for a while, sometimes for good (user, 2026-11-02 to 05). PipeWire sees no xrun:
  below it (ADSP, codec) or memory pressure. Capture: `q1-audioring` (docs/audio.md). Also a click at each
  Steam start.
- After each put-on, ~10 s where every commit blocks ~10.7 ms in `MSMFB_ATOMIC_COMMIT` (a frame late each).

## Tracking

- Tracking loss: confirm with a real loss that the headset keeps its last position and that the
  controllers come back (user).
- The kernel logs "SPI transaction rejected by SyncBoss" / "Bad magic number 0xcacacaca" several times a
  minute: compare with Android, check whether IMU or controller data is lost.
- Frame rate drops while the controllers move fast (Meta's controller tracking threads work harder).

## Display

- 90 Hz mode, selectable from the refresh rate setting (low priority). Base: github.com/TheCez/quest1-90hz-orangefox.
- Per-panel lens decenter and roll from the calibration (`*_screen_offset.json`), once Meta's convention is known.
- Lens distortion in Monado's warp (`/etc/quest1-warp-lens`, off): compare sharpness with the user once perf allows.
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
