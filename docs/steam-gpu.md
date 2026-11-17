# Steam client UI on the Adreno (webhelper GPU process)

The Steam client UI is Chromium (CEF 126.0.6478.183, `steamwebhelper`). Out of the box, on the native
Quest 1 its GPU process used ANGLE's OpenGL backend on Mesa **llvmpipe** (GPU report:
`ANGLE (Mesa, llvmpipe (LLVM 21.1.5 128 bits) …)`, `gl=egl-angle,angle=opengl`) and burned 1.4-3 cores
whenever Big Picture animated. It now runs ANGLE's **Vulkan** backend on the Adreno 540 blob through
libhybris:

```
GL implementation parts: (gl=egl-angle,angle=vulkan)
GL_RENDERER: ANGLE (Qualcomm, Vulkan 1.1.128 (Adreno (TM) 540 (0x05040001)), Qualcomm Technologies Inc. Adreno Vulkan Driver-512.555.0)
```

## How

```
steam.service -> steam-q1 (QUEST1_STEAM_GPU=1, default)
   STEAM_CEF_GPU_CMD_PREFIX=steam-webhelper-gpu, steam args -cef-disable-gpu-sandbox -cef-disable-xcomposite-workaround
 steamwebhelper (browser) --gpu-launcher='steam-webhelper-gpu'
   -> /usr/local/bin/'steam-webhelper-gpu' (symlink) -> steam-webhelper-gpu:
        LD_PRELOAD  libbionictls (hybris TLS) + libquest1_vkredirect (dlopen("…/libvulkan.so.1") -> shim)
        env         QUEST1_VKSHIM_WSI=1 QUEST1_VKSHIM_LAYER=none
        args        --use-angle=vulkan --disable-angle-features=supportsExtendedDynamicState
                    --enable-angle-features=disableFlippingBlitWithCommand --avoid_stencil_buffers
      steamwebhelper --type=gpu-process: ANGLE Vulkan -> quest1_vkshim (X11 WSI) -> hybris libvulkan -> Adreno blob
```

Pieces (sources in `src/vklayer-steamvr/`, installed by `device/holo-steam-session.sh`):

- **X11 WSI in the shim** (`quest1_wsi_x11.c`, built into `quest1_vkshim`'s `libvulkan.so.1`, enabled by
  `QUEST1_VKSHIM_WSI=1`). The Android loader under libhybris only has Android/Wayland surfaces; this adds
  `VK_KHR_xcb_surface`, `VK_KHR_xlib_surface` and a `VK_KHR_swapchain` for them, like Mesa's software X11
  WSI: swapchain images are ordinary optimal images; `vkQueuePresentKHR` submits a pre-recorded copy into
  a host-visible (cached) buffer waiting on the app's semaphores; a per-swapchain thread waits for it,
  pushes the pixels with MIT-SHM (`xcb_shm_put_image`, PutImage strips as fallback) and syncs with the X
  server (`GetGeometry` round trip, which also detects resizes -> `VK_SUBOPTIMAL_KHR`, a destroyed window
  -> `VK_ERROR_OUT_OF_DATE_KHR`). Acquire returns an idle image and signals the semaphore/fence with an
  empty submission; app queue submissions are serialized with a mutex because of that. Non-X11 surfaces
  and swapchains pass through. Generic: any Vulkan app that resolves through
  `vkGetInstanceProcAddr` (ANGLE/volk, SteamVR...) can use it with the shim; the shim does not export
  the core entry points, so apps linking `libvulkan` directly (vkcube) don't.
- Installed separately from SteamVR's shim: `/usr/local/lib/quest1-vk-steam/libvulkan.so.1` (SteamVR uses
  `/usr/local/lib/quest1-vk/`), linked with `-z nodelete`: Chromium loads Vulkan once to collect GPU info,
  unloads it, and ANGLE loads it again; a second copy of the shim crashed in the already initialized
  hybris loader.
- **dlopen redirect** (`quest1_vkredirect.c`): ANGLE opens `libvulkan.so.1` from its own directory first
  (Steam ships a Khronos loader there, which finds no ICD), so `LD_LIBRARY_PATH` is not enough.
