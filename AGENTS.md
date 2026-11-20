# Working on Quest1-SteamOS: a guide for AI coding agents (and humans)

This file is the handoff for anyone, human or AI agent (Claude Code, Codex, Gemini, DeepSeek, Cursor...), who works
on this repository. `CLAUDE.md` and `GEMINI.md` only point here. Read it whole before your first change: most of it
was learnt the hard way, on a real headset that froze, crashed or looked fine while being wrong.

## 1. What the project is

Valve's SteamOS (Holo, arm64) runs natively on an Oculus Quest 1, the way the Steam Frame runs it: Linux 4.4 (Meta's
kernel plus backports) with systemd as PID 1, no Android underneath. SteamVR runs on the headset. Meta's tracking
service, GPU driver and SyncBoss libraries are used from the headset's own Android partitions, mounted read-only,
through libhybris.

- [README.md](README.md): what works, the boot chain, the repository layout, the docs index.
- [docs/](docs/): one page per area, written as a dated engineering log (what was tried, measured, kept). Read the
  page of the area you touch **before** touching it: most "obvious" fixes were already tried and the page says why
  they failed.
- [docs/todo.md](docs/todo.md): open items only. Done work moves to the area's page.

The display path, in one line: SteamVR's vrcompositor → our Vulkan layer (`src/vklayer-steamvr`) → our SteamVR
driver (`src/steamvr-quest1`) → OpenXR → Monado with our patch (`src/monado`) → the display controller (MDSS),
with a 72 Hz rotational re-projection. Tracking: Meta's tracking service → `q1trackd` (`src/q1track`) → shared
memory → Monado. Streaming: venus V4L2 decoder → `src/v4l2shim` (preloaded into Steam's clients and vrlink).

## 2. Safety rules (the headset is real hardware, and there is only one)

These are hard rules. Do not break them to "try something quickly".

- **Boot only with `fastboot boot`** (the image goes to RAM). Never `fastboot flash`, `fastboot erase`,
  `fastboot oem ...` or anything that writes a partition. Rebooting the headset brings Meta's system back.
- Reboot to the bootloader only when the headset is on USB right now (otherwise nobody can reach it).
- Meta's partitions are mounted **read-only**. Never write the `persist` partition or any calibration data.
- Things that hang, crash or damage the headset (all learnt on the device):
  - never stop or restart the modem within a boot: the reload hangs the SoC (watchdog, reboot into Android);
  - never call SyncBoss pairing / unpairing / firmware update functions, nor Meta's `fw_init`;
  - never enable `timeline` in `qgl_config`, never read the debugfs `adsprpc` nodes, never write venus's
    `trigger_ssr`;
  - never enlarge `/sys/kernel/debug/tracing/buffer_size_kb` (it is per CPU: kernel memory runs out, GPU hang);
  - never attach gdb to vrcompositor (detaching kills it);
  - never empty `MESA_EXTENSION_OVERRIDE` in Steam's unit (the headset froze twice);
  - never `pkill -f` / `pgrep -f` with a pattern that also appears in your own ssh command line (it kills the ssh
    session); kill by PID.
- **OLED panels burn in.** When you keep the panels on without anyone wearing the headset (benches), set the
  brightness to the minimum (`/tmp/quest1-brightness`, written as user `steamos`), keep the window short, and
  undo everything afterwards.
- **Memory is the most common cause of freezes.** The headset has 4 GB. Steam, SteamVR and their web helpers fill
  most of it; zram (zstd) and a swap file help, but starting Steam and SteamVR at the same time, or loading
  anything big while they start, can lock the system (ping answers, ssh does not). Restart only what your change
  needs. When you chase a freeze, log `/proc/meminfo` every few seconds to a file on `/userdata` (it survives the
  reboot; `/tmp` and the journal do not) and read it first after the reboot.
- **One change to the headset at a time.** If two things change in the same minutes and the headset freezes, the
  cause can't be found. With several workers (people or agents) on one headset, restarts go through the shared lock
  (section 5).
- Recovery after a freeze: hold the power button, the headset reboots into Meta's system; then
  `adb reboot bootloader` and `fastboot boot` the SteamOS boot image again. To repair the system image when
  SteamOS can't boot: boot Android with Magisk (`fastboot boot`), `losetup` + `mount -t ext4` the image
  `/data/steamos/holo.img` and fix it from there.

## 3. Legal and repository rules

- **No Meta or Valve software in the repository or in releases**: no firmware, libraries, binaries, headers taken
  from their images, or extracted assets. The installer downloads them on the user's machine.
- **Never patch Valve's or Meta's binaries.** Reading and disassembling them to understand behaviour is fine;
  the fix goes into our own code (the Vulkan layer, the driver, `LD_PRELOAD` shims, settings files).
- **Never commit personal data**: device serial numbers, Steam IDs, account or user names, e-mail addresses, Wi-Fi
  names or passwords, host names of the developer's PC, Bluetooth or controller addresses, camera images,
  passthrough snapshots, tracking maps or anchor files, audio recordings, recorded head motion. Do not paste them
  into issues, PRs or logs either. In particular, vrlink's command line and Steam's remote connection logs contain a
  connection ID: never print or copy them.
- `private/` is git-ignored and is not for you: leave it alone.
- **Everything committed is in English**: code, comments, docs, commit messages. Quotes from users are translated.
- Line endings are LF (`.gitattributes`), except `.bat`/`.cmd` (CRLF). Scripts with CRLF break on the headset.
  Python on Windows writes CRLF unless you open files with `newline='\n'`.
- Code reused from [quest2-steamos](https://github.com/URFAVF3MBOY/quest2-steamos) is allowed by its author; say
  so in the code where it is used.

## 4. Environment

The project is developed from a Windows PC with Git Bash and WSL (Debian), and a Quest 1 on USB:

- **The headset**: `ssh root@192.168.77.1` over USB networking (NCM; the PC is 192.168.77.2). Steam runs as user
  `steamos`. A serial console is also on the USB gadget (ACM), and a busybox telnet on port 23 in the initramfs
  (`tools/qsh.py`).
- **Boot**: `release/boot-quest1.sh` / `.bat` (Android → bootloader → `fastboot boot`). The kernel and boot image
  are built from public sources: `tools/build-boot.sh` (see README, "Building").
- **Cross-builds** in WSL with `aarch64-linux-gnu-gcc/g++`; SDK headers in WSL `/root/openvr-sdk`,
  `/root/OpenXR-SDK`, `/root/Vulkan-Headers` (public: OpenVR v2.15.6, OpenXR SDK, Vulkan-Headers).
- **Git Bash pitfalls**: set `MSYS_NO_PATHCONV=1` for `adb`/`fastboot` and for anything that passes `/abs/paths` to
  the headset, or MSYS rewrites them. `wsl -- bash -c '...$VAR...'` gets expanded by the outer shell: pipe a script
  to `bash -s` instead. Write scripts with real files rather than heredocs that contain `\n` in C strings.
- **Holo has no `ping` or `hostname`**; `pacman` packages are installable when the headset has a network.
- Many SteamVR/Steam pieces are only on the headset (`/opt/steamvr` is Valve's): read them there, never copy them
  into the repo.

## 5. Building and installing

| Part | Source | Build | Installed as |
| --- | --- | --- | --- |
| Boot image (kernel + initramfs) | `kernel/`, `initrd/` | `tools/build-boot.sh` (x86_64 Linux / WSL) | RAM only (`fastboot boot`) |
| Vulkan layer | `src/vklayer-steamvr` (`quest1_compat.c` is one translation unit that `#include`s its `quest1_<area>_<part>.c` files) | `build.sh` (WSL cross) or `gcc -std=gnu11 -O2 -fPIC -shared ...` on the headset | `/usr/local/lib/libVkLayer_quest1_steamvr_compat.so` |
| SteamVR driver | `src/steamvr-quest1` | `build.sh` (WSL cross) | `install.sh` → `/opt/quest1-steamvr/quest1` |
| V4L2 / vrlink shim | `src/v4l2shim` | on the headset: `gcc -std=gnu11 -O2 -Wall -Wno-unused-parameter -fPIC -shared -fvisibility=hidden` over `quest1_v4l2.c quest1_vdec.c quest1_streamclient.c quest1_vrlink.c -lpthread -lm` | `/usr/local/lib/libquest1_v4l2.so` |
| Monado | `src/monado/quest1-monado.patch` on Monado `cfa6078` | on the headset | `/opt/monado` |
| Tracking bridge | `src/q1track` (bionic-linked) | `src/q1track/build.sh` | `/usr/local/lib/q1track/q1trackd` |
| Everything native at once | all of the above | `tools/build-native.sh` (in a Holo aarch64 root) | `prebuilt/native-arm64.tar.zst` for the installer |
| System setup | `device/*.sh` | — | run on the headset; each script installs its units and files |

Rules for installing on the headset:

- Keep a copy of the installed binary before replacing it, and install atomically (copy to `X.new`, then `mv`): a
  library being loaded while it is half written crashes its process.
- The `device/` scripts are the source of truth. A change made by hand on the headset must end up in a `device/`
  script or a `src/` tree, or it is lost on the next image build.
- **Restarts go through the shared lock**: `sh /root/q1restart.sh <who> "<what>" <command...>` (from
  `device/q1restart.sh`), logged in `/run/q1-restarts.log`. It waits while `/run/q1-user-testing` exists (someone
  is wearing the headset) or while another worker holds a quiet window (`/run/q1-quiet`).
- What to restart:
  - layer or driver change → SteamVR only: `quest1-frame restart` (from `device/holo-frame-mode.sh`;
    start|stop|restart|status);
  - Monado restarted → SteamVR must be restarted after it (vrcompositor's direct connection breaks);
  - after `quest1-frame restart`, Steam Link VR needs `systemctl restart steam` before a stream starts
    (otherwise `VRApplicationError_UnknownApplication`);
  - avoid restarting Steam and SteamVR together when you can (memory, section 2).
- Flag files that the layer or the driver read in `/tmp` must be created **as `steamos`**
  (`runuser -u steamos -- touch ...`): `/tmp` has `protected_regular`, so root-owned files there can't be written
  by Steam's processes, and a root-owned snapshot trigger once made the driver write 5.6 MB every frame.

### Building the complete system image yourself, then installing it

Yes, it can be done from scratch: the repository builds the whole image itself, from public downloads only. Nothing
is taken from a running headset, and no Meta or Valve file is in the repository. The Windows app (`app/`) runs these
same steps for users. [installer/README.md](installer/README.md) has the details (sources, steps, progress lines);
this is the command sequence.

1. **Boot image and installer image** (x86_64 Debian/Ubuntu or WSL 2, packages listed at the top of
   `tools/build-boot.sh`):
   ```sh
   tools/build-boot.sh kernel    work kernel-out
   tools/build-boot.sh image     work kernel-out out/boot-native-holo.img   # the SteamOS kernel + initramfs
   tools/build-boot.sh installer work kernel-out out/installer.img         # the micro Linux that builds the image
   ```
2. **Our arm64 binaries** (libhybris, Monado + patch, the SteamVR driver, the Vulkan layer and shims, q1trackd,
   ovrstub...): `tools/build-native.sh <repo> out/native-arm64.tar.zst`, as root inside a Holo aarch64 root with
   network. Run it on a headset that already runs SteamOS (fastest), or in WSL 2 in a chroot of Holo's
   `system.rootfs.zst` through qemu-user binfmt (slow). The installer otherwise takes these three files from the
   `prebuilt` branch.
3. **Download and check everything else** into a work folder (any OS with Python 3):
   ```sh
   python3 installer/prepare.py --workdir W --prebuilt out
   ```
   This fetches Holo core aarch64, Holo's packages (`device/packages.txt`), Valve's arm64 Steam client, the Steam
   Frame's SteamVR 2.19.3 and GTK 2 (rebuilt from Valve's public update store, `frame_source: ota` in
   `installer/sources.json`) and Meta's last Quest 1 OTA (`system`, `modem`). Every file is checked against a pinned
   hash. The `repo` step takes this repository from GitHub (`sources.json`, `ref`). To build your local tree instead,
   replace `W/stage/repo` with it afterwards: empty the folder, then `git archive HEAD | tar -x -C W/stage/repo` (commits only).
4. **Build the image** (an ext4 file system the size of `userdata`, written out as an Android sparse image). There
   are two ways:
   - **On the headset, as the app does.** Nothing is written to the headset's storage; its scratch disk is a sparse
     file on the PC, served over the USB network. Run `fastboot boot out/installer.img`, then on the PC:
     `python3 installer/remote_build.py --stage W/stage --out userdata.simg --scratch-file scratch.img`.
     Without `--stage`, the headset downloads everything itself through the PC.
   - **In WSL 2 / Linux, as root.** This needs `chroot` with qemu-user binfmt for aarch64, GNU tar, zstd and libzstd,
     and e2fsprogs: `python3 installer/build_image.py --stage W/stage --out userdata.simg --scratch DIR`.

   The steps are `format holo setup pkgs frame steam meta native ours finish sparse`. `--only` resumes from a step.
5. **Install.** Back to the bootloader, then exactly what the app does (`app/install.go`):
   ```sh
   fastboot flash boot_a out/boot-native-holo.img && fastboot flash boot_b out/boot-native-holo.img
   fastboot flash userdata userdata.simg
   fastboot erase system_a && fastboot erase system_b
   fastboot set_active a && fastboot reboot
   ```
   `initrd/init` sees a system in `userdata` (`/usr/lib/systemd/systemd`) and boots it as the root, and the swap file
   is made at the first boot. This **wipes Android and all its data**. Going back to stock means the OTA's `boot.img`
   and `system.img` to both slots and `userdata` wiped. The boot chain, firmware, `modem`, `persist`, `private` and
   `vision` are never touched. **For an agent this is the one exception to section 2: it writes partitions, so only
   with the user's explicit go, given for that flash.**

Status (2026-11-20): the build has run in WSL (2026-11-04) and on the headset's micro Linux (the fixes are in the
`Installer:` and `Image build:` commits). The flash itself has not been done on the development headset yet: it
still boots the development layout, `/data/steamos/holo.img` inside Android's `userdata`, by `fastboot boot`.

## 6. Verifying a change

A build that compiles proves nothing here. **Check every change on the headset** before you call it done, with a
measurement, and say what you checked and what you didn't.

- **Logs**: the layer writes one file per process, e.g. `/tmp/quest1-compat-vrcompositor.log` (its `display:`
  lines give vrcompositor's fps); `journalctl -u monado`, `-u steam`; SteamVR's own logs under the `steamos` user's
  Steam directory; `/run/q1-restarts.log`.
- **Without anyone wearing it**, the proximity sensor turns the panels off and Monado stops presenting, so frame
  rates and GPU numbers are meaningless. Turn the panels on with a volatile drop-in,
  `/run/systemd/system/monado.service.d/zz-measure.conf` with `Environment=QUEST1_IGNORE_PROX=1`, restart Monado then
  SteamVR (through the lock), brightness to the minimum, and remove it all afterwards.
- **Frame timing**: the layer's `display:` lines; Monado's `late commit` count and its warped/kept rate (must be 72):
  a picture can be placed right in snapshots and still be shown a vsync late.
- **GPU time per process**: `tools/headset-dev/gputrace.sh` + `vrgpu.sh` (kgsl tracepoints). CPU per thread:
  `thr.py`. Bench of the dashboard: `bench.sh`.
- **Snapshots** of what the panels show: `tools/headset-dev/snap.sh`, `snapcrop.py`. They show the room through the
  passthrough cameras: crop to the region you need on the headset, delete them after, never commit them.
- **Placement tests (stream, HUD, overlays)**: measure in the same snapshot against a reference, never with timing
  alone. `q1marker.cpp` (world- or head-locked overlay on the headset), `pcmarker.py` (its twin drawn by the PC into
  the stream), `fake-motion*.patch` / `replay-head-monado.patch` (a known head motion with the headset still),
  `mjit.py`, `marker-jitter.py`, `stepsnap.py` (pose steps sent to the PC; a picture placed right stays at 0 px),
  `shift.c`. All described in [tools/headset-dev/README.md](tools/headset-dev/README.md). Pitfalls: a region that
  lands on an overlay (the dashboard, a PC overlay) gives 0 px and fakes a fix; animated skies move by themselves;
  compare regions close to each other under pitch and roll.
- **Test-only patches** live in `tools/headset-dev/*.patch`. Apply them to a build tree for the test; never commit
  them applied, and leave no test knobs (flag files, env switches, debug dumps) in committed code.
- What only a person wearing the headset can judge (comfort, trembling, image quality, sound) is checked by them
  last: give them one clear thing to look at, and ask before playing any sound on the headset's speakers.

## 7. How changes are written

- **Find the cause, then fix it once for every case.** A fix for the HUD, the spectator and the stream should be
  one mechanism with one test, not three patches. Do not tune a symptom you can't explain.
- **Measure before and after**, with the same test, ideally A/B on the same boot. Keep image quality: a frame-rate
  gain that costs sharpness or breaks the dashboard is not a gain.
- **Comments say why**, with what was measured and when ("2026-11-16: ..."), so the next person doesn't redo it.
  Remove code made useless by a new version (e.g. SteamVR 2.17-only paths once 2.19 is the target).
- **Docs move with the code**: update the area's page in `docs/` (dated section: what, why, measured how), and keep
  `docs/todo.md` to what is still open.
- **Commits**: small and frequent. Subject line `area: what changed`, then a body that explains the cause, the fix
  and what was checked on the headset (see `git log`). Don't commit build outputs (`.gitignore` lists them).
- The repository is public: everything you commit is published. Re-read section 3 before each commit.

## 8. Where things stand

- SteamVR on the headset is **2.19.3** (the Steam Frame's SteamOS 0.5.9). 2.17.10 still works and is the older
  reference; new work targets 2.19.
- Streaming (Steam Link VR, `vrlink`): read [docs/streaming.md](docs/streaming.md) item by item. The stream's
  trembling has a long history there (picture/pose pairing in vrcompositor's host memory, Monado late commits, PC
  prediction); its tests are in section 6.
- Open work is in [docs/todo.md](docs/todo.md); large areas: memory use, performance (GPU bound), passthrough depth,
  suspend, x86 games (FEX/Proton), the PC installer ([installer/README.md](installer/README.md)).
- The Steam client and SteamVR are Valve's moving targets: after an update, check the hooks the layer and the shims
  rely on (interface versions, vtable slots, memory layouts) before anything else.
