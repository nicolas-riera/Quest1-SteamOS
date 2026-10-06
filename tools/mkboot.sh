#!/bin/bash
# Assemble a bootable image: Magisk-patched stock ramdisk + our Image.gz-dtb.
# usage: mkboot.sh <kernel outdir name> <output.img>
set -e
O=$HOME/q1/${1:-out}; OUT=${2:-/mnt/d/Documents/Projets/Quest1-SteamOS/build/boot-test.img}
W=$HOME/q1/mkboot; rm -rf $W; mkdir -p $W; cd $W
MB=$HOME/q1/magisk/magiskboot
$MB unpack /mnt/d/Documents/Projets/Quest1-SteamOS/tools/magisk/magisk_patched.img 2>/dev/null
# split our Image.gz-dtb into raw Image + dtb, like magiskboot does
python3 - "$O/arch/arm64/boot/Image.gz-dtb" <<'PY'
import sys, zlib
d = zlib.decompressobj(31); k = d.decompress(open(sys.argv[1], 'rb').read())
open('kernel', 'wb').write(k); open('kernel_dtb', 'wb').write(d.unused_data)
PY
# Magisk legacy-SAR patch: boot from ramdisk
$MB hexpatch kernel 736B69705F696E697472616D667300 77616E745F696E697472616D667300
$MB repack /mnt/d/Documents/Projets/Quest1-SteamOS/tools/magisk/magisk_patched.img new.img 2>/dev/null
mkdir -p "$(dirname "$OUT")"; cp new.img "$OUT"; ls -la "$OUT"
