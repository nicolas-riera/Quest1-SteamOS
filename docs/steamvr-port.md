# SteamVR (Linux arm64) on the Quest 1: offline analysis

Source: the SteamVR 2.17.10 bundled in the owner's Steam Frame image at `/opt/steamvr` (rootfs `/mnt/sf/p3`, mounted read-only in WSL).
The analysis is static only: readelf, strings, `aarch64-linux-gnu-objdump` 2.44 disassembly, cross-references from code to `.rodata`/`.data`, and the public OpenVR SDK 2.15.6 (`/root/svr/openvr`).
Work files are in WSL under `/root/svr/{str,dis}` plus the helper scripts `xref.py`, `dump.py`, `xcode.py`, `ann.py`, `vt.py` and `isa.py`. Nothing was copied into the project tree.
See `steamframe-stack.md` for the general stack. Target facts: blob Vulkan 1.1.128 (`recon/vulkaninfo_hybris.txt`), 4.4 kernel, MDSS fbdev, no WSI.

Confidence tags: **[H]** = shown directly by a file, a string or a disassembled instruction. **[M]** = strong inference. **[L]** = educated guess.

---

## 1. Inventory

### 1.1 `bin/linuxarm64` (sizes in bytes) [H]
| File | Size | Role |
|---|---|---|
| vrserver | 6 104 456 | Server; loads the drivers |
| vrcompositor | 5 964 624 | Compositor (statically contains the "facet" renderer) |
| vrcompositor-launcher(.sh) | 10 632 | Adds `CAP_SYS_NICE`, then execs vrcompositor |
| systemlayer | 5 948 240 | Split-out system-layer compositor personality |
| vrclient.so | 6 470 544 | In-app OpenVR client (also the OpenXR runtime) |
| libopenvr_api.so | 245 064 | OpenVR loader |
| vrstartup / vrcmd / vrpathreg / vrurlhandler | 759 240 / 648 400 / 269 984 / 697 704 | Launch and CLI tools |
| vrmonitor | 3 979 248 | Qt 5.7.1 status window (bundled `qt/lib`, `platforms/`); optional |
| vrdashboard | 393 872 | Dashboard process |
| libprism.so / vrprismhost | 1 013 808 / 504 592 | Prism compositor (disabled) |
| libopenxr_loader.so, libsteam_api.so | 412 672 / 381 904 | Bundled |
| vraudiocompositor.so + libphonon.so | 280 192 + 37 133 008 | Audio |
| audiofilter.so, vraudioloadermodule.so, proxmicmute, v4l2cam, trackingtest, overlay_viewer, hellovr_sdl, helloxr, openvr_api_canary | small | Tools and samples |
| libpulsecommon-10.0.so | symlink | Dangling Debian-path symlink to `.../pulseaudio/libpulsecommon-16.1.so`. Harmless. |

`bin/vrwebhelper/linuxarm64/`: `vrwebhelper` (2.4 MB) and `cefsimple`, plus `libcef.so` (218 MB) with its own ANGLE `libEGL/libGLESv2` and `libvk_swiftshader.so`.
**[H]** `vrwebhelper.sh` refuses to start unless `xset q` succeeds, so the dashboard UI needs an X server.

### 1.2 `drivers/` [H]
| Driver | Content |
|---|---|
| `cv` | The real Frame HMD driver: `driver_cv.so` (4.9 MB) + `libArcturusPerception.so` (45 MB), `libmediapipe_linux_arm64.so`, `libonnxruntime_linux_arm64.so`. Manifest: `hmd_presence ["28DE.2300"]`. |
| `prism` | Manifest has `"redirectsDisplay": true`. `bin/` is empty. Its `default.vrsettings` sets `driver_prism.enable=false`. |
| `frame_hmd`, `frame_controller`, `indexhmd`, `indexcontroller`, `htc` | `resourceOnly: true` |
| `deckard` (firmware only), `oculus` (input profiles and anims only) | Resources only, no manifest |

