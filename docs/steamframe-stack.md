# Steam Frame software stack — offline analysis for the Quest 1 port

Source: owner's Steam Frame image, rootfs = partition 3 (`/mnt/sf/p3`), mounted read-only in WSL.
Image identity (`/usr/share/deckard/version`): `VERSION_ID=0.3.0`, `BUILD_ID=20260922.5152327`,
`BUNDLED_STEAMVR_VERSION=2.17.10`. The kernel is `6.18.0-gfbdbca41fd45`. The SoC is SM8650: the cv driver ships `adsp_sm8650.mbn`/`cdsp_sm8650.mbn`.
The analysis is static only: strings, readelf, unit files and scripts. Nothing was run on any device.
The image has a private package repo whose URLs were deliberately not read or reproduced here.

Confidence tags: **[H]** = directly shown by a file or string. **[M]** = strong inference from several strings. **[L]** = educated guess.

Extracted material stays in WSL `/root/sfstr/*.txt` (string dumps, kernel config `/root/sfstr/kconfig`) and
`/root/sfsteam/steamrtarm64/` (a few files pulled from `steam.tar.zst`). None of it is in the project tree.

---

## 1. How SteamVR on the Frame gets frames to the panels

### 1.1 Kernel/GPU base
- **[H]** The display uses mainline DRM/KMS: `CONFIG_DRM_MSM=y`, `DRM_MSM_DPU/DSI/MDSS/KMS=y` and the panel driver `CONFIG_DRM_PANEL_DECKARD=y`
  (`panel-deckard.ko`, `panel-deckard-plano.ko`). There is **one DSI connector driving both eyes at 4320x2160**:
  `/usr/bin/displays_turn_off_drm_master` runs `modetest -M msm ... DSI-1 ... RESOLUTION="4320x2160"`.
- **[H]** The GPU driver is Mesa **Turnip** (freedreno Vulkan). The only ICD is `/usr/share/vulkan/icd.d/freedreno_icd.aarch64.json`.
  `/usr/share/deckard/mesavars.sh` sets `VRCOMPOSITOR_TU_DEBUG=sysmem,preempt`, and `vrcompositor-launcher` copies it into `TU_DEBUG`.
- **[H]** `vrcompositor-launcher` (`vrcompositor-launcher.sh` just execs it) raises the ambient capability **CAP_SYS_NICE**, which is needed for high-priority queues.
  It then execs `vrcompositor` with `VRCOMPOSITOR_LD_LIBRARY_PATH` / `VRCOMPOSITOR_LD_PRELOAD`.

### 1.2 vrcompositor output backends (strings in `bin/linuxarm64/vrcompositor`)
vrcompositor has three HMD output paths, plus an experimental compositor called Prism:

