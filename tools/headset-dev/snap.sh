#!/bin/sh
# snap.sh OUT.png: snapshot of the next frame, downscaled 4x (private: camera image)
rm -f /tmp/quest1-snap.ppm; runuser -u steamos -- touch /tmp/quest1-snap
for i in $(seq 1 30); do [ -f /tmp/quest1-snap.ppm ] && break; sleep 0.5; done; sleep 1
python3 /root/pt-snapdown.py "$1" 4; rm -f /tmp/quest1-snap.ppm
