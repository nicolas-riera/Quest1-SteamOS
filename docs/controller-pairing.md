# Pairing Touch controllers from Steam's "Add controllers" page

Status 2026-11-17: backend written and tested with the pairing call stubbed (dry run). The real pairing call is not wired up.
[V] = verified on the headset or in disassembly, [I] = inferred.

## How Steam's page works on a Steam Frame [V]
* Settings > Controller > "Add a new controller" opens the Frame variant of the page when the client has the Frame identity
  (`ON_FRAME`, docs/frame-mode.md): searching text, two controller SVGs, and at the bottom the string
  `Settings_Controller_Frame_Pairing_Desc` ("Press and hold the System and Menu buttons until the LED flashes white..."),
  plus Identify / Forget / Forget All / Disconnect All.
* The page sends `start_pairing {timeout_seconds: 30}` when it opens and `stop_pairing` when it closes. The transport is
  neither BlueZ nor steamos-manager: steamui's JS talks to vrserver's web socket (protobuf `CSteamVR_ControllerPairing_Request`,
  state in `CSteamVR_ControllerPairing_State_Message`).
* vrserver forwards each request as JSON `{"type": ...}` to the driver mailbox `frame_controller_pairing`. Types:
  `update_pairing_state`, `start_pairing` (+`timeout_seconds`), `stop_pairing`, `identify_controller`, `disconnect_controller`
  (+`serial_number`), `disconnect_all_controllers`, `unpair_controller` (+`serial_number`), `unpair_all_controllers`,
  `unpair_disconnected_controllers`.
* The driver answers in vrserver's mailbox `web_steam_mailbox`:
  `{"type":"pairing_state","num_controller_slots":2,"is_pairing":b,"connected_controllers":[{"serial_number","role":"left"|"right"}],"paired_controllers":[{"serial_number"}]}`.
* On a Frame, Valve's frame_controller driver serves this; here that driver has no binary, so nobody did. Other controller types
  (BlueZ pads) use the generic Bluetooth page and already work.

## Our backend: src/q1pairing/q1pairing.cpp
A small arm64 program using vrclient's `IVRMailbox_001` (same as src/camtap/vrmailbox). It registers `frame_controller_pairing`,
answers state requests, and keeps the pairing action behind `class Backend` (the only code that can change pairing).
New controller types: add a Backend (or extend the `scan` / `pair` / `unpair` helper protocol) and their entries to the state.
* Default backend is dry run: it only logs "would ...". `QUEST1_PAIR_SIM=1` fakes a controller 4 s after start_pairing.
* The real backend is used only if `/etc/quest1/pairing-real` exists (and `QUEST1_PAIR_DRYRUN` is not 1). It runs
  `/usr/local/libexec/q1-pair-hal scan | pair <hex id> | unpair <hex id>` (QUEST1_PAIR_CMD overrides). That helper does not exist yet,
  so the real backend currently changes nothing.
* It never loads libsyncboss. Controller list comes from `/run/q1track/controllers` (read only). Serials are the driver's
  `QUEST1-Touch-Left/Right`.
* Must run as the `steamos` user: as root, VR_Init fails with Init_Internal (124) on SteamVR 2.19.3 [V].
* Build: `src/q1pairing/build.sh` (WSL). Not yet started by any unit: for now run by hand (see tests).

## How Meta pairs [V from symbols and strings, call order I]
* Android: `OVRRemoteService::scanAndPairDevice/Secure/ById/Filtered(type, timeout, ...)`, `unpairDevice`, `getPairedDeviceStatus`,
  `verifyControllersConnectable` (permission com.oculus.permission.MODIFY_CONTROLLER_STATUS). `pairing_ctl start [-l id] [-r id] [-t s] [-a]`
  and `unpair` use it (default timeout 5 s per controller, `-a` pairs any controller, message "Timed Out (please check controller is in pairing mode)").
