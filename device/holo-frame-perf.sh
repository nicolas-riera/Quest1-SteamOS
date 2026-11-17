#!/bin/sh
# The Steam Frame's performance features in Steam's quick menu (Performance tab), docs/frame-features.md.
# Run as root: sh holo-frame-perf.sh install   (uninstall removes it)
#  - q1-steamos-manager.service: the com.steampowered.SteamOSManager1 D-Bus API Steam looks for ("SteamOSManager:
#    daemon not present" in its log otherwise): Manual GPU Clock (KGSL devfreq) and the performance profiles
#    (CPU/GPU ceilings). device/q1-steamos-manager.py; runs as steamos on Steam's session bus.
#  - q1-perfhud.service: the Performance Overlay Level (Steam writes <Steam>/config/mangohud.conf for mangoapp, which
#    needs gamescope: absent here) drawn as a head-locked SteamVR overlay. src/q1perfhud, built by tools/build-native.sh
#    (/usr/local/bin/q1-perfhud). Steam writes that file only when told mangoapp is there: steam.service's drop-in.
set -e
case "$1" in
install) ;;
uninstall)
	systemctl disable --now q1-steamos-manager.service q1-perfhud.service 2>/dev/null || true
	rm -f /etc/systemd/system/q1-steamos-manager.service /etc/systemd/system/q1-perfhud.service \
		/etc/systemd/system/steam.service.d/q1-perfhud.conf /etc/systemd/system/quest1-steamvr.service.wants/q1-perfhud.service \
		/usr/local/sbin/q1-steamos-manager /usr/local/sbin/q1-steamos-manager-perms /usr/local/lib/libquest1_gamescopeid.so \
		/usr/share/dbus-1/system.d/com.steampowered.SteamOSManager1.conf
	systemctl daemon-reload
	exit 0 ;;
*) echo "usage: $0 install|uninstall" >&2; exit 2 ;;
esac

install -m 755 "$(dirname "$0")/q1-steamos-manager.py" /usr/local/sbin/q1-steamos-manager

# The files the daemon writes go to steamos (sysfs keeps a chown until the device goes away): the daemon then needs no
# root and no polkit. Android's power HAL, masked from them (holo-android-tracking.sh), is the only other writer.
cat > /usr/local/sbin/q1-steamos-manager-perms <<'EOF'
#!/bin/sh
# q1-steamos-manager-perms: hand the clock knobs q1-steamos-manager writes to steamos (ExecStartPre=+ of its unit).
for f in /sys/class/kgsl/kgsl-3d0/devfreq/min_freq /sys/class/kgsl/kgsl-3d0/devfreq/max_freq \
	/sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_max_freq; do
	[ -e "$f" ] && chown steamos "$f"
done
exit 0
EOF
chmod 755 /usr/local/sbin/q1-steamos-manager-perms

cat > /etc/systemd/system/q1-steamos-manager.service <<'EOF'
[Unit]
Description=SteamOS Manager API for Steam's quick menu (GPU clock, performance profile), q1-steamos-manager
After=user@1000.service
Wants=user@1000.service
# Steam looks for the name at start (and watches for it later): before it when both start together
Before=steam.service

[Service]
Type=notify
NotifyAccess=main
User=steamos
# Steam's own bus (steam.service sets the same): the daemon claims com.steampowered.SteamOSManager1 on it
Environment=DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus XDG_RUNTIME_DIR=/run/user/1000
ExecStartPre=+/usr/local/sbin/q1-steamos-manager-perms
ExecStart=/usr/bin/python3 -u /usr/local/sbin/q1-steamos-manager
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
EOF

# QUEST1_SM_BUS=system (in a drop-in) claims the name on the system bus instead: this lets steamos own it there
cat > /usr/share/dbus-1/system.d/com.steampowered.SteamOSManager1.conf <<'EOF'
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <policy user="steamos">
    <allow own="com.steampowered.SteamOSManager1"/>
  </policy>
  <policy context="default">
    <allow send_destination="com.steampowered.SteamOSManager1"/>
  </policy>
</busconfig>
EOF

# Performance overlay: Steam writes mangohud.conf only when its environment says mangoapp is there. With presets
# supported the file holds "preset=<level>" (0 to 4). STEAM_DISABLE_MANGOAPP_ATOM_WORKAROUND: no xprop calls on
# gamescope's overlay window (Steam's DISPLAY is our Xvfb).
mkdir -p /etc/systemd/system/steam.service.d
cat > /etc/systemd/system/steam.service.d/q1-perfhud.conf <<'EOF'
# Steam connects to SteamOSManager (session bus, object manager at /) once at its start: the daemon goes first
[Unit]
Wants=q1-steamos-manager.service
After=q1-steamos-manager.service

[Service]
Environment=STEAM_USE_MANGOAPP=1 STEAM_MANGOAPP_PRESETS_SUPPORTED=1 STEAM_DISABLE_MANGOAPP_ATOM_WORKAROUND=1
# The Performance tab of the quick menu, in VR, needs the web UI's IN_GAMESCOPE (steamui.so's one Plat_IsGamescope call
# for it); the shim answers yes to that call site only (src/gamescopeid). steam-q1 keeps an LD_PRELOAD from the environment.
Environment=LD_PRELOAD=/usr/local/lib/libquest1_gamescopeid.so
EOF

cat > /etc/systemd/system/q1-perfhud.service <<'EOF'
[Unit]
Description=Performance overlay (Steam's Performance Overlay Level) as a head-locked SteamVR overlay
After=quest1-steamvr.service
BindsTo=quest1-steamvr.service

[Service]
User=steamos
Environment=XDG_RUNTIME_DIR=/run/user/1000 HOME=/home/steamos
ExecStart=/usr/local/bin/q1-perfhud
Restart=always
RestartSec=5
Nice=5
MemoryLow=16M
EOF

systemctl daemon-reload
systemctl enable q1-steamos-manager.service
systemctl add-wants quest1-steamvr.service q1-perfhud.service
