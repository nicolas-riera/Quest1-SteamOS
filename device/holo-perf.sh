#!/bin/sh
# Keep the Quest 1 responsive under memory pressure (run as root on the native Holo system).
# The 3.8 GB RAM is shared by the CPU and the GPU; Steam + SteamVR + the dashboard's CEF
# processes push it into swap. The 4 GB swap file on userdata thrashes so hard that the whole
# system stalls (load 20+, sshd no longer answers). So:
#   - zram (compressed RAM swap, lz4) with a higher priority than the swap file;
#   - sshd protected from the OOM killer and scheduled ahead of the desktop/VR processes.
set -e

cat > /usr/local/sbin/quest1-zram <<'EOF'
#!/bin/sh
# quest1-zram start|stop: 1.5 GB zram swap (lz4), used before the swap file
Z=/sys/block/zram0
case "$1" in
start)
	grep -q /dev/zram0 /proc/swaps && exit 0
	echo 1 > $Z/reset 2>/dev/null || true
	echo lz4 > $Z/comp_algorithm
	echo 4 > $Z/max_comp_streams 2>/dev/null || true
	echo 1536M > $Z/disksize
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
Description=Quest 1 compressed RAM swap (zram, lz4)
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

# swap less eagerly: zram is fast, but the file swap is not
cat > /etc/sysctl.d/90-quest1-vm.conf <<'EOF'
vm.swappiness=60
vm.page-cluster=0
EOF

mkdir -p /etc/systemd/system/sshd.service.d
cat > /etc/systemd/system/sshd.service.d/quest1-priority.conf <<'EOF'
# stay reachable when the headset is overloaded (inherited by the sessions)
[Service]
OOMScoreAdjust=-900
Nice=-10
EOF

systemctl daemon-reload
systemctl enable quest1-zram.service
systemctl start quest1-zram.service
sysctl -q --system || true
echo "holo-perf: zram swap on, sshd protected (restart sshd to apply: systemctl restart sshd)"
