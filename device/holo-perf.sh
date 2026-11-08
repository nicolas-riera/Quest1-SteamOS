#!/bin/sh
# Keep the Quest 1 responsive under memory pressure (run as root on the native Holo system).
# The 3.8 GB RAM is shared by the CPU and the GPU; Steam + SteamVR + the dashboard's CEF
# processes push it into swap. The 4 GB swap file on userdata thrashes so hard that the whole
# system stalls (load 20+, sshd no longer answers). So:
#   - zram (compressed RAM swap, zstd) with a higher priority than the swap file;
#   - sshd protected from the OOM killer and scheduled ahead of the desktop/VR processes.
set -e

cat > /usr/local/sbin/quest1-zram <<'EOF'
#!/bin/sh
# quest1-zram start|stop: 3 GB zram swap (zstd), used before the swap file. SteamVR + Steam fill 1.5 GB
# (2026-11-02: full, then the slow swap file on the loop device, and vrcompositor's pages swapped out).
# zstd: 256 MB of the headset's swapped pages (zero pages left out) took 60 MB against 94 MB with lz4
# (2026-11-05), for 19 us per 4 KiB page read back instead of 7 (the swap file: milliseconds) and
# compression ~3x slower, paid by kswapd during bursts only (~1 page/s swapped at rest)
Z=/sys/block/zram0
case "$1" in
start)
	grep -q /dev/zram0 /proc/swaps && exit 0
	echo 1 > $Z/reset 2>/dev/null || true
	echo zstd > $Z/comp_algorithm 2>/dev/null || echo lz4 > $Z/comp_algorithm
	echo 4 > $Z/max_comp_streams 2>/dev/null || true
	echo 3072M > $Z/disksize
	mkswap /dev/zram0 >/dev/null
	swapon -p 100 /dev/zram0
	;;
stop)
	swapoff /dev/zram0 2>/dev/null || true
	echo 1 > $Z/reset
	;;
esac
EOF
chmod 755 /usr/local/sbin/quest1-zram

cat > /etc/systemd/system/quest1-zram.service <<'EOF'
[Unit]
Description=Quest 1 compressed RAM swap (zram, zstd)
DefaultDependencies=no
Before=swap.target
After=systemd-modules-load.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/quest1-zram start
ExecStop=/usr/local/sbin/quest1-zram stop

[Install]
WantedBy=swap.target
EOF

# Memory runs short (Steam's webhelpers, SteamVR): compress idle anonymous memory into zram rather
# than drop the file cache, which comes back from the loop-mounted image (I/O wait, kswapd at 50 %,
# Steam's audio crackling until it settles; 2026-11-03). The file swap has the lower priority, it
# only takes what zram can't. min_free_kbytes: room for allocations without direct reclaim stalls.
cat > /etc/sysctl.d/90-quest1-vm.conf <<'EOF'
vm.swappiness=100
vm.page-cluster=0
vm.min_free_kbytes=49152
# Android's values on this headset (init rc): write dirty pages back early and in small amounts, so that a burst of
# writes (Steam's caches, the journal) never queues hundreds of MB in front of the page reads everything waits for
vm.dirty_background_ratio=5
vm.dirty_expire_centisecs=200
EOF

# Block devices as Android tunes them: CFQ without idling (slice_idle/group_idle 0: idling is for spinning disks, on
# flash it holds reads back), 128 KiB read-ahead (512 by default: under memory pressure every code page fault read
# 512 KiB). On 2026-11-08 the headset froze twice under memory pressure with Steam busy: every process that needed a
# page from the disk (code, the ext4 journal) waited for minutes (q1-hangwatch), SSH included.
cat > /usr/local/sbin/quest1-io <<'EOF'
#!/bin/sh
for q in /sys/block/sd[a-z]/queue /sys/block/loop[0-9]*/queue; do
	[ -d "$q" ] || continue
	echo 128 > "$q/read_ahead_kb" 2>/dev/null
	[ -e "$q/iosched/slice_idle" ] && echo 0 > "$q/iosched/slice_idle"
	[ -e "$q/iosched/group_idle" ] && echo 0 > "$q/iosched/group_idle"
