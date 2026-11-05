# Bluetooth

The Quest 1's Bluetooth is the Bluetooth side of its Qualcomm WCN3990 (Wi-Fi is the other side, on SNOC). On Android,
Meta's HAL (`android.hardware.bluetooth@1.0-impl-qti.so`) drives it from userspace over `/dev/ttyHS0`. The Steam
Frame uses BlueZ on a kernel driver: so do we, with BlueZ and the kernel's `hci_uart` QCA protocol.

Status (2026-11-05): working. hci0 comes up powered (BR/EDR and LE) with the headset's own address about 13 s after
`quest1-bluetooth.service` starts, and scans find the devices around. Pairing a keyboard, mouse, gamepad or headphones
is still to try.

## The chip (probed from userspace, 2026-11-05)

- Power: the `bt_power` rfkill (Meta's `bluetooth-power.c`, regulators only). The supplies are shared with Wi-Fi, so
  cutting `bt_power` does not reset the Bluetooth core: it keeps its state, its UART speed included, across power
  cycles and even reboots (a core left at 3 Mbit/s by an earlier run stays deaf at 115200). The WCN399x power pulses
  reset it: a circuit on its receive line decodes 0xC0 sent at 2400 baud (off) and 0xFC at 115200 (on), flow control
  off. After them it answers HCI_Reset at 115200 within 0.3 s. The version request only gets an answer after an
  HCI_Reset.
- Version (EDL vendor command 0xFC00 / 0x19): product 0x0a, ROM 0x0201, SoC id 0x40010214 = WCN3990 2.1, patch 1
  (2 once the patch is applied). The reply is followed by a Command Complete with opcode 0, as on later WCN399x.
- Firmware, in Meta's `vendor/firmware` (exposed in `/run/fw`, the kernel's firmware path, by holo-android-blobs):
  `crbtfw21.tlv` (patch, 130 KB) and `crnv21.bin` (NVM, a type-4 TLV enclosing several tag sets, the first of
  type 2). The `bluetooth_a` partition is empty.
- Patch download (TLV download mode 3): no segment is acknowledged, not even the last one, contrary to Linux's
  rule. ~150 ms after the last segment the core has applied the patch and sends in-band sleep wake indications
  (0xFD) until the host answers 0xFC. Segments sent back to back faster than the UART carries them are lost (the
  download then does nothing); one at a time, each fully sent before the next, it works.
- NVM download: each segment acknowledged by a vendor event and a Command Complete. The HCI_Reset that follows is
  answered, then the core restarts with its NVM: a command sent right after that answer is lost.
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

- `hci_qca.c`: the WCN399x power pulses (Linux 5.x `qca_wcn3990_init`), an HCI_Reset, the version read at 115200 to
  tell a WCN399x (SoC id 0x4001xxxx), then 3 Mbit/s: the host switches as soon as the command has left the UART.
  This chip sends no vendor event after the speed change (the driver waits 100 ms for it and goes on), and toggling
  flow control around the switch garbled the next commands. Then the firmware, each unacknowledged segment waited
  for until it has left the UART (~11 s for the patch). No in-band sleep on a WCN399x (below).
- `btqca.c`: WCN399x firmware names (`qca/crbtfw%02x.tlv`, `qca/crnv%02x.bin`, from the ROM version), the TLV
  download modes with an injected Command Complete (Linux 5.4), the last patch segment unacknowledged on a WCN399x,
  NVM files with several tag sets (Linux 5.17), larger file limits, the NVM edited on a copy; the address written
  into the NVM's BD_ADDR tag from the parameter `btqca.nvm_bdaddr` (12 hex digits, NVM order); `qca_set_bdaddr`
  (0xFC14) for later changes; 300 ms for the patch to apply before the NVM, 200 ms after the final reset.
- In-band sleep and deep sleep off in the NVM (HCI transport tag 17 bit 7, tag 27 bit 0), like Qualcomm's HAL with
  its low-power mode off: with them on, the core answered the host's wake indication after the NVM but never the
  HCI reset that followed. A plain H4 UART; the core stays awake while the service runs.
- `hci_core.c`: `__hci_cmd_send` (Linux 5.1), for segments the chip does not acknowledge.
- Config: `BT_HCIUART` (H4, QCA), RFCOMM, BNEP, HIDP; HID drivers for Sony and Microsoft pads and Apple keyboards
  (Steam itself reads pads through hidraw).

Left: a "command 0xfc00 tx timeout" line during the download (the version read's Command Complete has opcode 0,
which does not stop hci_core's command timer; harmless), and the patch download could go faster than one segment
per ~20 ms.

## Userspace (`device/holo-bluetooth.sh`)

`quest1-bluetooth.service` runs `quest1-bt start`: link `/run/fw/qca` to `/run/fw`, set `btqca.nvm_bdaddr` from
`.bt_nv.bin`, power the chip through `bt_power`, then `btattach -B /dev/ttyHS0 -P qca -S 115200`, which keeps the
line discipline (and hci0) while it runs; the kernel downloads the firmware when hci0 registers and bluetoothd
(BlueZ) powers it on. Stopping the unit cuts the chip's power (restart tested).
