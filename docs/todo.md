# To do

Only what is left to do, roughly by priority. Done work leaves this list: it is described in the other docs
(`docs/*.md`) and in the commit log.

Priority (user, 2026-11-18): Steam Link VR first (it is what this headset can do really well, and it is
unusable as it is), then the tracking, then the stability; the rest after.

## Steam Link VR and streaming (first)

- Steam Link VR: still black flashes (fewer), and sometimes a flash brings a trembling that goes away once the
  headset's load drops (seen with desktop mode and Dolphin open). Not reproduced on the table (2026-11-17, panels on,
  stream running, desktop mode plus 500 MB taken, load 19, MemAvailable down to 126 MB): no black picture from the PC,
  no gap over 100 ms after the start, no empty submit, Monado receiving 69-72 frames a second with none left unshown.
  The shim now logs each such event with the time of day (`journalctl -b | grep "quest1_v4l2: vrlink: [0-9]"`, the
  journal is lost at reboot): after a flash seen worn, read it before rebooting, and note the time.
- The Wi-Fi driver runs out of receive buffers as a stream starts (2026-11-20): `wlan: NBUF alloc failed 2115B @
  htt_rx_ring_buf_attach` and "NBUF allocation timer expired Fail count", 9,415 failures and 2,771 allocation stalls
  in the first 45 s of one stream start (MemFree down to 65 MB; the PC saw PingSpike, HostRxLoss, ClientRxLoss then),
  none in steady streaming. The Normal zone's watermarks are 6 MB apart (min 24.7, low 31, high 37 MB), so bursts
  fall to direct reclaim and the driver's atomic allocations fail. Tried `vm.extra_free_kbytes=65536` (live, not kept):
  the next Steam + SteamVR restart and stream start left the headset unreachable (ping yes, SSH no) for 15 min and
  more: maybe a coincidence (that restart pair froze it before), but not to retry blindly. Next: a smaller step
  (`min_free_kbytes` 49152 -> 65536, or extra_free 16 MB) with the freeze capture running, and stream starts that
  allocate less at once.
- Microphone latency in Steam Link VR (~500 ms felt, user 2026-11-17, still there at the end of the night): the headset's capture holds ~20-40 ms; measure
  vrlink's path (its audio jitter buffer is 50 ms on the headset; the PC's side unknown).
