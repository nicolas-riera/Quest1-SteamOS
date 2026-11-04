#!/bin/sh
# Install our whole system into a SteamOS image, from inside its chroot (installer/build_image.py, step "native"),
# once Holo, the packages (device/packages.txt) and our prebuilt binaries (tools/build-native.sh) are in place:
# the pieces no other script installs, then every device/holo-*.sh install, in dependency order. systemctl only
# enables units here (it ignores start/restart in a chroot). Idempotent.
# Usage (root, in the image's chroot): sh install-image.sh      (run from a copy of the repository's device/)
# Checked against the development headset by reading the scripts only: the users and groups, the
# holo-android-blobs and fakelogd units below were set up by hand there and are written here from that reading.
set -e
D=$(cd "$(dirname "$0")" && pwd)
T=$D/../tools
U=/etc/systemd/system
say() { echo "install-image: $*"; }

# --- users: Android's network groups (paranoid network, firstboot.sh) and Steam's user ----------------------
sh "$D/firstboot.sh" || true   # its last line probes the network: no network during the build
for g in render video audio input wheel; do getent group $g >/dev/null || groupadd -r $g; done
if ! getent passwd steamos >/dev/null; then
	useradd -m -u 1000 -U -G wheel,video,audio,input,render,aid_inet,aid_net_raw -s /bin/bash steamos
	say "user steamos (1000)"
fi

# --- pieces installed by hand on the development headset ----------------------------------------------------
install -Dm755 "$D/holo-android-blobs.sh" /usr/local/sbin/holo-android-blobs
install -Dm755 "$D/holo-android-modem.sh" /usr/local/sbin/holo-android-modem
install -Dm644 "$T/mkproparea.py" /usr/local/lib/holo/mkproparea.py
install -Dm644 "$D/fakelogd.py" /usr/local/lib/holo/fakelogd.py
# Adreno Vulkan driver overrides (docs/adreno-vk-extensions.md), read from Android's data path
install -Dm644 "$D/qgl_config.txt" /data/vendor/gpu/qgl_config.txt
cat > $U/holo-android-blobs.service <<'EOF'
[Unit]
Description=Meta's Android blobs for native Holo (read-only mounts, firmware path, property area)
DefaultDependencies=no
After=local-fs.target systemd-udevd.service
Before=sysinit.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/holo-android-blobs

[Install]
WantedBy=sysinit.target
EOF
cat > $U/fakelogd.service <<'EOF'
[Unit]
Description=liblog receiver for the Android daemons (logd stand-in, to the journal)
After=holo-android-blobs.service
Before=android-hwservicemanager.service

[Service]
ExecStart=/usr/bin/python3 -u /usr/local/lib/holo/fakelogd.py
Restart=always

[Install]
WantedBy=multi-user.target
EOF
systemctl enable holo-android-blobs.service fakelogd.service

# --- our scripts, in dependency order ------------------------------------------------------------------------
cd "$D"
sh holo-identity.sh install
sh holo-wifi.sh install
sh holo-android-hals.sh
sh holo-android-modem.sh install
sh holo-android-tracking.sh install
sh holo-monado.sh
sh holo-audio.sh install
systemctl enable holo-audio.service
sh holo-steam-session.sh install
sh holo-frame-mode.sh install
sh holo-passthrough.sh install
sh holo-perf.sh
sh holo-battery.sh install
sh holo-bootsplash.sh install

# --- the SteamVR driver: registered for steamos like `vrpathreg adddriver` does ------------------------------
H=/home/steamos/.config/openvr
if [ ! -f $H/openvrpaths.vrpath ]; then
	mkdir -p $H
	cat > $H/openvrpaths.vrpath <<'EOF'
{
	"config" : [ "/home/steamos/.local/share/Steam/config" ],
	"external_drivers" : [ "/opt/quest1-steamvr/quest1" ],
	"jsonid" : "vrpathreg",
	"log" : [ "/home/steamos/.local/share/Steam/logs" ],
	"runtime" : [ "/opt/steamvr" ],
	"version" : 1
}
EOF
	chown -R 1000:1000 /home/steamos/.config
fi
say "done"