| Path | Evidence | Used on the Frame? |
|---|---|---|
| `CHmdWindowVulkanWSI`: Vulkan **direct mode** through `VK_KHR_display` + `VK_EXT_acquire_drm_display` / `VK_EXT_direct_mode_display` + `VK_EXT_display_control` | `vkAcquireDrmDisplayEXT`, `vkGetDrmDisplayEXT`, `vkCreateDisplayPlaneSurfaceKHR`, `vkGetSwapchainCounter`, `CVulkanDirectModeVblankThreadWSI`, `Creating CHmdWindowVulkanWSI!`, `drmOpenWithType`, `drmModeGetConnector` | **[M] yes**. gamescope does not own KMS here (it runs `--backend openvr`). After SteamVR stops, `steamvr.service` runs `ExecStopPost=/usr/bin/displays_turn_off_drm_master`, which has to "become the drm master" to blank the panels, so vrcompositor held DRM master. The kernel threads `card0-crtc*` are re-prioritised in `set_kernel_thread_priorities.sh`. |
| `CHmdWindowSDL`: X11 RandR lease, or a **Wayland `wp_drm_lease_v1`** lease | `No support for wl_drm_lease. This is needed for VR under Wayland.`, `WaylandHMDLeaseDevice::*`, `GAMESCOPE_WAYLAND_DISPLAY` | Desktop Linux path. **[M]** Not used on the Frame. |
| **Driver direct mode**: `IVRDriverDirectModeComponent_001..008(_009 in driver_cv)` and **`IVRVirtualDisplay_001/002`** ("display redirect") | vrserver: `Headset is using driver direct mode`, `CDriverDirectSubmitFrameThread`, `DriverDirectMode::SubmitFrameThread sending legacy vsync events automatically.`, `Using display redirect device id=%d for driver direct mode.`, `TrackedDeviceClass_DisplayRedirect`, `CVRMsg_CreateSwapTextureSet`, `Failed to initialize interop texture: bad dma-buf fd`. vrcompositor: `DoDriverDirectPresent`, `DoVirtualDisplayPresent`, `[Driver Direct] SubmitLayer`, `disableLinuxWaitForPresent` | **[H] Compiled into Linux arm64 SteamVR**, and swap textures cross processes as **dma-buf fds**. Nothing in the image uses it for the panels. VR Link host drivers on PCs use this kind of path. |
| **Prism** (`libprism.so`, `vrprismhost`, `drivers/prism`): a newer render-graph compositor (AMD RPS + Valve "facet" Vulkan layer, motion estimation, "frame hallucination") | Exports `PRISM_RegisterDisplaySubsystem`, `PRISM_AcquireDisplay`, `PRISM_SetDisplayMode`, `CPrismDisplay_Null`. `driver_cv.so` imports `PRISM_RegisterDisplaySubsystem` and `PRISM_GetConnectedDisplays`. `drivers/prism/driver.vrdrivermanifest` has `"redirectsDisplay": true` | **[H] Disabled**: `drivers/prism/resources/settings/default.vrsettings` contains `"driver_prism": {"enable": false}`. **[M]** It is the future path, where the HMD driver supplies the display backend as a Prism plugin. |

`resources/settings/default.vrsettings` contains `"direct_mode": {"enable": true, edidVid/Pid list...}`. The Frame's settings in `drivers/frame_hmd/resources/frame_hmd_additional.vrsettings` include
`useFacetRenderer: true`, `enableLinuxVulkanAsync: true`, `enableScanoutScaling: true`, `defaultPerAppRefreshRate: 72`,
`renderSystemLayerInDistortPass: true` and `motionSmoothing: false`. **[M]** The compositor can also be split into processes
(`-personality_distort`, `-personality_systemlayer`, with a standalone `systemlayer` binary).

### 1.3 Where the Frame's HMD driver actually lives
- **[H]** `drivers/frame_hmd` and `drivers/frame_controller` are **resourceOnly** manifests: settings, input profiles and firmware only.
- **[H]** The real driver is **`drivers/cv/bin/linuxarm64/driver_cv.so`**. Its manifest has `"hmd_presence": ["28DE.2300"]`, and it reads `{frame_controller}/config/*.json`.
  It links `libArcturusPerception.so` (tracking) and talks to the DSPs through `dsp_service` and `XRService`.
  It contains `IVRDisplayComponent_003`, `IVRDriverDirectModeComponent_009` and `IVRVirtualDisplay_002`.
  It also drives the panels' side-band controls itself: `Panel Init`, `Backlight Init`, `ERROR setting displays on/off`, `/dev/spidev0.1`, a "DisplayFPGA", gpiochip lines and `prismCorrection*`.
  It reports EDID VID/PID (`direct_mode_edid_vid/pid`) so the compositor can pick the KMS display.

### 1.4 What vrcompositor wants from Vulkan/kernel
- **[H]** Extension strings in vrcompositor: `VK_KHR_display`, `VK_EXT_acquire_drm_display`, `VK_EXT_direct_mode_display`, `VK_EXT_display_control`,
  `VK_EXT_display_surface_counter`, `VK_EXT_external_memory_dma_buf`, `VK_EXT_image_drm_format_modifier`, `VK_KHR_external_memory_fd`,
  `VK_KHR_external_semaphore_fd`, `VK_KHR_timeline_semaphore`, `VK_KHR_dynamic_rendering`, `VK_KHR_present_id/present_wait`,
  `VK_EXT_global_priority`, `VK_EXT_queue_family_foreign`, `VK_KHR_sampler_ycbcr_conversion`, `VK_EXT_extended_dynamic_state(3)`, and others.
  It also has `CVRMsg_CompositorImportDmaBuf`, `GetDmabufFormats/Modifiers` and the env var `STEAMVR_DISABLE_DMABUF_MODIFIERS`.
  Static analysis cannot tell which of these are required and which are optional (it has both "Required ..." and "Optional ... unavailable" messages). **[L]** dma-buf import, external fds and timeline semaphores are very likely required.
