#!/bin/sh
# Meta's stock Quest 1 tracking stack, run natively under Holo from the read-only system_a mount
# ("Path M", docs/tracking-6dof.md): sensors HAL + IAD HAL + settingsserver + vrfocusserver +
# cameramuxmodeservice + calibrationserver + trackingservice, with src/ovrstub standing in for
# the system_server binder services (permission, appops, vrpowermanager, settingsproxy...).
#
# Usage: holo-android-tracking setup    (runtime prep, run by android-tracking-setup.service)
#        holo-android-tracking install  (copy this script, write the systemd units)
#        holo-android-tracking base     (start everything that does not touch SyncBoss)
#        holo-android-tracking trial    (monado.service stopped first: sensors HAL +
#                                        trackingservice for TRIAL_SECONDS (60), polling head
#                                        poses, then collect logs and stop the SyncBoss units)
#        holo-android-tracking monado-trial [s] (stop monado.service, trial, start it again:
#                                        only through /root/q1restart.sh, it breaks SteamVR)
#        holo-android-tracking meta-on | meta-off  (switch Monado between Meta's 6DoF tracking
#                                        and its own SyncBoss IMU; restarts Monado: only through
#                                        /root/q1restart.sh)
#        holo-android-tracking stop | cli <trackinginterface_cli args>
#        holo-android-tracking restart  (Meta's stack again, e.g. for a controller it missed; only
#                                        through /root/q1restart.sh: ~30 s without tracking)
#
# Meta mode ("meta-on"): the sensors HAL owns SyncBoss, trackingservice tracks, q1trackd
# (src/q1track) publishes the poses in /run/q1track/poses and Monado's quest1 driver reads them
# (/etc/quest1-tracking = meta), touching only /dev/syncboss_powerstate0 (proximity, a
# multi-reader node). Monado keeps driving the panels, so the display TE the HAL needs runs
# whenever the headset is worn.
#
# Safety:
#  * /persist (sda8, mounted rw by android-modem for the modem's RFS) is never written by this
#    stack: every unit sees /persist read-only, with a tmpfs copy of /persist/calibration.
#  * The controller firmware archives are hidden from the sensors HAL and trackingservice
#    (InaccessiblePaths), and persist.ovr.skipctrlfwupdate=1 is in the property area, so no
#    controller firmware update can start.
#  * The sensors HAL owns SyncBoss (/dev/syncboss*) and the cameras: it must never run together
#    with another libsyncboss user (Monado's quest1 driver in IMU mode, qcam, sbimu). Its unit
#    refuses to start while another process holds /dev/syncboss0, _control0 or _stream0, and is
#    not enabled at boot.
set -e
R=/run/q1track
E="ANDROID_ROOT=/system ANDROID_DATA=/data ANDROID_RUNTIME_ROOT=/apex/com.android.runtime"

