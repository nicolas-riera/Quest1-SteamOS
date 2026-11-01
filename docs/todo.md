# To do

Open items, roughly by priority. Done items move to the commit log.

## Display
- Lens distortion and chromatic aberration are still off, most at the top and bottom of the
  view, also left and right (user, 2026-11-01). Plan: a full-screen calibration grid drawn with the
  lens model (`q1grid`), tuned live against the user's eyes or phone photos through the lens, then
  the result goes into driver_quest1 `ComputeDistortion` and Monado's warp.
- SteamVR popups (volume bar, IPD) show up too low in the view; the IPD popup has very strong
  chromatic aberration. Re-check after the distortion fix: probably the same vertical error.
- Apply the per-panel calibration from `/persist/calibration/display` (decenter, roll, mura).
- Monado's re-projection in Meta tracking mode deforms the image (user, 2026-11-01): turning the head
  left stretches the top of the view to the right, "soft". Sign is right (negating it is far worse).
  Suspects: q1trackd's angular velocity frame used for extrapolation, or a frame mismatch between
  the render pose and Monado's head pose. Live knobs in `/tmp/quest1-reproj` (`vel=0`, `delay=1`,
  `rx/ry/rz=`), per-axis delta and o-jitter in Monado's `direct_tick` log.
- Performance: vrcompositor ~26 fps while warping at 72 Hz, ~41 fps without (GPU contention). GPU:
  vrcompositor ~46 %, SteamVR dashboard CEF (vrwebhelper) ~26 % even when hidden (both pages
  visible, rAF ~100 Hz), warp ~23 % (~4 ms/pass, linear 1440x1600 buffers). Next: non-blocking
  warp on a high-priority queue, throttle the hidden dashboard, Steam's steamwebhelper CPU.

## Tracking
- 6DoF with Meta's trackingservice (docs/tracking-6dof.md): poses are valid for short periods;
  RuntimeIpcBroker `connect` still fails, the `/dev/cpuset` fix is untested, then q1trackd →
  Monado meta mode end to end.
- Controllers: input and tracking while Meta's stack owns SyncBoss.

## Streaming
- Steam Link streaming from a PC fails (2026-11-01): VR streaming leaves the PC's SteamVR waiting
  for the connection and the stream app on the headset closes; a 2D game started in cinema mode,
  its audio stuttered, then the headset's SteamVR crashed. Logs saved on the headset in
  `/root/stream-crash-20261101/`.

## Power
- The power button only opens the dashboard: a short press must put the headset to sleep and
  wake it like the stock behaviour, and a long press should open the power menu.
- Sleep must work properly: panels off, tracking and SteamVR paused, then a clean wake-up.
- Restart and Shut down from Steam do nothing (user, 2026-11-02): the headset can only be turned
  off by a forced power-off. Both must work from Steam's power menu.

## Hardware
- LEDs: the white front LED must be on while the cameras are active (stock behaviour), and the
  power LED must get its stock behaviour back (it stays solid yellow/orange).

## Later
- Windows installer that downloads SteamOS from Valve, reads the tracking software from the
  user's own headset, builds the image and flashes it (slot B), once everything works.