- **[H]** Frame kernel features: `SYNC_FILE=y`, `DMABUF_HEAPS(_SYSTEM)=y`, `UDMABUF=y`, msm DRM with syncobj (gamescope has `DRM_IOCTL_SYNCOBJ_EVENTFD`).
- **[L]** `COpenXRDirectModeBridge` in vrcompositor and systemlayer (`libopenxr_loader.so` is NEEDED) can drive output through an OpenXR session.
  The message `Missing internal interfaces - check your OpenXR runtime setting` suggests it expects SteamVR's own OpenXR runtime. It is probably not a generic "run SteamVR on top of Monado" bridge, and testing that would be cheap.

**Answer to "could SteamVR run on a different HMD driver that provides the display?"**
**[H]** Yes in principle: Linux arm64 SteamVR contains driver-direct-mode and virtual-display support, with dma-buf swap texture sets. A custom OpenVR driver
could take the compositor's distorted or undistorted eye buffers and scan them out itself, for example via MDSS `MSMFB_ATOMIC_COMMIT`, which accepts ION/dma-buf fds.
Whether vrcompositor would initialise on the Quest 1 Vulkan stack is a separate problem; see section 5.

---

## 2. PC VR streaming ("Steam Link" VR) on the Frame

- **[H] Client binary**: `/opt/steamvr/tools/vrlink/bin/linuxarm64/vrlink`, started through `run_vrlink.sh`. `run_vrlink.sh` sets `TU_DEBUG=gmem`,
  `VRLINK_SUBMIT_DELAY_US=6000` and `VRLINK_SAVE_VIDEO_DATA=0`. The other launchers are `run_vrlink_udp.sh <ip>` (= `run_vrlink.sh -c <ip>`) and `vrlinkquery`.
  The binary is part of **SteamVR**, not of the Steam client.
- **[H] Who launches it**: the Steam client. `steamrtarm64/steamclient.so` contains `/tools/vrlink/bin/linuxarm64/run_vrlink.sh`, placed next to the
  stream-client args (`-I %llu -G %llu -A %u -C %llu`, `-c %s`, `-e`), plus `CRemoteClientManager`, `VRLinkInviteChanged_t`,
  `EVRLinkCaps`, `-vrlinkforceenable`, Wi-Fi AP pairing messages (`CMsgRemoteClientPairWifiAP`) and the dongle classes (`CRemoteClientDongle`).
  Discovery, pairing and session setup therefore go through Steam's Remote Play / Remote Client machinery, and Steam then spawns vrlink.
  The polkit policy `org.valve.steamos.steamvr.policy` also mentions a `tools/vrlink/.../stream_mode.sh` helper, which is not present in this image.
- **[H] What vrlink is**: a **local OpenVR scene application**. Its NEEDED list is `libopenvr_api.so`, `libpipewire-0.3`, `libpulse` and `libX11`.
  It uses `IVRCompositor_029`, `IVRSystem_026`, `IVROverlay_029`, `IVRMailbox_002`, `IVRBlockQueue_005` and `IVRSystemLayerInternal_XXX`.
  It runs `WaitGetPoses`, sends the poses to the PC and submits the decoded frames as layers (`SR Submit Layer 0/1/2/N`).
  The **on-headset SteamVR compositor (vrcompositor + driver_cv)** therefore does the final reprojection and scan-out. PC-side SteamVR uses `driver_vrlink` (string in vrserver).
- **[H] Protocol hints**: "SVL" (Steam VR Link) has the classes `SVLDataLinkUDP`, `SVLDataLinkTransferUDP` and `SVLFECRAID6` (Reed-Solomon/RAID6 FEC over UDP, galois tables).
  It also has `Key setup failure in transport subsystem` (encrypted), `-c <IP> connect to UDP`, `-b bind UDP (server)`, `-d <device>` (bind interface) and
  `-n paired link access point network`. Audio goes through `SVLClientAudio` on PipeWire. Settings live in `vrlink.client` / `preferredRefreshRate: 120` (frame_hmd settings).
