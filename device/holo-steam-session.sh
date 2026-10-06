#!/bin/sh
# Steam session on the native Quest 1: the arm64 Steam client (Big Picture) on a virtual X server,
# shown in the headset by xscreen (an OpenXR quad layer on Monado) and controllable from the PC over VNC.
#   xvfb.service      Xvfb :1, 1280x800, framebuffer exported as an XWD file in /run/xvfb
#   steam.service     steam-q1 -gamepadui as steamos (restarts itself after client self-updates)
#   x11vnc.service    VNC on port 5900, password in /etc/x11vnc/passwd
#   steam-screen.service  shows the Steam screen in VR (xscreen); the Monado target keeps the
#                     panels off while the proximity sensor says the headset is not worn
# Usage (on the headset): holo-steam-session.sh install
set -e

cat > /etc/systemd/system/xvfb.service <<'EOF'
[Unit]
Description=Virtual X server for the Steam client (:1)

[Service]
ExecStartPre=/usr/bin/mkdir -p /run/xvfb
ExecStart=/usr/sbin/Xvfb :1 -screen 0 1280x800x24 -fbdir /run/xvfb -nolisten tcp
Restart=on-failure

[Install]
WantedBy=multi-user.target
EOF

cat > /etc/systemd/system/steam.service <<'EOF'
[Unit]
Description=Steam client (arm64, Big Picture)
Requires=xvfb.service
After=xvfb.service network-online.target

[Service]
User=steamos
# steamos needs gid 3003 (aid_inet) for IP sockets: the Android kernel has PARANOID_NETWORK
Environment=DISPLAY=:1 HOME=/home/steamos USER=steamos XDG_RUNTIME_DIR=/run/user/1000
ExecStartPre=+/usr/bin/install -d -o steamos -g steamos -m 700 /run/user/1000
ExecStart=/usr/local/bin/steam-q1 -gamepadui
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

cat > /etc/systemd/system/x11vnc.service <<'EOF'
[Unit]
Description=VNC access to the Steam screen (:1)
Requires=xvfb.service
After=xvfb.service

[Service]
ExecStart=/usr/local/bin/x11vnc -display :1 -rfbauth /etc/x11vnc/passwd -forever -shared -rfbport 5900 -noxdamage -nowf -noscr
Restart=on-failure
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF

cat > /usr/local/bin/steam-screen <<'EOF'
#!/bin/sh
# steam-screen [width m] [distance m] [timeout s]: show the Steam screen in the headset.
# kill -USR1 $(pidof xscreen) recenters it in front of the current gaze.
exec /usr/local/bin/xr-run /usr/local/bin/xscreen /run/xvfb/Xvfb_screen0 "${1:-1.6}" "${2:-1.4}" "${3:-0}"
EOF
chmod 755 /usr/local/bin/steam-screen

cat > /etc/systemd/system/steam-screen.service <<'EOF'
[Unit]
Description=Steam screen in VR (OpenXR quad layer on Monado)
Requires=monado.service xvfb.service
After=monado.service xvfb.service steam.service

[Service]
ExecStart=/usr/local/bin/steam-screen
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
EOF

mkdir -p /etc/x11vnc
[ -f /etc/x11vnc/passwd ] || /usr/local/bin/x11vnc -storepasswd quest1 /etc/x11vnc/passwd
chmod 600 /etc/x11vnc/passwd
systemctl daemon-reload
systemctl enable xvfb.service steam.service x11vnc.service steam-screen.service