- **GPU launcher**: Steam turns `STEAM_CEF_GPU_CMD_PREFIX` into `--gpu-launcher='<value>'` *with* the
  single quotes, and Chromium `execvp()`s that token as is; a symlink literally named
  `'steam-webhelper-gpu'` in `/usr/local/bin` (in `PATH`) makes it work. With a launcher, Chromium execs the
  GPU process instead of forking it from the zygote, so only that process gets the hybris environment.
- Device nodes: `/dev/kgsl-3d0` and `/dev/ion` group `render` 0660 (`/etc/udev/rules.d/70-quest1-gpu.rules`,
  steamos is in `render`); the GPU process also opens `/dev/hwbinder` (0666 by `60-quest1-binder.rules`).

Steam knobs found in the client binaries (`steamclient.so`, `chromehtml.so`): `-cef-use-vulkan`
(adds `--use-angle=vulkan` and features `Vulkan,DefaultANGLEVulkan,VulkanFromANGLE`), `-cef-disable-gpu`,
`-cef-disable-gpu-sandbox`, `-cef-force-gpu`, `-cef-disable-xcomposite-workaround`, env
`STEAM_CEF_GPU_CMD_PREFIX`, `STEAM_DEBUGCEF`. We do not use `-cef-use-vulkan` (Skia-Vulkan compositing in
Chromium needs more than the WSI); ANGLE-Vulkan with GL compositing is enough.

## Steam's VR pages: GPU queue limit, readback memory, memory budget

While SteamVR runs, Steam's VR UI pages are off-screen (CEF OSR): the GPU process composites each 1920x1080 frame and
reads it back for the browser process, which hands it to vrclient. Three settings of the launcher keep that from
eating the headset (measurements: docs/performance.md, 2026-11-08 night):

- `QUEST1_VKSHIM_MAX_INFLIGHT=4` (quest1_wsi_x11.c): at most 4 submissions of this process waiting for the GPU. It
  runs on the low-priority ring behind vrcompositor; unthrottled, 17-20 batches waited (Steam seconds behind the laser)
  and the buffers ANGLE frees only when the GPU is done piled up to ~700 MB more GPU memory, which froze the headset.
- `QUEST1_VKSHIM_CACHED_READBACK=1` (quest1_vkshim.c): the blob's HOST_CACHED|HOST_COHERENT memory type is
  write-combined (`test_readback`: CPU reads ~220 MB/s); it is reported without HOST_CACHED and buffers may also use
  the real cached (non-coherent) type, so ANGLE reads each frame back from cached memory (~36 -> a few ms of CPU).
- `--force-gpu-mem-available-mb=384` (was 192): Skia keeps its scratch textures instead of allocating new 12 MB ones
  every frame.

## Adreno 512.555 blob workarounds (ANGLE features)

- `supportsExtendedDynamicState` **off**: with `useVertexInputBindingStrideDynamicState`
  (`vkCmdBindVertexBuffers2EXT` strides) every draw renders nothing - clears worked, Chromium's output was
  fully transparent black. Disabling only that sub-feature is enough; the whole extension is disabled to
  stay away from the other dynamic states of this old driver.
- `disableFlippingBlitWithCommand` **on**: `glBlitFramebuffer` from the window surface came out wrong
  (`vkCmdBlitImage` path); ANGLE enables this for Qualcomm on Android only.
- Chromium workaround `--avoid_stencil_buffers` (GPU process switch named after the workaround):
  stencil-based drawing does not work on this stack. Skia's stencil-then-cover path renderer, used for
  paths too big for its small-path atlas, filled SVG icons over their whole bounding box: most Steam
  UI icons of ~32 px and more came out as solid squares/blobs (very visible in the VR dashboard, whose
  panels are rendered at a high scale: Home/Downloads/Power menu icons, search, Stream button...).
  With the workaround Skia draws such paths (and complex clips) from coverage masks. Reproduced and
  verified with cefsimple on a page of Steam's own 36x36 icons at 16/24/32/48 px. The root cause in
  ANGLE/the blob (stencil attachment or stencil state) was not investigated further.

## Steam's xcomposite workaround

