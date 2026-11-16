# Phase 0 — Reconnaissance (2026-10-05)

## Headset (adb, no root)
- Serial QUEST-SERIAL, `vr_monterey`, Android 10, build `49845030443200410` (2024-07-31, patch 2024-07-05), slot `_a`.
- Bootloader **unlocked**: `ro.boot.flash.locked=0`, `verifiedbootstate=orange`. `ro.secure=1`, `ro.debuggable=0`, `ro.adb.secure=0`.
- No root (uid 2000 shell). SELinux presumably enforcing.
- Kernel `4.4.205-perf+` (2024-07-31), APQ8098, 3.7 GB RAM, userdata 51 GB (FBE).
- Partitions: A/B for xbl, rpm, tz, hyp, pmic, modem, abl, boot, keymaster, cmnlib(64), devcfg, ovrtz, bluetooth, system. No separate `vendor` (it is inside system). Single copy: persist (sda2), private (in fact /persist), vision (sda9, FBE), modemst1/2, fsg, fsc, devinfo, misc, frp, keystore, ssd, cdt, ddr, sec, splash, logfs, logdump, etc.

## Quest 1 ROM (`q1_49845030443200410.zip`)
- **Full** A/B OTA, the same build as the headset. Extracted to `roms/quest1/` (14 images, SHA-256 checked). The system tree is in WSL at `~/q1/sys`.
- boot.img: header v0, page 4096, `Image.gz-dtb`, legacy system-as-root (`skip_initramfs`). Magisk can patch it.
- Kernel: pstore/ramoops and RNDIS present; no SysV IPC, no binfmt_misc, no IKCONFIG.
- GPU: `vulkan.msm8998.so` V@0555 (06/2021), declares Vulkan 1.1; strings present: timeline_semaphore, external_memory_fd, external_semaphore_fd, AHardwareBuffer. Missing: dma_buf, drm_format_modifier, dynamic_rendering, present_wait.
- Tracking: native `trackingservice` (system), `libtrackingengines.so` (25 MB), `vrapiserver`, `xrspd`, HAL `vendor.oculus.hardware.sensors`, firmware `syncboss.bin`, `adsprpcd`. OpenXR runtime: `libopenxr_forwardloader.oculus.so`.

## Steam Frame ROM (`steamframe-oobe-repair-…-0.3.0`)
- GPT with 5 partitions: ESP (vfat, u-boot), vfat, rootfs **btrfs** 5 GB, var ext4, home ext4.
- SteamOS holo `VARIANT_ID=vr` 0.3.0, aarch64, kernel 6.18 (deckard), 966 packages, KDE 6.2 + gamescope.
- `/opt/steamvr`: native **linuxarm64** SteamVR (vrserver, vrcompositor, OpenXR runtime, OpenVR API drivers), drivers cv/deckard/frame_*/index/oculus/prism.
- Mesa for deckard (Turnip a7xx, of no use on the a540).
- The pacman repositories contain a token marked DO_NOT_SHARE: do not use or share it.
- Mounting: `tools/sf_mount.sh` (WSL root, read-only).
