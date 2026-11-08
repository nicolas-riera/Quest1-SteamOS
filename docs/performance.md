# Performance

Where the frame time goes and what was tried. Current numbers and the direct scanout path:
docs/direct-scanout.md; Steam's UI: docs/steam-gpu.md.

## 2026-11-08 (night): the Steam library at 72 fps, Steam's UI behind SteamVR

With Steam's library open in the dashboard SteamVR was at 57-60 fps (70 with Steam's CEF GPU process stopped). Three
causes, three fixes:

- **vrcompositor's vblank thread woke up late.** Its VulkanVblankThread (the one calling vkGetSwapchainCounterEXT)
  polls the vblank fence, then sleeps until the next vblank under SCHED_OTHER. With Steam's processes and Meta's
  tracking loading the CPUs (load ~11 on 8 cores) it woke up to 7 ms late; the RenderThread then aimed at the vblank
  after and a frame was missed (about one a second, frame trace). quest1_display.c makes that thread SCHED_RR 90 on its
  first call, and the layer's own vblank thread SCHED_RR 95 (SteamVR has CAP_SYS_NICE; its RenderThread is RR 99).
- **Steam's UI shared the CPUs with SteamVR.** The kernel's cgroup v2 has only the memory and pids controllers (4.4: no
  cpu, no io), so quest1_vkredirect (preloaded in the Steam client) gives every steamwebhelper process nice 10 and best-
  effort I/O priority 7 (`QUEST1_STEAM_UI_NICE`, 0 = off). Games are children of the steam client, not of
  steamwebhelper: they keep normal priorities. Steam's GPU work was already LOW priority (below).
- **The memory protections did nothing.** On 4.4 a cgroup's memory.low only protects it while all its ancestors are
  below their own memory.low (mem_cgroup_low()), and system.slice had none. holo-perf.sh now gives system.slice 8G, so
  the per-unit MemoryLow of SteamVR, the dashboard, Monado and Meta's tracking take effect and steam.service (none) is
  what goes to swap first.

Measured panels on, scene mode, library open: 71.6-72.1 fps (72.0 after a fresh boot, GPU 75 % busy: vrcompositor
~8.3 ms and Monado ~2.2 ms a frame). Still open: when Steam's main page animates, every repaint costs ~14 ms of GPU in
Steam's CEF (the "SteamVR" page is 1280x720 at device scale 1.5): 51-66 fps while it runs (70-72 for an animation in
the bar). RenderMode=Direct is not a way out for Steam: given to steam.service as a test, a GPU page fault in
steamwebhelper (`kgsl_iommu_fault_handler`, read translation fault) was followed ~20 s later by a whole-headset freeze
with nothing in the console log.

Scene mode also works on the 4x MSAA system layer now (the dashboard asks for it while Steam's VR UI loads, in the
OOBE and on the lock screen: `systemlayer-msaa-enable-override` in systemui.js). In scene mode the layer makes
vrcompositor's 4x colour/depth images, render passes and pipelines 1-sample (quest1_compat.c, `qs_scene_single_sample`):
the eyes render straight into Monado's 1-sample buffers, and the distortion pass that used to resolve the samples is
gone. Before, scene mode ran at 9 fps there with the controllers drawn wrong.

## 2026-11-08 (evening): vrcompositor's render thread realtime

vrcompositor asks for realtime priority for its RenderThread and SignalThread ("Error establishing real-time priority",
vrcompositor.txt): on desktop Linux SteamVR's launcher is setcap'd for it. The unit now gives SteamVR
`AmbientCapabilities=CAP_SYS_NICE`: both run SCHED_RR. Monado's warp thread (FIFO 99) keeps its timing (late 0, wake
max < 0.1 ms). Benchmark (scene mode, warm): 71.8 / 71.1 / 72.0 / 71.2 fps (71.8 / 71.2 / 70.9 / 70.7 before); the
frame trace of a quiet dashboard shows 218 presents for 217 vblanks.

## 2026-11-08 (evening): scene mode, the eyes render straight into Monado's buffers (`/etc/quest1-warp-lens` 2)

