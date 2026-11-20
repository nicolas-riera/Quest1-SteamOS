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

The user saw the PC's game in the headset (theater mode, sound): "it works". Performance is "ok" while nothing
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

The user: performance "not bad" now, where it used to fall apart as soon as anything else ran. The client takes its
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
invalidated on each QBUF), which the CPU no longer reads: `quest1_streamdrm.c` allocates them uncached since. With
that, at 60 decoded frames a second (`perf stat -e v4l2:v4l2_dqbuf`: 300 frames in 5 s), the client takes 17 % of a
core in all (decoder thread 6 %, network 4.6 %, main thread 2.4 %, mostly waiting on fences: vrclient's glFinish).
The CPU copy the client falls back to for odd sizes reads uncached memory now, slower.

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
receives HEVC through `SVLCodecV4L2` into frame buffers it makes in Vulkan and exports (dma-buf + modifier), and hands those to vrcompositor
(`IVRIPCResourceManagerClient::ImportDmabuf`), which shows them. The PC runs `driver_vrlink`.

Here today: [H] Steam starts vrlink and it connects to vrserver (`vrlink.client`, legacy bindings loaded, `quest1_hmd`), but **it exits after about 15 s** each time
(19:15:45 to 19:16:00 and 19:16:17 to 19:16:32 on 2026-11-01; no WaitGetPoses, no layers), and the PC's SteamVR keeps waiting. The timer suggests it never finishes decoder and renderer
init, most likely because of (a) no `/dev/video-dec0` (SVLCodecV4L2 CaptureSetup/S_FMT error strings are in the binary) and (b) the Vulkan dma-buf and modifier requirement
(`SVLRendererVulkan`). Its stdout and stderr are not in the saved logs; getting them is step 0.

### 2026-11-11: vrlink connects to the PC

vrlink writes its own log, `~/.local/share/Steam/logs/vrlink.txt` (the PC's side is `Steam/logs/driver_vrlink.txt`). In order, what stopped it:
1. `SVLCodecV4L2::InitializeDevice: Failed to get video device path [38]`: it finds its decoder with `popen("vrdevice_path video_decode")`, a
   Frame tool. `device/holo-steam-session.sh` installs a `/usr/local/bin/vrdevice_path` that knows `video_decode` and `video_encode` and
   fails for the rest (`model`, `imu_sync_clk`, ... as before, when it did not exist).
2. `vkCreateDevice failed with error -7`: its device needs `VK_EXT_external_memory_dma_buf` and `VK_EXT_image_drm_format_modifier`. The
   layer has them for vrlink only (`src/vklayer-steamvr/quest1_modifier.c`): the linear modifier, the dma-buf imported as a gralloc BLOB
   buffer (`quest1_dmabuf.c`), and plane layouts that must be the blob's own linear ones. `test_modifier` (same directory): a one-plane
   image on a venus-like ION buffer reads back exact. **NV12 does not fit**: venus rows are 128-byte aligned and its UV plane follows
   the rows rounded up to 32, the blob's linear NV12 has neither; the blob samples venus NV12 only as a gralloc buffer with an Android
   external format (`src/vdectest/vkahbtest.c`: Vulkan format 0, external format 0x1fa). So for vrlink an NV12 image with the linear
   modifier is created with that external format, its dma-buf imported (dedicated) as a gralloc NV12 buffer (a handle of our own of
   the same frame with the fd replaced), and its YCbCr conversions and views take the external format. gralloc only takes a dma-buf
   whose size is exactly the one it gives the frame (venus's `VENUS_BUFFER_SIZE` with the video usage: the planes, 4096, and 16 kB or 8
   rows of metadata room), which the decoder's buffers are. `test_modifier`: NV12 at 1920x1080, 2880x1600, 1832x1920 and 1024x4096
   sampled through an NV12 conversion give BT.709's colours.
3. With both, `Init complete`: vrlink links to the PC (UDP over the PC's hotspot), sends the chaperone, gets a session id; then the PC
   sends nothing and vrlink quits after 15 s ("Timed out getting information from the server"; it then crashes on its way out).
   The PC got a Valve Index: vrlink ships default bindings for Knuckles and Frame controllers only, so SteamVR gives the Touch
   controllers the Knuckles ones and shows vrlink a simulated Index (model "Index", LHR-FFFFFFF0, Knuckles). Found by attaching gdb to
   vrlink and logging its `GetStringTrackedDeviceProperty` calls (it sends props 1000-1005, 1035-1037, 7000 of devices 0-2).
   `src/v4l2shim/quest1_vrlink.c` (in libquest1_v4l2.so, which Steam preloads, active in vrlink only) answers those with the Quest 1
   identity of the patched Android app (quest1-steamlink-patcher): "Oculus Quest" VRLINKHMDQUEST1 with VRLINKQ1 Touch controllers on
   SteamVR's `{oculus}` profiles. The PC then took them, but still activated no headset: "No available client display frequencies".
   vrlink reads them from `Prop_DisplayAvailableFrameRates_Float_Array` (2080), which driver_quest1 now sets (the panels' rate).
4. Still open: controller input. vrlink forwards buttons only from controllers whose type it reads as `frame_controller` ("Unsupported
   Prop_ControllerType_String"), through the Frame's input paths (`/user/hand/right/input/thumbstick/x`, ...).
5. With the frequencies the PC started SteamVR as a Quest 1 (its controllers as Quest 1 Touch) and sent HEVC: an atlas of four
   1024x1024 views, 1024x4096 at 72 Hz. The headset stayed on its loading screen: vrlink sets OUTPUT up (saying 1536x6144, a bound),
   streams it and queues packets, and only sets CAPTURE up at the first SOURCE_CHANGE, which it waits for with epoll; the driver starts
   the firmware session only once both queues stream, so nothing came and vrlink reset the decoder every 800 ms. Found with strace on
   vrlink and the driver's debugfs (CAPTURE count 0, no packet sent to the firmware). The decoder layer (`src/v4l2shim/quest1_vdec.c`)
   now sets CAPTURE up itself when packets have waited 50 ms with none (buffers of the OUTPUT size: smaller ones make the firmware call
   the stream unsupported and die), takes the firmware's size at its first "insufficient", gives those buffers back and raises the
   client's SOURCE_CHANGE through an eventfd it adds to the client's epoll set. A size beyond 4096 on a side goes to the driver as
   1920x1088 (a 1536x6144 session fails at the STREAMON after the reconfiguration), packet buffers are 4 MB at most (vrlink asks for 16
   of 7 MB), and vrlink's DISPLAY_DELAY 0 becomes the firmware's decode order with realtime priority and a 90 fps operating rate: venus
   decodes 1024x4096 HEVC at 51 fps flat out without them, 94 with them, 9.5 ms from packet to frame when paced at 72 (`vdectest`).
   `src/v4l2shim/test_vdec_epoll.c` plays vrlink's part on an NVENC stream of that size: SOURCE_CHANGE after 163 ms, every frame decoded.
6. vrlink's frame buffers: 16 NV12 images with the QCOM_COMPRESSED modifier (UBWC, what the Frame's decoder gives; read in its code),
   dedicated memory exported as dma-bufs (`vkGetMemoryFdKHR`), queued on the decoder (V4L2 DMABUF) and handed to vrcompositor. Our
   decoder writes linear venus NV12, so in the layer both NV12 modifiers are that: the exported memory is a gralloc NV12 buffer of ours
   (ION, venus's layout, which the decoder takes), imported again on the external format in vrcompositor, which now gets the two
   extensions with modifiers for NV12 only (its clients see no other dma-buf format and keep sharing their textures as before:
   vrclient logs "Supports dmabuf formats + modifiers? - No!"). `test_modifier` covers it: exported, written as venus writes, imported
   again with that modifier and sampled, exact at all four sizes.
7. First real session with items 5-6 (2026-11-11): SOURCE_CHANGE came, vrlink set CAPTURE up and vrcompositor imported its
   frames, but 20 ms later vrlink stopped: "Could not find a free OUTPUT buffer", 12 of its 16 packet buffers queued, then
   "ExperiencedAnUnrecoverableError" (the PC side: "Connection inactive" 5 s after it started sending, 211 of 229 frames
   "NotReceived"). vrlink takes all its packet buffers being held by the decoder at once as fatal, and venus holds every packet
   until its session runs and from the first sequence header until CAPTURE is set up again. vrlink had reset its decoder at the
   first packet (its "no forward progress for 800 ms" counts from its start), then sent the P-frames that came before the
   I-frame it asked for, while the 50 ms wait and our CAPTURE set-up ran. `test_vdec_epoll` (new third argument: the first
   access unit, to start inside a GOP) showed it: 11 to 16 packets held, sometimes none decoded. Now the decoder layer sets its
   CAPTURE up as soon as an epoll client waits (before any packet), and gives such a client the packets before the stream's
   start (parameter sets / IDR) back at once, through its eventfd (EPOLLOUT); the driver's EPOLLIN for our own CAPTURE is
   hidden from the client. Result: at most 2-3 packets held, SOURCE_CHANGE 17 ms and the first frame 50 ms after the first
   packet, 11 ms from packet to frame, about two epoll wake-ups per frame. FFmpeg's H.264/HEVC decode stays bit-exact.
8. **The picture shows** (2026-11-11 afternoon: the PC's dashboard and its Quest Touch models in both eyes, vrcompositor at 71 fps).
   What it took, in the order found:
   - vrlink's `SVLCodecV4L2::CheckStuck` (read in its code): it takes the older of two times, the last packet buffer it got back
     and the last frame (also set when its CaptureSetup ends). Past 330 ms it asks the PC for an I-frame (every 100 ms), past
     800 ms it closes the decoder and opens a new one. Its first decoder is always reset this way. So a new decoder has 800 ms to
     give a frame, I-frame wait included. The decoder layer now makes that: the stream's size of the previous decoder in the
     process is kept; the next one gets it at once (OUTPUT set up at that size instead of 1920x1088 when vrlink gives its
     1536x6144 bound, CAPTURE's format and the client's SOURCE_CHANGE before any packet), its close runs in the background
     (it took 240 ms), and its packet buffers are kept for the next one (140 ms of ION allocation).
   - With OUTPUT at the stream's size and `CONTINUE_DATA_TRANSFER` set, the firmware finds its buffers sufficient at the first
     sequence and the driver sends it its "continue" itself. Before, its "insufficient" wanted CAPTURE set up again, after which
     it often held every packet and every frame buffer (14 of 15 resets in a row gave no frame).
   - The layer forgot NV12 images when they were destroyed only at device end: after a few resets vrcompositor's import table
     was full ("Unable to create VkImage") and vrlink waited for it forever. Images are now forgotten in `vkDestroyImage`.
     vrcompositor imports each frame buffer and then exports it again for itself as OPAQUE_FD: the layer hands back the
     dma-buf it imported.
   - **Packet loss was the headset's own**: `net.core.rmem_max` was the kernel's 208 KiB, vrlink could not get the socket buffer
     it asks for ("could not set socket buffer size") and ~500 datagrams/s were dropped (`/proc/net/snmp` RcvbufErrors; vrlink
     logs them as FEC "substantive retry" and resets its decoder at "too many packets were missing"). `device/holo-perf.sh`
     sets 16 MiB: none dropped since.
   - Starting a session from the headset without the user (for tests): Steam's `SharedJSContext` (CEF remote debugging on
     127.0.0.1:8080) runs `SteamClient.Apps.StreamGame(250820, "<PC client id>", -1)`, the dashboard button's call (the id is in
     `logs/remote_connections.txt`). The panels must be on. `SteamClient.RemotePlay.StopStreamingSession` now wants an
     argument and, given the PC's id, does nothing (2026-11-11): vrlink has to be killed (by PID; its main thread is
     `vrlinkrunthread`), and Steam then starts no new session until it is restarted with SteamVR.
   Still open: vrlink hangs on its way out (a thread waits in vrclient for a reply of vrcompositor, the main thread joins it),
   and while it lives Steam starts no new session: it has to be killed.
9. **The stream in the headset** (2026-11-11 evening). That "picture" was the headset's own dashboard; the stream itself
   needed:
   - vrcompositor draws vrlink's frames in its distortion pass only, through a plain view of the NV12 image and a separate
     sampler; the blob samples NV12 only through a YCbCr conversion with an immutable sampler, so nothing came out. The
     layer (`quest1_modifier.c`, shaders `nv12_copy.vert/.frag`, `build_nv12_shaders.sh`) gives each NV12 image an RGBA8
     copy, filled with BT.709 RGB right after vrcompositor's barrier on the NV12 image, and vrcompositor's views are of the
     copy (raw YCbCr shows magenta: vrcompositor shows what it samples as it is).
   - Direct scanout's scene mode drops the distortion pass, so the panels showed only the eye passes (the headset's
     dashboard over black). While vrlink frames come, the eyes render into vrcompositor's own images and its distortion
     pass (an identity here) runs into the eye's buffer, stretched from the panel's size (`quest1_scanout.c`).
   - Identity: the model starts with "Deckard" ("Deckard (Oculus Quest)", `quest1_vrlink.c`). The PC's driver_vrlink
     takes a model starting with "steam frame" or "deckard" for a Steam Frame (headset type 5), and only then is the PC's
     SteamVR in the Frame's dashboard mode (`SteamVRClientUnified`: no dashboard of its own, its panels go to the headset's
     dashboard) and uses the Frame's stream profile. The PC's overlays show in the headset's dashboard only when the PC's
     SteamVR started after vrlink: restarting vrlink alone leaves them out.
   - Realtime threads: vrlink asks SCHED_FIFO 80 for its network and decoding threads; Steam may only go to 40
     (`LimitRTPRIO=40` in steam.service, `device/holo-steam-session.sh`), so they stayed normal threads, ran late with the
     headset busy, and the PC read that as a bad link ("Bad link event", holdoff) and held its encoder back. The shim lowers
     the requests to 40, and makes vrlink's main thread (it submits each frame to vrcompositor), `EventsThread` and its
     PipeWire `data-loop.0` realtime as they come up (they waited 169, 274 and 173 ms a second for a CPU).
   - The decoder runs at the core's top clock (`VIDC_CID(26)` = 2, turbo).
   - **10 bits**: for a Frame the PC encodes HEVC Main10 by default ("Using 10bit mode: 1" in driver_vrlink.txt). The
     driver's first "insufficient" says so (bit depth 10, and colour space); the decoder layer then puts the session in
     split mode at that reconfiguration (CAPTURE without buffers): `STREAM_OUTPUT_MODE` secondary and `DPB_COLOR_FORMAT`
     TP10 UBWC, set together with S_EXT_CTRLS, as Qualcomm's own decoder does for a client that reads frames. Venus keeps
     its references in TP10 UBWC (buffers the driver makes, 9.3 MB each at 1024x4096, as many as CAPTURE had frame
     buffers) and writes each frame as 8-bit NV12 into CAPTURE: the client sees NV12 as for any stream. An NVENC Main10
     1024x4096 clip through FFmpeg: 144 of 144 frames, PSNR 55 dB (Y) and 54 dB (U) against FFmpeg's own 10-to-8-bit
     conversion of its software decode, one rounding step at most. In 8 bits split mode is slower (12.7 ms against 11.0 ms
     from packet to frame at ~140 KB a frame), so it is used for 10-bit streams only. The PC's `"10bit": false` is not
     needed any more.
   - That reconfiguration made vrlink stop decoding for good: the driver says EPOLLIN for frame buffers the layer takes
     back itself (the flush) and for empty ones, vrlink woke, its DQBUF found nothing (EAGAIN), its `MediaThread` ended
     ("Failed to dequeue capture buffer ... Resource temporarily unavailable" in vrlink.txt) and it reset the decoder every
     800 ms while the firmware held 9 decoded frames. For an epoll client EPOLLIN now passes only when a frame is there:
     the layer takes it from the driver first (`frame_ready`), so the client's DQBUF always finds it. This was also the
     8-bit stream's "insufficient that wanted CAPTURE set up again" above.
   - Measured worn, 8 bits (2026-11-11): venus takes 8 ms from packet to frame for small frames and 11 ms at ~140 KB;
     vrlink's epoll wake to its DQBUF ~2 ms; then ~22 ms until vrlink tells the PC it queried the frame and ~31 ms more
     until it is shown: ~80 ms from the PC's encode to the panel, and the PC marks ~90 % of the frames "Mispredicted"
     (predicted and actual display a few ms apart) and ~80 % "DecodeSlow". With the headset not worn vrcompositor runs at
     9 fps, the PC reads that as a bad link every ~110 ms and sends ~1 Mbit/s: measure worn, or with the panels forced on.
   - The decode is exact: 400 packets of a real session (captured at the decoder's input) decode with no error in FFmpeg,
     and venus's frame of packet 300 is bit-exact against FFmpeg's (luma MSE 0). The compression blocks the user sees
     are in what the PC encodes: ~75 Mbit/s of video for four 1024x1024 views at 72 fps (P-frames of 120-130 KB when
     the head moves, 4-10 KB still; the I-frame 73 KB), the rest of the ~108 Mbit/s on the wire is vrlink's FEC.
10. **A new picture each frame** (2026-11-12; user: "clearly smoother"). The headset showed only vrlink's buffer 0,
    as the decoder wrote over it every 9 frames: a few pictures a second, each late. vrlink itself was fine: it picks a new
    decoded frame every 1/72 s (read in its memory without stopping it: its last shown frame stays ~2 behind the newest).
    It submits each frame as a texture set (`IVRClientInternal_XXX` slot 40): the 16 frame buffers it shared, and the index
    of the one to show. vrclient gives vrcompositor the set's first texture as the eye texture, with the index and the 16
    handles (`SubmitTextureSetInfo`, frame state offset 0xcf88); vrcompositor's `UpdateCachedTextures` then copies
    texture[index] into texture[0] through a graphics device method that does nothing in its Vulkan device (vrcompositor
    0x7c9e0 calls vtable+384, a bare `ret`), and draws texture[0]. The shim (`quest1_vrlink.c`) wraps that interface in
    vrlink, as it does IVRSystem: the frame to show goes first in the set, submitted with index 0 (the same 16 buffers,
    two of them swapped). vrcompositor then reads 9 buffers in turn, a new picture 329 times out of 330, at 71 fps.
    - The "1.65 s" lead of the night before was gdb's doing: its breakpoints stop vrlink, frames pile up, and only then do
      they all fall more than 1 s in the past. Frame IDs, the PC clock offset and the timing are fine without it.
    - Each NV12 import does show its own buffer (`test_modifier` checks a second frame next to the first now).
11. **The sharp centre** (2026-11-12; user: "it's sharp"). The centre broke up into blocks as soon as a controller moved
    ("abstract art"), the edges much less. The decode was not it: 40 frames that venus decoded while the user moved a
    controller (1440 frames of a session, recorded at the decoder's input) match FFmpeg's decode of the same packets
    (PSNR ~53.5 dB in every view, the 10-to-8-bit rounding), no packet missing. vrcompositor showed only the wide views
    (a snapshot of its eye buffers), whose centre the PC codes with few bits, as the inset is to cover it.
    - vrlink submits each eye's wide view (the bounds) and, after each eye's VRTextureWithPoseAndDepth_t (offset 152), the
      inset's place over it in the atlas (left (0.4, 0.329, 0.780, 0.415), right (0.220, 0.829, 0.600, 0.915): measured
      in the frames too) and a view count of 2 (offset 168); the inset is the view just above its wide view. The Frame's
      shader for that, `distort_ps_achromatic_se2`, blends the inset into the wide view in the distortion pass; here
      vrcompositor draws with `distort_ps_layered` (its fragment shader bound twice a frame, read in the layer), which
      knows no inset, although `enableAchromaticDistortion` is true.
    - So the layer does it: the shim publishes the places as vrlink submits (`/dev/shm/quest1-vrlink-views`,
      `quest1_views.h`), and vrcompositor's copy of each frame goes on to an output 1.5 times as large (its eye buffers
      are 1500x1664 for a 1024x1024 wide view) where each wide view gets its inset blended in, read at the level of
      the copy that fits its ~1.8 times reduction, with the edge of Valve's shader (`nv12_inset.frag`). It costs ~2.5 ms
      of GPU a frame: 58-60 fps against ~69 without it, 71 without any copy (dashboard open, panels forced on).
      Timed alone (`src/vklayer-steamvr/streambench.c`, 2026-11-16, median of 200 with SteamVR running): the copy
      1.63 ms, its smaller levels 1.47, the output 2.97. The smaller levels are read for the insets only, and only
      the two the inset's level falls between (~0.96 here: levels 0 and 1): made over the insets' rows only and
      without the unread level, 0.57 ms, so ~0.9 ms of GPU less a stream frame, the same picture. A fragment pass
      per level instead of the blits, or the output as one pass per eye band: no gain.
    - The copies (and outputs) of one size share one memory: each frame copies only the frame vrcompositor then reads.
      vrlink shares 16 frame buffers, and 16 copies of 16 MB each were made as the session started: the headset swapped
      ~700 MB within seconds and stalled (Monado's re-projection too). ~150-250 MB now; vrcompositor's GPU memory went
      from 666 to 397 MB.
12. **The head's pose for the PC** (2026-11-12; user: "it trembles A LOT less", the PC's VR view too). The PC's
    own VR view shook far more than the head. driver_quest1 changes the HMD pose only when vrcompositor presents, with
    no velocity (so that vrcompositor renders with exactly the pose the layer gives Monado, docs/steamvr-port.md): the
    PC saw a pose that stood still, then jumped, at uneven times (10-27 ms apart during a head turn, logged on the PC
    with `GetDeviceToAbsoluteTrackingPose`), and could not predict it. Replayed on a recorded head turn, what the PC
    rendered at 72 Hz was ~6 times rougher than the head.
    - A velocity in the HMD pose does not do: vrcompositor renders with it for a time of its own, ~39 ms after the
      pose came, but ~7 ms later for seconds at a time (measured with a fake velocity, from the shift of the picture in
      snapshots), so its frames would be off by velocity x 7 ms.
    - vrlink reads the pose elsewhere: not through `WaitGetPoses` (it passes no arrays, for the timing only) nor
      `IVRSystem`, but SteamVR's raw driver pose, `IVRClientInternal_XXX` slot 12 (`vrlink.bin` 0x56044, from its
      EventsThread, ~200 times a second; found with a hardware read watchpoint on vrserver's shared memory). vrclient
      copies a CLOCK_MONOTONIC time then a `DriverPose_t`; vrlink predicts it with its velocity and sends it.
    - So the driver also writes the head's current pose with its velocities at 250 Hz (`/dev/shm/quest1-hmd-pose`,
      `quest1_hmd_pose.h`, while vrlink reads it), and the shim gives vrlink that one, with vrserver's time convention
      (the pose's time plus a lead vrserver adds, ~39 ms from the driver's call, measured as it runs). The PC's pose now
      moves at every query (~650 changes a second) instead of in steps; the headset's own frames are unchanged.
    - vrlink turns `vecAngularVelocity` by the head's orientation: it takes it in the head's frame (fitted on the PC's
      poses: cosine 0.99 that way, ~0 given in the driver's space, which made the PC predict a nod as a roll and lean
      the view as the headset was lifted). The linear velocity is in the driver's space.
    - Left (docs/todo.md): the stream still trembles, less. The frames' poses match the head ~20 ms after vrlink
      submits them, and vrcompositor shows them ~60 ms after; 40 ms more of prediction (closer, by the logs) changed
      nothing the user could see.
