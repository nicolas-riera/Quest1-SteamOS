#!/bin/sh
# Native Wi-Fi on Holo: NetworkManager (what the Steam UI drives) runs wlan0 only, usb0 stays with
# systemd-networkd. wlan0 itself comes from android-modem.service (holo-android-modem.sh).
# Usage (on the headset): holo-wifi.sh install
set -e
P="networkmanager wpa_supplicant iw iproute2"
pacman -Q $P >/dev/null 2>&1 || pacman -S --needed --noconfirm $P
mkdir -p /etc/NetworkManager/conf.d
cat > /etc/NetworkManager/conf.d/10-holo-quest.conf <<EOF2
# NetworkManager runs the Wi-Fi only; usb0 (the USB link to the PC) stays with systemd-networkd.
[main]
dns=default
[keyfile]
unmanaged-devices=*,except:interface-name:wlan0
[device]
wifi.backend=wpa_supplicant
wifi.scan-rand-mac-address=no
EOF2
cat > /etc/systemd/network/10-usb0.network <<EOF2
[Match]
Name=usb0
[Network]
# USB link to the PC only (PC = 192.168.77.2). Internet comes from the Wi-Fi (NetworkManager).
Address=192.168.77.1/24
ConfigureWithoutCarrier=yes
IPv6AcceptRA=no
IPv6SendRA=no
EOF2
networkctl reload
# android-modem.service is enabled by holo-android-modem.sh install, which runs after this script
systemctl enable NetworkManager.service
# no 30 s boot stall when no Wi-Fi network is around
systemctl disable NetworkManager-wait-online.service

# Wi-Fi power saving only while little goes through (user, 2026-11-12): the chip sleeps between beacons when idle
# (battery), and stays awake while a stream or a download runs, where its wake-ups would delay packets. q1-wifipower
# looks at wlan0's byte counters every 2 s: power saving off above 1 MB/s, back on after 30 s below 128 kB/s. It sets
# the state again every 30 s, since a reconnection gives the driver's default (on) back.
mkdir -p /usr/local/sbin
cat > /usr/local/sbin/q1-wifipower <<'EOF2'
#!/bin/sh
D=wlan0 S=/sys/class/net/wlan0/statistics
want=on cur= calm=0 tick=0 last=
bytes() { echo $(( $(cat $S/rx_bytes 2>/dev/null || echo 0) + $(cat $S/tx_bytes 2>/dev/null || echo 0) )); }
while :; do
	now=$(bytes)
	if [ -n "$last" ] && [ "$now" -ge "$last" ]; then
		rate=$(( (now - last) / 2 ))
		if [ $rate -gt 1048576 ]; then
			want=off calm=0
		elif [ $rate -lt 131072 ]; then
			calm=$((calm + 2))
			[ $calm -ge 30 ] && want=on
		else
			calm=0
		fi
	fi
	last=$now
	tick=$((tick + 2))
	if [ "$want" != "$cur" ] || [ $tick -ge 30 ]; then
		[ -d /sys/class/net/$D ] && iw dev $D set power_save $want 2>/dev/null &&
			{ [ "$want" != "$cur" ] && echo "q1-wifipower: power saving $want"; cur=$want; }
		tick=0
	fi
	sleep 2
done
EOF2
chmod 755 /usr/local/sbin/q1-wifipower
cat > /etc/systemd/system/q1-wifipower.service <<'EOF2'
[Unit]
Description=Wi-Fi power saving only while the link is quiet (q1-wifipower)
After=NetworkManager.service

[Service]
ExecStart=/usr/local/sbin/q1-wifipower
Restart=always
RestartSec=10
Nice=10

[Install]
WantedBy=multi-user.target
EOF2
systemctl enable q1-wifipower.service
