#!/bin/sh
# mic-check.sh [SECONDS] (as root on the headset): the microphone's health while an application records it. Records
# the echo-cancelled source for SECONDS (default 10; the level is computed in memory, nothing is kept), with PipeWire's
# debug log for 3 s of it, and counts what made the voice crackle before 2026-11-17 (docs/audio.md): the capture's
# resyncs, the echo canceller's dropped samples, the nodes' xruns. A healthy system: 0 resyncs (2 at the start of a
# stream is the start), 0 ring buffer xruns, no new node errors.
n=${1:-10}
U="runuser -u steamos -- env XDG_RUNTIME_DIR=/run/user/1000"
err() { $U pw-top -b -n 2 2>/dev/null | awk '$NF=="quest1-output"{o=$9} $NF=="quest1-mic"{m=$9} $NF=="quest1-mic-aec"{a=$9; q=$3} END{print o+0, m+0, a+0, q+0}'; }
level() {
	python3 -c '
import sys, array, math
d = sys.stdin.buffer.read()
a = array.array("h", d[: len(d) // 2 * 2])
print("level: rms %.0f, peak %d of 32767" % (math.sqrt(sum(x * x for x in a) / max(len(a), 1)), max(map(abs, a)) if a else 0))'
}
($U timeout $((n + 3)) parecord -d quest1-mic-aec --raw --format=s16le --channels=1 --rate=48000 2>/dev/null | level) &
sleep 2
set -- $(err)
$U pw-metadata -n settings 0 log.level 4 >/dev/null
sleep 3
$U pw-metadata -n settings 0 log.level 2 >/dev/null
sleep $((n - 3 > 0 ? n - 3 : 0))
set -- "$@" $(err)
L=$(journalctl -b _UID=1000 --since "-$((n + 1))s" --no-pager -o cat)
wait
echo "quantum $4; in ${n} s: errors output +$(($5 - $1)), mic +$(($6 - $2)), echo-cancelled +$(($7 - $3));" \
	"in 3 s of debug log: $(echo "$L" | grep -c "resync") resyncs, $(echo "$L" | grep -c "ringbuffer xrun") ring buffer xruns"
echo "MIC Volume: $(amixer -c 0 cget name='MIC Volume' | sed -n 's/.*: values=//p') (23 = 0 dB)"
