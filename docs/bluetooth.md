# Bluetooth

The Quest 1's Bluetooth is the Bluetooth side of its Qualcomm WCN3990 (Wi-Fi is the other side, on SNOC). On Android,
Meta's HAL (`android.hardware.bluetooth@1.0-impl-qti.so`) drives it from userspace over `/dev/ttyHS0`. The Steam
Frame uses BlueZ on a kernel driver: so do we, with BlueZ and the kernel's `hci_uart` QCA protocol.

## The chip (probed from userspace, 2026-11-05)

- Power: the `bt_power` rfkill (Meta's `bluetooth-power.c`, regulators only). No power-on pulse is needed, but the
  chip ignores commands for about a second after the rfkill is unblocked; then it answers HCI_Reset at 115200 baud
  with hardware flow control. The version request only gets an answer after an HCI_Reset.
- Version (EDL vendor command 0xFC00 / 0x19): product 0x0a, ROM 0x0201, SoC id 0x40010214 = WCN3990 2.1. The reply
  is followed by a Command Complete with opcode 0, as on later WCN399x.
- Firmware, in Meta's `vendor/firmware` (exposed in `/run/fw`, the kernel's firmware path, by holo-android-blobs):
  `crbtfw21.tlv` (patch, 130 KB, TLV download mode 3: only the last segment is acknowledged) and `crnv21.bin`
  (NVM, a type-4 TLV enclosing several tag sets, the first of type 2). The `bluetooth_a` partition is empty.
- Address: the NVM's BD_ADDR tag is all zeros. Meta keeps the headset's address in persist
  `bluetooth/.bt_nv.bin`: 9 bytes, `01 01 06` then the address in NVM byte order (it ends with Oculus' OUI
  2C:26:17 when reversed). Read only.

## Kernel (`kernel/bt-wcn3990.patch`, `kernel/steamos.config`)

Meta's 4.4 tree has `hci_uart` with the QCA protocol for ROME chips only, and it was never built there: it did not
compile (`hci_ldisc.c` initialized a removed `rx_lock`), `btqca.c` could not download anything (its file-size
checks always ended in "wrong config type selected"), and closing the line discipline unregistered and freed the
hci_dev twice (a kernel oops when btattach exits, e.g. restarting the service: it crashed the headset on
2026-11-05). Bluetooth's log helpers printed a pointer instead of the message (`%pKV`: a `%p` -> `%pK` replace
that also hit `%pV`). The patch:

- `btqca.c`: WCN399x firmware names (`qca/crbtfw%02x.tlv`, `qca/crnv%02x.bin`, from the ROM version), the TLV
  download modes with an injected Command Complete (Linux 5.4), NVM files with several tag sets (Linux 5.17),
  larger file limits, the NVM edited on a copy; the address written into the NVM's BD_ADDR tag from the
  parameter `btqca.nvm_bdaddr` (12 hex digits, NVM order); `qca_set_bdaddr` (0xFC14) for later changes.
- `hci_qca.c`: an HCI_Reset, then the version read at 115200 to tell a WCN399x (SoC id 0x4001xxxx), then the speed
  goes to 3 Mbit/s: the host switches as soon as the command has left the UART. This chip sends no vendor event
  after the speed change (the driver waits 100 ms for it and goes on), and toggling flow control around the
  switch garbled the next commands. Then the firmware.
- `hci_core.c`: `__hci_cmd_send` (Linux 5.1), for segments the chip does not acknowledge.
- Config: `BT_HCIUART` (H4, QCA), RFCOMM, BNEP, HIDP; HID drivers for Sony and Microsoft pads and Apple keyboards
  (Steam itself reads pads through hidraw).

## Userspace (`device/holo-bluetooth.sh`)

`quest1-bluetooth.service` runs `quest1-bt start`: link `/run/fw/qca` to `/run/fw`, set `btqca.nvm_bdaddr` from
`.bt_nv.bin`, power-cycle the chip through `bt_power`, then `btattach -B /dev/ttyHS0 -P qca -S 115200`, which keeps
the line discipline (and hci0) while it runs. bluetoothd (BlueZ) powers hci0 on, which runs the firmware download.
Stopping the unit cuts the chip's power.

## Status (2026-11-05)

Not working yet. Power-on, reset, version and the 3 Mbit/s switch work; the patch download (`crbtfw21.tlv`, mode 3)
gets no vendor event after its last segment, so setup fails and hci0 stays down. A userspace probe of the same
download (python on the raw tty) was followed by a headset reboot. `quest1-bluetooth.service` is installed but not
enabled until the download works (`systemctl enable --now quest1-bluetooth` to try it).
