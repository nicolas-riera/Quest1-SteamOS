# Performance

Where the frame time goes and what was tried. Current numbers and the direct scanout path:
docs/direct-scanout.md; Steam's UI: docs/steam-gpu.md.

## 2026-11-10: headset not worn: vrcompositor at 9 frames a second instead of 72

With the headset on the table (panels off) vrcompositor went on rendering 72 frames a second that Monado never
showed ("frames received 72.0/s ... never shown"): 32-47 % of a core for its render thread, the GPU's work on top,
vrserver at 19-24 %, for nobody. The compat layer now holds vrcompositor's present (`qd_QueuePresentKHR`) so
that presents are 8 refresh periods apart while Monado's `/tmp/quest1-panels-off` exists (not worn, or standby):
the rate Monado already gave OpenXR apps then (`MDP_IDLE_PERIOD_NS`).

Measured without wearing it (`QUEST1_IGNORE_PROX=1`, worn or not with `/run/quest1-standby`): not worn 9.0 fps,
vrcompositor out of the five busiest processes, vrserver 13 %, the GPU at 257 MHz and 0 % busy; worn again 71.8 fps
at 13.6 ms a frame as before, one late commit at the moment the panels come on, none after; three round trips, no
crash. Meta's tracking stays the big idle cost (trackingservice 70-110 % of a core worn or not: docs/todo.md,
"tracking stays off until the headset is worn").

Also: the dashboard watcher (`vrdashboard-run`) ran two `pgrep` every 5 s (50 ms of CPU each with 600 tasks); it
now looks the two processes up again only when the one it found is gone.

Looked at and dropped: the idea that Steam and the 2D apps crawl on the little cores. New and bursty tasks do go
to the big cores (a Python burst test ran on CPUs 4-7 every time), and steamwebhelper is already pinned to 2-6.

## 2026-11-09 (afternoon): image jumps in bursts: the panels run at 71.82 Hz, not 72

The user, worn: no real trembling but image jumps, sometimes several in a row. Monado now counts, per 2 s, the frames
it receives from the layer, those shown a period late, those never shown, and when they came after its tick (2 ms
buckets): `frames received ...` lines next to the `direct:` ones.

- **Threads on the frame's path real-time** (the night's leftover, now built and measured): the layer's submission,
  completion, present and scanout fence threads (SCHED_RR 50, vrcompositor only), driver_quest1's display and frame
  threads (SCHED_RR 50) and Monado's frame receiver "Quest1 direct" (SCHED_FIFO 53). Under the Steam load: fresh
  frames 60.6 -> 68.4/s, vrcompositor 69.5 -> 71.5 fps, 1 late commit in 180 s.
- **The cause of the bursts**: the panels' vsync events come every 13.924 ms (71.82 Hz, measured on
  `/sys/class/graphics/fb0/vsync_event`), while the layer ticked vrcompositor's vblanks on a 72 Hz grid of its own and
  the driver declared 72 Hz. vrcompositor's frames slid across Monado's tick (every ~5.5 s), and around each crossing
  two came in one period and none in the next: one frame skipped, the other shown twice, several times in a row.
  vrcompositor presented every 13.45 ms (frame trace), then waited 21 ms, over and over: it times its frames on
  `GetTimeSinceLastVsync`, which came from the OpenXR frame loop's wake-ups.
  Now Monado's vsync thread publishes the real vsync (time, period averaged over ~64 vsyncs, count) in a shared page,
  `/run/monado/quest1_vsync` (`struct q1d_vsync`, quest1_direct_proto.h); the layer ticks the vblank fences on it, and
  driver_quest1 answers `GetTimeSinceLastVsync` from it and sets `Prop_DisplayFrequency_Float` to the measured rate.
  Both fall back to their own clock without it (no Monado, panels off). vrcompositor then presents every ~13.9 ms.
