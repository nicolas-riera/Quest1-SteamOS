# Quest1-SteamOS

> **Disclaimer: this whole project was entirely vibecoded.** Every line of code, every
> script and this README were written by an AI (Claude), directed and tested on real
> hardware by a human who did not write the code. It works on the headset it was tested on;
> expect rough edges elsewhere, and use it at your own risk.

**Native SteamOS on the Oculus Quest 1**, the way the Steam Frame runs it: Valve's SteamOS
(Holo, arm64) boots directly on the headset as the main system, with no Android underneath.
SteamVR runs on the headset itself. Head tracking, the panels, the controllers and the
Wi-Fi are driven natively.

The system starts with `fastboot boot`. The boot image is loaded in RAM, so Meta's system
stays installed and untouched. Rebooting the headset brings it back.

> **Status: experimental, for developers.** A Quest 1 boots into SteamOS and runs SteamVR
> with the Aurora environment, 3DoF head tracking and the Touch controllers. There is no
> 6DoF yet, and SteamVR runs at about 40 fps.

## What works

| Part | State |
| --- | --- |
| Boot | Linux 4.4 (Meta's kernel with backports for systemd 258), then SteamOS Holo with systemd as PID 1. No Android |
| Access | USB network (`ssh root@192.168.77.1`), a serial console over USB, native Wi-Fi |
| Display | Both panels at 72 Hz with the lens distortion, through [Monado](https://monado.freedesktop.org/) (OpenXR) with a zero-copy path to the display controller |
| Head tracking | 3DoF from the headset IMU, read natively through Meta's SyncBoss library |
| Sensors | The proximity sensor turns the panels off when the headset is removed. The fan keeps running |
| GPU | The Adreno 540 Vulkan driver runs natively through libhybris |
| Steam | The arm64 Steam client (Big Picture) on a virtual screen in VR, also reachable over VNC |
| SteamVR | Valve's arm64 SteamVR 2.17 with the Aurora environment, through our own driver (`src/steamvr-quest1`). About 40 fps for now |
| Controllers | Touch controllers natively: buttons, triggers, grips, sticks, vibration. 3DoF with an arm model |

## What does not work yet

- **6DoF**, for the headset and the controllers. The four tracking cameras can already be
  read natively (`src/qcam`); SLAM is next.
- **SteamVR performance.** It runs at about 40 fps; the goal is 72.
- **The SteamVR dashboard.** It opens, but Steam's own panel stays empty.
- **Audio.**
- **x86 games** (FEX, Proton, Lepton) and **Steam Link** PC VR streaming.
- **An installer for the SteamOS system image**: see the warning in the next section.

## Trying it

> **Important.** The automatic releases only contain the **boot image**: the kernel and
> its small startup system. SteamOS itself is a disk image (`/data/steamos/holo.img`)
> stored on the headset. For now it is assembled by hand on the development headset, and
> it is not published: some pieces of it, such as Valve's arm64 Steam and SteamVR builds,
> cannot be redistributed. Without it, the headset stops in a rescue shell. A
> reproducible way to build that image is the next big step.

### Requirements

| Side | What |
| --- | --- |
| Headset | An Oculus Quest 1 whose **bootloader is already unlocked** (`fastboot getvar unlocked` says `yes`). This project does not cover unlocking |
| Cable | A USB-C cable that carries data |
| PC | Windows, Linux or macOS. On Windows, the headset needs a fastboot driver, such as the Oculus ADB Drivers |

### Booting

1. Open the [Releases](https://github.com/nicolas-riera/Quest1-SteamOS/releases) page. Each
   push to `main` publishes a **prerelease**. Download the latest
   `Quest1-SteamOS-build-…zip` and unzip it.
2. Plug in the headset. It can be running Horizon OS with USB debugging allowed, or already
   in its boot menu (headset off, then hold **Volume −** and press **Power**).
3. Run `boot-quest1.bat` (Windows) or `boot-quest1.sh` (Linux, macOS).

The script checks that the device is a Quest 1 with an unlocked bootloader, restarts it into
its bootloader if needed, and runs `fastboot boot` on the image. It never runs
`fastboot flash`, `erase` or `oem`. If `adb` and `fastboot` are not installed, it offers to
download Google's platform-tools next to itself.

After about a minute, the headset is reachable over USB at `192.168.77.1`. To go back to
the normal system, reboot the headset: hold **Power** for about 10 seconds.

## How it works

```
fastboot boot ─► Quest kernel 4.4 + initramfs (initrd/init)
                   └─► mounts userdata, loop-mounts /data/steamos/holo.img, switch_root
SteamOS Holo (systemd) ─► the headset's own Android partitions, read-only:
                          firmware, GPU and SyncBoss libraries (run through libhybris)
  Monado (OpenXR) ─► display controller (both panels), IMU, controllers, proximity
    ├─ xscreen: the Steam client (Xvfb) on a virtual screen
    └─ SteamVR: vrcompositor ─► Vulkan layer ─► driver_quest1 ─► OpenXR ─► Monado
```

Meta's firmware and libraries are never copied into this repository or into a release. On
the headset they are used from its own partitions, mounted read-only.

## Repository layout

| Path | Contents |
| --- | --- |
| `kernel/` | The kernel patch (`quest1-kernel.patch`) on top of Meta's published source, the config additions and the LSE atomics emulation |
| `initrd/` | The initramfs: `init`, a display test, a reboot helper |
| `device/` | Install scripts run on the headset: Android blobs, HALs, Wi-Fi, Monado, Steam session, memory tuning |
| `src/monado/` | The Monado patch: the Quest 1 driver and the display controller target |
| `src/steamvr-quest1/` | The SteamVR driver: virtual display, controllers, dashboard tool |
| `src/vklayer-steamvr/` | Vulkan layer and shim that make SteamVR run on the Adreno 540 driver |
| `src/xscreen/`, `src/qcam/`, `src/sbimu/`, `src/sbinput/` | Steam screen in VR, camera capture, IMU and controller tests |
| `tools/` | Build scripts (kernel, boot image, system image), helpers on the PC side |
| `release/` | The boot scripts shipped in the releases |
| `docs/` | Research notes, see below |
| `recon/` | The stock headset's configuration (kernel config, properties, partitions) |

## Documentation

| Page | Contents |
| --- | --- |
| [Steam Frame stack](docs/steamframe-stack.md) | How the Steam Frame's software is put together, and what the Quest needs from it |
| [SteamVR port](docs/steamvr-port.md) | Running Valve's arm64 SteamVR on the Quest 1 |
| [Steam on the GPU](docs/steam-gpu.md) | The Steam client UI on the Adreno |
| [Adreno Vulkan extensions](docs/adreno-vk-extensions.md) | The Adreno 540 driver's hidden extensions and timeline semaphores |
| [Headset IMU](docs/syncboss-imu.md) | Reading the IMU through SyncBoss, without Android |
| [Controllers](docs/controllers-native.md) | The Touch controllers through SyncBoss |
| [Lens](docs/quest1-lens.md) | Lens distortion, field of view and geometry |
| [6DoF study](docs/tracking-6dof.md) | Paths to inside-out tracking |
| [Tracking IPC](docs/tracking-ipc.md) | Talking to Meta's tracking service |
| [SteamOS image](docs/rootfs-build.md) | What the system image holds, and the plan to build and install it reproducibly |
| [Phase 0](docs/phase0-recon.md) | First survey of the headset (in French) |

## Building

The boot image is built from public sources only: Meta's
[Quest kernel](https://github.com/facebookincubator/oculus-linux-kernel), Android's GCC 4.9
toolchain, Debian's BusyBox and this repository. On an x86_64 Debian or Ubuntu machine, or
in WSL:

```sh
tools/build-boot.sh kernel work kernel-out
tools/build-boot.sh image  work kernel-out boot-native-holo.img
```

The [Prerelease workflow](.github/workflows/prerelease.yml) runs the same two commands on
every push to `main`.

The parts that run on the headset (Monado, the SteamVR driver, the Vulkan layer) are built
on the headset or cross-built in WSL. Their `build.sh` scripts and the `device/` scripts
show how.

## Safety

- Everything boots with `fastboot boot`. Nothing in this project flashes, erases or
  unlocks a partition.
- The headset's own Android partitions are only mounted read-only.
- The Linux system lives in a file on the data partition. A factory reset deletes it, and
  the headset is back to stock.
- Back up your headset's partitions before experimenting anyway.

## Legal

This project is not affiliated with, endorsed by or sponsored by Meta or Valve. Oculus,
Quest and Horizon are trademarks of Meta Platforms. Steam, SteamVR, SteamOS and Steam Frame
are trademarks of Valve Corporation.

No firmware, library or software from Meta or Valve is included in this repository or in
its releases. The kernel and its changes are GPL-2.0, like Linux. The Monado patch follows
Monado's license (BSL-1.0).
