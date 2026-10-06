# Quest 1 (monterey) lens distortion, FOV and lens geometry

This was an offline analysis of the owner's own dumps, with nothing run on the headset. Sources:
- `system/priv-app/VrDriver/VrDriver.apk` → `lib/arm64-v8a/libvrruntimeservice.so` (sha256 `2a0a4472…bb48`, build `49845030443200410`). The extracted copy is at WSL `~/q1/re/vrdriver/`.
- `system/etc/calibration/distortion-mesh.bin` (sha256 `ee995926…5bd5`).
- The Oculus `/persist` filesystem is the GPT partition **`private`**, not `persist`. `fstab.monterey` mounts `by-name/private` on `/persist`. It was carved from `sda_0-userdata.bin` (4 KiB sectors, `private` = LBA 1319432, 16384 sectors) into WSL `~/q1/lens/private.img` and dumped to `~/q1/lens/x_private/` (read-only copies; the backup was not touched). These copies contain serial numbers, so keep them local.

## TL;DR: numbers to put in Monado

| Item | Value | Source (libvrruntimeservice.so, file offset = VA) |
|---|---|---|
| Distortion model | Oculus SDK `LensConfig`, `Eqn = 2` = **CatmullRom10** | `str w10=2,[x19,#24]` @0x2d1234 in the HMD-config function 0x2d1030 |
| K[0..10] | **1.0, 1.0374, 1.0810, 1.1330, 1.1970, 1.2754, 1.3771, 1.5133, 1.7018, 1.9732, 2.3000** | rodata 0x1413b8 (11 × f32) |
| MaxR | **1.0** | rodata 0xd6900[0] |
| MetersPerTanAngleAtCenter | **0.0389 m** | rodata 0xd6900[1] (overridable by `debug.oculus.metersPerTanAngCtr`, which writes this field) |
| Chromatic, red | scale_R = **1 − 0.004 − 0.007·rsq** (+0·rsq² +0·rsq³) | rodata 0xd6900[2..3], 0xd55b0[0..1] |
| Chromatic, blue | scale_B = **1 + 0.006 + 0.020·rsq** (+0·rsq² +0·rsq³) | rodata 0xd55b0[2..3], zeros stored at +144/+148 |
| Panel (whole) | 2880 × 1600 px, **0.1188 × 0.0660 m** | 0xd6eb8 (ints), 0xd6dd8 |
| Panel per eye | 1440 × 1600 px, **0.0594 × 0.0660 m** (pixel pitch 41.25 µm) | 0xd7188 (ints), 0xd7078 |
| Lens→screen offset, left eye | (x, y, z) = **(−0.002986, −0.002720, 0.043922) m** | 0xd6f90, z = imm `0x3d33e78e` @0x2d1190 |
| Lens→screen offset, right eye | **(+0.002986, −0.002720, 0.043922) m** | 0xd7190, same z |
| FOV, left eye (deg) | **up 47, down 53, left (outer) 52, right (inner) 42** | rodata 0xd6160 = (47,53,52,42) |
| FOV, right eye (deg) | **up 47, down 53, left (inner) 42, right (outer) 52** | rodata 0xd5120 = (47,53,42,52) |
| Recommended eye buffer | 1216 × 1344 | 0xd70b0 |
| Default lens positions | x = ∓0.03175 m (63.5 mm separation), y = z = 0 | imm `0xbd020c4a`/`0x3d020c4a` @0x2d1250, passed to SetLensPositions 0x2cef84 |
| Screen orientation | identity quaternion for both screens, clocking 0, no tilt, verticalFlip off | Quest‑1 branch @0x2d111c–0x2d1278 |

Confidence is **high** for the model, coefficients, panel geometry, lens offsets and FOV. `/system/etc/calibration/distortion-mesh.bin` was regenerated from exactly these parameters, and every vertex of both eyes for G, R and B matched with a maximum error of 4e-5 tan units (at the extreme panel corners) and a median of 1e-7 (script: scratchpad `lens/verify.py`).

**These are not per-device lens numbers.** They are the runtime's hardcoded Quest 1 defaults. They are also the values the runtime falls back to when the headset type is unknown: the log string there is "Invalid headset type. Configuring default Quest 1 distortion", and that branch @0x2d1df0 loads the same constants. The device's `/persist` has no distortion file. The only per-unit optical data is the display decenter in `screen_offset.json` (below). The shipped `distortion-mesh.bin` is generic: its header says deviceModel 0x101 / headset 0x103.

