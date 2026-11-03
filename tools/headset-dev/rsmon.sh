#!/bin/sh
# rsmon.sh SECONDS: while the user does the room setup, log every 2 s the setup step, Monado's and the
# driver's frame rates, GPU load and vrcompositor CPU; 3 snapshots (spaced 15 s) while step 7 is on.
# Output /tmp/rsmon/log.txt and snap*.png (camera images: private, never committed)
D=/tmp/rsmon; rm -rf $D; mkdir -p $D; L=/home/steamos/.local/share/Steam/logs
end=$(( $(date +%s) + ${1:-300} )); snaps=0; lastsnap=0
P=$(pgrep -x vrcompositor); a=$(awk '{print $14+$15}' /proc/$P/stat)
while [ $(date +%s) -lt $end ]; do
	sleep 2
	b=$(awk '{print $14+$15}' /proc/$P/stat 2>/dev/null || echo $a)
	step=$(python3 /root/cdp.py "ConstructStore.roomSetupStep" 2>/dev/null | grep -o '"value": [0-9]*' | grep -o '[0-9]*$')
	mon=$(journalctl -u monado -n 1 --no-pager -o cat | sed -n 's/.*direct: \([0-9.]*\/s warped ([0-9.]*\/s of them fresh)\).*late \([0-9]*\).*/\1 late \2/p')
	drv=$(grep -h "frames/s shown" $L/vrserver.txt | tail -1 | sed 's/.*quest1: \([0-9.]*\) SteamVR.*/\1/')
	echo "$(date +%T) step ${step:-?} | monado $mon | steamvr ${drv}/s | gpu $(cat /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage) | vrcompositor cpu $(( (b-a)*100/200 ))%" >> $D/log.txt
	a=$b
	now=$(date +%s)
	if [ "$step" = 7 ] && [ $snaps -lt 3 ] && [ $((now - lastsnap)) -ge 15 ]; then
		rm -f /tmp/quest1-snap.ppm; runuser -u steamos -- touch /tmp/quest1-snap
		for i in $(seq 1 20); do [ -f /tmp/quest1-snap.ppm ] && break; sleep 0.3; done
		[ -f /tmp/quest1-snap.ppm ] && python3 /root/pt-snapdown.py $D/snap$snaps.png 4 && snaps=$((snaps+1)) && lastsnap=$now
		rm -f /tmp/quest1-snap.ppm
	fi
done
echo done >> $D/log.txt
