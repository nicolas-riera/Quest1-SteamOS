# Touch controllers via Meta's libsyncboss (no Android)

Static analysis of `system/vendor/lib64/libsyncboss.so` and of
`vendor/bin/hw/vendor.oculus.hardware.sensors@1.0-service` (Quest 1 build 49845030443200410).
Symbols come from `.gnu_debugdata` (see `docs/syncboss-imu.md`). The library's own `__assert2`
strings also give several exact prototypes. This is for personal interoperability use only.
Test program: `src/sbinput/sbinput.c`.

Tags: **[V]** = verified by disassembly, **[I]** = inferred, still to be confirmed on the device.

## 1. TL;DR

* Quest 1 controllers are hardware type **LCON** (`type 2`). They talk to the nRF radio inside
  the SyncBoss MCU (Meta calls the radio "pulsar"). libsyncboss expects LCON app firmware
  **1.17.2** and SPL **1.13.0** (constants inside the library) [V].
* **The pairing list lives on the MCU** and holds at most 6 devices. libsyncboss reads it with
  MCU data register `0x7c`. `unpair` sends MCU command `0xd1`. No host file holds pairing data:
  neither the library nor the HAL has a `/persist` or `/data` path for it [V]. A native process
  only has to:
  1. call `syncboss_init`;
  2. call `syncboss_input_start`, which starts "beacon mode" (MCU command `0x85`);
  3. call `syncboss_wait_on_stream_data_exclusive` continuously.

  Paired controllers then connect when a button wakes them up [V for the code path; I that the
  MCU keeps the list across reboots, because nothing re-sends it].
* The controller data arrives through the **same stream API as the headset IMU**:
  * record type **3**: controller IMU, about 500 Hz, already in m/s² and rad/s;
  * record type **8**: full input state, sent once per radio notification.

  Each record starts with the 64-bit controller id. Left or right comes from
  `syncboss_input_enumerate` (`subtype` 1 or 2) [V].
* All raw-packet decoding, including controller notifications and "devices changed" events,
  runs **inside `syncboss_wait_on_stream_data_exclusive`, in the caller's thread**. That
  function must therefore be pumped continuously, in a dedicated thread [V].
* Haptics: `syncboss_input_set_haptic(h, id, amp 0..255)` writes controller register `0x97`.
  Re-send it while the pulse should last, then send 0 [V/I].
* Firmware updates and pairing only happen when they are explicitly requested
  (`syncboss_input_update_firmware`, work item 6; `syncboss_input_pair`, work item 4). Nothing
  in input start or streaming triggers them [V].

## 2. API (exported C functions)

`syncboss_handle` is `void *`. `syncboss_input_id_t` is a `uint64_t` controller id (logs print
only its upper 32 bits). Unless noted, the return value is 0 or `-errno`.

| Function | Prototype | Semantics |
|---|---|---|
| `syncboss_input_start` | `int (h)` | Fails with -22 unless pulsar state (`h+0x4160`) is 0 = idle. Reads props `vendor.syncbosshal.pulsar_blocklist` / `pulsar_afh`: if they are set, writes MCU data `0x7f` / `0x96`. If they are absent it only logs. Writes MCU data `0xd5 = 0` (auto-whitelisting off). Then **queues** start-beacon-mode (work item 9, MCU cmd `0x85`, timeout 125 ms) and returns 0 at once. State becomes 2 = beacon [V]. |
| `syncboss_input_stop` | `int (h)` | Fails with -22 unless state = 2. Queues stop-beacon-mode [V]. |
| `syncboss_is_input_started` | `int (h, bool *out)` | Reads MCU data `0xd2`. `*out = (byte0 == 2)` [V]. |
| `syncboss_input_enumerate` | `int (h, syncboss_input_device_info_t *out, int *inout_count, uint32_t *generation /*nullable*/)` | `*inout_count` = capacity on input. The function lists the connected devices from the cache, sorted. It then reads the MCU pairing list (data `0x7c`) and appends paired devices that are not connected (`connected = 0`). Returns -12 if more devices are connected than the capacity. `*generation` = device-change counter [V]. |
| `syncboss_input_wait_for_device_change_and_enumerate` | `int (h, uint32_t *generation, uint32_t timeout_ms, info *out, int *inout_count)` | Waits until the generation differs from `*generation`, then enumerates. Returns -110 on timeout and -125 if cancelled (`syncboss_input_cancel_device_wait`). The HAL calls it in a loop with 1000 ms and 6 entries [V]. |
| `syncboss_input_get_device_info` | `int (h, uint64_t id, info *out)` | Connected devices only. Returns -22 if the device is not found [V]. |
| `syncboss_input_set_haptic` | `int (h, uint64_t id, uint8_t amplitude)` | Returns 0 without doing anything if the controller is asleep. Otherwise it does a low-priority queued write of controller register `0x97` = amplitude, with pulsar timeout 4. The write is dropped when more than 24 work items are queued. The HAL passes an `unsigned char` intensity straight through [V]. |
| `syncboss_input_send_haptic_syncbuffer` | `int (h, uint64_t id, syncboss_input_haptic_sample_buffer_t *)` | Buffered haptics (not needed) [V prototype]. |
| `syncboss_input_wake` / `_sleep` | `int (h, uint64_t id)` | Synchronous write of controller register `0x06` = 0 / 1 [V]. |
| `syncboss_input_get_imu_temp` | `int (h, uint64_t id, float *out)` | Reads controller register `0x33` [V]. |
| `syncboss_input_pair` | `int (h, uint64_t id)` | Queues work item 4 (`input_pair`: DMM session, then legacy or ECDH pairing). Refused while wireless is running. **Do not call.** |
| `syncboss_input_unpair` | `int (h, uint64_t id)` | Writes controller register `0x13`, then MCU cmd `0xd1`. **Do not call.** |
| `syncboss_input_update_firmware` | queues work item 6 | **Never call.** |
| `syncboss_set_stream_filter` | `int (h, const int *record_types, int n /*<=16*/)` | Maps record types to MCU packet types, then calls ioctl `SYNCBOSS_SET_STREAMFILTER_IOCTL` (`0x40110a01`) on this handle's stream fd. With no filter, the kernel delivers everything. Filterable types: 0,1,2,3,4,5,6,8,9,14 [V]. Not needed. |
| `syncboss_wireless_device_stats_streaming_set_interval` | `int (h, int)` | The HAL calls it with 1 right before `input_start` (record type 9 stats). Optional [V]. |