Steam's webhelper redirects its CEF windows offscreen (XComposite) and re-composites them in a
"GL Composer Thread" (SDL3 + GLX = Mesa llvmpipe again, texture-from-pixmap). With the UI rendered fast on
the GPU, that composer became the bottleneck (~2 cores while navigating). `steam-q1` passes
`-cef-disable-xcomposite-workaround` in GPU mode: CEF windows are plain X child windows and our
swapchains present into them directly. Menus/overlays (main menu, VR modal) render fine.
`QUEST1_STEAM_CEF_ARGS=` (empty) keeps the composer.

## Measurements (Big Picture, 1280x800 Xvfb, headset otherwise running Monado + xscreen)

| | GPU process | browser | renderer | Xvfb |
|---|---|---|---|---|
| before, llvmpipe, navigating the home carousel | 301 % | 30 % | 58 % | 3 % |
| Adreno + Steam composer, navigating | 84 % | 205 % (llvmpipe composer) | 116 % | 32 % |
| Adreno, no composer, idle home screen | 0-4 % | 6 % | 1-3 % | 2 % |
| before, home screen 10 min after boot (first sample, UI busy) | 228 % | 158 % | 21 % | 11 % |

cefsimple (Steam's CEF test app) on an animated page with 20 blurred/shadowed moving boxes (800x600,
`bench.sh` below): **Adreno 60 fps**, GPU process 55 % + renderer 55 %; **llvmpipe 25 fps**, GPU process
105 % + renderer 117 % - about 5x less CPU per frame. MIT-SHM upload of a 1280x800 frame costs the X
server ~0.5 ms; Xvfb is at ~30 % at 60 fps.

## Known issues

- **`backdrop-filter` content is mirrored vertically** (blurred background behind Big Picture menus and
  modals shows the page upside down). Reproduced with cefsimple and a two-color page with a
  `backdrop-filter: blur()` box (`/tmp/bd.html` in the test notes below); fine with llvmpipe. ANGLE-level
  tests of the same operations (window -> texture with `glCopyTexImage2D`, sub-rect `glCopyTexSubImage2D`
  drawn back, `glBlitFramebuffer` incl. flipped, with/without `EGL_SURFACE_ORIENTATION_INVERT_Y_ANGLE`)
  all pass, and no copy/blit reads the swapchain image, so it is in how Chromium/Skia read the root pass
  on this renderer; `forceDriverUniformOverSpecConst` and `--disable-gpu-driver-bug-workarounds` don't
  change it. Also seen: `glReadPixels` on the default framebuffer returns rows top-down while draws are
  bottom-up, which may be the same root cause (ANGLE default-framebuffer Y flip on this driver).
- **While SteamVR runs**, the webhelper's VR overlay renderer (browser process, `logs/webhelper_vr.txt`:
  "Initializing graphics device … Loaded GL 4.5") draws with GLX on llvmpipe: ~2 cores (5 llvmpipe threads
  at 40 %). Not touched here; candidates: Mesa zink on top of this shim (needs timeline semaphores - the
  compat layer emulates them - and the X11 WSI), or an EGL/GLES path. Steam also shows "This device is
  being used in VR" over Big Picture then.
- Startup is still heavy (~1 min of 100-200 % in browser/renderer while Big Picture loads): that is
  JavaScript/layout, not rendering.
- The WSI copies every frame in full (no damage tracking, `VK_KHR_incremental_present` regions ignored).

## Revert / debug

- `QUEST1_STEAM_GPU=0` in steam.service's environment (e.g. a drop-in
  `/run/systemd/system/steam.service.d/gpu.conf` with `[Service]` `Environment=QUEST1_STEAM_GPU=0`, then
  `systemctl daemon-reload; systemctl restart steam`) -> Mesa llvmpipe as before.
- `QUEST1_STEAM_GPU_ARGS` adds GPU-process args (e.g. `--use-angle=swiftshader`).
- Shim logging: `QUEST1_VKSHIM_DEBUG=1` (device/queue, swapchains, every 300th frame with a pixel
  sample), `QUEST1_VKSHIM_TRACE=1` (render passes/copies into and out of swapchain images),
  `QUEST1_WSI_NO_SHM=1`. GPU process stderr ends up in `~/.local/share/Steam/logs/steamwebhelper.log`;
  the Chromium GPU report in `logs/webhelper_gpu.txt`, errors in `logs/cef_log.txt`.