- Steam Link VR (docs/streaming.md, section 2): a new picture each frame, a sharp centre (user, 2026-11-12: "it's
  sharp"), and on SteamVR 2.19.3 "it nearly doesn't tremble any more" (user, 2026-11-17, item 13), but it flashes black
  when the headset is loaded (the dashboard open) and its latency is still a bit high (user, 2026-11-16). On the table
  (2026-11-16, after the CPU split, device/holo-cpuset.sh) it runs at ~71 fps with the dashboard open or not: check worn.
  - Putting the headset on: a strong tremble for a moment before the stream settles (user, 2026-11-17): the change
    from 9 fps (panels off) to 72, the PC's rate control and prediction catching up. Measure it (stepsnap while
    Monado switches the panels on) and find which side lags.
  - The PC's chroma key (SteamVR beta, Camera settings, "Key Content": games draw a key colour where the room view
    should show): the headset shows the key colour (green). 2.19's vrcompositor keys only in its simplified shader
    for YUV ("spatial encoding 2") input, `distort_ps_achromatic_se2_ck`, and logs "Chroma key (virtual) room view
    support"; the layer converts vrlink's NV12 itself, so the key must be applied there (alpha where the colour is
    near the key) with vrcompositor blending the stream over the room view.
  - Encode sizes other than 1024 a view (user, 2026-11-17): below 1024 the picture is wrong in the headset; both
    smaller and larger must work (the layer prewarms 1024x4096 buffers; venus's stride and alignment).
    2026-11-20, the PC's width set to 832 then 960 during one session: 832x3328 (venus stride 896) and 960x3840
    (stride 1024) decoded and shown right in a snapshot, the change of size mid-session too (a kernel WARN in
    `__vb2_queue_cancel` at the reset, harmless). What broke that session was the tracking: lost, the roomview came up
    and its pipeline crashed vrcompositor (fixed, 2fba144). Larger: venus takes up to 5760x5760 (ENUM_FRAMESIZES) and
    2,563,200 macroblocks/s, but 1024x4096 already takes 13 ms of firmware a frame: 1280x5120 would take ~20 ms (no
    72 Hz). Newer PCs choose 1280-1344 in their automatic mode; the PC caps with its own `maxStreamFormatWidth`, and
    nothing found yet lets the headset ask for less (vrlink only gives the decoder a 1536x6144 bound).
  - The edges look painted ("oil painting", user, 2026-11-17): it was the PC lowering the quality while the decoder
    was slow (fixed, item 13); to judge again, then 2.19's new `sharpening` settings.
  - The controllers' battery levels don't reach the PC through vrlink (user, 2026-11-17): find which property vrlink
    forwards (`Prop_DeviceBatteryPercentage_Float`, `Prop_DeviceProvidesBatteryStatus_Bool` on our driver's
    controllers?) and set it.
  - Picture quality (user, 2026-11-16: "a bit pixelated/blurry", the PC's desktop view unreadable): the PC alone sizes
    the stream (docs/streaming.md "Picture quality"), and caps each view by its GPU's generation (this PC's Pascal:
    1024; Turing 1280, Ampere and later 1344; no setting lifts it). The bitrate is now 160 (`driver_vrlink/
    targetBandwidth`, was 110; no cost on the headset). Left for the user to judge worn: a larger sharp centre
    (`steamvr/preferredFoveatedAngle`, see the frame rate below) and the PC's sharpening (`driver_vrlink/useSharpening`).
  - Latency: measure it (the head's pose to the picture's pose on screen) and find where it goes: the PC's ~90 ms
    prediction says how far ahead it must guess.
  - The head is still over-predicted (user, 2026-11-17 end of night), although the PC's prediction no longer
    overshoots from a delayed rate (the low-pass is gone, docs/streaming.md item 15): the black edges and the avatar's
    head still overshoot. Measure what the PC predicts against the head's real motion (`vrlink_aftm_*.csv`, stepsnap).
    Read 2026-11-20 from the PC's CSVs and the code (not worn): the PC predicts 83 ms ahead (97 ms on the worn night
    of 11-17) and that matches when the frame is shown (Submit Illumination - Predicted Display: +0.7 ms mean, +-6 ms),
    so it is not a wrong latency figure: it is a constant-velocity guess over ~90 ms (~27 deg at 300 deg/s). Of the
    83 ms: encode 7, network 4-5, decode 13, then ~26 ms between the decoded frame and vrlink taking it, and 29 ms from
    vrlink's submit to the photons.
    2026-11-21: the shim now gives vrlink a braking rate while the head slows down (`brake_rate`: the turn stops
    where a constant deceleration stops it); simulated on the user's recorded head, the overshoot p99 6.7 -> 3.2 deg
    (max 16.8 -> 9.4), the PC's predicted pose shaking 0.46 -> 0.56 deg rms. To judge worn: fast turns and stops. Levers: shorten that hold (vrlink's own queue; the PC's
    `driver_vrlink/maxVideoQueueLatencyUs` is the user's call), a deceleration-aware rate in the shim (`raw_pose`,
    to simulate on a recorded head first). Controllers: driver_quest1 gives SteamVR Monado's world-frame angular rate
    (controllers.cpp, ~line 1125) where the HMD's is device-local: check with a constant rate on a yawed controller.
    The panel runs at 71.82 Hz and vrlink answers the PC "72.00" every second (a +-6 ms beat, small).
  - Strong lag at random moments (user, 2026-11-17): the stream stalls hard for a moment, then recovers. Match the
    times with the shim's log, the PC's `driver_vrlink.txt` (ProblemAttribution) and `vrlink_aftm_*.csv`, and the
    headset's load and memory then.
    What it looks like (user, 2026-11-17): the video is gone (the headset falls back to its own Aurora home), the
    audio lags very strongly, then it comes back, goes again, comes back, and so on. Only lead so far: it happens
    when the PC's frame rate varies a lot.
    2026-11-20 (docs/streaming.md "Decoding: every frame DecodeSlow"): in those logs the delay hits a flat ~1 s
    (a full receive queue) in episodes of ~6 s, with the decoder late on every frame; the decoder's path is now
    ~6 ms shorter and steady (13.0 ms, p90 13.9). To judge worn: whether the lags still come.
    Logs of the night 2026-11-17 saved outside the repo (they hold connection IDs): `../vrlink-logs-2026-11-17/`
    (`pc/`: driver_vrlink, vrserver, vrcompositor, vrmonitor and 10 `vrlink_aftm_*.csv`; `headset/vlk/`: shim
    journal, vrlink, vrclient_vrlink, vrserver, vrcompositor, layer logs, meminfo; `headset/vlk2/`: Steam's
    `*.previous.txt`, q1trackd, monado, warnings, coredump list).
  - Stopping the stream crashed all of Steam and SteamVR on the headset (user, 2026-11-17 night; no coredump kept).
    2026-11-21: every vrlink of a boot (8 sessions) crashes (SIGSEGV) on its way out, after its 15 s "Timed out
    getting information from the server" once the PC's SteamVR is closed: start there (coredumps are refused,
    "Failed to send coredump datagram").
    Find the crash in the logs above (Steam's `*.previous.txt`, vrserver, warnings) and reproduce it.
  - Position: vrlink submits no depth, so a position error of the PC's prediction is not corrected at all.
  - On a system that had run for hours, the stream was 16.6 px rms off the world in Monado's output (2.5-3 px fresh).
    Follow it over a long run (`mjit.py` every few minutes, with vrcompositor's fps, fds and memory) to find which of
    these takes it there (vrcompositor's `sync_fence` leak is fixed: PR #4, display-event fences from a pool):
    - The headset slowing down over time: 64-71 fps at the start, 55 fps 90 minutes later, ~380 MB available, swap
      in use, load ~14.
  - Black flashes (user, 2026-11-12, still on 2026-11-17): one worn was a second without a picture as the dashboard
    opened, with datagrams dropped (now fewer: vrlink's receive threads realtime, 8 MB buffers, docs/streaming.md
    item 16); match the next ones with the shim's log (`journalctl -b | grep "quest1_v4l2: vrlink: [0-9]"`).
  - GPU (user, 2026-11-16: a stable 72 fps with margin, for PCs that send views above 1024): vrcompositor takes
    8-15 ms of GPU a frame with the stream depending on the session, ~12 typically (kgsl trace: one batch a frame), so
    there is no margin. Not the layer's inset blend (off: the same), not memory fragmentation (compaction before a
    restart: the same), not Steam's webhelper (idle then). Sessions with the PC's inset widened
    (`preferredFoveatedAngle` +16) ran at 55-57 fps, +8 at 62-66, 0 at 66-72: unexplained. Next: a per-pass GPU profile
    during a stream (`/tmp/quest1-gputime` is not used while streaming: the user's call). `enableSimplifiedShaders` (off; the
    shader choice needs it, docs/streaming.md "The distortion shader") saves 2.2 ms, but the stream's colours come out
    washed (the simplified shaders take its texture as linear): an sRGB view of the copy fixes the colours but, made
    mutable, costs ~9 ms (45 fps). Next: copies in an sRGB format of their own, the copy shaders writing linear.
  - Start of a session: vrcompositor at 22-45 fps for 2-4 s (Monado's re-projection holds the head steady: 0 late),
    ~250 MB swapped (2026-11-16): vrcompositor +232 MB (ION +174, KGSL +58), vrlink's frame buffers, the decoder's
    references, and vrlink's first decoder always reset (two rounds of 16 imports). Freeing memory as vrlink starts did
    not help. 2026-11-21: the first decoder's reset is avoided in 5 starts of 6 (the shim keeps the stream's size
    across sessions, docs/streaming.md "Session start"); left: the 10-bit references made at the first sequence
    (~127 MB, up to 1.1 s under memory pressure, then vrlink resets), and what else allocates.
    Timeline of a start (2026-11-16, vrlink.txt): the session at 0 s, the PC's first packet 11.8 s later (the PC's
    side), then ~3.5 s of decoder resets: one at once at the first SOURCE_CHANGE (CheckStuck's "last progress" is
    from the decoder made 11 s before), two "Received frame outside of backtrack window" (the first frames arrive
    later than vrlink's pose history), one more CheckStuck; stable after that. With the orphaned /dev/shm segments
    gone (q1-shmclean), one 5 s window at 49 fps, then 70-72.
  Also: the PC's overlays show only when the PC's SteamVR started after
  vrlink; stopping a session from the headset (`StopStreamingSession`) does nothing and vrlink hangs on its way out.
  With the USB cable in, vrlink and the PC send the stream over the USB network too (each datagram on each link: about
  a core on the headset, 2026-11-12). Hiding usb0 from vrlink (getifaddrs, and dropping what it sends there) made it
  never connect ("Timed out getting information from the server"): the PC reached it over USB first.
- VRChat's loading skybox (user, 2026-11-17, photo): the world's picture only inside the sharp inset's rounded
  rectangle, black (the grid) between it and the edges. The PC's compositor draws the stream's views (vrlink forwards
  no skybox): check whether a Quest 3 on the same PC shows it too, and whether the wide view lacks the skybox in a PC
  capture (`driver_vrlink` "Dump H265"); the headset can only place the views it gets.
- Steam Link VR latency (docs/streaming.md item 15; PC logs `vrlink_aftm_*.csv`, 2026-11-17): ~91 ms from the PC's
  pose to the headset's display, of which decode 15 ms and ~34 ms waiting on the headset between the decode and
  vrlink taking the frame (the PC predicts for that display time). The PC's `driver_vrlink/maxVideoQueueLatencyUs`
  (not set: its value goes to the headset in message 0x24) and its 10-bit stream (8-bit decodes ~4 ms faster) are PC
  settings: ask the user before trying them.
- Steam Link from a PC, 2D (docs/streaming.md, section 1): it works well (user, 2026-11-12); more performance is
  welcome: the kernel emulates atomic instructions for Steam's programs (`emulate_atomic` in the profiles), the
  Vulkan/OpenGL semaphore is refused (glFinish fallback).
- Steam Link from a PC, 2D: a controller (gamepad) connected to the headset itself is untested (user,
  2026-11-18): connect one and check that it reaches the PC's game during a 2D stream.
- Hardware video decoding for the whole system (user, 2026-11-09: "hardware video decoding system wide, Chromium
  included"). FFmpeg's `*_v4l2m2m` decoders and VLC use venus (`q1-hwdec`, docs/streaming.md "The decoder"). Left:
  GStreamer's v4l2 decoders (they fall back by themselves, but set CAPTURE up after SOURCE_CHANGE: the library must
  start the firmware session itself first); Chromium decodes with VA-API only (Arch's build has no V4L2 decoder): a
  VA-API driver on top, or another build. The user's target (2026-11-10): the stream at 72 fps next to the dashboard
  and overlays, so everything on the hardware path and no CPU copy of the frames.

## Then: tracking, stability and the rest found by the user (2026-11-16 evening)

- Storage (development headset): the development leftovers are deleted (user's go, 2026-11-17: `.debug`, the old
  monado-service and library copies, `tmp`, `stream-crash-*`, old audio logs, the package cache): 5.8 GB free of the
  24 GB image, was 2.2. Left: check what grows over a day of use (`du` before/after); the full install wipes the
  headset anyway (user).
- A whole-system freeze (SSH too; ping fine): desktop mode open with Discover searching, headset on the forehead, then
  put on: SteamVR froze first (Monado still ran), then everything. Same symptom at 18:43 right after a stack restart
  and a stream start. Not reproduced since; a capture to the PC (dmesg -w and a 10 s snapshot of memory, load, top
  and D-state tasks over the USB link) catches the next one: keep one running during tests. The capture of
  19:13-19:25 (desktop mode, Discover, Dolphin, no freeze): available memory down to 256 MB, swap-out up to 2165
  pages/s, load 17-20, Discover and Dolphin each in D >5 s waiting on pages: memory pressure is the first suspect.
  Measured 2026-11-16 22:42 (stream running, nobody wearing it): the desktop alone takes ~0.75 GB (MemAvailable 0.48
  -> 0.29 GB, +0.4 GB in zram) and the load goes 11 -> 18 while it starts; Baloo, Discover's notifier, ibus and
  kaccess are now off (holo-desktop.sh, ~0.1 GB). Next if it freezes again: what else to trim (plasmashell 0.3 GB, the
  Steam webhelpers 0.65 GB), or stop the desktop's apps first under pressure (no PSI on 4.4: a MemAvailable watch).
  2026-11-17 (night, autonomous): under the user's load (stream-load.sh: desktop mode plus 500 MB) MemAvailable fell to
  ~140 MB, vrcompositor to 56 fps for a few seconds and vrlink's pictures came with gaps of ~100 ms (the shim's flash
  log): a stutter the user may see as a flash and a tremble. No freeze. At 02:07 the headset powered itself off
  cleanly with nothing special running (673 MB available, 71 fps): logind "The system will power off now!", no power
  key event (logind ignores it), after ~4.5 h of panels, stream and benches with only the PC's USB port charging. Most
  likely the battery's critical level (UPower powers off at 2 %): check the battery's level when it comes back
  (`/run/vpower`, Steam's battery icon), and charge it before long benches. The journal lives in RAM but goes to the
  kernel log, which pstore keeps across a reboot: at the next boot read `/sys/fs/pstore/console-ramoops*` for what
  asked for the power-off (UPower, Steam or other).

- Performance (docs/performance.md): GPU-bound, ~12.4 of 13.9 ms a frame. Levers left, all visible, so the user
  decides: achromatic distortion (vrcompositor's distortion pass is ~2 ms per eye), a lighter environment than Aurora
  (~4 ms; passthrough itself is cheaper), simpler controller models (~2 ms), the 4x MSAA of the dashboard layer.
  Also: the LSE emulation kernel (`build/perf/boot-native-holo-lse.img`, built 2026-11-08) to boot and measure (the
  running kernel traps ~150,000 LSE atomics a second with SteamVR up, 2026-11-16: ~12-24 % of a core; needs USB);
  Meta's tracking threads starving when worn (Android pins syncboss to CPUs 0-3); memory (~500 MB available after
  boot; kswapd and loop0 busy while Steam is used).
- Passthrough depth (on by default since 2026-11-17, docs/passthrough.md "Dense stereo depth"): its artifacts are
  still too visible (user), and it costs CPU while the roomview shows (~25 % of a core; no longer when hidden,
  2026-11-20); Meta's route (disparity from the Venus encoder's motion vectors) next.
- Audio: a click at each Steam start (user, 2026-11-12: still there; the crackle is gone). The output no longer
  closes between sounds (docs/audio.md), so find what Steam does at its start: a sound of its own, or a stream it
  opens and closes.
- The room comes off (rotated and moved, "quite a lot") after a tracking loss (user, 2026-11-17, docs/boundary.md
  "The room tied to anchors, as on Meta's OS"): the room is tied to the map's world origin, which moves when the
  engine relocalizes through another submap of the same map. Meta's OS (logcat of the same morning, same room) had
  the same failed validations and two contexts, yet its room mapping (13 scene anchors) did not move: each anchor is
  located from the landmarks around it. User: 100 % Meta's behaviour, tracking never worse than on Meta's OS. Do:
  tie the playspace and floor to persisted spatial anchors located by the engine (several around the room), as
  Guardian and Home do, through the engine's anchor API (InternalAnchorApiServer v2 / SlamAnchorServer over
  RuntimeIPC, or TrackingEnvironment registerAnchor + its shared memory). No "verify your space" prompt (user: the
  old Guardian had one, Meta removed it, SteamOS has none and that is better).
  2026-11-17: written, committed as work in progress, NOT tested (src/q1track/q1room.h, q1trackd.c, q1anchord.py;
  installed on the headset that evening, q1trackd logs `room state` every 10 s): q1anchord places and persists up to
  8 anchors while worn; q1trackd locates them (map manager, 20 Hz) and moves the room by a blend of them, slewed
  (2 cm/s, 2 deg/s). To test worn: walk the room ~2 min (`journalctl -u android-q1trackd | grep room`: anchors > 0),
  restart SteamVR then Monado (the room must come back in place), cover the cameras and uncover them (no jump).
  Unverified: the world frame (assumed the world anchor's), a second persisted anchor in the same context.
  First worn use (user, 2026-11-17 night): after SteamVR restarted, the whole room had moved again, so the anchors
  don't hold the room yet. q1trackd's `room state` lines of that boot are kept locally (not in the repo, see the
  Streaming lag entry): check whether anchors were placed and located at all, then what moved the room.
- Worn test of the map state (2026-11-16, docs/boundary.md "Map state and the room"): the chaperone must hide during
  relocalizing/lost/bad and come back in place; check that vrcompositor's chaperone follows the collisionBounds alpha
  setting live (not verified without the headset worn), the Meta-like UX (black/passthrough while relocalizing) if
  hiding the walls is not enough, and tune BAD_WINDOW/BAD_COUNT (q1anchord) and kBadHideS (driver).
- The playspace is not where it was drawn (user, 2026-11-12: a new boundary, the headset off and on, no longer in
  place, "the space anchors work really badly"; again on 2026-11-13 after a reboot), and the engine opens new
  submaps instead of finding the saved ones (user). The boundary shifts mostly after a reset of the tracking, e.g.
  when the tracking service is restarted (user, 2026-11-13): test the playspace across stack restarts first. Found
  2026-11-12 (docs/boundary.md "Tracking starved of CPU"):
  - Meta's tracking is pinned to CPUs 0-3 as on Android (~3 cores' worth: IOTcam ~1.2, controllers ~0.85), but
    here vrserver, the vrwebhelpers, Steam and q1apps run there too: the four little cores at 100 %, the engine
    logging "Map tracking did not run" up to once a second, "Bad map tracking" 30-50 times a minute, falling to 3DoF,
    and while it tracks two contexts it skips map building. With everything else moved to CPUs 4-7 (a live
    `taskset`, lost at each restart): 0-1 "Bad map tracking" a minute worn, no new context at a put-on, and the
    boundary was in place after that put-on. Made permanent 2026-11-16 (device/holo-cpuset.sh: a cpuset v1
    `tracking` 0-3 for the `android-*` units, `rest` 4-7 for all else, Steam's 2-6 pinning is cut to 4-6; q1trackd and
    q1anchord stay in `rest`); still to do: validate after a reboot and worn ("Bad map tracking" a minute), then test the
    playspace over several sessions and reboots.
  - The playspace drawn at 21:40 was drawn while the tracking was failing (the frame may have slid under it).
- Meta's tracking runs from boot, worn or not (trackingservice IOTcam ~50 % of a core, CtrllrCam ~11 %,
  2026-11-08). Do as Meta's OS (user, 2026-11-09): after power-on, tracking stays off until the headset is worn the
  first time; from then on it stays on, even when the headset is taken off. It stops only when the headset goes to
  sleep (standby), and after waking it starts again only once the headset is worn. Check that the map and the
  controllers come back fine after each start.