### `syncboss_input_device_info_t` (0x148 bytes) [V offsets, I meanings marked ?]

```c
struct syncboss_input_device_info {
    uint64_t id;            /* +0x000 */
    uint8_t  connected;     /* +0x008 */
    uint8_t  asleep;        /* +0x009 cache+29; set_haptic is a no-op when != 0 */
    uint8_t  fw_up_to_date; /* +0x00a */
    uint8_t  _pad;
    uint32_t type;          /* +0x00c 2 LCON, 4 Jedi, 5 Starlet, 6 Ruby, 7 EXT_SYNC, 8 Raven */
    uint32_t subtype;       /* +0x010 0 invalid, 1 left, 2 right, 3 unconfigured, 4 none */
    char desc[64];          /* +0x014 from register device_desc (?) */
    char serial[16];        /* +0x054 assembly_sn ("(no serial)" when empty) */
    char pcb_serial[16];    /* +0x064 pcb_sn */
    char fw_version[64];    /* +0x074 "%i.%i.%i" app fw */
    char fw_expected[64];   /* +0x0b4 "%i.%i.%i" expected by this lib */
    char imu_info[64];      /* +0x0f4 register imu_info (?) */
    float accel_scale;      /* +0x134 g/LSB   (register imu_config) */
    float gyro_scale;       /* +0x138 deg/s/LSB */
    uint32_t gyro_range;    /* +0x13c constant 2000 (dps ?) */
    double battery_percent; /* +0x140 */
};
```

Pulsar type, internal: byte0 = 0x01 (input), byte1 = hardware byte (0x11 = LCON), byte2 = side
code (0 = left, 1 = right) [V].

## 3. Stream records

Record = `struct sb_record { u64 seq; u32 type; u32 pad; u8 data[96]; }` (see syncboss-imu.md).
MCU stream packet dispatch: 0x50 IMU, 0x51 mag, 0x56/0xe0 camera shutter, 0x55 display, 0x92
telemetry, 0x90 pulsar advertisement, **0x8f pulsar data**, 0x93 devices changed, 0xd8 assert,
0xcf prox, 0xd9 wireless stats [V].

| rec type | from | content | HAL route [V] |
|---|---|---|---|
| 0 | 0x50 | headset IMU (`docs/syncboss-imu.md`) | `Imu` |
| 1 / 14 | 0x56 / 0xe0 | camera shutter / v2 (the ~30 Hz "type 14" seen by sbimu) | camera |
| 2 | 0x55 | display frame (vsync), used for clock drift | VsyncClock |
| **3** | 0x8f | **controller IMU** | `ControllerImuData` |
| **8** | 0x8f | **controller input state** | `ButtonData` |
| 9 | 0xd9 | wireless device stats | `WirelessDeviceStats` |
| 11 | 0x8f chunk 11 | 0x30 bytes, unknown; the HAL does not route it | dropped |
| 20 | 0x8f | IMU sample-loss counter `{u64 id; u32 1; u32 missed}` | n/a |
| 24 | 0x8f chunk 8 | ADC stream (only after `syncboss_input_set_adc_stream_enable`) | `ControllerInputADCData` |

