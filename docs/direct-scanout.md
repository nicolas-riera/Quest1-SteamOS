# Direct scanout and 72 Hz reprojection

How SteamVR's frames reach the Quest 1 panels, and the plan for smooth head motion when vrcompositor
renders below 72 fps.

## Current state (2026-11-01)

`/etc/quest1-direct-scanout` = 1 enables it (read by `/usr/local/bin/steamvr-q1`, which exports
`QUEST1_DIRECT_SCANOUT=1` and `QUEST1_DISPLAY_SIZE=2880x1600`).

- **driver_quest1** (vrserver) declares the real Quest 1 lens distortion (`ComputeDistortion`, Meta's
  CatmullRom10 model, per-channel chroma) and a 2880x1600 display. The left eye is drawn in the
  right half of the display image and the right eye in the left half, because the panels show the
  framebuffer turned by 180°. `QUEST1_DIRECT_FLIP` (from `/etc/quest1-direct-flip`, 19) holds the
  orientation fixes found on the device: bits 1+2 (vrcompositor's distortion v runs bottom-up, on
  the output and on the texture side), and 16, which mirrors the lens model's vertical asymmetry
  (lens centre 2.72 mm, 47 deg up / 53 deg down) without turning the image: 19 is now the default,
  user-judged better than 3 for distortion and chromatic aberration at the top and bottom. Bit 8
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

### 2026-11-06: 72 fps with the dashboard shown