setup() {
	mkdir -p $R
	# /persist for the tracking stack: the units bind it read-only and put a tmpfs copy of
	# calibration/ on top (calibrationserver writes online/ and online.tmp/). An overlayfs upper
	# layer cannot be used: copy-up fails on the ext4 files' security.selinux xattrs.
	mkdir -p /persist
	if ! mountpoint -q /persist; then # android-modem not running: mount it ourselves, read-only
		dev=/dev/$(grep -l "PARTNAME=private\$" /sys/class/block/sd*/uevent | head -1 | cut -d/ -f5)
		mount -t ext4 -o ro,noload "$dev" /persist
	fi
	if [ ! -d $R/calibration ]; then
		cp -a /persist/calibration $R/calibration.tmp
		mkdir -p $R/calibration.tmp/online $R/calibration.tmp/online.tmp
		chown -R 1000:1000 $R/calibration.tmp
		mv $R/calibration.tmp $R/calibration
	fi

	# Android's post-fs-data directories (Holo's /data is plain storage on the root image)
	mkdir -p /data/misc/tracking /data/oculus/settings /data/oculus/preferences \
		/data/vendor/misc/sensors/controllercal /vision/tempMaps /data/local/tmp
	chmod 0777 /data/misc/tracking
	chown 1000:1000 /data/misc/tracking /data/oculus /data/oculus/settings /data/oculus/preferences \
		/data/vendor/misc/sensors /data/vendor/misc/sensors/controllercal /vision /vision/tempMaps
	# remote file system dirs the ADSP/SLPI/CDSP write through tftp_server (uid 2903): it cannot
	# create them itself, and without them the ADSP services stall
	for d in lpass slpi cdsp tn; do
		mkdir -p /data/vendor/tombstones/rfs/$d; chown 2903:2903 /data/vendor/tombstones/rfs/$d; chmod 0770 /data/vendor/tombstones/rfs/$d
	done
	chmod 0750 /data/oculus /data/oculus/settings /data/oculus/preferences

	# Property area for this stack only (bound over /dev/__properties__ in its units): the global
	# one plus vendor/default.prop and product/build.prop. ro.vndk.version=29 makes the bionic
	# linker load /system/etc/ld.config.29.txt (linker namespaces and the runtime APEX, e.g.
	# settingsserver -> libsqlite -> libandroidicu). The global area leaves it out on purpose so
	# the existing Android daemons and libhybris users keep their namespace-less loading.
	python3 /usr/local/lib/holo/mkproparea.py $R/__properties__.new \
		-s ro.hardware=monterey -s ro.boot.hardware=monterey -s ro.board.platform=msm8998 \
		-s hwservicemanager.ready=true -s service.sf.present_timestamp=0 \
		-s ro.boot.slot_suffix=_a -s sys.boot_completed=1 \
		-s persist.ovr.skipctrlfwupdate=1 		-s vendor.syncbosshal.fw_update_complete=true \
		/android/system/build.prop /android/system/vendor/build.prop \
		/android/system/vendor/default.prop /android/system/product/build.prop \
		/android/system/etc/prop.default /android/default.prop
	chmod 0644 $R/__properties__.new
	mv -f $R/__properties__.new $R/__properties__

	# powerhint.json for the power HAL: the same hint names, all mapped to one dummy file node,
	# so libperfmgr never writes (nor retries writing) a real CPU/GPU/bus frequency node
	: > $R/powerhint-dummy
	python3 - /system/vendor/etc/powerhint.json $R/powerhint.json $R/powerhint-dummy <<'PY'
import json, sys
src, out, dummy = sys.argv[1:4]
hints = sorted({a["PowerHint"] for a in json.load(open(src))["Actions"]})
cfg = {"Nodes": [{"Name": "Dummy", "Path": dummy, "Values": ["1", "0"], "DefaultIndex": 1,
                  "ResetOnInit": False}],
       "Actions": [{"PowerHint": h, "Node": "Dummy", "Duration": 0, "Value": "1"} for h in hints]}
json.dump(cfg, open(out, "w"), indent=1)
PY

	# device nodes, as Android's ueventd.rc sets them (devtmpfs resets them each boot)
	chmod 0666 /dev/ashmem
	# Android cpuset groups: libosutils moveToCpuSet() writes thread ids to /dev/cpuset/<group>/tasks and
	# aborts trackingservice when the file is missing. Plain RAM files (no pinning; the units set CPUAffinity).
	# Same idea as the Quest 2 port (github.com/URFAVF3MBOY/quest2-steamos, tracking-kit), own implementation.
	for g in tracking object_tracking mrsystem foreground background top-app system-background restricted camera-daemon; do
		mkdir -p $R/cpuset/$g; : > $R/cpuset/$g/tasks; chmod 0666 $R/cpuset/$g/tasks
	done
	chmod 0777 $R/cpuset $R/cpuset/*
	[ -e /dev/adsprpc-smd ] && { chown 1000:1000 /dev/adsprpc-smd; chmod 0664 /dev/adsprpc-smd; }
	# libadsprpc tries the secure node first and the ADSP channel is "secure" here: without it
	# GetInfo fails (EPERM) and trackingservice aborts in boltlib initDomains (error 59)
	[ -e /dev/adsprpc-smd-secure ] && { chown 1000:1000 /dev/adsprpc-smd-secure; chmod 0660 /dev/adsprpc-smd-secure; }
	# trackingservice creates virtual HID devices for the controllers (libossdk "Error while initializing
	# input device: -1" aborts it when /dev/uinput is not openable): root:600 by default
	[ -e /dev/uinput ] && { chown 1000:1000 /dev/uinput; chmod 0600 /dev/uinput; }
	true
}

install_units() {
	install -m 755 "$0" /usr/local/sbin/holo-android-tracking
	[ -f "$(dirname "$0")/q1anchord.py" ] && install -m 755 "$(dirname "$0")/q1anchord.py" /usr/local/sbin/q1anchord
	# Android's AID_NOBODY, for ashmemd (systemd needs the uid in the user database)
	mkdir -p /etc/sysusers.d
	echo 'u aid_nobody 9999 "Android nobody (ashmemd)" / -' > /etc/sysusers.d/android-tracking.conf
	systemd-sysusers /etc/sysusers.d/android-tracking.conf
	U=/etc/systemd/system
	# fakelogd writes the controller list now (ctlwatch read it from the journal, where the line no longer goes)
	rm -f $U/android-ctlwatch.service
	cat > $U/android-tracking-setup.service <<'EOF'
[Unit]
Description=Quest 1 tracking stack: /persist calibration copy, data dirs, device permissions
Requires=holo-android-blobs.service
After=holo-android-blobs.service android-modem.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/holo-android-tracking setup
EOF

	# unit name, description, user, extra [Service] lines, binary
	unit() {
		cat > $U/android-$1.service <<EOF
[Unit]
Description=Quest 1 tracking: $2
Requires=android-tracking-setup.service android-hwservicemanager.service
After=android-tracking-setup.service android-hwservicemanager.service android-modem.service
$6

[Service]
Environment=$E
User=$3
BindReadOnlyPaths=/persist $R/__properties__:/dev/__properties__
BindPaths=$R/calibration:/persist/calibration
BindPaths=$R/cpuset:/dev/cpuset
ExecStart=$5
Restart=on-failure
RestartSec=2
# Android init gives services 32768+ fds (trackingservice needs >1024: EMFILE otherwise)
LimitNOFILE=65536
$4
EOF
	}
	W="Wants=android-ovrstub.service
After=android-ovrstub.service"
	unit ovrstub "binder stubs for the system_server services (src/ovrstub)" root "" \
		"/usr/local/bin/ovrstub --then 2 --skip RuntimeIpcBroker" ""
	# Meta's real RuntimeIpcBroker (ovrstub's stand-in is skipped): trackingservice's anchor server
	# (SlamAnchorRuntimeIpcServer: spatial anchors, map persistence) connects to it; with the stub
	# it fails ("initServer failed"). It needs ovrstub's permission and package_native services.
	unit runtimeipcbroker "Meta RuntimeIpcBroker (anchor server of trackingservice connects to it)" root \
		'ExecStartPre=/bin/sh -c "n=0; while [ $$n -lt 60 ]; do /system/bin/service check permission | grep -q found && /system/bin/service check package_native | grep -q found && exit 0; n=$$((n+1)); sleep 1; done; exit 1"' \
		/system/bin/runtimeipcbroker "$W
PartOf=android-ovrstub.service
Before=android-trackingservice.service"
	unit settingsserver "Oculus preferences store (settingsserver)" 1000 "Group=1000" \
		/system/bin/settingsserver "$W"
	unit vrfocus "VR focus arbitration (vrfocusserver)" root "SupplementaryGroups=1000" \
		/system/bin/vrfocusserver "$W"
	# libcutils' ashmem_create_region asks ashmemd for /dev/ashmem fds first ("Failed to get
	# IAshmemDeviceService" otherwise, then falls back to opening /dev/ashmem itself). Android's
	# nobody is uid 9999: servicemanager refuses addService from app-range uids (>= 10000), which
	# Holo's nobody (65534) is.
	unit ashmemd "ashmem device service (ashmemd)" 9999 "Group=9999" /system/bin/ashmemd ""
	# FastRPC listener of the ADSP root PD (Android: vendor.adsprpcd). trackingservice's Bolt library
	# calls the Hexagon skel on the ADSP; without this daemon the first fastrpc invoke never returns.
	unit adsprpcd "FastRPC listener for the ADSP (adsprpcd)" root 'Environment="ADSP_LIBRARY_PATH=/system/lib/rfsa/adsp;/vendor/lib/rfsa/adsp;/vendor/dsp;/dsp"' /system/vendor/bin/adsprpcd ""
	# IDeviceTelemetry is in the framework VINTF manifest: trackingservice waits for it forever
	unit telemetry-hal "device telemetry HAL (local, no network)" 1000 "Group=1000" \
		/system/bin/hw/vendor.oculus.hardware.telemetry@1.0-service "$W"
	# IPower is in the vendor VINTF manifest (libtrackingutils waits for it). libperfmgr resets
	# every node of /vendor/etc/powerhint.json at start (seen live: GPU devfreq min_freq went
	# 257 -> 180 MHz) and applies VR/launch hints later. It reads a generated powerhint.json whose
	# only node is a dummy file (see setup), and the real nodes are also inaccessible in its mount
	# namespace: it registers IPower/IClocks and cannot change CPU/GPU/bus frequencies (Monado
	# and SteamVR tune those themselves).
	K=/sys/class/kgsl/kgsl-3d0
	unit power-hal "power HAL (registered, cannot touch frequencies)" root "InaccessiblePaths=-/sys/devices/system/cpu/cpu4/cpufreq/scaling_max_freq -/sys/devices/system/cpu/cpu4/cpufreq/scaling_min_freq -/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq -/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq
InaccessiblePaths=-/sys/class/devfreq/soc:qcom,cpubw/max_freq -/sys/class/devfreq/soc:qcom,cpubw/min_freq -/sys/class/devfreq/soc:qcom,gpubw/max_freq -/sys/class/devfreq/soc:qcom,gpubw/min_freq -/sys/class/devfreq/soc:qcom,mincpubw/max_freq -/sys/class/devfreq/soc:qcom,mincpubw/min_freq
InaccessiblePaths=-$K/devfreq/max_freq -$K/devfreq/min_freq -$K/clk_on -$K/force_clk_on -$K/force_rail_on -$K/idle_timer
InaccessiblePaths=-/dev/cpu_dma_latency -/proc/sys/kernel/sched_boost -/dev/stune
BindReadOnlyPaths=$R/powerhint.json:/system/vendor/etc/powerhint.json" \
		/system/vendor/bin/hw/android.hardware.power@1.3-service-libperfmgr "$W"
	unit iad "IPD/IAD slider HAL" 1000 "Group=1000" \
		/system/vendor/bin/hw/vendor.oculus.hardware.sensors@1.0-iad "$W"
	# The sensors HAL runs as root (Android: system + SYS_NICE) so it can open /dev/syncboss*,
	# the camera nodes and /dev/ion without changing their permissions for everyone.
	unit sensors-hal "sensors HAL (SyncBoss IMU/controllers/cameras owner)" root \
		"InaccessiblePaths=-/system/vendor/firmware/lcon_archive.bin -/system/vendor/firmware/lcon-downgrade.bin
ExecStartPre=/usr/local/sbin/holo-android-tracking sb-free
# never re-grab SyncBoss automatically
Restart=no
CPUAffinity=0-3" \
		/system/vendor/bin/hw/vendor.oculus.hardware.sensors@1.0-service "$W"
	# everything below needs the sensors HAL (ICameraProvider etc.): bound to it, so they can
	# only run while it runs, and stop with it (Android: "onrestart restart ...")
	H="Requires=android-sensors-hal.service
After=android-sensors-hal.service
PartOf=android-sensors-hal.service"
	unit cameramuxmode "camera mux mode service" 1000 "Group=1000
SupplementaryGroups=video
AmbientCapabilities=CAP_SYS_NICE" /system/bin/cameramuxmodeservice "$W
$H"
	unit calibration "calibration server (writes only the tmpfs calibration copy)" 1000 "Group=1000" \
		/system/bin/calibrationserver "$W
$H"
	unit trackingservice "trackingservice (head/controller/hand tracking)" 1000 "Group=1000
SupplementaryGroups=aid_inet render
AmbientCapabilities=CAP_SYS_NICE
IOSchedulingClass=realtime
IOSchedulingPriority=4
CPUAffinity=0-3
InaccessiblePaths=-/system/vendor/firmware/lcon_archive.bin -/system/vendor/firmware/lcon-downgrade.bin" \
		/system/bin/trackingservice "$W android-settingsserver.service android-vrfocus.service android-iad.service android-telemetry-hal.service android-power-hal.service android-ashmemd.service
Wants=android-settingsserver.service android-vrfocus.service android-iad.service android-telemetry-hal.service android-power-hal.service android-ashmemd.service
Wants=android-cameramuxmode.service android-calibration.service
Wants=android-runtimeipcbroker.service android-q1anchord-restore.service
After=android-runtimeipcbroker.service android-q1anchord-restore.service
$H"
	# q1trackd: libossdk client (root: vrfocusserver grants uid 0 background tracking focus),
	# started from /data/local/tmp for the linker's [unrestricted] namespace section
	unit q1trackd "q1trackd (Meta tracking poses -> /run/q1track/poses for Monado)" root \
		"BindReadOnlyPaths=/usr/local/lib/q1track/q1trackd:/data/local/tmp/q1trackd
TemporaryFileSystem=/data/local/tmp" \
		"/data/local/tmp/q1trackd serve 500" "$W
Wants=android-trackingservice.service
After=android-trackingservice.service
PartOf=android-sensors-hal.service"
	# q1anchord: persists Meta's map (spatial anchor) and tells driver_quest1 whether its frame is the persisted one
	unit q1anchord "q1anchord (persisted tracking map, /run/q1track/frame-state)" root "" /usr/local/sbin/q1anchord "$W
Wants=android-trackingservice.service
After=android-trackingservice.service
PartOf=android-sensors-hal.service"
	# the engine loads the persisted map when trackingservice starts: q1anchord first puts its copy back if
	# the engine emptied mapdb (a unit of its own: trackingservice's LD_PRELOAD, a bionic library, breaks
	# glibc programs started from its unit)
	cat > $U/android-q1anchord-restore.service <<'EOF'
[Unit]
Description=q1anchord: restore the persisted tracking map if mapdb was emptied
Before=android-trackingservice.service

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/q1anchord restore
EOF
	# Meta mode survives reboots: when /etc/quest1-tracking says "meta", start Meta's stack at boot,
	# before Monado (which then leaves SyncBoss to the sensors HAL)
	cat > $U/android-tracking-boot.service <<'EOF'
[Unit]
Description=Quest 1 tracking: Meta's stack at boot when /etc/quest1-tracking is "meta"
After=holo-android-blobs.service android-hwservicemanager.service android-modem.service
Before=monado.service

[Service]
Type=oneshot
RemainAfterExit=yes
TimeoutStartSec=120
ExecStart=/usr/local/sbin/holo-android-tracking boot

[Install]
WantedBy=multi-user.target
EOF
	systemctl daemon-reload
	systemctl enable android-tracking-boot.service
	echo "units written; only android-tracking-boot is enabled (Meta's stack at boot in meta mode)." \
		"Without SyncBoss: holo-android-tracking base." \
		"With SyncBoss (monado.service stopped): holo-android-tracking trial"
}

BASE="android-ovrstub android-ashmemd android-adsprpcd android-settingsserver android-vrfocus android-iad android-telemetry-hal android-power-hal"
SB="android-q1anchord android-q1trackd android-trackingservice android-runtimeipcbroker android-calibration android-cameramuxmode android-sensors-hal"
TE=/sys/bus/spi/devices/spi12.0/control/te_timestamp
cli() { # trackinginterface_cli as root (vrfocus grants uid 0) with the stack's property area
	unshare -m sh -c 'mount --bind /run/q1track/__properties__ /dev/__properties__ && exec env '"$E"' /system/bin/trackinginterface_cli "$@"' cli "$@"
}
fb_users() { # processes holding /dev/fb0 open
	for f in /proc/[0-9]*/fd/*; do
		[ "$(readlink "$f" 2>/dev/null)" = /dev/fb0 ] && { p=${f#/proc/}; echo "${p%%/*}"; }
	done | sort -u | while read -r p; do echo "$p $(cat /proc/$p/comm 2>/dev/null)"; done
}
sb_free() { # ExecStartPre of the sensors HAL: nobody else may drive SyncBoss (powerstate is shared)
	# SyncBoss's kernel threads (SPI polling, RT) on CPUs 0-3 like Meta's tracking, as Android's init.monterey.rc does
	# (they ran on all eight, next to vrcompositor's render thread on the big cores)
	echo 0f > /sys/devices/virtual/misc/syncboss0/spi/control/cpu_affinity 2>/dev/null
	# one find, not a readlink per descriptor: with Steam up there are thousands, and the per-fd forks outlasted
	# the unit's start timeout (2026-11-07: the stack could not be restarted)
	busy=$(find /proc/[0-9]*/fd -maxdepth 1 \( -lname /dev/syncboss0 -o -lname /dev/syncboss_control0 \
		-o -lname /dev/syncboss_stream0 \) 2>/dev/null | cut -d/ -f3 | sort -u |
		while read -r p; do echo "$p $(cat /proc/$p/comm 2>/dev/null)"; done)
	[ -z "$busy" ] && return 0
	echo "SyncBoss is in use (Monado in IMU mode? holo-android-tracking meta-on): $busy"
	return 1
}
sb_users() { # processes holding /dev/syncboss* open
	find /proc/[0-9]*/fd -maxdepth 1 -lname "/dev/syncboss*" 2>/dev/null | cut -d/ -f3 | sort -u |
		while read -r p; do echo "$p $(cat /proc/$p/comm 2>/dev/null)"; done
}
pids_of() { systemctl show -p MainPID --value "$@" | grep -v '^0$'; }
snapshot() { # $1 = output file: thread states and pending binder transactions of the stack
	for u in android-trackingservice android-sensors-hal android-cameramuxmode android-calibration 		android-vrfocus android-ovrstub; do
		p=$(pids_of $u.service) || continue
		echo "== $u pid $p: $(systemctl show -p NRestarts --value $u.service) restarts"
		for t in /proc/$p/task/*; do
			echo "  $(cat $t/comm 2>/dev/null) $(cat $t/wchan 2>/dev/null) $(cut -d' ' -f1 $t/syscall 2>/dev/null)"
		done | sort | uniq -c
		grep -E "transaction|thread" /sys/kernel/debug/binder/proc/$p 2>/dev/null | grep -v "^  thread.*l 1[12] need_return 0 tr 0$" | head -40
	done > "$1" 2>&1
}
trial() { # one bounded Path M run; Monado must already be stopped (SyncBoss has one owner)
	D=/root/q1track-trial-$(date +%Y%m%d-%H%M%S)
	mkdir -p "$D"
	if systemctl is-active -q monado.service || [ -n "$(sb_users)" ]; then
		echo "SyncBoss in use (monado.service, or these processes):"
		sb_users
		exit 1
	fi
	T=$(date +%s)
	# display TE: the sensors HAL drops IMU samples while it is frozen (panels off). q1black
	# lights the panels black and powers them down again when it exits, but only when nothing
	# else has /dev/fb0 open: two processes switching the panel power can wedge mdss
	# ("mdss_fb_pan_idle ... wait for idle timeout", only a reboot recovers).
	B=
	te0=$(cat $TE); sleep 0.1
	fb_users > "$D/fb0-users.txt"
	if [ -s "$D/fb0-users.txt" ]; then
		echo "TE frozen and /dev/fb0 in use, no q1black:"; cat "$D/fb0-users.txt"
	elif [ "$(cat $TE)" = "$te0" ] && [ -x /usr/local/bin/q1black ]; then
		LD_PRELOAD=/opt/hybris/lib/libbionictls.so /usr/local/bin/q1black 0 500 > "$D/q1black.txt" 2>&1 &
		B=$!
		sleep 1
	fi
	systemctl start $BASE
	systemctl start android-sensors-hal android-cameramuxmode android-calibration android-trackingservice || true
	for i in $(seq 1 "${TRIAL_SECONDS:-60}"); do
		sleep 1
		cli getheadtrackingdata json > "$D/head.$i.json" 2>&1
		echo "$i $(cat $TE) $(env $E /system/bin/service check tracking 2>&1)" >> "$D/te-tracking.txt"
		case $i in 5|15|30) snapshot "$D/snapshot.$i.txt" ;; esac
	done
	snapshot "$D/snapshot.end.txt"
	cli ls > "$D/ls.txt" 2>&1
	cli getcontrollertrackingdata json > "$D/controllers.json" 2>&1
	timeout 10 env $E /system/bin/service list > "$D/services.txt" 2>&1
	timeout 10 env $E /system/bin/lshal > "$D/lshal.txt" 2>&1
	systemctl status --no-pager $BASE $SB > "$D/units.txt" 2>&1
	systemctl stop $SB
	[ -n "$B" ] && { kill $B; wait $B || true; }
	sleep 2
	sb_users > "$D/syncboss-users-after-stop.txt"
	journalctl --since=@$T --no-pager -o short-monotonic > "$D/journal.txt"
	coredumpctl list --no-pager --since=@$T > "$D/coredumps.txt" 2>&1
	dmesg | tail -300 > "$D/dmesg.txt"
	echo "results in $D; SyncBoss units stopped (base stubs left running). SyncBoss users now:"
	cat "$D/syncboss-users-after-stop.txt"
}
# monado-trial [seconds]: stop Monado, trial, start Monado again. It breaks SteamVR tracking for
# everyone meanwhile: run it through the shared restart lock (sh /root/q1restart.sh <who> <why> ...).
monado_trial() {
	systemctl stop monado.service
	sleep 1
	TRIAL_SECONDS=${1:-45} trial || true
	systemctl start monado.service
	sleep 3
	echo "monado: $(systemctl is-active monado.service), vrserver: $(pgrep -x vrserver || echo none)"
}

meta_start() { # Meta's stack, in order (the sensors HAL first: it takes SyncBoss)
	systemctl start $BASE
	systemctl start android-sensors-hal
	systemctl start android-cameramuxmode android-calibration android-trackingservice android-q1trackd android-q1anchord
}
# restart: Meta's stack from the sensors HAL up, for a controller it did not take. The sensors HAL and
# trackingservice enumerate the controllers when they start: one asleep then and woken later (2026-11-08: the
# right one, battery out and in) gets its radio link back but never reaches SteamVR, though ovrstub announces it
# every 3 s. ovrstub then replays what trackingservice saw at boot, RUNNING and a second later STANDBY: given
# STANDBY alone (ovrstub already in it), trackingservice never powered the cameras on (front LED off, rotation only).
meta_restart() {
	systemctl stop android-q1anchord android-q1trackd android-trackingservice android-runtimeipcbroker 		android-calibration android-cameramuxmode android-sensors-hal
	sleep 2
	t0=$(date +%s)
	meta_start
	# once the new trackingservice has registered with ovrstub's vrpowermanager (~5 s)
	for i in $(seq 60); do
		journalctl -u android-ovrstub --since "@$t0" -o cat --no-pager | grep -q "vrpowermanager: client registered" && break
		sleep 1
	done
	sleep 2
	p=$(systemctl show -p MainPID --value android-ovrstub)
	[ "${p:-0}" -gt 0 ] && { kill -USR1 "$p"; sleep 1.5; kill -HUP "$p"; }
}
# meta-on: Monado without SyncBoss + Meta's stack. Restarts Monado: through /root/q1restart.sh.
meta_on() {
	echo meta > /etc/quest1-tracking
	systemctl stop monado.service
	sleep 1
	meta_start
	systemctl start monado.service
	sleep 3
	echo "monado: $(systemctl is-active monado.service)," \
		"trackingservice: $(systemctl is-active android-trackingservice)," \
		"q1trackd: $(systemctl is-active android-q1trackd)"
}
# meta-off: back to Monado's own SyncBoss IMU (3DoF). Restarts Monado: through /root/q1restart.sh.
meta_off() {
	rm -f /etc/quest1-tracking
	systemctl stop monado.service
	systemctl stop $SB
	sleep 2
	systemctl start monado.service
	sleep 3
	echo "monado: $(systemctl is-active monado.service); SyncBoss users: $(sb_users | tr '\n' ' ')"
}

case "$1" in
setup) setup ;;
install) install_units ;;
base) systemctl start $BASE ;;
trial) trial ;;
monado-trial) shift; monado_trial "$@" ;;
meta-on) meta_on ;;
meta-off) meta_off ;;
boot) [ "$(cat /etc/quest1-tracking 2>/dev/null)" = meta ] && meta_start || true ;;
sb-free) sb_free ;;
stop) systemctl stop $SB $BASE ;;
restart) meta_restart ;;
cli) shift; cli "$@" ;;
*) echo "usage: $0 setup|install|base|trial|monado-trial|meta-on|meta-off|boot|sb-free|stop|cli <trackinginterface_cli args>" >&2; exit 2 ;;
esac
