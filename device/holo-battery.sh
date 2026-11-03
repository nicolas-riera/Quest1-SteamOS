#!/bin/sh
# Battery state for Steam (every 5 s) and the power LED (every second): q1-battery.
#
# Steam: on the Steam Frame (-steamframe), Steam reads the battery from /run/vpower/ (steamclient's
# battery manager, query mode 3) before the Deck's /sys/class/power_supply/BAT* and SDL: without it
# it fell back to SDL, which has no charge time ("?h ?min"). Files: battery_percent,
# secs_until_battery_full, secs_until_shutdown_request, ac_status (Connected / Disconnected),
# battery_status (Charging / Discharging / Full). The times are UPower's (averaged rate).
# secs_until_shutdown_request is when Steam powers the headset off: 0 makes it shut down at once
# (it did, 2026-11-03), so it is never under 60 s except on an empty battery without charger; when
# UPower has no time to empty (charging) it is estimated from the charge and the current.
#
# LED (the PMI8998 RGB LED, /sys/class/leds/{red,green,blue}), as on the stock Quest 1:
#  - under 10 % (config_lowBatteryWarningLevel): red, steady (Meta's
#    config_notificationsBatteryLowAnimation is empty);
#  - charging: orange, green when full or from 98 % (config_oculusFullBatteryLevel), asleep too;
#  - on battery: white while the headset is on (user, 2026-11-03), off in standby
#    (/run/quest1-standby).
# Meta's BatteryService sets the battery colours through the Monterey light HAL (framework-res:
# low 0xff0000, medium 0xff4600, full 0x00ff00), each channel scaled by the HAL's 0.2745 (70/255):
# red 70, orange 70/19, green 70; white the same 70. The bootloader leaves it lit (yellow) and
# nothing turned it off before.
#
# Run as root: sh holo-battery.sh install
set -e
[ "$1" = install ] || { echo "usage: $0 install" >&2; exit 2; }

cat > /usr/local/sbin/q1-battery <<'EOF'
#!/bin/sh
# q1-battery: /run/vpower for Steam and the power LED (device/holo-battery.sh).
L=/sys/class/leds
P=/sys/class/power_supply
V=/run/vpower
U="org.freedesktop.UPower /org/freedesktop/UPower/devices/battery_battery org.freedesktop.UPower.Device"
cur=
rgb() {
	[ "$1 $2 $3" = "$cur" ] && return
	echo "$1" >$L/red/brightness
	echo "$2" >$L/green/brightness
	echo "$3" >$L/blue/brightness
	cur="$1 $2 $3"
}
put() { # file value: written only when it changes, atomically
	[ "$(cat $V/$1 2>/dev/null)" = "$2" ] && return
	echo "$2" >$V/.$1 && mv $V/.$1 $V/$1
}
upower_secs() { busctl get-property $U "$1" 2>/dev/null | awk '{print $2 + 0}'; }
trap 'rgb 0 0 0; exit 0' TERM INT
mkdir -p $V
n=0
while :; do
	read -r level <$P/battery/capacity
	read -r status <$P/battery/status
	ac=Disconnected
	[ "$(cat $P/usb/online $P/dc/online 2>/dev/null | grep -c 1)" -gt 0 ] && ac=Connected
	case $status in
	Charging | Full) st=$status ;;
	*) st=Discharging; [ $ac = Connected ] && st=Charging ;; # plugged but not charging: Steam's charge limit
	esac

	if [ -e /run/quest1-standby ] && [ "$status" != Charging ] && [ "$status" != Full ]; then
		rgb 0 0 0
	elif [ "$level" -lt 10 ]; then
		rgb 70 0 0
	elif [ "$status" = Full ] || { [ "$status" = Charging ] && [ "$level" -ge 98 ]; }; then
		rgb 0 70 0
	elif [ "$status" = Charging ]; then
		rgb 70 19 0
	else
		rgb 70 70 70
	fi

	if [ $((n % 5)) -eq 0 ]; then
		put battery_percent "$level"
		put ac_status $ac
		put battery_status $st
		full=$(upower_secs TimeToFull)
		empty=$(upower_secs TimeToEmpty)
		if [ "${empty:-0}" -le 0 ]; then # µAh left / µA drawn (at least 0.5 A when charging)
			read -r cf <$P/battery/charge_full
			read -r cn <$P/battery/current_now
			empty=$(awk -v f="$cf" -v l="$level" -v c="$cn" 'BEGIN { c = c < 0 ? -c : c; if (c < 500000) c = 500000;
				print int(f * l / 100 / c * 3600) }')
		fi
		[ "${empty:-0}" -lt 60 ] && { [ "$level" -gt 0 ] || [ $st != Discharging ]; } && empty=60
		put secs_until_battery_full "${full:-0}"
		put secs_until_shutdown_request "$empty"
	fi
	n=$((n + 1))
	sleep 1 &
	wait $!
done
EOF
chmod 755 /usr/local/sbin/q1-battery

cat > /etc/systemd/system/quest1-battery.service <<'EOF'
[Unit]
Description=Quest 1 battery state for Steam (/run/vpower) and the power LED
After=upower.service
Wants=upower.service

[Service]
ExecStart=/usr/local/sbin/q1-battery
Nice=10

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable quest1-battery.service
systemctl restart quest1-battery.service