- Tests (headset, sources in `src/vklayer-steamvr/`): `test_wsi_x11` (raw Vulkan through the WSI, reads
  the window back; `reload` arg = Chromium's unload/reload), `test_angle_x11` (Steam's ANGLE on an X
  window: `TEST=clear|draw|copy|blit|flipblit|backdrop`, `ORIENT=1`). Run them as steamos on a second
  Xvfb (`Xvfb :2 -fbdir /run/xvfb2`) with the same environment as `steam-webhelper-gpu`
  (and `ANGLE_FEATURE_OVERRIDES_DISABLED=supportsExtendedDynamicState` for the ANGLE test).
  cefsimple: `steamrtarm64/cefsimple --no-sandbox --disable-gpu-sandbox
  --gpu-launcher=/usr/local/bin/steam-webhelper-gpu --user-data-dir=/tmp/cefs --url=…` on `:2`.

## The browser process on ANGLE (2026-11-16, tried, not shipped)

What the browser process does with Steam's VR pages (read in steamwebhelper, arm64): an SDL3 desktop OpenGL 4.3 context
on a hidden 1x1 window (GLX: host libglvnd, Mesa llvmpipe; Xvfb has no DRI3), one RGBA8 texture per overlay
(`glTexImage2D` with no pixels), and per CEF paint a `glTexSubImage2D` of the whole BGRA frame (the dirty rect is not
used, no PBO), then `IVROverlay::SetOverlayTexture` with that OpenGL texture. No setting picks another way;
`SetOverlayRaw` takes ~200 px at most. vrclient, in that process, shares an OpenGL texture with vrcompositor through
`GL_EXT_memory_object_fd`; steam.service hides those from llvmpipe, so it takes its fallback: a blit and a
`glGetTexImage` into shared memory, on llvmpipe's threads. Mesa's zink is no way out: no GLX visual on Xvfb, and on
EGL its screen fails on this Vulkan 1.1.128 driver with no message (after our shim's `VK_EXT_debug_utils`, which the
blob refuses, is hidden).

Tried: the browser process (steamwebhelper without `--type=`) given the stream client's ANGLE libEGL.so.1/libGL.so.1
(`quest1_streamclient.c`), in a directory of its own (the stream client's fake libdrm.so.2 breaks libcef:
"undefined symbol: drmPrimeHandleToFD"), with `SDL_VIDEO_FORCE_EGL=1`, its RGBA textures made `GL_BGRA_EXT` (OpenGL ES
refuses BGRA pixels into RGBA storage) and ANGLE's `disableFlippingBlitWithCommand` (vrclient's copy into the shared
texture is upside down, and the blob's flipped `vkCmdBlitImage` left the panel flat grey). Then "Loaded GL 3.1", no
llvmpipe thread, the panel right (colours and orientation checked with test boxes). With a small box animating on the
main page for 20 s: the browser 20 % of a core before, 8-13 % after; vrcompositor at 71.6-71.9 fps instead of
61-66 (its CPU upload of the shared memory is gone). vrclient's semaphore still fails ("Failed to get cross-process
semaphore FD -1000072003", glFinish instead), as in the stream client. But ~5 minutes in, vrcompositor's context hung
the GPU (`gpu fault ctx 3`, "GPU hang detected", right after a "Wait for global ctx=5 ... error=-110" on a context
being destroyed; VK_ERROR_DEVICE_LOST, SteamVR restarted): the only GPU hang of that boot. Not shipped. Suspects: a
shared texture freed or replaced by vrclient (overlay reallocation) while vrcompositor still read it, with no
semaphore between them. The patch is not in the repo; the pieces above are what it needs.


## Steam VR page frame cost: UBWC and the readback allocation (2026-11-17)

