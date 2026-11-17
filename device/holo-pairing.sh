#!/bin/sh
# Steam's Settings > Controllers > "Add controllers" for the Touch controllers (docs/controller-pairing.md).
# Run as root: sh holo-pairing.sh install   (uninstall removes it)
#  - q1-pairing.service: src/q1pairing (/usr/local/bin/q1-pairing, built by tools/build-native.sh) answers vrserver's
#    "frame_controller_pairing" mailbox. As steamos (as root vrclient fails with VRInitError_Init_Internal), after SteamVR.
#    Dry run by default: it never pairs. Pairing for real needs /etc/quest1/pairing-real AND a working q1-pair-hal.
#  - /usr/local/libexec/q1-pair-hal: the real pairing step (a stub until the hwbinder client exists).
#  - /run/q1pairing (RuntimeDirectory): the Identify requests q1-pairing leaves for driver_quest1.
set -e
case "$1" in
install) ;;
uninstall)
	systemctl disable --now q1-pairing.service 2>/dev/null || true
	rm -f /etc/systemd/system/q1-pairing.service /etc/systemd/system/quest1-steamvr.service.wants/q1-pairing.service \
		/usr/local/libexec/q1-pair-hal
	systemctl daemon-reload
	exit 0 ;;
*) echo "usage: $0 install|uninstall" >&2; exit 2 ;;
esac

install -D -m 755 "$(dirname "$0")/q1-pair-hal" /usr/local/libexec/q1-pair-hal

cat > /etc/systemd/system/q1-pairing.service <<'EOF'
[Unit]
Description=Steam's Add controllers page for the Touch controllers (vrserver mailbox frame_controller_pairing)
After=quest1-steamvr.service
BindsTo=quest1-steamvr.service

[Service]
User=steamos
Environment=XDG_RUNTIME_DIR=/run/user/1000 HOME=/home/steamos
RuntimeDirectory=q1pairing
ExecStart=/usr/local/bin/q1-pairing
Restart=always
RestartSec=5
Nice=5
MemoryLow=8M
EOF

systemctl daemon-reload
systemctl add-wants quest1-steamvr.service q1-pairing.service
