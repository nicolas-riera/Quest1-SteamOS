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

## Why Meta's map cannot be persisted (investigated 2026-11-02)

Stock Quest remembers rooms through trackingservice's `ITrackingEnvironment` (binder service
`TrackingEnvironment`; transaction codes: 4 getCurrentMapUUID, 5 keepMap, 6 removeAllMaps, 7 loadMap,
8 listMaps, 9 stopMapExpansion, 10 placeAnchor, 11 registerAnchor, 13 persistAnchor, 14 locateAnchor,
16 getAnchorUuid, 20 setOrientationOnlyMode; callable with `/root/svc.sh call TrackingEnvironment <code>`
inside the stack's property area). On this headset's tracker ("SuperVega"/VIPER):

- `getCurrentMapUUID` works (a live in-memory map exists), but `keepMap` returns false and `listMaps` is empty:
  trackingservice logs "InsideOutTrackingEngine API keepMap is not supported" / "listMaps is not supported".
  So the map is never written to `/vision/insideout/mapdb` and `loadMap` has nothing to load. This engine has
  no persistent whole-map API.
- The only persistence left is spatial anchors (Guardian uses them: `SpatialPersistence capability v1`,
  `persistAnchor`, `registerAnchor`, `locateAnchor`, `exportMapDataForAnchor`/`importMapDataForAnchor`). Their
  runtime is `SlamAnchorRuntimeIpcServer`, which rides on RuntimeIpcBroker and fails at start-up here
  ("initServer failed", the known RuntimeIpcBroker `connect` -74 failure), and the Parcelable layouts are
  not documented (`placeAnchor` returned handle -1). Making it work means fixing ovrstub's RuntimeIpcBroker
  first, then reverse-engineering the anchor parcelables, with no guarantee the engine relocalizes.

Hence the raw-space anchoring above stays the mechanism.
