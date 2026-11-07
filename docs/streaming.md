# Streaming survey (2D Remote Play, PC VR, PC overlays)

Status 2026-11-06: feasibility survey with read-only probes. Nothing here was made to work. The "Frame" parts come from files on the headset
(`docs/steamframe-stack.md` has the long version). Tags: [H] measured or read, [M] likely, [L] guess.

## What the headset has

- Video block (probed): `/dev/video32` = `msm_vdec_8974`, `/dev/video33` = `msm_venc_8974` (downstream msm_vidc on `cc00000.qcom,vidc`, multiplanar M2M).
  [H] Decoder OUTPUT formats: H264, MVC, **HEVC**, HEVC hybrid, VP8, VP9, MPEG-2/4, H263, VC-1, DivX. CAPTURE: NV12, NV12 UBWC (Q128), TP10 UBWC.
  Frame sizes 16x16 to 5760x5760. Encoder CAPTURE: H264, HEVC, VP8. Opening and closing instances works (dmesg `msm_vidc: Opening video instance`).
  No actual decode has been completed yet.
- [H] No `/dev/video-dec0` / `/dev/video-enc0` (the Frame's `steam-video-codec-setup.service` makes them from `vrdevice_path`; it does not exist here).
  Steam's `streaming_client` logs `CGameStreamVideoStageV4L2::BInitEncoder: Failed to open device: /dev/video-enc0`.
- [H] ffmpeg has `h264/hevc/vp9_v4l2m2m` wrappers, but `ffmpeg -c:v hevc_v4l2m2m` fails here (`Could not find a valid device`, EBUSY). It probes every
  `/dev/video*`, including the msm camera nodes (querycap on them logs harmless kernel WARNINGs `Bad caps for driver`, v4l2-ioctl.c:1039).
  GStreamer has no `v4l2*dec` elements (only src/sink/convert).
- [H] Software decode is fine for flat streams: 1280x720 HEVC and H.264 each decode at about 100 fps (ffmpeg libavcodec, `-f null`).
  1080p60 H.264 should be OK; 4K or 120 fps is not.
- [H] Network: wlan0 5 GHz VHT 80 MHz NSS2, -43 dBm, link rate up to 866 Mbit/s (tx). Bandwidth is not the limit.
- [H] Steam (arm64) has `streaming_client` 2.17.10, which runs as an OpenVR overlay inside SteamVR (`vrclient type=VRApplication_Overlay`, `--openvr`).
- [H] Vulkan on the headset: streaming_client reports `VK_KHR_external_memory_fd`, `external_semaphore_fd` and `timeline_semaphore: TRUE`, but
  `vkGetPhysicalDeviceFormatProperties2 returned zero modifiers ... Supports dmabuf formats + modifiers? - No!`.

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

Plan (cheapest first):
1. **Software decode, picture to the compositor through a path our Vulkan supports** (3-5 days, [M]). Force streaming_client's FFmpeg path (setting name still to find in
   its strings or the Advanced Streaming settings), then avoid GL sharing (a Vulkan layer or LD_PRELOAD that answers "no dma-buf modifiers" with a linear fallback,
   or opaque `external_memory_fd`). Costs about one CPU core at 1080p60 H.264.
2. **Own overlay player** (3-5 days, [M]): a small OpenVR overlay app (we already have `q1-camoverlay`) that takes decoded frames (streaming_client `--windowed` output
   or a PipeWire stream) and sets them as an overlay texture from raw memory (`SetOverlayRaw`). Avoids all GL and dma-buf sharing.
3. **Hardware decode via venus** (3-6 days, [L]): symlink `/dev/video-dec0` -> `/dev/video32` (the Frame's own mechanism) and see how `CV4L2Accel` copes with the
   downstream msm_vidc (UBWC capture formats, buffer counts, event and DISPLAY_DELAY controls). Start with a standalone V4L2 test. dma-buf export from ION may not
   fit the Vulkan import without a shim.
4. Fallback: a Moonlight-style client (ffmpeg + V4L2) as an OpenXR or OpenVR quad app. Not Steam-integrated, so last.

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
So it is solved by plans 1 and 2 of section 1. Effort: about 1 more day for the laser pointer input (controllers are Oculus Touch with the Frame's dashboard actions since 88f7fd6).

## Ranking

| # | Item | Effort | Gain |
|---|---|---|---|
| 1 | Software decode and a raw-memory overlay path for flat streams (sections 1 and 3) | 3-5 days | 2D games and the PC desktop in VR |
| 2 | vrlink's stdout and exit reason, plus the `/dev/video-dec0` symlink test | 1-2 days | decides the VR path |
| 3 | venus hardware decode: standalone V4L2 test, then `CV4L2Accel` | 3-6 days | lower CPU and latency |
| 4 | Vulkan dma-buf import shim | 1-2 weeks | proper VR streaming and compositor sharing |
| 5 | ALVR or WiVRn alternative | 1-2 weeks | fallback if vrlink stays blocked |
