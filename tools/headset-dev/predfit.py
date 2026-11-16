"""predfit.py TAG... (on the PC, in the directory of sub-TAG.txt and head-TAG.txt from pred-log-vrlink.patch).
Prediction bench analysis: the pose each stream frame was rendered with (vrlink's submits) against the head's
true pose (driver_quest1's, as replayed), as a function of the time after the submit."""
import numpy as np, sys


def rot2q(m):  # 3x3 -> w x y z
    t = np.trace(m)
    if t > 0:
        s = np.sqrt(t + 1) * 2
        return np.array([0.25 * s, (m[2, 1] - m[1, 2]) / s, (m[0, 2] - m[2, 0]) / s, (m[1, 0] - m[0, 1]) / s])
    i = np.argmax(np.diag(m))
    if i == 0:
        s = np.sqrt(1 + m[0, 0] - m[1, 1] - m[2, 2]) * 2
        return np.array([(m[2, 1] - m[1, 2]) / s, 0.25 * s, (m[0, 1] + m[1, 0]) / s, (m[0, 2] + m[2, 0]) / s])
    if i == 1:
        s = np.sqrt(1 + m[1, 1] - m[0, 0] - m[2, 2]) * 2
        return np.array([(m[0, 2] - m[2, 0]) / s, (m[0, 1] + m[1, 0]) / s, 0.25 * s, (m[1, 2] + m[2, 1]) / s])
    s = np.sqrt(1 + m[2, 2] - m[0, 0] - m[1, 1]) * 2
    return np.array([(m[1, 0] - m[0, 1]) / s, (m[0, 2] + m[2, 0]) / s, (m[1, 2] + m[2, 1]) / s, 0.25 * s])


def qmul(a, b):
    w1, x1, y1, z1 = a.T
    w2, x2, y2, z2 = b.T
    return np.stack([w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2, w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2,
                     w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2, w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2], -1)


def conj(q): return q * np.array([1, -1, -1, -1])


def qlog(q):  # rotation vector, degrees
    q = q * np.sign(q[..., :1] + 1e-15)
    v = q[..., 1:]
    s = np.linalg.norm(v, axis=-1, keepdims=True)
    return np.degrees(v * np.where(s > 1e-12, 2 * np.arctan2(s, q[..., :1]) / np.maximum(s, 1e-12), 2))


def slerp_at(t, T, Q):
    i = np.clip(np.searchsorted(T, t) - 1, 0, len(T) - 2)
    a = ((t - T[i]) / (T[i + 1] - T[i]))[:, None]
    q0, q1 = Q[i], Q[i + 1] * np.sign((Q[i] * Q[i + 1]).sum(1, keepdims=True))
    q = q0 * (1 - a) + q1 * a
    return q / np.linalg.norm(q, axis=1, keepdims=True)


def analyse(sub, head, label):
    H = np.loadtxt(head)
    S = np.loadtxt(sub)
    Th, Qh = H[:, 0] * 1e-9, H[:, 2:6]
    Ts = S[:, 0] * 1e-9
    Qs = np.array([rot2q(r[1:].reshape(3, 4)[:, :3]) for r in S])
    keep = (Ts > Th[0] + 0.2) & (Ts < Th[-1] - 0.3)
    Ts, Qs = Ts[keep], Qs[keep]
    # head speed at the submit, for the moving-only statistics
    w = np.linalg.norm(qlog(qmul(conj(slerp_at(Ts - 0.01, Th, Qh)), slerp_at(Ts + 0.01, Th, Qh))), axis=1) / 0.02
    mov = w > 30
    res = []
    for D in np.arange(-0.02, 0.151, 0.005):
        Hd = slerp_at(Ts + D, Th, Qh)
        R = qmul(Qs, conj(Hd))  # F = C H  ->  C = F H^-1
        R *= np.sign(R[:, :1] + 1e-15)
        C = R.mean(0)
        C /= np.linalg.norm(C)
        E = qlog(qmul(qmul(conj(np.tile(C, (len(Qs), 1))), Qs), conj(Hd)))  # the frame's pose against the head's
        e = np.linalg.norm(E, axis=1)
        res.append((D, np.sqrt((e ** 2).mean()), np.sqrt((e[mov] ** 2).mean()), np.percentile(e, 99)))
    res = np.array(res)
    b = res[np.argmin(res[:, 1])]
    print(f'{label}: {len(Ts)} frames, moving >30 deg/s: {mov.mean() * 100:.0f} %, median head speed {np.median(w):.0f} deg/s')
    print(f'  best match: the frames show the head {b[0] * 1e3:.0f} ms after their submit (rms {b[1]:.2f} deg)')
    for D in (0.0, 0.02, 0.03, 0.04, 0.05, 0.06):
        r = res[np.argmin(abs(res[:, 0] - D))]
        print(f'  head at submit+{D * 1e3:3.0f} ms: err rms {r[1]:.2f} deg, moving {r[2]:.2f}, p99 {r[3]:.2f}')
    return res


for tag in sys.argv[1:]:
    analyse(f'sub-{tag}.txt', f'head-{tag}.txt', tag)