- **Frames never skipped**: vrcompositor needs ~13.2 ms of the 13.9 ms period for a frame, so its frames still come at
  a phase that wanders with its load, and two can land in one period of Monado's tick. Offsets of its vblank against
  the vsync (-1 to -7 ms) gave no stable phase (the same offset: 269 then 1506 frames late in 30 s). Monado now keeps
  one frame waiting (`pend`): when a frame comes before the previous one was shown, the next tick shows the older one
  and the one after shows the newer. Each frame is shown once, at most a period late (the warp still re-projects it to
  the display's head pose), and the queue empties when vrcompositor misses a vsync, without a repeat. Worn
  (user's session, ~100 s): 0 frames never shown, 0 late commits.

- **Late commits again, worn** (the user: the whole image jumps as soon as the headset is loaded): ~70 late commits
  in 25 minutes with Steam and the desktop in use, each the whole image shown twice. The warp's GPU work waited up
  to 8.7 ms behind vrcompositor's and Steam's, and a commit made after ~vsync + 8 ms blocks in the kernel until the
  vsync after (up to 6.4 ms in `MSMFB_ATOMIC_COMMIT`): with the tick at vsync + 3.9 ms (lead 10) the warp had ~4 ms of
  margin. The tick now comes 12.5 ms before the latching vsync (~1.4 ms after the previous one): 0 late commits under
  the Steam load. Leads of 7-8 ms made it worse (17 late a minute).
- **The warp's head prediction**: Monado logs, once their time has passed, how far the orientations the warp predicted
  were from the ones tracked then (`warp prediction error` lines, and a line for each one over 0.5 deg). With the
  angular velocity alone, 20 ms ahead (the tick ~1.4 ms after a vsync, the frame lit at vsync + 20.9 ms): 0.07-0.28 deg
  on average while the head moved, over 0.5 deg up to 10 times in 2 s (~7 px, a different error at each vsync). 60 s
  of the user's head poses recorded from q1trackd and replayed offline through several predictors: the gyro's change
  over the last 10 ms as angular acceleration (capped at 60 rad/s^2) cut the error from 0.106 to 0.060 deg on average
  and the vsyncs over 0.5 deg from 1.2 to 0.26 % (libossdk's own acceleration did a little worse, a smoothed velocity
  worse still). Measured live next to the old one before switching (worn): 2-3 times less error. quest1_hmd.c uses it
  past the newest pose (the warp and Monado's clients). The first try integrated the base-frame velocity as a
  body-frame one (`math_quat_integrate_velocity` wants body frame): the axes were wrong and the user felt sick within
  seconds; reverted at once, then redone and measured in parallel first.
- **The pose vrcompositor renders with**: at each present the layer asks the driver to publish the next frame's pose
  (QD_POSE) and waits for its reply (QD_POSED) before returning, at most 4 ms. When the wait ran out, vrcompositor
  rendered that frame with the previous pose while the layer told Monado the new one: the warp re-projected it from the
  wrong pose, a frame of head motion off (over a degree at a normal head speed), the whole image trembling with the
  head's motion and more under load. The layer now counts it (`pose wait ... ran out N` in its 5 s line): 30-45 times in
  5 s even idle. Causes and fixes: the layer's thread receiving the driver's replies was a normal thread (now SCHED_RR
  50, 9-15 -> 0-7); Monado's IPC thread answering the driver's `xrLocateSpace` was a normal thread (now SCHED_FIFO 51);
  the Monado client library in vrserver dated from 2026-10-06, without the priority-inheriting mutexes: the driver's
  real-time thread waited up to 72 ms behind a normal thread (controllers, camera) holding the IPC connection under
  load (rebuilt: `xrLocateSpace` 0.24 ms average, 3.3 ms at most). The wait is now 8 ms (`QD_POSE_WAIT_NS`): a rare
  late reply costs a late frame (content repeated, rotation right) instead of a wrong warp. Worn with load: the user
  saw no more trembling (2026-11-09 evening).
- Tried and dropped: a phase loop moving vrcompositor's vblanks so that its frames reach Monado mid-period. Its frames
  did not follow (12 ms of offset, no change in their arrival): vrcompositor is not paced by its vblank there; its
  render thread uses ~36 % of a core and the GPU ~42 %, its 13.2 ms a frame are mostly waiting in series.

Left: the frames vrcompositor itself does not render (its fps below 71.8: ~69-71 worn) show as a repeat; activating
passthrough stalls it ~0.3 s (16:44:02, 51 frames/s over 2 s, CTrackedCamera's "Late initialization"). More slack in
its frame time is the lever (GPU-bound, see below).

## 2026-11-09 (night): the view trembled while the headset was loaded: frames shown twice

The user: the view still trembles now and then while Steam works, "as if the previous frame were shown again". That
is what a late commit does: Monado's warp for a vsync is committed after that vsync, so the panels show the previous
warped image a second time (a one-frame jump while the head moves).

Measured without the user (panels forced on with a volatile `QUEST1_IGNORE_PROX=1` drop-in, dashboard shown), 20 s idle
then 60-240 s of Steam load (its VR page repainted on every animation frame through CEF remote debugging, and switched
between games/store/library every 2 s), from Monado's `direct_tick` lines and a watcher of q1trackd's shared memory.
Before: 6 late commits a minute (none idle), and q1trackd's 2 ms pose updates up to 28 ms apart (36 gaps over 8 ms a
minute, up to 12 ms even idle). Fixed, in order:

- **Poses**: q1trackd's publishing thread and Monado's "Quest1 Meta trk" thread (which copies q1trackd's poses into
  the head pose history the warp predicts from) ran as normal threads. Both are SCHED_FIFO 52 now, above Meta's own
  tracking threads (48-51) and SteamVR's (50 at most): updates at most 2.9 ms apart under the load.
- **Where each late commit's time went**: a log line per late commit (`late commit, done at vsync + ...`, 10 per 10 s
  at most): when the tick woke, the warp's mesh, GPU wait and GPU time, the commit ioctl, and the stage times. It
  showed three causes besides the GPU:
  - an `unlink()` of the snapshot trigger in /tmp on every tick took 4-5 ms now and then. All the files the vsync
    thread read or wrote (test knobs, snapshot trigger, standby and brightness flags, the pose age for driver_quest1)
    are now handled by a normal thread of their own ("Quest1 files", every 100 ms); the vsync thread reads memory.
  - 2-4 ms before the warp started: the head pose history's mutex, shared with Monado's normal IPC threads. Every
    Monado mutex (`os_mutex`, the thread helpers') now has priority inheritance.
  - a commit made in the last ~3.5 ms before the vsync blocks in `MSMFB_ATOMIC_COMMIT` until that vsync.
  After these: 0 late in 120 s, 1 in 240 s.
- **The GPU**: the warp's 2.5-3 ms of GPU work sometimes waited 7-12 ms (vrcompositor's and Steam's draws are
  preempted only at their end; every context already uses fine-grained preemption, Monado has the highest ring).
  kgsl's dispatcher kthread at FIFO 96 instead of 2 changed nothing (not kept). The tick now starts 10 ms before the
  latching vsync instead of 8 (`MDP_TICK_LEAD_DEFAULT_MS`): no late commit in 150 + 240 s of the load; the warp
  extrapolates 2 ms further (libossdk's prediction: 0.04 deg p90 at 26 ms).

Left: vrcompositor runs ~70 fps under the load but only ~61 of its frames a second reach the warp fresh (68 idle):
a frame that reaches Monado after the tick is shown a vsync later or replaced. The head's rotation stays smooth (every
vsync is warped), moving content and position jump. Followed up the next afternoon (section above).

## 2026-11-08 (night): Steam's VR UI ran the RAM out and froze the headset; its GPU queue, its readback memory

Measured without the user, with a repeatable load: Steam's CEF remote debugging (`.cef-enable-remote-debugging` in
Steam's folder, port 8080 on localhost, removed afterwards) and a nearly transparent full-page layer on the VR main page
(`valve.steam.gamepadui.main`) changing colour on every animation frame, so Steam repaints and sends its whole page all
the time; also a 64 px square (the cost turned out the same: a fixed cost per frame).

Where the time went, before:
- Steam's VR page repainted at ~8 frames a second, the CPUs were not saturated (320 % of 800 %) and its VR thread
  (CVRAPIITask, vrclient's GL fallback) took ~5 %: not the CPU copy chain.
- Its CEF GPU process: ~20-27 ms of GPU per frame, on the low-priority ring that vrcompositor preempts (each batch took
  40-220 ms of wall time), and 17-20 batches always waited in the kernel queue (kgsl `ctx/<id>`: queued minus retired),
  i.e. several frames: Steam reacted seconds after the laser.
- Its VizCompositorThread spent ~36 ms of CPU a frame in one memcpy: the readback of each 1920x1080 frame out of memory
  the Adreno blob reports as HOST_CACHED|HOST_COHERENT but maps write-combined (test_readback: 221 MB/s CPU reads; its
  HOST_CACHED non-coherent type reads at 4 GB/s).
- Its GPU memory: every frame allocated ~2.7 new 12 MB textures (Skia's scratch textures, with the 192 MB GPU memory
  budget the launcher gave Chromium) plus an 8 MB readback buffer, 380 MB/s of kgsl allocations (zeroed, mapped, cache
  flushed, freed later), and ANGLE frees them only once the GPU is done with them: with 17-20 batches pending, the
  process's kgsl memory went from ~250 to ~1000 MB within seconds of a repaint starting, kswapd ran at 50 %, and
  vrcompositor dropped to 38-69 fps.

**The freezes.** That is what froze the headset twice this night (and once before, right after a Steam restart): the
RAM ran out, and every process that needed a page from the disk waited for minutes. The new q1-hangwatch
(device/q1-hangwatch.py) showed it in the kernel log kept by pstore (read after a reboot from an Android `adb
bugreport`, LAST KMSG): sshd, steam, vrdashboard, systemd-userwork in D state on `filemap_fault`, `__wait_on_buffer`,
`jbd2_journal_get_write_access`, swap out at ~3400 pages a second just before. The kernel and Monado (whose pages were
resident) kept running; when the panels came on, Monado's thread stopped too.

Fixes:
- **A GPU queue limit in Steam's GPU process** (quest1_wsi_x11.c, `QUEST1_VKSHIM_MAX_INFLIGHT=4`): each submission is
  followed by an empty one with a fence of the shim's; with 4 in flight the next submission waits for the oldest.
  Chromium then draws as fast as the GPU finishes. 2, 4 and 8 gave 9-13 fps alike; 4 kept.
- **Really cached readback memory** (quest1_vkshim.c, `QUEST1_VKSHIM_CACHED_READBACK=1`): the "cached coherent" type
  is reported without HOST_CACHED and buffers that may use it may use the cached non-coherent one, which ANGLE then
  takes for its readbacks (it flushes and invalidates non-coherent memory). VizCompositorThread 39-80 % -> 13-15 %.
- **Chromium's GPU memory budget 384 MB** instead of 192 (the launcher): under one new full-page texture a frame.
- **Block devices tuned like Android does on this headset** (holo-perf.sh, quest1-io.service): CFQ without idling
  (slice_idle and group_idle 0), read-ahead 128 KiB instead of 512, dirty pages written back from 5 % and after 2 s.

After, same load: Steam 9-13 fps (avg ~10, small or full damage alike), its kgsl memory flat at 260-290 MB, 1-4 batches
pending, MemAvailable steady (~600 MB), no kswapd in the top threads, vrcompositor 71.4-72.0 fps for 5 minutes on end,
no GPU fault, no GPU process restart. Steam's CPU: ~1 core in all for its processes.

What limits Steam now is the GPU: 100 % busy (vrcompositor ~9.5 ms a frame, Monado ~2.2, Steam's ~24 ms per frame in the
gaps). That fixed cost per frame (any damage) is the full composite of the page plus its capture and readback; the
next lever (docs/todo.md).

On the way: one GPU hang on Steam's ring (`RB[2]: gpu fault`, kgsl snapshot and recovery, Chromium's watchdog restarted
its GPU process) happened under memory pressure before the queue limit, with the cached readback on; not seen again in
the 5-minute soak with both. Also found: at some boots udev's helpers hang for 3 minutes and are killed
(`libinput-device-group` on the power key's event0, `scsi_id` on UFS LUNs), the power key stayed root 0600 and
quest1-powerbutton restarted every 2 s for the whole boot (fixed: its unit gives the event nodes udev's default group).

## 2026-11-08 (late night): the view trembled while Steam worked; real-time priorities; Steam's overlay path

Worn, the view trembled as soon as Steam was touched (and in desktop mode), with half-second freezes. Two causes in the
display path, both fixed, and a third that is Steam's own:

- **A chain of late commits.** Monado's `direct_tick` lines showed stretches of 2-10 s where every commit was late
  ("late 144" per 2 s) and MSMFB_ATOMIC_COMMIT took ~5 ms instead of 0.1. Kernel stacks sampled during them (a
  recorder reading `/proc/<tid>/stack` at ~1 kHz): Monado's vsync thread in `mdss_fb_pan_idle`, the MDP's display
  thread (`mdss_fb0`) in `mdss_mdp_video_wait4comp`. Once one commit reaches the display thread too late for its vsync,
  that thread waits for the vsync after, the next commit waits for it in pan_idle and is taken at that vsync, too late
  again, and so on while the head moves (a warp at every tick). Each frame was shown a vsync after the time its warp
  was made for. Monado now skips the tick after a commit that waited in the kernel (> 1 ms) and came late
  (`commit_late`): one image comes late, the next tick is on time (worn: "late 7 (ticks skipped after one 7)").
  A forced late commit that does not wait in the kernel (lead=2) does not chain and is not skipped.
- **Real-time priorities upside down.** Monado's vsync thread (FIFO 40) woke up to 6 ms late behind vrcompositor's
  RenderThread (SCHED_RR 99, several ms of CPU a frame on cores 4-6) and Meta's ELEVATED tracking threads (FIFO 48,
  big cores); the kernel's display thread was at FIFO 16, below all of them. Now: Monado's vsync thread FIFO 98, the
  display kernel thread FIFO 97 (Monado sets it after each unblank, `boost_display_kthread`), and the compat layer
  brings SteamVR's real-time threads down to 50 (`q1_rt_cap`, from the Vulkan calls they make: RenderThread,
  q1-queryresults, the vblank threads; SignalThread stays at 99 but only sleeps). Worn afterwards: 71.8 warps/s,
  late ~0, wake max 0.1-0.4 ms.
- **Steam's CPU priority.** At nice 10 (previous section) Steam's threads waited for a CPU longer than they ran (2.4 s
  against 1.4 s over 20 s of browsing; 1.0 s at nice 0, SteamVR still at 71.6-71.8 fps). Steam's UI is back at nice 0;
  quest1_vkredirect keeps only the lowest best-effort I/O priority.

What is left is Steam's VR overlay path: while Steam works, SteamVR drops to 59-69 fps (Monado then re-projects the
rotation only: moving objects and the controllers judder) and Steam itself shows a few frames a second, seconds behind.
Its VR UI pages are 1920x1080 textures (fixed in Steam's code: 1280x720 CSS pixels at device scale 1.5, no setting)
and each frame goes through the CPU: the GPU process reads it back (VizCompositorThread), the browser uploads it into
an OpenGL texture on Mesa llvmpipe, vrclient's GL fallback reads it back (`MESA_EXTENSION_OVERRIDE` hides
GL_EXT_memory_object, since llvmpipe can't import the Adreno's memory) into its 256 MB ring ("Ring Buffer Wrapped!"
every 5-10 s while browsing: ~50 MB/s, about 6 frames a second), and vrcompositor's RenderThread copies it in. Measured
worn while browsing: load 13-16 on 8 cores, GPU 100 % busy, 40-110 MB/s swapped out, vrcompositor's RenderThread
at about a full core (~4 ms of CPU a frame at rest). Next: a zero-copy overlay path (docs/todo.md). (Measured the next
night: the CPU copies were not the limit; Steam's GPU queue and memory were, section above.)

## 2026-11-08 (night): the Steam library at 72 fps, Steam's UI behind SteamVR

With Steam's library open in the dashboard SteamVR was at 57-60 fps (70 with Steam's CEF GPU process stopped). Three
causes, three fixes:

- **vrcompositor's vblank thread woke up late.** Its VulkanVblankThread (the one calling vkGetSwapchainCounterEXT)
  polls the vblank fence, then sleeps until the next vblank under SCHED_OTHER. With Steam's processes and Meta's
  tracking loading the CPUs (load ~11 on 8 cores) it woke up to 7 ms late; the RenderThread then aimed at the vblank
  after and a frame was missed (about one a second, frame trace). quest1_display.c makes that thread SCHED_RR 90 on its
  first call, and the layer's own vblank thread SCHED_RR 95 (SteamVR has CAP_SYS_NICE; its RenderThread is RR 99).
  (Both at 50 since: section above.)
- **Steam's UI shared the CPUs with SteamVR.** The kernel's cgroup v2 has only the memory and pids controllers (4.4: no
  cpu, no io), so quest1_vkredirect (preloaded in the Steam client) gave every steamwebhelper process nice 10 and best-
  effort I/O priority 7. Games are children of the steam client, not of steamwebhelper: they keep normal priorities.
  Steam's GPU work was already LOW priority (below). (The nice 10 made Steam unusable and is gone: section above.)
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

## Trembling: how to watch for it (2026-11-09)

The user saw no trembling or image jumps left, worn and with load, once late commits, the 71.82 Hz vsync, the head
prediction and the render pose were fixed (sections above). To check that it has not come back: Monado's
`warp prediction error` and `late commit` lines, the compat layer's `pose wait ... ran out`. Tools (on the headset,
temporary): /root/tmp/jitter.sh (its Steam load flashes the page: never while the user wears the headset),
posegap.py, measure.sh on|off.