13. **Measuring where the stream's frames land** (2026-11-12/13; open, docs/todo.md). The user: the stream jumps
    ("jerks") at every head movement, even a tiny one, never while still; the whole picture, near or far; depth
    feels wrong while turning ("it almost feels like I'm going cross-eyed"); SteamVR's HUD overlays (IPD, volume) jump in
    exactly the same way, while the dashboard and the chaperone are steady. A Quest 3 on the same PC (same overlays)
    has none of it: the cause is on this headset.
    - Method (tools/headset-dev/README.md): the headset lies still with its panels on (`QUEST1_IGNORE_PROX=1`),
      `fake-motion.patch` gives it a known smooth head motion (4° at 2 Hz, 50°/s at most), and the layer's snapshots
      (vrcompositor's output, before Monado) are taken in a series. A world-locked overlay (`q1marker`) is measured in
      the same snapshot as a stream region: the error of when a snapshot was taken cancels. Without the marker, that
      error (one or two of vrcompositor's frames) hid everything at 72 fps, and at ~52 fps a series showed the stream
      smooth because the region was partly the dashboard: check each region's contrast, and that it stays put without
      motion (Aurora's sky moves).
    - vrcompositor's re-projection is exact: a stream frame left on screen (vrlink stopped, the last frame shown
      again) follows the marker within 0.8 px.
    - Live frames: ~75 % land within 4 ms of motion of where their pose says; ~20 % are 30-100 ms off, most ~65 ms
      (the picture newer than its pose), 1-5° at 50°/s: the jumps.
    - vrcompositor re-projects with the pose vrlink submits with each frame (`VRTextureWithPose_t` at byte 16 of each
      eye's 176-byte struct, flags 0x2018018): turning that pose turns the picture. The submit's time (`seconds`) does
      nothing.
    - Not the cause: the velocities given to vrlink (none at all: the same), the time given with the pose (±20 ms:
      the same), the decoder (each frame back in order with its own timestamp, 1:1, ~14 ms to decode), the PC (its app
      frames repeat only during rare ~1 s stalls in `vrlink_aftm_*.csv`, whose "remote" times are the headset's
      CLOCK_MONOTONIC in µs, so both sides can be lined up).
    - One cause found, in the layer: it copied vrlink's NV12 frame into its RGBA output at each of vrcompositor's frames,
      also when vrcompositor showed the same frame again, but by then vrlink may have given that buffer back to the
      decoder (9 buffers), which wrote a newer picture into it: that newer picture went out under the old pose.
      Copying each frame once (`qcp_record`): the 52-76 ms cluster is gone, but with vrcompositor at ~57 fps (not
      72) only ~31 % of frames landed right, the others spread over ±40 ms; the user still saw the stream tremble.
    - The HUD overlays' trembling was not the stream's: Monado's warp moved them with each frame's prediction error,
      stream or not. They are drawn apart now, stream or not (docs/direct-scanout.md, 2026-11-13).
    - 2026-11-13, one test for every case: the fake yaw moved into Monado's head pose, with its angular velocity
      (`fake-motion-monado.patch`), so that vrcompositor, the warp, vrlink and the PC all see it; a world-locked and a
      head-locked `q1marker` and a stream region measured in the same snapshots of Monado's own output (`mjit.py`).
      On a system that had run for hours with many sessions: the stream 16.6 px rms off the world (up to 50). After a
      reboot: 2.5-3 px (max 4-5), the head marker 0 px, in every series since.
    - Where frame and pose come apart was checked at each step, and it is nowhere on a fresh system:
      - vrlink's submits: the shim read the luma of each submitted frame (the decoder's buffer of that index, mapped
        in vrlink) and its shift against a still reference, next to the yaw of the pose submitted with it. In the
        sharp inset the picture follows its own pose within 0.56° rms (0.3° of measurement noise at rest): vrlink and
        the PC pair them right. (Measured in the wide view's centre, which the PC codes with few bits under the
        inset, it looked 2° off: the blocks, not the pose.) The pose vrlink gives the PC is clean too: the fake sine
        within 3.4 %, its angular velocity within 3.7 %.
      - vrcompositor's frames: it settles on vrlink's frame 4-9 ms before it reads it, and ~25 % of the frames it
        takes are older than vrlink's last submit. Drawing vrlink's last frame instead (each frame buffer tagged by the
        shim, read by the layer) changed nothing: 2.8 px against 2.7 px, A/B on the same run. So those frames go out
        with their own pose; not kept.
      - The shim's texture handles are `fd << 32 | vrcompositor's pid`, the fd vrcompositor got for each of vrlink's
        buffers; it duplicates and closes it, so that number names nothing later.
    - Left (docs/todo.md): what makes a long-running system bad. vrcompositor leaked a `sync_fence` fd every frame and
      slows down (64-71 -> 55 fps in 90 minutes, swap in use). The leak is fixed (PR #4, merged 2026-11-15):
      vrcompositor's vblank thread registers display events with fences it waits on at once, gets a timeout as the
      layer has not signalled them yet, and never destroys them, while each fence the layer submits holds a
      descriptor; such fences now come from a pool of 16 the layer reuses (quest1_display.c, `evnopool` in
      /tmp/quest1-layer turns it off). Tested on main: 13-16 `sync_fence` descriptors for 7 minutes with the pool,
      ~58 more a second without, the same frame rate.
    - 2026-11-13 evening (user, on a fresh system: "it still trembles"). The bench widened, all clean: both eyes
      (the same error, no disparity between them); a 4 cm sideways sine (vrcompositor re-projects the stream with
      vrlink's depth, in step with the world marker, < 0.5 px off); 190°/s (a smooth 3.4 ms lag in the wide views, the
      same with vrlink stopped: vrcompositor's own re-projection, not the stream); and the user's recorded head
      (`replay-head-monado.patch`: 85 s of Meta's poses fed to Monado's input, prediction included), stream next to
      the world marker within 5 px. Every picture was in the right place: the jumps were in when they were shown.
      Monado's log of the user's test: 53-65 frames a second on the panels, ~17 commits a second after their vsync,
      each a frame shown a period late. The warp's CPU time went to reading the HUD's meshes from write-combined
      memory whenever a head-locked overlay showed, as during every stream (docs/direct-scanout.md, 2026-11-13
      evening). Fixed: no late commit with a stream and a HUD.
    - 2026-11-13 night (user: still trembles; a fast vibration as the head turns, and at each heartbeat while still;
      more when leaning; the dashboard stays put, the PC's own VR view trembles a lot). A second marker drawn by the PC
      (`tools/headset-dev/pcmarker.py`, a world-locked overlay of the PC's SteamVR, in the stream) next to `q1marker`,
      the headset still on the table, and the pose vrlink gives the PC changed by the shim
      (`pose-knobs-vrlink.patch`):
      - A fixed turn (3°, 10°) of the pose the PC renders with: B stays at 0 px. A turn that changes, a yaw sine of 1°:
        0.5 px at 0.25 Hz, 1-1.6 px at 1 Hz, 2-5 px at 3 Hz, over 25 px at 8 Hz; random each read (±1°): 9-12 px rms.
        vrcompositor corrects the picture for its pose as far as that pose changes slowly; a change from one frame to
        the next shows as it is (about the pose's change over ~25 ms).
      - It is not the pairing: each picture follows the pose vrlink submits with it within 0.007-0.01°, in the inset
        and in the wide view (the picture's shift in the decoded frame against the submitted pose's yaw, one frame off
        gives 7 px). Nor the layer's choice of frame: drawing vrlink's last frame instead of the one vrcompositor took
        (it took an older one 20-42 % of the time): 9.8 / 9.9 / 8.5 px rms, 100 snapshots each, interleaved. Nor push
        constants or `vkCmdUpdateBuffer` (the stream's pose is in neither: mapped memory).
      - On a smooth motion the PC's prediction is right and nothing shows (the earlier benches). Worn, the PC predicts
        ~90 ms ahead (5.9° of picture for a 60°/s rate) from the angular velocity: its noise and the heartbeat's jolts
        make the pose rendered with change from frame to frame, and that shows. So the shim low-passed the angular
        velocity it gives vrlink (40 ms; user, blind A/B worn: less than none, 20 or 100 ms; removed 2026-11-17, item 15). Offline on the user's
        recorded head (Meta's poses, the PC's prediction simulated): the shake above ~10 Hz falls as the filter gets
        slower (0.34° rms raw, 0.30° at 40 ms, 0.28° at 100 ms) while the error that follows the head's turns grows
        (the stream swims): a trade-off only, an adaptive (one-euro) filter barely better.
      - Position: vrlink submits no depth (the depth flag on, the depth texture null), so vrcompositor re-projects by
        rotation only. A position 5 cm off moves B 21 px (the full parallax at ~1.8 m); the submitted pose carries
        it. Worn, the PC's 80 ms position prediction is off by 1.9 mm median (p99 7 mm, ~1-3 px at 1.8 m).
      - Where vrcompositor keeps the stream frame's pose (2026-11-14, `tools/headset-dev/vrc-trace-layer.patch`, the
        pose sent to the PC turned by a 2° 2.3 Hz sine, the submitted poses following it at 0.99 of a pure sine): in
        none of its `vkCmdUpdateBuffer` data (the 624-byte overlay uniforms, the 800/120/272/1440/9600-byte ones: no
        component at 2.3 Hz), no push constants, no descriptor writes while it runs (its sets are written once, no
        dynamic offsets). Its only `vkMapMemory` mapping is the 256 MB ring. Its uniform buffers (each with a small
        memory of its own, 192-2816 bytes) are filled by `vkCmdCopyBuffer` from host-visible staging buffers, 2800
        bytes into the 2816-byte one and 400 bytes into others at every frame; that staging is not mapped through
        `vkMapMemory` (`vkMapMemory2` presumably, hooked by the patch but not yet run). Next: read those copies'
        data under the sine; the field that follows it, and which submit's pose it holds at each frame, tells what
        vrcompositor pairs with which picture.
      - 2026-11-15, the test that shows the trembling (`tools/headset-dev/stepsnap.py`): the headset still, the pose
        sent to the PC stepped by 1.5° of pitch back and forth, each step waited for in vrlink's submits, then a
        snapshot 0-40 ms later; the horizon of the PC's scene measured. 12-22 ms after a step, 37 of 39 snapshots were
        18 px off, the whole step; none after 25 ms. So the frame shown after each change of the pose goes out with
        the picture of one submit and the pose of another (cause and fix below). With a pose that changes at every
        frame, as on a head, that is every frame: the vibration. ±1° of random
        yaw shook the scene's floor by 7.7-9.6 px rms the same way. Pitfalls met: regions on an overlay measure
        nothing (the headset's dashboard, which `quest1-frame restart` opens, and the PC's overlays stay at 0 px); a
        frame vrcompositor shows again when vrlink stops looks like a live one; vrlink's process is `vrlinkrunthread`
        in `ps` once it runs.
      - The 2800-byte uniforms (their host memory mapped by the layer itself: vrcompositor maps it in no way the layer
        sees): per eye, at floats 44 and 156, the eye's pose (vrlink's submitted head pose moved by ±IPD/2,
        column-major), the same again, then its inverse twice; the 400-byte ones hold the headset's eye poses. Their
        copy into a device buffer, at the end of each frame's commands, is not what the distortion reads: a pose
        turned by 3° written there moved nothing, while the same written into the host memory moved the stream by
        3° (36 px), even with that copy replaced.
      - The cause (2026-11-16): vrcompositor writes the stream's pose into that host memory at the start of each
        frame, before it takes vrlink's frame (1409 of 1411 frames: the same bytes there and at the end of the
        recording), and its distortion reads it there, through a uniform buffer in that memory, when the GPU runs
        the frame: one buffer for every frame, no double buffering. The GPU runs a frame well after vrcompositor
        recorded it (~12 of 14 ms busy) and vrcompositor has often written the next frame's pose by then, so the
        frame after each change of the pose went out with the next pose. vrlink's frames are right: in vrlink each
        decoded picture's horizon moves at the same submit as its pose, at every step (a test probe in the shim).
      - The fix (quest1_modifier.c "The stream frame's pose", quest1_rpmerge.c `qsp_bind`): when vrcompositor binds a
        descriptor set that reads a uniform buffer in that memory (2 a frame, one an eye), the layer binds a copy of
        the set whose descriptor reads a snapshot of it taken there, in the command buffer's own staging (the
        `ub_*` arenas, kept until the command buffer is begun again). The memory is found as the source of
        vrcompositor's 2800-byte copy. `stepsnap.py`, pitch steps of 1.5°: the frames shown 0-25 ms after a step,
        0 of 19 and 0 of 14 off with it, 16 of 17 off by the whole step without it (`nostreampose` in
        /tmp/quest1-layer); later ones 0 of 41 and 1 of 46 (a 60 px miss, the search's edge). The frame rate is the
        same either way (55-57 fps with the stream). It needs the render pass merge (quest1_rpmerge.c, on unless
        `nomerge`) for its descriptor pool. The user, blind A/B worn the same night: without it "it still
        trembles", with it "it's a thousand times better, it doesn't tremble at all any more". Things tried first that did not do it, kept in case: giving the device
        copy the picture's pose; drawing the picture whose pose the host memory held when the copy was recorded;
        recording that copy as a `vkCmdUpdateBuffer` of what the memory held then (all no change, since the device
        copy is not read). Drawing vrlink's last frame instead of the one vrcompositor took makes it worse (the
        layer's choice comes before vrcompositor's pose).
      - SteamVR 2.19.3 (2026-11-17): the trembling came back (user, worn). 2.19's vrcompositor writes that memory
        later, after it bound the sets and just before it submits the frame, with the pose of vrlink's last submit
        at that moment, while the picture is vrlink's last submit before the layer's copy, earlier in the frame. When
        vrlink submitted in between, the bind's snapshot holds the pose of the frame before: at 8 steps of the pose,
        the picture (its horizon read at the copy) went with the bind's pose 4 times and with the submit's 4 times.
        The fix: the shim publishes each submit's head pose, time and frame buffer index (`/dev/shm/quest1-vrlink-poses`,
        quest1_views.h `q1_poses`); the layer finds the picture's submit at the copy (`qsp_picture`: by the frame
        buffer, since "the last submit before the copy" was wrong ~10 % of the time) and at
        vrcompositor's submit keeps whichever snapshot has that pose (bind or submit), or, when neither has it, writes
        the eye poses made from it into the snapshot (`qsp_repose`). The layer's log gives the counts once a minute
        (`stream: ... the picture's pose was the bind's N times, the submit's N, made from them N ...; pictures found
        by their frame buffer N, as the last submit N`). `stepsnap.py`, pitch steps of 1.5°: 0 of 52 and 0 of 60
        pictures off, 4 of 40 without it; ±1° of random pitch at every pose read (`/tmp/q1-posenoise` of
        pose-knobs-vrlink.patch): 2.5 px rms instead of 7.2. User, worn: "it doesn't tremble any more, almost";
        still under load, which was the decoder's clock (next item).
      - Which frame buffer a picture is (2026-11-17, later): first learnt per NV12 image from the copies (votes for
        vrlink's last submit before each copy). Worn, half the pictures stayed unknown, more with the dashboard open
        (vrcompositor busier: the copy further from the submit), and a stream restart brought new buffers at the old
        images' handles (votes now fade). Exact instead: the shim publishes vrlink's pid and its own fd of each frame
        buffer (the dma-buf it queues to the decoder, `q1dec_client_fd`), and at each NV12 import the layer compares
        that dma-buf with them across the two processes (`kcmp(KCMP_FILE)`; same user, no pid namespace between
        them), again once a second until vrlink has queued it. The handles vrlink submits are no help: their fds
        are vrcompositor's shared memory files, not the buffers. A minute with the stream: 485 of 486 pictures found
        by their buffer. User, worn, dashboard open or not: "it nearly doesn't tremble any more". Left: a strong
        tremble for a moment when the headset is put on (9 to 72 fps), docs/todo.md.
      - The decoder's clock (2026-11-17): as the 2.19 streams started, the kernel logged "Failed to set clock rate:
        -19" once and venus's core stayed at 200 MHz of 533 for the whole session (`/sys/kernel/debug/clk/
        video_core_clk_src/rate`), turbo asked for or not: decoding took 76 ms a frame (p50, the PC's
        vrlink_aftm CSV) instead of 12-17, every frame late. The driver sets the clock again only when a control
        changes (V4L2's control framework skips a write of the same value), so the shim switches the perf level to
        nominal and back to turbo every 2 s while a low-latency client takes frames (`keep_clock` in
        quest1_vdec.c): 533 MHz again, decode p50 12.2 ms, p90 17.5.

14. **Controller buttons** (2026-11-16). vrlink forwards a controller's buttons and axes to the PC only if the
    controller's type reads `frame_controller` (vrlink.bin 0x563b0: each frame it reads Prop_ControllerType_String into
    a 1024-byte buffer, else "Unsupported Prop_ControllerType_String"). The identity it sends the PC reads the same
    property into 128 bytes. So the shim answers `frame_controller` to the 1024-byte read only. vrlink then needs the
    hand's legacy controller state (GetControllerState must succeed, nothing is sent otherwise): the driver selects a
    `vrlink.client` legacy binding of ours (`quest1_touch_vrlink_bindings.json`, the Frame's legacy layout). SteamVR
    ignores a driver's per-app default binding when the app's manifest has bindings for other controllers (vrlink's
    Knuckles one was converted instead): the driver saves the choice as the bindings UI does, in steamvr.vrsettings
    `vrlink.client/oculus_touch_250820_CurrentURL_legacy` (the key ends with the binding's category, `_legacy` for an
    app without actions; vrserver builds it at 0x230600). vrlink finds the controllers through
    `GetTrackedDeviceIndexForControllerRole`: asleep controllers give -1 and nothing is sent.

    The values themselves do not come from that state: vrlink reads each control by its Frame path through IVRPaths
    (StringToHandle once, then ReadPathBatch each frame; 0x54a18 buttons, 0x55840 axes): right a, b, x, y, menu; left
    dpad_down, dpad_right, dpad_left, dpad_up, view; both thumbstick, grip, bumper, trigger, system, thumbrest. It sends
    them in slots in that order, and the PC's driver_vrlink fills the controller games get from the same slots in the
    order of the identity's type (Windows driver_vrlink.dll: 0x180a16385 for `oculus_touch`, 0x180a16dcb for
    `frame_controller`). The two orders differ: the Frame's stick x, stick y and trigger slots are the Touch's trigger,
    stick x and stick y; its y and dpad_up slots the Touch's stick click; its x and dpad_left touch slots the Touch's
    trigger touch. Seen by the user (2026-11-16, worn): the sticks did nothing (our controllers have no `thumbstick`
    path), X and Y did nothing (no `dpad_*`), and the trigger moved the stick up on the PC's controller model (the
    Frame's trigger slot). Fixed in the shim: vrlink's `IVRPaths_002` gets a StringToHandle that answers each Frame
    path with our Touch control the PC reads from that slot (`input_paths` in quest1_vrlink.c).

    Test (no one needs to wear the headset; the controllers can sleep): `tools/headset-dev/vrlink-buttons.py`, run on
    the PC during a session, presses each Touch control through driver_quest1's simulated press
    (docs/controllers-native.md) and checks the PC's render model of the controller (the component that moves or turns
    touched/pressed: what games get). 2026-11-16: all 24 as expected (A, B, X, Y, menu, triggers and grips with
    their touches, sticks on both axes, stick clicks and touches; the right Oculus button stays on the headset). The
    model draws no touch for A, B, X and Y, so those four touches are read from the slot layout only. Again 24/24 with
    Steam in Frame mode (device type 5001, docs/frame-mode.md). While SteamVR's dashboard is open on the headset it
    keeps the controllers: vrlink's GetControllerState fails and nothing reaches the PC (as on any SteamVR headset;
    closing it with the system button gives them back). vrlink tells the PC the dashboard's state
    (`VTE_DRIVER_SYSTEM_FLAGS` 1 1 1 open, 0 0 0 closed, in its log); the test reads it there, because a new OpenVR
    client (vrdash) connecting opened the dashboard again within a second (2026-11-16).
    Checked worn by the user (2026-11-18): the controllers' buttons reach the PC's games.

15. **The PC's prediction "bounced"** (user, 2026-11-17: on a sharp turn the black edges and the user's own avatar
    head in VRChat visibly overshoot and come back; the controllers a little too). The shim's 40 ms low-pass of the
    angular velocity (item 13, 2026-11-13) was the cause: the PC predicts ~90 ms ahead, and a delayed rate keeps the
    prediction going after the head stops (and short when it starts). It was there because vrcompositor did not
    correct fast changes of the frame's pose; since 1efbd78 it corrects them exactly, so the rate goes as measured.
    - Bench, no one wearing the headset (`tools/headset-dev/pred-log-vrlink.patch` in the shim,
      `replay-head-monado.patch` in Monado, the panels on): the user's recorded head (85 s) replayed at Monado's input
      during a stream, each submitted frame's pose against the head's true pose after the submit (`predfit.py`; a
      constant rotation between the two spaces fitted out). Same 84 s of the recording each (2026-11-17):

      | angular rate | frames match the head at | error at submit+30 ms (rms / moving / p99) |
      |---|---|---|
      | as measured | submit + 20 ms | 2.53 / 3.66 / 8.8 deg |
      | low-passed 40 ms | submit + 10 ms | 3.93 / 5.73 / 13.2 deg |

      A frame is shown ~25-30 ms after its submit (vrcompositor takes it at its next frame, ~7 ms on average, and
      predicts ~21 ms to the panels): the PC's prediction now lands within ~10 ms of that. Offline on the same
      recording, the frame-to-frame shake grows by ~10 % without the low-pass; the headset corrects it.
    - Left: the rest is the latency itself (the error grows ~0.6 deg rms per 10 ms of horizon): docs/todo.md.

16. **Black flashes: logged where they would come from** (2026-11-17). Not reproduced on the table under the user's
    load (desktop mode, 500 MB more taken, load 19): no black picture, no gap, no empty submit (a test probe on every
    submit). The shim now logs, with the time of day, in the journal (`quest1_v4l2: vrlink: HH:MM:SS.mmm ...`): a gap
    of more than 100 ms between vrlink's pictures (a freeze, re-projected), empty submits passed on (no picture for
    200 ms: no stream drawn), and black pictures (every sample of the decoded atlas at most 24; video black is 16).
    The picture's luma is read on a 512-sample grid by a `writev` of those bytes into a pipe, not through the
    pointer: the decoder's thread may unmap a buffer meanwhile, and the copy then fails instead of crashing vrlink
    (which takes Steam with it); `process_vm_readv` cannot read these mappings (EFAULT). Checked on a session: Aurora
    reads means of 29-53, the start's empty submits and slow pictures are logged as one line each.
    - The VRChat loading skybox (user, 2026-11-17, photo: the world's picture only inside a rounded rectangle, the
      shape of the sharp inset, the grid around it, black between): vrlink forwards no skybox to the headset (no such
      code in either side's binaries), so the PC's compositor drew it into the inset only. Not reproduced with the
      PC's default environment (both views drawn). Open (docs/todo.md).
    - Worn the same day (VRChat): runs of 19-69 black pictures as VRChat started on the PC (its own black screens),
      single ones later. The probe also logs pictures with no chroma (green); the green stream the user saw that day
      was vrcompositor's simplified shader instead (holo-frame-mode.sh, "The distortion shader" below).
    - SteamVR 2.19 (2026-11-17): the headset dropped the stream's datagrams (UDP `RcvbufErrors`, 18,644 in 35 min)
      and the PC logged "Bad link event"s and `ClientRxLoss`, then held its encoder back; when the dashboard opened,
      a second without a picture (a black flash). 2.19's vrlink receives in threads of its own (`RXT<address>`) that
      ask for nice -10 only (RealtimeKit cannot place them), and its sockets for 1 MB of buffer (~33 ms at
      160 Mbit/s). The shim makes those threads SCHED_FIFO 40 like vrlink's other network threads and raises the
      buffers to 8 MB (`setsockopt` of vrlink's). Next session, 5 minutes: its sockets dropped none
      (`/proc/net/udp`); the system's counter still grew by ~2,000 over the 30 min and five stream restarts around
      it, not attributed (other sockets, or the sessions' starts).
    - Multilink: with the headset on the PC's USB cable (usb0, the development link) vrlink receives the whole stream on
      both links, 141 Mbit/s over USB and 145 over Wi-Fi, ~0.5 core of receive threads and USB interrupt work. Kept:
      the user wants Valve's multilink as it is.

### Decoding: every frame "DecodeSlow" for the PC (2026-11-20)

The PC's `driver_vrlink.txt` put "DecodeSlow" on nearly every frame (1,060-1,075 of ~1,075 every 15 s), and its
`vrlink_aftm_*.csv` gave 19.3 ms from vrlink queuing a packet to taking the frame (p90 38, p99 119) for the 1024x4096
Main10 stream, against a frame every 13.9 ms. Venus was at its top clock (533 MHz, `video_core_clk_src`), its DDR
vote ~4.9 GB/s, the instance's operating rate 90 (the "fps: 15" of its debugfs `info` is not what the load uses).
ftrace (`msm_vidc` buffer events, `v4l2`/`vb2` dqbuf, default buffer size) split the 19 ms:
- the firmware, from the packet (ETB) to the frame (FBD): 13.2 ms (p90 13.8), little more for big frames (12.6 ms at
  68-179 kB, 13.7 at 222-274 kB);
- from the FBD to vrlink's DQBUF: 4.2 ms (p90 10.3, max 23.5). Two causes:
  - vrlink's decoder thread (`V4L2MediaThread`) is a normal thread in 2.19, on CPUs 4-7 at a load of ~16: it waited
    for a CPU. The shim makes it realtime (FIFO 39, `quest1_vrlink.c` rt_threads): 2.4 ms (p90 2.9, max 3.8).
  - the remaining ~2.1 ms were inside the DQBUF: venus's driver invalidates a cached ION frame from the CPU caches
    (`output_buffer_cache_invalidate`, 6 MB), and vrlink's frames were cached gralloc buffers (usage "CPU read
    often"). Gralloc's private uncached bit makes them uncached (`QNV_AHB_USAGE`, quest1_modifier_extfmt.c; "CPU read
    rarely" or no CPU usage still gave cached ones): 0.2 ms (p90 0.3, max 1.1).

Result, same PC, same stream, panels on with nobody wearing it: the PC's packet-to-frame 13.0 ms (p90 13.9, p99 17.7)
instead of 19.3 (38, 119); "DecodeSlow" on 48-192 frames every 15 s instead of all of them; the picture checked in a
cropped snapshot, no black or green picture in the shim's log. Left: the firmware's 13 ms itself (an 8-bit stream
skips the split mode's two outputs, ~1.5-2 ms in the 2026-11-11 measures, but it is the PC's `driver_vrlink` "10bit"
setting: the user's call), and the ~50 ms from the decoded frame to its display ("Submit Illumination").

The strong lags of 2026-11-17 (the video gone, the sound far behind, back, gone again): in that session's CSV the
network delay and vrlink's ping climbed to ~1,000 ms in three episodes of ~6 s in the first 25 s, with "ClientRxLoss",
"PingSpike", "StreamReset" and "AudioClientDrop" in the PC's log. A ceiling that flat is a queue, not the radio:
~1 s is what the 8 MB socket buffers (item 16) hold at that bitrate, so the receiving side fell that far behind,
with the decoder at the edge of its budget (DecodeSlow on every frame, the PC's frame rate rising and falling). Not
reproduced yet with the faster decoder.

### Picture quality (2026-11-16, read in driver_vrlink and the PC's settings)

The PC alone sizes the stream: an atlas of four square views (each eye's wide view and inset), `streamFormatWidth`
1024 because the PC's `driver_vrlink/automaticStreamFormatWidth` is false (automatic would give the Frame profile's
1280: beyond what venus decodes at 72 Hz, ~1088-1152). The headset's properties only set the game's render size. The
inset covers an inner field of 45-48° plus `steamvr/preferredFoveatedAngle` (default 0, 30-90): ~21 px/deg there,
~10 px/deg in the wide view (the panels: ~15). The bitrate is `driver_vrlink/targetBandwidth` 110 (automatic off;
Valve's default 200): ~75 Mbit/s of video, ~130 KB a frame, which the encoder fills as soon as the head moves (the
blocks). The PC draws its overlays and desktop windows into those same views, so outside the inset a desktop window
gets ~10 px/deg (text needs ~29). Levers, all PC settings: the bitrate (150-180, the link carries ~200 Mbit/s), a larger
inset (`preferredFoveatedAngle` +16..22: ~15 px/deg over 64-70°, no decode cost), the PC's sharpening
(`driver_vrlink/useSharpening`, CAS before encoding). The view size has a hard cap in driver_vrlink by the PC's NVIDIA
generation (read through NvAPI, 0x1809fad80): below Pascal 832, Pascal and Volta 1024, Turing 1280, Ampere and later
1344, applied after every setting (which the driver then rewrites with the capped value). On this PC (Pascal) a
non-foveated stream (`foveationMode` 1) is two views of 1024: fewer pixels than today's four. Tried 2026-11-16 at the
user's request: 1024 whatever the keys; the bitrate raised to 160 Mbit/s.

### The distortion shader (2026-11-16, read in vrcompositor)

vrcompositor picks its distortion shader per frame in `CGraphicsDevice::DrawScene` (0x7ab08): the achromatic ones
(`distort_ps_achromatic`, `_layered` with overlays in the pass, `_se2` that blends a stream's inset, "spatial encoding
2") only if `steamvr/enableSimplifiedShaders` is on (with `enableAchromaticDistortion` also on; both read once at
start, so a live change does nothing). `enableAchromaticDistortion` alone only changes how the distortion textures are
built. With the stream and the dashboard: ~11.6 ms of GPU a frame with the setting off, 9.3 ms on; but on, the stream
looks washed out: the simplified shaders take its texture as linear and encode it again. An sRGB view of the layer's
copy gives the right colours, but the copies made mutable for it cost ~9 ms (45 fps, compression lost?). `_se2` would
also need vrcompositor to see a spatial encoding of 2 for vrlink's submits (not found) and its own YUV input (R=Cr,
G=Y, B=Cb).

2026-11-17: on. The layer's output for vrcompositor (the wide views with their insets) is now an sRGB image of its own
when `enableSimplifiedShaders` is on (read from steamvr.vrsettings once, as vrcompositor does): `nv12_inset.frag`
writes linear light (`LINEAR_OUT`, a specialization constant), the sampler decodes it, the simplified shaders see what
they expect. Not mutable (no UBWC lost). `holo-frame-mode.sh` sets the setting with `enableAchromaticDistortion`
(Monado does the lens and its chromatic aberration; vrcompositor's pass is a copy). Measured with a stream and the
dashboard open, panels on: vrcompositor 11.8 -> 11.0 ms of GPU a frame (`vrgpu.sh`, three 2 s windows each), Monado's
warp 2.8 ms either way; colours of the stream and of the dashboard the same within 0.3/255 in vrcompositor's output
(snapshots, region means). The 2.2 ms of 2026-11-16 was not found again.

Off again the same day: worn, the whole stream turned green whenever the headset drew nothing over it (no chaperone,
dashboard closed; user). With no overlay in the pass vrcompositor draws the stream with its plain achromatic shader,
which the layer does not replace (it replaces `distort_ps_layered` only), and that shader read the frame wrongly. The
snapshots above were all taken with overlays in the pass. The layer's sRGB output stays, unused while the setting is
off; to try again, replace the plain achromatic shader too and test with every overlay hidden.

Plan:
0. ~~Capture vrlink's own output~~ done (above).
1. ~~`/dev/video-dec0` and a decode test~~ done (`vrdevice_path`, items 5 and 6).
2. ~~Vulkan dma-buf import shim~~ done (items 2 and 6).
3. ~~The picture in the headset~~ done (items 8-11), in 8 and 10 bits, a new picture each frame, a sharp centre. Next (docs/todo.md):
   the stream trembles, flashes black now and then and runs below 72 fps (item 11); vrlink's exit hang.
4. Alternative: ALVR or WiVRn (PC-side driver + headset client with V4L2 or software HEVC decode). 1-2 weeks, avoids Valve's stack but loses Steam integration.
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

## Notes moved from the to-do list (2026-11-12)

- Remote Play from the headset works well enough for what it is (user, 2026-11-12). Left as measured: the encode
  takes 13-17 ms a frame during a stream (venus at its top clock, 355 MHz) against 8-9 ms on a quiet headset; the DDR
  bandwidth vote (`venus_bus_ddr`) or a firmware low-latency mode could close the gap.
- Steam Link 2D from a PC works well (user, 2026-11-12); more performance is welcome (docs/todo.md). Points noted on
  2026-11-11 that the user has not run into since: a change of the stream's size (the PC's game window minimised:
  1920x1080 to 270x1080) killed the decoder and closed the stream; the stream's sound stuttered; the Vulkan/OpenGL
  semaphore is refused (glFinish fallback); the user's list then was 72 fps, microphone, gamepads, the PC's overlays,
  the headset's dashboard and not the PC's SteamVR one.
- Wi-Fi power saving was turned off by hand while looking for the cause of the VR stream's problems; it was not the
  cause.
