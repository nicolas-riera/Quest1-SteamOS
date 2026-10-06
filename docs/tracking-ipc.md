# Quest 1 tracking IPC: talking to `trackingservice` without the VR runtime

Static reverse engineering of build `49845030443200410` (Android 10, `ro.build.branch=releases-oculus-10.0-v50`).
Source tree: WSL `~/q1/sys/system`. All work was offline. Nothing was run on the headset.
Scratch material (scripts, disassembly, symbol dumps): `%TEMP%\claude\...\scratchpad\tracking_re\`.
`tools/dis.py`, `vt.py`, `xref.py`, `keys2.py` are small objdump/readelf helpers. Every binary has a
`.gnu_debugdata` (MiniDebugInfo) section. It was extracted with `xz` and gives full local function
symbol names (`*.syms` files).

Confidence markers: **[V]** = verified in disassembly, **[I]** = inferred, needs a live check.

---

## 0. TL;DR

* Head pose, controller poses and buttons, hand tracking and haptics are all served by
  **`/system/bin/trackingservice`**. It publishes them in **ashmem shared-memory regions**. A client
  gets each region's fd over **binder**: service name **`tracking`**, interface
  `oculus.internal.tracking.ITrackingService`. Haptics go over a **socket fd** that is also
  obtained through that binder.
* Meta's official runtime (`libvrapiimpl.so` and `libvrruntimeservice.so` inside
  `VrDriver.apk`) does **not** use a private path. It uses the same client library as the CLI
  tools: **`/system/lib64/libossdk.oculus.so`** ("OS SDK"). That library is listed in
  `/system/etc/public.libraries-oculus.txt`, so any process can `dlopen` it. It exports a C
  factory ABI: `createHeadTracker(long ver)`, `createControllers`, `createHands`,
  `createControllerInput`, `createHaptics`, `createInputHub`, and so on. Each one returns a C++
  interface pointer with a stable vtable.
* **`trackinginterface_cli`** is a ready-made test client built on libossdk. Run it as **root**:
  `trackinginterface_cli getheadtrackingdata json`, `getcontrollertrackingdata json`,
  `getcontrollerbuttondata json`, `gethandtrackingdata`, `ls -m`,
  `setcontrollerhapticssimple <id> <0-255>`. This is the fastest live test.
* Access gating: shared memory is only handed to clients that **vrfocusserver** reports as
  focused. **vrfocusserver grants all background focus types to uid 0** [V], so a root helper
  passes. The alternative is permissive mode (`ro.debuggable=1` plus
  `debug.ovr.vrfocus.permissive=1`).
* Hard runtime dependencies:
  * **system_server**. It provides `permission` (trackingservice *asserts/aborts* if it is
    missing when a client attaches [V]), `settingsproxy`, `appops`, `vrpowermanager` and
    `OVRRemoteService` (controller pairing).
  * `vrfocusserver`, `cameramuxmodeservice`, `settingsserver` (preferences),
    `calibration_svr`, and the sensors HAL `vendor.oculus.sensors-hal-1-0`, which owns
    `/dev/syncboss*` and the cameras.
* State gates:
  * **VrPowerManager** (Java, in system_server) must report RUNNING. That requires
    headset-mounted (proximity) and screen-on. Use
    `am broadcast -a com.oculus.vrpowermanager.prox_close` to fake mounted.
  * The **sensors HAL drops IMU data when it cannot correlate SyncBoss time with display
    vsync**. It reads the TE timestamp from `/sys/bus/spi/devices/spi12.0/control/te_timestamp`.
    The panel must be producing TE, so Linux has to keep the DSI panels on.
* Hand tracking runs **inside trackingservice**: `HandTrackingGlue`, `libtrackingengines.so`,
  and Hexagon DSP via fastrpc. It is not in the app runtime. It is enabled with the preference
  `hand_tracking_enabled` (`oculuspreferences --setc hand_tracking_enabled true`). The camera mux
  mode switches to `HandTracking`. The 19 bone rotations still need the static hand skeleton,
  which is available from `vrapi_GetHandSkeleton` in `libvrapiimpl.so` (no session needed [V]).

---

## 1. Process and service graph

### 1.1 Daemons (init `.rc`, verbatim essentials)

| Service (init name) | Binary | rc file | class | user / groups / caps | Notes |
|---|---|---|---|---|---|
| `vendor.oculus.sensors-hal-1-0` | `/vendor/bin/hw/vendor.oculus.hardware.sensors@1.0-service` | `vendor/etc/init/vendor.oculus.hardware.sensors@1.0-service.rc` | `hal` | system / system / `SYS_NICE`, `writepid /dev/cpuset/tracking/tasks`, `shutdown delayed 20` | HIDL `android.hardware.sensors@2.0::ISensors`, `vendor.oculus.hardware.sensors@1.0::{ICameraProvider, IControllerProvider, IImu, IMag}`, `vendor.oculus.hardware.sensors_java@1.0::IPowerstate`. `onrestart restart calibration_svr / mrsystemservice / sensorproxy / trackingservice`. Links `libsyncboss.so`, `libcamerahal.so`, `libcalibrationstore.so`, `libfmq.so`. |
| `vendor.oculus.iad-hal-1-0` | `/vendor/bin/hw/vendor.oculus.hardware.sensors@1.0-iad` | `vendor.oculus.hardware.sensors@1.0-iad.rc` | `early_hal` | system / system | `vendor.oculus.hardware.sensors@1.0::IIad` (physical IPD slider, `libiad.so`) |
| `trackingservice` | `/system/bin/trackingservice` (1.39 MB, + `libtrackingengines.so` 25.9 MB) | `etc/init/trackingservice.rc` | `late_start` | **system / system uhid inet / `SYS_NICE`**, `writepid /dev/cpuset/tracking/tasks`, `ioprio rt 4` | `on post-fs-data mkdir /data/misc/tracking 0777`. `early-boot` creates cpusets `tracking` and `object_tracking` (CPUs 0-3). |
| `calibration_svr` | `/system/bin/calibrationserver` | `calibrationserver.rc` | `late_start` | system / system | `/persist/calibration{,/online,/online.tmp}` |
| `cameramuxmodeservice` | `/system/bin/cameramuxmodeservice` | `cameramuxmodeservice.rc` | `late_start` | system / system camera / `SYS_NICE` | Reads `/system/etc/cameramuxmode/config.json` (modes `default`, `HandTracking`, `HandTrackingUseIOT`, ...) |
| `vrfocus` | `/system/bin/vrfocusserver` | `vrfocusserver.rc` | `late_start` | **root** / root system | Focus arbitration (binder `vrfocus`). Uses ActivityManager and PackageManager (system_server) and `vrfocushelper`. |
| `mrsystemservice` | `/system/bin/mrsystemservice` | `mrsystemservice.rc` | `late_start` | system / system camera | Passthrough. Consumes HEADSET frames. Not needed. |
| `settingsserver` | `/system/bin/settingsserver` | `preferencesserver.rc` | `core` (started on post-fs-data) | system | Oculus preferences store (`/data/oculus/preferences`). Read by trackingservice through `createPreferencesManager`. |
| `vrapi_svr` | `/system/bin/vrapiserver` | `vrapiserver.rc` | `late_start` | system / system readproc | Clocks, thermal and power for VR apps. Not a tracking dependency. |
| `runtimeipcbroker` | `/system/bin/runtimeipcbroker` | `runtimeipcbroker.rc` | `late_start` | system | Runtime IPC (app to VrDriver). Not needed. |
| system_server (Java) | `framework/oculus-system-services.jar`, `com.oculus.os.platform.jar` | n/a | n/a | n/a | Hosts `vrpowermanager` (`oculus.internal.VrPowerManagerService`), `settingsproxy`, `OVRRemoteService` (controller pairing), plus AOSP `permission` and `appops`. |
| VrDriver (`com.oculus.systemdriver`) | `priv-app/VrDriver/VrDriver.apk` (`libvrruntimeservice.so`, `libvrapiimpl.so`) | n/a | n/a | n/a | The official runtime. **Not required** for tracking. It is only a libossdk client. It also drives hand-tracking UX (auto-transition, toasts). |

`sensorproxy` appears in `onrestart` and in `syncboss_consumers_ctl`, but this build has no binary for it.

`/vendor/bin/syncboss_consumers_ctl` (shell script) stops and starts, in order:
`vendor.oculus.sensors-hal-1-0 trackingservice wifisighelper calibration_svr mrsystemservice sensorproxy`.

`/system/bin/trackingservice_ctl [start|stop|restart]` sends `SIGHUP`, sleeps 2 s, then runs `stop trackingservice`.

### 1.2 Device nodes and files

| Component | Nodes / paths (from strings) |
|---|---|
| sensors HAL (`libsyncboss.so`) | `/dev/syncboss0`, `/dev/syncboss_control0`, `/dev/syncboss_stream0`, `/dev/syncboss_powerstate0` (events `PROX_ON` / `PROX_OFF`), `/dev/ttyHS99`, `/sys/devices/virtual/misc/syncboss0/spi/control/{transaction_length,transaction_period_us,spi_max_clk_rate,reset,...}`, **`/sys/bus/spi/devices/spi12.0/control/te_timestamp`** (display TE, see §6), `/vendor/firmware/{jedi,lcon,ruby}_archive.bin`, `/data/vendor/misc/sensors/controllercal/` |
| camera stack (`libcamerahal` → `libqcameraoculushal` / `libqcameradriver`) | `/dev/ion` (+ qcom camera V4L2 nodes via the qcamera driver) |
| trackingservice | `/dev/uinput` (keyboard-only virtual device, §4), `/data/misc/tracking/*`, `/vision/`, `/vision/tempMaps/`. `libtrackingutils.so` dlopens `libadsprpc.so` / `libcdsprpc.so` (FastRPC to the DSP, `/dev/adsprpc-smd`). `libtrackingengines.so` drives Hexagon NN for hands (`/system/etc/handtracking/runtime/...`). |
| Binder | `/dev/binder` (AIDL services), `/dev/hwbinder` (HIDL sensors HAL, FMQ / eventflag), `/dev/vndbinder` |

### 1.3 Dependency graph (who talks to whom)

```
         syncboss MCU (SPI) ── /dev/syncboss* ──┐        cameras (qcom, ION)
                                               ▼              │
        vendor.oculus.sensors-hal-1-0  (HIDL over hwbinder + FMQ)
          IImu / IMag / ICameraProvider / IControllerProvider / IPowerstate / ISensors
                                               │
   iad-hal (IIad) ──────────────┐              ▼
   cameramuxmodeservice ────────┼──────►  trackingservice  ◄── settingsserver (prefs: hand_tracking_enabled ...)
   calibration_svr ─────────────┤        (IOT SLAM, constellation, hands, IAD, exposure, uinput)
   vrfocusserver (focus) ───────┤              │  binder services: tracking, TrackingService (dump),
   system_server: permission, appops,          │   HandTrackingService, TrackedObjectService,
     settingsproxy, vrpowermanager,            │   TrackingEnvironment, TrackingFidelityService
     OVRRemoteService ───────────┘             ▼
                                   ashmem regions (Head / Controller / Hand / Body / Anchor / Eye / Face / Orthofit)
                                   + haptics socket fd
                                               ▼
                        libossdk.oculus.so (in client process)  ◄─ trackinginterface_cli, VrDriver runtime, YOUR HELPER
```

There is **no runtime dependency on SurfaceFlinger or HWC vsync** inside trackingservice. Its
NEEDED list contains `vendor.oculus.hardware.graphics.composer@1.1.so`, but it imports no
composer symbols. The display coupling is through the **TE timestamp** in the sensors HAL (§6).

---

## 2. Client API

### 2.1 Binder services (servicemanager, `/dev/binder`)

From `etc/selinux/plat_service_contexts` and strings in trackingservice and libossdk:

| Name | Interface descriptor | SELinux type | Host |
|---|---|---|---|
| **`tracking`** | `oculus.internal.tracking.ITrackingService` | `tracking_service` | trackingservice |
| `TrackingService` | dump only (`dumpsys TrackingService` is deprecated, use `dumpsys tracking [--json] [--pretty]`) | `tracking_dump_service` | trackingservice |
| `HandTrackingService` | `oculus.internal.IHandTrackingService` (only `setHandTrackingVersion(int)`) | `handtracking_service` | trackingservice |
| `TrackedObjectService` | `oculus.internal.trackedobject.ITrackedObjectService` | `trackedobject_service` | trackingservice |
| `TrackingEnvironment` | `oculus.internal.ITrackingEnvironment` (maps, anchors, `getGroundPlaneHeight`, `setOrientationOnlyMode`, ...) | `trackingenvironment_service` | trackingservice |
| `TrackingFidelityService` | `oculus.internal.ITrackingFidelityService` | `tracking_fidelity_service` | trackingservice |
| `vrfocus` | `oculus.internal.IVrFocusService` | `vrfocus_service` | vrfocusserver |
| `vrpowermanager` | `oculus.internal.power.IVrPowerManager` | `vrpowermanager_service` | system_server |
| `settingsproxy`, `permission`, `appops`, `OVRRemoteService` | AOSP / Oculus | n/a | system_server |
| (cameramuxmodeservice) | `oculus.internal.ICameraMuxModeService` (`setActiveCameraMuxMode(String16)`, `registerClient`, `getAllMuxModes`) | n/a | cameramuxmodeservice |

### 2.2 `ITrackingService` (the data API) **[V]**

Proxy code is in `libossdk.oculus.so` (`BpTrackingService`, exported). Transaction codes come
from the `transact(code, ...)` immediates:

| code | method (C++) | in | out |
|---|---|---|---|
| 1 | `getSharedMemoryFileDescriptor(const sp<ITrackingServiceClient>& client, const SharedMemoryRequest& req, ParcelFileDescriptor* out)` | interface token, `writeStrongBinder(client)`, `writeParcelable(req)` | `binder::Status` + `ParcelFileDescriptor` |
| 2 | `getTrackingSocketFileDescriptor(ParcelFileDescriptor* out)` | token | Status + PFD (haptics socket) |
| 3 | `unregisterClient(const sp<ITrackingServiceClient>&)` | token, binder | Status |

* `SharedMemoryRequest` = one `int32 MemoryType` (`writeToParcel` writes a single `writeInt32` [V]).
  The values come from the server-side jump table in `TrackingService::getSharedMemoryFileDescriptor`
  and from the client `getSharedMemoryFromServiceFor<T>` constants [V]:

  | MemoryType | Region | Server function | Extra permission check |
  |---|---|---|---|
  | 0 | Anchor (`AnchorTrackerSharedData`) | `memoryAnchorTracker` | yes (anchor API) |
  | 1 | Body | `memoryBodyTracker` | n/a |
  | **2** | **Controller** (`ControllerSharedData`) | `memoryController` | **none** |
  | 3 | Eye | `memoryEyeTracker` | `com.oculus.permission.EYE_TRACKING` |
  | 4 | Face | `memoryFaceTracker` | `FACE_TRACKING` / `RECORD_AUDIO` |
  | **5** | **Hand** (`HandTrackerSharedData`) | `memoryHandTracker` | **none** |
  | **6** | **Head** (`HeadTrackerSharedData`) | `memoryHeadTracker` | **none** |
  | 7 | Orthofit | `memoryOrthofitTracker` | `ORTHOFIT_DATA` |
* The client must pass its own **`ITrackingServiceClient`** binder (descriptor
  `oculus.internal.tracking.ITrackingServiceClient`). It has one method, code 1:
  `getTrackingModeFlags(int* out)`, which the service calls back. It is also used for death
  notification.
* Delivery: the returned fd is an **ashmem** region (`ashmem_create_region`). libossdk maps it
  **read-only** (`OVR::OS::mapMemoryConst<T>(fd, size, ...)`). The writer is
  `OVR::OS::Tracking::MemoryBroker<T>`, and `reallocate()` exists, so a region can be remapped
  (`maybeRemapSharedMemory`). Inside, poses are kept in
  `OVR::ovrLocklessCircularBufferT<RigidBodyTrackingData, ovrHashPolicy_Copy, 5>`: a 5-deep
  lock-free history with a hash/copy consistency check. Prediction to the requested timestamp is
  done **client-side** in libossdk (`libposeprediction.so`, `BaseHeadTracker::getStateV4`,
  `getHistoricalState<>`).
* Haptics: libossdk calls `getTrackingSocketFileDescriptor` and then writes
  `OVR::OS::Haptics::trackingSocketMsgStruct` messages. The server handles them in
  `TrackingServiceHost::handle{Simple,MultiSimple,Buffered,AppendBuffered,AppendPcm}Haptics`.

Reimplementing this without libossdk is possible but not recommended. You would have to
replicate the per-version shared-memory structs, the lock-free ring buffer and the pose
predictor. Use libossdk.

### 2.3 Access control in `getSharedMemoryFileDescriptor` **[V]**

From `OVR::OS::Tracking::TrackingService::getSharedMemoryFileDescriptor`
(`0xe174c` in trackingservice) and `MemoryBrokerBase`:

1. `getCallingUid/Pid`, `Process::getProcessName`.
2. `MemoryBrokerBase::attachClient()` creates a `Client`, calls `getPackageName(uid, pid)` and
   `startAppOps()`:
   * `getPackageName` calls `defaultServiceManager()->getService("permission")`. **If that
     returns null it hits `__android_log_buf_assert("binder == nullptr", "Cannot get permission
     service")`**, which aborts trackingservice. So system_server must be up. Otherwise it calls
     `IPermissionController::getPackagesForUid`, and falls back to `/proc/<pid>/cmdline` when
     there is no package ("No packages for calling uid %d, pid %d").
   * `startAppOps()` calls `AppOpsManager::startOpNoThrow` for the broker's op list. A failure
     only logs "Access denied. Failed to start AppOp ..." and **the return value is ignored by
     attachClient**.
3. Then `VrFocusListener::onVrFocusChanged(..., FocusType)` re-queries vrfocusserver
   (`IVrFocusManager::getClientFocusStatus(type, pids)`).
   `MemoryBrokerBase::updateFocusedClients` sets `Client::hasAccess` = "pid present in the focus
   reply and flagged focused".
4. `MemoryBroker<T>::fd(client)` returns the fd **only if `hasAccess()`**. Otherwise you get the
   binder exception "requested shared memory region when not registered/focused".
   Head and Anchor use FocusType 0 (`HeadTracking`). Controller, hands and the others use
   FocusType 1 (`InputTracking`).

vrfocusserver policy (`OVR::OS::FocusPolicy`, `vrfocusserver` `0xff60` / `0x10664`) [V]:

* In `tryAddingFocusClient`, the loop over the background-permission map
  (`com.oculus.permission.ACCESS_BACKGROUND_HEAD_TRACKING` → HeadTracking,
  `ACCESS_BACKGROUND_INPUT_TRACKING` → InputTracking) **skips the permission check and grants
  the type when `uid == 0`**. A process name or uid allowlist also exists
  (`/system/bin/mrsystemservice`, `com.oculus.systemdriver`, `com.oculus.vrshell`).
* `computeFocusState`: a client has focus if it holds a background grant for that type, if it is
  the immersive or foreground app, or if `isPermissiveFocus()`. That function returns
  `property_get_bool("ro.debuggable") && property_get_bool("debug.ovr.vrfocus.permissive")`.
  It is evaluated on every query, so Magisk `resetprop ro.debuggable 1` plus
  `setprop debug.ovr.vrfocus.permissive 1` works at runtime.
* `dumpsys vrfocus [-a]` prints the clients, current focus and "Permissive: ...".

**So: run the helper as real uid 0** (in the container, uid 0 must be the init-namespace uid
seen by binder, so no user-namespace remapping). Alternatively, use permissive focus.

### 2.4 libossdk ABI (what the CLI and the runtime use) **[V]**

* Loader (CLI): `android_get_exported_namespace("default")` then
  `android_dlopen_ext("libossdk.oculus.so", RTLD_NOW)`, falling back to `dlopen`.
  `/data/local/tmp` binaries use the `[unrestricted]` linker section
  (`etc/ld.config.29.txt`), so a plain `dlopen` works there.
* Factories (extern "C"): `void* createX(long version)` / `void destroyX(long version, void* p)`.
  Supported versions: HeadTracker 6..8 [V, range check in `createHeadTracker`]. Versions the
  CLI uses [V]: HeadTracker 8 and 6, Controllers 6, Hands 6 and 2, ControllerInput 1,
  Haptics 1, InputHub 1 and 2. `createControllers` and `createHands` contain several Impl
  vtables (v2/3, v4/5, v6 for controllers; V1 to V6 for hands).
  The pointer returned is already the interface sub-object. The CLI does
  `createHeadTracker(8)` and calls through `*(void***)p`.
* Interfaces (vtables read from `.data.rel.ro`; RELR-packed relocations, so the addends are in
  place):

```cpp
// Reconstructed from libossdk.oculus.so (this build). Own declarations, not Meta headers.
#include <stdint.h>
struct OsTs { int64_t ns; };          // OSSDK::Sensors::v3::Timestamp<TheProcessingClock>: CLOCK_MONOTONIC ns
typedef uint64_t OsDeviceId;          // OSSDK::Sensors::v3::DeviceId (u64, printed %016lx)

// Big returns are via x8 (sret). Over-allocate; read the "has value" flag at the offsets in §3.
struct alignas(16) HeadStateOpt   { uint8_t b[0x100]; };   // RigidBodyTrackingData v5 (+flag @0xA0)
struct alignas(16) CtrlStateOpt   { uint8_t b[0x100]; };   // RigidBodyTrackingData v2 (+flag @0x98)
struct alignas(16) CollisionOpt   { uint8_t b[0x40];  };
struct alignas(16) ButtonPropsOpt { uint8_t b[0x40];  };   // flag @0x14
struct alignas(16) ButtonStateOpt { uint8_t b[0x100]; };   // flag @0x98
struct alignas(16) HandsStateOpt  { uint8_t b[0x1000]; };  // HandTrackingPoseData v6 (~0xCE0) + flag
struct alignas(16) DeviceArray    { void* data; uint64_t count; uint8_t rest[48]; }; // OSSDK::Array<DeviceDefinition(0xB8)>

struct IHeadTrackerV8 {                         // createHeadTracker(8), vtable 0x13c348
  virtual HeadStateOpt getState(const OsTs& t) = 0;      // [0] HeadTrackerImplv8::getState
  virtual uint64_t     getIpd() = 0;                     // [1] Optional<float>: float in low 32 bits, has_value = byte 4
  virtual CollisionOpt getLastCollision() = 0;           // [2]
};
struct IControllersV6 {                         // createControllers(6), vtable 0x13bc88
  virtual CtrlStateOpt getState(OsDeviceId id, OsTs t) = 0; // [0] (timestamp BY VALUE)
  virtual CollisionOpt getLastCollision(OsDeviceId id) = 0; // [1]
  virtual uint64_t     isInHand(OsDeviceId id) = 0;         // [2] Optional<bool> [I: return encoding unverified]
};
struct IInputHubV1 {                            // createInputHub(1), vtable 0x136d60
  virtual uint64_t     hasStateChangedSince(uint64_t* token) = 0; // [0]
  virtual DeviceArray  enumerateAll() = 0;                        // [1] elements 0xB8 bytes, +0 = u64 DeviceId
};
struct IControllerInputV1 {                     // createControllerInput(1), vtable 0x136c88
  virtual ButtonPropsOpt getButtonProperties(OsDeviceId id) = 0; // [0]
  virtual ButtonStateOpt getButtonState(OsDeviceId id) = 0;      // [1]
  virtual uint64_t       getTouchpadState(OsDeviceId id) = 0;    // [2] packed, see §3.4
};
struct IHapticsV1 {                             // createHaptics(1), vtable 0x136cc0
  virtual uint64_t setSimpleHaptics(OsDeviceId id, float amp01) = 0;                       // [0]
  virtual uint64_t setBufferedHaptics(OsDeviceId id, OsTs start, const float* s, uint64_t n) = 0; // [1] ArrayView = {ptr,count}
  virtual uint64_t setAppendBufferedHaptics(OsDeviceId id, const float* s, uint64_t n) = 0; // [2]
};
struct IHandsV6 {                               // createHands(6), vtable 0x13bee0
  virtual HandsStateOpt getState(const OsTs& t) = 0;     // [0]
  virtual uint64_t hasPoseChangedSince(uint64_t* tok) = 0; // [1]
  virtual uint64_t getHandScale() = 0;                   // [2] Optional<float> [I]
  // Return types of the haptics/hands helpers above are [I]; the slot order is [V].
  virtual uint64_t getHandTrackingEnabled() = 0;         // [3]
  virtual uint64_t requestHandTrackingVersion(uint32_t) = 0; // [4]
  virtual uint64_t getHandTrackingVersion() = 0;         // [5]
};
```

`ArrayView<const float>` is passed as two registers: `(ptr, count)`. The CLI passes 25 samples
for buffered haptics [V]. For a live client, make sure the binder thread pool is running. Both
libossdk and the CLI import `ProcessState::startThreadPool`. Call
`ABinderProcess_startThreadPool()` or the equivalent yourself to be safe.

---

## 3. Data layouts (from `trackinginterface_cli` JSON / text dumpers)

All offsets are relative to the start of the returned struct. Floats are 32-bit, timestamps are
int64 ns (CLOCK_MONOTONIC), quaternions are `x,y,z,w`. Units are meters, rad/s, m/s, m/s².
The coordinate convention is presumably the VrApi/OpenXR one (+Y up, -Z forward, right-handed)
**[I]**.

### 3.1 `RigidBodyTrackingData` (head = v5, controllers = v2) **[V]**

| off | type | meaning (CLI key) |
|---|---|---|
| 0x00 | u8 | `valid` |
| 0x04 | u32 | status bits: `0x1` OriTracked, `0x2` PosTracked, `0x4` OriValid, `0x8` PosValid (same values as VrApi `ovrTrackingStatus`) |
| 0x10 | f32[4] | orientation `rot_q_x..w` |
| 0x20 | f32[3] | position `pos_x..z` |
| 0x2C | f32[3] | angular velocity `rot_vel_*` |
| 0x38 | f32[3] | linear velocity `pos_vel_*` |
| 0x44 | f32[3] | angular acceleration `rot_accel_*` |
| 0x50 | f32[3] | linear acceleration `pos_accel_*` |
| 0x60 | i64 | sample time ns (`tracking_time`) |
| 0x68 | i64 | predicted-for time ns (CLI prints `Prediction = (0x68 - 0x60)/1e9`) |
| 0x70 | 16 B | odometry UUID (head only) |
| ~0x80-0x9C | pose | `ReferenceFromOdometry` (translation z at 0x98) (head only) |
| **0xA0** | u8 | head `Optional` has_value (v5 total payload 0xA0) |
| **0x98** | u8 | controller `Optional` has_value (v2 payload 0x98) |

### 3.2 Collision (`getLastCollision`) [V/I]

+0x08 f64 magnitude, +0x10 f64 duration (seconds), timestamp `last_collision_timstamp` (sic).

### 3.3 Controller enumeration (`IInputHub::enumerateAll`, element 0xB8 bytes) [V/I]

+0x00 u64 DeviceId, +0x08 i32 type/model, +0x10 u32 handedness (enum with 4 values; names in a
CLI LookupTable), +0x58 char* revision, +0x78 char* serial.
CLI text: `[Device %016lx serial=%s model=%s(%d) version=%d revision=%s handedness=%s battery=%s extra=%s]`.

### 3.4 Buttons (`IControllerInput v1`) **[V]**

`OSSDK::Input::v1::Button` is a bitmask. The CLI name table is at `0x2e9b0`:
`1 tr`, `2 h`(home), `4 b`(back/menu), `8 tp`(stick click), `16 vu`, `32 vd`, `64 tr`(touch),
`128 g`(grip), `256 b0`(A/X), `512 b0`(touch), `1024 b1`(B/Y), `2048 b1`(touch), `4096 tp`(prox),
`8192 tr`(prox), `16384 b0`(prox), `32768 b1`(prox), `65536 p0`(thumbrest touch), `131072 p0`(prox).

`ButtonState` (0x98 bytes, `Optional` flag at +0x98) is an array of 8-byte slots indexed by bit
number. Bit 0 of each slot = state:

| slot off | CLI key | slot off | CLI key |
|---|---|---|---|
| 0x00 | `tr` trigger click | 0x48 | `b0_touch` |
| 0x08 | `home` | 0x50 | `b1` (B/Y) |
| 0x10 | `back` | 0x58 | `b1_touch` |
| 0x18 | `ts` stick click | 0x60 | `ts_prox` |
| 0x20/0x28 | vol+/vol- | 0x68 | `tr_prox` |
| 0x30 | `tr_touch` | 0x70/0x78 | `b0_prox` / `b1_prox` |
| 0x38 | `grip` (digital) | 0x80/0x88 | `pt_touch` / `pt_prox` (thumbrest) |
| 0x40 | `b0` (A/X) | **0x90 f32** | trigger analog (`fore`) |
| | | **0x94 f32** | grip analog (`grip`) |

`getTouchpadState` returns 8 bytes in x0: byte0 = stick touched (`ts_touch`), u16 x at +2,
u16 y at +4, byte6 = has_value.
`ButtonProperties` (flag @0x14): +0 i32 maxX, +4 i32 maxY. The CLI normalizes
`sx = (x/maxX - 0.5)*2`, `sy = (y/maxY - 0.5)*-2`.

### 3.5 Hands (`IHands v6::getState`) **[V/I]**

Two `HandTrackingHandPose` records of 0x248 bytes each (index 0 = left, 1 = right), followed by
per-hand extra data (stride 0x308, EMM mode at +0x950 + hand*0x308). Total ≈ 0xCE0 bytes, with
the `Optional` flag after it **[I]**.

| off (per hand) | type | meaning |
|---|---|---|
| 0x000 | u32 | `HandTrackingStatus` |
| 0x008 | i64 | timestamp ns |
| 0x010 | i64 | output timestamp ns |
| 0x018 | f32[4] | root pose rotation |
| 0x028 | f32[3] | root pose translation |
| 0x034 | f32[22] | joint angles |
| 0x08C | f32[19][4] | **bone rotations** (WristRoot, ForearmStub, Thumb0-3, Index1-3, Middle1-3, Ring1-3, Pinky0-3) |
| 0x1C0 | f32[5] | finger confidences (`confid_thumb..pinky`) |
| 0x1D4 | f32[5] | pinch finger occlusion |
| 0x1E8 | f32 | MLPinchConfidence |
| 0x1F0 | u32 | hand-tracked version |
| 0x1F4.. | | visibility predictions, landmark sigmas |

`getHandScale()` returns Optional<float>. Turning the 19 local bone rotations into OpenXR's 26
joints needs the bind-pose skeleton (24 bones, including 5 tips). That skeleton is static data
in `libvrapiimpl.so` (`HandSkeletonData.cpp`, exported `vrapi_GetHandSkeleton`). The function
**ignores its `ovrMobile*` argument** (x0 is never read) and switches only on handedness (1/2)
and the header version (`0xdf000001` / `0xdf300001`, 24 bones) [V]. A helper can `dlopen` the
library from `VrDriver.apk!/lib/arm64-v8a/` (libs are stored uncompressed) and query it once at
runtime. This needs no redistribution.

---

## 4. CLI tools (fastest live test)

### `trackinginterface_cli` ("Test client for OSNDK TrackingService interface")

Usage strings (verbatim). Commands are matched lowercase:

```
getHeadTrackingData       [prediction_ms | 'rand'] [json] Get headset tracking data from shared memory
getcontrollertrackingdata [prediction_ms | 'rand'] [json] Get controller tracking data from shared memory
getcontrollerbuttondata   [json]                          Get controller button data from shared memory
handsenabled
gethandscale
gethandtrackingdata       [prediction_ms] [json]
getipd                                                   Get IPD
ls                                                       List connected input devices, add -m to monitor
setControllerHapticsSimple   [controllerId] [amplitude]  Set simple haptics; id from getcontrollertrackingdata
setControllerHapticsBuffered [controllerId] [amplitude]  Set 10 frames of pulsing buffered haptics; id from getcontrollertrackingdata
setControllerHapticsAppend   [controllerId] [amplitude]  Set 10 frames of pulsing append haptics; id from getcontrollertrackingdata
secure [command]                                         Execute another command in 'secure' mode
audit [command]                                          Execute another command in 'audit' mode
getEyeTrackingData [prediction_ms | 'rand']              Get eye tracking data from shared memory
getOrthofitState                                         Get Orthofit state from shared memory
getbodytrackingdata          [prediction_ms | 'rand']    Get body tracking state from shared memory
```

* Each query is a one-shot. `ls -m` monitors. Amplitude is 0-255 and is divided by 255; the id
  is hex.
* JSON keys: `valid, tracking_time, pos_x..z, rot_q_x..w, pos_vel_*, pos_accel_*, rot_vel_*,
  rot_accel_*, last_collision_*, in_hand`. Buttons: `tr home back ts b0 b1 tr_touch ts_touch
  b0_touch b1_touch pt_touch tr_prox ts_prox b0_prox b1_prox pt_prox fore grip ts_x ts_y ts_d`.
* Text mode: `Valid: %d OriTracked/OriValid/PosTracked/PosValid`,
  `Time: %.3f, Prediction: %.4f`, `Translation: (...)`, `Rotation: Q=(...) Euler=(...)`,
  `Controller #%016lx:`.

### Other tools

* `tracked_object_ctl --start|--stop <type> | --state | --watch | --names | --events`: tracked
  keyboards. Not needed.
* `calibration_manager_ctl --start|--stop`: online calibration of the RGB camera. Requires root.
* `oculuspreferences --get/--set/--getc/--setc/--reset/--listen`: Oculus preferences, for
  example `--setc hand_tracking_enabled true`.
* `dumpsys tracking [--json] [--pretty]`, `dumpsys vrfocus [-a]`: live state.
* `/vendor/bin/syncboss_input_tool` (statically linked, talks to `/dev/syncboss*` directly). It
  refuses to run while HAL clients are active, unless you run `syncboss_consumers_ctl stop` or
  pass `--ignore-other-clients`. Options: `--list`, `--list-watch`, `--stream`, `--stream-raw`,
  `--viz` (raw controller IMU acc/gyro, buttons, touch/prox, trigger/grip, stick, battery),
  `--set-haptic <amp...>`, `--get-props`, `--get-cal`, `--pair/--unpair`, fw update.
  It gives **raw** controller data with no 6DoF. It is useful as a fallback and debugging aid,
  not as a pose source.

---

## 5. Buttons, triggers, sticks, haptics

* Source of truth: trackingservice publishes a controller region (MemoryType 2). Read it through
  libossdk `IControllerInput v1` (buttons, analog, touch, prox, stick) and `IInputHub`
  (enumeration, battery, `hasStateChangedSince`). The underlying chain is controller RF
  (pulsar), the SyncBoss MCU, the sensors HAL (`IControllerProvider` + FMQ), and then
  trackingservice (`ControllerGlue`, `ControllerInputProcessor::applyButtonMapping`).
* trackingservice also creates a **uinput** device (`OVR::OS::VirtualDevice`,
  `UI_SET_EVBIT(EV_KEY)` only, `SendKey`). It is a keyboard-style device for Android system keys,
  not full gamepad state. Do not rely on evdev.
* Haptics: `IHapticsV1::setSimpleHaptics(id, amp 0..1)`. Re-send it each frame and send 0 to
  stop; VrApi simple vibration also has to be refreshed **[I]**. Buffered or append haptics take
  sample arrays. The transport is the tracking socket. The raw path is
  `syncboss_input_tool --set-haptic`.

## 6. Hand tracking

* Hand tracking runs in **trackingservice**: `OVR::TrackingService::HandTrackingGlue` (camera
  frames of type `HAND`), `libtrackingengines.so` (models under
  `/system/etc/handtracking/runtime/{common,quest}`, Hexagon NN through
  `libadsprpc`/`libcdsprpc`). Output goes to the Hand region (MemoryType 5) through
  `TrackingServiceHost::updateHandTrackingPose`. **It does not depend on the app runtime.**
* Gating: preferences `hand_tracking_enabled`, `hand_tracking_use_iot` and
  `multimodal_hands_and_controllers_enabled` are read in
  `TrackingFidelityService::setCameraMuxMode()`. That function calls
  `CameraMuxModeMonitor::setActiveCameraMuxMode("default" | "HandTracking" |
  "HandTrackingUseIOT")`. The `default` mode feeds `ConstellationTrackingEngine` (controllers).
  The `HandTracking` mode adds HAND frames. On Quest 1 the system switches between them.
  `ActivityMonitor` can sleep controllers when hands are on
  (`persist.ovr.tracking.sleepcontrolleronhandtracking`). The VrDriver runtime only adds UX
  (auto-transition, notifications). Live-check whether hands come up without VrDriver once
  `hand_tracking_enabled=true` and the controllers are put down **[I]**.

## 7. What keeps the cameras and IMU alive (VR mode, mount, vsync)

1. **VrPowerManagerService** (system_server). trackingservice registers as an
   `IVrPowerManagerClient` (`createVrPowerMonitor`) and receives
   `onRunning/runningToStandby/sleepToStandby/onStandby` calls, which map to
   `StateHandler::setState(PowerState {Running, Standby, Sleep})`. In Standby or Sleep,
   `BaseTrackingGlue::stopCameras`, and controllers are told the system state. The inputs are
   headset mount (proximity `PROX_ON/OFF` from `/dev/syncboss_powerstate0` via the
   sensors_java `IPowerstate` HAL), screen on/off (`ACTION_SCREEN_ON/OFF`, `isInteractive`)
   and `sys.hmt.mounted`. Overrides found in the jar:
   * `am broadcast -a com.oculus.vrpowermanager.prox_close` (virtual proximity: always mounted),
     `...prox_far`, `...automation_disable`.
   * The "MountWakeLock" / `acquirePowerStateLock` API.
   * The setting `prox_sensor_disabled`.

   Android must also think the screen is interactive (`svc power stayon true`, wake). In a
   headless container this is the biggest unknown.
2. **Display TE / vsync** (sensors HAL `OVR::Sensors::VsyncClock`). SyncBoss vsync events are
   paired with the host TE time read from `/sys/bus/spi/devices/spi12.0/control/te_timestamp`
   (`MontereyHostTimeSource`). If that fails you get "failed getting sync time from host source"
   and "Data for sensor type %s dropped because of failure to update clock drift from vsync".
   The HAL also toggles "Syncboss VSYNC signal". **The DSI panels must be on and producing TE**
   even when Linux, not SurfaceFlinger, scans out. Check with
   `cat /sys/bus/spi/devices/spi12.0/control/te_timestamp` (it should advance about every 14 ms
   at 72 Hz). The kernel side is in the Oculus kernel tree you already build (syncboss SPI
   driver).
3. **Camera mux mode** comes from cameramuxmodeservice and `config.json`. It is set by
   trackingservice itself, so no compositor is involved.
4. **Focus** (§2.3). The client must be root or permissive.
5. **Controller pairing and management**: `OVRRemoteService` in system_server.
   `pairing_ctl` and `rstest` binaries also exist.

---

## 8. Recommended approach for qbridge

Build a small **bionic helper (`qb_track`)** that runs in the Android container as uid 0. It
`dlopen`s `libossdk.oculus.so`, creates `HeadTracker(8)`, `Controllers(6)`, `InputHub(1)`,
`ControllerInput(1)`, `Haptics(1)` and `Hands(6)`, and polls each frame with
`t = CLOCK_MONOTONIC + predicted_display_latency`. It fills the qbridge message (head pose, IPD
from `getIpd` giving eye offsets, 2 controllers with buttons, trigger, grip and stick, and 2 hands
converted to 26 joints using `vrapi_GetHandSkeleton` data plus `getHandScale`). It applies
haptics received from Monado with `setSimpleHaptics`. FOV and distortion constants are not
served by trackingservice; take them from your existing qbridge or Monado config (candidate
source: `createVrDeviceManager` → `getDisplayParams` [I]).

The same libossdk calls also work from the existing qbridge Android app through JNI, because the
library is public. A native root helper avoids the focus problem.

### First live experiment (stock boot, Magisk root, headset worn)

1. `su -c 'trackinginterface_cli ls'`. Note the controller ids.
2. `su -c 'trackinginterface_cli getheadtrackingdata json'`, then move your head and repeat.
   Expect `valid:true` and a changing pose. For a live view:
   `su -c 'while true; do trackinginterface_cli getheadtrackingdata 0 json; sleep 0.1; done'`.
3. `su -c 'trackinginterface_cli getcontrollertrackingdata json'`,
   `... getcontrollerbuttondata json` (press buttons),
   `... setcontrollerhapticssimple <id> 200`.
4. `su -c 'oculuspreferences --getc hand_tracking_enabled'`. If it is false, run
   `--setc hand_tracking_enabled true`. Put the controllers down and run
   `trackinginterface_cli handsenabled` and `gethandtrackingdata`.
5. Negative test: run the same command as shell uid (no su). Expect the error
   "requested shared memory region when not registered/focused". This confirms the focus gate.
   Then check `dumpsys vrfocus`.
6. Remove the runtime: `am force-stop com.oculus.vrshell`, then stop or disable
   `com.oculus.systemdriver` (VrDriver). Run
   `am broadcast -a com.oculus.vrpowermanager.prox_close`, take the headset off, and repeat
   step 2. Expected result: poses keep updating.
7. Display test: turn the screen off (`input keyevent SLEEP`) and watch step 2 plus
   `logcat -s SensorService TrackingService libsensorscommon`. This tells you whether the TE or
   power gates stop tracking. Then try `svc power stayon true` and a wake.
8. Write a minimal `qb_track` (NDK, `dlopen` + the vtable structs above) that prints head pose
   at 72 Hz. Compare the output with the CLI.
9. In the container or Linux-display configuration, check the `te_timestamp` cadence first,
   then repeat steps 2 to 4.

### Risks

* **system_server is mandatory.** `permission` is fatal if missing (trackingservice asserts).
  `settingsproxy`, `vrpowermanager`, `appops` and `OVRRemoteService` are also needed. A
  zygote-less container would need stub services. Registering a fake `permission`
  (IPermissionController) and `vrpowermanager` is possible but is extra work.
* **Power state.** If the container's PowerManager or display state reads "off", VrPowerManager
  will put tracking in Standby or Sleep and the cameras will stop. Mitigations: `prox_close`,
  stay-awake, or hold a PowerStateLock (AIDL in `oculus-system-services.jar`, not yet decoded).
* **TE dependency.** If the panels are off or in a non-TE mode under Linux, IMU samples are
  dropped and there is no tracking.
* **uid and SELinux.** Focus bypass needs real uid 0. User namespaces break it. The helper's
  SELinux domain must be allowed to `binder_call` trackingserver and `find` `tracking_service`;
  Magisk `su` is fine, a container may need permissive mode. The fallback is
  `ro.debuggable=1` plus `debug.ovr.vrfocus.permissive=1`.
* **ABI fragility.** The vtable slots and struct offsets above are for this exact build. An OTA
  can change them, but versioned factories (`createHeadTracker(8)`) protect you as long as the
  version still exists. The minimum versions are logged ("%s version %lx not supported. Max ...
  min ...").
* **Hand skeleton.** You must call into `libvrapiimpl.so` (APK-embedded) at runtime. Do not ship
  the extracted data.
* **Controller sleep and pairing.** Controllers can be slept by the ActivityMonitor idle logic
  (`persist.ovr.tracking.idletimethreshold`, `sleepdelay`). Pairing state lives in
  system_server.
* trackingservice runs at `ioprio rt 4` in the `tracking` cpuset (CPUs 0-3). Keep Linux
  workloads off CPUs 0-3, or expect tracking jitter.

---

## 9. Evidence index

* Init: `system/etc/init/{trackingservice,calibrationserver,cameramuxmodeservice,vrfocusserver,mrsystemservice,preferencesserver,vrapiserver,runtimeipcbroker}.rc`,
  `system/vendor/etc/init/vendor.oculus.hardware.sensors@1.0-{service,iad}.rc`,
  `system/vendor/bin/syncboss_consumers_ctl`, `system/bin/trackingservice_ctl`.
* Services: `system/etc/selinux/plat_service_contexts`. Linker: `system/etc/ld.config.29.txt`,
  `system/etc/public.libraries-oculus.txt`.
* Key functions (trackingservice):
  `OVR::OS::Tracking::TrackingService::getSharedMemoryFileDescriptor` @0xe174c,
  `MemoryBrokerBase::attachClient` @0xf3afc, `getPackageName` @0xf3cf8 (assert),
  `startAppOps` @0xf4128, `updateFocusedClients` @0xf4660, `MemoryBroker<T>::fd(client)` @0xeda70,
  `TrackingServiceHost::VrFocusListener::onVrFocusChanged` @0xe9aa4,
  `TrackingFidelityService::setCameraMuxMode` @0xdf8e8, `VirtualDevice::Init` (uinput).
* libossdk: `BpTrackingService::*` @0xe6078/0xe6290/0xe63f8 (codes 1/2/3),
  `SharedMemoryRequest::writeToParcel`, `SingletonState::connectInner` @0x8d8e8 (service
  `"tracking"`), `getSharedMemoryFromServiceFor<T>` (types 0/1/2/5/6), `createHeadTracker`
  @0xde650, vtables 0x13c348 / 0x13bc88 / 0x13bee0 / 0x136c88 / 0x136cc0 / 0x136d60.
* vrfocusserver: `isPermissiveFocus` @0x1061c, `FocusPolicy::tryAddingFocusClient` @0x10664
  (uid 0 bypass at 0x10918), `computeFocusState` @0xff60.
* sensors HAL: `MontereyHostTimeSource` @0x54cf8 (te_timestamp),
  `VsyncClock::updateClockDriftFromVsync` @0x81768,
  `SyncbossEventHandler::Session::producerThreadFunc` (drop messages).
* CLI: `trackinginterface_cli` main @0x11a60, head text printer @0x15bec, Button table @0x2e9b0.
* libvrapiimpl (from VrDriver.apk): `vrapi_GetHandSkeleton` @0x4126fc.