vrcompositor's distortion pass resampled each 1500x1664 eye render into a 1440x1600 panel image (~2 ms an eye, all
memory traffic: as much as an identity copy), which Monado's warp then resampled again. In scene mode the layer
gives the eye's render passes a twin framebuffer whose colour attachment is the eye's gralloc buffer (an sRGB view;
the buffers are made at the scene's size, cached in `~/.cache/quest1-scene-size`), drops the distortion pass, and
Monado's warp does the lens distortion, its chromatic aberration and the re-projection in one pass (its lens mode, the
geometry checked on 2026-11-03 for mode 1). vrcompositor acquires the frame's swapchain image only after recording
the first eye: the layer reserves the index when that eye's first pass begins and that acquire returns it. When no
index is free, the eye renders into vrcompositor's own image and is copied in (slow, ~4 ms an eye).

Benchmark as below (second run, warm):

| Case | Mode 0 | Scene mode |
| --- | --- | --- |
| Dashboard | 71.6 fps, GPU 8.55 ms | 71.8 fps, GPU 4.19 ms |
| Dashboard in use | 71.1 fps, 8.32 ms | 71.2 fps, 3.81 ms |
| Passthrough + dashboard | 71.7 fps, 8.11 ms | 70.9 fps, 3.39 ms |
| Passthrough + dashboard in use | 70.8 fps, 7.92 ms | 70.7 fps, 3.92 ms |

GPU = vrcompositor's per frame; Monado's warp goes from ~1.1 to ~2.1 ms a vsync. Total ~6 ms of the 13.9 instead of
~9.7: the headroom is for games, streaming and the desktop. The frame rate stays where vrcompositor's running start
puts it (render-thread CPU). Off by default until checked in the headset (sharpness, geometry, chromatic aberration).

## 2026-11-08 (afternoon): render passes merged, Steam's GPU work below vrcompositor's

Where the GPU goes (kgsl trace of every batch, `adreno_cmdbatch_submitted/retired`; per pass with
`/tmp/quest1-gputime`): ~88-92 % busy at 62-71 fps when the scene is heavy (a controller lying in front of a lens),
~70-80 % in the benchmark below. Per eye: the Aurora environment ~2.2 ms, a controller up to 0.8 ms, the dashboard
overlay ~0.5 ms, the distortion pass ~2 ms; Monado's warp ~1.1 ms a vsync. DDR and GPU bus votes are already at their
maximum (`soc:qcom,cpubw` / `gpubw` 13763).

- **Render passes merged** (`quest1_rpmerge.c`). vrcompositor draws every layer of an eye in a render pass of its own
  (~27 an eye), each loading and storing the whole 1500x1664 colour and depth images, with its uniforms rewritten by
  `vkCmdUpdateBuffer` between barriers that drain the GPU. The layer keeps the pass open across them: the updates go
  into CPU copies of the buffers, each draw reads them through a copy of its descriptor set pointing at a snapshot
  in the command buffer's staging, and the buffers get their final contents once the pass really ends. Snapshots
  identical with and without it. Measured: render thread 4.2 -> 3.1-3.8 ms a frame, GPU -0 to -0.5 ms a frame (the
  draws are fill and bandwidth bound, not drain bound: a test that dropped the uniform updates looked like -4 ms,
  but it drew every object at the same place). `nomerge` in `/tmp/quest1-layer` turns it off.
