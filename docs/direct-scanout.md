# Direct scanout and 72 Hz reprojection

How SteamVR's frames reach the Quest 1 panels, and the plan for smooth head motion when vrcompositor
renders below 72 fps.

## Current state (2026-11-01)

`/etc/quest1-direct-scanout` = 1 enables it (read by `/usr/local/bin/steamvr-q1`, which exports
`QUEST1_DIRECT_SCANOUT=1` and `QUEST1_DISPLAY_SIZE=2880x1600`).

- **driver_quest1** (vrserver) declares the real Quest 1 lens distortion (`ComputeDistortion`, Meta's
  CatmullRom10 model, per-channel chroma) and a 2880x1600 display. The left eye is drawn in the
  right half of the display image and the right eye in the left half, because the panels show the
  framebuffer turned by 180°. `QUEST1_DIRECT_FLIP` (from `/etc/quest1-direct-flip`, 3) holds the
  orientation fixes found on the device: bits 1+2 (vrcompositor's distortion v runs bottom-up, on
  the output and on the texture side). User-confirmed 2026-11-01: distortion correct with 3. Bit 8
  (mirror u) is wrong: it puts each lens centre on the temple side and the eyes look far too
  far apart.
- **The compat layer** (`src/vklayer-steamvr/quest1_scanout.c`, inside vrcompositor) gives
  vrcompositor plain 2880x1600 swapchain images that are never rendered to. Each eye's distortion
  render pass is redirected into a 1440x1600 gralloc buffer of its own at x = 0 (`half_*` fields
  of `struct qd_swapchain`; twin image views and framebuffers; the right half's viewports and
  scissors move left). Reason: on the Adreno 540 blob, a render area at x = 1440 of a linear image
  comes out as repeated 320-px columns, and rendering into a 2880-wide linear image is slow.
- **The commit**: when a frame is rendered, the layer itself commits the two half buffers on
  `/dev/fb0` (`MSMFB_ATOMIC_COMMIT`, DMA pipes 64 and 128, `MDP_BGRA_8888` because vrcompositor
  writes through B8G8R8A8 views). Retire fences answer `vkWaitForPresentKHR`, release fences free
  the images.
- **Monado** stops compositing while `/tmp/quest1-scanout` names a live process and only keeps the
  panels powered while the headset is worn (proximity). Before powering the panels down it creates
  `/tmp/quest1-panels-off`, waits 60 ms, and removes it after powering them up: the layer must not
  commit in between, because a commit queued while the panels power down never completes and wedges
  mdss (every later FBIOBLANK times out; only a reboot recovers).

Cost (GPU timestamps per render pass, `runuser -u steamos -- touch /tmp/quest1-gputime`, result
in `/tmp/quest1-compat-vrcompositor.log`): ~18.7 ms per frame with the Steam dashboard open and the
Aurora environment (per eye: ~3.4 ms environment, ~1 ms overlays, 2.7–5 ms distortion with 6 texture
samples per pixel), so ~40 fps. Monado no longer uses the GPU.

**Problem**: nothing re-projects between vrcompositor's frames any more (Monado used to, at 72 Hz),
so at ~36–40 fps the world judders when the head turns.

## Plan: Monado presents vrcompositor's frames and re-projects them

Monado already has everything a 72 Hz re-projection needs: the head pose at any time, a
high-priority Vulkan queue (`VK_EXT_global_priority` is exposed by the blob, 3 queues), the vsync
thread and MDP commit code (`src/monado/src/xrt/compositor/main/comp_window_mdp.c`), the panel power
logic, and the Quest 1 lens model in the orientation the user validated
(`src/monado/src/xrt/drivers/quest1/quest1_hmd.c`: `hmd_compute_distortion`, views rotated by 180°,
view 0 = left eye = framebuffer right half). Having a single process commit also removes the
panel-power race for good.

### Data flow

