# Floor, playspace and boundary

## How the Steam Frame's room setup works (SteamVR 2.17)

The in-headset setup is not a separate app: it is the dashboard web UI (`vrwebhelper`, page `systemui`,
`/opt/steamvr/resources/webinterface/dashboard/*.js`) driving `vrcompositor`, which draws the floor grid,
the laser/brush and the boundary, through `IVRChaperoneSetup`.

- **Playspace menu** (bar): Recenter, Set up playspace, Adjust floor height, Clear playspace.
  Shown when the HMD property `Prop_SupportsInHMDRoomSetup_Bool` (11003) is true.
- The buttons call `ConstructStore.setRoomSetupStep(step)`. That function refuses every step but 0 and 1
  ("HMD doesnt support setRoomSetupStep") unless the HMD property
  `Prop_VRLinkClientHMDSupportsRoomSetupRequests_Bool` (11004) is true (or the dev setting
  `/settings/steamvr/inHMDRoomSetupDebug`, which is the beta/debug path: do not use it).
  `driver_quest1` sets both 11003 and 11004. With 11003 only, the menu showed but nothing happened.
- Steps: 1 recenter countdown, 2 adjust floor height, 7 full setup (touch the floor with a controller, draw
  the boundary with the controller's laser/trigger, Accept -> `CommitPlayspaceFull`), 3/4/5 older
  floor + circle / drawn variants. Accept/Back/Floor fine adjust also exist as optional input actions
  (`/actions/roomsetup`); the Touch profile has no binding for them, so the on-screen buttons are used.
- Result: `~steamos/.local/share/Steam/config/chaperone_info.vrchap` (universe, standing transform, bounds),
  `VREvent_ChaperoneRoomSetupCommitted`. "Clear playspace" calls `ClearRoomSetup`: that is the
  "delete all the mapping" button.
- No depth or camera is needed: the floor comes from the controller touching it, the boundary from the
  controller's laser projected on the floor.

## What the Adreno 540 needs (compat layer, `src/vklayer-steamvr/quest1_compat.c`)

The room setup works on the Quest 1 (user-tested 2026-11-05) with these fixes:

- **Default universe.** With no `chaperone_info.vrchap` (first start, or after Clear playspace) vrserver had
  no standing pose: no floor, no brush, Accept committed nothing. `driver_quest1` provides a default
  universe (`Prop_DriverProvidedChaperoneJson_String`, standing = raw).
- **Empty SDF texture.** vrcompositor sizes its "Playspace area SDF" texture from the boundary: 0x0 before
  one exists. The Adreno driver cannot allocate 0 bytes, CreateTexture failed every frame (~3 fps): empty
  images are made 1x1.
- **32-bit indices bound as 16-bit.** The setup's meshes (circle, drawn outline, drawing area) are uploaded
  with 32-bit indices (vkCmdUpdateBuffer for small ones, vkCmdCopyBuffer from a mapped staging buffer for
  big ones) and bound with `VK_INDEX_TYPE_UINT16` and the 32-bit count: every other index read as 0, a fan
  of rays from one floor point. Buffers filled with 32-bit shaped indices are bound as `UINT32`.
- **SDF texture without SAMPLED usage.** The drawing area is a grid of 64x64 blocks (`CBlockedSDFGrid`)
  whose signed distances vrcompositor writes into a 512x512 linear R8 image through a mapping and samples,
  but creates with TRANSFER usage only. The Adreno driver then samples 0 ("inside") everywhere: whole purple
  squares instead of the drawn shape. Linear R8 images get SAMPLED usage. `src/vklayer-steamvr/sdftest.c`
  reproduces it outside SteamVR (without SAMPLED 261120 of 262144 texels read wrong, with it none).
- The floor/boundary vertex shader samples bindings its pipeline layout lacks; the layer's pipeline retry
  turns those reads into 0 (once per setup start).

Debug notes: SPIR-V/pipeline logs in `/tmp/quest1-compat-vrcompositor.log`; `tools/headset-dev/sg-hide.sh`
removes one scene graph node live. Adding hooks for vkCreateImageView and vkCmdCopyImage to the layer made
vrcompositor's whole output black (not investigated): keep debug hooks off those.

## Raw space and persistence (`chaperone_frame.cpp`)

Meta's tracking has no persistent map here (`/vision/insideout/mapdb`, `/data/misc/tracking` stay empty,
`trackinginterface_cli` has no floor or map command): its world origin is where the headset was when the
stack started, so a fixed "eye height" offset put the floor at a random height after each boot.

