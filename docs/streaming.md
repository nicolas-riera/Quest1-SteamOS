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
- [H] ffmpeg has `h264/hevc/vp9_v4l2m2m` wrappers, but `ffmpeg -c:v hevc_v4l2m2m` fails here (`Could not find a valid device`, EBUSY). It probes every
  `/dev/video*`, including the msm camera nodes (querycap on them logs harmless kernel WARNINGs `Bad caps for driver`, v4l2-ioctl.c:1039).
  GStreamer has no `v4l2*dec` elements (only src/sink/convert).
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
from QBUF to bitstream, HEVC 9 ms; both decode cleanly in ffmpeg with the right colours. Left: a real stream
(Steam picks the encoder only when a session starts), then the capture's cost.

Notes for the decoder (`/dev/video-dec0`, Steam Link to the headset): the same layer is the place for it (same
buffer translation; vdectest.c lists the decoder's own rules), and `omxprobe.c` can run the vendor decoder the
same way to compare firmware logs.

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
   `CV4L2Accel` copes with msm_vidc (UBWC capture formats, buffer counts, events, DISPLAY_DELAY controls). If the gaps are small, a
   V4L2 shim (LD_PRELOAD on streaming_client) translates; else patch the kernel driver toward the mainline stateful decoder API.
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
