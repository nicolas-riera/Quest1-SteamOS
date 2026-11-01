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

## Hardware
- LEDs: the white front LED must be on while the cameras are active (stock behaviour), and the
  power LED must get its stock behaviour back (it stays solid yellow/orange).

## Later
- Windows installer that downloads SteamOS from Valve, reads the tracking software from the
  user's own headset, builds the image and flashes it (slot B), once everything works.
