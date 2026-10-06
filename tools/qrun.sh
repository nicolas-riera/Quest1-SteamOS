#!/bin/bash
# Run a local bash script (file or stdin) inside the Holo chroot on the Quest.
# usage: qrun.sh script.sh   |   qrun.sh - <<'X' ... X
export MSYS_NO_PATHCONV=1
T=$(mktemp); if [ "${1:--}" = - ]; then cat > $T; else cat "$1" > $T; fi
W=$(cygpath -m "$T")
adb push "$W" /data/local/tmp/qrun.sh >/dev/null && rm -f $T
adb shell "su -c 'tr -d \"\r\" < /data/local/tmp/qrun.sh > /data/steamos/root/tmp/qrun.sh'" 2>/dev/null \
 || { adb shell "su -mm -c '/data/steamos/enter.sh true'"; adb shell "su -c 'tr -d \"\r\" < /data/local/tmp/qrun.sh > /data/steamos/root/tmp/qrun.sh'"; }
adb shell "su -mm -c '/data/steamos/enter.sh bash /tmp/qrun.sh'"
