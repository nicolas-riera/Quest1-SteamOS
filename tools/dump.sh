#!/bin/bash
# Dump Quest 1 UFS (all LUNs except userdata) to PC, verify SHA-256 device vs PC.
export MSYS_NO_PATHCONV=1
OUT=/d/Documents/Projets/Quest1-SteamOS/backup/$(date +%Y%m%d)
mkdir -p "$OUT"; cd "$OUT"
# name  device  skip(4K blocks)  count(4K blocks, empty=all)
JOBS="sdb.bin /dev/block/sdb 0 -
sdc.bin /dev/block/sdc 0 -
sdd.bin /dev/block/sdd 0 -
sdf.bin /dev/block/sdf 0 -
sde.bin /dev/block/sde 0 -
sda_0-userdata.bin /dev/block/sda 0 1466888
sda_tail_gpt.bin /dev/block/sda 15161339 5"
echo "$JOBS" | while read name dev skip count; do
  c=""; [ "$count" != "-" ] && c="count=$count"
  cmd="dd if=$dev bs=4096 skip=$skip $c 2>/dev/null"
  t0=$(date +%s)
  adb exec-out "su -c '$cmd'" > "$name" < /dev/null
  hp=$(sha256sum "$name" | cut -d' ' -f1)
  hd=$(adb shell "su -c '$cmd | sha256sum'" < /dev/null | cut -d' ' -f1 | tr -d '\r')
  st=$([ "$hp" = "$hd" ] && echo OK || echo MISMATCH)
  echo "$st $name $(stat -c %s "$name") $(( $(date +%s)-t0 ))s $hp" | tee -a manifest.txt
done
