# Steam Frame identity (Steam client)

How the arm64 Steam client decides it runs on a Steam Frame (found by static analysis of the 2026-10 client):

- `Plat_IsSteamFrame_DoNotUse()` (libtier0_s.so) is just `CommandLineParamExists("-steamframe") || CommandLineParamExists("-deckard")`.
  No DMI, os-release or file is involved (the DMI reads in steamui.so are hardware-survey telemetry; the Quest kernel has no DMI anyway).
- steamui passes it to the web UI as `ON_FRAME=true` (webhelper_js.txt, "Storing new config params"). The OOBE picks its variant with
  `ON_FRAME ? Frame : (device == Steam Machine ? Machine : generic)`, so without the flag the Machine/generic first-boot
  screen ("plug the controller into the box") appears; with it the Frame OOBE (Frame graphics, no QR code, Continue enabled once both
  controllers are connected) and the Frame UI paths are used.
- `-deckard` is equivalent for this predicate; `-steamframe` is the one we use.

## What is installed

`steam.service` drop-in `/etc/systemd/system/steam.service.d/vr.conf` runs
`steam-q1 -gamepadui -steamos3 -steamdeck -steamframe` (also the default ExecStart in device/holo-steam-session.sh).
Revert: put the old args back (`/etc/systemd/system/steam.service.d/vr.conf.bak-frameid` is the pre-change copy), `systemctl daemon-reload`, restart steam.

## Caveats found

- With the flag, Steam's desktop (Xvfb) window stays black (the Frame has none) and, in our test, the SteamVR dashboard showed no Steam
  panel (environment only) when checked 2.5 min after the restart; baseline without the flag showed the Steam library panel.
  The user reported Steam is recognised as a Frame; the dashboard panel state under the flag was not conclusively settled.
- `src/frameid/` is an optional LD_PRELOAD shim (libframeid.so, build.sh with aarch64-linux-gnu-gcc) that overrides the predicate per
  call site (module:offset, env FRAMEID_FALSE / FRAMEID_FILE / FRAMEID_LOG) to separate "Frame UI" from other Frame-only code paths.
  Call sites seen: 13 in steamclient.so, 10 in steamui.so, 1 in chromehtml.so (offsets are for the 2026-10-21 build). Not required by default.
- The log line "IClientVR::BServeVRGamepadUIOverlay took too long" appears only when the call is slow: not a reliable signal.
  Check the dashboard instead: `runuser -u steamos -- touch /tmp/quest1-snap` writes /tmp/quest1-snap.ppm.

## Touch controllers as a gamepad (q1pad)

Steam's VR UI (the dashboard "+" menu's Desktop entry, probably other gamepad-UI widgets) ignores
laser-pointer clicks unless Steam Input sees a gamepad (observed with a Bluetooth DualSense). On the
Frame the controllers are a Steam Input device; ours are only visible to SteamVR (through
driver_quest1), so `src/q1pad` (unit `q1pad.service`, started with `android-q1trackd`) republishes
q1trackd's shared memory (`q1track_shm.h`) as a uinput "Microsoft X-Box 360 pad" (045e:028e, which
Steam maps without configuration). Mapping: right A/B -> South/East, left X/Y -> West/North, sticks,
index triggers -> LT/RT, grips -> LB/RB (click at half squeeze), stick clicks -> L3/R3, left menu ->
Start; the Oculus button is not mapped. It runs as `steamos` (owner of `/dev/uinput`), reads at
125 Hz and releases everything if q1trackd stalls. SteamVR's laser keeps working: it reads the
controllers through its own driver.

What Steam's UI does on the Frame (steamui JS): localisation key `GuidedTour_Frame_LaserMouseToggleTitle`
("Laser Mouse & Gamepad Mode Toggle"), dashboard actions `#VRDashboard_ShowLaserMouse` /
`#VRDashboard_HideLaserMouse` bound to the REAR_LEFT_LOWER / REAR_RIGHT_LOWER buttons, i.e. the Frame's
grip buttons toggle laser-mouse versus gamepad mode. Our pad has no such rear buttons; the grips are
mapped to the shoulders instead.
