# Hand tracking (Meta's, in SteamVR)

Status 2026-11-17: the pipeline is written and the parts that can be tested without wearing the headset are verified.
Nothing was tested with a real hand in view yet (the headset was not worn). **[V]** = verified on the headset or in
the binaries, **[I]** = inferred.

## 1. How Meta's OS v50 does hands

* Hand tracking runs inside `trackingservice` (`HandTrackingGlue`, models in `libtrackingengines.so`, Hexagon DSP).
  It publishes through `TrackingServiceHost::updateHandTrackingPose` into the Hand shared-memory region. Clients read
  it with libossdk `createHands(6)`: vtable[0] `getState(ts*)` (0xCE0 bytes, two 0x248 records, layout in
  `docs/tracking-ipc.md` 3.5), vtable[2] `getHandScale`, vtable[3] `getHandTrackingEnabled`. **[V]**
* Switch: the preference `hand_tracking_enabled` (`oculuspreferences --setc ...`, store in settingsserver).
  `TrackingFidelityService::setCameraMuxMode()` reacts at once: `cameramuxmodeservice` goes from mode `default` to
  `HandTracking` (verified with `dumpsys cameramuxmodeservice`; reverts when the preference is false). **[V]**
* Gatekeepers: trackingservice reads GKs (`oculus_mobile_hand_autotransition_v2`, killswitch strings in the runtime) but
  `hand_tracking_enabled` alone is enough here, no GK blocked it. **[V]**
* The UX layer (auto transition hands/controllers, double tap, "controller in hand" logic, the system gesture, toasts)
  is in `VrDriver.apk` (`libvrruntimeservice.so`: "HANDTRACKING going to hand mode since no controller detected in
  hand", "should fire system button for hand"), not in trackingservice. We do not run it, q1handsd replaces it. **[V]**

## 2. Multimodal (hands and controllers at once): no

`/system/etc/cameramuxmode/config.json` has three modes only. `default`: HEADSET + CONTROLLER frames (constellation
tracking for the controllers). `HandTracking` and `HandTrackingUseIOT`: HEADSET + HAND frames, no CONTROLLER frames.
The camera frames alternate by type, so the cameras serve one or the other. The strings `multimodal_hands_and_controllers_enabled`
and "Attempt made to enable multimodal hands+controllers when not supported by app" exist, but setting that
preference changes nothing (mode stays `HandTracking`, checked) **[V]**; the controllers are then only on their IMU
(3DoF-ish) and trackingservice puts them to sleep ("Controller sent to sleep as hand tracking is enabled and
controllers are not in use"). So like Meta's OS on Quest 1: **switch**. q1handsd does it (section 4).

## 3. Data path

```
trackingservice --libossdk--> q1handsd (bionic) --/run/q1track/hands--> driver_quest1 (hands.cpp, controllers.cpp)
                                                                          |--> SteamVR skeleton + pose of the two Touch devices
```

* `src/q1track/q1handsd.c`, `q1hands_shm.h`: seqlock shared memory (same scheme as q1trackd): the 2 hands (root pose,
  19 bone rotations, confidences), the head pose of the same instant, controller in-hand/connected flags, hand scale,
  Meta's bind skeleton. State is read only while `getHandTrackingEnabled` is 1: the state buffer is stack garbage
  otherwise and libossdk's Optional flag is unreliable. **[V]**
* The bind skeleton (24 bones: 19 + 5 tips, parents, local pose) is Meta's static data: `vrapi_GetHandSkeleton` in
  `libvrapiimpl.so` of the system's `VrDriver.apk`. `device/q1-vrapi-extract.sh` unpacks the libraries to a tmpfs
  (nothing stored or committed), `q1handsd skeleton` calls the function once (it ignores its session argument) and
  writes `/run/q1track/handskel.bin`. Layout: 8-word header, 24 x (quat xyzw, pos xyz), 24 int16 parents. **[V]**
  The bone rotations from libossdk are absolute local rotations (an untracked hand is the bind pose). **[I]**
* `q1hands_fk.h`: forward kinematics (joint world poses, palm normal, thumb-index distance).

## 4. Hands vs controllers

`/etc/quest1-hands`: `off` (default, preference never touched), `auto`, `hands`. In `auto` q1handsd sets
`hand_tracking_enabled` true when no controller is in use (no button/trigger/grip, not in hand by libossdk's own
detection, 3 s) or none is connected, and false as soon as a controller is picked up (200 ms). A 3 s hold-off avoids
flapping. Switching the cameras takes ~1-2 s, as on Meta's OS. In hand mode the Touch devices carry the tracked hands.

## 5. SteamVR side (driver_quest1)

* Each Touch device gets a skeleton component `/input/skeleton/left|right` (`/skeleton/hand/left|right`, base pose
  `/pose/raw`, full tracking level, 31 bones). With controllers: a hand holding the controller, fingers curled from
  trigger, grip and thumb touches. With a tracked hand: Meta's joints retargeted onto Valve's 31-bone skeleton
  (`/opt/steamvr/resources/skeletons/vr_glove_*_skeleton.glb`, read at run time): the frame change is derived from the
  two bind poses (wrist, index/middle/pinky knuckles), so no hard-coded axes. Index/middle/ring metacarpals follow the
  wrist, aux bones sit at the tips. The profile `quest1_touch_profile.json` lists the skeleton sources.
* The tracked hand is the device: pose = wrist, raw (pointer) frame = pinch point on the ray from the shoulder, so the
  laser works in the dashboard. No render model while the hand is the device (the skeleton is drawn by games; the
  built-in hand visualisation of SteamVR/vrcompositor uses the skeleton too **[I]**).
* Gestures: pinch (thumb-index tip < 21 mm, release 34 mm) with the palm facing the head = system button (Oculus/Menu),
  otherwise trigger, only while the dashboard is open (`VREvent_DashboardActivated`). **[I]** thresholds untested.

## 6. Steam Link VR

The arm64 `vrlink` client has no hand/skeleton strings, only controller inputs (including capsense finger curls); the
PC `driver_vrlink.dll` has `SVLHandTrackingDriver`, which takes data from the Android client. So the joints most likely
do not reach the PC; the Touch inputs (system, trigger gesture, poses) do. **[I]** Not tested.

## 7. Open

* Worn test: `echo auto > /etc/quest1-hands`, check the status values of a tracked hand, root pose space, retargeting,
  gestures. Debug: `q1handsd dump`, driver log lines "quest1: <side> hand tracked".
* Menu icon near the palm and a joint overlay (SteamVR overlay attached to the hand): not done.
* Deployment: unit `android-q1handsd` in `device/holo-android-tracking.sh` (needs `holo-android-tracking setup` and an
  image build), `tools/headset-dev/hands-fake.py` to test the driver without wearing the headset.
