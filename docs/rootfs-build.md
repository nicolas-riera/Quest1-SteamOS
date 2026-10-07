# Toward a reproducible SteamOS image (holo.img)

The releases ship the boot image only. The system itself, `/data/steamos/holo.img`, was
assembled by hand on the development headset between 2026-10-05 and 2026-10-07. This page
lists what went into it, which parts can be rebuilt from public sources, and how a release
could install it. Offline notes: nothing here has been run yet.

## What is in the current image

| Layer | Source | Public | How it got there |
| --- | --- | --- | --- |
| Holo Core aarch64 | `holo-packages.steamos.cloud/holo-core-aarch64-preview/mash-20251118.3/system.rootfs.zst` | yes | `tools/mkholo.sh` (ext4, no `metadata_csum`, mountable by kernel 4.4) |
| Native boot setup | `device/native-setup.sh`, `device/firstboot.sh` | yes | run in a chroot from Android (`device/enter.sh`) |
| Packages | Holo repositories (pacman) | yes | see the list below |
| libhybris | `github.com/libhybris/libhybris` + `Halium/android-headers` (halium-11.0, version set to 10) | yes | built on the headset, prefix `/opt/hybris` |
| bionic TLS shim | `src/bionictls` | yes | built on the headset |
| Android blobs and HALs | the headset's own `system_a` / `modem_a`, mounted read-only | no, never copied | `device/holo-android-blobs.sh`, `holo-android-hals.sh`, `holo-android-modem.sh` |
| Monado | `gitlab.freedesktop.org/monado/monado` @ `cfa6078` + `src/monado/quest1-monado.patch` | yes | built on the headset, `device/holo-monado.sh` |
| Wi-Fi, memory | `device/holo-wifi.sh`, `device/holo-perf.sh` | yes | run on the headset |
| x11vnc | LibVNCServer 0.9.15 + x11vnc 0.9.17 | yes | built on the headset (`-DCMAKE_POLICY_VERSION_MINIMUM=3.5`) |
| xscreen, Steam session | `src/xscreen`, `device/holo-steam-session.sh` | yes | built on the headset |
| Vulkan shim and SteamVR layer | `src/vklayer-steamvr` | yes | cross-built in WSL |
| SteamVR driver | `src/steamvr-quest1` | yes | cross-built in WSL |
| gtk2 (for `steamui.so`) | the Steam Frame rootfs | **no** | copied to `/usr/local/lib` |
| Steam client (arm64) | the Steam Frame's `steam.tar.zst` | **no** | unpacked in `~steamos/.local/share/Steam`; it then updates itself |
| SteamVR 2.17 (arm64) | the Steam Frame's `/opt/steamvr` | **no** | copied to `/opt/steamvr`, with local edits |

### Packages installed with pacman

`base-devel git cmake meson ninja python vulkan-icd-loader vulkan-tools vulkan-headers
wayland wayland-protocols libdrm libglvnd mesa-utils gdb eigen glslang shaderc libbsd hidapi
libusb libxrandr libxcb openxr python-jinja strace openssh xorg-server-xvfb xorg-xwininfo
xorg-xdpyinfo xorg-xauth networkmanager wpa_supplicant iw iproute2 libjpeg-turbo libpng zlib
openssl libxinerama libxcursor libxdamage libxcomposite libxi xorg-xwd alsa-lib at-spi2-core
libcups gdk-pixbuf2 ibus openal libpipewire libpulse libva cairo pango lsof libvdpau
sdl2-compat xorg-xset`

### libhybris configuration

```sh
./configure --prefix=/opt/hybris --enable-arch=arm64 --enable-adreno-quirks --enable-wayland \
  --with-android-headers=/opt/hybris/include/android \
  --with-default-hybris-ld-library-path=/vendor/lib64/egl:/vendor/lib64/hw:/vendor/lib64:/system/lib64:/apex/com.android.runtime/lib64/bionic
make CFLAGS="-g -O2 -DVK_ENABLE_BETA_EXTENSIONS"
```

## Still only on the headset

These must be copied back into the repository the next time the headset is connected,
before an image can be rebuilt without it:

- `/usr/local/bin/steamvr-q1`, `/root/vrstart.sh` and the dashboard service command line;
- the `/opt/steamvr/bin/vrwebhelper/linuxarm64/vrwebhelper.sh` edits (`.orig` kept next to it);
- the keys set in `~steamos/.local/share/Steam/config/steamvr.vrsettings`;
- any local change in `/root/libhybris` (`git -C /root/libhybris diff`);
- the exact list of installed packages (`pacman -Qqe`), to check the list above.

## Proposed build

1. **Base image in CI.** GitHub's `ubuntu-24.04-arm` runners are arm64, so the Holo rootfs
   can run in a plain chroot or `systemd-nspawn` without emulation. Unpack
   `system.rootfs.zst` into an ext4 image (`tools/mkholo.sh`), run `native-setup.sh` and
   `firstboot.sh`, install the packages, then build libhybris, Monado, x11vnc, xscreen and
   our libraries inside it. Everything in that image is public.
2. **What cannot be redistributed** (Steam client, SteamVR, gtk2) stays out. The image
   would boot to the 3DoF OpenXR runtime, Wi-Fi and SSH. The user then adds Steam and
   SteamVR from their own copies, with a script in the image.
3. **Size.** The image holds about 2.2 GB of data. Compressed with zstd it should stay under
   GitHub's 2 GiB limit per release file. Splitting it is the fallback.

## Installing it without Android root

The boot image already mounts `userdata` read-write before `switch_root`. An "install"
mode in `initrd/init` could do the rest without any Android tool:

1. The boot script runs `fastboot boot` with an install image (same kernel, `holo-mode` =
   `install`).
2. `init` mounts `userdata`, creates `/data/steamos` (a new top-level directory, so it is not
   file-based-encrypted, like the one created by hand), and serves a small receiver on the
   USB network (busybox `nc` or `httpd`).
3. The PC script sends `holo.img.xz` (busybox has `unxz`, not zstd). `init` decompresses it in place, checks its SHA-256,
   syncs, and reboots into the normal boot image.

This writes one file into the data partition and nothing else. A factory reset still
removes it.
