# Installer (design, work in progress)

A Windows app installs SteamOS on a Quest 1 from start to finish, and later updates it. Nothing from Meta or
Valve is ever in this repository or in a release: the app downloads everything on the user's machine, from its
owner's server, into `%TEMP%\quest1-steamos`, and deletes that folder when it is done.

## What the user does

1. Downloads `Quest1-SteamOS.exe` (a few MB) from the GitHub releases and runs it.
2. **Welcome page:** what the app does, what is needed (a Quest 1 on Android with developer mode and USB
   debugging, an **unlocked bootloader** (how to unlock it), a USB cable, ~10 GB free on the PC, a charged
   headset), and a "Start" button.
3. **Checks** (the user plugs the headset in):
   - `adb`: a Quest 1 (`ro.product.device` = `monterey`) on Meta's last build `49845030443200410` (the
     bundled tracking stack comes from that build's OTA and must match the headset's firmware partitions,
     which are not flashed); battery level.
   - `adb reboot bootloader`, then `fastboot getvar unlocked` / `oem device-info`: bootloader unlocked,
     current slot, partition table as expected (sizes of `userdata`, `boot_a`/`boot_b`...).
   - `fastboot reboot` back to Android (the bootloader times out if left waiting).
   - Anything wrong: the page says what and how to fix it, and stops there.
4. **Warning page:** the headset will be wiped, Android and all its data are gone (how to go back to stock
   is explained). The user has to accept.
5. **Install, A to Z, no further action:** download, build on the headset, flash, first boot.
6. Done: the headset boots SteamOS; `%TEMP%\quest1-steamos` is deleted.

The app bundles `adb.exe`/`fastboot.exe` (Google's platform-tools, as scrcpy does) and uses them, not the
user's own. It needs no WSL and no other tool on the PC.

## How it works (no WSL)

Windows cannot write an ext4 file system with Linux permissions, owners, symlinks and file capabilities,
and the Holo rootfs is a `tar.zst`. So **the headset builds its own system**: the app `fastboot boot`s an
installer boot image (our kernel + an installer initramfs, in RAM only, nothing flashed yet), which brings
up the USB network the project already uses (`192.168.77.1`, NCM/RNDIS; Windows has the drivers). The app
streams the files to it; the installer, a Linux system with real tools (GNU tar with xattrs, zstd,
e2fsprogs, btrfs-progs), writes them to the headset's storage.

1. **Download** on the PC (prototype: [`prepare.py`](prepare.py), tested 2026-11-04, sources pinned in
   [`sources.json`](sources.json), every file checked against a hash):

   | What | From | Status |
   | --- | --- | --- |
   | Our files | a folder of this repository's source tree (not a release): scripts, configs, prebuilt arm64 binaries, the boot and installer images | scripts work; **to do**: the prebuilt folder |
   | SteamOS Holo core aarch64 | `holo-packages.steamos.cloud` (public), `system.rootfs.zst` (tar.zst), 385 MB | works |
   | Steam client arm64 | Valve's client CDN, channel `steam_client_linuxarm64` (public): 35 zips, ~1 GB | works |
   | SteamVR arm64, GTK 2 | **the Steam Frame's recovery image (public on Valve's servers)**: it ships `/opt/steamvr` and the GTK 2 libraries `steamui.so` needs. SteamVR has no other public arm64 download (app 250820's public depots are x86) | **to do**: pin the image, find its layout (the rootfs is probably btrfs: extracted on the headset with `btrfs restore`, no kernel support needed). GTK 2 also works from Debian's arm64 package (tested) |
   | Meta's tracking stack, GPU driver, firmware | Meta's last Quest 1 OTA (`q1_49845030443200410.zip`, files.cocaine.trade): `system`, `modem` (+ `boot` to restore stock) from its `payload.bin` | works: all three are **bit-identical** to a dump of a headset on that build |

   Meta: only the files the system uses are kept, not the whole 2.7 GB `system` image. **To do**: the list
   (`meta-files.txt`), recorded on the development headset (files opened while the tracking stack, the
   GPU, audio, Wi-Fi and the cameras run), and the app's reader for the OTA's ext4 `system` and vfat
   `modem` images (read-only, small).
2. **Check and boot the installer:** `adb reboot bootloader`, `fastboot boot installer.img`.
3. **Build on the headset:** format `userdata` (56 GB) as ext4 (no `metadata_csum`, mountable by kernel
   4.4), unpack Holo, run our setup scripts (`device/native-setup.sh`, `device/holo-*.sh install`) in a
   chroot, copy Meta's files to `/usr/lib/quest1/meta` (`device/holo-android-blobs.sh` uses them instead of
   the Android partitions), Steam to `/home/steamos/.local/share/Steam`, SteamVR to `/opt/steamvr` (then
   `device/holo-steamvr.sh install`). Packages that Holo lacks (pacman) come with our files, prebuilt.
4. **Flash** (from the installer, after the build succeeded): our boot image to `boot_a` and `boot_b`,
   `system_a`/`system_b` erased, slot `a` active. **Never touched:** the boot chain and firmware
   (`xbl`, `abl`, `tz`, `rpm`, `hyp`, `pmic`, `keymaster`, `cmnlib*`, `devcfg`, `ovrtz`, `modem`,
   `bluetooth`...) and the per-device data (`persist`, `private` = the calibration the tracking reads,
   `vision` = Meta's tracking maps, `misc`, `frp`, `devinfo`): none of them is in the OTA or can be rebuilt.
   `initrd/init` then mounts `userdata` as the root (today it loop-mounts `/data/steamos/holo.img`).
5. **Reboot** into SteamOS; the app deletes `%TEMP%\quest1-steamos`.

**Back to stock** (a button in the app): the OTA's `boot.img` and `system.img` to both slots, `userdata`
wiped; the headset boots Meta's OS and can update itself.

## Updates

The user must not reinstall for each improvement. The system is split so that each part updates on its own:

- **Ours** (scripts, configs, binaries, Monado, the SteamVR driver, the Vulkan layer...): versioned as a
  whole (`image-version` + a manifest of components with hashes, in the repository). The app (or the
  headset itself, later) compares it with `/usr/lib/quest1/version` on the headset over the USB network
  while SteamOS runs, pushes the changed components and runs their install scripts. A new kernel/initramfs
  is written to `boot_a`/`boot_b` the same way.
- **Steam** updates itself, as on a Steam Deck. **SteamVR** comes with a newer Frame image: the app offers it.
- **Holo base:** a new Holo release means a rebuild of the root file system; `/home` (the Steam library,
  settings) is kept on its own subvolume or partition so that a rebuild does not lose it. To decide.

## Releases (when the app ships)

- `Quest1-SteamOS.exe` is the release asset; releases are normal releases (not prereleases), starting at
  **0.0.1**.
- The prerelease workflow (`.github/workflows/prerelease.yml`, boot image built on each push) is removed
  then: the boot and installer images and our binaries live in a folder of the repository, which the app
  downloads.

## App technology (to decide)

A single small `.exe`: Go or Rust (HTTP, zip, zstd, a hash, adb/fastboot driving, a minimal ext4/vfat
reader are all simple there), UI through WebView2 (present on Windows 10/11) or a native toolkit.
`prepare.py` is the reference for the download step.
