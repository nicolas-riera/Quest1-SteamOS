# Steam Frame performance features in Steam's quick menu

What the Frame's Performance tab offers and how each item reaches this headset. Static analysis of the Frame image
0.5.9 (`/usr/share/steamos-manager/devices/steam-frame.toml`, steamos-manager 26.4.1), Steam's `sp.js`, `steamui.so`
and `steamclient.so` (arm64 client of 2026-11), and SteamVR's dashboard script. Tags: [H] shown by a file or string,
[I] inferred.

State on the headset (user, 2026-11-17): the Performance tab shows in the quick menu (it needs the gamescope spoof
below); the profile slider is hidden on purpose (the Quest 1 stays in its performance profile). Not yet checked worn:
the manual GPU clock, the overlay. Motion smoothing shows but does nothing here (not ported).

On the Frame (`ON_FRAME`) the tab shows a "System profile" slider and a Basic/Advanced view toggle. Everything below
is in the **Advanced view**, in three groups:

| Group | Items |
| --- | --- |
| Common | Manual GPU Clock (toggle + slider) |
| Non-VR | Performance Overlay Level (0-4), Frame Limit, Disable Frame Limit, scaling filter, FSR |
| VR | Show Perf Overlay in VR, Record VR Performance, Record Tracking, Refresh Rate, supersample, motion smoothing |

## Manual GPU clock (steamos-manager)

- [H] Steam talks to `com.steampowered.SteamOSManager1` (gdbus object manager client in `steamui.so`). The toggle
  shows when `is_manual_gpu_clock_available` is true, the slider's range is `manual_gpu_clock_min/max` in MHz; it sets
  `GpuPerformanceLevel` ("manual") and `ManualGpuClock` on `GpuPerformanceLevel1`. With no daemon Steam logs
  `SteamOSManager: daemon not present` (it did here).
- [H] On the Frame the daemon is Valve's steamos-manager, configured per device by a toml: a `devfreq` GPU
  (`sysfs_path` of the GPU's devfreq directory, `min_freq = 500`) and three custom performance profiles (cpufreq
  `max` per policy, `gpufreq_limit`), no TDP and no fan entries.
- Here: `device/q1-steamos-manager.py` (installed by `device/holo-frame-perf.sh`) serves the same interfaces on Steam's
  session bus and writes KGSL's `devfreq/min_freq` and `max_freq` (manual = both at the chosen clock, snapped to
  `available_frequencies`; auto = 257 MHz to the top; `low` and `high` pin the ends) and the CPU clusters'
  `scaling_max_freq`. `PerformanceProfile1` is not exported: without it the quick menu shows no profile slider
  (user, 2026-11-17: on the Quest 1 the headset is best left in its performance profile). The daemon tells systemd it
  is ready once its name is on the bus (Type=notify) and steam.service is ordered after it.

## The Performance tab itself: a gamescope spoof

- [H] In VR the quick menu shows its Performance tab only when the web UI's `IN_GAMESCOPE` is true (steamui.js:
  `(!IsAnyVRWindow || IN_GAMESCOPE)`), and steamui.so computes it from one `Plat_IsGamescope()` call (libtier0_s.so:
  `getenv("GAMESCOPE_WAYLAND_DISPLAY")` not empty). Setting that variable for the whole client sends its other ~17
  callers down gamescope paths (Remote Play's capture, Wayland, X atoms).
- `src/gamescopeid` (`libquest1_gamescopeid.so`, preloaded into Steam by `steam.service.d/q1-perfhud.conf`) answers
  yes to that one call site only (`steamui.so:1009604` by default, `GAMESCOPEID_TRUE` overrides, `GAMESCOPEID_LOG`
  lists every caller with its answer) and forwards every other call. The offset moves with Steam client updates:
  when the tab disappears after an update, read the callers from the log. Checked (2026-11-17): only that site
  answers 1; the tab shows, and so does the time zone setting (another `IN_GAMESCOPE` item).

## Performance overlay

- [H] The Non-VR overlay level makes Steam (`systemperfmanager_linux.cpp`) write `<Steam>/config/mangohud.conf`
  (`preset=<level>` when `STEAM_MANGOAPP_PRESETS_SUPPORTED` is set) and run `mangohudctl`; mangoapp is a gamescope
  client (`--mangoapp` in the Frame's `gamescope-session`). It only works when the client's environment has
  `STEAM_USE_MANGOAPP`.
- Here: gamescope cannot run (needs Vulkan 1.3). `src/q1perfhud` (`q1-perfhud.service`) reads the same file and
  draws a head-locked SteamVR overlay: 1 = FPS, 2 = + frame times and reprojection, 3 = + GPU/CPU load, clocks and
  temperatures, 4 = + RAM, battery, dropped frames. `steam.service.d/q1-perfhud.conf` sets the environment.
- [H] "Show Perf Overlay in VR" is SteamVR's own graph (`perfcheck/perfGraphInHMD`, `DrawPerfGraph` in
  vrcompositor); nothing to add.

## Frame limit

- [H] The Non-VR "Frame Limit" belongs to Steam's gamescope controller (X atoms such as `GAMESCOPE_FPS_LIMIT`,
  `is_app_refresh_rate_supported`): not available without gamescope.
- [H] In VR the Frame's equivalent is the Playtime profile: vrserver, vrcompositor and the driver read
  `steamvr/performanceSteamOSManagerCustomProfile`, `powersaveSteamOSManagerCustomProfile` and
  `powersaveResolution`, `powersaveFovScale`, `powersaveFramesToThrottle`, `powersaveRefreshRate`; the dashboard's
  "System profile" slider appears when both names are set. Not set here (no profile slider, see above).
- The "Refresh Rate" slider needs several rates from the driver (`Prop_DisplayAvailableFrameRates_Float_Array`); the
  panels only run at 72 Hz.
