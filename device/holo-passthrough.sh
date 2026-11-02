#!/bin/sh
# Passthrough pieces on the headset (docs/passthrough.md). Run as root: sh holo-passthrough.sh install
#  - persistent camtap drop-in (LD_PRELOAD libq1camtap into android-trackingservice: camera ring /dev/shm/quest1-cameras)
#  - q1-camoverlay.service: head-locked camera view overlay, started with SteamVR (binary /usr/local/bin/q1-camoverlay,
#    built by src/camtap/build_overlay.sh; libq1camtap.so from src/camtap/build.sh in /data/local/tmp)
#  - /usr/local/bin/q1-passthrough on|off|toggle
set -e
[ "$1" = install ] || { echo "usage: $0 install" >&2; exit 2; }
mkdir -p /etc/systemd/system/android-trackingservice.service.d
cat > /etc/systemd/system/android-trackingservice.service.d/camtap.conf <<EOT
[Service]
Environment=LD_PRELOAD=/data/local/tmp/libq1camtap.so
EOT
cat > /etc/systemd/system/q1-camoverlay.service <<EOT
[Unit]
Description=Passthrough: head-locked camera view overlay (shown while /tmp/quest1-passthrough exists)
After=quest1-steamvr.service
BindsTo=quest1-steamvr.service

[Service]
User=steamos
Environment=XDG_RUNTIME_DIR=/run/user/1000 HOME=/home/steamos
ExecStart=/usr/local/bin/q1-camoverlay
Restart=always
RestartSec=5
Nice=5
EOT
cat > /usr/local/bin/q1-passthrough <<'EOT'
#!/bin/sh
# q1-passthrough [on|off|toggle]: show/hide the head-locked camera view
f=/tmp/quest1-passthrough
systemctl is-active -q q1-camoverlay || systemctl start q1-camoverlay
case "$1" in
on) runuser -u steamos -- touch $f ;;
off) rm -f $f ;;
*) if [ -e $f ]; then rm -f $f; else runuser -u steamos -- touch $f; fi ;;
esac
[ -e $f ] && echo on || echo off
EOT
chmod 755 /usr/local/bin/q1-passthrough
systemctl daemon-reload
systemctl add-wants quest1-steamvr.service q1-camoverlay.service
