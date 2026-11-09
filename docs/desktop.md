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
  instead of being lost. Plasma's own menu lists "Desktop" too: launched from inside the desktop
  (`XDG_RUNTIME_DIR` ends in `nested_plasma`) the script exits at once; it used to start a second desktop inside
  the first (its overlay refused, `VROverlayError_KeyInUse`), which upset the running one (user, 2026-11-07).
- A click in the "+" menu's first ~3 s after it opens was lost (user, 2026-11-07: still several clicks): Steam
  answered `Apps.LaunchNonSteamApp` in 1.9 ms without launching anything while it was loading the entries' icons.
  Each icon load runs a glycin loader in a bubblewrap sandbox, and every one failed ("Can't mount devpts on
  /newroot/dev/pts": the kernel lacked `CONFIG_DEVPTS_MULTIPLE_INSTANCES`, kernel/steamos.config), so the menu
  also had no icons. Timeline of one launch (2026-11-06, Steam's `LaunchNonSteamApp` called directly):
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
- CPU priority (2026-11-09): the desktop, everything started from it, and Discover's installs (flatpak's system
  helper and PackageKit, drop-ins from device/holo-desktop.sh) run at nice 10, and kwin without its real-time main
  thread (`setpriv --no-new-privs` cancels its `cap_sys_nice` file capability). Before, Discover installing a flatpak
  with the system monitor open (load ~18) took vrcompositor down to 50-55 fps, an image jump at each missing frame;
  after, 70-71.8 fps at load 21. The desktop itself may lag instead.
- Flatpak apps and kernel 4.4 (2026-11-09: Chromium installed by Steam's "Install Chromium" did not start, from
  the desktop or from Steam's "+" menu):
  - flatpak gives every app's bwrap `--disable-userns`, which writes `/proc/sys/user/max_user_namespaces` (Linux
    4.9+): "bwrap: cannot open /proc/sys/user/max_user_namespaces". `device/quest1-bwrap` (FLATPAK_BWRAP, set for
    system services, user units and shells by device/holo-desktop.sh) drops that option and its
    `--assert-userns-disabled`, also from the arguments flatpak passes in a file (`--args FD`). Apps may then
    create nested user namespaces, as with older flatpak releases. Steam needs a restart to get the variable.
  - Chromium's own sandbox (zypak, `flatpak-spawn --sandbox`) runs a bwrap inside the app's user namespace, which
    maps only the user: 4.4's devpts refused to create `ptmx` without a root in the namespace ("Can't mount devpts
    on /newroot/dev/pts: Invalid argument"). kernel/devpts-ptmx-owner.patch gives `ptmx` to the mounter then, as
    upstream does.
- System Monitor (and anything else using libksysguard's process list) aborted at start: its GPU plugin sizes a
  vector with `drmGetDevices2()`'s result, an error when `/dev/dri` is missing (no DRM here). device/holo-desktop.sh
  creates an empty `/dev/dri` at boot (tmpfiles.d). Its sensors (CPU, memory...) were all missing: ksystemstats
  read `/proc/cpuinfo` block by block up to an empty line and spun forever on the last one, Qualcomm's "Hardware"
  line with no empty line after it; kernel/cpuinfo-blank-line.patch adds one.
  Its GPU tile stayed empty: ksystemstats' GPU plugin only finds DRM GPUs of AMD, Intel or NVIDIA. src/ksysgpu is a
  plugin with the same id and the same sensor names (`gpu/all/usage`, `gpu/gpu0/usage`, `coreFrequency`,
  `temperature`, `usedVram`) read from kgsl's sysfs (busy %, clock, temperature, `page_alloc` as used memory, the
  RAM as total since it is shared). It lives in `/usr/local/lib/qt6/plugins` and ksystemstats gets that path first
  (`QT_PLUGIN_PATH`: a drop-in of `plasma-ksystemstats.service`, and steamos-nested-desktop for the desktop's own
  bus); KDE loads the first plugin of an id, so KDE's files stay as they are.
- Apps from Steam's "+" menu start on Steam's own X display (`:1`, Xvfb), which nobody sees: Steam launches them
  as they are, and on the Frame gamescope shows each one as an overlay. That display is also what Remote Play
  streams from the headset; on the Frame it shows the VR view. Steam also had no session bus
  (`DBUS_SESSION_BUS_ADDRESS`), so Flatpak apps it launched failed ("Failed to get portal proxy"): steam.service now
  sets it (device/holo-steam-session.sh), and user units get `DISPLAY=:1` (environment.d) for the portals.

## Apps from Steam's "+": q1apps

`q1apps` (src/q1desktop/q1apps.cpp, quest1-apps.service, started with SteamVR) does for `:1` what gamescope does on
the Frame: every app gets its own SteamVR dashboard overlay (tab with the app's name and icon), side by side in the
dashboard like on the Frame.

- A black, override-redirect cover window sits on top of `:1` with an empty input shape and the X cursor hidden: the
  streamed screen shows no app windows and no Linux cursor, and clicks still reach the windows under it.
- App windows are grouped by X client (one app = one client). Each window is redirected on its own
  (`XCompositeRedirectWindow`); the root's subwindows are not, because that changed the pixmaps of Steam's own
  windows and Steam's VR UI lost the dashboard bar. Steam's windows (WM_CLASS steam/steamwebhelper, or the PID's
  name) are never touched.
- XDamage marks what changed; the app's windows are composed into a screen-sized canvas (XShm) and uploaded to the
  overlay texture (Vulkan, shared code with q1desktop in src/q1desktop/q1overlay.h), only while the overlay is
  shown and at most at `Q1A_HZ` (default 60). Main windows fill the screen, dialogs are centred.
- Laser input goes through XTest: pointing at an overlay raises its app (then the cover again) and gives it the
  focus. Closing the tab sends `WM_DELETE_WINDOW`, then kills the client 5 s later.
- The apps themselves draw with llvmpipe (no GPU in X), so they are slow; SteamVR keeps its frame rate (q1apps runs
  at Nice 5 and only uploads changed rows).

### Remote Play spectator (the VR view in the stream)

On the Frame, a Remote Play stream from the headset shows what the wearer sees. Here the streamed screen is `:1`, so
during a session q1apps paints the VR view on its cover window:

- Session detection: Steam's stream threads (`CGameStreamVide...`) exist only while a session runs; q1apps looks for
  them once a second. Nothing else runs outside a session (user's rule: active only when used, no cost in the
  headset).
- While a session runs q1apps creates `/tmp/quest1-spectator`; Monado's warp (comp_window_mdp.c, `spect_tick`)
  then blits the left eye's undistorted image (scene mode), cropped to 16:10 around the view axis, into a 1280x720
  image every second vsync (36 a second; "30 a second" checked once a vsync gave 24), copies it to a host buffer
  (cached memory, ~6 ms a copy; the write-combined type took ~28 ms) and a thread of its own writes it to
  `/dev/shm/quest1-spectator` (two frames + a header with a sequence number). Monado frees all of it 5 s after the
  file goes away. The copy time is logged every 10 s ("spectator: ... frames/s, copy ... ms each").
- q1apps maps that file and hands it to the X server with MIT-SHM (`xcb_shm_attach_fd`): each new frame is one
  `XShmPutImage` on the cover, no copy on our side. The stream itself does not go through X: Steam captures through
  PipeWire and q1screencast reads that file directly (docs/streaming.md "The capture"); it goes back to the X screen
  after 0.5 s without a new VR frame, so the dark blue cover shows.
- The cover's cursor and the root's are an empty pixmap cursor: Remote Play draws the X cursor into the stream
  (XFixes' cursor image, hidden or not). The cover takes the pointer 1 s after the last laser input on an app.
- No VR frame for 0.5 s (the headset taken off and asleep: Monado renders nothing) turns the cover dark blue
  (#1b2838), as the Frame's stream does.
- Xvfb runs at 1280x720, the Frame's gamescope size (it was 1280x800).
- The PC only watches (user, 2026-11-09: its mouse moved and clicked the apps under the laser, its keyboard typed in
  them, and the apps' black cursor showed on the PC). `src/rpspectator` is preloaded into the Steam client
  (steam-q1) and, while `/tmp/quest1-spectator` exists:
  - drops Steam's injection of the PC's mouse and keyboard (`XTestFake{Key,Button,Motion,RelativeMotion}Event`,
    imported by steamui.so and steamclient.so); releases still pass, so nothing stays held;
  - returns the cursor image (`XFixesGetCursorImage`) fully transparent;
  - keeps the PC's gamepads (the stream forwards them, "Remote Device Found" in logs/controller.txt) from getting a
    virtual gamepad on the headset, and only them (user: gamepads plugged into the headset must keep working).
    Steam Input makes one uinput device per controller it drives; a gamepad of the headset's is an input device
    with BTN_GAMEPAD before Steam wraps it, the PC's are not. So `/dev/uinput` opens only while the headset has
    more gamepads of its own (Steam's 28de:11ff ones left out) than Steam holds uinput devices; each decision is
    logged ("quest1_rpspectator: Remote Play session: ..."). The PC's controllers come with the session's start, up
    to a second before q1apps makes the trigger, so here a session also counts from the moment Steam's
    `CGameStreamVide...` thread exists. Steam has no setting to stop the forwarding itself.

  The "Microsoft X-Box 360 pad" (045e:028e) seen on the headset is xscreen's (the Touch controllers in flat mode)
  and its Steam Input wrapper "Microsoft X-Box 360 pad 0" (28de:11ff) is made when Steam starts, not by the stream.
- SteamVR's own headset view (`system.HeadsetView`, `IVRHeadsetView`) was tried first: vrcompositor renders it in
  two 256x256 passes, ignores `SetHeadsetViewSize` and the image is black here, and Steam's own VR capture
  (`CVRStreamThread`, GL) only runs on the Frame's hardware type and needs GL interop that llvmpipe lacks.


## Notes moved from the to-do list (2026-11-08): Steam and desktop

History of measurements, fixes and open points as they were written in docs/todo.md (the to-do list now only
lists what is left to do).

- The "+" menu's Desktop entry took several clicks (user, 2026-11-06). Found: a click during the first seconds of a
  launch signalled `q1desktop` before it had its SIGUSR1 handler and ended the desktop; a click while the last desktop
  was ending was dropped. Both fixed (docs/desktop.md); a click on the entry (DOM events) launches it once, and four
  launches 1 s apart give one desktop. Left: confirm with the laser in the headset (user).
- 2026-11-07, still several clicks with the laser (user): a click in the menu's first ~3 s was dropped by Steam while
  its icon loaders (bubblewrap) failed one after the other; the kernel now has multi-instance devpts. To confirm in
  the headset (icons in the "+" menu, one click). "Desktop" from Plasma's own menu no longer starts a nested desktop.
- Discover showed no apps and asked for flatpak (user, 2026-11-06): polkit refused Flatpak's system actions to steamos
  (SteamOS's rule wants wheel and an active seat session), so the Flathub catalogue was never deployed ("DeployAppstream
  not allowed"). Fixed in holo-frame-mode.sh's polkit rule; Discover lists Flathub's apps (checked on the desktop's
  screen). Left: install an app from it in the headset (user).
- Desktop mode launcher (user, 2026-11-05 night: failed to start again): kcminit outlived the desktop and held
  the launcher's lock; fixed 2026-11-06 (docs/desktop.md), 9 launch/close cycles and a double launch OK from a
  shell with Steam's environment. Left: confirm from Steam's "+" menu in the headset; once, the first launch
  after killing old leftovers hung 48 s (plasma_session up, kwin silent) and closed fine.
- Desktop mode (docs/desktop.md): the nested Plasma runs and shows in the dashboard (user, 2026-11-05: Dolphin
  started, the headset struggling). Open:
  - lag, and the whole dashboard freezing while the desktop is used (memory: ~2 GB swapped with Plasma and
    an app; llvmpipe threads competing with vrcompositor and the tracking);
  - start it from Steam's "+" menu (the Frame's entry is installed) and check the overlay key the dashboard
    ties to the running app (`valve.steam.desktopgame.<appid>`), the control bar's keyboard and close;
  - check the Vapor look (wallpaper, dark theme, Steam Deck logo) on a fresh user, and that the "+" menu shows
    only Desktop and user-installed apps;
  - mouse orientation, scroll and keyboard typing in the headset;
  - snapshots (/root/snap.sh) show q1desktop's overlay turned by 180 degrees while it is right in the headset
    (user, 2026-11-05; Steam's overlays are right in both): don't trust snapshots for that overlay's
    orientation, find why they differ.
- Steam crashed while the desktop was used (user, 2026-11-05 ~18:07) and its VR UI did not come back:
  vrdashboard asserts "stalled cross-thread pipe (pipe is disconnected)" (/tmp/dumps), the headset deep in swap.
  The dashboard's vrwebhelper (started by vrdashboard) died with it and vrdashboard never restarted it. The
  next OpenVR client to start (here q1desktop) started a new one in its own cgroup, so stopping that client
  killed the dashboard again. Fixed 2026-11-07: quest1-frame-dashboard runs a watcher that ends a vrdashboard left
  without vrwebhelper; vrserver starts a new one (tested: all vrwebhelpers killed, the dashboard back in ~35 s).
