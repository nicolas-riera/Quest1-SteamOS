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
- `q1marker.cpp [DOWN_M] [head]`: a world-locked overlay with a random pattern, 1.5 m ahead and DOWN_M (0.45) below the
  head: a local reference in the same snapshot as the stream's picture. Cross-build in WSL:
  `aarch64-linux-gnu-g++ -std=c++17 -O2 -I/root/openvr-sdk/headers q1marker.cpp -L/root/openvr-sdk/lib/linuxarm64
  -lopenvr_api -static-libstdc++ -static-libgcc`; run as steamos with
  `LD_LIBRARY_PATH=/opt/steamvr/bin/linuxarm64:/opt/hybris/lib LD_PRELOAD=/opt/hybris/lib/libbionictls.so`.
  With `head` it is head-locked (placed relative to the headset like SteamVR's HUD, another pattern), to run next to
  the world-locked one: a negative DOWN_M puts it above. Monado's own output (`touch /tmp/quest1-snap-monado`, with
  `QUEST1_WARP_USAGE=0` for linear outputs) shows where the panels get it; for a motion both vrcompositor and the
  warp see, apply `fake-motion-monado.patch` (test only, to the headset's Monado tree; same `/tmp/q1-fake-yaw`, plus
  an optional third number: a sideways sine of that many metres) instead of the driver's patch (docs/direct-scanout.md,
  2026-11-13).
- `replay-head-monado.patch` (test only, to the headset's Monado tree, with or without the one above):
  `/tmp/q1-replay` "S" replays a recorded head (`/tmp/q1hd/headrec.txt`, lines "t qx qy qz qw wx wy wz" of Meta's
  poses, angular rate in the head frame) at Monado's input, its motion scaled by S, so the prediction runs on real
  data. The recording is the user's: keep it local.
- `pcmarker.py [DOWN_M] [SIDE_M]` (on the PC, `python -P`, during a Steam Link VR session): q1marker's twin on the
  PC side, a world-locked overlay of the PC's SteamVR (another pattern), 1.5 m ahead of the head as the PC sees it,
  DOWN_M (0.85) below and SIDE_M (0.5) to the right: the PC draws it into the stream, next to q1marker 0.85. Stop it
  with Stop-Process (its process is `python3.13.exe`); a running one makes the next fail (CreateOverlay 17).
- `stepsnap.py N STEP_DEG X,Y...` (as steamos on the headset, with `pose-knobs-vrlink.patch` and `shift` in
  /tmp/q1vr): the pose sent to the PC stepped back and forth (`AXIS=pitch` for pitch), a snapshot 0-40 ms after each
  step reached vrlink's submits, each region's shift against a still reference: a picture placed right stays at 0 px.
  Regions on the PC's scene only (a horizon for pitch), not on an overlay (docs/streaming.md item 13, 2026-11-15).
  The layer's fix (2026-11-16) is A/B'd with `nostreampose` in /tmp/quest1-layer.
- `vrlink-horizon-probe.patch` (test only, to the shim): per vrlink submit, the decoded picture's horizon rows next to
  the submitted pose's pitch: tells whether the PC's picture follows its own pose (it does).
- `pose-knobs-vrlink.patch` (test only, `git apply` to the shim, never commit it applied): files (as steamos) that
  change the head pose vrlink gives the PC: `/tmp/q1-posoff` "X Y Z" cm, `/tmp/q1-poseoff` "YAW PITCH" degrees,
  `/tmp/q1-posenoise` "DEG" (a random yaw at each read, or with `/tmp/q1-posesine` "HZ" a yaw sine), `/tmp/q1-angoff`
  "X Y Z" deg/s, `/tmp/q1-veloff` "X Y Z" cm/s; and `/tmp/q1-labellog`: each submit's pose to `/tmp/q1lab.txt`. With
  pcmarker next to q1marker, the headset still: a pose that is right in the headset shows the PC's marker at 0 px
  for any change of the pose the PC renders with (docs/streaming.md item 13, 2026-11-13 night).
- `vrc-trace-layer.patch` (test only, to the layer, never commit it applied): traces of vrcompositor's descriptors,
  buffer bindings, uniform updates and buffer copies (with the copied data), and a lock-in on its mapped memory, to
  find where it keeps the stream frame's pose (its header has the trigger files).
- `shift.c`: `shift A.ppm B.ppm X Y W H R`, the (dx, dy) that best moves a region of A onto B (build on the headset:
  `gcc -O2 -o shift shift.c -lm`). With `R` 2 and A = B it prints the region's contrast (sd): pick regions with
  some, and check they stay put without motion (Steam Link's animated skies do not).
- `marker-jitter.py AMP HZ N POS X,Y...`: N timed snapshots during the fake motion, the shift of each region
  (marker first, a stream region second) against a still reference.
- `marker-fit.py HZ FILE...` (on the PC, `python -P`): the stream region against the marker, and each frame's error
  as a time offset (0 ms: the picture is where its pose says).
- `mjit.py N X,Y...` (as root on the headset, with `shift` built next to it in `/tmp/q1hd`): the one test for every
  case, on Monado's own output (`fake-motion-monado.patch` and `QUEST1_WARP_USAGE=0`): a still reference, then N
  snapshots under a 4° 2 Hz yaw, each region's shift per line. With the world marker, the head marker and a stream
  region: the head marker stays at 0 and the stream moves as the world marker (stream minus world: the error). The
  snapshots stay on the headset and are deleted. `MOTION="DEG HZ [M]"` sets the fake motion, `REPLAY=S` replays the
  recorded head instead (seconds since its start last on each line). Compare regions close to each other: under pitch
  and roll, distant ones move apart even when right. Snapshots show where frames land, not when: a frame shown a
  vsync late is right in them; check Monado's `late commit` count for that.