**There is no `drivers/null` and no `driver_null.so` [H].** vrserver still knows the `driver_null` settings section and the `VR_FORCE_TEST_DRIVER` env var. `resources/safe_mode_driver_whitelist.json` lists `"null"`.
A custom driver is needed anyway. The public SDK `samples/drivers/drivers/simplehmd` is a usable skeleton.

### 1.3 Settings keys relevant to forcing our driver [H] (`resources/settings/default.vrsettings`)
- `steamvr`: `requireHmd: true`, `forcedDriver: ""`, `forcedHmd: ""`, `activateMultipleDrivers: false`, `enableSafeMode: false`, `enableLinuxVulkanAsync: false`, `preferredRefreshRate: 90.0`, `motionSmoothing: true`, `mirrorView: 0`, `showLegacyMirrorView: false`.
  vrserver prints `Does not match user setting "forcedDriver" for the HMD` and `Driver isn't the HMD driver and activateMultipleDrivers is false`.
- `direct_mode`: `enable: true`, `count: 4`, `edidVid0..3/edidPid0..3`. This is the KMS/`VK_KHR_display` panel whitelist. It is irrelevant for a virtual-display driver.
- `driver_defaults.vrsettings` (applied to every driver): `enable: true`, `blocked_by_safe_mode: false`, `loadPriority: 0`, `hmdAllowsClientToControlTextureIndex: true`, `forceSystemLayerUseAppPoses: true`.
- Compositor keys read at start-up (`steamvr` section, from vrcompositor disassembly): `useFacetRenderer`, `enableSimplifiedShaders`, `enableAchromaticDistortion`, `forceSceneTextureIndexZero`, `enableScanoutScaling`, `renderSystemLayerInDistortPass`, `preferredRefreshRate`.
  `useFacetRenderer` has **no default**, so it is `false` unless a driver's settings set it. The Frame sets it in `frame_hmd_additional.vrsettings`.
- Suggested `steamvr.vrsettings` for the port: `forcedDriver: "<ourdriver>"`, `activateMultipleDrivers: false`, `useFacetRenderer: false`, `enableLinuxVulkanAsync: false` at first, `motionSmoothing: false`, and `"driver_<ourdriver>": {"enable": true}`.

---

## 2. Linkage (DT_NEEDED, symbol versions) [H]

| Binary | max GLIBC / GLIBCXX | DT_NEEDED (beyond libc/libm/libdl/libpthread/librt/libstdc++/libgcc_s/ld-linux) | Notable dlopen strings |
|---|---|---|---|
| vrserver | 2.29 / 3.4.26 | libSDL2-2.0.so.0, libsteam_api.so, libsystemd.so.0, libz.so.1, libpulse.so.0, libX11.so.6 | libvulkan.so.1, libprism.so, libdrm |
| vrcompositor | 2.29 / 3.4.22 | libSDL2, libopenxr_loader.so, libsteam_api.so, libopenvr_api.so, libuuid, libz, **libGL.so.1**, libpulse, libdrm.so.2, libX11, libX11-xcb, libxcb, libxcb-randr | libvulkan.so.1, libwayland-client, libXpresent, librenderdoc, libArcturusPerception |
| systemlayer | 2.29 / 3.4.22 | same as vrcompositor | same |
| vrcompositor-launcher | 2.17 / 3.4.21 | libcap.so.2 | |
| vrstartup | 2.17 / 3.4.22 | libopenvr_api, libsteam_api | |
| vrcmd | 2.17 / 3.4.22 | libopenvr_api | |
| vrmonitor | 2.29 / 3.4.22 | libsteam_api, libopenvr_api, libQt5{Core,Gui,Widgets,OpenGL,Network,Multimedia,PrintSupport} (bundled), libGL, libpulse, libudev | |
| vrwebhelper | 2.27 | libSDL2, libopenvr_api, libcef.so (bundled), libpulse, libX11 | libcef itself needs nss/nspr, glib/gio, atk/atspi, cups, dbus, cairo/pango, gbm, drm, xkbcommon, X libraries and asound |
| libopenvr_api.so | 2.17 / 3.4.21 | none extra | |
| vrclient.so | 2.29 / 3.4.26 | libGL.so.1, libEGL.so.1, libuuid | libvulkan, libVkLayer_VALVE_fdm_injection.so |
| driver_cv.so | 2.29 / 3.4.26 | libSDL2, libArcturusPerception.so, libpulse, libudev, libatomic | libprism.so |
| libprism.so | 2.27 / 3.4.22 | none extra | libvulkan |
| vrlink | 2.17 / 3.4.22 | libopenvr_api, libpulse, libX11, **libpipewire-0.3.so.0** | libvulkan, libSDL2, libcrypto |