The driver now defines SteamVR's raw tracking space itself in 6DoF (meta) mode. At start-up, and each time
the headset is put on after at least 20 s off (panels on, `/tmp/quest1-panels-off` absent), it anchors the
raw space on the head: origin below the head, head at `QUEST1_EYE_HEIGHT` (1.65 m) above the raw floor,
raw -z = where the head looks. The floor and boundary saved in `chaperone_info.vrchap` are expressed in
that space, so they are valid after a reboot as long as the headset is put on at the same spot and facing
as when the room was set up, with the same posture (use the Playspace menu: Adjust floor height to fix
the floor, Set up to redo it). In 3DoF mode nothing is anchored (y = eye height). Disable with
`touch /tmp/quest1-no-anchor`.

Not possible without a Meta map: a boundary fixed in the real room that survives arbitrary put-on positions.

## Debugging the dashboard UI

`touch /tmp/q1-webdebug` (as steamos) before the dashboard starts makes `vrwebhelper.sh` enable CEF
remote debugging on 127.0.0.1:9333; `/root/cdp.py 'js'` evaluates JS in the systemui page
(`ConstructStore.roomSetupStep`, `VRHTML.VRChaperoneSetup.*`).

## Meta's tracking map persistence (q1anchord)

The tracker ("SuperVega") has no whole-map API (`keepMap`/`listMaps`: "API ... is not supported"), but it
persists maps through spatial anchors, like Guardian does. Binder service `TrackingEnvironment`
(`oculus.internal.ITrackingEnvironment`), transaction codes (call with `/root/svc.sh call TrackingEnvironment <code> ...`
inside the stack's property area): 1 getSharedMemory, 4 getCurrentMapUUID, 5 keepMap, 6 removeAllMaps, 7 loadMap,
8 listMaps, 9 stopMapExpansion, 10 placeAnchor, 11 registerAnchor, 13 persistAnchor, 14 locateAnchor (no-op),
16 getAnchorUuid, 20 setOrientationOnlyMode. Wire formats found by probing: AnchorHandle = int32 version + int32 index;
AnchorUuid = 16 bytes in text order; Parcelables are preceded by an int32 non-null flag (1).

- Needs Meta's real `RuntimeIpcBroker` (unit `android-runtimeipcbroker`, ovrstub started with `--skip RuntimeIpcBroker`):
  with ovrstub's stand-in the anchor server could not start.
- `placeAnchor` (code 10) returns the world-origin anchor handle (0,0) once "Gained map tracking" is logged;
  `persistAnchor` with that handle promotes the current map context to persistent ("transient -> persistent") and the
  engine writes it to `/vision/insideout/mapdb` (camera-derived data: never commit it, never copy it around).
- After a restart of the stack the engine loads that map by itself and relocalizes: the current map uuid is the saved
  one and the head pose comes back in the same world frame (measured: before/after restart, headset lying still,
  head position (-0.041, 0.032, 0.067) vs (-0.039, 0.032, 0.070), same orientation).
- `device/q1anchord.py` (unit `android-q1anchord`): after the headset has been worn 45 s with no saved map it persists the
  anchor and saves the map uuid in `/data/misc/tracking/world-map.uuid`; it writes `/run/q1track/frame-state`:
  `static` (the current map is the saved one, or none is saved yet) or `anchor` (a map is saved but the current one is
  another: relocalization failed). `touch /tmp/quest1-clear-map`, or clearing the playspace (the chaperone file loses its
  universe), calls `removeAllMaps` and forgets the uuids.
- `chaperone_frame.cpp` uses Meta's frame as is in `static` state (raw y = world y + eye height; floor and boundary
  in `chaperone_info.vrchap` stay valid because the frame is persistent) and falls back to the head anchoring above
  in `anchor` state or when `frame-state` is missing.

