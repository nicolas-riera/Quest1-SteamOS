#!/bin/sh
# Unpacks the runtime libraries of the headset's own Meta system app (VrDriver.apk, in the system partition the
# device already has) to /run/q1track/vrapi, a tmpfs, so q1handsd can ask libvrapiimpl.so for Meta's hand skeleton
# (docs/hand-tracking.md). Nothing is copied to the repository or to persistent storage.
APK=${1:-/system/priv-app/VrDriver/VrDriver.apk}
OUT=${2:-/run/q1track/vrapi}
[ -f "$APK" ] || { echo "q1-vrapi-extract: no $APK" >&2; exit 1; }
[ -f "$OUT/libvrapiimpl.so" ] && exit 0
mkdir -p "$OUT"
python3 - "$APK" "$OUT" <<'PY'
import os, sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
for i in z.infolist():
    if i.filename.startswith("lib/arm64-v8a/") and i.filename.endswith(".so"):
        open(os.path.join(sys.argv[2], os.path.basename(i.filename)), "wb").write(z.read(i))
PY
chmod 755 "$OUT"; chmod 644 "$OUT"/*.so