- Every glibc requirement is ≤ 2.29, so Holo's glibc 2.42 is fine.
- **Libraries that are not standard Arch packages:** only bundled Valve or third-party files: libsteam_api, libopenxr_loader (Arch has `openxr`), libphonon, Qt 5.7.1, libcef/ANGLE/SwiftShader, and driver_cv's perception/mediapipe/onnxruntime.
- Everything else comes from standard Arch packages, and all of them are present in the Frame's `/usr/lib`: sdl2(-compat), libpulse, systemd-libs, libx11/libxcb, libglvnd (`libGL.so.1`, `libEGL.so.1`), libdrm, util-linux-libs, gcc-libs (libatomic), libcap, pipewire, vulkan-icd-loader and zlib. libXpresent is dlopen'ed and optional.

### 2.1 Instruction set vs Kryo 280 (ARMv8.0 + crc + crypto, with LSE and LDAPR emulated by the kernel) [H]
Counts are from a full disassembly. `.inst`/SVE hits inside `.text` were checked and are embedded constants (for example the SHA-1 K values), not code.

| Binary | LSE (emulated) | LDAPR | Beyond v8.1 (would SIGILL) |
|---|---|---|---|
| vrserver | 1199 | 0 | none |
| vrcompositor | 750 | 0 | none |
| systemlayer | 747 | 0 | none |
| vrclient.so | 652 | 0 | none |
| vrmonitor | 4057 | 0 | none |
| driver_cv.so | 209 | 0 | none |
| vrlink | 109 | 0 | none (12 "SVE" and 104 `.inst` are constant data) |
| libprism / vrprismhost / vrcmd / vrstartup / vrdashboard / vrwebhelper / vraudiocompositor | 56 / 51 / 57 / 57 / 40 / 151 / 2 | 0 | none |
| libopenvr_api, libsteam_api, libopenxr_loader, libphonon, vrcompositor-launcher | 0 | 0 | none |
| libcef.so (vrwebhelper) | 51 | 0 | 3779 sdot/udot, 942 i8mm, 144 sha512/sha3, 31 SVE, 2 ldapur, 5 MTE `addg`. **[M]** These sit in Chromium/XNNPACK/BoringSSL paths that check HWCAP at runtime. |
| libArcturusPerception.so (driver_cv only, not needed) | 15 698 | 815 | 4217 SVE, 1614 RCPC2 `ldapur`, FP16. Built for ARMv9; unusable on the Quest, and we do not need it. |

- **[H]** None of the binaries we need uses outline atomics (no `__aarch64_*` helpers, no `__aarch64_have_lse_atomics`), so every atomic op is an inline LSE instruction that will trap into the kernel emulation.
- PAC/BTI appear only as HINT instructions. There is no dot-product, FP16 arithmetic, RCPC2, FlagM, FRINT32/64, BF16, LS64, MTE or SVE in any SteamVR binary we need.
- **[M] Risk:** trap cost in hot paths (shared_ptr refcounts, mutexes). Measure early. If it is too slow, binary-patch LSE sites to calls into LL/SC thunks.

---

## 3. vrcompositor Vulkan requirements

### 3.1 Which HMD window and which device it creates [H]
- At start-up vrcompositor reads the settings in §1.3 and then **always** does `new` (0x248 bytes) + `CHmdWindowVulkanWSI::CHmdWindowVulkanWSI` (call at 0x62798 / 0x62c2c).
  The `enableScanoutScaling` flag only adds a query of `ExternalProp_ScanoutScaling` first. `CHmdWindowSDL` (X11 RandR, Wayland lease) is never constructed from that path.
