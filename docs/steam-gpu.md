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
                    --enable-angle-features=disableFlippingBlitWithCommand
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

## Adreno 512.555 blob workarounds (ANGLE features)

- `supportsExtendedDynamicState` **off**: with `useVertexInputBindingStrideDynamicState`
  (`vkCmdBindVertexBuffers2EXT` strides) every draw renders nothing - clears worked, Chromium's output was
  fully transparent black. Disabling only that sub-feature is enough; the whole extension is disabled to
  stay away from the other dynamic states of this old driver.
- `disableFlippingBlitWithCommand` **on**: `glBlitFramebuffer` from the window surface came out wrong
  (`vkCmdBlitImage` path); ANGLE enables this for Qualcomm on Android only.

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