- The kernel logs "SPI transaction rejected by SyncBoss" / "Bad magic number 0xcacacaca" several times a
  minute: compare with Android, check whether IMU or controller data is lost.
- Controllers, more tests (user, 2026-11-12): one woken up late (battery out and in while Meta's stack starts) must
  still connect (docs/tracking-6dof.md); after a real tracking loss the headset keeps its last position and the
  controllers come back.
- Sometimes, as the headset is put on, the floor's direction seems uncertain and is seen to adjust (user,
  2026-11-12; not remembered from Meta's OS). To investigate: gravity/orientation at the first poses after wake,
  and whether the room's floor comes from the relocalization above.
- When Meta's tracking service is not running (stopped, restarting, not up yet), show the dark blue screen in the
  headset, as Meta's OS does, instead of the VR scene (user, 2026-11-17: otherwise the whole VR world spins in every
  direction and it is almost nauseating). Hook it on q1trackd's state (no fresh poses from the service) in Monado or
  the driver, and fade back to the scene once poses come back.

## Display

- SteamVR's own trembling or over-prediction (user, 2026-11-17): in the spectator view the black edges tremble a lot,
  which is suspect. Measure the head pose vrcompositor renders with against the pose at display (Monado's), without
  a stream, and whether the re-projection then corrects it on the panels only.
- 90 Hz mode, selectable from the refresh rate setting (low priority). Base: github.com/TheCez/quest1-90hz-orangefox,
  and a Reddit write-up (r/OculusQuest "I got a Quest 1 running at a real 90 Hz", Juspertinry's boot image and module):
  only the device tree changes on Meta's kernel (`qcom,mdss-dsi-panel-framerate`, `max-refresh-rate` and the timing
  bytes scaled ~1.67x); the check that it really runs at 90 is the panel clock against the DSI bit clock
  (`pclk0_clk_src` rate = `dynamic_bitclk` / 6, 133.0056 MHz at 90), after a short blank to resync it. **Not every
  panel takes it**: Samsung's (`qcom,mdss_dsi_sdc_lightman_video` in `mdss_mdp.panel` on the command line, the dev
  headset's) does, AUO's stays at 72. So the setting shows 90 only on a panel known to work, and only once the kernel
  reports the rate, with a fall back to 72 if not.
- Activating passthrough freezes the view (the only freeze left in Steam's VR UI, user 2026-11-12): vrcompositor
  stalls ~0.3 s (2026-11-09 16:44:02, CTrackedCamera "Late initialization"): find what it builds then (pipelines?
  camera textures?) and do it at start. Not reproduced on the table (2026-11-16, first activation after a SteamVR
  start through the system layer's mailbox, `set_camera_roomview_enabled`): 71.6-71.9 fps, no late commit. Check it
  worn with the dashboard's button (Steam's UI may be what stalls then).
- Steam's VR UI frame rate (user, 2026-11-12: SteamVR itself is very fluid, Steam is "usable" but needs more
  performance): ~10 fps while a page animates, any damage size. The GPU is the limit now (100 % busy):
  ~24 ms of GPU per Steam frame in the gaps vrcompositor leaves, for the full composite of the 1920x1080 page, its
  capture and readback. Find what costs that much (backdrop-filter blurs? the capture copy? ANGLE on this blob), or
  avoid the readback: frames from the GPU process to vrcompositor without the CPU (CEF OSR has no shared texture
  path in Steam on Linux: only CChromePainterSharedBuffer). A lower device scale is visible (user's call). UBWC images and a memory cache in
  the Vulkan shim took a page animation from 11.5 to 20 fps (docs/steam-gpu.md), but both are off again (corrupted
  UI worn): find which images ANGLE needs mutable and keep the rest compressed. Not
  RenderMode=Direct for Steam: it froze the headset (2026-11-08). Must leave room for streaming later (user).
- Steam's VR overlay copy runs on llvmpipe (user, 2026-11-10: the CPU at ~100 % when the pointer moves over a Steam
  page): the pages render on the Adreno (webhelper GPU process, ANGLE Vulkan), but Steam's browser process copies
  each frame for the VR overlay with GLX on Mesa llvmpipe (`webhelper_vr.txt` "Loaded GL 4.5", docs/steam-gpu.md
  "Known issues"). Move that copy to the GPU. Tried 2026-11-16 (docs/steam-gpu.md "The browser process on ANGLE"):
  it works and pays (browser 20 -> 8-13 % of a core, vrcompositor 72 instead of 61-66 fps while a page animates),
  but vrcompositor hung the GPU 5 minutes in (the only GPU hang of that boot): find why before shipping it.
- SteamVR's global render resolution slider: remove it as on the Steam Frame, later (user, 2026-11-09), without the
  side effects found so far (docs/frame-mode.md).

## Passthrough

- Turning the room view on (2026-11-17, with the layer's pipeline memo: no freeze any more): a first fade, then a
  black flash and a second fade, then fine (user). Find what restarts it (vrcompositor's roomview log, q1-ptprovider).
- Passthrough sometimes goes black and comes back with its fade-in, as if turned off and on at once (user,
  2026-11-10, with desktop mode and a stream open: the CPU was saturated). q1-ptprovider logged no disable and
  vrcompositor no roomview restart then: catch it with `PT_DUMP=1` (record states once a second) while it happens.
- Turning the head fast sideways shows black at the edges, at best in one eye only (user, 2026-11-17): the
  reprojected camera picture leaves areas no camera covers for that eye. Fill them, either by stretching the edge
  pixels or by reprojecting the other camera's view into that eye.
- q1-ptprovider takes 20-21 % of a core while passthrough shows (2026-11-16, after the composite's NEON rewrite:
  docs/passthrough.md): the composite itself is ~2 ms an eye a camera frame; profile the rest (`perf top -p`).
- The camera test in SteamVR's camera settings works (the four cameras, docs/passthrough.md "The camera test") but
  the whole dashboard panel lags while it runs (user, 2026-11-17), with no CPU saturation: the dashboard redraws the
  panel for each test picture (ten a second); find what makes that redraw slow here.
- Camera power-up race at some boots (trackingservice "Timeout: (power: SLEEP) => (power: STANDBY)", 3DoF
  only), and udev helpers hanging ~3 min at the same boots (2026-11-08: `libinput-device-group` on event0 and
  `scsi_id` on sdd/sde, killed after 2 min 59 s): q1-hangwatch starts at sysinit now and logs D-state threads with
  their kernel stacks to the kernel log (pstore); read it at the next such boot.

## Boundary and room setup

- `ComputeInverseDistortion` returns identity while direct scanout distorts.
- Pairing Quest controllers from Steam's UI (`syncboss_input_pair`: never without the user's explicit go).
  docs/controller-pairing.md; user, 2026-11-17, with q1-pairing and the driver installed: the page still shows the
  Steam Frame's controllers and no Touch controller (nothing to identify or forget), though it sees two connected;
  and it stops searching once two are connected (the user wants more than two later). Real pairing call not wired.
- SteamVR's playspace setup (user, 2026-11-12): the controllers' models are wrong there (seen before elsewhere too: a
  dark disc where the ring should be), and it runs poorly.

## Steam settings and Steam

- Steam's first-run setup of the Frame: untested.
- SteamVR restarted alone (Steam kept running) breaks Steam's VR UI: steamwebhelper's GPU process takes GPU page faults when
  vrcompositor stops (2026-11-11 17:50 and 17:51, two restarts) and the dashboard then shows its lower half empty and loses
  its bottom bar (restarting Steam brought it back, user). SteamVR alone should restart cleanly.
- Frame mode (docs/frame-mode.md, 2026-11-16: device type 5001): the user to check that the library shows the Frame's
  compatibility ratings and that nothing else in Steam broke (its settings' System and update pages, the startup
  movie).
- Steam crashes (SEGV, its webhelpers SIGBUS) when vrlink is killed during a session (2026-11-16 21:58 and 22:24, a
  test killing it by PID after the stream's stop did nothing). Not seen in normal use; vrlink's exit hang makes it
  more likely.
- The stream layer's variant D (wide views read from the NV12 frame, only the insets copied: -0.5..1.3 ms of GPU,
  streambench) was installed 2026-11-16 and the headset stopped answering minutes later (ping only, rebooted; no
  pstore record): not committed. Retry with the hang watch on (journal kept) before keeping it.

## Power

- The sleep animation shows inside the dashboard panel instead of full screen.
- Real sleep (suspend to RAM, `/etc/quest1-sleep` deep, opt-in): on resume Meta's tracking is lost, no sound,
  the proximity sensor wakes the headset while worn, the fan and power LED stay on.
- Restart from Steam: with the OS started by `fastboot boot` it restarts into Android; test it once the OS is
  flashed (user), then drop the sshd `pam_systemd` workaround in `device/holo-perf.sh` if it works.
- Shut down from Steam (user, 2026-11-21): the headset "crashed" instead of powering off (the user then held power,
  it came back in Android). The memory log on `/userdata` stops in the middle of the shutdown with nothing wrong in
  it. To check: whether the kernel's power-off takes the modem down (Linux cannot stop it, and a stop hangs the SoC,
  docs: `device/holo-android-modem.sh`), which would leave a hang then the watchdog's reboot into Android.
- Charging while off (user, 2026-11-16): plugged in while off, the headset boots into a charging-only mode (the
  bootloader starts the boot image in charger mode: `androidboot.mode=charger` on the command line, Meta's OS then
  shows its battery screen and charges). Once SteamOS is flashed, that boot would start the whole of SteamOS. The
  initramfs has to see charger mode and do what Meta's OS does instead, ideally with Meta's own charger from the
  stock image (its charger binary / healthd, screen off or the battery animation, power button to boot), rather than
  our own.

## System

- Updates (user, 2026-11-17): Steam's client updates itself (kept on); SteamOS and SteamVR are never updated by the
  headset, only by this project when the user asks (a manual update of ours). The app's own update system must never
  delete the user's data on the headset. SteamOS and SteamVR updates must keep working without undoing our changes.
- Code layout (user, 2026-11-17): split the long sources into small modules that are easy to read and work in.
  Done (2026-11-17, the same binary byte for byte: `tools/same-binary.sh`, the layer being one translation unit):
  `quest1_scanout.c` (2088 lines), `quest1_compat.c` (3272), `quest1_modifier.c` (1939) and `quest1_display.c`
  (1803), each now a short file and parts of 50-600 lines named after it. Next: `quest1_wsi_x11.c` (1357, a unit of
  its own), the driver (`driver_quest1.cpp`, `controllers.cpp`) and Monado's `comp_window_mdp.c`: separate units or
  C++, so check those with a build and a run in the headset instead.
- Slim the install: drop what this headset never uses (other controllers' render models, etc.).
- The telemetry HAL's loop (docs/performance.md, 2026-11-11 heat): once an event cannot go to ovrstub's
  TelemetryService it retries with no pause (a core at 100 %, ~50000 log lines/s, fakelogd at 90 %). Stopping the HAL
  froze the tracking: make the stand-in accept the events, or find what it fails on.
  Status 2026-11-16: not looping now (HAL at 0 % since boot); ovrstub already accepts the oneway code 1 (528 bytes) it
  gets and frees the buffer, so the failing send is not a refusal. Suspect: ovrstub down or restarted while the HAL
  holds its TelemetryService handle (dead object, never fetched again). Next: restart ovrstub on a test boot and
  watch the HAL; if it loops, make the HAL unit `PartOf=android-ovrstub.service` (restart together, tracking waits).
- Storage hang: twice on 2026-11-12 (and earlier) the headset stopped answering (ping fine, sshd's banner timing out,
  a build or a test running); only a reboot helped, and the journal is not kept. Keep the journal (or pstore), then
  read q1-hangwatch's D-state stacks.
- A GPU page fault in a steamwebhelper (2026-11-12 20:28, during a stream) took vrcompositor's device with it
  (`vkQueueSubmit` -4): SteamVR restarted by itself. Again at 21:48, same fault address: the layer's "q1 display
  done" thread spun at 93 % in its fence wait for 5 s, then VK_ERROR_DEVICE_LOST and vrcompositor aborted.

## Hardware

- Heat after hours of use: read the thermal zones after a long session (stream, games) and check whether the
  kernel's thermal mitigation throttles the CPU/GPU meanwhile (the fan and the CPU clocks follow the temperature now,
  docs/performance.md).
- Bluetooth works (user, 2026-11-12): more tests and polish: keyboard/mouse, audio (A2DP), Steam's and Plasma's
  Bluetooth settings, what Plasma's toggle does to the chip's power, a faster patch download (~11 s), the UART's
  power cost (in-band sleep).
- Power LED colour controlled by the user (low priority).
- USB OTG: keyboards, storage and other devices on the headset's port (low priority).

## Steam Frame features

- Steam's quick menu (docs/frame-features.md): the Performance tab and the time zone setting show (user, 2026-11-17).
  Left: check worn the manual GPU clock and the performance overlay's levels (`q1-perfhud`); a frame rate limiter
  (needs gamescope's atoms outside VR; in VR the Playtime profile, not set).

## Later

- Light overclocks within what other MSM8998 devices run stable (GPU, CPU), watching temperatures.
- Hand tracking.
- x86 games (FEX, Proton): very low priority. Ruby / RPG Maker games (mkxp, OneShot) close at once on ARM64 Proton
  (the Steam Frame too): github.com/URFAVF3MBOY/quest2-steamos (code reuse allowed, credit it) puts the 17 bytes of
  x86 `_isatty` code Ruby looks for back into Proton's `ucrtbase.dll` on a game's first start (its docs/PROTON.md).
- Other controllers: Windows Mixed Reality, Quest 2 (github.com/Juspertinry/q2ctrl-module).
- Quest 3 controllers (Touch Plus): github.com/Noculus-VR/TouchPlus-on-Quest-1 (user, 2026-11-13) pairs them with the
  Quest 1 by patching `libsyncboss.so` (no MCU or firmware change). Not finished: wait until it is, then see what our
  stack (q1trackd, libossdk) needs.
- The installer app (app/, installer/); later a Linux version that builds the image on the PC. The Steam Frame's
  files can come from Valve's public update store (`frame_source` "ota", SteamVR 2.19.3, installer/README.md): check
  2.19.3 on the headset (it runs 2.17 now), then make it the default over the 0.3.0 recovery image.
- While the headset's install system (the small Linux that builds the image on the headset) is building, the
  power LED blinks white <-> purple.
- Dual boot with a boot selector in the headset (very low priority).
- Valve's Steam Frame kernel (7.2) instead of Meta's 4.4, once the port is stable (user, 2026-11-18). Context:
  github.com/nicolas-riera/Quest1-SteamOS/issues/5, the overpressure organization (SteamOS on the PICO 4,
  github.com/overpressure) invites the Quest 1 as `overpressure/monterey` once it is stable. Their kernel tree
  (overpressure/linux, one branch per layer and headset) is the base; the first and biggest piece is Qualcomm's KGSL
  (GPL, in Meta's Quest kernel source, newer versions still carry the 5xx code) as a module, so the Quest's own
  Vulkan driver keeps running through libhybris (Turnip has no 5xx support). Needs GPU clock and bandwidth voting,
  buffer sharing with the display and preemption right. Their repositories (phoenix, packages, headset-template) may
  be reused for Steam Frame mode, room view, Steam Link, FEX/Proton/Lepton.

## Hand tracking (branch hand-tracking, docs/hand-tracking.md)
- Worn test of Meta's hands: `echo auto > /etc/quest1-hands`, then check tracked status values, the hand's pose space, the retargeted skeleton, pinch gestures (system / dashboard trigger).
- Deploy the `android-q1handsd` unit in the image (holo-android-tracking setup, build-native) and merge the branch.
- Menu icon near the palm (overlay) and a joint overlay; the vrlink hand path (probably controller inputs only).
- Worn, 2026-11-17 (merged, setting on): the controllers disappear when put down, but no hand shows: no skeleton, no
  palm mark (q1-handviz runs; check q1handsd's tracked status and the driver's hand device first).
- The switch to hands (user): only when the controllers lie still, nothing on them is touched, and the hands are away
  from them (not where the controllers are). Back to controllers on its own when one is picked up (a clear motion or
  a touch); today a button must be pressed.