- The `CHmdWindowVulkanWSI` constructor (0xb54d0) does, in order:
  1. Create the instance (0xb40a8). Failure is fatal: `CHmdWindowVulkanWSI: Failed to create vulkan instance`.
  2. Create the direct-mode surface (0xb3ae8). Failure only logs `Failed to create direct mode surface` and **continues**. The step bails out with `No direct mode features present!` when `VK_KHR_display`, `EXT_direct_mode_display` and `EXT_display_surface_counter` are absent.
  3. `CreateVulkanDevice` (0xb4c20). Failure is fatal.
  4. `CreateHMDWindow` (0xb37a0, swapchain via 0xb2bf8). Failure logs `Failed to create HMD window` and returns.
     The constructor **never sets the window error field** (`+176`, which `main` tests to print `Error making window!`). Only the SDL class writes it.
  5. `Direct mode: enabled` and `InitPresent`.
- After this, `CGraphicsDevice` logs one of `Headset display is virtual` (IVRVirtualDisplay), `Headset is using driver direct mode`, `Headset display is on desktop` or `Headset is using direct mode`. Then it logs `Using Facet Renderer` or `Using Vulkan Renderer`.

### 3.2 Extension lists (tables in `.data` at 0x5b02f0–0x5b0450, used by the WSI class) [H]
The device list is `memcpy`'d (0x88 bytes = 17 entries) unconditionally into `ppEnabledExtensionNames`. If one is missing, `vkCreateDevice` fails and the compositor fails with it.
The 16-byte `{name, found}` entries are filtered with `strcmp` against the available extensions and logged as `Enabling eEXTDmaBuf support!` and similar, so they are optional.

| Extension | Status in vrcompositor (WSI path) | Quest blob |
|---|---|---|
| VK_KHR_swapchain | **required** | advertised (Android WSI, unusable). The name alone passes `vkCreateDevice`. |
| VK_KHR_image_format_list | **required** | **missing**. Trivial to fake in a layer (strip the struct). |
| VK_KHR_create_renderpass2 | **required** | yes |
| VK_KHR_dedicated_allocation, VK_KHR_get_memory_requirements2 | **required** | yes |
| VK_KHR_external_memory(_fd), VK_KHR_external_semaphore(_fd) | **required** | yes (OPAQUE_FD) |
| **VK_KHR_timeline_semaphore** | **required** | **missing** |
| VK_EXT_host_query_reset | **required** | **missing** (emulate `vkResetQueryPool` with a command buffer) |
| VK_EXT_custom_border_color | **required** | **missing** (fake: strip the struct, use the nearest standard border) |
| VK_EXT_extended_dynamic_state, VK_EXT_extended_dynamic_state3 | **required** | **missing**. The feature bits could be reported as false; whether the compositor honours them is [L]. Otherwise a layer needs per-state pipeline variants. |
| VK_KHR_sampler_ycbcr_conversion | **required** | yes |
| VK_EXT_sample_locations | **required** | **missing** (fake, report no support) |
| VK_EXT_shader_viewport_index_layer | **required** | **missing**. The blob has `multiViewport=false` and `maxViewports=1`. If shaders write `Layer`/`ViewportIndex` from the vertex shader, SPIR-V must be rewritten (for example to multiview). [L] |
| VK_EXT_display_control, VK_EXT_global_priority, VK_KHR_present_id, VK_KHR_present_wait, VK_EXT_image_drm_format_modifier, **VK_EXT_external_memory_dma_buf**, VK_EXT_debug_marker | optional | only global_priority is present |
| Instance: VK_KHR_display, VK_EXT_direct_mode_display, VK_EXT_display_surface_counter, VK_KHR_external_{memory,semaphore}_capabilities, VK_KHR_get_physical_device_properties2 | optional ("direct mode features") | the last three are present |
| Instance: VK_KHR_surface, VK_EXT_debug_utils | appended | both present |

