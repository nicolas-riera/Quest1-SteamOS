#!/bin/bash
# Build a native (no Android) boot image: our kernel + initrd/ initramfs.
# Runs in WSL. usage: mknative.sh <shell|holo> [kernel outdir name] [output.img]
set -e
MODE=${1:-shell}; O=$HOME/q1/${2:-out-steamos}
P=/mnt/d/Documents/Projets/Quest1-SteamOS
OUT=${3:-$P/build/boot-native-$MODE.img}
W=$HOME/q1/native; rm -rf $W; mkdir -p $W; cd $W
MB=$HOME/q1/magisk/magiskboot

aarch64-linux-gnu-gcc -static -O2 -Wall -o fbtest $P/initrd/fbtest.c
aarch64-linux-gnu-gcc -static -O2 -Wall -o rb $P/initrd/rb.c
echo $MODE > holo-mode
tr -d '\r' < $P/initrd/init > init
cat > spec <<EOF
dir /dev 755 0 0
nod /dev/console 600 0 0 c 5 1
nod /dev/kmsg 644 0 0 c 1 11
dir /bin 755 0 0
dir /etc 755 0 0
dir /proc 755 0 0
dir /sys 755 0 0
dir /run 755 0 0
dir /tmp 1777 0 0
dir /data 755 0 0
dir /newroot 755 0 0
file /bin/busybox $HOME/q1/initrd/pkgs/x/usr/bin/busybox 755 0 0
slink /bin/sh busybox 777 0 0
file /bin/fbtest $W/fbtest 755 0 0
file /bin/rb $W/rb 755 0 0
file /etc/holo-mode $W/holo-mode 644 0 0
file /init $W/init 755 0 0
EOF
$O/usr/gen_init_cpio spec > ramdisk.cpio

# header from the stock boot image; kernel+dtb from our build
$MB unpack -h $P/tools/magisk/magisk_patched.img 2>/dev/null
# native-only kernel args (abl appends its own after these)
sed -i "s/^cmdline=.*/& rng_core.default_quality=700/" header
rm -f ramdisk.cpio.orig
$O/usr/gen_init_cpio spec > ramdisk.cpio
python3 - "$O/arch/arm64/boot/Image.gz-dtb" <<'PY'
import sys, zlib
d = zlib.decompressobj(31); k = d.decompress(open(sys.argv[1], 'rb').read())
open('kernel', 'wb').write(k); open('kernel_dtb', 'wb').write(d.unused_data)
PY
# abl always passes skip_initramfs on this legacy-SAR device: neutralise it
$MB hexpatch kernel 736B69705F696E697472616D667300 77616E745F696E697472616D667300
$MB repack $P/tools/magisk/magisk_patched.img new.img 2>/dev/null
mkdir -p "$(dirname "$OUT")"; cp new.img "$OUT"; ls -la "$OUT"
$MB cpio ramdisk.cpio ls 2>/dev/null | head -30 || true
