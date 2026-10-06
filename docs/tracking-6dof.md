# 6DoF head and controller tracking under native Holo: feasibility study

Offline study, 2026-10-06. The headset was not reachable, so nothing here has been run on it.
Sources:
* Quest 1 system dump in WSL `~/q1/sys` (build 49845030443200410).
* Kernel tree `~/q1/kernel`.
* Local backup `backup/20261005/sda_0-userdata.bin`. Partition sda8 (`private`) is the real `/persist`.
* Monado main at cfa6078 (the same base as our patch), Basalt for Monado, and thaytan's Monado fork, cloned in WSL `~/src/{monado,basalt,openhmd}`.

Scratch scripts are in `%TEMP%\claude\...\scratchpad\t6\`; symbol dumps are in `~/q1/re/dbg`. This study builds on
[tracking-ipc.md](tracking-ipc.md) (libossdk client API and focus gate) and [syncboss-imu.md](syncboss-imu.md).

**[V]** = verified in binaries, source or data. **[I]** = inferred, needs a live check.

---

## 0. Recommendation

| | Path M: Meta stack, native processes plus binder stubs | Path O: open source, Monado SLAM and constellation |
|---|---|---|
| Head 6DoF quality | Meta's production SLAM | Basalt; quality and CPU load on the SD835 unknown |
| Controllers 6DoF | Yes, Meta constellation | Needs an out-of-tree port (thaytan's Rift S code) plus RE of the controller radio stream |
| Hands | Yes (in trackingservice) | No |
| Main unknowns | Do the stubs satisfy every startup path? Does the power or TE gate stop the cameras? | Camera capture API (prototypes [I]), timestamp sync, Basalt performance |
| New code | One small bionic stub daemon, rc-to-systemd units, a libossdk client in the Monado driver | Frame grabber, Monado driver for SLAM and constellation, controller input decoding |
| Effort (rough) | 1 to 2 weeks to the first head pose | Months for head plus controllers |
| Proprietary blobs at runtime | Many (the whole tracking stack) | libsyncboss and libqcamera* only (the capture path) |

**Recommendation: do Path M first.** It is the only path that gives head, controllers and hands
at Meta quality in a reasonable time. Its system_server dependencies are narrow. trackingservice
needs only five Java-hosted binder services, and each is used through one or two transactions
that a generic stub can answer. There is no need to run Android's system_server or zygote.

**Keep Path O as the long-term open option.** Do its cheap first step, a native 4-camera frame
grabber, early: it is also the best debugging tool for Path M. The offline study made Path O much
more realistic than expected:
* The factory camera and IMU calibration is plain JSON in `/persist/calibration`. It has the same
  schema as the Rift S JSON that Monado's `rift_s` driver already parses [V].
* Camera capture goes through small C APIs (`libsyncboss` `syncboss_camera_*` and
  `libqcameraoculushal` `qcamera_*`) that we can call through libhybris [V for the exports, I for
  the prototypes].

---

## 1. Path M: minimal process set

### 1.1 Who needs what (binder names from xrefs of `getService`/`BinderClient` call sites, HIDL from imports)

| Process (run as) | Hosts | Binder clients it uses (on `/dev/binder`) | HIDL (hwbinder) | Blocking behaviour if a dependency is missing |
|---|---|---|---|---|
| `vendor.oculus.hardware.sensors@1.0-service` (system) | HIDL `ISensors 2.0`, `ICameraProvider`, `IControllerProvider`, `IImu`, `IMag`, `sensors_java IPowerstate` | none [V] | registers only | Needs hwservicemanager. Opens `/dev/syncboss*` and the cameras (libcamerahal → libqcamerahal → libqcameraoculushal → libqcameradriver). Reads `/persist/calibration/*.json`. Drops IMU samples without display TE. |
| `vendor.oculus.hardware.sensors@1.0-iad` | `IIad` | — | — | Must run: `IIad` is declared in the vendor VINTF manifest, so `getService` waits forever without it [V manifest, I wait]. |
| `cameramuxmodeservice` (system) | `cameramuxmodeservice` | none [V] | `ICameraProvider` | Self-contained. Reads `/system/etc/cameramuxmode/config.json`. |
| `calibration_svr` (system) | `HMDCalibration` | `permission` (only when a caller asks for calibration) [V] | sensors | Needs `/persist/calibration{,/online,/online.tmp}` to be writable. |
| `settingsserver` (system) | `PreferencesService` | `PreferencesAppInfoInquirer` (Java, lazy `ensureConnection`) [V] | — | Needs SQLite DBs under `/data/oculus/preferences`; fresh ones get default values. |
| `vrfocusserver` (**root**) | `vrfocus` | `activity`, `package_native`, `permission`, `vrfocushelper`, `OculusWindowManager`, all lazy, logs "Couldn't get ..." [V] | — | Not fatal. Each missing service costs the libbinder `getService` ~5 s retry on every lazy init [I], so stub them. The uid 0 focus bypass does not need them [V, tracking-ipc §2.3]. |
| `trackingservice` (system) | `tracking`, `TrackingService`, `HandTrackingService`, `TrackedObjectService`, `TrackingEnvironment`, `TrackingFidelityService`, `InputSettings`, `TrackingDataInjection` | **`permission`** (asserts when a client attaches) [V]; **`vrpowermanager`** (power state) [V]; **`appops`** [V]; `settingsproxy` (`getCurrentUserId`, user-switch listener) [V]; `OVRRemoteService` (controller status push) [V]; `power` (wake locks, libwakelock) [V]; `GatekeeperService` (feature flags) [V]; `notificationProxy` (OTA toasts) [V]; `cm_wifi` (pairing validation only) [V]; `cameramuxmodeservice`; `vrfocus` (via libossdk `createVrFocusManager`) [V] | `sensors_java IPowerstate`, all sensors interfaces through libvrsensors-hidlwrapper, `android.hardware.power@1.0 IPower` (libtrackingutils) [V] | `BinderClient<T>(name, timeout)` blocks on a future until the first connect attempt succeeds or times out [V], so the waits are bounded. **`IPower` is in the VINTF manifest**: run `android.hardware.power@1.3-service-libperfmgr` or expect a hang [I]. |
| system_server (Java) | `permission`, `appops`, `vrpowermanager`, `settingsproxy`, `OVRRemoteService`, `power`, `activity`, `package_native`, `GatekeeperService`, `notificationProxy`, `PreferencesAppInfoInquirer`, `OculusWindowManager`, `vrfocushelper` | | | **Replace with stubs (§1.3).** |

Already running natively today: servicemanager, hwservicemanager, vndservicemanager, the gralloc
allocator, configstore and the property area.

### 1.2 Stubs: what each must answer

All of them can live in **one bionic daemon** (`ovrstub`). It links `/system/lib64/libbinder.so`,
registers each name with `defaultServiceManager()->addService()`, and uses one generic `BBinder`
whose reply is `writeNoException()` followed by zeros. A zero reply is already right for most calls:

| Service | Interface descriptor | Calls that matter | Zero reply OK? | Override |
|---|---|---|---|---|
| `permission` | `android.os.IPermissionController` (hand-written, libbinder) | `getPackagesForUid` (code 3), `checkPermission` (1) | yes for 3 (empty list → trackingservice falls back to `/proc/<pid>/cmdline` [V]) | `checkPermission` → 1 (only for anchor/eye APIs) |
| `appops` | `com.android.internal.app.IAppOpsService` | `startOperation`/`finishOperation` | yes (`MODE_ALLOWED` = 0; the result is ignored anyway [V]) | — |
| **`vrpowermanager`** | `oculus.internal.power.IVrPowerManager` | `registerClient(IVrPowerManagerClient)` = **code 1**, `unregisterClient` = 2, `notifyDevice{AlmostIdle,Idle,NotIdle}` = 3/4/5, `acquire/releaseMountWakeLock(IBinder)` = 6/7 [V, `BpVrPowerManager`] | no | On `registerClient`, call the client binder with **code 1 `onStateChange(int)`** (descriptor `oculus.internal.power.IVrPowerManagerClient`) and the value for RUNNING. libsensorclientutils maps `PowerState = 3 - state` for states 1..3 [V], so RUNNING is most likely **3** [I]. To simulate unmount, send the sleep value. |
| `settingsproxy` | `oculus.internal.ISettingsProxy` | `getCurrentUserId` → 0, `registerUserSwitchListener` | yes | — |
| `OVRRemoteService` | `oculus.internal.remote.IRemoteService` | status/powerstate callbacks, `setOtaFailed` | yes [I] | — |
| `power` | `android.os.IPowerManager` | `acquireWakeLock`/`releaseWakeLock` | yes | — |
| `GatekeeperService`, `notificationProxy`, `activity`, `package_native`, `vrfocushelper`, `OculusWindowManager`, `PreferencesAppInfoInquirer`, `cm_wifi` | various | queries | yes [I] | Watch logcat for asserts. |

Notes:
* AIDL transaction codes are `FIRST_CALL_TRANSACTION + index`; verify each against the `Bp*`
  proxy in the client library the same way as for `vrpowermanager`.
* Interface-token checking: Android 10 `Parcel::enforceInterface` is server-side, so the stub can
  ignore it.
* There is **no standalone or debug mode** that removes these dependencies [V]. The only
  properties in trackingservice and libsensorclientutils are controller-tuning knobs
  (`persist.ovr.tracking.*`), `persist.ovr.skipctrlfwupdate`, `persist.ovr.ignorectrlcalfail`,
  `persist.trackingservice.*` SLAM feature flags (libtrackingengines) and
  `persist.vendor.ovr.camera.raw_image_mode` (sensors HAL). The only command-line flags are
  `--json`/`--pretty` for dump. vrfocus permissive mode (`ro.debuggable` +
  `debug.ovr.vrfocus.permissive`) is unnecessary for a root client.

### 1.3 Why not run the real system_server headless

* system_server needs zygote and ART boot images; WindowManager/DisplayManager wait for
  SurfaceFlinger.
* SurfaceFlinger needs the HWC HAL (`composer@2.1-service.monterey`). That HAL would take MDSS
  away from Monado's `comp_window_mdp`.
* PackageManager wants Android's FBE-encrypted `/data`, which we cannot unlock natively (no
  vold/keymaster).

That is a full Android boot in a container, which is the B2 plan, with a display conflict on top.
The stub approach avoids all of it. If a stub turns out insufficient, fall back to the LXC container
with Android's own system_server and make SurfaceFlinger use a dummy display; that is costly.

### 1.4 Other runtime requirements

| Need | How, natively |
|---|---|
| `/persist` | Mount **sda8** (GPT label `private`; Android mounts it on `/persist`). sda2, labelled `persist`, holds only `sensors/` and `speccfg/` [V backup]. calibration_svr writes `online/`. Bind a writable copy or overlay, never the raw partition rw at first. The image needs journal recovery: mount with `ro,noload`, then copy. |
| `/data/oculus/preferences`, `/data/misc/tracking`, `/data/vendor/misc/sensors/controllercal` | Fresh dirs on Holo storage, bind-mounted. The Android ones are FBE-encrypted. Preferences start at defaults (`hand_tracking_enabled` false: set it with `oculuspreferences --setc`). |
| Display TE | `te_timestamp` is updated by a GPIO IRQ (`timesync_irq_handler` in syncboss_spi.c) [V]. The panels must be producing TE, which is true while `comp_window_mdp` presents. **When the panels are blanked (prox off), the IMU is dropped and tracking stalls** [I]. That is acceptable, since that is also when nobody is looking. |
| Proximity | The sensors HAL serves `IPowerstate` from `/dev/syncboss_powerstate0`. The kernel `miscfifo` supports several readers [V], so Monado can keep reading prox. |
| `/dev/syncboss*` ownership | The kernel counts streaming clients [V], but two libsyncboss instances sending commands at once is risky [I]. **With Path M, Monado must stop using libsyncboss directly** and take the head pose (and IMU-quality data) from libossdk. |
| Firmware updates | The sensors HAL and trackingservice can flash SyncBoss and controller firmware (`/vendor/firmware/{jedi,lcon,ruby}_archive.bin`). Same image as Android, so no update is expected [I]. Still put `persist.ovr.skipctrlfwupdate=1` in the property area. |
| Properties | Add the `persist.ovr.*` and `ro.*` keys trackingservice reads to `tools/mkproparea.py` input (empty is fine). |
| Client | Monado driver → libossdk via libhybris `android_dlopen` (same pattern as libsyncboss and gralloc). Alternatively, a bionic root helper exporting poses over shm. Run as uid 0 (focus bypass). Start the binder thread pool. `createHeadTracker(8)` etc. as in tracking-ipc §2.4. Avoid libossdk entry points that call `vendor.oculus.hardware.graphics.composer@1.1::IComposer::getService` (in the manifest, so it would block without HWC) [V import, I which calls]. |

### 1.5 Process start order (systemd units mirroring the rc files)

1. **Existing services:** servicemanager, hwservicemanager, vndservicemanager, allocator, configstore.
2. **`ovrstub`:** registers all the stubs from §1.2.
3. **HALs:** `power@1.3-service-libperfmgr`, the sensors HAL, and the IAD HAL.
4. **Native daemons:** settingsserver, calibration_svr, cameramuxmodeservice, and vrfocusserver (root).
5. **trackingservice:** run as uid/gid 1000, groups system/uhid/inet, with `SYS_NICE`.
6. **Client:** Monado (or `trackinginterface_cli`) as root.

Pin trackingservice to CPUs 0-3 if jitter shows up.

---

## 2. Path O: open-source pipeline

### 2.1 Camera hardware and kernel path [V]

* **4× OV7251**: 640×480, global shutter, monochrome. From `CameraCalibration[].SensorType` in
  `/persist/calibration/camera_calibration_v2.json`.
* **Device tree**: `arch/arm64/boot/dts/oculus/vs1-camera.dtsi`.
  * Nodes `oculus,camera@0..3` under `&cci`, with MCLK0-3 at 24 MHz and PWDN on GPIO 137-140.
  * CSIPHY 0/1/2/2 and CSID 0/1/2/3. Cameras 2 and 3 share CSIPHY 2.
  * CCI masters 0/0/1/1.
* **Kernel driver**: `drivers/staging/oculus/mcu/syncboss/syncboss_camera.c` (compatible
  `oculus,camera`) does **power only**: regulators, clocks, pinctrl. The cameras power up when
  userspace sends SyncBoss message **0x28 CAMERA_PROBE** and power down on **0x29 CAMERA_RELEASE**.
  The kernel snoops these in `syncboss_snoop_write_locked`.
* **Sensor programming and sync**: the **SyncBoss MCU** handles sensor registers, exposure/gain,
  frame rate, frame tagging and the controller-LED sync. libsyncboss exports
  `syncboss_camera_{probe,release,init,start_streaming,stop_streaming,set_frame_rate,set_bpp,set_exposure_gain,set_exposure_gain_tag,set_controller_exposure_gain_tag,set_frame_tag_mode,params,register_read,register_write,serial_num,temperature,decode_metadata,testpattern}`.
  Command IDs seen: init 0x2e, start 0x2c, stop 0x2d, frame rate 0x89 (also sets the controller
  LED period through `pulsar_mgr_set_led_period_us`), bpp 0x8c, tag mode 0x8d, params 0x4a.
* **Frame receive**: plain Qualcomm `camera_v2` (CONFIG_MSMB_CAMERA). The path is CSIPHY → CSID →
  ISPIF → VFE (msm_isp), with buffers in ION/gralloc and frames dequeued from the ISP stream.
  **Not plain V4L2 capture**: `/dev/videoN` is only the msm session node.
  `libqcameradriver.so` (62 KB, a "mini-driver"; no mm-qcamera-daemon) issues
  `VIDIOC_MSM_CSIPHY_IO_CFG`, `VIDIOC_MSM_CSID_IO_CFG`, `VIDIOC_MSM_ISPIF_CFG`,
  `VIDIOC_MSM_ISP_{REQUEST_STREAM,CFG_STREAM,REQUEST_BUF,ENQUEUE_BUF,...}` and media-controller
  enumeration (`/dev/media%d`).

### 2.2 Userspace capture stack (reusable through libhybris) [V exports, I prototypes]

```
sensors HAL CameraProvider ─ libcamerahal (CameraHalC_Init/Start/Stop/Deinit, CreateCameraHal)
                               └ libqcamerahal (C++ QCamera, GraphicBuffer-backed buffers)
                                    └ libqcameraoculushal  ← small C API, deps: liblog, libqcameradriver
                                         └ libqcameradriver (CSI/ISP ioctls)
      + libsyncboss syncboss_camera_* (power, sensor config, exposure, tags)
      + syncboss stream records: camera shutter events (process_camera_shutter[_v2])
```

`libqcameraoculushal` API, from the call sites in libqcamerahal [I]:

| Function | Shape |
|---|---|
| `qcamera_open` | `hal = qcamera_open(int mode)`; mode is 0 or 1 depending on the pixel format |
| `qcamera_num_sensors` | `qcamera_num_sensors(hal)` |
| `qcamera_get_sensor` | `s = qcamera_get_sensor(hal, idx)` |
| `qcamera_query_sensor_info` | `qcamera_query_sensor_info(s, &info)` |
| `qcamera_query_buffer_dimensions` | `qcamera_query_buffer_dimensions(fmt, 1, &w, &h)` |
| `qcamera_start_sensor` | `qcamera_start_sensor(s, &a, &b, nbufs, bufs, 1)` |
| `qcamera_dequeue` | `buf = qcamera_dequeue(s, timeout_ms=33)` |
| `qcamera_enqueue` | `qcamera_enqueue(s, buf)` |

Also exported: `qcamera_start_all_sensors` ("starting all sensors with I2C broadcast", the
synchronized start), `qcamera_stop_*`, `qcamera_get_fd` (pollable), `qcamera_read_temperature`.
`libqcameraoculushal` maps buffers itself via `/dev/ion`. libqcamerahal allocates through
gralloc, which already runs natively.

**So a glibc program can grab frames** from all four cameras, the same way `src/sbimu` reads the
IMU:
1. `syncboss_init`, `syncboss_camera_probe`, `syncboss_camera_init`.
2. Set frame rate, bpp and exposure.
3. `qcamera_open`, then start all sensors.
4. `syncboss_camera_start_streaming`.
5. Dequeue loop.

Remaining RE (offline, 1 to 3 days): the `info`/buffer structs and the `qcamera_start_sensor`
arguments, from `libqcamerahal` `QCamera::Initialize/StartCamera/StreamingThread` and
`libqcameraoculushal` `get_bufs`/`extract_sensor_info`; the exact syncboss camera call order,
from the sensors HAL `CameraProvider::setupCameras/setupStreamInternal`; and the shutter-record
and frame-metadata layouts (`syncboss_camera_decode_metadata`). The unknown ~30 Hz stream record
type 14 seen by sbimu is a likely candidate for camera shutter or vsync events [I].

Frame scheduling: the cameras interleave **HEADSET** (SLAM exposure) and **CONTROLLER** (short
exposure for LEDs) frames ("default" mux mode, `config.json`) [V]. The HAND type replaces
CONTROLLER in hand mode. Frame tags select per-type exposure/gain
(`set_exposure_gain_tag` / `set_controller_exposure_gain_tag`).

### 2.3 Calibration data on the device [V]

`/persist/calibration/` (sda8):

| File | Content |
|---|---|
| `camera_calibration_v2.json` | Factory calibration for the 4 cameras. `Id`, `SensorType`, `ImageSize`, `DeviceFromCamera` (4×4 row-major), `Projection{Model:"PinholeSymmetric", Coefficients:[f,cx,cy]}`, `Distortion{Model:"Fisheye62", Coefficients:[k1..k6,p1,p2]}`. |
| `camera_calibration.json` | Same data as a legacy flat array. |
| `imu_calibration.json` | `DeviceFromImu`; `Gyroscope{RectificationMatrix, Offset{ConstantOffset}}`; `Accelerometer{RectificationMatrix, Offset{OffsetAtZeroDegC, OffsetTemperatureCoefficient}}`. |
| `mag_calibration.json` | Magnetometer calibration. |
| `online/<id>` | Online-refined camera calibration, same v2 schema, `Source:"Online"`, `AlgorithmVersion:3`, dated 2026-08. |
| `display/{left,right}` | Display calibration. |

* **This is exactly the Rift S calibration schema** that Monado's `rift_s_firmware.c` parses
  (same key names) [V]. `rift_s_util.cpp` `rift_s_get_cam_calib` already fits KB4 to Fisheye62
  for Basalt [V].
* The gyro and accel rectification matrices are ≈ `[[0,-1,0],[-1,0,0],[0,0,-1]]` [V]. That is the
  `-y,-x,-z` axis remap we found by hand for the 3DoF driver, which is a good consistency check.
* Controller LED models are not on the headset. They come from each controller via
  `syncboss_input_get_calibration_data` (cached by the HAL in
  `/data/vendor/misc/sensors/controllercal/`). libtrackingengines decodes them with
  "Constellation format" or "DCP format" and uses the keys `ledModelPoints`, `ledModelNormals`,
  `LedPositions`, `ImuPosition` [V strings]. Expect the same "TrackedObject/ModelPoints" JSON as
  the Rift S Touch, since it is the same controller generation [I]. Dump it with
  `syncboss_input_tool --get-cal`.

Do not commit these files: they contain the device serial number.

### 2.4 Monado upstream status (main cfa6078, our base) [V]

| Piece | Status |
|---|---|
| SLAM (`t_tracker_slam.cpp`) | Merged. VIT interface: it dlopens `libbasalt.so` (`VIT_SYSTEM_LIBRARY_PATH`). N cameras (≥2, max 8; Rift S feeds 5, WMR up to 4). Distortion models KB4, RT8, WMR; **Fisheye62 must be refit to KB4** (Rift S does this). Frames must be L8, cam0 first, one shared timestamp per frameset, IMU and frames on the same monotonic-ns clock. Build flag `XRT_FEATURE_SLAM`. |
| Rift S template | `rift_s_tracker.c`: `rift_s_fill_slam_imu_calibration`, `rift_s_fill_slam_cameras_calibration` (`inv(DeviceFromImu)*DeviceFromCamera`), device→monotonic offset filter, `u_autoexpgain` for exposure. **Closest model for a `quest1` SLAM driver.** |
| Basalt on ARM | Basalt for Monado has aarch64 guides (RPi 4/5, Radxa). Tuning: `optical_flow_detection_grid_size=40`, `min_threshold=20`. No SD835 numbers; 2 to 4 cameras at 30 Hz look plausible [I]. |
| Constellation (`src/xrt/tracking/constellation`) | Merged 2026-02 to 2026-09 (blobwatch, correspondence search, P3P, RANSAC; needs Ceres, `XRT_MODULE_CONSTELLATION_TRACKING`). The only user is **outside-in**: Rift CV1 sensors and the PS VR2 Sense controllers. The API allows a moving mosaic origin, which fits inside-out use [I]. |
| Inside-out controller tracking | **Not in main.** It is in thaytan's fork, branch `dev-constellation-controller-tracking` (2026-07): Rift S and WMR controllers. Rift S glue: `rift_s_fill_constellation_calibration`, `rift_s_tracker_push_controller_frameset`, `rift_s_controller_get_led_model`. It uses an older API, so porting means rebasing onto main's tracker. |
| Quest drivers | None upstream; no community Quest 1 camera/SLAM stack found. |

### 2.5 Path O work items

1. Frame grabber (`src/qcam`): libhybris + libsyncboss + libqcameraoculushal. Output PGMs and
   timestamps. **Prerequisite for everything else.**
2. Timestamps: frames and IMU both in SyncBoss time [I]. Map to CLOCK_MONOTONIC with the Rift S
   style offset filter.
3. Monado `quest1` SLAM: load the `/persist` JSON (reuse the rift_s parser and KB4 fit). Push the
   HEADSET-tagged frames from 2 to 4 cameras plus the 1 kHz IMU into `t_slam`. Build Basalt for
   aarch64 on the headset.
4. Controllers:
   * RE the controller stream (`syncboss_input_start`, record types; `syncboss_input_tool --viz`
     shows the raw fields).
   * Load the LED model from `syncboss_input_get_calibration_data`.
   * Feed CONTROLLER-tagged frames to the constellation tracker, porting thaytan's Rift S glue.
   * Haptics: `syncboss_input_set_haptic`.
5. Hands: out of scope.

---

## 3. Ordered work plan

| # | Step | Where | Exit criterion |
|---|---|---|---|
| 1 | Baseline on stock Android (Magisk). See §4 A. | live | Reference poses, logs, controller LED blob saved |
| 2 | Write `ovrstub`: generic zero-reply BBinder plus overrides for `vrpowermanager`/`permission`; link the system libbinder. Cross-build in WSL with the NDK. | offline | Builds; `service list` shows the names on stock-like test |
| 3 | systemd units for the sensors HAL, IAD HAL, power HAL, settingsserver, calibration_svr, cameramuxmodeservice, vrfocusserver, trackingservice. `/persist` copy and `/data/*` dirs. Property additions. | offline | Units written (`device/holo-android-tracking.sh`) |
| 4 | Native bring-up, in order: sensors HAL alone, then the daemons, then trackingservice. Then `trackinginterface_cli getheadtrackingdata json`. See §4 B. | live | `valid:1`, PosTracked bit set |
| 5 | Monado `quest1` driver: replace libsyncboss 3DoF with libossdk head (v8) and controllers (v6, input, haptics). Keep the 3DoF fallback when trackingservice is absent. | offline+live | 6DoF in hello_xr, controllers in SteamVR |
| 6 | Hands (`hand_tracking_enabled`, Hands v6 → XR_EXT_hand_tracking). | later | |
| 7 | Path O step 1: RE the qcamera prototypes, then the frame grabber. Use it to validate camera power and exposure even under Path M (it must not run while the sensors HAL owns the cameras). | parallel | 4 synchronized PGMs |
| 8 | Path O SLAM prototype (Basalt, 2 cameras) if Path M proves unstable or as the open alternative. | later | |

---

## 4. First live experiments when the headset is back

**A. Stock Android (Magisk boot)**: the reference run, and safe.
1. `su -c 'trackinginterface_cli getheadtrackingdata json'` and
   `getcontrollertrackingdata json`: the baseline (tracking-ipc §8).
2. `service list > svc.txt; lshal > lshal.txt; ps -AZ > ps.txt`: the exact service set on a
   working system.
3. `su -c 'setprop ctl.restart trackingservice'; logcat -b all -d | grep -iE 'tracking|BinderClient|getService|Waiting'`:
   which services trackingservice waits for, and in what order.
4. `su -c 'syncboss_consumers_ctl stop; syncboss_input_tool --get-cal; syncboss_input_tool --get-props; syncboss_consumers_ctl start'`,
   with both controllers awake. Save the LED-model blob (it decides the Path O controller
   feasibility).
5. `su -c 'cat /sys/bus/spi/devices/spi12.0/control/te_timestamp'` twice, 100 ms apart. Then
   repeat with the screen off. This tells us whether TE stops when the panel is blanked.
6. `su -c 'getprop | grep -iE "ovr|tracking|syncboss"'`: properties to copy into
   `mkproparea.py`.

**B. Native Holo**: one step at a time, and stop at the first hang.
1. While `monado.service` presents, read `te_timestamp` twice and check it advances every ~14 ms.
2. Stop monado (it frees libsyncboss). Start the sensors HAL as `system` with `/persist` (sda8
   `ro,noload`, or a copy) mounted. Check `lshal` lists the ICameraProvider/IImu/IPowerstate
   instances, and look in `dmesg` for "Turning on cameras" once a client asks for frames.
3. Start `ovrstub`, then IAD and power HALs, settingsserver, calibration_svr,
   cameramuxmodeservice, vrfocusserver, trackingservice, with fakelogd capturing into the journal.
   Grep for `assert`, `Waiting for service`, `could not connect`.
4. Run `trackinginterface_cli getheadtrackingdata json` as root, with monado restarted so TE runs.
   If `valid` stays 0, dump the vrpowermanager client state (`dumpsys tracking`) and try the other
   `onStateChange` value.
5. Only after B4 works: point Monado at libossdk.

**Safety:**
* Never write sda8 directly. Use `ro,noload` or a copy.
* Keep `persist.ovr.skipctrlfwupdate=1`.
* Never run two libsyncboss users at once (the Monado quest1 driver plus the sensors HAL).
* Blank the panels after display tests.

---

## 5. Evidence index

* Kernel:
  * `drivers/staging/oculus/mcu/syncboss/syncboss_camera.c` (power, `enable_cameras`)
  * `syncboss_spi.c:113-114,816-826` (probe/release snoop) and `:2023-2030` (TE IRQ)
  * `syncboss_sysfs.c:511` (`te_timestamp`)
  * `arch/arm64/boot/dts/oculus/vs1-camera.dtsi`
  * `drivers/media/platform/msm/camera_v2/*`
  * Config: `CONFIG_MSMB_CAMERA=y`, `CONFIG_SYNCBOSS_CAMERA_CONTROL=y`
* Camera libs (`system/vendor/lib64`): `libqcameradriver.so`, `libqcameraoculushal.so`,
  `libqcamerahal.so` (`QCamera::Initialize` 0x5eb0, `EnableCamera` 0x62ec, `StartCamera` 0x661c,
  `StreamingThread` 0x6a24, `QCameraHal::Initialize` 0x82e0), `libcamerahal.so`. libsyncboss
  camera functions are at 0x5888-0x66bc.
* Services:
  * xrefs via `scratchpad/t6/xsvc.py` (string → enclosing function → following calls).
  * `etc/selinux/plat_service_contexts`; `vendor/etc/vintf/manifest*.xml`.
  * `libsensorclientutils.so`: `BpVrPowerManager::*` codes 1-7, `StatusCallback::onStateChange` 0x1a1f4.
  * `vrfocusserver` `ConnectionManager::init*` (0xddb0-0xe5fc).
  * trackingservice: `(anonymous)::getService` 0x1208c8 (settingsproxy),
    `BinderClient<IPermissionController>` 0xe2eb0, `BinderClient<IRemoteService>` 0xb5a38.
* Calibration: `/persist/calibration/*` extracted with `debugfs` from the sda8 image
  (`~/q1/re/parts/cal/`, not in git).
* Monado: `~/src/monado` (main cfa6078; thaytan branches under `refs/remotes/thaytan/*`),
  `~/src/basalt`.
