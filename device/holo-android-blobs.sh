#!/bin/sh
# Make Meta's Android blobs usable from native Holo (libhybris), without running Android.
# Everything is read-only or in RAM: system_a and modem_a are mounted ro, overrides live in tmpfs.
# Installed as /usr/local/sbin/holo-android-blobs, run by holo-android-blobs.service.
set -e
part() { echo /dev/$(grep -l "PARTNAME=$1\$" /sys/class/block/sd*/uevent | head -1 | cut -d/ -f5); }

# /android = system_a (system-as-root); /system, /vendor, /apex like on Android
mkdir -p /android /firmware /apex
mountpoint -q /android || mount -t ext4 -o ro,noload "$(part system_a)" /android
mountpoint -q /system || mount --bind /android/system /system
mountpoint -q /apex || mount -t tmpfs -o mode=755 tmpfs /apex
for a in /android/system/apex/*; do
	n=$(basename "$a"); n=${n%.release}
	mkdir -p "/apex/$n"; mountpoint -q "/apex/$n" || mount --bind "$a" "/apex/$n"
done

# /firmware = modem_a (NON-HLOS vfat: GPU zap shader, ADSP, venus, wlan...)
# Android's options (fstab.monterey): world-readable, the modem's tftp_server (uid vendor_rfs) reads wlanmdsp.mbn
mountpoint -q /firmware || mount -t vfat -o ro,shortname=lower,uid=1000,gid=1000,dmask=222,fmask=333 "$(part modem_a)" /firmware
mountpoint -q /system/vendor/firmware_mnt || mount --bind /firmware /system/vendor/firmware_mnt

# one directory for the kernel firmware loader (it takes a single path)
mkdir -p /run/fw
for f in /system/vendor/firmware/* /firmware/image/*; do ln -sf "$f" /run/fw/; done
echo -n /run/fw > /sys/module/firmware_class/parameters/path
echo 2 > /sys/class/firmware/timeout   # no ueventd to answer the user-helper fallback

# HAL selection normally comes from ro.hardware/ro.board.platform: point the defaults at the Qualcomm HALs
H=/system/vendor/lib64/hw
mkdir -p /run/hwov/up /run/hwov/work
mountpoint -q $H || mount -t overlay overlay -o lowerdir=$H,upperdir=/run/hwov/up,workdir=/run/hwov/work $H
ln -sf vulkan.msm8998.so $H/vulkan.default.so
ln -sf gralloc.msm8998.so $H/gralloc.default.so

# bionic system property area (single "pre-split" file)
python3 /usr/local/lib/holo/mkproparea.py /dev/__properties__ \
	-s ro.hardware=monterey -s ro.boot.hardware=monterey -s ro.board.platform=msm8998 \
	-s hwservicemanager.ready=true -s service.sf.present_timestamp=0 \
	-s ro.boot.slot_suffix=_a \
	/android/system/build.prop /android/system/vendor/build.prop /android/system/etc/prop.default /android/default.prop
