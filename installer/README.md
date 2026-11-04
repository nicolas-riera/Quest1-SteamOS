# Installer (design, work in progress)

A Windows app installs SteamOS on a Quest 1 from start to finish, and later updates it. Nothing from Meta or
Valve is ever in this repository or in a release: the app downloads everything on the user's machine, from its
owner's server, into `%TEMP%\quest1-steamos`, and deletes that folder when it is done.

## What the user does

1. Downloads `Quest1-SteamOS.exe` (a few MB) from the GitHub releases and runs it.
2. **Welcome page:** what the app does, what is needed (a Quest 1 on Android with developer mode and USB
   debugging, an **unlocked bootloader** (how to unlock it), a USB cable, ~30 GB free on the PC (checked before
   anything starts), a charged headset), and a "Start" button.
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
and the Holo rootfs is a `tar.zst`. So **the headset builds the image, the PC flashes it**: the app
`fastboot boot`s a micro Linux (our kernel + an installer initramfs, in RAM only, nothing flashed), which
brings up the USB network the project already uses (`192.168.77.1`, NCM/RNDIS; Windows has the drivers).
The app streams the downloads to it; the micro Linux, with real tools (GNU tar with xattrs, zstd,
e2fsprogs, btrfs-progs), builds the system image, the app copies the image back to the PC, reboots the
headset to its bootloader and flashes everything with fastboot. All partition writes are plain fastboot
flashes from the PC, and the PC keeps the finished image: re-flashing or repairing needs no rebuild.
(Recovery "sideload" cannot be used: it only takes OTA packages signed by Meta.)

