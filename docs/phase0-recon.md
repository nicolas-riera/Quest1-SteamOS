# Phase 0 — Reconnaissance (2026-10-05)

## Casque (adb, sans root)
- Série QUEST-SERIAL, `vr_monterey`, Android 10, build `49845030443200410` (31/07/2024, patch 2024-07-05), slot `_a`.
- Bootloader **déverrouillé** : `ro.boot.flash.locked=0`, `verifiedbootstate=orange`. `ro.secure=1`, `ro.debuggable=0`, `ro.adb.secure=0`.
- Pas de root (uid 2000 shell). SELinux enforcing présumé.
- Kernel `4.4.205-perf+` (31/07/2024), APQ8098, 3,7 Go RAM, userdata 51 Go (FBE).
- Partitions : A/B pour xbl, rpm, tz, hyp, pmic, modem, abl, boot, keymaster, cmnlib(64), devcfg, ovrtz, bluetooth, system. Pas de `vendor` séparé (dans system). Uniques : persist (sda2), private (/persist en fait), vision (sda9, FBE), modemst1/2, fsg, fsc, devinfo, misc, frp, keystore, ssd, cdt, ddr, sec, splash, logfs, logdump, etc.

## ROM Quest 1 (`q1_49845030443200410.zip`)
- OTA A/B **complet**, même build que le casque. Extrait dans `roms/quest1/` (14 images, SHA-256 vérifiés). Arborescence system dans WSL `~/q1/sys`.
- boot.img : header v0, page 4096, `Image.gz-dtb`, legacy system-as-root (`skip_initramfs`). Patchable Magisk.
- Kernel : pstore/ramoops et RNDIS présents ; pas de SysV IPC, pas de binfmt_misc, pas d'IKCONFIG.
- GPU : `vulkan.msm8998.so` V@0555 (06/2021), Vulkan 1.1 déclaré ; chaînes présentes : timeline_semaphore, external_memory_fd, external_semaphore_fd, AHardwareBuffer. Absents : dma_buf, drm_format_modifier, dynamic_rendering, present_wait.
- Tracking : `trackingservice` natif (system), `libtrackingengines.so` (25 Mo), `vrapiserver`, `xrspd`, HAL `vendor.oculus.hardware.sensors`, firmware `syncboss.bin`, `adsprpcd`. Runtime OpenXR : `libopenxr_forwardloader.oculus.so`.

## ROM Steam Frame (`steamframe-oobe-repair-…-0.3.0`)
- GPT 5 partitions : ESP (vfat, u-boot), vfat, rootfs **btrfs** 5 Go, var ext4, home ext4.
- SteamOS holo `VARIANT_ID=vr` 0.3.0, aarch64, kernel 6.18 (deckard), 966 paquets, KDE 6.2 + gamescope.
- `/opt/steamvr` : SteamVR **linuxarm64** natif (vrserver, vrcompositor, runtime OpenXR, API drivers OpenVR), drivers cv/deckard/frame_*/index/oculus/prism.
- Mesa deckard (Turnip a7xx, inutile sur a540).
- Les dépôts pacman contiennent un jeton marqué DO_NOT_SHARE : ne pas utiliser ni diffuser.
- Montage : `tools/sf_mount.sh` (WSL root, lecture seule).
