#!/bin/bash
# Build the native boot image (kernel + initramfs) from public sources only:
#   - Meta's GPL Quest kernel (facebookincubator/oculus-linux-kernel) + kernel/quest1-kernel.patch
#     + kernel/loop-no-discard-on-ext4.patch + kernel/cgroup-events-frozen.patch
#     + kernel/bt-wcn3990.patch + kernel/kgsl-lazy.patch + kernel/lse_emul.c + kernel/lse-fast-undef.patch
#     + kernel/devpts-ptmx-owner.patch + kernel/cpuinfo-blank-line.patch, config = recon/kernel.config + kernel/steamos.config;
#   - Android's gcc 4.9 prebuilt toolchains (the ones Meta's tree builds with);
#   - Debian's static busybox, initrd/ from this repo.
# Nothing comes from a device dump: no Meta blob ends up in the image. Same result as
# tools/mknative.sh holo, without the stock boot image (the header values are written here) and
# with the kernel's built-in trusted keys left empty (that build used Meta's verity cert).
# Runs on any x86_64 Debian/Ubuntu with
#   bc bison flex libssl-dev device-tree-compiler cpio gcc-aarch64-linux-gnu curl git python3 unzip
# usage: build-boot.sh kernel <workdir> <kernel-out dir>      (Image.gz-dtb + gen_init_cpio)
#        build-boot.sh image <workdir> <kernel-out dir> <output.img>
#        build-boot.sh installer <workdir> <kernel-out dir> <output.img>   (the installer's micro Linux:
#            Alpine aarch64 + installer/agent.py, see installer/README.md; needs no arm64 machine)
set -eo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
STEP=$1; W=$(mkdir -p "$2" && cd "$2" && pwd); KO=$(mkdir -p "$3" && cd "$3" && pwd)

KREPO=https://github.com/facebookincubator/oculus-linux-kernel.git
KCOMMIT=6929f734ce0e602018790ff3a52dc7bad646af60 # oculus-quest-kernel-master
TC=https://android.googlesource.com/platform/prebuilts/gcc/linux-x86
TCREF=refs/heads/android10-release
MAGISK=https://github.com/topjohnwu/Magisk/releases/download/v28.1/Magisk-v28.1.apk
ALPINE=https://dl-cdn.alpinelinux.org/alpine/v3.24
ALPINE_ROOTFS=alpine-minirootfs-3.24.3-aarch64.tar.gz
ALPINE_SHA256=4d3f80afaebd447c5591a63a73bde380a149fe8b2d85488cafabb2ff97551f50
# the micro Linux's tools: build_image.py needs python3, GNU tar (xattrs), zstd (also its libzstd, for
# installer/btrfsread.py: the Frame image's btrfs), e2fsprogs; blkid for the scratch space; udhcpd/telnetd from
# busybox-extras
ALPINE_PKGS="python3 tar zstd e2fsprogs blkid busybox-extras"
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
	# holo.img sits on ext4, where Meta disabled punch hole: no failing discards on loop0
	git -C kernel apply "$P/kernel/loop-no-discard-on-ext4.patch"
	# systemd needs a "frozen" key in cgroup.events to notice that a cgroup became empty
	git -C kernel apply "$P/kernel/cgroup-events-frozen.patch"
	# Bluetooth: the WCN3990 on hci_uart's QCA protocol (firmware download, 3 Mbit/s, the address in the NVM)
	git -C kernel apply "$P/kernel/bt-wcn3990.patch"
	# KGSL: allocations backed on first CPU touch, for vrclient's 256 MB upload rings (IOCTL_KGSL_LAZY_NEXT)
	git -C kernel apply "$P/kernel/kgsl-lazy.patch"
	cp "$P/kernel/lse_emul.c" kernel/arch/arm64/kernel/lse_emul.c
	# the LSE emulation's traps skip undef_lock and the hook list (lse_emul_fast)
	git -C kernel apply "$P/kernel/lse-fast-undef.patch"
	# devpts mounts in user namespaces that map no root (bubblewrap inside a flatpak sandbox: Chromium)
	git -C kernel apply "$P/kernel/devpts-ptmx-owner.patch"
	# /proc/cpuinfo ends with an empty line (KDE's ksystemstats spun forever without it)
	git -C kernel apply "$P/kernel/cpuinfo-blank-line.patch"
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
	busybox_static
	aarch64-linux-gnu-gcc -static -O2 -Wall -o fbtest "$P/initrd/fbtest.c"
	aarch64-linux-gnu-gcc -static -O2 -Wall -o rb "$P/initrd/rb.c"
	aarch64-linux-gnu-gcc -static -O2 -Wall -o loopdio "$P/initrd/loopdio.c"
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
	file /bin/loopdio $I/loopdio 755 0 0
	file /etc/holo-mode $I/holo-mode 644 0 0
	file /init $I/init 755 0 0
	EOF
	"$KO/gen_init_cpio" spec | gzip -9n > ramdisk.gz
	mkboot ramdisk.gz "$OUT"
	echo "built $OUT ($(stat -c %s "$OUT") bytes), busybox: $(basename "$DEB")"
}

