# Installer (design, work in progress)

The PC app will install SteamOS on a Quest 1 in three steps: **prepare** (download), **build** (make the
image) and **flash**. Nothing from Meta or Valve is ever in this repository or in a release: the app downloads
everything on the user's machine, from its owner's server, into `%TEMP%\quest1-steamos`, and deletes that
folder once the headset is flashed.

## 1. Prepare: `prepare.py` (done, tested 2026-11-04)

Full run: ~1.4 GB downloaded, ~6.4 GB staged (Steam 3.4 GB with its zips kept in `package/`, Meta 2.7 GB),
about 10 minutes on a fast connection; a second run re-uses the verified downloads.

`python prepare.py [--workdir DIR] [--only STEPS] [--cleanup]`. Standard library only. Every download is
checked against a hash. The sources are pinned in [`sources.json`](sources.json):

| What | From | Status |
| --- | --- | --- |
| Our files | this repository's source tree (GitHub zip of a branch) | works |
| SteamOS Holo core aarch64 | `holo-packages.steamos.cloud` (Valve, public), `system.rootfs.zst`, 385 MB | works |
| Steam client arm64 | Valve's client CDN, channel `steam_client_linuxarm64` (public): 35 zips, ~1 GB, each with its SHA-256 in the manifest | works |
| GTK 2 (for `steamui.so`) | Debian 12 arm64 `libgtk2.0-0` (gone from Holo and Arch Linux ARM) | works |
| Meta's tracking stack, GPU driver, firmware | Meta's last Quest 1 OTA (`q1_49845030443200410.zip`, files.cocaine.trade), `system` + `modem` (+ `boot`, to restore stock) extracted from its `payload.bin` | works: all three are **bit-identical** to a dump of a headset on that build |
| SteamVR arm64 | not public: SteamVR (app 250820) has only x86 public depots. Candidate: depot 4128110 (linux, ~1 GB, gated by a hidden DLC that came with the Steam Frame) | **open**: test with the user's Steam login (DepotDownloader, QR login) |

## 2. Build (to do)

Needs Linux (WSL 2 on Windows). From `stage/`:

1. `holo/system.rootfs.zst` unpacked into an ext4 image (`tools/mkholo.sh`), then `device/native-setup.sh`,
   `firstboot.sh` and the `device/holo-*.sh install` scripts, as the dev headset was set up
   ([docs/rootfs-build.md](../docs/rootfs-build.md)). Packages (pacman) need an arm64 chroot: qemu-user in WSL,
   or an arm64 machine.
2. Our arm64 binaries (Monado, libhybris, the SteamVR driver, the Vulkan layer, q1trackd, ovrstub,
   ptprovider...): built by CI and kept in a folder of the repository, so the app only downloads them.
   **To do**: that folder and the CI job (today they are built on the headset or in WSL).
3. Meta: `meta/system.img` and `meta/modem.img` copied to `/usr/lib/quest1/meta/`.
   `device/holo-android-blobs.sh` loop-mounts them (read-only) when they exist, else the Android partitions
   (dual-boot dev setup).
4. Steam: `steam/` to `/home/steamos/.local/share/Steam` (the client updates itself from there).
   GTK 2: `gtk2/` to `/usr/local/lib` (the `.symlink` files list the links to make).
   SteamVR: `/opt/steamvr`, then `device/holo-steamvr.sh install`.
5. The boot image: `tools/build-boot.sh` (public sources only).

## 3. Flash (to do; "full" mode wipes Android)

Needs an unlocked bootloader. The app runs fastboot, and the user confirms first.

- **Never touched:** the boot chain and firmware partitions (`xbl`, `abl`, `tz`, `rpm`, `hyp`, `pmic`,
  `keymaster`, `cmnlib*`, `devcfg`, `ovrtz`, `modem`, `bluetooth`...), and the per-device data: `persist`,
  `private` (calibration read by the tracking stack), `vision` (Meta's tracking maps), `misc`, `frp`, `devinfo`.
  None of them is in the OTA or can be rebuilt.
- **Written:** `boot_a` and `boot_b` (our kernel and initramfs); `userdata` (56 GB) becomes the SteamOS root
  file system (a sparse ext4 image, grown to the partition at first boot). `system_a` and `system_b` are
  erased: SteamOS carries Meta's images itself.
- `initrd/init` has to mount `userdata` as the root itself (today it loop-mounts `/data/steamos/holo.img` on
  the Android data partition).
- **Back to stock:** the app keeps the OTA's `boot.img` and `system.img`, flashes them to both slots and wipes
  `userdata` (the OTA has everything a stock headset needs on those partitions).
- Then `prepare.py --cleanup`.
