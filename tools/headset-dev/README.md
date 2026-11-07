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
- `gputrace.sh [SECONDS]` + `vrgpu.sh`: GPU time per context from the kgsl tracepoints (vrcompositor shows as
  "q1compat submi"), the SteamVR and Monado rates.
- `thr.py [SECONDS]`: CPU % per thread of every process over an interval (top 35, with the last CPU).
- `bench.sh LABEL [SECONDS]`: shows the dashboard, then averages vrcompositor's fps (the layer's `display:` lines),
  its render thread CPU per frame and the GPU load. Meaningful only with the panels on (worn, or Monado with
  `QUEST1_IGNORE_PROX=1` through a volatile drop-in in /run, removed after) and the GPU governor Monado sets.
- `snapcrop.py OUT.png`: the dashboard panel only, cut from /tmp/quest1-snap.ppm (no room image around it).
- `cdp2.py "PAGE TITLE" '[[method, params], ...]'`: sends CDP commands to a Steam page (exact title first, then a
  substring); `Page.captureScreenshot` results go to /tmp/cdpshot<N>.png, ["sleep", s] waits.
- `dprobe.cpp`: prints every second whether the dashboard is shown and whether the desktop overlay is active in it
  (build on the headset against the OpenVR headers, run as steamos with LD_LIBRARY_PATH to libopenvr_api.so).
