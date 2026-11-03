#!/bin/sh
# The headset presents itself as SteamOS on an Oculus Quest: os-release (Holo is Valve's SteamOS base, its own
# os-release says "Holo core Aarch64 port (preview)"; Steam reads os-release for its system information), the host
# name, and fastfetch with the real device, SoC and GPU names (the device tree model is the board code "vs1 pvt b",
# the SoC shows as "mtp", and fastfetch only finds Mesa llvmpipe: the Adreno driver is Android's blob, through
# libhybris). fastfetch itself is the official aarch64 release (not in Holo's package repos).
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

if ! command -v fastfetch >/dev/null; then
	t=$(mktemp -d)
	curl -sSfL -o "$t/ff.tar.gz" https://github.com/fastfetch-cli/fastfetch/releases/latest/download/fastfetch-linux-aarch64.tar.gz
	tar xzf "$t/ff.tar.gz" -C "$t"
	install -m755 "$t/fastfetch-linux-aarch64/usr/bin/fastfetch" /usr/local/bin/fastfetch
	rm -rf "$t"
fi
mkdir -p /etc/xdg/fastfetch
cat > /etc/xdg/fastfetch/config.jsonc <<'EOF2'
// Quest 1 names (holo-identity.sh): see there why fastfetch cannot find them itself
{
  "logo": { "source": "steamos" },
  "modules": [
    "title", "separator", "os",
    { "type": "host", "format": "Oculus Quest" },
    "kernel", "uptime", "packages", "shell", "terminal",
    { "type": "cpu", "format": "Qualcomm Snapdragon 835 ({core-types}) @ {freq-max}" },
    { "type": "gpu", "format": "Qualcomm Adreno 540 [Integrated]" },
    "memory", "swap", "disk", "localip", "battery", "locale", "break", "colors"
  ]
}
EOF2