- **Not referenced anywhere in vrcompositor [H]:** synchronization2, maintenance4, descriptor_indexing, push_descriptor, `VK_KHR_display_swapchain`.
- `dynamic_rendering` only appears in the facet list.
- **API version:** the WSI instance's `VkApplicationInfo.apiVersion` is set from `0x400000` (Vulkan 1.0) **[M]**. Core-1.2 entry points fall back to their KHR/EXT aliases (`Driver is missing vkWaitSemaphores; using vkWaitSemaphoresKHR instead`, the same for vkSignalSemaphore and vkResetQueryPool). **Vulkan 1.2/1.3 core is not required [M].**
- A second, similar table (0x5b0160..) belongs to `CHmdWindowSDL`. That class is not used here.

**Facet renderer** (only when `useFacetRenderer=true`) [H]. It is statically linked (`/data/src/facet`) and is **not a Vulkan layer**.
- Its list is `VK_EXT_host_query_reset`, `VK_KHR_external_memory_fd`, `VK_KHR_external_semaphore_fd`, `VK_EXT_global_priority`, `VK_EXT_queue_family_foreign`, `VK_NV_compute_shader_derivatives` and `VK_KHR_dynamic_rendering`. Instance: `VK_KHR_surface`, `VK_KHR_display`, `VK_EXT_direct_mode_display`, `VK_EXT_debug_utils`.
- It also hard-fails on `device does not support timeline semaphores` and `device does not support host query reset`.
- **Keep it off.** Prism (`libprism.so`) has a similar list plus `dynamic_rendering`, and it is disabled by default. No Valve implicit Vulkan layer exists in the image: the layers in `/usr/share/vulkan` are fossilize, gamescope WSI, MangoHud, vram_report_limit and the explicit VALVE_fdm_injection/rpo.

### 3.3 vrserver and vrclient also create Vulkan devices [M]
`vrcommon/vulkan.cpp` (`Required vulkan device extension is unavailable: %s`) builds its lists from static vectors:
- Instance: `external_memory_capabilities`, `get_physical_device_properties2`, `external_fence_capabilities`, `KHR_surface`, `external_semaphore_capabilities`.
- Device: `external_memory`, `external_semaphore`, **`timeline_semaphore`**, `dedicated_allocation`, `get_memory_requirements2`, `external_memory_fd`, `external_semaphore_fd`, **`image_format_list`**.
- Optional: `debug_utils`.

### 3.4 How textures and semaphores cross processes (shared resource manager) [H]
- On Linux, `SharedTextureHandle_t` is a 64-bit id in vrcompositor's `vrsharedresourcemanager` (`SharedResourceManager: id: 0x%lx type: %d refcount: %d`).
- The memory is exported with `vkGetMemoryFdKHR(handleType = OPAQUE_FD)` (0x2ace10). The fds travel over unix sockets with `sendmsg` (`Failed send FD for shared resource export`, `ReceiveSharedFd`).
- **The shared semaphores are timeline semaphores exported as OPAQUE_FD**: `VkSemaphoreCreateInfo` → `VkExportSemaphoreCreateInfo` → `VkSemaphoreTypeCreateInfo{TIMELINE}` (0x2b46b0, 0x316484). App↔compositor sync (vrclient) uses the same mechanism.
- dma-buf only enters through `ImportDmabuf` / `CVRMsg_CompositorImportDmaBuf`, which is optional and needs `EXT_external_memory_dma_buf`.

---

## 4. Driver-side display interfaces

### 4.1 Versions [H]
| Interface | Binaries | SDK 2.15.6 (`headers/openvr_driver.h`) |
|---|---|---|
| IVRDriverDirectModeComponent | adapters for _001…_008 + Latest = **_009** (vrserver, vrcompositor). driver_cv uses _009. | **_009** |
| IVRVirtualDisplay | IVRVirtualDisplay001 adapter + Latest = **_002** | **_002** |
| IVRIPCResourceManagerClient | vrserver accepts _001–_004. driver_cv/vrclient use **_004**. | _003 (usable) |
| IVRDisplayComponent | _003 | _003 |

