#!/bin/sh
# Hand tracking extras (docs/hand-tracking.md): q1-handviz.service, the hand skeleton and the Oculus/Menu mark drawn as SteamVR
# overlays on tracked hands (src/q1handviz, built by tools/build-native.sh as /usr/local/bin/q1-handviz). Meta's hand
# daemon (android-q1handsd) is in holo-android-tracking.sh. Hands stay off until /etc/quest1-hands says auto or hands.
# Usage: holo-hands.sh install|uninstall
set -e
case "$1" in
install) ;;
uninstall)
	systemctl disable --now q1-handviz.service 2>/dev/null || true
	rm -f /etc/systemd/system/q1-handviz.service /etc/systemd/system/quest1-steamvr.service.wants/q1-handviz.service
	systemctl daemon-reload
	exit 0 ;;
*) echo "usage: $0 install|uninstall" >&2; exit 2 ;;
esac
cat > /etc/systemd/system/q1-handviz.service <<'UNIT'
[Unit]
Description=Hand skeleton and system-gesture mark as SteamVR overlays (tracked hands only)
After=quest1-steamvr.service
BindsTo=quest1-steamvr.service

[Service]
User=steamos
Environment=XDG_RUNTIME_DIR=/run/user/1000 HOME=/home/steamos
ExecStart=/usr/local/bin/q1-handviz
Restart=always
RestartSec=5
Nice=5
MemoryLow=8M
UNIT
systemctl daemon-reload
systemctl add-wants quest1-steamvr.service q1-handviz.service
