#!/system/bin/sh
# Mount the Holo Core image and chroot into it. Run as root (su -mm -c).
# usage: enter.sh [command...]   (default: login shell)
R=/data/steamos/root
IMG=/data/steamos/holo.img
mkdir -p $R
if ! grep -q " $R " /proc/mounts; then
  LOOP=$(losetup -sf $IMG) && mount -t ext4 -o noatime $LOOP $R || exit 1
  mkdir -p $R/proc $R/sys $R/dev $R/run $R/tmp
  mount -t proc proc $R/proc
  mount -t sysfs sysfs $R/sys
  # Isolated /dev: devtmpfs (unused by Android) + Android's devpts bound in
  # (kernel has no multi-instance devpts). Never bind Android's /dev itself.
  mount -t devtmpfs devtmpfs $R/dev
  mkdir -p $R/dev/pts $R/dev/shm
  mount -o bind /dev/pts $R/dev/pts
  rm -f $R/dev/fd $R/dev/stdin $R/dev/stdout $R/dev/stderr
  ln -s /proc/self/fd $R/dev/fd
  ln -s fd/0 $R/dev/stdin; ln -s fd/1 $R/dev/stdout; ln -s fd/2 $R/dev/stderr
  mount -t tmpfs -o mode=1777 tmpfs $R/dev/shm
  mount -t tmpfs -o mode=755 tmpfs $R/run
  mount -t tmpfs -o mode=1777 tmpfs $R/tmp
  # Android userspace for libhybris: the vendor blobs talk to the HAL services
  # of the Android instance running alongside (shared binder/hwbinder).
  mkdir -p $R/system $R/apex $R/dev/__properties__ $R/dev/socket
  mount -o bind,ro /system $R/system
  # toybox rbind isn't recursive: bind each flattened APEX (bionic lives there)
  mount -t tmpfs -o mode=755 tmpfs $R/apex
  for a in /apex/*; do mkdir -p $R$a; mount -o bind,ro $a $R$a; done
  [ -e $R/vendor ] || ln -s /system/vendor $R/vendor
  mount -o bind /dev/__properties__ $R/dev/__properties__
  mount -o bind /dev/socket $R/dev/socket
  rm -f $R/etc/resolv.conf; printf 'nameserver 1.1.1.1\nnameserver 8.8.8.8\n' > $R/etc/resolv.conf
fi
# Big C++ builds exceed RAM (Android shares it): keep a swap file so they
# page instead of triggering an OOM kernel panic.
if [ -f /data/steamos/swap ] && ! grep -q steamos/swap /proc/swaps; then
  swapon -p 5 /data/steamos/swap
fi
# Android has no chroot binary: run Holo's own through its dynamic loader,
# with a clean environment (no Android TMPDIR/LD_*/BOOTCLASSPATH leaking in).
CHROOT="$R/usr/lib/ld-linux-aarch64.so.1 --library-path $R/usr/lib $R/usr/bin/chroot"
ENV="/usr/bin/env -i PATH=/usr/local/sbin:/usr/local/bin:/usr/bin:/usr/sbin HOME=/root TERM=${TERM:-xterm-256color} LANG=C.UTF-8"
if [ $# -eq 0 ]; then exec $CHROOT $R $ENV /bin/bash -l; else exec $CHROOT $R $ENV /bin/bash -lc "$*"; fi
