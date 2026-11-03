# pt-seq.sh: roomview test sequence (props driver already set). Restarts SteamVR, starts the pattern provider,
# holds the camera stream with roomView=2, enables roomview for scene apps through the system layer mailbox.
for p in $(pgrep -x ptprovider) $(pgrep -x trackedcam_test); do kill $p; done
sh /root/q1restart.sh main "roomview test" systemctl restart quest1-steamvr
until pgrep -x vrcompositor >/dev/null; do sleep 1; done
sleep 20
cd /tmp
(setsid nohup runuser -u steamos -- env XDG_RUNTIME_DIR=/run/user/1000 PT_PATTERN=${PATTERN:-2} PT_DUMP=1 /tmp/ptprovider > /tmp/ptprov.out 2>&1 &)
sleep 16
(setsid nohup runuser -u steamos -- env XDG_RUNTIME_DIR=/run/user/1000 PT_ROOMVIEW=${ROOMVIEW:-2} PT_HOLD=900 /tmp/trackedcam_test /tmp 1 > /tmp/tct.out 2>&1 &)
sleep 5
timeout 20 runuser -u steamos -- env XDG_RUNTIME_DIR=/run/user/1000 /tmp/vrmailbox vrcompositor_systemlayer '{"type":"set_camera_roomview_enabled_forsceneapps","enabled":true}' 1 2>&1 | grep -v "Info\]"
sleep 3
grep -i "roomview\|room view\|CTrackedCamera" /home/steamos/.local/share/Steam/logs/vrcompositor.txt | tail -5
