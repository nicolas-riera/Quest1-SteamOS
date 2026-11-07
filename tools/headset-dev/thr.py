#!/usr/bin/python3
# thr.py [seconds]: per-thread CPU % over an interval, all processes, top 35
import os, sys, time
def snap():
    d = {}
    for p in os.listdir('/proc'):
        if not p.isdigit(): continue
        try:
            pc = open(f'/proc/{p}/comm').read().strip()
            for t in os.listdir(f'/proc/{p}/task'):
                s = open(f'/proc/{p}/task/{t}/stat').read()
                r = s.rsplit(')', 1)
                tc = r[0].split('(', 1)[1]; f = r[1].split()
                d[t] = (pc, tc, int(f[11]) + int(f[12]), int(f[36]) if len(f) > 36 else -1)
        except Exception: pass
    return d
T = float(sys.argv[1]) if len(sys.argv) > 1 else 5
a = snap(); time.sleep(T); b = snap()
hz = os.sysconf('SC_CLK_TCK')
rows = []
for t, (pc, tc, c, cpu) in b.items():
    if t in a: rows.append(((c - a[t][2]) * 100.0 / hz / T, pc, tc, cpu))
rows.sort(reverse=True)
tot = sum(r[0] for r in rows)
print('total %.0f%% of 800%%' % tot)
for r in rows[:35]: print('%5.1f%% %-16s %-16s cpu%d' % r)
