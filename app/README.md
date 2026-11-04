# Quest1-SteamOS.exe

The Windows app that installs SteamOS on a Quest 1 from start to finish ([installer/README.md](../installer/README.md)
has the whole design). One file, about 20 MB: Go, the window is Edge WebView2 (part of Windows 10/11), Google's
adb and fastboot are embedded.

| Page | What happens |
| --- | --- |
| Welcome | what is needed (Quest 1 on Meta's last build, USB debugging, unlocked bootloader, charge, PC space) |
| Checks | adb: the headset, model (`monterey`), Meta build, battery; the PC's free space; bootloader (`fastboot getvar unlocked`, `userdata` size), then back to Android |
| Warning | the wipe, accepted with a checkbox |
| Install | one progress bar per step: installer files, micro Linux started (`fastboot boot`), scratch space, the headset's downloads (`prepare.py`), the build (`build_image.py`), image copied back, flash, cleanup |

The PC never accepts a connection: the headset downloads through tunnels the app opens to it (`GET /tunnel`
of [`installer/agent.py`](../installer/agent.py)), so there is no firewall prompt and nothing to set up. Its
scratch space is a sparse file here, served through a tunnel as a network block device ([`nbd.go`](nbd.go)):
the headset is only written by the final flash. About 30 GB on the PC while installing, in
`%TEMP%\quest1-steamos\work`, or `X:\TEMP-STEAMOS-QUEST1` on a drive the user picks when the system drive is
too full; deleted at the end (the scratch file as soon as the image is back).

Flashed partitions: `boot_a`, `boot_b`, `userdata`; `system_a`/`system_b` erased; slot `a`. Nothing else.

## Build

```sh
app/build.sh [version]     # Linux/WSL, Go 1.27+: fetches platform-tools, writes app/Quest1-SteamOS.exe
```

A `v*` tag builds and publishes it as a release ([release.yml](../.github/workflows/release.yml)).
`third_party/go-webview2` is a patched copy of the WebView2 binding (see its `PATCHED.md`).

## Not done yet

- Updates without reinstalling (see installer/README.md, "Updates"), back to stock.
- Tested so far: the window and the pages; the install sequence only in parts (the agent and the build in
  WSL, not on a headset).
