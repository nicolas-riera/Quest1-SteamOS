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

### The encoder's frame rate: 60 at most (2026-11-10)

Steam feeds the encoder far more frames than the picture has: the PC asks for its monitor's rate (165 Hz) and
Steam's limiter only steps the target down, by whole divisors, when a stage is slow (126 fps on average in one
session, 55 <-> 82 fps back and forth in another). Each frame costs two 1.4 MB copies and an encode.
`quest1_v4l2` now gives the bitstreams out no closer than 1/60 s (`ENC_MIN_PERIOD_NS`): the encode stage then takes
that long and Steam feeds it at that rate, with no delay added when the frames come slower. Measured live in a
720p HEVC session with the PC (the delay set from a file, 20 s each): Steam's process 56 % of a core with no cap
(about 100-110 frames a second, from venus's interrupt count), 44 % at 60, 35 % at about 55 (every bitstream held
14 ms), 32 % at about 40 (held 20 ms). Steam logs "Slow framerate ... (encode)" once a second and does nothing else:
"V4L2: Hardware encoding is too slow, disabling" did not come in 6 minutes of it. The user found the picture on the
PC as good at 60. The encode is so not the main cost of the stream: the capture and Steam's composition are the rest.

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

### Decoded frames to the GPU with no copy: proven (2026-11-10)

The two halves of the path a video frame takes from venus to the compositor, each checked on the headset:
- **venus decodes straight into gralloc buffers** (`vdectest -g`): AHardwareBuffers from Android's libnativewindow
  through libhybris, format `Y8Cb8Cr8_420` (0x23) with the usages GPU_SAMPLED_IMAGE + VIDEO_ENCODE, which Qualcomm's
  gralloc lays out as NV12 "venus" (handle format 0x102, stride = width aligned to 128, scanlines = height aligned
  to 32, 3153920 bytes at 1920x1088: exactly the size and layout msm_vidc asks for on CAPTURE). Their ION fd (the
  native handle's first) goes to the driver as the USERPTR buffer, as Android's decoder does. 120 frames of
  1080p H.264: bit-exact against ffmpeg.
- **the Adreno blob samples such a buffer** (`vkahbtest`): imported with
  VK_ANDROID_external_memory_android_hardware_buffer as an external-format image (external format 0x1fa, no Vulkan
  format) with a sampler YCbCr conversion, read by a compute shader. Four flat colours written by the CPU come out
  exact over the whole 1920x1088 picture. The driver suggests BT.601 narrow range for the conversion whatever is in
  the buffer: the client sets the model and range of the stream (BT.709 for HD). Linear filtering is offered. The
  image is taken over with a barrier from VK_QUEUE_FAMILY_FOREIGN_EXT so its content is kept.

**At 72 frames a second next to the compositor** (2026-11-10, panels on with `QUEST1_IGNORE_PROX=1`, SteamVR with
the dashboard at 71.8 fps, `vdectest -g -R -r 72`: a live stream paced at 72 fps into gralloc buffers, 15-40 Mbit/s
test pattern, no B-frames):

| stream | decoded | latency QBUF to frame | vrcompositor meanwhile | Monado late |
|---|---|---|---|---|
| H.264 1920x1080 | 720/720, 72.0 fps | 11.5 ms | 71.9 fps, 13.5 ms a frame | 0 |
| HEVC 1920x1080 | 720/720, 72.0 fps | 8.6 ms | 71.7 fps | 0 |
| H.264 2880x1600 (both panels) | 360/360, 72.0 fps | 16.9 ms | 71.8 fps | 0 |
| HEVC 2880x1600 (both panels) | 360/360, 72.0 fps | 14.2 ms | 71.6 fps | 0 |

The test program took 13-15 % of a core (it copies each packet into the decoder's buffer; the frames are not
touched). So venus holds the VR stream at the panels' size and rate (user's requirement, 2026-11-10) while the
compositor keeps its rate; at that size the decode takes about one refresh period (13.9 ms), HEVC a little less.
Not measured yet: real game content at PC VR bitrates (50-100 Mbit/s), and the GPU pass that draws the frames.

