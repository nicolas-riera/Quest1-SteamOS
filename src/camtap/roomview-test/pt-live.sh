# pt-live.sh: boundary-drawing roomview with the pattern provider; snapshot; undo unless KEEP=1
echo props > /etc/quest1-camera
sh /root/pt-seq.sh >/dev/null 2>&1
since=$(date "+%H:%M:%S")
timeout 20 runuser -u steamos -- env XDG_RUNTIME_DIR=/run/user/1000 /tmp/vrmailbox vrcompositor_systemlayer "{\"type\":\"set_camera_roomview_enabled\",\"enabled\":true}" 1 >/dev/null 2>&1
sleep 8
journalctl --since "$since" --no-pager -o cat | grep "quest1_compat\] \(failed\|graphics pipeline retried\)"
grep -h "CTrackedCamera\|room view" /home/steamos/.local/share/Steam/logs/vrcompositor.txt | tail -3
ps -o pid,etimes -C vrcompositor
rm -f /tmp/quest1-snap.ppm; runuser -u steamos -- touch /tmp/quest1-snap
for i in $(seq 1 30); do [ -f /tmp/quest1-snap.ppm ] && break; sleep 0.5; done; sleep 1
python3 /root/pt-snapdown.py /tmp/pt-snap.png 5; rm -f /tmp/quest1-snap.ppm
[ "$KEEP" = 1 ] && exit 0
for p in $(pgrep -x ptprovider) $(pgrep -x trackedcam_test); do kill $p; done
sh /root/q1restart.sh main "undo roomview test settings" sh /root/pt-undo.sh
