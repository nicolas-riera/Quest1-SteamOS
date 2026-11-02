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

`touch /tmp/quest1-webdebug` (as steamos) before the dashboard starts makes `vrwebhelper.sh` enable CEF
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

Not yet verified: relocalization after a real reboot with the headset worn and moved around the room.