- **[H] Decoder**: **V4L2 stateful hardware decode** (`SVLCodecV4L2`, `vrdevice_path video_decode`, `V4L2_EVENT_SOURCE_CHANGE`, DISPLAY_DELAY controls).
  The codec is **HEVC** (`test.h265` debug dump). Decoded CAPTURE buffers are imported into Vulkan as dma-buf: vrlink needs
  `VK_EXT_external_memory_dma_buf`, `VK_EXT_image_drm_format_modifier`, `VK_KHR_external_memory_fd`, `VK_KHR_external_semaphore_fd` and
  `VK_KHR_timeline_semaphore` (renderer `SVLRendererVulkan`).
  The kernel side is `CONFIG_VIDEO_QCOM_IRIS=m` (`iris.ko`). `steam-video-codec-setup.service` creates `/dev/video-dec0` and `/dev/video-enc0` from `vrdevice_path`.
  **There is no MediaCodec, no Android and no Vulkan Video in vrlink.**
- **[H] Flat Remote Play** (desktop or game streaming, not VR) is a different binary: `steamrtarm64/streaming_client` from the Steam client.
  It has V4L2 decode (`CV4L2Accel`, `/dev/video-dec%d`, `STEAMLINK_V4L2_BUFFER_COUNT`) with DRM, EGL or **Vulkan** frame output, an FFmpeg software fallback (`libvideo.so`, libavcodec 62),
  Vulkan Video strings (`VK_KHR_video_decode_h264/h265/av1`) and codecs H264/HEVC/AV1/"Pyrowave".
  In VR it shows itself as an **SDL3 OpenVR overlay** (`--openvr`, `SDL_OPENVR_OVERLAY_*`, `steamlink_openvr-overlay`).

---

## 3. Session startup on the Frame

1. **[H]** SDDM autologin (`/etc/sddm.conf.d/steamos.conf`) uses `User=steamos` (uid 1000, home `/home/steamos`) and `Session=gamescope-wayland.desktop`,
   which runs `start-gamescope-session gamescope-session.target`. That script exports `XDG_SESSION_TYPE=x11` and runs `systemctl --user --wait start gamescope-session.target`.
2. **[H]** `gamescope-session.target` (user) contains `Requires/BindsTo gamescope-session.service` and `Wants=` steamvr, steamvr-logs, steamvr-proxmicmute,
   steamvr-v4l2cam, steam, steam-notif-daemon and ibus-gamescope. `gamescope-session.service.wants` pulls in `pidbridge` and `steamos-manager`.
3. **[H]** `gamescope-session.service` runs `/usr/lib/steamos/gamescope-session`, which execs `gamescope` with
   `--allow-deferred-backend --backend openvr --vr-session-manager --vr-overlay-* ... --xwayland-count 2`, then
   `--nested-width 1280 --nested-height 720 --output-width 1920 --output-height 1080 --steam --mangoapp --virtual-connector-strategy PerAppId
   --vr-overlay-key valve.steam.gamepadui.fallback --vr-app-overlay-key valve.steam.desktopgame`.
   gamescope **does not touch KMS**. Its openvr backend (`gamescope::COpenVRConnector/COpenVRPlane/COpenVRFb::Import(wlr_dmabuf_attributes*)`) turns Xwayland
   surfaces (Steam UI and flat games) into **SteamVR overlays**. That is how the Steam UI appears in VR.
   The unit also has `PartOf=steamvr.service`, so if SteamVR dies, the session dies.
4. **[H]** `steamvr.service` (`Requires=gamescope-session.service`) runs these steps:
   - `ExecStartPre`: `steamvr_first_init.sh`, which copies `/usr/share/deckard/openvrpaths.vrpath` (runtime `/opt/steamvr`, config and logs in `~/.local/share/Steam/...`) and
     links `~/.config/openxr/1/active_runtime.json` to `/opt/steamvr/steamxr_linuxarm64.json` (library `bin/linuxarm64/vrclient.so`).
   - `ExecStartPre`: `steamvr apply-setpath` and `merge-down-overlays`. A Steam-managed SteamVR (app 250820 or 330050) can override `/opt/steamvr`.
   - `ExecStartPre`: `select_steamvr.sh bin/linuxarm64/vrstartup`.
   - `ExecStart`: `vrcmd --background --waitforquit`.
   - `ExecStop`: `vrstartup -shutdown`.
   - `ExecStopPost`: `displays_turn_off_drm_master`.
   - `EnvironmentFile=/usr/share/deckard/mesavars.sh`.
