#!/bin/sh
# Native Wi-Fi on Holo: NetworkManager (what the Steam UI drives) runs wlan0 only, usb0 stays with
# systemd-networkd. wlan0 itself comes from android-modem.service (holo-android-modem.sh).
# Usage (on the headset): holo-wifi.sh install
set -e
pacman -S --needed --noconfirm networkmanager wpa_supplicant iw iproute2
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
systemctl enable android-modem.service NetworkManager.service
# no 30 s boot stall when no Wi-Fi network is around
systemctl disable NetworkManager-wait-online.service