Pulsar data is only decoded in beacon mode (state 2). If the controller's app or SPL firmware
differs from the expected version, **all its data is silently dropped**
(`process_beacon_mode_pulsar_data`). Setting the environment variable
`SYNCBOSS_DISABLE_FW_VERSION_CHECK=1` (or prop `persist.vendor.syncbosshal.disable_fw_version_check=true`)
skips this check. It does not trigger any update [V].

### Radio notification (`LCON_process_notification` → `controller_process_notification`) [V]

A pulsar data packet is `{u64 id; u32 pulsar_type; ...; u8 reg @18; payload @19}`, 19 to 71
bytes in total. The payload is at most 52 bytes and is a sequence of chunks. Each chunk has a
`u16` header: the low 5 bits are the chunk type, the next 7 bits are the length.

| chunk | bytes | meaning |
|---|---|---|
| 0 | u8 | battery % (logs the change, triggers device-change) |
| 1 | 18 | IMU sample: `u48 ts_us; i16 accel[3]; i16 gyro[3]`. Gives record 3 (and 20 when gaps occur). Stall if the gap is over 48 ms; missed = (gap+1000)/2000-1 once the gap reaches 2100 µs, so the **nominal period is 2 ms, 500 Hz**. |
| 2 | 4 | thumbstick `i16 x, y` |
| 3 | 3 | trigger/grip: 24-bit LE, low 12 bits = trigger, high 12 bits = grip |
| 4 | 1 | buttons: bit0 A/X, bit1 B/Y, bit2 stick click, bit3 system. Bit4 = IR-LED 60 Hz flag, checked against the camera shutter. |
| 5 | 1 | capsense (LCON): bit0 A/X, bit1 B/Y, bit2 stick, bit3 trigger touch; bits 4/5/7/6 = "prox" for A/X, B/Y, trigger, stick |
| 6 | 1 | state flags: bit0 low_power, bit1 assert, bit2 cap_touch_err, bit3 imu_err, bit4 asleep |
| 7 | 4 | analog capsense bytes: A/X, B/Y, stick, trigger |
| 8 | 10 | gives record 24 (ADC) |
| 9 / 10 / 11 | 16 / 4 / 32 | cached, unknown (11 also gives record 11) |

### Record 3: controller IMU (0x30 bytes) [V]

```c
struct sb_imu_event {        /* identical layout to the headset IMU record (type 0) */
    uint64_t id;             /* +0  controller id (0 for the headset) */
    uint64_t timestamp_us;   /* +8  48-bit µs from the IMU chunk */
    uint32_t aux;            /* +16 0 for controllers */
    float accel[3];          /* +20 m/s²  = raw * accel_scale * 9.80665 */
    float gyro[3];           /* +32 rad/s = raw * gyro_scale  * π/180  */
    uint32_t pad;
};
```

The scales are per controller and come from register `imu_config`, which is cached at
connection time. The axes are passed through unchanged, so the convention still has to be
checked on the device [I].

### Record 8: controller input state (0x40 bytes) [V]

```c
struct sb_ctrl_input {
    uint64_t id;               /* +0 */
    uint64_t timestamp_us;     /* +8  ts of the last IMU chunk received */
    uint8_t  btn_ax, btn_by, btn_sys, btn_stick;                              /* +16..19 */
    uint8_t  touch_ax, touch_by, touch_trigger, touch_stick, touch_thumbrest; /* +20..24 */
    uint8_t  prox_ax,  prox_by,  prox_trigger,  prox_stick,  prox_thumbrest;  /* +25..29 */
    int16_t  cap_ax, cap_by, cap_trigger, cap_stick, cap_thumbrest;           /* +30..39 raw */
    float    stick_x, stick_y; /* +40 i16 / 32767 (>0) or / 32768 (<=0) → -1..1 */
    float    grip_raw;         /* +48 12-bit / 4095; 1.0 = released */
    float    trigger_raw;      /* +52 12-bit / 4095; 1.0 = released */
    uint8_t  battery_percent;  /* +56 (100 until the first battery chunk) */
    uint8_t  _pad[7];
};
```

* **The analog values are inverted.** The HAL forwards `1 - clamp(x, 0, 1)` for grip and
  trigger [V], and libsyncboss reports 1.0 before the first analog chunk arrives. So
  `trigger = 1 - trigger_raw`.
* Until the first chunk of each kind arrives, the fields stay at their defaults: buttons and
  touch bits 0, stick 0, analog 1.0 (released).