1. driver_quest1, when it publishes the pose of frame N+1 (`PublishFramePose`, answered to the layer
   by `QD_POSED`), adds that pose to the `QD_POSED` message: orientation quaternion (x, y, z, w, in
   Monado's LOCAL space as returned by `xrLocateSpace`) and the predicted time it was located for.
   `struct qd_msg` grows by these fields (both sides are rebuilt together).
2. The layer, in direct mode, connects to a unix socket of Monado (`/run/monado/quest1_direct`,
   SOCK_SEQPACKET; vrcompositor runs as steamos, so the socket must be accessible to it). At
   swapchain creation it sends every half buffer once, with
   `AHardwareBuffer_sendHandleToUnixSocket` (libnativewindow, same hybris route as
   `AHardwareBuffer_allocate`), tagged (image index, half). For each rendered frame it sends
   {image index, frame number, render orientation, render time} instead of committing it.
   If Monado's socket is absent it keeps today's self-commit path (fallback, and for debugging).
3. Monado (`comp_window_mdp.c`): a receiver thread imports the buffers
   (`AHardwareBuffer_recvHandleFromUnixSocket`, then the same Vulkan AHB import it uses for its own
   targets, as sampled images) and queues frame messages. While a client is connected Monado's own
   compositing stays paused (as with `/tmp/quest1-scanout` today) and its vsync thread drives the
   panels:
   - at each vsync, for the next one: if a new frame arrived, commit its two half buffers directly
     (zero copy, today's layer commit code moves here) when its render orientation is within a small
     angle (≈0.2°) of the head orientation predicted for that vsync; otherwise
   - **re-project**: render the latest frame into Monado's own target images with a warp mesh and
     commit those. Per eye a grid (e.g. 48x48) over the output panel: for each vertex, the panel
     point → tangent direction by the lens model (green channel; the source is already
     chroma-corrected), rotated by `q_delta = conj(q_render) * q_display` (head frame, OpenXR axes:
     x right, y up, −z forward; direction = (tx, ty, −1)), → back to a panel point of the source
     frame by inverting the lens model (Newton on `hmd_compute_distortion`'s radial function,
     a few iterations), which is the texture coordinate in that eye's half buffer. One bilinear
     sample per pixel, high-priority queue; expected ≈1 ms per eye.
   - the frame's release goes back to the layer (message: image index free) once no commit shows it
     and it is no longer the newest frame (it stays the re-projection source until replaced).
   - "displayed" goes back to the layer (frame number, vsync time) for `vkWaitForPresentKHR`.
   - panel power: unchanged (prox); nothing is committed while the panels are off.
4. vrcompositor then simply renders as fast as it can (≈40 fps) and the user sees 72 Hz motion.

### Testing aids

- Snapshot of what is committed: Monado should keep an equivalent of the layer's
  `/tmp/quest1-snap` (PPM of the two halves) for its warped output.
- A test knob that adds a fixed yaw (e.g. 5°) to `q_delta` makes the warp direction obvious in a
  snapshot (the image must move the right way: content shifts left when the head turns right).
- `/tmp/quest1-gputime`-style timing of the warp pass.

## Panel calibration data (for the brightness mismatch the user reported)

Read-only, in `/persist/calibration/display/{left,right}/` (file names carry the panel serials: never
copy them into the repo):
- `<serial>.uniformity` (96 bytes, magic "UNIF", little-endian floats): factory colorimetry of the
  panel: CIE xy of white/red/green/blue and their luminances. Both panels measure ~98.4 nits white
  (R ~26.3, G ~71.5, B ~5.9): the panels themselves match.
- `<serial>.mura` (21-byte header "mURA..." + 1440x1600x3 bytes) and `.mura.astc`: a per-pixel
  correction map Meta's compositor applies (OLED mottling, "mura"); we don't apply it yet. A
  candidate for the re-projection/warp shader (one more texture fetch).
- `{left,right}_screen_offset.json`: per-panel lens-to-panel alignment (`decenter` in pixels, about
  2.5–3.9 px x and −4.8 px y; `roll` 0.02–0.056°). Not applied yet either; it belongs in the
  distortion (driver_quest1 `ComputeDistortion` and Monado's `hmd_compute_distortion`).
- Brightness: the Samsung dual-DSI panel (`Dual Samsung video mode dsi panel`) uses
  `bl_ctrl_dcs` with `qcom,cmd-sync-wait-broadcast`, so the kernel sends the backlight DCS command to
  both panels (mdss_dsi_panel.c `BL_DCS_CMD`): a single `lcd-backlight` level drives both. A
  remaining whole-eye brightness difference would come from the missing mura correction or from
  the rendering path, not from the backlight.
