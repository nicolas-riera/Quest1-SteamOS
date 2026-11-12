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

Stream picture placement (docs/streaming.md, "Measuring where the stream's frames land"):

- `fake-motion.patch`: test-only patch for driver_quest1 (`git apply`, never commit it applied): `/tmp/q1-fake-yaw`
  "DEG HZ [M]" (as steamos) adds a yaw sine (and a sideways x sine) to every head pose, local and the one vrlink
  sends, so a headset lying still sees a known, smooth head motion.
- `q1marker.cpp [DOWN_M]`: a world-locked overlay with a random pattern, 1.5 m ahead and DOWN_M (0.45) below the
  head: a local reference in the same snapshot as the stream's picture. Cross-build in WSL:
  `aarch64-linux-gnu-g++ -std=c++17 -O2 -I/root/openvr-sdk/headers q1marker.cpp -L/root/openvr-sdk/lib/linuxarm64
  -lopenvr_api -static-libstdc++ -static-libgcc`; run as steamos with
  `LD_LIBRARY_PATH=/opt/steamvr/bin/linuxarm64:/opt/hybris/lib LD_PRELOAD=/opt/hybris/lib/libbionictls.so`.
- `shift.c`: `shift A.ppm B.ppm X Y W H R`, the (dx, dy) that best moves a region of A onto B (build on the headset:
  `gcc -O2 -o shift shift.c -lm`). With `R` 2 and A = B it prints the region's contrast (sd): pick regions with
  some, and check they stay put without motion (Steam Link's animated skies do not).
- `marker-jitter.py AMP HZ N POS X,Y...`: N timed snapshots during the fake motion, the shift of each region
  (marker first, a stream region second) against a still reference.
- `marker-fit.py HZ FILE...` (on the PC, `python -P`): the stream region against the marker, and each frame's error
  as a time offset (0 ms: the picture is where its pose says).