5. **[H]** `steam.service` (`After=steamvr.service`) runs `ExecStart=/usr/share/deckard/select_steam.sh RUNSTEAM.sh`, which ends in
   `~/.local/share/Steam/steamrtarm64/steam -cef-enable-debugging -deckard -gamepadui -steamdeck -steamos3 -vrgamepadui -skipinitialbootstrap`.
   The env vars are `STEAM_LAUNCH_WRAPPER_AFFINITY_LIST=0xf8`, `STEAM_LAUNCH_WRAPPER_SCOPE=true`, `STEAM_DEBUGCEF_FAKEUI=1`, `STEAM_ALLOW_DRIVE_ADOPT/UNMOUNT=1`,
   `STEAM_COMPAT_TOOL_MAPPINGS="546560 100 steamrt4-any"`, `STEAM_LAUNCH_WRAPPER_AUDIO_NAMESPACE=0` and the mesavars.
   The per-game env defaults are in `/usr/share/deckard/steam_launch_wrapper_env_defaults.txt`, copied to `$XDG_RUNTIME_DIR/steam/env/gfx.txt`. They include
   `PROTON_USE_PREPOPULATED_PREFIX=1`, `PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES/LAYERS=1`, `ENABLE_MESA_VRAM_REPORT_LIMIT=1` and Turnip autotune flags.
6. **[H]** Other services:
   - `steamvr-v4l2cam.service` runs `v4l2cam --output=99` into a v4l2loopback device (`steamvr-v4l2loopback.service` creates `video_nr=99 card_label="SteamVR"`). It is used for screenshots, recording and broadcast (`/etc/bashrc.d/vrshortcuts.sh`).
   - `steamvr-proxmicmute` and `steamvr-logs` (`steamvr_logs_to_journald.py`) are small helpers.
   - `steamvr-nested-desktop.service` runs a nested Plasma (kwin_wayland 1280x800) shown via gamescope overlay key `valve.plasma.desktopmode`.

---

## 4. FEX, Proton, Lepton, Steam client

### Steam client (native arm64)
- **[H]** The bootstrap is `/usr/lib/steam/steam.tar.zst` (700 MB, 14 472 entries). It is a **complete pre-installed client**, not a small bootstrap.
  `steam-health-check` ("triplefrog" repair) untars it into `~/.local/share/Steam` when the `.install-complete` sentinel file is missing.
  `bin_steam.sh` (`/usr/bin/steam` → `/usr/lib/steam/steam`) hard-codes the platform `steamrtarm64` and execs `$LAUNCHSTEAMDIR/steamrtarm64/steam`. It refuses to run as root.
- **[H]** Top-level contents of the tarball:
  - `steamrtarm64/` (171 files): native client. It includes `steam`, `steamclient.so`, `steamui.so`, `steamwebhelper` + `libcef.so` (CEF), `streaming_client`, `libvideo.so`,
    `libminigbm.so`, `libvulkan.so.1`, `libEGL.so`/`libGLESv2.so` (ANGLE), `swiftshader/` + `libvk_swiftshader.so`, `gameoverlayui`, `reaper`, `steamservice.so` and ffmpeg/SDL3 libraries.
  - `linuxarm64/`: the arm64 SDK, with `steamclient.so` (for games), `steam-launch-wrapper` and `pw-audio-namespace`.
  - `steamrt64/` (7 600 files, including `pv-runtime/steam-runtime-steamrt`) and `steamrt32/`: x86 parts that run under FEX.
  - `linux32`/`linux64` `steamclient.so` and `ubuntu12_32/64`.
  - `androidarm64/`: `libsteamclient.so`, `steamservice.so` and `libsteamnetworkingsockets.so` for Android titles and Lepton.
  - `legacycompat/`, `*.dll` (Windows-side client libraries for Proton), and `controller_base/`, `steamui/`, `tenfoot/`, `friends/` resources.
  - A `package/beta` file. The client is on an arm64 beta channel, whose name is not reproduced here.
