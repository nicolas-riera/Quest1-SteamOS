# marker-fit.py HZ FILE...: reads marker-jitter.py's output with the marker region first and a stream region second.
# Fits the stream's horizontal shift to the marker's (both world-locked, so stream = k * marker + b when every stream
# frame sits where its pose says) and prints the residual, then each residual as a time offset (residual / the
# stream region's speed at that moment, from a sine fitted to the marker): 0 ms is a frame shown right, n ms a frame
# whose picture is n ms of motion away from its pose. Needs numpy (run it on the PC).
import math
import sys

import numpy as np

hz = float(sys.argv[1])
w = 2 * math.pi * hz
for fn in sys.argv[2:]:
    d = np.array([[float(v) for v in line.split()] for line in open(fn) if line.strip() and "?" not in line])
    ph = w * (np.mod(d[:, 0], 1e12) * 1e-9)
    m, s = d[:, 1], d[:, 3]
    k, b = np.polyfit(m, s, 1)
    r = s - (k * m + b)
    good = abs(r) <= max(2 * np.median(abs(r)), 2)  # refit on the frames that agree: wrong ones would pull k and b
    k, b = np.polyfit(m[good], s[good], 1)
    r = s - (k * m + b)
    print("%s: n %d, stream = %.2f * marker %+.1f, residual sd %.2f px, p90 %.1f, max %.1f" % (
        fn, len(d), k, b, r.std(), np.percentile(abs(r), 90), abs(r).max()))
    best = None
    for tau in np.arange(-0.12, 0.12, 0.0005):
        a = np.column_stack([np.sin(ph - w * tau), np.ones_like(ph)])
        c, *_ = np.linalg.lstsq(a, m, rcond=None)
        e = ((m - a @ c) ** 2).sum()
        if best is None or e < best[0]:
            best = (e, tau, c)
    _, tau, c = best
    vel = k * c[0] * w * np.cos(ph - w * tau)
    fast = abs(vel) > 150
    ms = 1e3 * r[fast] / vel[fast]
    print("  time offsets (frames moving > 150 px/s): %d%% within 6 ms" % (100 * np.mean(abs(ms) < 6)))
    h, e = np.histogram(ms, bins=np.arange(-100, 101, 8))
    for i in range(len(h)):
        if h[i]:
            print("  %5d..%4d ms %s" % (e[i], e[i + 1], "#" * h[i]))