* LCON never sets `touch_thumbrest`, `prox_thumbrest` or `cap_thumbrest`. Those fields belong
  to other models [V].
* `btn_sys` is Menu on the left controller and Oculus on the right one [I]. AX/BY are A and B
  on the right controller, X and Y on the left one.
* HAL `ButtonData` bitmasks [V]:
  * buttons: `sys<<0 | ax<<2 | by<<3 | stick<<5`;
  * touch and prox: `ax<<2 | by<<3 | trigger<<4 | stick<<5 | thumbrest<<6`.

  trackingservice then maps these to the OSSDK bits listed in tracking-ipc.md §3.4.
* The stick sign convention (whether up is +y) is unverified [I].

## 4. Pairing and connection

* Pairing state: the MCU's whitelist (`pulsar_whitelist_t { u8 n (<=6); { u64 id; u32
  pulsar_type; ... } [n] }` at 16-byte stride, read with get_data `0x7c`) [V]. The host never
  writes it at start-up. `syncboss_input_start` only writes the blocklist/AFH (if props are
  set) and `0xd5 = 0` (auto-whitelisting disabled, same as Android) [V]. The HAL's only files
  are `/data/vendor/misc/sensors/controllercal/<id>` (calibration cache) and a haptics debug
  CSV [V].
* Connection: the radio only runs in beacon mode. After `input_start`, a paired controller
  connects when a button wakes it up. libsyncboss gets event 0x93, refreshes its cache by
  reading the controller registers (fw versions, imu_config, serials, battery), and bumps the
  generation counter [V]. `enumerate` then reports `connected = 1` and records 3/8 start
  flowing [I: to confirm live].
* New pairing (not needed if Android already paired them): `syncboss_input_pair` with input
  stopped, the controller in pairing mode, and its id from
  `syncboss_input_enumerate_advertising_devices`. Leave it to Android or `pairing_ctl`.

## 5. Time base

* The headset IMU (record 0) and the controller records (3, 8) are in µs. The HAL wraps every
  record in a `StreamPacket` with two extra fields:
  * +112: a clock-drift offset in ns, updated from display records (type 2) against the panel
    TE timestamps in `/sys/bus/spi/devices/spi12.0/control/te_timestamp`;
  * +120: host `CLOCK_MONOTONIC` at receive time.

  It converts **both** headset and controller timestamps as `ts_us*1000 - offset` [V]. They
  therefore share the SyncBoss MCU time base. The radio must keep the controller clocks synced
  to it, since the stall and loss logic compares 48-bit controller timestamps at 2 ms
  resolution [I].
* Natively there is no TE path. Estimate `offset = min(host_mono_us - ts_us)` over the 1 kHz
  headset IMU and apply it to the controller records too. `sbinput --headset-imu` prints this
  minimum for both kinds of source so the shared time base can be checked live.

## 6. Native call sequence

```
android_dlopen("libsyncboss.so")                       # LD_PRELOAD=libbionictls.so
syncboss_init(&h, opts = {0,0,1,1, 0...})               # telemetry off
thread: loop syncboss_wait_on_stream_data_exclusive(h, 100, &rec)   # never stop pumping
        type 3 -> IMU, type 8 -> input, type 0x14 -> loss counter
[optional] syncboss_imu_enable(h)                       # headset IMU, for time-base offset
syncboss_input_start(h)                                 # beacon mode (async)
poll syncboss_is_input_started(h, &b) until b           # ~100 ms
loop: syncboss_input_wait_for_device_change_and_enumerate(h, &gen, 1000, info, &n)
      -> id -> left/right (subtype), connected
haptic: syncboss_input_set_haptic(h, id, amp) every <=50 ms, then (h, id, 0)
exit:   set_haptic 0; syncboss_input_stop(h); keep pumping ~300 ms; stop thread; syncboss_deinit(h)
```

Prerequisites are the same as for `sbimu`: system_a mounted read-only, `/system` binds, the
firmware path set, and the property area. No Android daemon is needed for this path:
`syncboss_get_str_prop` failures are harmless [I].

## 7. To verify on the device (`sbinput 30 0.5 --haptic --headset-imu`)

1. Enumeration lists 2 paired LCON devices (left and right) before any button press.
2. After a button press: `connected`, record 3 at about 500 Hz and record 8 per notification.
   `fw` should be 1.17.2. If it is not, the data is dropped: retry with `--no-fw-check`.
3. Button, touch and analog mapping. Check the trigger inversion and the stick sign.
4. Haptic pulse is felt; it stops when 0 is sent.
5. `min(host-ts)` is about equal for the headset and the controllers (shared time base).
6. Controller IMU axis convention, for Monado 3DoF.