- **[H]** `RUNSTEAM.sh` sets `LD_LIBRARY_PATH=$STEAMROOT/steamrtarm64` and creates the `~/.steam/{sdk32,sdk64,sdkarm64,binarm64,bin32,bin64,root,steam}` symlinks.
  If FEX is installed, it computes `STEAM_RUNTIME_LIBRARY_PATH` through `steamapps/common/FEX-Emu/fex-compat-tool run -- .../steam-runtime/run.sh`.
- **[H]** The `steam` binary is NEEDED only on libc, libdl, libm and libpthread, and loads everything else at runtime. The CEF UI ships its own ANGLE and SwiftShader, so a GPU is not mandatory.

### FEX-Emu and SteamLinuxRuntime_4
- **[H]** Both are **Steam-delivered apps**, not OS packages. `/usr/bin/FEXBash` requires `~/.steam/steam/steamapps/common/FEX-Emu` and `.../SteamLinuxRuntime_4`.
  It chains `sdkarm64/steam-launch-wrapper -- binarm64/reaper SteamLaunch -- FEX-Emu/fex-compat-tool waitforexitandrun -- SteamLinuxRuntime_4/run --devel -- ...`.
  steamclient.so contains `DEBUG: adding FEX dependency to %s %u`, `STEAM_COMPAT_FEX_CONFIG` and `Software\Valve\Steam\CompatFexOptions`.
  **No binfmt_misc registration** exists in the rootfs (`/usr/lib/binfmt.d` is empty): FEX is invoked explicitly as a compat tool.
- **[H]** `/usr/share/guestos/fex-mesa` (964 MB) is an x86_64/i386 Arch-based guest graphics provider (`graphics_provider.json`). It contains **x86 builds of Mesa Turnip**
  (`libvulkan_freedreno.so`, `freedreno_icd.x86_64.json`, `libGLX/EGL_mesa`), which run emulated under FEX against the real msm DRM.
- **[H/M]** Proton is **ARM64EC** (`steam_launch_wrapper_env_defaults.txt` references `deckard/proton-arm64ec`, with `PROTON_USE_PREPOPULATED_PREFIX=1`).
  The x86 code inside Wine runs through FEX's arm64ec backend. `proton-stable` and `proton-experimental` are compat-tool names in steamclient.so.

### Lepton (Android)
- **[H]** Lepton is a **Steam compat tool for Android titles**. steamclient.so has the tool name `lepton-stable` next to the compat-manager code, plus `AndroidSteamPackageRoot` and
  `...a android specific depot is present ... forcing android as a supported platform`. It is installed at `~/.steam/steam/steamapps/common/Lepton`
  (`vrshortcuts.sh` adds it to `PATH` and sources `liblepton/completions.sh`).
- **[H]** Lepton runs in **podman containers**. `podman.service.d/lepton-podman-timeout.conf` uses `podman system service --time=0`, and `catatonit` is mentioned. Rootless subids are `steamos:100000:65536`.
  The `pidbridge.service` unit ("Host PID bridge for Lepton containers", socket `$XDG_RUNTIME_DIR/pidbridge/pidbridge.sock`, SO_PEERCRED) maps container PIDs to host PIDs.
- **[H]** The Android-side vendor graphics are in `/usr/share/guestos/android/vendor`: Mesa for Android (`libEGL_mesa`, `libGLESv2_mesa`, `hw/vulkan.freedreno.so`),
  the Vulkan layers `VALVE_fdm_injection` and `fossilize`, and the **OpenXR API layer** `XrApiLayer_VALVE_fdm_injection`. Android XR apps are therefore supported.
- **[H]** Kernel requirements on the Frame: `CONFIG_ANDROID_BINDER_IPC=y`, `CONFIG_ANDROID_BINDERFS=y` and
  `CONFIG_ANDROID_BINDER_DEVICES="anbox-binder,anbox-hwbinder,anbox-vndbinder"` (Anbox/Waydroid-style names). There is no ashmem config (memfd is used instead).

