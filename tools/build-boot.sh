#!/bin/bash
# Build the native boot image (kernel + initramfs) from public sources only:
#   - Meta's GPL Quest kernel (facebookincubator/oculus-linux-kernel) + kernel/quest1-kernel.patch
#     + kernel/lse_emul.c, config = recon/kernel.config + kernel/steamos.config;
#   - Android's gcc 4.9 prebuilt toolchains (the ones Meta's tree builds with);
#   - Debian's static busybox, initrd/ from this repo.
# Nothing comes from a device dump: no Meta blob ends up in the image. Same result as
# tools/mknative.sh holo, without the stock boot image (the header values are written here) and
# with the kernel's built-in trusted keys left empty (that build used Meta's verity cert).
# Used by .github/workflows/prerelease.yml; runs on any x86_64 Debian/Ubuntu with
#   bc bison flex libssl-dev device-tree-compiler cpio gcc-aarch64-linux-gnu curl git python3 unzip
# usage: build-boot.sh kernel <workdir> <kernel-out dir>      (Image.gz-dtb + gen_init_cpio)
#        build-boot.sh image <workdir> <kernel-out dir> <output.img>
set -eo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
STEP=$1; W=$(mkdir -p "$2" && cd "$2" && pwd); KO=$(mkdir -p "$3" && cd "$3" && pwd)

KREPO=https://github.com/facebookincubator/oculus-linux-kernel.git
KCOMMIT=6929f734ce0e602018790ff3a52dc7bad646af60 # oculus-quest-kernel-master
TC=https://android.googlesource.com/platform/prebuilts/gcc/linux-x86
TCREF=refs/heads/android10-release
MAGISK=https://github.com/topjohnwu/Magisk/releases/download/v28.1/Magisk-v28.1.apk
DEBIAN=https://deb.debian.org/debian
CMDLINE="androidboot.configfs=true androidboot.hardware=monterey ehci-hcd.park=3 lpm_levels.sleep_disabled=1 msm_rtb.filter=0x237 sched_enable_hmp=1 sched_enable_power_aware=1 service_locator.enable=1 softdog.soft_panic=1 swiotlb=2048 user_debug=31 bootver=1596585601 cursysver=1596585601 minsysver=1 buildvariant=user veritykeyid=id:cc158dc3bf03e76f1e4f8e38b2841cb9f2d421e4 rng_core.default_quality=700"

kernel() {
	cd "$W"
	if [ ! -d kernel/.git ]; then
		git init -q kernel
		git -C kernel fetch -q --depth 1 "$KREPO" $KCOMMIT
		git -C kernel checkout -q FETCH_HEAD
	fi
	git -C kernel reset -q --hard $KCOMMIT && git -C kernel clean -qfdx
	git -C kernel apply "$P/kernel/quest1-kernel.patch"
	cp "$P/kernel/lse_emul.c" kernel/arch/arm64/kernel/lse_emul.c
	# Kconfig sources Meta's unpublished internal/ drivers: empty stand-ins (ignored by git)
	mkdir -p kernel/drivers/staging/oculus/internal
	touch kernel/drivers/staging/oculus/internal/Kconfig kernel/drivers/staging/oculus/internal/Makefile

	for t in aarch64/aarch64-linux-android-4.9 arm/arm-linux-androideabi-4.9; do
		d=tc/$(basename $t); g=$d/bin/$(basename ${t%-4.9})-gcc
		[ -x $g-4.9.x ] || { mkdir -p $d && curl -fsSL "$TC/$t/+archive/$TCREF.tar.gz" | tar -xzm -C $d; }
		# <triple>-gcc is a python2 deprecation wrapper there: call the compiler directly
		ln -sfn $(basename $g)-4.9.x $g
	done
	export ARCH=arm64 CROSS_COMPILE=$W/tc/aarch64-linux-android-4.9/bin/aarch64-linux-android-
	export CROSS_COMPILE_ARM32=$W/tc/arm-linux-androideabi-4.9/bin/arm-linux-androideabi-

	rm -rf out && mkdir out
	{ tr -d '\r' < "$P/recon/kernel.config"; tr -d '\r' < "$P/kernel/steamos.config"
	  echo 'CONFIG_SYSTEM_TRUSTED_KEYS=""'; } > out/.config
	make -C kernel O=$W/out DTC=/usr/bin/dtc olddefconfig >/dev/null
	# about 15 min on a 4-core CI runner: a progress line every 30 s, the log tail on failure
	make -C kernel O=$W/out DTC=/usr/bin/dtc -j"$(nproc)" Image.gz-dtb > kernel.log 2>&1 & pid=$!
	while kill -0 $pid 2>/dev/null; do
		sleep 30; echo "kernel: $(grep -c '^  CC' kernel.log) files compiled"
	done
	wait $pid || { grep -E 'error|Error' kernel.log | head -20; tail -20 kernel.log; exit 1; }
	cp out/arch/arm64/boot/Image.gz-dtb out/usr/gen_init_cpio out/.config "$KO/"
}

