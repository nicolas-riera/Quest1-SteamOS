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


## Notes moved from the to-do list (2026-11-08): Power

History of measurements, fixes and open points as they were written in docs/todo.md (the to-do list now only
lists what is left to do).

- Power button and sleep work (2026-11-02: short press = Steam's sleep, panels off and Meta's
  tracking asleep with its cameras; 1 s press = Steam's power menu). Left (user, 2026-11-03):
  - the sleep animation shows inside the dashboard panel instead of full screen, and it is
    SteamOS's (deck-suspend-animation), the Frame's one if it has its own;
  - real sleep: suspend to RAM works (`/etc/quest1-sleep` deep, opt-in) but on resume Meta's
    tracking was lost (head spinning), the sound gone, the proximity sensor kept waking the headset
    while worn (SyncBoss "wakeup received", irq 271), and the fan and power LED stayed on.
    syncboss_spi.c (Meta's kernel) eats PROX_ON until a PROX_OFF when suspended while worn.
- Steam's own startup movie (each Steam start, also after a SteamVR restart) is SteamOS's: Steam
  picks it from its hardware type (5001 = Frame, read from /proc/device-tree: the Frame's DT says
  qcom,sm8650-deckard), not from -steamframe. The boot animation (q1splash) plays the Frame's, and its last
  frame (the Steam logo) stays on the panels until SteamVR's first one (`/run/quest1-splash-hold`: Monado takes
  the panels over when vrcompositor's layer connects, or blanks them if the headset is not worn; q1splash blanks
  them itself after 60 s). Monado's own frames at its start (black) used to replace it.
- Restart from Steam: retest. If it works, drop the sshd `pam_systemd` workaround in
  `device/holo-perf.sh`.


## Notes moved from the to-do list (2026-11-08): Steam settings

History of measurements, fixes and open points as they were written in docs/todo.md (the to-do list now only
lists what is left to do).

- The timezone dropdown (Settings, next to the 24-hour clock) is hidden: Steam shows it only when
  `IN_GAMESCOPE && IS_STEAMOS` (steamui library.js, export XM). Our Steam runs under Xvfb, not gamescope. The user
  wants it (2026-11-03); meanwhile the system timezone is set with timedatectl (Europe/Paris). Find what sets
  IN_GAMESCOPE and whether it can be set without gamescope's other paths; check IS_STEAMOS too (os-release is now
  ID=steamos, device/holo-identity.sh).
  2026-11-07: `IN_GAMESCOPE=true` is set by steamui.so itself (native), along with its gamescope controller
  (CGamescopeControllerLinux, gamescope-control Wayland protocol): not a flag to set alone.