- **Steam's CEF at LOW GPU priority.** steamwebhelper's GPU process (Chromium compositing, e.g. a repaint when the
  laser hovers the dashboard) and its main process (the VR pages through vrclient's Vulkan renderer) submitted batches
  of 30-40 ms at kgsl priority 8, on vrcompositor's ring buffer: a frame lost each time. They now ask for
  `VK_QUEUE_GLOBAL_PRIORITY_LOW` (kgsl 12, the ring below, which vrcompositor's preempts): the compat layer in the
  processes that load it, `quest1_vkshim` in the GPU process (it loads no layer). Monado (root) stays above both.

Benchmark (`bench4.sh`-style: dashboard shown / in use = fake pointer sweeping + Steam switching pages, with and
without passthrough), after both: 71.3 / 69.8 / 70.7 / 70.2 fps, render thread 3.1-3.8 ms, GPU 9.6-10.7 ms a frame.

Why not 72 with GPU to spare: vrcompositor uses a running start. It starts recording ~4.4 ms before the vblank and
presents ~2.6 ms before it (the GPU then renders through the next period). A frame whose CPU part runs late
presents after the vblank and that vblank gets no new frame (`/tmp/quest1-frametrace`, below). Its render-thread
time is the lever.

Tried and dropped:

- **Scene mode with a copy** (the layer copying each eye's render into Monado's buffer): the copy into the UBWC
  gralloc buffer costs ~4.4 ms an eye, 15 fps. Replaced by the zero-copy scene mode above (the copy stays as its
  fallback).
- **Vblank event fences signalled from the CPU** (the layer answering the waits): present-to-rendered 17 -> 14 ms,
  but 69.6 against 71.0 fps (alternated A/B): with less slack, the running start misses more vblanks.
- **Anisotropic filtering** (vrcompositor asks for 16x on most samplers): capped to 1x, no difference.

Debug: `runuser -u steamos -- touch /tmp/quest1-frametrace` records 3 s of vrcompositor's frame loop (vblank ticks,
display events, fence waits, acquires, submits, presents, with their thread) in `/tmp/quest1-frametrace.txt`.

## 2026-11-08: vrcompositor's CPU and GPU now overlap (55 -> 68-72 fps)

vrcompositor's render thread read its GPU timestamps with `vkGetQueryPoolResults(..., VK_QUERY_RESULT_WAIT_BIT)` right
after submitting each frame. The blob implements the wait as a `usleep` loop, so the thread slept until the GPU had
finished the frame (55 % of its time in `nanosleep`, sampled through /proc wchan and eu-stack) and only then recorded
the next one: ~5 ms of CPU and ~12 ms of GPU in series, 55 fps. The timestamps only feed its frame-timing statistics,
so the compat layer (`quest1_compat.c`, `GetQueryPoolResults`) returns at once: the results when ready, else the last
ones read for the same queries, while a helper thread (`q1-queryresults`) waits for the real ones for the next call.
`qrwait` in `/tmp/quest1-layer` restores the plain wait. No visible change in snapshots; vrcompositor rotates several
command buffers, so recording the next frame while the GPU runs the previous one is safe.

Measured on the table, panels forced on, both controllers seen by the cameras and drawn, dashboard shown (`bench4.sh`
style: the dashboard "in use" = both controllers emulated in front of the head, the right laser sweeping the panel and
its stick scrolling, plus Steam switching pages every 1.2 s):

| Case | Before | After |
| --- | --- | --- |
| Dashboard | 56.0 fps | 71.7 fps |
| Dashboard in use | 55.2 fps | 68.5 fps (min 66.4) |
| Passthrough + dashboard | - | 71.8 fps |
| Passthrough + dashboard in use | - | 68.6 fps |

GPU per frame: ~11.7-11.9 ms with the Aurora environment, ~8 ms with passthrough (it replaces the environment).
Of the 11.8 ms: vrcompositor's distortion pass ~2 ms per eye (1440x1600), the Aurora environment ~4 ms, the two
controller models ~2 ms, the dashboard overlays (4x MSAA system layer) the rest. In use, the render thread takes
5.6-6.3 ms a frame (Steam's page changes and pointer events), on top of Steam's own load on the big cores.


## Notes moved from the to-do list (2026-11-08): Performance

History of measurements, fixes and open points as they were written in docs/todo.md (the to-do list now only
lists what is left to do).

- Target (user, 2026-11-05): 72 fps with a usable Steam (the dashboard is very slow), with headroom left
  for streaming and desktop mode. Fewer translation/emulation layers is an avenue to weigh.
- 2026-11-06 night: 72 fps with the dashboard shown, head still, passthrough on (was 53): vkCmdUpdateBuffer
  staging, UBWC scanout buffers, 4 swapchain images (docs/direct-scanout.md). Measured on a table with the panels
  forced on; to confirm worn, with the controllers in view (they cost ~5 ms of GPU a frame, see below) and
  moving the head (Monado's warp). Next: the LSE trap path (do_undefinstr takes a global spinlock per trap and
  emulate_atomic does two exclusive loops: ~18 % of vrcompositor's render thread), the passthrough mesh pass
  (~1.4 ms per eye), the controller models.
- 2026-11-07: worn it was 37-46 fps (user: "un peu mieux"): ~18 ms of GPU with the controllers in view, each of
  their parts in a render pass of its own. vrcompositor now renders straight to memory (RenderMode=Direct, SteamVR
  only): 14.1 -> 9.3 ms a frame on the table, 72 fps with the dashboard. To measure worn again. The controller
  models stay as they are (user: no visible change); the ovrstub log flood (~850 kernel log lines a second with the
  controllers on) is gone.
- Memory (user, 2026-11-05: "faut bien gérer ce problème de saturation"): right after boot, with
  SteamVR up, only ~500 MB is available and ~550 MB is already swapped (zram). Per process (Pss + swap
  + KGSL): Steam's webhelpers ~1.65 GB (two of them with 270 + 185 MB of GPU memory), vrcompositor
  ~610 MB (470 MB GPU), vrwebhelper (dashboard) ~560 MB (274 MB GPU), steam ~130 MB; KGSL total
  1.26 GB. After ~12 stack restarts in one boot the headset fell to 12-15 fps (kswapd 35 %, GPU trace
  buffer allocation failing): reboot rather than restart the stack many times. Leads: Steam's
  gamepadui window rendered in Xvfb for nobody, CEF memory flags, vrcompositor's textures,
  extra_free_kbytes so kswapd starts before allocations stall.
  2026-11-05 evening: holo.img's loop device now uses direct I/O (335 MB of double page cache gone), and
  vrclient's 256 MB transfer ring (one per OpenVR Vulkan process: vrcompositor, the dashboard, Steam's
  webhelper, q1desktop, every game) is backed on first CPU touch (kernel/kgsl-lazy.patch). Next: zram
  with zstd/lz4 instead of lzo, Steam's webhelper ring (fully used: its CPU-drawn pages), the dashboard's
  and Steam's CEF processes.
- vrcompositor, head still, panels on (2026-11-02): 22-28 fps in the morning, 36 with Monado's "early"
  ack, 45 with the layer's present waits returning at once. The limit is GPU time: the kgsl trace
  (adreno_cmdbatch_retired start/retire ticks) shows vrcompositor's one batch per frame running
  ~17.8 ms (gpu_busy_percentage under-reports, ~40 %). The layer's profiler (`touch
  /tmp/quest1-gputime`) splits it per eye: 12 scene passes at 1500x1664 (~5.6 ms, the controller
  models ~1.5 ms of it) and the distortion pass at 1440x1600 (~3.2 ms). Tried without useful gain:
  merging the scene passes (same time merged), dropping the distortion draws (-3 to -4 ms only),
  GpuSpeed.gpuSpeedRenderTargetScale (the scene stays 1500x1664), live vrcmd settings
  (enableSimplifiedShaders, overlay quality, skydome, supersampleScale: < 1 ms), SteamVR's realtime
  priority, GPU bus floors, Steam on the little cores. Left: lighter controller models, Facet, or
  ~45 fps with a clean 72 Hz re-projection. Never lower the resolution (user: it is already low). Tracking on all 8
  cores (it is pinned to the busy little cores) seemed to make the shaking worse: back to 0-3.
- SteamVR's Facet renderer (tip from a user, 2026-11-02) runs and draws correctly behind
  `/etc/quest1-facet` (off by default, `src/vklayer-steamvr/quest1_facet.c`): ~29-32 fps, eye passes
  6-7 ms each. Needs a fair A/B against the old renderer in one quiet window
  (`/root/facet-iter2.sh 0|1 <dir>`), with the dashboard shown (dropped stencil dynamic states).
  Leave it off unless it wins.
- Light, safe overclocks (user, 2026-11-03): GPU (A540 max 710 MHz on the Quest 1) and CPU OPPs a
  step above stock, only within ranges other MSM8998 devices run stable, watching tsens and the
  fan. Kernel DT/OPP tables, so a new boot image (fastboot boot only).

- User, 2026-11-02 (after the present-wait, UBWC warp and memory changes): "no more shaking, much
  better perf", but controllers are still not smooth (vrcompositor's frame rate), and the dashboard
  and Steam's UI lag.
- The controllers cost ~10 fps (2026-11-03, headset on a table, dashboard shown, no re-projection):
  ~46 fps while SteamVR draws no controller, 34-36 fps with both drawn (vrcompositor's GPU batch
  20 -> 25-27 ms). The layer's profiler shows each visible controller as a ~0.6 ms pass (9792
  vertices, the body) plus 6 small passes (~0.1 ms each, the buttons), per eye: ~5 ms a frame.
  Monado's build, achromatic distortion and the vm settings made no difference (bench in
  /root/bench-20261103/). Next: why each render model component is its own render pass, and a
  lighter render model (could also fix the Oculus button texture below).
- Frame rate drops while the controllers move fast, until they stop (user, 2026-11-03): Meta's
  controller tracking threads (CtrllrCam, CtrlTracking, SCHED_FIFO on cores 0-3) work harder. In
  part (only in part) the laser pointer passing over Steam UI elements: their hover animations cost
  frames (user).
- Re-projection off when the headset is put on, then swinging slowly into place while fresh frames
  sit right (user, 2026-11-03): Monado's warp averaged the device-to-LOCAL rotation `o` at 5 % a
  frame and never reset it, but driver_quest1's LOCAL space is created at the head's yaw with each
  SteamVR start. Now seeded again per layer connection and when far for 8 frames (comp_window_mdp.c);
  to confirm in the headset after a SteamVR restart.
- Slim the install: drop what this headset never uses (other controllers' render models, SteamVR
  Home if present, etc.): less storage, memory and load time (user, 2026-11-03).
- Steam's UI and the dashboard lag, and so does Steam's VR keyboard (it is Steam's: it types fine,
  the lag is Steam's UI). Faster Steam fixes both (user, 2026-11-03).
  Measured 2026-11-06 (CDP, arrow keys every 150 ms on the library home): requestAnimationFrame falls from 60 to
  ~9 fps (median frame 100 ms, max ~470 ms). The renderer's main thread runs ~80 % of a big core (libcef + V8:
  the gamepadui's own work), the GPU process's VizCompositorThread ~70 % (half of it memcpy: the off-screen
  frames copied for the VR overlay), the browser process uploads them with Mesa GL (libgallium). Steam itself
  pins its webhelpers to CPUs 2-6 (two little, three big cores; `STEAM_DESKTOP_APP_AFFINITY_LIST` is read but
  unset), though the hot threads ran on the big ones. Leads: fewer copies on the overlay path (shared textures
  instead of the CPU fallback), less UI work (animations, blur).
- LSE atomics emulation (kernel/lse_emul.c): ~100,000 traps/s while SteamVR runs (vrserver ~38k,
  vrcompositor ~29k, steamwebhelper ~24k), 0.8 us each on a big core and 1.6 us on a little one,
  against ~20 ns native: ~10-15 % of one core. Make the emulation itself faster (trap path, hook
  lookup, decode). Not binary rewriting: patched Valve binaries break with Steam updates and
  can't be shipped (user, 2026-11-03).
- The kernel logs "SPI transaction rejected by SyncBoss" / "Bad magic number 0xcacacaca" several
  times a minute (2026-11-03): check against Android, and whether IMU or controller data is lost.
- Lens distortion in Monado's warp (`/etc/quest1-warp-lens` = 1, off by default): vrcompositor hands
  over undistorted eyes, Monado does distortion, chromatic aberration and re-projection in one pass
  like Meta's TimeWarp. Geometry right since Meta's lens centre (2026-11-03), but vrcompositor's
  distortion pass costs the same as a copy (3.6 ms per eye, bandwidth) and the warp then runs at
  every vsync with 3 samples: more GPU in total. Compare sharpness with the user once perf allows.
