# Development helpers (copied to /root on the headset)

Not part of the image. Need `/root/cdp.py` (CEF remote debugging client) and `touch /tmp/q1-webdebug` (as steamos)
before a full `quest1-frame restart` (restarting the dashboard alone loses Steam's bar).

- `dis.py BIN ADDR [BEFORE AFTER]`: objdump around an address, adrp+add string references resolved.
- `snap.sh OUT.png`: snapshot of the next displayed frame, downscaled (camera image: private, never commit).
- `rsmon.sh SECONDS`: during a room setup test, logs every 2 s the setup step, Monado/driver rates, GPU load,
  vrcompositor CPU, plus 3 snapshots in step 7 (/tmp/rsmon).
- `sg-hide.sh NODE-TYPE` (+ `sg-hide.js` in /root): removes one scene graph node (e.g. `playspace-dots`) from the
  dashboard's live scene and rebuilds it (webpack module 5723 `my()`); restarting the room setup step restores it.
  Node types of step 7: playspace-center, -dots, -circle-perimeter-interaction, -floorheight-adjust,
  -floordetection-geo, -draw-interaction, -area-drawing, -outline.