vrcompositor's frames are serial: it records (CPU), submits, polls its fence every 100 µs until the GPU is done,
then waits for the next vblank event. Its CPU time and its GPU time add up, and a frame that ends after the vblank
costs a whole period. Measured with the panels on (`QUEST1_IGNORE_PROX=1` for Monado), head still, dashboard
shown, passthrough on (`/root/tmp/bench.sh`-style: average of the layer's `display:` lines over 30 s):

| Change | fps | render thread CPU per frame | GPU per frame |
|---|---|---|---|
| before | 53.1 | 4.04 ms | 11.5 ms |
| `vkCmdUpdateBuffer` through the layer's staging (quest1_compat.c, `ub_*`) | 54.2 | 3.11 ms | 11.3 ms |
| + UBWC scanout buffers (`QS_AHB_USAGE`) | 58.6 | 3.09 ms | 10.1 ms |
| + 4 swapchain images | **71.9** | 3.04 ms | 10.2 ms |

- **vkCmdUpdateBuffer**: the blob gave each one a 4 KB KGSL object of its own (alloc ioctl, mmap, IOMMU map, info
  ioctl, and the unmap and free when the command buffer is reset): ~18 a frame, ~940 a second on the render
  thread (`perf record -e kgsl:kgsl_mem_alloc --call-graph dwarf`). The layer copies the data into host-visible
  buffers it keeps per command buffer and records a `vkCmdCopyBuffer` (the same transfer write).
  `QUEST1_BLOB_UPDATEBUFFER=1` restores the blob's path.
- **UBWC**: gralloc's `VENDOR_0` usage gives compressed buffers. vrcompositor's distortion pass writes ~2.3 ms
  per eye into them instead of ~2.75 into linear ones, and Monado's warp reads them in ~1.2 ms instead of ~3 ms.
  Tiled surfaces ignore the B8G8R8A8 swap (the Adreno stores them as RGBA, like Mesa's freedreno does): mdss scans
  them as `MDP_RGBA_8888_UBWC` and Monado samples them without its red/blue swizzle (only applied when the layer
  says `bgra`). Checked against a CDP capture of the same Steam page. Snapshots of UBWC buffers are GPU copies
  into a linear buffer (`qs_snapshot_copy`); the linear snapshots used to have red and blue swapped.
- **4 images**: Monado holds the image on the panels and the one queued for the next vsync, so with 3 the
  next `vkAcquireNextImageKHR` waited for a vsync (~1.5 ms a frame on average, the margin that was missing).
  The app's 2880x1600 images are only handles (never rendered to or read): they share one allocation, so the
  fourth image costs only its two gralloc halves.

### 2026-11-07: vrcompositor renders straight to memory (RenderMode=Direct)

Worn, with the controllers in view, vrcompositor was at 37-46 fps: ~18 ms of GPU a frame in ~40 render passes
(per eye: distortion 2.5 ms, passthrough 2.2 ms, each controller part in a pass of its own, ~0.08 ms for the
smallest). The Adreno blob's tiled path loads and stores the 1500x1664 eye image around each pass; the Frame's
own setup avoids it for vrcompositor (`TU_DEBUG=sysmem`, docs/steamframe-stack.md). The blob's equivalent is
the `RenderMode` key of qgl_config.txt (`Direct`, docs/adreno-vk-extensions.md), given to SteamVR alone: its unit
binds `/etc/quest1/qgl_config-steamvr.txt` (the system's file plus that key, written by holo-frame-mode.sh) over
`/data/vendor/gpu/qgl_config.txt`. Same image (snapshot checked), measured head still, dashboard shown, no
controllers:

| | GPU per frame | smallest passes | distortion per eye | fps |
|---|---|---|---|---|
| tiled (blob's default) | 14.1 ms | 0.08-0.1 ms | 2.2 ms | 55 |
| `RenderMode=Direct` | 9.3 ms | 0.02-0.04 ms | 1.9 ms | 72 |

The first ~40 s after SteamVR starts run slower (shaders, the dashboard loading) before it settles. GPU clock
pinned at 710 MHz instead of the governor's 596: only +3 fps worn (the passes were memory-bound), not kept.
The GPU clock and the DDR vote (cpubw at its maximum already) are not the limit.

### 2026-11-07: the pose prediction follows the measured latency

Worn, the warp's delta (render pose against display pose) was 1.5-5 degrees on average while the head turned
(max 13), and the user saw the distortion come back. Monado now measures, for each new frame, how late it reaches
the panels against the time its pose was predicted for ("pose age" in its `direct_tick` line): 22-25 ms with 3 or
4 swapchain images alike, so driver_quest1's fixed 30 ms prediction (pose publication to photons) was short: the
path is ~53 ms at 72 fps. Monado writes the 2-second average to `/run/monado/quest1-pose-age`; driver_quest1
(`ReadPoseAge`) adds half of it to its prediction (20-90 ms; fixed when `QUEST1_POSE_PREDICT_MS` or
`/tmp/quest1-pose-mode` sets one). The age then averages about 0 (-10 to +10 ms per frame).

That did not remove the distortion (user): during head turns the warp's delta stayed 1.5-2.2 degrees and so did
the "o jitter". o, the constant rotation between Monado's device space and driver_quest1's LOCAL, was estimated
from every frame (its render pose against Monado's head pose at the same time), so each frame's prediction error
went into it; that error has the same direction for a whole head turn, and the 5 % average followed it (the
53 ms prediction makes it larger). Now o only learns from frames rendered while the head turns slower than
15 deg/s (Monado's head pose 50 ms apart), where the prediction error is small.

The distortion was still there (user). Three checks on the table (panels on, warp outputs made linear with
`QUEST1_WARP_USAGE=0` for Monado's snapshot, `thr=0`), with temporary driver_quest1 builds, image shifts measured
by block matching on the dashboard (40 px patches, normalised correlation):

- Geometry, yaw: SteamVR rendering with the head pose turned by 5 degrees moves the dashboard +78..+82 px; Monado's
  warp of the straight frame by the opposite 5 degrees (`yaw=5`) moves it -76..-82 px, the same profile over the
  whole panel (and the same small vertical components, mirrored). The lens model matches SteamVR's distortion.
- Pose to frame association: frames rendered with the pose turned by +-3 degrees by frame parity (and recorded
  so) come out of the warp within 0-1 px of the untagged frame: each frame is warped from the pose it was really
  rendered with. Pitch and roll the same way: within 0-6 px, the same residual in both, i.e. the slow drift of o
  the tagged frames cause (keep tags under the 4 degrees at which o is seeded again, or o jumps).
- Outside the rendered image the warp samples black (CLAMP_TO_BORDER), no smeared edges.

So the warp itself is right for a still head; what is left happens only while the head moves: the size of the
corrections (worn: 4 degrees average, 16 max during fast turns) and the latency behind them. The prediction the
driver settles on swings between 50 and 77 ms: SteamVR's frames take ~65 ms from the pose's publication to the
panels. Next: an A/B with the user wearing the headset, switched live (warp off, a fixed prediction), to tell the
warp's corrections from the rendering's own latency.

The A/B, worn, switched live: warp off, no deformation (but the image trembles: every frame carries its
prediction error); warp on with a fixed 30 ms prediction (larger corrections), worse. A temporary per-vsync log in
Monado (the head pose each image is warped to against Monado's head pose at that display time, read 80 ms later)
gave 0.3-0.5 deg even above 250 deg/s, no late commit, never the output set on screen reused; LOCAL against
Monado's device space is one fixed rotation (within 0.7 deg from yaw 28 to 172 and pitch -76 to +54 deg), and
snapshots of warped outputs taken while the head turned show nothing torn. What was left: when the image is
really on the panels against Monado's pose clock. The warp's display time, vsync + 1.5 periods (21 ms) for a video
mode panel (`mipi dsi video panel`), switched live with `delay=`: 21 ms deformed, 10 ms better, 3 ms no
deformation (user). Both clocks are CLOCK_MONOTONIC (q1trackd asks libossdk for "now"), so Meta's head pose is
most likely ~18 ms ahead of its stamp. `MDP_DISPLAY_DELAY_NS` (3 ms) is now the display time of the warp and of
the pose age the driver's prediction follows: the prediction settled at ~43 ms instead of ~65, the corrections
shrank with it, and a still head gets 0.05-0.09 deg (raw frames).

Tilting the head back and forth still trembled a little: its turn-arounds passed the 15 deg/s stillness test (no
net motion over 50 ms, the largest prediction error) and o took estimates 0.7 deg off. o now learns only when the
head turned less than 5 deg/s over each of the two 50 ms spans before the frame's pose time. What remains is the
warp's own limit: it turns the image, it does not move the eyes (tilting moves them sideways by centimetres).

### 2026-11-07 evening: the warp ticks just before the vsync, display time 18 ms

Worn with the 3 ms display time the user found the re-projection laggy when turning the head. Two findings:
- The 20 ms old IMU samples libossdk extrapolates from (its state's sample time against the time asked) only
  happen while the panels are off: with them on (the cameras and the display's TE running) the samples are 4.3 ms
  old (p95 5.0) and Meta's "Delayed IMU" warnings stop. So Meta's head pose is not ahead of its stamp, and 3 ms
  showed every turn ~15 ms late. The deformation that led to 3 ms predates the o fix above.
- Monado warped right after the vsync, for a commit latched at the next one: 13.9 ms of extrapolation for
  nothing. Its vsync thread now sleeps until 8 ms before that next vsync (`MDP_TICK_LEAD_DEFAULT_MS`), then picks
  the newest frame, warps and commits it. Measured with a warp every vsync (head still, `yaw=0.3`): late
  commits per 2 s with a 3 ms lead 51-143, 4 ms 60-99, 6 ms 0-9, 7 and 8 ms none; vrcompositor at 70-72 fps in
  every case. The warp's pose is ~6 ms fresher, and a frame done early in the period reaches the panels a vsync
  sooner.

The display time is now 18 ms after the vsync before the commit (the middle of the panel's scan is ~21 ms on
paper), fixed: a SteamVR setting for it existed for one day, the user wants no such knob (2026-11-08). The pose
age the driver's prediction follows uses the same value. `delay=` in `/tmp/quest1-reproj` overrides it for tests,
`lead=` sets the tick lead.
A frame whose render-to-display rotation exceeds 45 degrees is shown unwarped (worn, two such frames, 112 and 142
degrees, while Meta's tracking had lost the position: a render pose from another frame of reference).

Left: for ~10 s after each put-on every commit blocks ~10.7 ms in `MSMFB_ATOMIC_COMMIT` (until the next vsync), so
every frame is a vsync late until it stops (seen three times worn, not with the panels forced on).

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

## Panel calibration data and the brightness

Read-only, in `/persist/calibration/display/{left,right}/` (file names carry the panel serials: never
copy them into the repo):
- `<serial>.uniformity` (96 bytes, magic "UNIF", then 23 little-endian floats): factory colorimetry of the
  panel at full drive. [0..8] its RGB -> XYZ matrix (rows X, Y, Z, linear light; the white it gives matches the
  chromaticity stored further on), [9..11] its white luminance (~98.4 cd/m2 for both panels), then CIE xy
  chromaticities. The two whites differ slightly (xy 0.2931/0.3138 against 0.2949/0.3199).
- `<serial>.mura` (21-byte header "mURA", "gr", a float scale ~0.157, width 1440, height 1600, then
  1440x1600x3 bytes centred on 127) and `.mura.astc` (the same as an ASTC texture): a per-pixel correction
  map. Meta's boot animation shader (`/system/bin/vr_bootanimation`, GLSL in the binary) applies it in
  gamma-encoded space: `out += (mura - 0.5) * clamp(out * MURA_CLAMP_SCALE, MURA_CLAMP_MIN, MURA_CLAMP_MAX) *
  MuraScale` (sRGB output converted with pow(1/2.2) around it). Not applied: it needs a GPU pass over every
  frame, and frames shown without re-projection go to the panels untouched (zero copy).
- `{left,right}_screen_offset.json`: per-panel lens-to-panel alignment (`decenter` in pixels, about
  2.5-3.9 px x and -4.8 px y; `roll` 0.02-0.056 degrees). Not applied: Meta's sign and axis
  conventions for it are unknown, and the effect is ~0.3 % of the panel width.

Brightness (2026-11-08). The Samsung dual-DSI panel (`Dual Samsung video mode dsi panel`) uses
`bl_ctrl_dcs` with `qcom,cmd-sync-wait-broadcast`: one `lcd-backlight` level drives both panels, but at low
levels the two OLEDs answer it differently (user: the right eye dimmer at reduced brightness). So the backlight
now stays at full (`QUEST1_BRIGHTNESS`, default 255) and Monado dims in the display engine instead: each DSI has
its own DSPP, whose PCC (polynomial colour correction) Monado programs per panel with `MSMFB_MDP_PP`
(`MDSS_PP_SPLIT_LEFT_ONLY` / `RIGHT_ONLY`; the fb's left half is the right eye; 1.0 = 0x8000). PCC works on
gamma-encoded values, so a brightness b (linear) is a coefficient b^(1/2.2). The same coefficients carry a
per-channel gain from the uniformity matrices: both panels are driven to the mean of their two whites (gains
left 0.972/0.981/0.957, right 0.984/0.976/1.000), so they match in colour and luminance. Applied by the MDP at
each commit: no GPU cost, also for frames shown without re-projection.

The brightness is SteamVR's own setting, as on the Steam Frame: driver_quest1 sets
`Prop_DisplaySupportsAnalogGain_Bool` (with min 0.03, max 1, UI min 0.2), so the dashboard's Video page shows
"Display Brightness" and Steam's quick menu its slider; both write `/settings/steamvr/analogGain` (the slider's
percent is 100 * gain^(1/2.2)). The driver writes the gain in percent of linear light (clamped to 2-100: SteamVR's
default gain is 1.3) to `/tmp/quest1-brightness`, which Monado polls twice a second. Tested through the
dashboard's slider (CEF remote debugging): 47 % on the slider -> gain 0.196 -> PCC 0.476.


## Notes moved from the to-do list (2026-11-08): Display

History of measurements, fixes and open points as they were written in docs/todo.md (the to-do list now only
lists what is left to do).

- Apply the per-panel calibration from `/persist/calibration/display` (decenter, roll, mura).
- 90 Hz mode, selectable from the headset refresh rate setting (the Frame's quick menu / SteamVR's
  setting) (low priority). Base: github.com/TheCez/quest1-90hz-orangefox.

- Steam's VR UI: the right controller's Oculus button used the menu button's texture (SteamVR's
  Quest render model, user 2026-11-03). Fixed 2026-11-07: src/steamvr-quest1/mkrendermodel.py copies the model
  into driver_quest1's resources with the Quest 2 controller's logo on that button (holo-steamvr.sh runs it).
  Checked through IVRRenderModels; to look at in the headset (user).
