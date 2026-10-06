S=/mnt/sf/p3/opt/steamvr
ls $S/drivers | tr '\n' ' '; echo
ls $S/bin/linuxarm64 | tr '\n' ' '; echo
cat $S/steamxr_linuxarm64.json
echo "== vrcompositor deps"; readelf -d $S/bin/linuxarm64/vrcompositor 2>/dev/null | grep NEEDED | sed 's/.*\[//;s/\]//' | tr '\n' ' '
echo; echo "== vk ext strings"; strings -n 10 $S/bin/linuxarm64/vrcompositor 2>/dev/null | grep -E "^VK_(KHR|EXT|ANDROID|QCOM)_" | sort -u | tr '\n' ' '
