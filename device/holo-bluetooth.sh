#!/bin/sh
# Bluetooth: the WCN3990's Bluetooth side on its UART (/dev/ttyHS0), driven by the kernel's hci_uart QCA
# protocol (kernel/bt-wcn3990.patch) and BlueZ, like the Steam Frame (BlueZ, no Android stack).
#
# quest1-bluetooth.service (quest1-bt):
#  - powers the chip through the bt_power rfkill (Meta's bluetooth-power driver: regulators only, no pulse
#    needed, the chip answers at 115200 right away);
#  - gives the kernel the headset's own address: Meta keeps it in persist bluetooth/.bt_nv.bin (9 bytes:
#    01 01 06, then the 6 address bytes in NVM order), read only, and written into the NVM's BD_ADDR tag by
#    btqca (parameter nvm_bdaddr) before the NVM is downloaded. The stock NVM's address is all zeros;
#  - runs btattach (QCA protocol), which keeps the tty open: hci0 exists while the unit runs. The firmware
#    (qca/crbtfw21.tlv, qca/crnv21.bin = WCN3990 2.1) comes from Meta's vendor/firmware through /run/fw
#    (holo-android-blobs), loaded at hci0's first power-on by bluetoothd (~2 s: patch at 3 Mbit/s).
#
# Run as root: sh holo-bluetooth.sh install
set -e
[ "$1" = install ] || { echo "usage: $0 install" >&2; exit 2; }

cat > /usr/local/sbin/quest1-bt <<'EOF'
#!/bin/sh
# quest1-bt start|stop: the WCN3990's Bluetooth (device/holo-bluetooth.sh)
R=
for d in /sys/class/rfkill/rfkill*; do [ "$(cat $d/name 2>/dev/null)" = bt_power ] && R=$d; done
[ -n "$R" ] || { echo "quest1-bt: no bt_power rfkill" >&2; exit 1; }
case "$1" in
start)
	# the kernel asks for qca/<name>: /run/fw has Meta's firmware files flat
	[ -e /run/fw/qca ] || ln -s . /run/fw/qca
	N=/mnt/vendor/persist/bluetooth/.bt_nv.bin
	if [ -r $N ] && [ "$(od -An -tx1 -N3 $N | tr -d ' \n')" = 010106 ]; then
		od -An -tx1 -j3 -N6 $N | tr -d ' \n' > /sys/module/btqca/parameters/nvm_bdaddr
	else
		echo "quest1-bt: no address in $N, the controller keeps the NVM's" >&2
	fi
	# a fresh chip: firmware and NVM are downloaded at hci0's first power-on
	echo 0 > $R/state; sleep 0.2
	echo 1 > $R/state; sleep 1.5  # the chip ignores commands for about a second after power-on
	exec btattach -B /dev/ttyHS0 -P qca -S 115200
	;;
stop)
	echo 0 > $R/state
	;;
*) echo "usage: $0 start|stop" >&2; exit 2 ;;
esac
EOF
chmod 755 /usr/local/sbin/quest1-bt

cat > /etc/systemd/system/quest1-bluetooth.service <<'EOF'
[Unit]
Description=Quest 1 Bluetooth controller (WCN3990 on ttyHS0, hci_uart QCA)
# /run/fw (firmware) and /mnt/vendor/persist (the address) come from these
After=holo-android-blobs.service android-modem.service
Wants=android-modem.service
Before=bluetooth.service

[Service]
ExecStart=/usr/local/sbin/quest1-bt start
ExecStopPost=/usr/local/sbin/quest1-bt stop
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
# quest1-bluetooth is enabled once the firmware download works (docs/bluetooth.md, Status)
systemctl enable bluetooth.service