1. **Download** on the PC (prototype: [`prepare.py`](prepare.py), tested 2026-11-04, sources pinned in
   [`sources.json`](sources.json), every file checked against a hash):

   | What | From | Status |
   | --- | --- | --- |
   | Our files | this repository's source tree (a zip of the branch, not a release): scripts, configs; our arm64 binaries, the boot and installer images from its `prebuilt` branch, built by CI ([`prebuilt.yml`](../.github/workflows/prebuilt.yml), [`tools/build-native.sh`](../tools/build-native.sh)) | scripts work; binaries: built locally, CI to run |
   | Holo packages | Holo's pacman repositories (public, the rootfs's snapshot): [`device/packages.txt`](../device/packages.txt) + dependencies, resolved like pacman does, each file checked against the repository database | works: 367 packages, 434 MB (temporary list, it will change) |
   | SteamOS Holo core aarch64 | `holo-packages.steamos.cloud` (public), `system.rootfs.zst` (tar.zst), 385 MB | works |
   | Steam client arm64 | Valve's client CDN, channel `steam_client_linuxarm64` (public): 35 zips, ~1 GB, staged as they are (the build unpacks them) | works |
   | SteamVR arm64, GTK 2 | **the Steam Frame's recovery image**, public: `steamframe-oobe-repair-20260922.5153644-0.3.0.img.bz2` on Valve's image server (linked from store.steampowered.com/steamos/download/?ver=steamframe), 4.07 GB. It ships `/opt/steamvr` (where the development headset's SteamVR comes from) and the GTK 2 libraries `steamui.so` needs (gone from Holo and Arch ARM). SteamVR has no other public arm64 download (app 250820's public depots are x86) | works: a GPT disk (512-byte sectors: esp, efi-A, rootfs-A, var-A, home); the build cuts `rootfs-A` (btrfs) out of the bz2 stream and copies from it |
   | Meta's tracking stack, GPU driver, firmware | Meta's last Quest 1 OTA (`q1_49845030443200410.zip`, files.cocaine.trade): `system`, `modem` (+ `boot` to restore stock) from its `payload.bin` | works: all three are **bit-identical** to a dump of a headset on that build |

   Meta: native Holo runs no Android framework, no Java, no app and no 32-bit process, so the build leaves
   them out of the `system` tree ([`meta-exclude.txt`](meta-exclude.txt)): 577 MB kept of 1.9 GB (the
   native 64-bit userspace, configs, firmware, the DSPs' code). To refine with a record of the files really
   opened on the headset.
   Before anything is downloaded, `prepare.py` checks the free space of the work folder's drive: what is
   still to download + what is staged beyond the downloads + the image that comes back (12 GB), +15 %.
2. **Check and boot the installer:** `adb reboot bootloader`, `fastboot boot installer.img`: the micro Linux
   ([`micro-init`](micro-init), built by `tools/build-boot.sh installer`: our kernel + Alpine aarch64 with
   python3, GNU tar, zstd, e2fsprogs, btrfs-progs; 36 MB, in RAM only) brings up the USB network and runs
   [`agent.py`](agent.py) on `http://192.168.77.1:8077`. Nothing is written until the app asks for the
   scratch space (`POST /scratch` with the confirmation text): `userdata` is formatted and mounted.
3. **Build on the headset:** the app uploads the staged files (resumable: files already there with the same
   sha256 are skipped), starts the build and shows its progress, then downloads the sparse image
   ([`remote_build.py`](remote_build.py) is the reference client). [`build_image.py`](build_image.py)
   creates an ext4 file system the size of `userdata` (no `metadata_csum`, mountable by kernel 4.4) in an
   image file and fills it:

   | Step | What |
   | --- | --- |
   | `format`, `holo` | the file system, Holo core unpacked with its xattrs (file capabilities) |
   | `setup` | `device/native-setup.sh` in an arm64 chroot, full layout (`QUEST1_LAYOUT=full`: the root is `userdata`, swap file made at first boot) |
   | `pkgs` | the Holo packages, pacman offline from the staged files |
   | `frame` | `/opt/steamvr` and GTK 2 from the Frame image (`rootfs-A` cut out of the bz2 stream), then `device/holo-steamvr.sh install` from outside (`ROOT=`: no python3 in Holo core) |
   | `steam` | Valve's client zips unpacked into `~steamos/.local/share/Steam` |
   | `meta` | Meta's trimmed `system` tree and the `modem` image in `/usr/lib/quest1/meta` (`device/holo-android-blobs.sh` mounts them) |
   | `native` | our binaries (`native-arm64.tar.zst`), then [`device/install-image.sh`](../device/install-image.sh) in the chroot: the `steamos` user, the units set up by hand on the development headset, every `device/holo-*.sh install`, the SteamVR driver's registration |
   | `ours`, `finish`, `sparse` | this repository in `/usr/lib/quest1/repo` and the version file, `e2fsck`, the Android sparse image (only the used blocks) |

   Tested 2026-11-04 in WSL 2 (the arm64 chroot through qemu-user) on the files `prepare.py` staged.
4. **Flash** from the PC (`adb`/`fastboot` reboot to the bootloader): `fastboot flash userdata` with the
   sparse image (fastboot splits it to the bootloader's download size), our boot image to `boot_a` and
   `boot_b`, `system_a`/`system_b` erased, slot `a` active. Cost of the round trip: ~8-10 GB back over
   USB and as much free space on the PC, about 10 more minutes. **Never touched:** the boot chain and firmware
   (`xbl`, `abl`, `tz`, `rpm`, `hyp`, `pmic`, `keymaster`, `cmnlib*`, `devcfg`, `ovrtz`, `modem`,
   `bluetooth`...) and the per-device data (`persist`, `private` = the calibration the tracking reads,
   `vision` = Meta's tracking maps, `misc`, `frp`, `devinfo`): none of them is in the OTA or can be rebuilt.
   `initrd/init` mounts `userdata` as the root when it holds a system (`/usr/lib/systemd/systemd`), else
   the development layout (`/data/steamos/holo.img` in Android's `userdata`).
5. **Reboot** into SteamOS; the app deletes `%TEMP%\quest1-steamos`.

**Back to stock** (a button in the app): the OTA's `boot.img` and `system.img` to both slots, `userdata`
wiped; the headset boots Meta's OS and can update itself.

## Progress (for the app)

`prepare.py` and `build_image.py` print a human log plus machine lines the app parses:

- `@space {"need": bytes, "free": bytes}`: the free-space check (prepare.py stops when it fails)
- `@step {"step": name, "status": "start"|"done"}`: one per step (download: `repo prebuilt holo pkgs steam
  frame meta`; on the headset: `upload`, then the build's `format holo setup pkgs frame steam meta native
  ours finish sparse`, then `download`)
- `@progress {"step": name, "done": n, "total": n}`: bytes of the step (downloaded, read, written), at most
  twice a second, so each step gets its own bar.

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
  then: the boot and installer images and our binaries are on the `prebuilt` branch (one commit, replaced by
  each CI build, so the history does not grow with binaries), which the app downloads.

## To check on the headset

Written from the scripts and docs only, the development headset being unplugged (2026-11-04): the
`holo-android-blobs` and `fakelogd` units, the `steamos` user's groups (`device/install-image.sh`), the
SteamVR driver registration (`openvrpaths.vrpath`), the libhybris commit and Monado's CMake options
(`tools/build-native.sh`), the vrcompositor Vulkan shim (built as Steam's), the bionic stubs `q1trackd` and
`libq1camtap.so` link against, the Meta exclusions, the micro Linux on the 4.4 kernel (Alpine's musl).

## App technology (to decide)

A single small `.exe`: Go or Rust (HTTP, zip, zstd, a hash, adb/fastboot driving, a minimal ext4/vfat
reader are all simple there), UI through WebView2 (present on Windows 10/11) or a native toolkit.
`prepare.py` is the reference for the download step.
