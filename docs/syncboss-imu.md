# Headset IMU via Meta's libsyncboss (no Android)

Static analysis of `system/vendor/lib64/libsyncboss.so` (Quest 1 build 49845030443200410).
Local symbols come from its `.gnu_debugdata` section (xz-compressed ELF, extracted by offset;
`objcopy -O binary` returns nothing for this non-alloc section). Personal interoperability use only.

## Device path

SyncBoss is the MCU behind the IMU, the controller radio (nRF) and camera sync. The kernel driver
(`drivers/staging/oculus/mcu/syncboss`, uapi `drivers/staging/oculus/include/uapi/linux/syncboss.h`)
only transports SPI packets: `/dev/syncboss0` (commands), `/dev/syncboss_stream0` (data),
`/dev/syncboss_control0`, `/dev/syncboss_powerstate0`. The stream is silent until the IMU is enabled
by a command, which is what libsyncboss does.

## API (all C, exported)

| Function | Prototype (recovered) | Notes |
|---|---|---|
| `syncboss_init` | `int (syncboss_handle *out, const syncboss_init_options_t *opts)` | `opts = NULL` → library defaults. Options are 16 bytes: byte 2 must be non-zero ("old interface"), +8 = log handler. Starts the stream thread, waits ≤1 s for the MCU, checks the firmware version, starts the pulsar (radio) manager. Refcounted. |
| `syncboss_imu_enable` | `int (syncboss_handle h)` | Sends command 0x6e (110) and waits ≤125 ms for the reply; non-zero = error. |
| `syncboss_imu_disable` | `int (syncboss_handle h)` | Stages command 0x6f (111). |
| `syncboss_wait_on_stream_data_exclusive` | `int (syncboss_handle h, uint32_t timeout_ms, struct sb_record *out)` | 0 = one record copied; -11 (`-EAGAIN`) = timeout. Pops from a 32-entry ring, so read faster than the IMU rate. |
| `syncboss_set_stream_filter` | `int (syncboss_handle h, ...)` | Not needed for the IMU test. |

## Stream record (112 bytes)

```c
struct sb_record {          /* add_stream_data_to_history() */
    uint64_t seq;           /* +0  monotonically increasing */
    uint32_t type;          /* +8  0 = headset IMU, 0x0c = double-tap, ... */
    uint32_t _pad;
    uint8_t  data[96];      /* +16 type-specific */
};

struct sb_imu_event {       /* type 0, 0x30 bytes, built by process_imu_data() */
    uint64_t _zero;         /* +0 */
    uint64_t timestamp;     /* +8  raw IMU timestamp from the firmware packet (u64 at +0 of the raw sample) */
    uint32_t aux;           /* +16 u32 at +32 of the raw sample (probably temperature) */
    float    accel[3];      /* +20 m/s²  (raw g × 9.80665) */
    float    gyro[3];       /* +32 rad/s (raw °/s × π/180) */
    uint32_t _pad;          /* +44 */
};
```

Raw firmware sample (input of `process_imu_data`): `u64 ts; float accel_g[3]; float gyro_dps[3]; u32 aux`.
Both scale constants are positive (9.80665f at 0x2cc7c, π/180 at 0x2cc80): axes are passed through
unchanged, so the axis convention still has to be checked on the device.

## Live result (native Holo, 2026-10-05) — works

`src/sbimu/sbimu.c` (glibc + libhybris `android_dlopen("libsyncboss.so")`, `LD_PRELOAD=libbionictls.so`)
reads the headset IMU with **no Android running**:

```
syncboss_init -> 0 / syncboss_imu_enable -> 0
accel=[ -9.950 -0.093 0.285] |a|=9.955   gyro=[-0.0117 0.0107 -0.0160]   (headset at rest)
imu samples: 4988 in 5.00 s (998 Hz), timeouts 0;  record type 14: ~30 Hz (unknown yet)
```

- `timestamp` is in µs; `aux` is a float (≈34 → temperature in °C).
- Gyro bias at rest ≈ 0.01 rad/s → needs bias estimation in the fusion.

Prerequisites on the native system:
- `/dev/sda6` (system_a) mounted **read-only** (`-o ro,noload`) on `/android`; `/system` = bind of
  `/android/system`; `/apex/<name>` = binds of `/android/system/apex/<name>[.release]`.
- Firmware loader: `echo -n /system/vendor/firmware > /sys/module/firmware_class/parameters/path`
  and `echo 2 > /sys/class/firmware/timeout`. The driver asks for absent prox calibration files
  (`PROX_PS_*`); without this it stalls 4 × the fallback timeout on open.
- Init options `{0,0,1,1}`: byte 3 disables telemetry. With telemetry on (the default), `syncboss_init`
  spins forever in libbase `WaitForProperty` (libtelemetry waits for Android's hwservicemanager).