So a stream client of ours (or a layer under Valve's) can show decoded frames with no CPU copy and no colour
conversion on the CPU: decode into AHBs, import each once, sample it in the pass that draws the overlay or the
layer. What is left is the consumer: Valve's `streaming_client` wants dma-buf import with modifiers in Vulkan
(the layer would have to turn that into AHB imports), or our own client on the same pieces.

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

### 2026-11-10: the client starts, on the GPU; sound and theater mode work; the picture is still black

Tested with a real stream from the PC (a 2D game, 1920x1080). How to look at it: the client writes its own log to
`/tmp/streaming_client.log` (the run before in `.log.last`), the PC's Steam logs what the client tells it in
`logs/streaming_log.txt` ("CLIENT:" lines), and the client can be started by hand with Steam's environment and its
command line (`/proc/<pid>/cmdline`, which holds the account's id: never in the repo) under gdb or strace: it goes
as far as its renderer and the shared texture without a session.

- **Why it closed at once**: SDL's OpenVR video driver makes a desktop OpenGL context through the system's EGL
  (libEGL.so.1 is linked into libSDL3: SDL's library hints do not apply), so Mesa's llvmpipe; vrclient then shares
  the overlay's texture with vrcompositor through `GL_EXT_memory_object_fd` and stops on an ASSERT
  (`sharedresource_linux.cpp:208`, SIGTRAP) when the functions are not there. The `MESA_EXTENSION_OVERRIDE` of
  steam.service hides them on purpose (the webhelper then copies through the CPU), and llvmpipe could not import the
  Adreno's memory anyway.
- **Fix** (`src/v4l2shim/quest1_streamclient.c`, `quest1_angle_egl.c`): Steam ships ANGLE next to the client (OpenGL
  ES on Vulkan), and on the Adreno's Vulkan it has `GL_EXT_memory_object_fd` and `GL_EXT_semaphore_fd`. The client
  starts itself again once with `/usr/local/lib/quest1-streamclient` first in its library path; the `libEGL.so.1`
  and `libGL.so.1` there hand everything to ANGLE, turn the desktop context into an OpenGL ES 3.1 one and add the
  three desktop-only things SDL's driver and vrclient use (`glCheckNamedFramebufferStatus`, an unsized depth
  renderbuffer, the extension functions asked through `glXGetProcAddress`). vrclient is loaded so that it sees its
  own libGL/libEGL first, so a preloaded library does not reach it: hence the library path. Result: "Created
  opengles2 renderer on openvr", "Created remote GL texture ... 1920 x 1080", "Created remote Vulkan image": the
  overlay's picture is shared with the compositor on the GPU, with no copy.
- The semaphore between Vulkan and OpenGL is refused ("Failed to get cross-process semaphore FD -1000072003",
  VK_ERROR_INVALID_EXTERNAL_HANDLE): vrclient falls back to `glFinish()`. To look at.
- Mesa's Zink (OpenGL on Vulkan) was tried first: it reaches the blob and fails to make its screen with no message
  (the driver has Vulkan 1.1.128, no `VK_KHR_imageless_framebuffer`, no `logicOp`). Not pursued.
- **Why the picture is black**: "WARNING: No supported video codec, will not stream video" in the client's log, and
  "Allowed Codecs:" empty on the PC (which then encodes x264 for nobody). This arm64 client has no software
  decoder. From its disassembly (read only): a codec is supported when (1) its DRM object has a device: libdrm's
  `drmGetDevices2` gives a device with a primary node, `open` works, `drmModeGetResources` gives at least one CRTC
  and the client finds a connector and mode on it (0x389900, 0x389690); `/dev/dri` is empty here (the Android
  kernel's display is not DRM); and (2) a V4L2 device among `/dev/video-dec%d` then `/dev/video%d` answers
  `VIDIOC_QUERYCAP` with `V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING` and lists H264 or HEVC in `VIDIOC_ENUM_FMT`
  on the OUTPUT queue (0x420350). The client never got to (2): no video device was opened.
- The sound of the stream stutters (user): not looked at yet; the client could not raise its threads' priority
  ("setpriority() failed").
- Remote Play's encoder side is not touched by any of this.

### 2026-11-11: the picture shows, decoded by venus

The user saw the PC's game in the headset (theater mode, sound): "ça fonctionne". Performance is "ok" while nothing
else runs on the headset and drops as soon as something else does. What it took, in the order the problems came:

- **A DRM display in name only** (`src/v4l2shim/quest1_streamdrm.c`, the client's `libdrm.so.2` in
  `/usr/local/lib/quest1-streamclient`): one connected panel, one CRTC, a 1920x1080@72 mode, on `/dev/null`;
  everything that would draw on it fails. Dumb buffers are real: `DRM_IOCTL_MODE_CREATE_DUMB` allocates from ION's
  system heap and `PRIME_HANDLE_TO_FD` gives its fd, because the client makes the decoder's frame buffers there.
  `drmGetVersion` answers NULL (ANGLE asks). The client then lists H.264 to the PC and opens the decoder.
- **`/dev/video-dec0`** (udev rule in `device/holo-steam-session.sh`): without it the client asks every
  `/dev/video<n>` and each camera node logs a kernel warning.
- **CAPTURE buffers as the client's dma-bufs** (`quest1_vdec.c`): REQBUFS with `V4L2_MEMORY_DMABUF` on CAPTURE, the
  client's fds (ION) mapped and queued to the driver as they come. The client keeps a fixed table of 16 frame
  buffers: a larger count from the driver (18) runs past it (SIGSEGV in `BSubmitFrame`), so the client's count is
  kept, which venus decodes fine with.
- **The first frame**: the client waits 30 ms for it after the first packet and otherwise starts the decoder's
  queues all over, which msm_vidc does not survive ("Core is in bad state"). The firmware always asks for its
  reconfiguration at the first packet, and the shim only did it when the client called it. Now a thread of the
  shim takes the driver's events as they come (reconfiguration with the same size is done there and the client is
  told nothing: it sets CAPTURE up again at any source change), and `poll()` on the decoder waits up to a second
  after the first packet for the first frame.
- **Reading a frame**: with no `EGL_EXT_image_dma_buf_import` the client uploads each frame from the CPU, through a
  read-only **private** mapping of a `dup()` of its dma-buf, which ION refuses (EINVAL; the client then gives up on
  hardware decoding and, having no software decoder, disconnects). The shim's `mmap` retries such a refused mapping
  as a shared one (the same thing when nothing writes). `DMA_BUF_IOCTL_SYNC`, which this kernel lacks, is answered
  with success (the driver gave the frame back ready for the CPU), so the client does not log each frame.
- **The window was transparent**: ANGLE on the Adreno driver loses every draw, with no GL error, while its
  `supportsExtendedDynamicState` feature is on (the dynamic vertex-binding stride draws nothing; already known from
  `test_angle_x11.c`). Only clears worked: black before the video, then nothing. The client is started with
  `ANGLE_FEATURE_OVERRIDES_DISABLED=supportsExtendedDynamicState` (`quest1_streamclient.c`). Found with a small SDL3
  test on the client's own SDL and ANGLE: a rectangle, an RGBA texture and an NV12 texture all came out empty before,
  right after.
- The compat layer's linear tiling for CPU-written camera images (the input-attachment usage mark) now applies in
  vrcompositor only: ANGLE asks that usage for its own textures.
- vrclient asks `eglGetProcAddress("pglFramebufferRenderbuffer")` (its pointer's name): answered with
  `glFramebufferRenderbuffer`. `QUEST1_ANGLE_EGL_TRACE` also traces vrclient's copy into the shared texture (a pixel
  before and after `glBlitFramebuffer`).

Still open: when the PC's game window is minimised the stream's size changes (1920x1080 to 270x1080) and the decoder
does not survive it (the client restarts its queues by itself after the driver's event); the sound stutters; the
Vulkan/OpenGL semaphore is refused (glFinish).

### 2026-11-11: the frames reach the GPU with no copy

The user: performance "pas mal" now, where it used to fall apart as soon as anything else ran. The client takes its
own zero-copy path (`CEGLFrames`): per decoder buffer, one SDL NV12 texture whose two OpenGL textures each get an EGL
image of one plane of the dma-buf (`EGL_LINUX_DMA_BUF_EXT`, `DRM_FORMAT_R8` for Y and `DRM_FORMAT_GR88` for UV, fd,
offset, pitch), with `glEGLImageTargetTexture2DOES`, and SDL's NV12 shader samples them. Pieces:
- **`EGL_EXT_image_dma_buf_import` in the client's EGL** (`quest1_angle_egl.c`): ANGLE has it only on a Vulkan with
  dma-buf and DRM modifiers, which the Adreno blob lacks. The shim adds it to `eglQueryString` and makes each plane
  itself: the dma-buf is imported with `GL_EXT_memory_object_fd` as a linear texture (`glTexStorageMemFlags2DANGLE`),
  which is the source of a real ANGLE EGL image (`EGL_GL_TEXTURE_2D_KHR`). A plane whose pitch is not the width
  times the pixel size (odd sizes such as 270x1080) is refused: the client copies by CPU then, as before.
- **The blob refuses an ION dma-buf as `OPAQUE_FD` memory** (`VK_ERROR_INVALID_EXTERNAL_HANDLE`), **and closes the
  fd anyway**. The compat layer (`quest1_dmabuf.c`) keeps a duplicate of the fd and, on that refusal, imports the
  same dma-buf as a gralloc buffer: `AHardwareBuffer_createFromHandle` on the native handle of a small BLOB buffer of
  its own with the fd and size replaced. The blob binds plain (non-external) linear images to such memory at any
  offset, so linear images made for `OPAQUE_FD` memory are created plain (an `OPAQUE_FD` image refuses AHB memory, an
  AHB image wants a dedicated import of its own format). Tried on the headset with `vkimp`/`vkblob` tests first.
- **ANGLE binds at offset 0** whatever offset it is given: the UV plane came out as Y. The shim chains a structure of
  ours (`0x51310001`, the plane's offset) to ANGLE's `VkImageCreateInfo` through `glTexStorageMemFlags2DANGLE`; the
  layer takes it out and binds the image at that offset (`vkBindImageMemory`, `vkBindImageMemory2`).
- **`glActiveTextureARB`**: the client asks for it, logs "falling back to CPU texture upload" when it is missing,
  and calls it anyway (a jump to address 0). `eglGetProcAddress` now gives the core function for an `ARB` name.
- Every Vulkan device gets `VK_ANDROID_external_memory_android_hardware_buffer` and `VK_EXT_queue_family_foreign`
  (only direct scanout had them).
- `src/v4l2shim/test_dmabuf_egl.c` checks the whole path as the client uses it: an ION buffer in venus's NV12 layout,
  both planes read back exact at 1920x1080, 1280x720 and 3840x2160, again after the buffer changed, and 270x1080
  refused.

Measured during the user's stream (1080p): the client at about half a core (decoder thread 17 %, main thread 16 %,
network 9 %). A quarter of a core went to msm_vidc's cache maintenance of the frame buffers (cached ION, cleaned and
invalidated on each QBUF), which the CPU no longer reads.

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
