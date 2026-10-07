#!/bin/sh
# Boots an Oculus Quest 1 (unlocked bootloader) into native SteamOS, once (Linux and macOS).
# Only "fastboot boot": the image goes to RAM, nothing is written to the headset.
# Rebooting the headset brings back its normal system.
HERE=$(cd "$(dirname "$0")" && pwd)
IMG=$HERE/boot-native-holo.img
cd "$HERE" || exit 1

echo
echo " Quest1-SteamOS - boot native SteamOS on an Oculus Quest 1"
echo " Nothing is flashed: the headset goes back to its normal system on the next reboot."
echo
die() { echo "[!] $*"; exit 1; }
[ -f "$IMG" ] || die "boot-native-holo.img must be in the same folder as this script."

# fastboot/adb: ./platform-tools, then PATH, else Google's platform-tools
if [ -x "$HERE/platform-tools/fastboot" ]; then
	FB=$HERE/platform-tools/fastboot ADB=$HERE/platform-tools/adb
elif command -v fastboot >/dev/null && command -v adb >/dev/null; then
	FB=fastboot ADB=adb
else
	case "$(uname -s)" in
	Darwin) OS=darwin ;;
	Linux) OS=linux ;;
	*) die "unsupported system: $(uname -s)" ;;
	esac
	printf "fastboot and adb were not found. Download Google's Android platform-tools next to this script? [y/N] "
	read -r a; case "$a" in [yY]*) ;; *) exit 1 ;; esac
	curl -fL -o platform-tools.zip "https://dl.google.com/android/repository/platform-tools-latest-$OS.zip" &&
		unzip -qo platform-tools.zip && rm -f platform-tools.zip || die "the download failed."
	FB=$HERE/platform-tools/fastboot ADB=$HERE/platform-tools/adb
fi

nfastboot() { "$FB" devices 2>/dev/null | grep -c fastboot; }
adbstate() { "$ADB" devices 2>/dev/null | awk 'NR > 1 && NF == 2 { print $2 }'; }

while :; do
	n=$(nfastboot)
	[ "$n" -gt 1 ] && die "more than one device is in fastboot mode: leave only the Quest plugged in."
	[ "$n" -eq 1 ] && break
	st=$(adbstate)
	if echo "$st" | grep -q unauthorized; then
		echo "Put the headset on and allow USB debugging for this computer..."
		sleep 3; continue
	fi
	nd=$(echo "$st" | grep -cx device)
	[ "$nd" -gt 1 ] && die "more than one Android device is connected: leave only the Quest plugged in."
	if [ "$nd" -eq 1 ]; then
		[ "$("$ADB" shell getprop ro.product.device | tr -d '\r')" = monterey ] ||
			die "the connected device is not an Oculus Quest 1."
		echo "Restarting the headset into its bootloader..."
		"$ADB" reboot bootloader
		until [ "$(nfastboot)" -ge 1 ]; do sleep 2; done
		break
	fi
	if [ -z "$waiting" ]; then
		echo "No headset found. Either:"
		echo " - plug it in with Android/Horizon OS running and developer mode + USB debugging on, or"
		echo " - turn it off, then hold Volume - and press Power until the boot menu appears."
		echo "Waiting for the headset (Ctrl+C to quit)..."
		[ "$(uname -s)" = Linux ] && echo "(On Linux, fastboot may need udev rules or sudo to see the device.)"
		waiting=1
	fi
	sleep 2
done

"$FB" getvar product 2>&1 | grep -q "product: monterey" || {
	"$FB" getvar product 2>&1; die "the device in fastboot mode is not an Oculus Quest 1."
}
"$FB" getvar unlocked 2>&1 | grep -q "unlocked: yes" ||
	die "the bootloader of this headset is locked: it cannot boot this image."
echo "Oculus Quest 1 with an unlocked bootloader found."
printf "Press Enter to start native SteamOS (Ctrl+C to cancel). "
read -r _
"$FB" boot "$IMG" || die "fastboot boot failed."
cat <<'EOF'

Done. The headset is starting native SteamOS (about one minute).
 - Over USB, the headset is 192.168.77.1: ssh root@192.168.77.1
   (or telnet 192.168.77.1 for the rescue shell).
 - SteamOS itself lives in /data/steamos/holo.img on the headset (see the README):
   without it, the headset stays in the rescue shell.
 - Back to the normal system: reboot the headset (hold Power for 10 seconds).
EOF