## The model: radius → scale

This is the Oculus PC SDK 0.4+ `LensConfig::DistortionFnScaleRadiusSquared` with CatmullRom10. It was verified instruction by instruction: the mesh builder is at 0x2cde74 and the spline is `2cea48`.

For one eye, take a point P on that eye's panel in metres, in a frame with x to the right and y up, relative to the **panel centre**:
```
lensToScreen = (-0.002986, -0.00272) left eye, (+0.002986, -0.00272) right eye   # panelCentre - lensCentre
x = (P.x + lensToScreen.x) / 0.0389          # "tan-angle units" at lens centre
y = (P.y + lensToScreen.y) / 0.0389
rsq = x*x + y*y
s   = CR10(rsq)                               # scale factor, ≥1 (pincushion compensation)
tanG = (x*s, y*s)                             # tangent of view angle for green
tanR = tanG * (0.996 - 0.007*rsq)
tanB = tanG * (1.006 + 0.020*rsq)
```
CR10 (exactly as in the binary; MaxR = 1, so scaled = 10·rsq):
```
f = 10*rsq/(MaxR*MaxR); k = clamp(floor(f),0,10); t = f-k
k==0 : p0=1,     m0=K1-K0,         p1=K1,  m1=0.5*(K2-K0)
1..8 : p0=K[k],  m0=0.5*(K[k+1]-K[k-1]), p1=K[k+1], m1=0.5*(K[k+2]-K[k])
k==9 : p0=K9,    m0=0.5*(K10-K8),  p1=K10, m1=K10-K9      # SDK uses 0.5*(K10-K9) for m0; binary uses central diff
k==10: p0=K10,   m0=K10-K9,        p1=p0+m0, m1=m0          # linear extrapolation
omt=1-t
CR10 = (p0*(1+2t) + m0*t)*omt*omt + (p1*(1+2*omt) - m1*omt)*t*t
```
Units: x, y and rsq are in "metres / MetersPerTanAngleAtCenter", which is tan-angle units at the lens centre. The output is the tangent of the angle off the lens axis. This direction runs **screen → tan-angle**, which is what a distortion mesh needs: for each display pixel or vertex it gives the direction to sample from the eye render. Sample the eye texture at:
`u = (tanX - tan(fovLeft)) / (tan(fovRight) - tan(fovLeft))`, `v = (tan(fovUp) - tanY) / (tan(fovUp) - tan(fovDown))`, with fovLeft and fovDown negative and v running top to bottom. Use tanR, tanG and tanB for the three UV sets.

Sanity values for the left eye, middle row of the mesh: the outer panel edge is at tan −1.287 (52.1°) and the inner edge at +0.862 (40.8°). The middle column runs from tan −1.677 (59°, bottom) to +1.083 (47.3°, top). The panel-centre vertex is at tan (−0.077, −0.070), which equals lensToScreen/0.0389.

### distortion-mesh.bin format (what BuildDistortionBuffer writes)

The magic `0x56347807` is written at 0x2ce21c. All fields are little-endian:
```
0x00 u64 magic 0x56347807 | 0x08 u64 deviceModel | 0x10 u64 headsetModel
0x18 i32 meshW (32) | i32 meshH (32)
0x20 f32 lensSeparationMeters (file: 0.053428), horizOffsetMeters (0) | 0x28 f32 displayW_m, displayH_m
0x30 i32 displayW_px, displayH_px | 0x38 i32 eyeTexW, eyeTexH
0x40 f32 leftFov[up,down,left,right] (deg) | 0x50 f32 rightFov[...]
0x60 vertices: for j in 0..32 (row), for eye in 0..1, for i in 0..32: 6 f32 = tanR.xy, tanG.xy, tanB.xy
```
Vertex (i, j) sits at P = ((i/32 − 0.5)·0.0594, (j/32 − 0.5)·0.066) on that eye's panel. Row j = 0 is the −y side, which is the "down" side (53°). The current runtime writes 10 floats per vertex (it adds 4 Jacobian terms for ASW), while the shipped file uses the older 6-float layout. You can use this file directly as a Monado mesh (`XRT_DISTORTION_MODEL_MESHUV`) after converting tan→UV with the FOV above, or regenerate it from the formula, which gives the same result.

## Per-device calibration found in /persist (`private` partition)