done
exit 0
EOF
chmod 755 /usr/local/sbin/quest1-io
cat > /etc/systemd/system/quest1-io.service <<'EOF'
[Unit]
Description=Quest 1 block device tuning (Android's values)
DefaultDependencies=no
After=local-fs.target

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/quest1-io

[Install]
WantedBy=sysinit.target
EOF

# Reclaim the Steam client's memory before SteamVR's, Monado's, the passthrough's and the tracking's (cgroup v2
# memory.low). On this 4.4 kernel a cgroup is only protected while all its ancestors are below their own memory.low
# too (mem_cgroup_low()), so system.slice gets one larger than the RAM: until 2026-11-08 it had none and these
# protections did nothing. steam.service has none: it is what goes to swap first.
mkdir -p /etc/systemd/system/system.slice.d
printf '[Slice]\nMemoryLow=8G\n' >/etc/systemd/system/system.slice.d/quest1-memory.conf
for u in quest1-steamvr:700M quest1-frame-dashboard:400M monado:150M android-trackingservice:150M \
	android-sensors-hal:64M android-q1trackd:32M android-ovrstub:16M q1-ptprovider:64M q1-camoverlay:64M; do
	mkdir -p /etc/systemd/system/${u%%:*}.service.d
	printf '[Service]\nMemoryLow=%s\n' "${u#*:}" >/etc/systemd/system/${u%%:*}.service.d/quest1-memory.conf
done

mkdir -p /etc/systemd/system/sshd.service.d
cat > /etc/systemd/system/sshd.service.d/quest1-priority.conf <<'EOF'
# stay reachable when the headset is overloaded (inherited by the sessions)
[Service]
OOMScoreAdjust=-900
Nice=-10
EOF

# q1-hangwatch: what blocks the headset (D-state threads with their kernel stacks, spinning real-time threads), into
# the kernel log that pstore keeps across a reboot (q1-hangwatch.py)
install -m 755 "$(dirname "$0")/q1-hangwatch.py" /usr/local/sbin/q1-hangwatch
cat > /etc/systemd/system/q1-hangwatch.service <<'EOF'
[Unit]
Description=Log hung and spinning threads to the kernel log (pstore)
# early: udev's helpers hung for 3 min at some boots (2026-11-08)
DefaultDependencies=no
Conflicts=shutdown.target
Before=shutdown.target

[Service]
ExecStart=/usr/local/sbin/q1-hangwatch
Restart=always
RestartSec=5

[Install]
WantedBy=sysinit.target
EOF

systemctl daemon-reload
systemctl enable quest1-zram.service q1-hangwatch.service quest1-io.service
systemctl start quest1-zram.service q1-hangwatch.service quest1-io.service
sysctl -q --system || true
echo "holo-perf: zram swap on, sshd protected (restart sshd to apply: systemctl restart sshd)"

# ssh sessions: pam_systemd creates a logind session + scope per connection and the scope is never
# reaped here ("abandoned"), so every ssh command leaked a session (400+ after an hour of agent polling)
# and slowed logind/systemd. sshd gets the system-login session stack without pam_systemd.
[ -f /etc/pam.d/sshd.orig ] || cp /etc/pam.d/sshd /etc/pam.d/sshd.orig
cat > /etc/pam.d/sshd <<'PAM'
#%PAM-1.0
auth      include   system-remote-login
account   include   system-remote-login
password  include   system-remote-login
session   optional  pam_loginuid.so
session   optional  pam_keyinit.so       force revoke
session   include   system-auth
session   optional  pam_lastlog2.so      silent
session   optional  pam_motd.so
session   optional  pam_mail.so          dir=/var/spool/mail standard quiet
session   optional  pam_umask.so
session   required  pam_env.so
PAM
