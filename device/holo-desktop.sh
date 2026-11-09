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
systemctl daemon-reload 2>/dev/null || true
