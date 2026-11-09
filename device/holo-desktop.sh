#!/bin/sh
# Desktop mode, as on the Steam Frame: Steam's "+" menu entry "Desktop" (X-Steam-Special=Desktop, from the Frame's
# steamdeck-kde-presets, copied out of the Frame image by installer/build_image.py with the rest of that package:
# Vapor look-and-feel, SteamOS wallpapers, the NoDisplay entries that keep developer tools out of the "+" menu)
# runs steamos-nested-desktop, a nested KDE Plasma (Wayland). docs/desktop.md.
#  - /usr/local/bin/steamos-nested-desktop: the Frame's script, with q1desktop (built by tools/build-native.sh)
#    and an Xvfb in gamescope's place;
#  - the look-and-feel the Frame's steamos-set-plasma-theme sets (it checks for vrdevice_path, Frame only):
#    com.valve.vapor.deck.desktop, i.e. dark Vapor colours, the SteamOS wallpaper, the Steam Deck logo on the
#    application launcher (applied by Plasma to a new user's desktop).
# Run as root: sh holo-desktop.sh install
set -e
[ "$1" = install ] || { echo "usage: $0 install" >&2; exit 2; }
D=$(cd "$(dirname "$0")" && pwd)

install -Dm755 "$D/steamos-nested-desktop" /usr/local/bin/steamos-nested-desktop
[ -x /usr/local/bin/q1desktop ] || echo "holo-desktop: /usr/local/bin/q1desktop missing (tools/build-native.sh)" >&2

if [ -d /usr/share/plasma/look-and-feel/com.valve.vapor.deck.desktop ]; then
	kwriteconfig6 --file /etc/xdg/kdeglobals --group KDE --key LookAndFeelPackage "com.valve.vapor.deck.desktop"
else
	echo "holo-desktop: the Frame's KDE presets are missing (installer frame step)" >&2
fi

# Installs from Discover run as system services (flatpak's helper, PackageKit), outside the desktop's nice 10
# (steamos-nested-desktop): they yield the CPUs and the disk to the VR stack too (an install took vrcompositor down
# to 50-55 fps, 2026-11-09).
for u in flatpak-system-helper packagekit; do
	install -d /etc/systemd/system/$u.service.d
	printf '[Service]\nNice=10\nIOSchedulingClass=idle\n' > /etc/systemd/system/$u.service.d/quest1-background.conf
done

# Flatpak apps (Chromium from Steam's "Install Chromium", Discover installs): flatpak's bwrap options
# --disable-userns/--assert-userns-disabled need /proc/sys/user/max_user_namespaces (Linux 4.9+), so every app
# failed to start. quest1-bwrap drops them; FLATPAK_BWRAP points flatpak at it for system services (Steam, and the
# desktop it starts), user units and shells.
install -Dm755 "$D/quest1-bwrap" /usr/local/libexec/quest1-bwrap
install -d /etc/systemd/system.conf.d /etc/environment.d /etc/profile.d
printf '[Manager]\nDefaultEnvironment=FLATPAK_BWRAP=/usr/local/libexec/quest1-bwrap\n' \
	> /etc/systemd/system.conf.d/quest1-flatpak-bwrap.conf
echo FLATPAK_BWRAP=/usr/local/libexec/quest1-bwrap > /etc/environment.d/quest1-flatpak-bwrap.conf
echo 'export FLATPAK_BWRAP=/usr/local/libexec/quest1-bwrap' > /etc/profile.d/quest1-flatpak-bwrap.sh

# KDE apps listing processes (System Monitor) crash in libksysguard's GPU plugin when drmGetDevices2 fails, which it
# does when /dev/dri is missing (no DRM here: fbdev + KGSL). An empty /dev/dri makes it return no device.
echo 'd /dev/dri 0755 root root -' > /etc/tmpfiles.d/quest1-dev-dri.conf
systemd-tmpfiles --create /etc/tmpfiles.d/quest1-dev-dri.conf 2>/dev/null || true

# Apps Steam starts from its "+" menu run on Steam's X display (:1), which Remote Play streams: q1apps (built by
# tools/build-native.sh) covers it and shows each app in its own dashboard overlay, gamescope's part on the Frame
[ -x /usr/local/bin/q1apps ] || echo "holo-desktop: /usr/local/bin/q1apps missing (tools/build-native.sh)" >&2
cat > /etc/systemd/system/quest1-apps.service <<'EOF'
[Unit]
Description=Apps started by Steam, each in its own SteamVR dashboard overlay (q1apps)
Requires=quest1-steamvr.service
After=quest1-steamvr.service xvfb.service
PartOf=quest1-steamvr.service

[Service]
User=steamos
Environment=HOME=/home/steamos USER=steamos XDG_RUNTIME_DIR=/run/user/1000 DISPLAY=:1
Environment=DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
# Vulkan on the Adreno, as SteamVR's own clients
Environment=LD_PRELOAD=/opt/hybris/lib/libbionictls.so
Environment=LD_LIBRARY_PATH=/usr/local/lib/quest1-vk-steam:/opt/hybris/lib QUEST1_STEAMVR_COMPAT=1
Nice=5
ExecStartPre=/bin/sh -c 'for i in $(seq 1 30); do pgrep -u steamos -x vrcompositor >/dev/null && exit 0; sleep 2; done; exit 1'
ExecStart=/usr/local/bin/q1apps
Restart=always
RestartSec=5

[Install]
WantedBy=quest1-steamvr.service
EOF

systemctl daemon-reload 2>/dev/null || true
systemctl enable quest1-apps.service 2>/dev/null || true
