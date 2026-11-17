#!/bin/sh
# Meta's charging sound when the charger is plugged in: q1-chargesound.
#
# The sound is Meta's own (ChargingStarted.ogg, played by their OS on the same event). It is never in this
# repository nor in its output: it is read at run time from the headset's Meta system partition, mounted read-only
# at /system by holo-android-blobs (installer builds: kept from Meta's public OTA by installer/build_image.py, the
# rest of ./system/product is dropped). Without the file nothing plays.
#
# Event: the charger input (/sys/class/power_supply/{usb,dc}/online, the ones q1-battery reads) going from 0 to 1.
# Nothing stays running for it: a udev rule runs q1-chargesound on the power supplies' change events, and it compares
# with the state it last saw (/run/q1-charge-online, written at boot by quest1-chargesound.service, so already
# plugged at boot is silent). It plays at once (the charger's first event comes ~0.1 s after the plug, the sound
# starts ~0.3 s later); sounds are at least 3 s apart (a connector bouncing as it goes in; 15 s was too long for
# the user). No sound in standby (/run/quest1-standby). Played through the user's PipeWire session (pw-play as
# steamos, default sink, so the system volume applies), with or without SteamVR running.
# Setting: "Play a sound when the charger is plugged in" on the Oculus Quest page of SteamVR's settings,
# /settings/driver_quest1/chargeSound in steamvr.vrsettings (on when absent).
#
# Run as root: sh holo-chargesound.sh install
set -e
[ "$1" = install ] || { echo "usage: $0 install" >&2; exit 2; }

cat > /usr/local/sbin/q1-chargesound <<'EOF'
#!/bin/sh
# q1-chargesound [init]: Meta's charging sound on plug-in (device/holo-chargesound.sh). init: record the state only.
P=/sys/class/power_supply
S=/run/q1-charge-online
online() { cat $P/usb/online $P/dc/online 2>/dev/null | grep -qx 1 && echo 1 || echo 0; }
exec 9>/run/q1-charge.lock
flock 9
now=$(online)
was=$(cat $S 2>/dev/null)
echo $now > $S
[ "$1" = init ] && exit 0
[ "$was" = 0 ] && [ "$now" = 1 ] || exit 0
t=$(cut -d. -f1 /proc/uptime)
last=$(cat /run/q1-charge-sound 2>/dev/null || echo -99)
[ $((t - last)) -ge 3 ] || exit 0
echo $t > /run/q1-charge-sound
[ -e /run/quest1-standby ] && exit 0
[ -e /run/user/1000/pipewire-0 ] || exit 0
# the setting: off only when explicitly false
python3 -c 'import json, sys
try:
    on = json.load(open(sys.argv[1])).get("driver_quest1", {}).get("chargeSound", True)
except (OSError, ValueError):
    on = True
sys.exit(0 if on else 1)' /home/steamos/.local/share/Steam/config/steamvr.vrsettings || exit 0
for f in /system/product/media/audio/ui/ChargingStarted.ogg /android/system/product/media/audio/ui/ChargingStarted.ogg; do
	[ -f $f ] || continue
	exec setpriv --reuid=1000 --regid=1000 --groups=995 -- env HOME=/home/steamos XDG_RUNTIME_DIR=/run/user/1000 \
		timeout 15 pw-play $f
done
logger -t q1-chargesound "no ChargingStarted.ogg on the Meta system partition"
EOF
chmod 755 /usr/local/sbin/q1-chargesound

# the state at boot (silent), then the udev rule for the changes
cat > /etc/systemd/system/quest1-chargesound.service <<'EOF'
[Unit]
Description=Charger state at boot for Meta's charging sound (q1-chargesound)
After=holo-android-blobs.service

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/q1-chargesound init

[Install]
WantedBy=multi-user.target
EOF
# udev kills long RUN programs (the sound): they go through systemd-run
cat > /etc/udev/rules.d/90-quest1-chargesound.rules <<'EOF'
SUBSYSTEM=="power_supply", KERNEL=="usb|dc", ACTION=="change", RUN+="/usr/bin/systemd-run --no-block --quiet --collect --nice=10 /usr/local/sbin/q1-chargesound"
EOF
systemctl daemon-reload
systemctl enable quest1-chargesound.service
systemctl restart quest1-chargesound.service
udevadm control --reload
