# Desktop mode

## How the Steam Frame does it

- Steam's "+" menu lists the `.desktop` entries Steam can launch. `deckard-nested-desktop.desktop` (package
  `steamdeck-kde-presets`) is the "Desktop" entry: `X-Steam-Special=Desktop`, `Exec=steamos-nested-desktop`.
  The same package ships `NoDisplay=true` copies of developer tools' entries (Qt Designer, Linguist, Assistant,
  qv4l2, qvidcap, qdbusviewer, htop, xterm...) in `/usr/local/share/applications`, which keeps them out of the menu:
  the menu shows Desktop and the apps the user installs (Flatpaks from Discover, `Apps.ListFlatpakApps`).
- `steamos-nested-desktop` makes a nested `XDG_RUNTIME_DIR`, shares PipeWire/Pulse with the main session, shadows
  `kwin_wayland_wrapper` to add `--width 1280 --height 800 --no-lockscreen`, and runs
  `dbus-run-session startplasma-wayland`: kwin_wayland nested inside the gamescope Steam runs in.
- gamescope's OpenVR backend shows each window of an app Steam runs as the SteamVR overlay
  `valve.steam.desktopgame.<appid>` (`--vr-app-overlay-key`), which the dashboard ties to the running app
  (`strGameOverlayKey`). Its overlay options: physical width 2.67 m, control bar with keyboard, click
  stabilization. `valve.plasma.desktopmode` is the overlay of the Frame's other desktop path, a whole Plasma
  session (`plasma-session.target`, `steamvr-nested-desktop.service`), for which the dashboard's
  `bareSessionMode` becomes "desktop".
- Look: `steamos-set-plasma-theme` sets the look-and-feel `com.valve.vapor.deck.desktop` (Vapor colours, Breeze
  Dark icons, the "Steam Deck Logo Default" wallpaper, the Steam Deck logo on the launcher, set by its plasmoid
  setup scripts when Plasma creates a user's desktop).

## On the Quest 1

Everything above is the same, except gamescope:

- gamescope 3.16 needs Vulkan 1.3 with DRM format modifiers, dma-buf import and `VK_EXT_physical_device_drm`; the
  Adreno 540 blob is Vulkan 1.1 without them ("failed to find physical device"). Emulating those would be far more
  code than what gamescope does here.
- So `steamos-nested-desktop` (`device/steamos-nested-desktop`, the Frame's script with "Quest 1" parts) starts an
  Xvfb of kwin's size, kwin_wayland draws its window into it (X11 backend), and `q1desktop` (`src/q1desktop`) plays
  gamescope's part: it creates the overlay with gamescope's key and options, copies the rows XDamage reports as
  changed from Xvfb's framebuffer file into a Vulkan image (only while the overlay is visible), and turns the
  overlay's mouse, scroll and keyboard events into XTest input. Closing the overlay from its control bar ends the
  session.
- One desktop at a time: Plasma takes a while to come up here and the headset reacts slowly meanwhile, so the
  "Desktop" entry gets clicked again and again. A second run of the script used to clean up the first one's
  `XDG_RUNTIME_DIR` (kwin: "Could not create wayland socket", a black overlay); a lock now makes it signal
  `q1desktop` (SIGUSR1: show the dashboard on the desktop's overlay) and exit. Only the script holds the lock
  (its children get `9>&-`), and closing the desktop ends every process of the nested session, found by its
  `XDG_RUNTIME_DIR`: `kcminit` leaves Plasma's process group, outlived the desktop (stuck once kwin is gone)
  and kept the lock, so every later launch showed nothing (user, 2026-11-05).
  `q1desktop` starts with SIGUSR1 ignored (its handler comes after SteamVR's init, seconds later): a click in
  that window used to end it, and the desktop with it. A launch that finds the lock taken but no `q1desktop`
  (the last desktop was just closed and is still ending) waits for the lock (20 s) and opens a new one
  instead of being lost. Timeline of one launch (2026-11-06, Steam's `LaunchNonSteamApp` called directly):
  the overlay is shown, black, after 3 s, KDE's splash spinner is on it at ~5 s, Plasma's desktop at ~14 s.
- Steam passes its own environment on: our Vulkan shim first in `LD_LIBRARY_PATH` and Steam's runtime after it.
  Every Qt program probes Vulkan, and the shim without libhybris' TLS preload crashed them (ksplash, kded6, the
  portals; plasmashell never came up, a black desktop). The script starts Plasma with the system's environment
  (`q1desktop` keeps the Adreno's). Plasma's shell is then up ~10 s after the click.
- The Frame's `steamdeck-kde-presets` is not in Holo's repositories: the installer copies its files out of the Frame
  image (`installer/build_image.py`, frame step, by the package's own file list), except the Frame's
  `steamos-nested-desktop` (ours is in `/usr/local/bin`, first in `PATH`). `device/holo-desktop.sh` sets the
  look-and-feel like `steamos-set-plasma-theme`.

## Known limits

- Memory: with Steam, SteamVR and the tracking running, Plasma and an app push the headset deep into swap; the
  desktop and the dashboard then stall (see the Performance section of docs/todo.md).
- Everything in the desktop is drawn by llvmpipe (no GL driver for the Adreno outside libhybris).
- The keyboard types through the X keymap: characters missing from kwin's layout can't be typed.
- Discover installs Flatpaks system-wide, through flatpak's system helper and polkit. SteamOS's rule allows it to wheel
  members in an active local session; here steamos has no seat session and is not in wheel, so our polkit rule
  (device/holo-frame-mode.sh) grants it Flatpak's actions (except overriding parental controls).