### Notable Frame kernel config (extracted from `/boot/Image` via IKCONFIG, saved as `/root/sfstr/kconfig`)
These options are set: `ARM64_VA_BITS=48`, 4K pages, **`COMPAT` not set** (no 32-bit ARM), `USER_NS/PID_NS/NET_NS/IPC_NS/UTS_NS=y`, `SECCOMP_FILTER=y`,
`BPF_SYSCALL=y`, `CGROUP_BPF=y`, `CGROUPS/MEMCG/CPUSETS/PSI=y`, `SCHED_CLASS_EXT=y`, `PREEMPT=y`, `NTSYNC=y`, `FUTEX_PI=y`, `IO_URING=y`, `USERFAULTFD=y`,
`MEMFD_CREATE=y`, `FUSE_FS/OVERLAY_FS/SQUASHFS/EROFS=y`, `NF_TABLES=y`, `VETH/TUN/WIREGUARD=y`, `BINFMT_MISC=y`, `KVM=y`, `VIRTIO_FS`, `VSOCKETS=y`,
`SYNC_FILE=y`, `DMABUF_HEAPS=y`, `UDMABUF=y`, `HID_STEAM=y`, `HIDRAW=y`, `QCOM_FASTRPC=y`, `VIDEO_QCOM_IRIS=m`, `VIDEO_QCOM_CAMSS=y`, and the LSM list `yama,loadpin,safesetid,integrity` + AppArmor.
One out-of-tree module, `updates/v4l2loopback.ko`, is present.

---

## 5. Feasibility for the Quest 1 (MSM8998 / Adreno 540, 4.4 kernel, fbdev MDSS, Vulkan only via the Qualcomm blob through libhybris)

### Key gaps compared with the Frame
| Need | Frame | Quest 1 today |
|---|---|---|
| Display for SteamVR | KMS + `VK_KHR_display`/`VK_EXT_acquire_drm_display` | fbdev MDSS only. **[H]** There is no KMS, and the blob has no `VK_KHR_display`. |
| Vulkan | Turnip, Vulkan 1.3-class, dma-buf, modifiers, timeline semaphores | Adreno blob, Vulkan **1.1.128** (`recon/vulkaninfo_hybris.txt`): `external_memory_fd` (opaque), **AHB**, `external_semaphore_fd`, `external_fence_fd`. **No** `EXT_external_memory_dma_buf`, `EXT_image_drm_format_modifier`, `KHR_timeline_semaphore`, `KHR_dynamic_rendering` or `KHR_display`. Turnip does **not** support a5xx, so Mesa Vulkan is not an option **[H]**. |
| HEVC decode | iris V4L2 stateful, dma-buf export | `CONFIG_MSM_VIDC_V4L2=y` (venus, downstream msm_vidc). **[M]** HEVC decode should be available. The buffers are ION (= dma-buf on 4.4), and the downstream V4L2 semantics differ from upstream stateful-decoder conventions. |
| FEX x86-64 | 48-bit VA, Cortex-X4/A720 (ARMv9) | **`ARM64_VA_BITS=39`** (`/home/nicolas/q1/out-steamos/.config`). Kryo 280 is ARMv8.0 (no LSE atomics, no RCpc). **[M]** x86-64 guests expect a 47-bit address space, so many games will break. **[L]** Current FEX releases may not support ARMv8.0 well; this needs checking. |
| Lepton | binderfs, anbox-* binder nodes, Turnip for Android | 4.4 has binder (`binder,hwbinder,vndbinder`) but no binderfs. **[H]** The Android vendor graphics in `guestos/android` are Turnip and will not work on a5xx. Meta's own Android already runs Android apps, so Lepton is low value for the Quest 1. |
| Other kernel | | `USERFAULTFD` not set, `NF_TABLES` not set (podman/netavark needs iptables mode), `USER_NS=y` (pressure-vessel/bwrap is fine), `COMPAT=y`. |

### (a) Minimum to show the native arm64 Steam client UI
1. Unpack `steam.tar.zst` into `/home/<user>/.local/share/Steam` and install `RUNSTEAM.sh` / `select_steam.sh` (or simply `bin_steam.sh`).
   Run it as a non-root user (uid 1000). Glibc arm64 is already provided by Holo.
2. An X server: **Xvfb or Xwayland under a headless wlroots compositor**. CEF can use its bundled SwiftShader/ANGLE, so no GPU is needed for a first light.
   Launch with `-gamepadui` (Big Picture). `-steamdeck -steamos3` make it behave like SteamOS. `-vrgamepadui` requires SteamVR and should be left out at first.