### 4.2 Linux allocation API for drivers: `IVRIPCResourceManagerClient` [H, SDK]
- `NewSharedVulkanImage(VkFormat, w, h, renderable, mappable, compute, mips, layers, extra create/usage flags, &handle)`: **the compositor allocates** a Vulkan image and returns its handle (OPAQUE_FD-exportable memory).
- `NewSharedVulkanSemaphore(bCounting, &handle)` (counting = timeline).
- `RefResource(handle, &ipcHandle)` + `ReceiveSharedFd(ipcHandle, &fd)`: gives the driver an fd so it can import the resource into its own VkDevice.
- `GetDmabufFormats` / `GetDmabufModifiers` / `ImportDmabuf(DmabufAttributes_t{DRM_FORMAT, modifier, ≤4 planes {fd, offset, stride}}, &handle)`: wraps a dma-buf the driver allocated. The compositor needs `VK_EXT_external_memory_dma_buf` for this.

### 4.3 Driver direct mode flow [H strings + SDK; ordering M]
1. The HMD reports `Prop_HasDriverDirectModeComponent_Bool` and returns the component (`Headset is using driver direct mode`).
2. vrcompositor sends `CVRMsg_CreateSwapTextureSet` to vrserver, which calls `CreateSwapTextureSet(pid, {w,h,format,samples}, &{handles[3], flags})`.
   The driver fills the three handles from `NewSharedVulkanImage` or `ImportDmabuf`. vrcompositor then opens them (`Failed to open shared resolve texture for driver direct mode (compositor)!`, `VRInitError_Compositor_Create/OpenDriverDirectModeResolveTextures`).
3. Every frame: `GetNextSwapTextureSetIndex`, then `SubmitLayer(perEye[2]{hTexture, hDepthTexture, bounds, mProjection, mHmdPose, prediction})` per layer (`[Driver Direct] SubmitLayer - Begin/End`), then `Present(syncTexture)`, `PostPresent(throttling)` and `GetFrameTiming`.
   vrserver runs `CDriverDirectSubmitFrameThread`.
4. **The driver composites, distorts and scans out the layers itself.** This is the "drivers that implement direct mode entirely on their own" model in the SDK comment.
5. Vsync: if `Prop_DriverDirectModeSendsVsyncEvents_Bool` (2043) is false, vrserver logs `DriverDirectMode::SubmitFrameThread sending legacy vsync events automatically`. Otherwise the driver sends `VREvent_VSync` itself.

### 4.4 IVRVirtualDisplay (display redirect) [H strings + SDK; M flow]
- **Interface:** `Present(const PresentInfo_t{backbufferTextureHandle, vsync, nFrameId, flVSyncTimeInSeconds}, size)`, `WaitForPresent()` and `GetTimeSinceLastVsync(&sec, &frameCounter)`.
- **Who allocates:** the compositor allocates the backbuffer (`TextureVirtualDisplay`, `VRInitError_Compositor_FailedToCreateVirtualDisplayBackbuffer`) and composites **with its own distortion**, using our `IVRDisplayComponent::ComputeDistortion`. It then hands **one final texture per frame** to the driver.
- **Threads and timing:** vrserver runs `CVirtualDisplayServerThread` (`[VirtualDisplay] Present Begin/End`, `WaitForPresent(%d) Done`, `TimeSinceLastVsync`, `Advancing vsync timing`). Frame timing comes from `GetTimeSinceLastVsync` (`CGraphicsDevice::GetTimeSinceLastVsync/VirtualDisplay`).
- **Setup:** a display redirect device is `TrackedDeviceClass_DisplayRedirect`, and the manifest uses `"redirectsDisplay": true` (as Prism does). vrserver logs `Using display redirect device id=%d`. The HMD can also expose the component itself (`Prop_HasVirtualDisplayComponent_Bool`).
- **[L] Open question:** the GPU sync between the compositor's last submit and `Present` (no semaphore in `PresentInfo_t`). The first implementation should be conservative and wait idle before the blit, then be refined.
- **Verdict [M]:** this path is simpler for us than driver direct mode, because reprojection, overlays, chaperone and distortion stay in vrcompositor.

---

