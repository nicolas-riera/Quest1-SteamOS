#!/system/bin/sh
# Unmount everything under the Holo chroot and detach its loop device. Run as root (su -mm -c).
R=/data/steamos/root
for m in $(grep " $R" /proc/mounts | cut -d' ' -f2 | sort -r); do umount $m || umount -l $m; done
for l in $(losetup -a 2>/dev/null | grep holo.img | cut -d: -f1); do losetup -d $l; done
grep -c " $R" /proc/mounts
