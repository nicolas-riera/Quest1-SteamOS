# Monado for the native Quest 1

`quest1-monado.patch` applies to Monado commit `cfa6078b8d7f368fa5296f540a436d59833e496c`
(`git apply quest1-monado.patch`). Build on the headset (Holo, -j2) with libhybris in /opt/hybris.

- `drivers/quest1`: 3DoF HMD from the SyncBoss IMU (Meta's libsyncboss.so through libhybris),
  Meta's Quest 1 lens distortion (CatmullRom10, docs/quest1-lens.md), views rotated 180°.
- `compositor/main/comp_window_mdp.c`: zero-copy target, gralloc buffers scanned out with
  MSMFB_ATOMIC_COMMIT, vsync pacing, panels/GPU governor switched with activity.
- `XRT_HYBRIS_AHB`: AHardwareBuffer swapchains through libhybris (needs the Android
  hwservicemanager, graphics allocator and configstore daemons: device/holo-android-hals.sh).
- `qbridge`: older experiment (Monado frames handed to Meta's runtime under Android), unused natively.

Install: device/holo-monado.sh. Apps: `xr-run <app>`.