## 5. Can vrcompositor run with no WSI at all? [M]
- **No null or headless mode exists.** There is no `-nohmd` and no null driver. `vkCreateHeadlessSurfaceEXT` only appears in the entry-point table. The command-line switches are `-keepalive -debug -nodistort -nowait -crash -nocheck -showmirror -nsight -vkvalidation -disablewatchdogs -personality_distort/-personality_systemlayer`.
- **The WSI window is always constructed (§3.1)**, but missing WSI is tolerated:
  - With no `VK_KHR_display`, surface creation fails and is only logged.
  - The device is still created.
  - `CreateHMDWindow` fails, is logged, and the constructor returns without setting an error.
  - The virtual-display and driver-direct present paths (`DoVirtualDisplayPresent`, `DoDriverDirectPresent`) do not use the swapchain.
- **Prerequisites:** instance creation and **device creation with the 17 required extensions** (§3.2) must succeed.
- **Caveat [L]:** some later code may still touch the window (mirror, vblank thread, `GetSwapchainCounter`). This needs a run with `-debug` on the device to confirm.
- **Alternative:** a layer that fakes `VK_KHR_display` + `EXT_direct_mode_display` + `EXT_display_surface_counter` + `EXT_display_control` + a swapchain backed by gralloc AHBs, scanned out with `MSMFB_ATOMIC_COMMIT` (the `comp_window_mdp.c` logic).
  This makes the **Frame's own production path** (`CHmdWindowVulkanWSI`, `CVulkanDirectModeVblankThreadWSI`) work unchanged and with zero copies.
  - The display is chosen by name or EDID from `vkGetPhysicalDeviceDisplayPropertiesKHR` (`Looking for direct display through Vulkan WSI`, `- Vulkan output %d: ...`).
  - `vkAcquireDrmDisplayEXT` is only referenced from the SDL class and the dispatch table.

---

## 6. vrlink (the VR Link client) [H unless noted]
- **What it is:** `tools/vrlink/bin/linuxarm64/vrlink`, a **local OpenVR client application, not a SteamVR driver**. There is no `drivers/vrlink` on the headset; `driver_vrlink` is the PC side.
  - NEEDED: libopenvr_api, libpulse, libX11, libpipewire-0.3. It dlopens libvulkan, SDL2 and libcrypto.
  - It uses public and internal client interfaces: `IVRSystem_026`, `IVRCompositor_029`, `IVROverlay_029`, `IVRInput_011`, `IVRMailbox_002`, `IVRBlockQueue_005`, `IVRDriverDirectInternal_XXX`, `IVRCompositorSystemInternal_XXX`, `IVRSystemLayerInternal_XXX`, `IVRCameraPassthroughInternal_001` and others.
  - **[M]** It therefore only works against Valve's own vrserver/vrcompositor (not OpenComposite or xrizer).
- **Network:** a separate protocol to the PC (SVL over UDP with RAID6 FEC). Steam's remote-client stack launches it.
- **Decode:** `SVLCodecV4L2` is a V4L2 stateful decoder. Its capture buffers are imported into Vulkan.
  - **[M]** Its static extension list `{external_memory_fd, EXT_external_memory_dma_buf, EXT_image_drm_format_modifier, get_memory_requirements2}` is required. vrcommon adds timeline semaphores and image_format_list.
  - The Turnip-only hint is `TU_DEBUG=gmem` in `run_vrlink.sh`.
- **On the Quest:** it needs dma-buf import. A layer that maps an ION dma-buf fd to an AHB import would help here; see option A below.

---

## 7. Conclusions

### (a) Blockers for vrserver + vrcompositor + a custom HMD driver on the Quest 1, ranked
1. **Cross-process timeline semaphores [H requirement].** Timeline semaphores are required by vrcompositor, vrserver and vrclient, and are created exportable (`OPAQUE_FD`). The blob has none.
   The Khronos `VK_LAYER_KHRONOS_timeline_semaphore` does not handle external handles. We need our own layer: a memfd-backed shared counter as the exported "fd", futex waits, and binary semaphores or fences for the GPU side.
