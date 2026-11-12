# marker-jitter.py AMP_DEG HZ N POS_M X,Y [X,Y...]: on the headset, as steamos, with the panels on and
# fake-motion.patch in driver_quest1: sets the fake head motion (/tmp/q1-fake-yaw "AMP HZ POS"), takes a still
# snapshot as the reference, then N snapshots during the motion. Prints per snapshot the CLOCK_MONOTONIC time at which
# the layer took it and, per 200x120 region at (X, Y) of the 2880x1600 snapshot, the shift (dx dy) against the
# reference (./shift, built from shift.c). Snapshots stay in /tmp/q1jit and are deleted at the end.
import os
import subprocess
import sys
import time

amp, hz, n, pos = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
regions = [a.split(",") for a in sys.argv[5:]]
D = "/tmp/q1jit"
SHIFT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "shift")


def snap(path):
    try:
        os.unlink("/tmp/quest1-snap.ppm")
    except OSError:
        pass
    open("/tmp/quest1-snap", "w").close()
    while os.path.exists("/tmp/quest1-snap"):  # the layer removes the trigger when it takes the frame
        time.sleep(0.0005)
    t = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
    for _ in range(200):
        if os.path.exists("/tmp/quest1-snap.ppm"):
            break
        time.sleep(0.01)
    os.rename("/tmp/quest1-snap.ppm", path)
    return t


def shift(xy):
    r = subprocess.run([SHIFT, D + "/ref.ppm", D + "/cur.ppm", xy[0], xy[1], "200", "120", "85"],
                       capture_output=True, text=True).stdout
    return r.split("shift dx ")[1].split(" (")[0].replace(" dy", "") if "shift dx" in r else "? ?"


def fake(s):
    with open("/tmp/q1-fake-yaw", "w") as f:
        f.write(s + "\n")


os.makedirs(D, exist_ok=True)
fake("0 0")
time.sleep(1.5)
snap(D + "/ref.ppm")
fake("%s %s %s" % (amp, hz, pos))
time.sleep(1.5)
for _ in range(n):
    t = snap(D + "/cur.ppm")
    print(t, " ".join(shift(r) for r in regions), flush=True)
fake("0 0")
for f in ("ref.ppm", "cur.ppm"):
    os.unlink(D + "/" + f)
