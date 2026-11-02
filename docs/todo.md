# To do

Open items, roughly by priority. Done items move to the commit log.

## Performance
- vrcompositor ~26 fps while warping at 72 Hz, ~41 fps without (GPU contention); the goal is to get
  as close to 72 as possible without visible quality loss. GPU: vrcompositor ~46 %, SteamVR
  dashboard CEF (vrwebhelper) ~26 % even when hidden (both pages visible, rAF ~100 Hz), warp ~23 %
  (~4 ms/pass, linear 1440x1600 buffers). Next: throttle the hidden dashboard, non-blocking warp on
  a high-priority queue, vrcompositor's passes, Steam's steamwebhelper CPU, trackingservice CPU.
- SteamVR's Facet renderer (tip from a user, 2026-11-02) runs and draws correctly behind
  `/etc/quest1-facet` (off by default, `src/vklayer-steamvr/quest1_facet.c`): ~29-32 fps, eye passes
  6-7 ms each. Needs a fair A/B against the old renderer in one quiet window
  (`/root/facet-iter2.sh 0|1 <dir>`), with the dashboard shown (dropped stencil dynamic states).
  Leave it off unless it wins.

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
- Camera frames reach `/dev/shm/quest1-cameras` through `src/camtap` (30 Hz, 4 cameras), but
  SteamVR's `AcquireVideoStreamingService` fails, so nothing is shown. Either open that gate or
  draw the cameras ourselves (overlay), then depth-correct re-projection like Meta's.

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

## Power
- Power button and sleep work (2026-11-02: short press = Steam's sleep, panels off and Meta's
  tracking asleep with its cameras; 1 s press = Steam's power menu). A few details are left (user):
  to list at the next test. Sleep is still a standby, not a kernel suspend.
- Restart and Shut down from Steam: retest with the PR #1 kernel (build-6 and later). If they work,
  drop the sshd `pam_systemd` workaround in `device/holo-perf.sh`.

## Steam and desktop
- The Steam Frame's desktop mode (KDE Plasma) from the "+" menu. It works in the Quest 2 port
  (github.com/URFAVF3MBOY/quest2-steamos), whose author allows us to use its code: base it on that.
- The "+" menu shows 4 apps that should not be there (Xfce-related, it seems).

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
