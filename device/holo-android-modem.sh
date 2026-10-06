#!/bin/sh
# Bring up the modem subsystem natively, which hosts the WCN3990 Wi-Fi firmware on MSM8998, and
# then the qcacld host driver (wlan0). Same daemons Android's init.monterey.rc starts, run from
# the read-only system_a mount: the modem writes only its usual EFS/RFS data (modemst1/2,
# /persist/rfs), exactly as under Android.
# Usage: holo-android-modem start | stop
set -e
E="env ANDROID_ROOT=/system ANDROID_DATA=/data ANDROID_RUNTIME_ROOT=/apex/com.android.runtime"
L=/run/android-modem
V=/system/vendor/bin

start() {
	mkdir -p $L /dev/block/bootdevice/by-name /dev/socket
	# Once per boot only: reloading the modem after a shutdown from Linux hangs the SoC
	# (watchdog bite -> panic -> reboot into Android).
	[ -e $L/started ] && { echo "modem already started this boot"; return 0; }
	touch $L/started
	# Android's by-name partition links
	for u in /sys/class/block/sd*/uevent; do
		n=$(sed -n 's/^PARTNAME=//p' "$u"); d=$(sed -n 's/^DEVNAME=//p' "$u")
		[ -n "$n" ] && ln -sf "/dev/$d" "/dev/block/bootdevice/by-name/$n"
	done
	# /persist = the "private" partition on Oculus devices (fstab.monterey)
	mkdir -p /persist
	mountpoint -q /persist || mount -t ext4 -o nosuid,nodev,barrier=1 /dev/block/bootdevice/by-name/private /persist
	# Android 10 path of persist; tftp_server drops root for vendor_rfs (2951) and needs these dirs
	mkdir -p /mnt/vendor/persist
	mountpoint -q /mnt/vendor/persist || mount --bind /persist /mnt/vendor/persist
	mkdir -p /data/vendor/tombstones/rfs/modem /data/vendor/wifi/wpa/sockets /data/vendor/wifi/sockets
	chown -R 2951:2951 /data/vendor/tombstones/rfs
	chmod 0771 /data/vendor/tombstones/rfs /data/vendor/tombstones/rfs/modem
	# a modem crash restarts only the modem instead of rebooting the whole headset
	for s in /sys/bus/msm_subsys/devices/*; do echo RELATED > "$s/restart_level"; done

	# IPC router security rules (who may talk to which modem QMI service), as Android's vendor.irsc_util
	$E $V/irsc_util /vendor/etc/sec_config > $L/irsc_util.log 2>&1 || true

	# pd-mapper serves the protection-domain list (from the modem partition's *.jsn) that icnss
	# looks up once per boot to follow the modem's Wi-Fi user PD: it must be ready first.
	for d in "qrtr-ns -f" pd-mapper; do
		setsid $E $V/$d > $L/${d%% *}.log 2>&1 &
	done
	sleep 2
	for d in rmt_storage tftp_server; do
		setsid $E $V/$d > $L/$d.log 2>&1 &
	done
	sleep 1
	# Hold a reference on the modem subsystem (pm-service does this on Android)
	setsid sh -c 'exec 3</dev/subsys_modem; echo $$ > /run/android-modem/subsys.pid; exec sleep infinity' &
	# cnss-daemon blocks in libbinder until a binder context manager exists, and only then sends
	# the board data file (bdwlan.bin) the Wi-Fi firmware waits for before FW_READY.
	setsid $E /system/bin/servicemanager > $L/servicemanager.log 2>&1 < /dev/null &
	setsid $E $V/vndservicemanager /dev/vndbinder > $L/vndservicemanager.log 2>&1 < /dev/null &
	sleep 1
	# vendor binary using the system libnl: give the default linker namespace the system libs
	setsid $E LD_LIBRARY_PATH=/system/lib64/vndk-29:/system/lib64/vndk-sp-29:/system/lib64 $V/cnss-daemon -n -l > $L/cnss-daemon.log 2>&1 &
	sleep 2
	echo 1 > /sys/kernel/boot_wlan/boot_wlan 2>/dev/null || true  # EALREADY once loaded
}

stop() {
	# Never shut the modem down: Linux cannot stop it cleanly ("modem shutdown request
	# rejected") and the next load hangs the SoC. Daemons and the subsystem stay up until reboot.
	echo "android-modem: left running until reboot"
}

case "$1" in
start | stop) "$1" ;;
install)
	cat > /etc/systemd/system/android-modem.service <<'EOF2'
[Unit]
Description=Android modem subsystem and Wi-Fi firmware (WCN3990)
Requires=holo-android-blobs.service
After=holo-android-blobs.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/holo-android-modem start
ExecStop=/usr/local/sbin/holo-android-modem stop

[Install]
WantedBy=multi-user.target
EOF2
	systemctl daemon-reload
	systemctl enable android-modem.service
	;;
esac
