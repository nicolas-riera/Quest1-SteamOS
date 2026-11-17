#!/bin/sh
# stream-load.sh [SECONDS] (as root on the headset, during a Steam Link VR session, panels on): the user's heavy case
# for black flashes (2026-11-16: desktop mode and Dolphin open). Starts desktop mode for SECONDS (default 150) and,
# from 60 s, takes 500 MB of memory for 60 s; every 5 s prints the memory available, the load and vrcompositor's fps,
# then the shim's flash log of that time (docs/streaming.md item 16: gaps, empty submits, black pictures).
n=${1:-150}
since=$(date '+%F %T')
runuser -u steamos -- env XDG_RUNTIME_DIR=/run/user/1000/nested_plasma timeout "$n" \
	/usr/local/bin/steamos-nested-desktop >/dev/null 2>&1 &
t=0
while [ $t -le $((n + 15)) ]; do
	[ $t -eq 60 ] && (python3 -c '
import time
b = bytearray(500 << 20)
for i in range(0, len(b), 4096):
    b[i] = 1
time.sleep(60)' &)
	m=$(awk '/MemAvailable/{print int($2 / 1024)}' /proc/meminfo)
	f=$(grep -a "fps;" /tmp/quest1-compat-vrcompositor.log | tail -1 | cut -d" " -f2)
	echo "t=$t s: MemAvailable ${m} MB, load $(cut -d' ' -f1 /proc/loadavg), vrcompositor ${f} fps"
	sleep 5
	t=$((t + 5))
done
echo "flash log since $since:"
journalctl --since "$since" --no-pager -o cat | grep "quest1_v4l2: vrlink: [0-9]" || echo "(nothing)"
