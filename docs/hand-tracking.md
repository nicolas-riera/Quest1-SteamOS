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

`/tmp/quest1-hands` (SteamVR setting) or `/etc/quest1-hands`: `off` (default, preference never touched), `auto`, `hands`. In `auto` q1handsd sets
`hand_tracking_enabled` true when no controller is in use (no button/trigger/grip, not in hand by libossdk's own
detection, 3 s) or none is connected, and false as soon as a controller is picked up (200 ms). A 3 s hold-off avoids
flapping. Switching the cameras takes ~1-2 s, as on Meta's OS. In hand mode the Touch devices carry the tracked hands.

### The switch: SteamVR settings, Oculus Quest page

"Hand tracking" (`driver_quest1/handTracking`, off by default): the driver writes `auto` or `off` to `/tmp/quest1-hands`
and q1handsd follows it (the file replaces `/etc/quest1-hands` as the normal switch; that one stays as a debug override,
`hands` = hands only). There is **no "Multimodal (hands + controllers)" toggle on purpose**: Quest 1's camera mux has no
mode with hand and controller frames at once (section 2), and the multimodal preference changes nothing. The
setting that does exist already gives the nearest thing, hands when the controllers are down and controllers when picked up.

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

## 6. Steam Link VR (checked in the binaries, 2026-11-17)

* The arm64 `vrlink` client (`/opt/steamvr/tools/vrlink/bin/linuxarm64/vrlink`) contains **no** string or protobuf
  field name about hands, skeletons, joints, bones, fingers (other than `capsense/*`), pinch or gestures. Its tracking
  upload is `PackControllerInput` plus the head pose; the input paths it knows are the Frame controller's
  (`/user/hand/<side>/input/...`: system, menu, a/b/x/y, trigger, grip, thumbstick, thumbrest, bumper, dpad, view,
  `capsense/{grip,trigger,ring,pinky,bumper}`, `handgrip1/2/touch`). So **no skeleton or joint data goes from this
  client to the PC**. **[V]** (absence of any code path/strings; it reads controllers only when their type is
  `frame_controller`, docs/streaming.md).
* The PC side (`driver_vrlink.dll`) has `SVLHandTrackingDriver`, an `svl_hand_interaction_augmented` device with
  Index-style `index/middle/ring/pinky_pinch` inputs, `enableHandTracking` and `HandAnimEvaluator`
  (`hand_right_closeanim.glb`): it is fed by the hand data of Valve's own Android/Frame clients, over a message our
  client does not have. **[V]** strings; the message format is not known.
* What works today over the stream: the Touch device the PC sees carries our gestures (pinch with the palm toward you
  = system, pinch in the dashboard = trigger) and the hand pose as the controller pose, so the dashboard and
  controller-style games work with hands.
* What would be needed for joints/skeleton on the PC: (a) reverse the hand message that `driver_vrlink.dll` parses
  (SVLHandTrackingDriver) and the client packing in the Android Steam Link APK, then send it from the shim
  (`src/v4l2shim/quest1_vrlink.c`); or (b) cheaper, partial: send finger curls through the Frame controller's
  `capsense/grip|trigger|ring|pinky` paths from Meta's joints, so the PC's controller skeleton curls the fingers
  (the thumb has no capsense path except thumbrest/touch).

## 7. Visualisation

`src/q1handviz` (`/usr/local/bin/q1-handviz`, unit from `device/holo-hands.sh`): a SteamVR overlay per tracked hand
with the 24 joints drawn as a skeleton seen from the palm side, attached to the hand's tracked device, and, while the
palm faces the user, the Oculus mark (right: ring) or Menu mark (left: bars) 7 cm in front of the palm (yellow while
pinched). driver_quest1 publishes the joints and flags in `/run/q1track/handviz` (made by q1handsd, world-writable
tmpfs file). Nothing is drawn while no hand is tracked. Untested on the headset (it was off SteamOS).

## 8. Worn test and open items

`tools/headset-dev/hands-worn-test.sh on|log|off` and its header: the steps for the user.
* Check with real hands: the status values of a tracked hand, the root pose space, the retargeting (fingers, wrist twist),
  gesture thresholds, the overlay placement (skeleton picture offset, mark position).
* Deploy: image build runs `holo-hands.sh install`, the `android-q1handsd` unit comes from `holo-android-tracking.sh`.
  `tools/headset-dev/hands-fake.py` tests the driver without wearing the headset (it rewrites the file, so use
  q1handsd's in-place writes for long runs).
