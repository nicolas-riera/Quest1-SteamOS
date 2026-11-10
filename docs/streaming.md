# Streaming survey (2D Remote Play, PC VR, PC overlays)

Status 2026-11-06: feasibility survey with read-only probes. Nothing here was made to work. The "Frame" parts come from files on the headset
(`docs/steamframe-stack.md` has the long version). Tags: [H] measured or read, [M] likely, [L] guess.

## What the headset has

- Video block (probed): `/dev/video32` = `msm_vdec_8974`, `/dev/video33` = `msm_venc_8974` (downstream msm_vidc on `cc00000.qcom,vidc`, multiplanar M2M).
  [H] Decoder OUTPUT formats: H264, MVC, **HEVC**, HEVC hybrid, VP8, VP9, MPEG-2/4, H263, VC-1, DivX. CAPTURE: NV12, NV12 UBWC (Q128), TP10 UBWC.
  Frame sizes 16x16 to 5760x5760. Encoder CAPTURE: H264, HEVC, VP8.
- [H] **Hardware decode works** (2026-11-07, `src/vdectest`): 1920x1080 H.264 and HEVC decode **bit-exact** against ffmpeg's software decode
  (300/300 frames each, linear NV12 in both primary and split mode). UBWC output flat out: 127 fps H.264, 104 fps HEVC. Fed as a live
  60 fps stream: **9.2 ms (H.264) / 10.8 ms (HEVC)** from QBUF to the decoded frame, 28-31 ms for the first one. What msm_vidc wants
  (USERPTR + ION, one access unit per buffer, the reconfiguration order, the coded size on CAPTURE) is in the header of `src/vdectest/vdectest.c`.
  Two traps: a capture STREAMOFF while the firmware holds buffers kills the firmware (Err_Fatal, SYS_ERROR; the driver recovers on the
  next open), and split mode scales its linear output to the CAPTURE size (1920x1080 squeezes H.264's 1088 decoded lines).
- [H] No `/dev/video-dec0` (the Frame's `steam-video-codec-setup.service` makes it and `/dev/video-enc0` from `vrdevice_path`; it does not exist here).
  `/dev/video-enc0` is a udev link to `video33` since 2026-11-09 (section 0); before, Steam logged
  `CGameStreamVideoStageV4L2::BInitEncoder: Failed to open device: /dev/video-enc0` and encoded with x264.
- [H] ffmpeg's `h264_v4l2m2m` / `hevc_v4l2m2m` decoders work on venus through `libquest1_v4l2.so` since 2026-11-10
  (`q1-hwdec ffmpeg -c:v h264_v4l2m2m ...`, section 0, "The decoder"). Without the library they fail (`Could not find a valid
  device`): msm_vidc is not the mainline API. libavcodec probes every `/dev/video*`, including the msm camera nodes (QUERYCAP on
  them logs harmless kernel WARNINGs `Bad caps for driver`, v4l2-ioctl.c:1039). GStreamer has no `v4l2*dec` elements (only
  src/sink/convert).
- [H] Software decode is fine for flat streams: 1280x720 HEVC and H.264 each decode at about 100 fps (ffmpeg libavcodec, `-f null`).
  1080p60 H.264 should be OK; 4K or 120 fps is not.
- [H] Network: wlan0 5 GHz VHT 80 MHz NSS2, -43 dBm, link rate up to 866 Mbit/s (tx). Bandwidth is not the limit.
- [H] Steam (arm64) has `streaming_client` 2.17.10, which runs as an OpenVR overlay inside SteamVR (`vrclient type=VRApplication_Overlay`, `--openvr`).
- [H] Vulkan on the headset: streaming_client reports `VK_KHR_external_memory_fd`, `external_semaphore_fd` and `timeline_semaphore: TRUE`, but
  `vkGetPhysicalDeviceFormatProperties2 returned zero modifiers ... Supports dmabuf formats + modifiers? - No!`.

## 0. Remote Play from the headset (spectator) and the hardware encoder

2026-11-09: a Remote Play stream from the headset to the PC shows the VR view, as on the Frame (q1apps + Monado,
docs/desktop.md "Remote Play spectator"). It ran at about 5 fps: Steam encoded with libx264 (4 threads, ~1.7
cores, "encode 54-58 ms" in its "Slow framerate" log) on top of its "Desktop OpenGL NV12" capture on llvmpipe
("capture 25-30 ms"). The encoder now runs on venus.

Steam's encoder (steamui.so, read from its disassembly): `CGameStreamVideoStageV4L2` opens `/dev/video-enc0`
(the Frame's node name) and uses the mainline stateful encoder API with MMAP buffers on both queues; without the
node it falls back to x264. The downstream msm_vidc wants something else in a dozen places, so
`src/v4l2shim/quest1_v4l2.c` (libquest1_v4l2.so, preloaded by steam-q1; udev links `/dev/video-enc0` to
`video33`) translates. Its header lists each difference; the main ones:
- ION-backed USERPTR buffers, each registered with the firmware (`VIDIOC_PREPARE_BUF`) right after REQBUFS.
  Without that, the session's START killed the firmware (`Err_Fatal venus_venc_host_driver.c:1754`) whatever the
  controls. Found by running Android's own encoder (the vendor libOmxVenc, through libhybris: `omxprobe.c`) with
  the firmware log on (`fw_level` 0x3f) and comparing: its log shows the bitstream buffers' `BufferChk` before
  LOAD_RESOURCES, ours did not.
- NV12's UV plane: venus reads it at stride x height aligned to 32 (736 lines for 720), Steam writes it right
  under 720 lines. At 1280x720 that offset is a whole number of pages, so Steam's mapping of each buffer is two
  mappings of the ION buffer and its UV rows land where venus reads them (no copy).
- `bytesperline` filled in (msm_vidc leaves 0, and Steam copies rows with it), the CAPTURE format sent before
  the OUTPUT one, controls mapped (GOP, keyframe request, HEVC profile/level), the sequence headers in every IDR
  from the firmware (msm_vidc's header mode 2), H.264 picture order count type 2 (no reordering).

Measured with `src/v4l2shim/venctest.c` (Steam's exact call sequence, 1280x720 at 60 fps, 20 Mbit/s): H.264 12 ms
from QBUF to bitstream, HEVC 9 ms; both decode cleanly in ffmpeg with the right colours. In a real stream (2026-11-09) Steam logs
"Created encoder V4L2 for codec 5" and "Capture method set to Desktop OpenGL NV12 + V4L2 HEVC": encode 8 ms a
frame instead of x264's 55. It still lags (user): its "Slow framerate" lines blame the capture, ~20 ms a frame,
with Steam at ~250 % CPU (its GL capture of Xvfb and NV12 conversion run on llvmpipe).

### The capture: Steam's PipeWire path, fed by q1screencast (2026-11-10)

Steam's desktop capture (`CDesktopCaptureComposite` in steamui.so) holds three captures and tries them in this
order: PipeWire (`CDesktopCapturePipeWire`), OpenGL (`CDesktopCaptureX11GL`, "Desktop OpenGL NV12") and plain X11
(`CDesktopCaptureX11`, XShmGetImage, "Desktop X11 RGB"). The PipeWire one is only on with the `-pipewire` command
line option (also `-pipewire-dmabuf`, `-pipewire-fresh-start`, `-pipewire-record-deck-ui`). Outside gamescope
(`Plat_IsGamescope`) it asks the ScreenCast portal for persistent permission once at start ("Finished obtaining
persistent permissions, exiting PipeWire until streaming or recording"), keeps the restore token
(`DesktopCaptureRestoreToken`) and opens a portal session for each stream. Under gamescope it reads the "gamescope"
node instead. It takes NV12 or RGB, as MemFd/MemPtr or dmabuf ("Desktop PipeWire NV12", "... NV12 DMABUF",
"... RGB DMABUF"); without a DRM render node GBM is off ("Couldn't find any DRM devices"), so SHM only here.

The headset's portal (xdg-desktop-portal 1.20, gtk backend) has no ScreenCast, so before this Steam fell back to
OpenGL. `src/q1screencast` is that backend: `org.freedesktop.impl.portal.ScreenCast` as
`org.freedesktop.impl.portal.desktop.quest1` (D-Bus activated user unit `q1screencast.service`;
`/etc/xdg-desktop-portal/portals.conf` routes only ScreenCast to it, the rest stays on gtk; a `*` there woke the KDE
backend, which never ran before). Each started session gets a PipeWire video source node; no dialog; Flatpak apps
(non-empty app id) are refused. Frames:
- While the VR view is streamed: Monado makes NV12 on the GPU and q1screencast copies it as it is (2 ms a frame)
  from its double buffer `/dev/shm/quest1-spectator` ("QPS2" header), each frame once the header's `seq` says it is
  complete. No X server in the path, nothing tears. After 0.5 s without a new one (headset off, Monado renders
  nothing) it reads the X screen again, where q1apps' cover is dark blue for the whole session, as the Frame's
  stream shows then. q1apps no longer paints the VR view on the cover (36 XShmPutImage of 3.7 MB a second saved).
- Otherwise (Steam's own screen): XShmGetImage of the root, 3.7-7.5 ms a frame, when the screen changed (XDamage on
  the root) or the pointer moved. Xlib queues the events read along with a reply (XQueryPointer), so they are
  drained on every tick: waiting on the socket alone missed them and gave 1 frame a second. Steam asks cursor mode
  "hidden" and draws the X cursor itself (rpspectator hides it while spectating).
- Tried and dropped: reading Xvfb's framebuffer file (`-fbdir /run/xvfb`, the XWD image Xvfb draws into) saved the
  3.7 ms but tore in the stream (user) whenever the screen was drawn during the read.
- The X screen is converted to NV12 with NEON: 3.4 ms on a big core, 7.1 ms on a little one, at 1280x720; memory bound (a 1.4 MB
  memcpy alone takes 0.6 ms on a big core). Steam's NV12 format asks for **BT.709 full range** (`pw-cli
  enum-params <steam node> EnumFormat`: colorMatrix 3 = BT709, colorRange 1 = 0-255). BT.601 limited range looked
  washed out on the PC, and declaring BT.601 in our format made the NV12 formats not match, so Steam took BGRx
  (colours right, Steam converting, less smooth).
- At most 60 frames a second, one a second when nothing changes.

Result in a real stream: "Capture method set to Desktop PipeWire NV12 + V4L2 HEVC", capture 3-4.5 ms a frame
(was 20-80), Steam ~100 % CPU (was ~250 %). "Smoother, not smooth enough" (user). The next limits measured then:
- The VR view itself: Monado's spectator copy ran at 16-21 frames/s, 27-37 ms a copy: its host buffer was in the
  Adreno blob's HOST_CACHED|HOST_COHERENT memory type, which is write-combined in fact (CPU reads ~220 MB/s, see
  docs/performance.md). It now takes the cached non-coherent type (~4 GB/s) and invalidates before each read
  (comp_window_mdp.c, `spect_init`/`spect_thread`): 4-7 ms a copy. It then ran at 24 a second: the 1/30 s period is
  checked once a vsync, and at 72 Hz that takes three (41.7 ms); now every second vsync, 36 a second.
  `5 * U_TIME_1S_IN_NS` also overflowed an int there (the idle timeout was 0.7 s, the log every 1.4 s).
- Encode 14-16 ms a frame in the stream (8 ms measured earlier with a quieter headset).

Then (2026-11-10) the VR view's NV12 is made on the GPU: one compute pass in Monado (`quest1_spect_spv.h`: crop,
bilinear scale and BT.709 full range in one go, 4x2 pixels an invocation) into a device buffer, a buffer copy into
cached host memory (the blob offers no cached type for storage buffers: written straight, the thread's read took
10.5 ms), then the thread's 1.4 MB memcpy into the file: **1.6 ms a frame** (was 28 ms). q1screencast copies it
(2 ms, was 11 with the conversion). Measured without the PC (a q1screencast session over busctl and a PipeWire
consumer, the spectator trigger made by hand): 35.5 frames a second, the most every second vsync gives; the user
asks for 30 steady, no more, for the spectator (more is for the VR/2D streaming later).

Layers per frame now, VR view to encoder: GPU pass + buffer copy (Monado), memcpy into `/dev/shm/quest1-spectator`,
memcpy into a PipeWire buffer (q1screencast), Steam's copy into the V4L2 buffer, venus.

### The decoder: FFmpeg's v4l2m2m decoders on venus (2026-11-10)

`src/v4l2shim/quest1_vdec.c`, in the same `libquest1_v4l2.so` behind the same hooks (ioctl, mmap, close), is the
mainline stateful decoder API on `/dev/video32` as libavcodec's `*_v4l2m2m` decoders use it; its header lists what
libavcodec does and what msm_vidc wants instead. `q1-hwdec <program> ...` preloads the library (Steam already has
it). In short:
- MMAP buffers are ION buffers given to the driver as USERPTR, **cached** ones (the driver cleans and invalidates
  the caches itself; frames read from uncached ION memory were what made vdectest's linear runs slow).
- The firmware's "insufficient" event at the first sequence header (it then waits) is the driver's whole
  reconfiguration, done inside the library with the client's buffers: flush, flush-done, every buffer back,
  STREAMOFF, REQBUFS 0, S_FMT, REQBUFS, QBUF, STREAMON. The client only sees a SOURCE_CHANGE that changes nothing.
  Another size than the one announced is a real SOURCE_CHANGE (flushed here, the client's STREAMOFF/REQBUFS/STREAMON
  do the rest).
- The frame is venus's own layout with no copy: G_FMT says height = scanlines (so the client puts the UV plane at
  stride * scanlines, where venus writes it) and G_SELECTION says the picture (1920x1080 in 1920x1088).
- Packet buffers: the driver wants 12 MB each whatever the stream (and libavcodec asks for 16): limited with the
  driver's BUFFER_SIZE_LIMIT control to half a raw frame (1.5 MB at 1080p). 20 frame buffers of 3 MB at 1080p.
- DECODER_CMD STOP is an empty packet with the EOS flag; the frame buffer with the EOS flag is the LAST one.

Measured (ffmpeg 8, `-c:v h264_v4l2m2m` against `-c:v h264`, the md5 of every frame as NV12): **bit-exact** for
1920x1080 H.264 (no B-frames), 1920x1080 HEVC, 1280x720 H.264 with B-frames and 854x480 H.264 (stride 896, picture
854 in 864). 300 frames of 1080p: H.264 0.9 s of CPU against 3.75 s in software, HEVC 0.8 s against 5.9 s; flat
out about 150 frames a second (2.07 s with the start), with SteamVR and the dashboard running. Stopped early,
killed (`kill -9`: the driver logs "Failed to move from state" and recovers), two decodes at once: all fine, and
the next decode is bit-exact. VLC 3 (its own libavcodec 58) decodes with `q1-hwdec vlc
--avcodec-codec=h264_v4l2m2m` (that option names one decoder: HEVC stays in software with it).

Limits: a size change in the middle of a stream hangs **ffmpeg itself** (libavcodec waits for every frame it gave
out before it frees the capture buffers, and ffmpeg's filter thread keeps one; the library has flushed and sent
SOURCE_CHANGE by then). A client that waits for SOURCE_CHANGE before it sets CAPTURE up (the mainline order) gets
nothing: the driver starts the firmware session only when both queues stream. 8-bit only (NV12). No dma-buf
export yet (Steam's stream client takes its CAPTURE buffers as dma-buf fds, section 1).

Nothing picks these decoders by itself: libavcodec's default for H.264 is its software decoder, and it has no
fallback from one decoder to another, so making the hardware one the default for every program would break
10-bit and other streams venus refuses. Chromium (Arch build) decodes with VA-API only.

VLC is the exception (user, 2026-11-10: hardware with a software fallback): its launcher is `q1-vlc`
(`/usr/local/share/applications/vlc.desktop`), which looks at the file VLC is opened with (ffprobe) and adds
`--avcodec-codec=h264_v4l2m2m` or `hevc_v4l2m2m` and the library when the video is 8-bit progressive H.264 or
HEVC up to 4096x2304, and starts plain VLC otherwise. Checked: 8-bit H.264 (a `file://` URL with spaces) and HEVC
on venus, 10-bit H.264 and VP9 in software. The choice is made once, at the start: another codec opened later in
the same VLC plays in software (VLC ignores the name then), but a 10-bit stream of the same codec would not play.

`omxprobe.c` can run the vendor decoder the same way as it did the encoder, to compare firmware logs.

## 1. 2D streaming (Steam Remote Play / Steam Link)

How the Frame does it: Steam client -> `streaming_client` (arm64) with V4L2 decode (`CV4L2Accel`, `/dev/video-dec%d`), output through DRM/EGL/Vulkan, an FFmpeg software
fallback, codecs H264/HEVC/AV1. In VR it is an SDL3 OpenVR overlay (a virtual screen); Big Picture's "Stream" button starts it.

Here today (from `/root/stream-crash-20261101`, `-20261102` and the live `streaming_log.txt` on the headset):
- Discovery, pairing, session start, audio (Opus) and input forwarding work; the PC encodes and sends (x264 main, 1280x800 capture, 1920x1080 overlay).
- Then the picture is **black** and the SteamVR tree can crash. Cause [H]: streaming_client's OpenGL path needs `GL_EXT_memory_object(_fd)` and
  `GL_EXT_semaphore(_fd)` to share the image with vrcompositor (`sharedresource_linux.cpp:208-224` ASSERTs) and libhybris GL does not expose them. The Vulkan path is off
  (`CVulkanAccel: Not using a Vulkan renderer`). V4L2 decode is not tried because `/dev/video-dec0` does not exist. Which decoder the headset client really picks was not captured.
- Blockers, in order: (a) sharing the image with the compositor (GL memory objects missing; Vulkan has opaque fd external memory but no dma-buf or modifiers);
  (b) the V4L2 decoder node and msm_vidc quirks; (c) the crash in the SteamVR tree.

Plan (hardware decode only: software decode is not an option, user decision 2026-11-07):
1. ~~Standalone V4L2 test on venus~~ **done 2026-11-07** (`src/vdectest`, `run-tests.sh`): bit-exact, 9-11 ms per 1080p60 frame (above).
2. **`/dev/video-dec0` for Steam** (1-3 days): the Frame's own node name (`steam-video-codec-setup.service` makes it), then see how
   `CV4L2Accel` copes. The translation exists since 2026-11-10 for libavcodec (section 0, "The decoder"); Steam's client
   also wants its CAPTURE buffers as dma-buf fds (EXPBUF of the ION buffers, to see with strace on a real stream) and may
   set CAPTURE up in another order. The node is not made yet: it changes what the stream client does, so it comes with
   a PC stream to test it.
   [H] 2026-11-07, from `steamrtarm64/streaming_client` (2.17.10, not stripped, strings and imports only): `CV4L2Accel`
   (`/data/src/streaming/client/v4l2accel.cpp`) opens `/dev/video-dec%d`, **mmaps its OUTPUT buffers** ("Couldn't memory map
   output buffer"), takes CAPTURE buffers as **dma-buf fds** ("capture planes has invalid m_fd"), expects mainline behaviour
   (`V4L2_BUF_FLAG_LAST`, "Resetting capture buffers because we had last buffer flagged"), rewrites the SPS ("Couldn't rewrite
   SPS packet": low-latency VUI), reads `STEAMLINK_V4L2_BUFFER_COUNT` / `STEAMLINK_V4L2_RESET_AFTER_LAST_BUF`, and hands the
   frames to DRM, EGL or Vulkan (`CDRMFrames`/`CEGLFrames`/`CVulkanFrames<V4L2FrameContext_t>`). It calls libc's `open`,
   `ioctl`, `mmap` and `poll`, so an LD_PRELOAD shim can sit between it and msm_vidc: MMAP -> ION-backed USERPTR,
   EXPBUF/dma-buf -> the ION fds, the msm_vidc events -> `V4L2_EVENT_SOURCE_CHANGE`, EOS flag -> `V4L2_BUF_FLAG_LAST`,
   the coded-size and reconfiguration rules of `src/vdectest`. The frame output (EGL dma-buf import or Vulkan modifiers on
   libhybris) is step 3's problem.
3. **Decoded frames to the compositor** (3-5 days): the decoder's dma-buf/ION buffers as AHBs (Monado's `comp_window_mdp.c` already
   imports gralloc AHBs), so the frames reach vrcompositor without a CPU copy; `GL_EXT_memory_object` is missing on libhybris GL,
   so the Vulkan path (streaming_client's `CVulkanAccel`) or an overlay from AHB-backed images.
4. Fallback: a Moonlight-style client on the same V4L2 decode, as an OpenVR overlay. Not Steam-integrated, so last.

## 2. PC VR streaming (Steam Link VR / vrlink)

How the Frame does it: the Steam client starts `/opt/steamvr/tools/vrlink/bin/linuxarm64/run_vrlink.sh -I <steamid> -G <app> -A <app> -C <id> -c <ip> ...` (the `-c` list is every
address of the PC, virtual adapters included). vrlink is a normal OpenVR scene app on the headset's own SteamVR: it connects over UDP (FEC, encrypted),
receives HEVC through `SVLCodecV4L2`, imports the decoded buffers into Vulkan (dma-buf + modifiers) and submits them as layers. The PC runs `driver_vrlink`.

Here today: [H] Steam starts vrlink and it connects to vrserver (`vrlink.client`, legacy bindings loaded, `quest1_hmd`), but **it exits after about 15 s** each time
(19:15:45 to 19:16:00 and 19:16:17 to 19:16:32 on 2026-11-01; no WaitGetPoses, no layers), and the PC's SteamVR keeps waiting. The timer suggests it never finishes decoder and renderer
init, most likely because of (a) no `/dev/video-dec0` (SVLCodecV4L2 CaptureSetup/S_FMT error strings are in the binary) and (b) the Vulkan dma-buf and modifier requirement
(`SVLRendererVulkan`). Its stdout and stderr are not in the saved logs; getting them is step 0.

Plan:
0. **Capture vrlink's own output** (0.5 day): run `run_vrlink.sh -c <PC ip>` by hand with output to a file while a PC session waits. It connects to the running SteamVR as a
   scene app and may take over the headset scene, so ask the user first.
1. **`/dev/video-dec0` symlink and a decode test** (1 day) to see how far V4L2 init goes on msm_vidc.
2. **Vulkan dma-buf import shim** (1-2 weeks, [L]): map ION/dma-buf fds to AHBs (Monado's `comp_window_mdp.c` already handles gralloc AHBs), advertise
   `EXT_external_memory_dma_buf` and `EXT_image_drm_format_modifier` (linear only). It is the same blocker as the flat stream's compositor sharing, so do it once for both.
3. Alternative: ALVR or WiVRn (PC-side driver + headset client with V4L2 or software HEVC decode). 1-2 weeks, avoids Valve's stack but loses Steam integration.
Not worth pursuing: patching vrlink (internal interfaces `IVRMailbox`, `IVRBlockQueue`).

## 3. PC overlays (desktop and windows in the headset)

How the Frame does it: [H] Steam Link's own desktop streaming (the same `streaming_client` overlay, "Desktop mode", `--windowed`) and, during a VR link session,
vrlink's `desktop_windows_remote` / `desktop_windows_tx` paths and `VRLink_LaserMouseOverlay` (laser-pointer mouse for PC windows). SteamVR's own VRMonitor desktop
view is not started on this platform ("Not starting VRMonitor by default on this platform"). A Linux PC needs Steam with `-pipewire` for desktop capture.

Here: it is the 2D streaming path plus the laser-mouse overlay, with no extra blockers of its own. The PC side already negotiates and encodes the desktop (see the logs).
So it is solved by plans 1-3 of section 1 (hardware decode, then the frames to the compositor). Effort: about 1 more day for the laser pointer input (controllers are Oculus Touch with the Frame's dashboard actions since 88f7fd6).

## Ranking

| # | Item | Effort | Gain |
|---|---|---|---|
| 1 | ~~venus hardware decode: standalone V4L2 test~~ done: bit-exact, 9-11 ms per 1080p60 frame | - | the base of every path |
| 2 | vrlink's stdout and exit reason, plus the `/dev/video-dec0` symlink test | 1-2 days | decides the VR path |
| 3 | `/dev/video-dec0` and `CV4L2Accel` on msm_vidc, then zero-copy frames to the compositor | 1-2 weeks | 2D games and the PC desktop in VR |
| 4 | Vulkan dma-buf import shim | 1-2 weeks | proper VR streaming and compositor sharing |
| 5 | ALVR or WiVRn alternative | 1-2 weeks | fallback if vrlink stays blocked |


## Notes moved from the to-do list (2026-11-08): Streaming

History of measurements, fixes and open points as they were written in docs/todo.md (the to-do list now only
lists what is left to do).

- Steam Link streaming from a PC fails (2026-11-01): VR streaming leaves the PC's SteamVR waiting
  for the connection and the stream app on the headset closes; a 2D game started in cinema mode,
  its audio stuttered, then the headset's SteamVR crashed. Logs saved on the headset in
  `/root/stream-crash-20261101/`.
- Again 2026-11-02 20:29 (user): a SteamVR-tree process crashed (`/tmp/dumps/crash_20261102202929_2.dmp`,
  libllvm-qgl mapped), Steam shut down and restarted, vrserver quit with it and nothing restarts
  it, so the headset stays black. The stream's `streaming_client` (GL) can't share images with the
  compositor: libhybris GL lacks GL_EXT_memory_object(_fd) and GL_EXT_semaphore(_fd) (ASSERTs in
  sharedresource_linux.cpp), so the stream shows black. Logs in `/root/stream-crash-20261102/`.
  SteamVR now restarts by itself when it quits.