- Map lost (2026-11-07): the engine's MapDB is a key-value store with CRC-checked entries (`ad_<anchor>.mapdata`,
  `nd_<node>_<n>.mapdata` in mapdb). On 2026-11-07 it logged "MapDB: Deleting 3 corrupted entrees" (most likely writes
  cut by a reboot) and mapdb was empty, while `world-map.uuid` stayed: the frame stayed `anchor` (head anchoring) for
  every later boot and no map was ever saved again. Now q1anchord copies mapdb to `/data/misc/tracking/mapdb.bak` once
  the saved map is the current one and mapdb has been unchanged for a minute, `android-q1anchord-restore.service`
  (before trackingservice) puts the copy back when mapdb is empty, and an empty mapdb with nothing to restore makes
  q1anchord forget the uuids and save a new map. Tested: mapdb emptied by hand, stack restarted: restored, and the
  engine relocalized in it (current map = saved).
- Tracking lost mid-session: the engine runs a temporary map until it relocalizes. q1anchord used to switch to `anchor`,
  so driver_quest1 re-anchored the room on the head (the room turned and moved), and switched back when the saved map
  was found (a second jump). Once the saved map has been found in a run, q1anchord now stays `static`.

- Clearing the map from SteamVR (2026-11-08): SteamVR's settings have an "Oculus Quest" page
  (`quest1_settingsschema.json`, merged into SteamVR's schema by `steamvr-q1` through `VR_SETTINGSSCHEMA_FILE`) with a
  "Clear" button for `/settings/driver_quest1/clearSpatialAnchors`. SteamVR's schema has no button control: it is a
  one-option segment control (value true), which shows as a button as long as the setting is false; driver_quest1
  sees it go true, touches `/tmp/quest1-clear-map` and sets it back. Tested by a click through CEF remote debugging
  (q1anchord: "all maps removed").
- Map events log (2026-11-08): the journal is volatile, so what happened to the map at a put-on was lost with the boot
  (user: room in the wrong place, no chaperone). q1anchord's messages and the engine's map events (fakelogd lets its
  info lines about map contexts, map tracking and relocalization through) go to `/var/log/quest1-map.log` with the
  wall-clock time (256 KB, then `.1`). The clock itself was never set before (systemd-timesyncd lacked the
  aid_inet group: firstboot.sh). Found that morning: no chaperone file at all (the playspace had not been set up again
  after the map loss of 2026-11-07), which alone explains the missing chaperone.

Not yet verified: relocalization after a real reboot with the headset worn and moved around the room; whether the
persisted map keeps growing (mapdb rewritten) while used.


## Notes moved from the to-do list (2026-11-08): Boundary

History of measurements, fixes and open points as they were written in docs/todo.md (the to-do list now only
lists what is left to do).

- The Steam Frame's room setup works (user, 2026-11-05; the Adreno fixes are in docs/boundary.md). Left:
  - Leaving the setup, Steam showed a "something went wrong" popup (Refresh cleared it); maybe memory.
  - Frame rate in step 7: ~30 fps after a clean boot, as outside the setup (see Performance).
  - ComputeInverseDistortion returns identity while direct scanout distorts (distort_geom_* shaders place
    geometry with the inverse LUT).
  - Test in the headset: Recenter and Clear playspace, and relocalization after a reboot in the persisted
    map (`q1anchord`, docs/boundary.md).
- Pairing Quest controllers from the headset (Steam's UI). Pairing goes through SyncBoss
  (`syncboss_input_pair`): never call it without the user's explicit go.
- Controller connection state: a powered-off controller turns disconnected within 0.3 s (tested 2026-11-03); one the
  cameras have not tracked yet (start, or after the head's tracking came back) shows as searching (2026-11-07, Tracking).
