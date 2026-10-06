#!/bin/bash
# Generate ~/q1/<out>/.config = stock config + fragment, report options that didn't stick.
# usage: kconfig.sh <outdir-name> <fragment (WSL path)>
O=$HOME/q1/$1; F=$2
mkdir -p $O
cp /mnt/d/Documents/Projets/Quest1-SteamOS/recon/kernel.config $O/.config
tr -d '\r' < $F >> $O/.config
cd ~/q1/kernel
ARCH=arm64 CROSS_COMPILE=$HOME/q1/tc/gcc49/bin/aarch64-linux-android- make O=$O DTC=/usr/bin/dtc olddefconfig > $O.cfg.log 2>&1 || { tail $O.cfg.log; exit 1; }
tr -d '\r' < $F | grep -E '^(CONFIG_|# CONFIG_)' | while read -r l; do
  grep -qxF "$l" $O/.config || echo "NOT APPLIED: $l -> $(grep -E "(^| )${l#\# }" $O/.config | head -1)"
done
echo "config ok: $O/.config"