`/persist/calibration/display/{left,right}/{left,right}_screen_offset.json`:
```
left : decenter [ 2.47272727, -4.86266090] px, decenter_mm [0.102, -0.145], roll 0.02,  source AUTO, auto_decenter [2.4727, -3.5152]
right: decenter [ 3.85454545, -4.78582395] px, decenter_mm [0.159, -0.253], roll 0.056, source AUTO, auto_decenter [3.8545, -6.1333]
```
(`decenter_mm` = `auto_decenter` × 41.25 µm.)

How the runtime uses this file: CompositorVR, function 0x234d74 in libvrruntimeservice.so, logs `EYE_DECENTER`. It requires `source == "AUTO"`, reads `decenter`, and **truncates it to integer display pixels**. That gives left H = 2, V = −4 and right H = 3, V = −4, stored at CompositorVR+1412/1416/1420/1424 as a per-eye whole-pixel image shift. `debug.oculus.hOffsetPixelsLeft` and the related properties override these values. The `ipd_min/mid/max` keys are for other headsets and are absent here. `roll` is not read by this reader.

I did not trace the sign of the shift. If you want to include it in Monado, add `decenter_px × 41.25e-6 m` to lensToScreen for each eye, then confirm the sign visually. It is at most about 0.2 mm, roughly a 0.3° shift.

Also present but not relevant to the lens: `*.mura`, `*.mura.astc` and `*.uniformity`, which are per-panel OLED mura/uniformity correction. Panel serials are PANEL-SERIAL-L (left) and PANEL-SERIAL-R (right).

## IPD / IAD (physical slider)

- Sensor: the PM8998 VADC AMUX channel 0x17 (downstream DT `vs1-dvt.dtsi`, `label = "ipd_sensor"`, absolute calibration, 1:1). The sysfs path is `/sys/devices/soc/800f000.qcom,spmi/spmi-0/spmi0-00/800f000.qcom,spmi:qcom,pm8998@0:vadc@3100/ipd_sensor` and its content is `Result:<µV>`.
- Converter: `/vendor/bin/hw/vendor.oculus.hardware.sensors@1.0-iad`, `MontereyIadSensorHandler::getIad` @0x2aa8, with the constructor @0x2624 (symbols come from .gnu_debugdata):
  ```
  V   = raw_uV * 1e-6
  r   = 9.1 * V / (3.3 - V)                    # same transform applied to calib endpoints (mV*1e-3)
  r   = clamp(r, rMin, rMax),  rMin/rMax from /persist/IPD_MIN, IPD_MAX (mV)
  IAD = IAD_MIN + (IAD_MAX - IAD_MIN)*(r - rMin)/(rMax - rMin) + 0.0546    [metres]
  ```
  This unit's values: `IPD_MIN = 69`, `IPD_MAX = 1543` (mV), `IAD_MIN = 3.916`, `IAD_MAX = 17.2` (mm, converted to m). The resulting lens-centre separation is **58.5 mm to 71.8 mm**.
- On Quest 1 each lens moves together with its own panel, so lensToScreen and the mesh do not depend on IPD. The IAD only changes the render eye positions: in the runtime, `SetLensPositions(-iad/2, +iad/2)` sets eye translation = lensToScreen + lens position. Without the sensor, the default is 63.5 mm. For Monado, set the eye separation from IAD, or default to 0.0635. This conclusion is inferred from the code structure (medium confidence). The exact IAD → SetLensPositions call site was not traced.

## Other notes

- Header model IDs: 0x100–0x103 take the Quest‑1 branch (`(w & ~3) == 0x100`) of the config function 0x2d1030. The `monterey` / `monterey_proto1` strings are mapped in 0x3184a8.
- Debug properties that override these fields (useful for tuning without a rebuild): `debug.oculus.distK`, `distK11`, `distRedCAC`, `distBlueCAC`, `metersPerTanAngCtr`, `maxR`, `leftScreenCtr`/`rightScreenCtr`, `lensSepMeters`, `eyeFovUp/Down/Inward/Outward`, `displayMetersW/H`, `verticalFlip`, `distCurve` (CatmullRom10 | RTech16).
- Confidence summary:
  - High: the coefficients, model, FOV, panel size and lens offsets (verified against the shipped mesh).
  - Medium: IAD semantics in the runtime, the decenter sign, and the absolute up/down orientation of the panel rows (mesh row 0 = the "down" side; confirm with a test pattern on the device).