Measured with the kgsl trace (uncontended = vrcompositor stopped for 2 s, so the batch times are the GPU's own) and a
shim that logs every render pass (`vkCmdBeginRenderPass`, areas, draws, load/store ops): a frame of Steam's 1920x1080
VR page is 2 GPU batches, ~40 render passes (22 Mpixel, 13 of them 1920 wide, 80 draws in all), 23.7 + 1.1 ms. It is
memory-bound, not draw-bound: ~1 ms per Mpixel of pass area. ANGLE creates every image with
`VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT`, which the Adreno blob does not UBWC-compress. `QUEST1_VKSHIM_NO_MUTABLE=1`
(quest1_vkshim.c, set by steam-webhelper-gpu) drops the flag on optimal colour attachments without external memory:
the heavy batch is 11-12 ms, Steam's GPU submissions 23.0 -> 35.6 a second (animated page, 3 x 3 s, steady state;
pages look right in a vrcompositor snapshot). The same flag removal in vrcompositor's process changed nothing (7.7 vs
7.8 ms).

Second limit, the CPU of the GPU process's main thread (58-61 % of a core at ~18 fps, 100 % at the ~29 fps it reaches
with the GPU free): perf shows 35 % in the kernel, and the shim's trace shows why: every frame Chromium allocates and
frees an 8.3 MB readback buffer (vkAllocateMemory 6.8 ms on average, up to 14.5 ms; kernel memset + cache
invalidate), 4 MB buffers 3-6 ms, and ~75 `vkFlushMappedMemoryRanges` of 64-1000 bytes (25-200 us each, 8 % of the
thread). `QUEST1_VKSHIM_MEM_CACHE=1` keeps freed allocations of 1 MB or more for the next one of the same size
(<= 4 per size, 48 MB, dropped after 3 s idle): 35.6 -> 39.6 submissions a second, uncontended 57 -> 68.

What does not help: GPU priority of Steam equal to vrcompositor's (`QUEST1_COMPAT_KEEP_PRIORITY`: Steam 42 batches/s but
vrcompositor falls to 44-57 fps), a bigger Skia budget (768 MB: no change), dropping backdrop filters/masks/shadows
in the page (each 2-5 ms of the 24, no visual-neutral choice). The GPU is shared 56 % vrcompositor, 19 % Monado, the
rest Steam, so 30 fps needs the Steam frame under ~7 ms GPU or less vrcompositor/Monado work.

Both flags are off again (same day): worn, Steam's UI showed corruption everywhere, white flashes and missing
pictures (the avatar, the sidebar), and the kernel logged GPU page faults from steamwebhelper (48 in one 90 s
hover/scroll run, none in four later runs with any combination: intermittent). Without the flags the user found
the UI clean. A likely cause for NO_MUTABLE: ANGLE does view some images in another format (sRGB/UNORM pairs), which
the flag promises it won't; MEM_CACHE reuses memory the GPU may still read. To find out which one, and a safe form
(e.g. drop the flag only for formats with no view of another format), before turning either back on.

## White panels, missing text and pictures: the V4L2 shim's stale fds (fixed 2026-11-17)

The corruption that stayed with both flags off (whole panels white, the sidebar missing, pages without text or
pictures, in Steam and in the dashboard) was the webhelper's GPU process losing its shared memory. Its
`logs/cef_log.txt` had thousands of `raster_command_buffer_stub.cc: Failed to map shared state buffer`, then
`glRasterCHROMIUM: Can not read paint buffer`, `Invalid font discardable handle`, `Invalid shm for discardable
handle`, `Can not read transfer cache entry data`: no raster context for a renderer (a white panel), no glyphs (text
gone, the DOM still had it), no decoded images. strace showed no failing `mmap` at all.

`libquest1_v4l2.so` (Steam's encoder and stream-client shim, src/v4l2shim) is preloaded into the Steam client and so
into every webhelper process. Chromium's GPU process probes the venus decoder (Steam enables `V4L2VideoDecode`); the
shim tracks that fd as a decoder, and Chromium closes it without going through the shim's `close()`. The number then
went to a shared memory region, and the shim served its `mmap` as a decoder buffer: EINVAL, no system call. The
bursts came with each Remote Play start (2800 failures at 18:32 for one session) and at startup.

Fix (quest1_v4l2.c `find()`, quest1_vdec.c `q1dec_find()`): an encoder or decoder remembers the device and inode its
fd was opened on, and a lookup that hits checks them with `fstat`; a stale entry is forgotten and logged
("fd 68 is no longer the decoder (closed behind the shim): forgotten", seen in steamwebhelper.log right after the fix).
No cost when no encoder or decoder is tracked.

