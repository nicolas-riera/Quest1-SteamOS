#!/bin/sh
# The headset presents itself as SteamOS on an Oculus Quest: os-release (Holo is Valve's SteamOS base, its own
# os-release says "Holo core Aarch64 port (preview)"; Steam reads os-release for its system information) and the
# host name.
# Usage (on the headset): holo-identity.sh install. The original os-release is kept in /root/os-release.holo.
set -e
[ "$1" = install ] || { echo "usage: $0 install" >&2; exit 1; }
cp -n /etc/os-release /root/os-release.holo
. /root/os-release.holo
cat > /etc/os-release <<EOF2
NAME="SteamOS"
PRETTY_NAME="SteamOS"
ID=steamos
ID_LIKE=arch
VARIANT_ID=$VARIANT_ID
ANSI_COLOR="1;35"
HOME_URL="https://store.steampowered.com/steamos"
LOGO=steamos
VERSION_ID=$VERSION_ID
BUILD_ID=$BUILD_ID
EOF2
hostnamectl set-hostname oculus-quest
hostnamectl set-hostname --pretty "Oculus Quest"
sed -i 's/\bholo-quest\b/oculus-quest/g' /etc/hosts

