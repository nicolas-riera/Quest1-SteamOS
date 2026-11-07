#!/bin/sh
# bench.sh <label> [seconds]: dashboard shown, average vrcompositor fps + RenderThread CPU + GPU busy
L=$1; S=${2:-30}
runuser -u steamos -- env XDG_RUNTIME_DIR=/run/user/1000 HOME=/home/steamos LD_LIBRARY_PATH=/opt/steamvr/bin/linuxarm64 timeout 20 /usr/local/bin/vrdash show >/dev/null 2>&1
sleep 5
P=$(pgrep -x vrcompositor); T=$(grep -l RenderThread /proc/$P/task/*/comm | head -1 | xargs dirname)
c0=$(awk '{print $14+$15}' $T/stat)
n0=$(wc -l < /tmp/quest1-compat-vrcompositor.log)
sleep $S
c1=$(awk '{print $14+$15}' $T/stat)
fps=$(tail -n +$((n0+1)) /tmp/quest1-compat-vrcompositor.log | grep -o "display: [0-9.]* fps" | awk '{s+=$2;n++} END {printf "%.1f", s/n}')
cpu=$(awk -v a=$c0 -v b=$c1 -v s=$S "BEGIN{printf \"%.1f\", (b-a)/s}")
echo "$L: $fps fps, RenderThread ${cpu}% CPU ($(awk -v a=$c0 -v b=$c1 -v s=$S -v f=$fps "BEGIN{printf \"%.2f\", (b-a)*10/s/f}") ms/frame), $(sh /root/vrgpu.sh 2>/dev/null | head -1)"
