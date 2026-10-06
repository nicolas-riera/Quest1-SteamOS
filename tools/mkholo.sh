#!/bin/bash
# Build an ext4 image of the Holo Core aarch64 rootfs, mountable by the Quest 4.4 kernel.
set -e
SRC=/mnt/d/Documents/Projets/Quest1-SteamOS/roms/holo/system.rootfs.zst
IMG=$HOME/q1/holo.img; MNT=/mnt/holo
rm -f $IMG; truncate -s 6G $IMG
mkfs.ext4 -q -L holo -O ^orphan_file,^metadata_csum_seed,^metadata_csum $IMG
mkdir -p $MNT; mount -o loop $IMG $MNT
zstd -dc $SRC | tar -xp --xattrs --xattrs-include='*' --numeric-owner -C $MNT
du -sh $MNT; ls $MNT; cat $MNT/etc/os-release | head -4
getcap -r $MNT/usr/bin 2>/dev/null | head
umount $MNT
e2fsck -fy $IMG >/dev/null; resize2fs -M $IMG 2>&1 | tail -1
ls -la $IMG; dumpe2fs -h $IMG 2>/dev/null | grep -E "features"