image() {
	OUT=$1; I=$W/initramfs; rm -rf "$I"; mkdir -p "$I"; cd "$I"
	# static busybox from Debian stable (arm64)
	curl -fsSL "$DEBIAN/dists/stable/main/binary-arm64/Packages.xz" | xz -d > Packages
	DEB=$(awk '/^Package: busybox-static$/{p=1} p&&/^Filename:/{print $2; exit}' Packages)
	curl -fsSL "$DEBIAN/$DEB" -o busybox.deb
	mkdir bb && dpkg-deb -x busybox.deb bb
	aarch64-linux-gnu-gcc -static -O2 -Wall -o fbtest "$P/initrd/fbtest.c"
	aarch64-linux-gnu-gcc -static -O2 -Wall -o rb "$P/initrd/rb.c"
	echo holo > holo-mode
	tr -d '\r' < "$P/initrd/init" > init
	cat > spec <<-EOF
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
	file /bin/busybox $I/bb/usr/bin/busybox 755 0 0
	slink /bin/sh busybox 777 0 0
	file /bin/fbtest $I/fbtest 755 0 0
	file /bin/rb $I/rb 755 0 0
	file /etc/holo-mode $I/holo-mode 644 0 0
	file /init $I/init 755 0 0
	EOF
	"$KO/gen_init_cpio" spec | gzip -9n > ramdisk.gz

	# boot image v0 with the stock header values (base 0, 4 KiB pages, Android 10 / 2024-07)
	python3 - "$KO/Image.gz-dtb" ramdisk.gz new.img "$CMDLINE" <<-'PY'
	import gzip, hashlib, struct, sys, zlib
	src, rd, out, cmdline = sys.argv[1:5]
	d = zlib.decompressobj(31); kernel = d.decompress(open(src, 'rb').read()); dtb = d.unused_data
	# abl always passes skip_initramfs on this legacy-SAR device: neutralise it
	kernel = kernel.replace(b'skip_initramfs\0', b'want_initramfs\0')
	kernel = gzip.compress(kernel, 9, mtime=0) + dtb
	ramdisk = open(rd, 'rb').read()
	sha = hashlib.sha1()
	for blob in (kernel, ramdisk, b''):
	    sha.update(blob); sha.update(struct.pack('<I', len(blob)))
	osv = ((10 << 14) | (0 << 7) | 0) << 11 | ((2024 - 2000) << 4 | 7)
	hdr = struct.pack('<8s10I16s512s32s1024s', b'ANDROID!', len(kernel), 0x8000, len(ramdisk),
	                  0x1000000, 0, 0xf00000, 0x100, 4096, 0, osv, b'',
	                  cmdline.encode()[:511], sha.digest(), b'')
	pad = lambda b: b + b'\0' * (-len(b) % 4096)
	open(out, 'wb').write(pad(hdr) + pad(kernel) + pad(ramdisk))
	PY
	# AVB 1.0 signature with the AOSP test key, like magiskboot repack did for the tested images
	# (the unlocked bootloader does not require it)
	[ -x "$W/magiskboot" ] || {
		curl -fsSL "$MAGISK" -o "$W/magisk.apk"
		unzip -p "$W/magisk.apk" lib/x86_64/libmagiskboot.so > "$W/magiskboot"; chmod 755 "$W/magiskboot"
	}
	"$W/magiskboot" sign new.img >/dev/null 2>&1
	"$W/magiskboot" verify new.img >/dev/null 2>&1
	mkdir -p "$(dirname "$OUT")"; cp new.img "$OUT"
	echo "built $OUT ($(stat -c %s "$OUT") bytes), busybox: $(basename "$DEB")"
}

case "$STEP" in
kernel) kernel ;;
image) image "$(realpath -m "$4")" ;;
*) echo "usage: $0 kernel|image <workdir> <kernel-out dir> [output.img]" >&2; exit 2 ;;
esac
