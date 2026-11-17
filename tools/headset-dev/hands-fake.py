#!/usr/bin/env python3
"""Fake q1handsd for testing driver_quest1's hand tracking without a worn headset (docs/hand-tracking.md).

Writes /run/q1track/hands (same layout as src/q1track/q1hands_shm.h) with a right hand held up in front of the head,
palm toward the face, the index finger curling now and then. Needs the skeleton file q1handsd made
(/run/q1track/handskel.bin: run `q1handsd skeleton` once, or start android-q1handsd). Stop q1handsd first: both
write the same file.  Usage: hands-fake.py [seconds] [left|right|both]
"""
import math
import os
import struct
import sys
import time

PATH = "/run/q1track/hands"
SKEL = "/run/q1track/handskel.bin"
SIZE = 4368
BONES, SB = 19, 24


def qmul(a, b):  # x y z w
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def qaxis(axis, ang):
    s = math.sin(ang / 2)
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(ang / 2))


def qmat(m):  # 3x3 rows -> x y z w
    t = m[0][0] + m[1][1] + m[2][2]
    if t > 0:
        s = math.sqrt(t + 1) * 2
        return ((m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s, 0.25 * s)
    i = max(range(3), key=lambda k: m[k][k])
    j, k = (i + 1) % 3, (i + 2) % 3
    s = math.sqrt(m[i][i] - m[j][j] - m[k][k] + 1) * 2
    q = [0, 0, 0, 0]
    q[i] = 0.25 * s
    q[j] = (m[j][i] + m[i][j]) / s
    q[k] = (m[k][i] + m[i][k]) / s
    q[3] = (m[k][j] - m[j][k]) / s
    return tuple(q)


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 60
    which = sys.argv[2] if len(sys.argv) > 2 else "right"
    sk = open(SKEL, "rb").read()
    rot = struct.unpack_from("<%df" % (2 * SB * 4), sk, 4)
    pos = struct.unpack_from("<%df" % (2 * SB * 3), sk, 4 + 2 * SB * 16)
    par = struct.unpack_from("<%dh" % SB, sk, 4 + 2 * SB * 16 + 2 * SB * 12)
    bind = [[rot[(h * SB + b) * 4:(h * SB + b) * 4 + 4] for b in range(SB)] for h in range(2)]
    t0 = time.monotonic()
    seq = 0
    n = 0
    while time.monotonic() - t0 < secs:
        t = time.monotonic()
        ns = int(t * 1e9)
        curl = 0.5 - 0.5 * math.cos(t * 1.5)
        hands = b""
        for h in range(2):
            on = which == "both" or (which == "right" and h == 1) or (which == "left" and h == 0)
            sign = -1 if h == 1 else 1  # right: the bones run along -X
            # fingers up (world +Y), palm toward the face (+Z): Meta x -> -Y (right) / +Y (left), y -> -Z (right) / +Z (left)
            m = [[0, 0, 1], [sign, 0, 0], [0, sign, 0]] if h == 1 else [[0, 0, -1], [-1, 0, 0], [0, 1, 0]]
            q = qmat(m)
            b = [list(bind[h][j]) for j in range(BONES)]
            if on:  # curl the index finger (Index1..3 = bones 6..8) about the flexion axis (Z)
                for j in (6, 7, 8):
                    b[j] = list(qmul(bind[h][j], qaxis((0, 0, 1), sign * -1 * curl * 1.2)))
            wrist = (0.18 if h == 1 else -0.18, 1.45, -0.3)
            rec = struct.pack("<IIII", 1, 1 if on else 4, 1 if on else 0, 0)
            rec += struct.pack("<qq", ns - 20000000, ns)
            rec += struct.pack("<4f3f", *q, *wrist) + struct.pack("<f", 0)
            rec += b"".join(struct.pack("<4f", *x) for x in b)
            rec += struct.pack("<5f", *([0.9] * 5 if on else [-1.0] * 5)) + struct.pack("<5f", *([0.0] * 5))
            rec += struct.pack("<fI", 0.9, 6)
            hands += rec
        hdr = struct.pack("<IIII", 0x53444E48, 1, 0, os.getpid())
        mid = struct.pack("<qQ", ns, n) + struct.pack("<II", 1, 1) + struct.pack("<2I2I", 0, 0, 1, 1)
        mid += struct.pack("<fI", 1.0, 1) + struct.pack("<4f3fI", 0, 0, 0, 1, 0, 1.6, 0, 1)
        skel = struct.pack("<%df" % (2 * SB * 4), *rot) + struct.pack("<%df" % (2 * SB * 3), *pos)
        skel += struct.pack("<%dh" % SB, *par)
        blob = hdr + mid + hands + skel
        blob += b"\0" * (SIZE - len(blob))
        assert len(blob) == SIZE, len(blob)
        seq += 2
        tmp = PATH + ".tmp"
        with open(tmp, "wb") as f:
            f.write(blob[:8] + struct.pack("<I", seq) + blob[12:])
        os.rename(tmp, PATH)
        n += 1
        time.sleep(1 / 60)
    os.unlink(PATH)


if __name__ == "__main__":
    main()