* The sensors HAL (`vendor.oculus.hardware.sensors@1.0-service`) implements `IControllerManagementClient`:
  `getAdvertisingControllers()` -> `syncboss_input_enumerate_advertising_devices`, `pairController(addr)` -> `syncboss_input_pair(h, id)`
  (HAL code 0x5c120: stops nothing itself, logs "Failed to pair device, ret: %d", ret -110 = timeout), `unpairController`. Secure pairing
  is "not supported" in the HAL. `syncboss_input_pair` is refused while input is started (beacon mode), so a real implementation has to
  stop input first (the HAL stops it itself in its own flow [I]).
* Controller button combo (Meta's public docs, not from the binaries): right controller hold Oculus + B, left hold Menu + Y until the LED blinks.
  Quest 1 Touch controllers are not the Frame's: this differs from the Frame's "System + Menu" text.
* Real path to implement (next step): drive IControllerManagementClient through the already running sensors HAL via hwbinder
  from a bionic helper (like q1trackd), so libsyncboss is not opened a second time. Do this only with the user present.

## Hint text and pictograms (no supported hook found)
* The Frame text is `Settings_Controller_Frame_Pairing_Desc` in steamui's string table and the controller drawings are inline SVG in
  `sp.js` (both Valve files, checked by Steam's file verification).
* `config/uioverrides` (the folder steam-q1 uses for the startup movies) only takes `webm;jpg;jpeg` (steamui.so [V]): not text or SVG.
* SteamVR's `/opt/steamvr/drivers/frame_controller/resources/icons/` (svg/png/gif) belong to SteamVR's own UI, not this page.
* Remaining options, none done: (a) patch `steamui_english-json.js` at install time (Steam may revert it when it verifies);
  (b) a CDP injector on Steam's CEF port that rewrites the string and the SVG at run time (touches no file; needs the debug port
  enabled, today a test-only `.cef-enable-remote-debugging`). The Touch text to show: right controller hold Oculus + B, left hold Menu + Y.

## Install (2026-11-17)
`device/holo-pairing.sh install` (wired into device/install-image.sh; tools/build-native.sh builds `/usr/local/bin/q1-pairing`):
`q1-pairing.service` as steamos after SteamVR (that fixes VR_Init 124), `/run/q1pairing` for Identify requests, `/usr/local/libexec/q1-pair-hal`
(stub from `device/q1-pair-hal`, exits 3). Identify: q1-pairing writes `left`/`right` into `/run/q1pairing/identify`; `Quest1Controllers::PollRequests`
(driver, called from RunFrame) buzzes that controller 0.4 s. Compiles; not deployed (needs a driver install + SteamVR restart).
Disconnect is logged only (needs the sensors HAL).

## q1-pair-hal (real pairing): still to write
Over hwbinder to `vendor.oculus.hardware.sensors@1.0::IControllerProvider/default`: `getManagementClient` (HIDL code to take from
`BnHwControllerProvider::onTransact`, disassembly in libvrsensors-hidl@1.0.so), then on `IControllerManagementClient`: `enterDMM`,
`getAdvertisingControllers`, `pairController(addr)`, `unpairController`, `exitDMM`. Hand-rolled hwbinder in C (like src/ovrstub for binder) or a
bionic C++ client; the HAL does the input stop/start around syncboss_input_pair. Needs hwservicemanager `get` and scatter-gather parcels:
not started, and it can only be tested with the user present (it is the pairing call).

## Dry-run tests done (2026-11-17, SteamVR 2.19.3)
`QUEST1_PAIR_DRYRUN=1 QUEST1_PAIR_SIM=1 q1pairing` as steamos + `vrmailbox frame_controller_pairing '{"type":...}'`: start_pairing/stop_pairing/
unpair_all/identify handled, state messages sent to `web_steam_mailbox` with no "malformed mailbox" in vrserver.txt, DRY RUN lines only.
Not tested: Steam's page itself (needs the user's Steam UI).

## Live test, main session with the user
1. Copy `q1pairing` to the headset, run as steamos with `QUEST1_PAIR_DRYRUN=1 QUEST1_PAIR_SIM=1`, open Steam Settings > Controllers >
   Add a new controller (SteamVR must be running, Frame identity): expect "Searching..." and the simulated controller after 4 s.
2. Check /tmp log shows `request: start_pairing` on page open and `stop_pairing` on leaving. Only then consider the real helper.