3. Get the pixels into the headset. Monado can show them as a quad layer: write a small OpenXR app that grabs the X root via XShm/XComposite and submits a quad layer.
   This avoids gamescope, whose openvr backend needs SteamVR, and whose Vulkan needs dma-buf, which the blob lacks.
4. Input: uinput from controllers comes later. A USB keyboard or mouse works now.
   **[M]** This is realistic in the short term. The main unknowns are CEF performance under SwiftShader on 4xA73 and whether the arm64 client self-update needs the beta channel.

### (b) PC VR streaming like the Frame
- **Route 1, Valve's own stack, as the Frame does it** (Steam client + on-device SteamVR + driver + `vrlink`). It needs all of:
  1. vrserver/vrcompositor running on the Qualcomm blob through a glibc Vulkan ICD wrapper over libhybris (Monado already does a variant of this).
  2. A **Vulkan shim layer** that adds `VK_EXT_external_memory_dma_buf` + `VK_EXT_image_drm_format_modifier` (linear only) by mapping dma-buf/ION fds to gralloc AHBs.
     Our `comp_window_mdp.c` already handles gralloc AHBs. Timeline semaphores can be added with Khronos `VK_LAYER_KHRONOS_timeline_semaphore` from Vulkan-ExtensionLayer. `dynamic_rendering`, if it turns out to be required, would also need emulation.
  3. A **custom OpenVR HMD driver** (Quest 1 3DoF IMU via libsyncboss) with `IVRDisplayComponent` + **`IVRDriverDirectModeComponent`**.
     It would receive the compositor's dma-buf swap textures and scan them out through `MSMFB_ATOMIC_COMMIT`, reusing the Monado `comp_window_mdp.c` logic. Linux arm64 SteamVR contains this path **[H]**. Whether it works without KMS or a Linux WSI is **[L]**.
  4. vrlink's V4L2 HEVC decode on the downstream venus driver, plus dma-buf import into Vulkan (again through the shim).
  5. The Steam client (arm64) for discovery, pairing and launch. PC-side SteamVR must accept the Quest 1 as a VR Link client; **[L]** it may be gated by capabilities or device type.
  **Verdict [M]:** possible but heavy. The Vulkan dma-buf shim and the direct-mode driver are the critical items. Static analysis cannot confirm that vrcompositor initialises with no KMS display. Running `vrcompositor` with driver direct mode under a null or test driver on the headset is the cheapest experiment.
- **Route 2, open alternatives on top of the existing Monado:**
  - OpenVR apps (gamescope `--backend openvr`, `streaming_client --openvr`) can run on Monado through **OpenComposite or xrizer** (OpenVR→OpenXR). **[L]**
    `vrlink` itself uses SteamVR-internal interfaces (`IVRMailbox`, `IVRBlockQueue`, `IVRSystemLayerInternal_XXX`), so it is unlikely to work there.
  - For PC VR streaming specifically, an open-source streamer whose headset client targets OpenXR/Linux, with the PC side as a SteamVR driver or OpenXR runtime, fits our Monado + fbdev design better than Valve's vrlink. **[L]** Candidates are WiVRn and ALVR; a Linux OpenXR client build plus V4L2 or software decode has to be checked for each.
- **Flat Remote Play inside VR** (desktop or games as a virtual screen) is the easy win: the Steam client's own `streaming_client` decodes via V4L2 (`/dev/video-dec0` symlink to the venus decoder) or FFmpeg software.
  Show it in an X server and composite it into Monado as in (a).

### Reusable bits from the image
- `steamxr_linuxarm64.json` (SteamVR as OpenXR runtime → `vrclient.so`) and `helloxr`, `hellovr_sdl`, `tools/hellovr_vulkan_linux` (OpenVR sample + source) and `overlay_viewer` are useful test clients.
- `openvrpaths.vrpath`, `steamvr_first_init.sh`, the `steamvr` CLI and the `select_*.sh`/`RUNSTEAM.sh` scripts can be reused almost verbatim; replace `/home/steamos` with our user.
- The gamescope args in `/usr/lib/steamos/gamescope-session` show the exact VR overlay configuration, if SteamVR ever runs on the Quest 1.
