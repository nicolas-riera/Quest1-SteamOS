#!/bin/bash
# Copy a local directory into the Holo chroot (default: src/qbridge -> /root/qbridge).
# usage: qsync.sh [local_dir] [chroot_dir]
export MSYS_NO_PATHCONV=1
SRC=${1:-/d/Documents/Projets/Quest1-SteamOS/src/qbridge}; DST=${2:-/root/$(basename "$SRC")}
T=/data/local/tmp/qsync_$(basename "$SRC")
adb shell "rm -rf $T" && adb push "$(cygpath -m "$SRC")" "$T" >/dev/null || exit 1
adb shell "su -c 'mkdir -p /data/steamos/root$DST && cp -r $T/. /data/steamos/root$DST/ && rm -rf $T'"
echo "synced $SRC -> chroot:$DST"