2. **Eight more required device extensions missing in the blob [H].** `image_format_list`, `host_query_reset`, `custom_border_color`, `sample_locations` and `extended_dynamic_state(3)` must be faked or emulated in the same layer.
   `shader_viewport_index_layer` is the risky one, because the blob has `maxViewports=1` and no `multiViewport`; it may need SPIR-V rewriting. The compositor's SPIR-V might also need features Adreno 540 lacks (no geometry shaders, `shaderStorageImageExtendedFormats=false`) [L].
3. **No WSI or display.** Rely on IVRVirtualDisplay or driver direct mode (tolerated per §5 [M], must be tested), or implement a fake-`VK_KHR_display` layer.
4. **Driver scan-out.** The driver must import the compositor's OPAQUE_FD image into its own blob VkDevice (inside vrserver), blit into gralloc/ION buffers, `MSMFB_ATOMIC_COMMIT` them, and turn MDSS vsync timestamps into `GetTimeSinceLastVsync`. [M] Whether the blob exports and imports OPAQUE_FD images across processes under libhybris must be checked on the device.
5. **Runtime glue.** A glibc Vulkan ICD wrapper over libhybris for the loader (as Monado already has). The CAP_SYS_NICE launcher (high-priority queue; it falls back by itself). An X server for vrwebhelper (dashboard) and probably vrmonitor; vrcompositor links libX11/libGL but should not need a display in this mode [L].
6. **Performance.** Thousands of inline LSE sites trap into emulation. The compositor runs on 4×A73 + Adreno 540, with an extra blit per frame in options B and C1.
7. **Not blockers:** the glibc/libstdc++ versions; ISA (no post-v8.1 instructions in the needed binaries); VA_BITS=39 (all native arm64). Vulkan 1.2/1.3 core is not required [M]. dma-buf, DRM modifiers, synchronization2 and dynamic rendering are optional or absent on the default (non-facet) path.

### (b) Recommended architecture
- **[A] Driver direct mode + a dma-buf→AHB import layer in vrcompositor: not recommended for the compositor.**
  - Swap textures do **not** have to be dma-bufs: the driver can ask the compositor to allocate them (`NewSharedVulkanImage`, OPAQUE_FD), which the blob supports natively.
  - Driver direct mode also makes our driver do layer composition, distortion and reprojection.
  - The dma-buf→AHB layer is only worth building later, for **vrlink's** V4L2 import.
- **[B] IVRVirtualDisplay: recommended for first light.** vrcompositor composites and distorts with our `ComputeDistortion`, and hands one backbuffer handle per `Present`. Our driver:
  1. Calls `RefResource` + `ReceiveSharedFd`, then imports the OPAQUE_FD into its own blob device.
  2. Blits into a gralloc AHB triple buffer and runs `MSMFB_ATOMIC_COMMIT` (reusing `comp_window_mdp.c`).
  3. Implements `WaitForPresent` and `GetTimeSinceLastVsync` from MDSS vsync.

  Requirements: the timeline-semaphore + missing-extension layer (blockers 1–2), but **no dma-buf, no modifiers and no WSI**.
- **[C] Fake-KHR_display/swapchain layer, zero-copy: better long-term.** Implement `VK_KHR_display`, `EXT_direct_mode_display`, `EXT_display_surface_counter`, `EXT_display_control` and `VK_KHR_swapchain` over gralloc AHBs + MSMFB inside the same layer. vrcompositor then runs exactly as on the Frame (`CHmdWindowVulkanWSI` + vblank thread). This removes the blit and the virtual-display sync uncertainty.
  The HMD driver shrinks to tracking + `IVRDisplayComponent` (the display is matched by name/EDID). It costs more layer surface area than B. Do it after B works, or directly if the on-device test in §5 shows the no-WSI path breaking later in vrcompositor.

**Cheapest first experiment:** on the Quest, run vrserver + vrcompositor with a stub virtual-display driver, under a pass-through layer that only lies about extension names (timeline etc.). Read `vrcompositor.txt` for how far initialisation gets: device creation, then `Headset display is virtual`, then the first `DoVirtualDisplayPresent`.
