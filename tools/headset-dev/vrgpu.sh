#!/bin/sh
# vrgpu.sh: GPU time per frame of each context over 2 s (vrcompositor = "q1compat submi"), SteamVR fps
sh /root/gputrace.sh 2 >/dev/null
python3 - <<'PY'
import re, collections
R = re.compile(r'adreno_cmdbatch_retired: ctx=(\d+) .*?start=(\d+) retire=(\d+)')
ev = [(int(c), int(s), int(r)) for c, s, r in R.findall(open('/tmp/gpu.trace').read())]
names = {}
for l in open('/tmp/gpu.trace'):
    m = re.search(r'^\s*(.*)-(\d+)\s+\[.*adreno_cmdbatch_queued: ctx=(\d+)', l)
    if m: names[int(m.group(3))] = m.group(1).strip()
F = 19.2e3
span = (max(r for _, _, r in ev) - min(s for _, s, _ in ev)) / F
# own GPU time: union of a context's batches minus the time other contexts' batches overlap them
iv = collections.defaultdict(list)
for c, s, r in ev: iv[c].append((s, r))
out = []
for c, l in iv.items():
    tot = sum(r - s for s, r in l) / F
    big = [r - s for s, r in l if (r - s) / F > 0.5]
    out.append((tot, '%s: %.0f%% (%d batches >0.5ms/s, %.1f ms each incl. preemption)' % (names.get(c, 'ctx%d' % c), 100 * tot / span, len(big) * 1000 / span, sum(big) / F / max(len(big), 1))))
print('\n'.join(t for _, t in sorted(out, reverse=True)))
PY
grep -h "frames/s" /home/steamos/.local/share/Steam/logs/vrserver.txt | tail -1 | sed 's/.*quest1: /SteamVR shown: /; s/ SteamVR frames.s shown.*/ fps/'
grep -h "display:" /tmp/vrserver.out | tail -1 | sed 's/.*display: /vrcompositor: /'
journalctl -u monado --no-pager -n 1 | sed 's/.*mdp: /monado: /' | cut -c1-60
