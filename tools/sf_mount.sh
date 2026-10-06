#!/bin/sh
# Idempotent read-only mount of the Steam Frame image partitions in WSL (root).
I=/mnt/d/Documents/Projets/Quest1-SteamOS/roms/steamframe/steamframe.img
if ! mountpoint -q /mnt/sf/p3; then
  L=$(losetup -j "$I" | cut -d: -f1 | head -1)
  [ -n "$L" ] || L=$(losetup -f --show -r -P "$I")
  for n in 1 2 3 4; do mkdir -p /mnt/sf/p$n; mountpoint -q /mnt/sf/p$n || mount -o ro "${L}p$n" /mnt/sf/p$n; done
fi
