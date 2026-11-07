#!/bin/sh
# run-tests.sh DIR: vdectest's matrix on the headset (as root), with the streams and reference checksums in DIR:
#   t1080.264 / t1080.265  1920x1080 60 fps, no B-frames (ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=60 -t 5
#                          -c:v libx264 -bf 0 -g 60 -bsf:v h264_mp4toannexb -f h264, and libx265 bframes=0:keyint=60)
#   ref264.md5 / ref265.md5  one md5 per frame of ffmpeg's software decode as NV12 (the frames, cropped)
# Linear NV12 runs are checked frame by frame against them; UBWC runs only for speed and latency. The linear runs'
# speed is mostly the CPU reading uncached ION memory to write the file.
set -u
D=${1:?usage: run-tests.sh DIR}
V=$(dirname "$0")/vdectest
cmp_md5() { # decoded.nv12 ref.md5
	python3 - "$1" "$2" <<'PY'
import hashlib, sys
fs = 1920 * 1080 * 3 // 2
ref = open(sys.argv[2]).read().split()
ok = n = 0
with open(sys.argv[1], "rb") as f:
    while True:
        b = f.read(fs)
        if len(b) < fs:
            break
        ok += n < len(ref) and hashlib.md5(b).hexdigest() == ref[n]
        n += 1
print(f"  frames {n}, bit-exact {ok}/{len(ref)}")
PY
}
for c in 264:h264 265:hevc; do
	ext=${c%%:*}; codec=${c#*:}
	for m in split primary; do
		echo "== $codec, linear NV12, $m"
		$V -c $codec -s 1920x1080 -m $m -o /tmp/vdec.nv12 "$D/t1080.$ext" && cmp_md5 /tmp/vdec.nv12 "$D/ref$ext.md5"
		rm -f /tmp/vdec.nv12
	done
	echo "== $codec, UBWC (speed only)"
	$V -c $codec -s 1920x1080 -f ubwc "$D/t1080.$ext"
	echo "== $codec, UBWC, paced at 60 fps (a live stream: the latency that counts), decode order, realtime"
	$V -c $codec -s 1920x1080 -f ubwc -O decode -R -r 60 "$D/t1080.$ext"
done