installer() {
	OUT=$1; I=$W/installer; rm -rf "$I"; mkdir -p "$I/root"; cd "$I"
	curl -fsSL "$ALPINE/releases/aarch64/$ALPINE_ROOTFS" -o rootfs.tgz
	echo "$ALPINE_SHA256  rootfs.tgz" | sha256sum -c --quiet
	tar -xzf rootfs.tgz -C root
	# apk.static (x86_64) installs the aarch64 packages without running them; signatures checked with the
	# rootfs's own keys. Package scripts are skipped: Alpine's are busybox links and cache triggers
	APKS=$(curl -fsSL "$ALPINE/main/x86_64/APKINDEX.tar.gz" | tar -xzO APKINDEX | awk -F: '/^P:apk-tools-static$/{p=1} p&&/^V:/&&!v{v=$2} END{print v}')
	curl -fsSL "$ALPINE/main/x86_64/apk-tools-static-$APKS.apk" | tar -xzf - sbin/apk.static 2>/dev/null
	echo "$ALPINE/main" > root/etc/apk/repositories; echo "$ALPINE/community" >> root/etc/apk/repositories
	./sbin/apk.static --root root --arch aarch64 --keys-dir etc/apk/keys --no-scripts --update-cache \
		add $ALPINE_PKGS >/dev/null
	# smaller: no compiled caches, tests, docs, Tk, IDLE, pip
	rm -rf root/usr/share/man root/usr/share/doc root/var/cache/apk/*
	find root/usr/lib/python3* \( -name __pycache__ -o -name test -o -name tests -o -name idlelib -o -name tkinter \
		-o -name ensurepip -o -name turtledemo \) -prune -exec rm -rf {} +
	aarch64-linux-gnu-gcc -static -O2 -Wall -o root/bin/rb "$P/initrd/rb.c"
	aarch64-linux-gnu-gcc -static -O2 -Wall -s -o root/bin/fbblank "$P/initrd/fbblank.c"
	mkdir -p root/installer root/scratch
	cp "$P/installer/agent.py" "$P/installer/build_image.py" "$P/installer/btrfsread.py" "$P/installer/prepare.py" \
		"$P/installer/sources.json" root/installer/
	# the init and its debug path (USB gadget, telnetd, serial shell) run on a static busybox, as in the native
	# initramfs: nothing there depends on Alpine's musl; the console nodes the kernel opens before /dev is mounted
	busybox_static; install -m755 bb/usr/bin/busybox root/bin/busybox-static
	rm -f root/dev/console root/dev/kmsg
	mknod -m 600 root/dev/console c 5 1; mknod -m 644 root/dev/kmsg c 1 11
	tr -d '\r' < "$P/installer/micro-init" > root/init; chmod 755 root/init
	(cd root && find . | cpio -o -H newc -R 0:0 --quiet) | gzip -9n > ramdisk.gz
	mkboot ramdisk.gz "$OUT"
	echo "built $OUT ($(stat -c %s "$OUT") bytes), Alpine $ALPINE_ROOTFS + $ALPINE_PKGS"
}

busybox_static() { # Debian stable's static busybox (arm64) into ./bb/usr/bin/busybox, its package in $DEB
	curl -fsSL "$DEBIAN/dists/stable/main/binary-arm64/Packages.xz" | xz -d > Packages
	DEB=$(awk '/^Package: busybox-static$/{p=1} p&&/^Filename:/{print $2; exit}' Packages)
	curl -fsSL "$DEBIAN/$DEB" -o busybox.deb
	mkdir bb && dpkg-deb -x busybox.deb bb
}

mkboot() { # ramdisk.gz output.img: the kernel and a ramdisk into a signed boot image
	local RD=$1 OUT=$2
	# boot image v0 with the stock header values (base 0, 4 KiB pages, Android 10 / 2024-07)
	python3 - "$KO/Image.gz-dtb" "$RD" new.img "$CMDLINE" <<-'PY'
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
}

case "$STEP" in
kernel) kernel ;;
image) image "$(realpath -m "$4")" ;;
installer) installer "$(realpath -m "$4")" ;;
*) echo "usage: $0 kernel|image|installer <workdir> <kernel-out dir> [output.img]" >&2; exit 2 ;;
esac
