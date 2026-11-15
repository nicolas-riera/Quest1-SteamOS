# stepsnap.py N STEP_DEG X,Y [X,Y...]: the stream's picture right after a change of the head pose the PC renders for
# (docs/streaming.md item 13, 2026-11-15). On the headset as steamos, the headset still with its panels on, a stream
# running, pose-knobs-vrlink.patch in the shim. The pose sent to the PC steps by STEP_DEG (yaw; AXIS=pitch for pitch)
# back and forth through /tmp/q1-poseoff; each step is waited for in vrlink's submits (/tmp/q1-labellog), then a
# snapshot of vrcompositor's output is taken 0-40 ms later. Prints to /tmp/q1wj/out.txt per snapshot: its time, the
# first submit with the new pose (both CLOCK_MONOTONIC ns), the step's sign, and each 200x120 region's shift (dx dy)
# against a still reference (./shift from shift.c in /tmp/q1vr). A picture placed right stays at 0 px at any time
# after a step. Pick regions of the PC's scene with contrast in the step's direction (a horizon for pitch), away from
# the headset's own overlays, which the steps never move. Snapshots are deleted; /tmp/q1lab.txt is left.
import os, subprocess, sys, time, random, math

n, step = int(sys.argv[1]), sys.argv[2]
regions = [a.split(",") for a in sys.argv[3:]]
D = "/tmp/q1wj"
SHIFT = "/tmp/q1vr/shift"
os.makedirs(D, exist_ok=True)


def trigger():
    try:
        os.unlink("/tmp/quest1-snap.ppm")
    except OSError:
        pass
    open("/tmp/quest1-snap", "w").close()
    while os.path.exists("/tmp/quest1-snap"):
        time.sleep(0.0002)
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC)


def collect(path):
    for _ in range(300):
        if os.path.exists("/tmp/quest1-snap.ppm"):
            break
        time.sleep(0.01)
    time.sleep(0.05)
    os.rename("/tmp/quest1-snap.ppm", path)


def shift(a, b, x, y):
    out = subprocess.run([SHIFT, a, b, x, y, "200", "120", "60"], capture_output=True, text=True).stdout
    if "shift dx " not in out:
        return "? ?"
    return out.split("shift dx ")[1].split(" (")[0].replace(" dy", "")


def poseoff(v):
    open("/tmp/q1-poseoff.tmp", "w").write(("0 %s\n" if os.environ.get("AXIS") == "pitch" else "%s 0\n") % v)
    os.rename("/tmp/q1-poseoff.tmp", "/tmp/q1-poseoff")


def yaw(line):
    p = line.split()
    if os.environ.get("AXIS") == "pitch":
        return math.degrees(math.asin(max(-1.0, min(1.0, float(p[8]))))), int(p[0])
    return math.degrees(math.atan2(float(p[7]), -float(p[9]))), int(p[0])


for f in ("/tmp/q1-posenoise", "/tmp/q1-posesine"):
    try:
        os.unlink(f)
    except OSError:
        pass
poseoff("0")
time.sleep(1.5)
trigger()
collect(D + "/ref.ppm")
open("/tmp/q1-labellog", "w").close()
time.sleep(0.6)
lab = open("/tmp/q1lab.txt")
out = open(D + "/out.txt", "w")
s = 0
last = None
for i in range(n):
    s ^= 1
    poseoff(step if s else "0")
    t_step = None
    t0 = time.time()
    while t_step is None and time.time() - t0 < 1.0:
        line = lab.readline()
        if not line:
            time.sleep(0.0005)
            continue
        if " pos " not in line:
            continue
        y, t = yaw(line)
        if last is not None and abs(y - last) > 0.8:
            t_step = t
        last = y
    if t_step is None:
        continue
    time.sleep(random.uniform(0, 0.040))
    t = trigger()
    collect(D + "/cur.ppm")
    out.write("%d %d %d %s\n" % (t, t_step, 1 if s else -1,
                                 " ".join(shift(D + "/ref.ppm", D + "/cur.ppm", x, y) for x, y in regions)))
    out.flush()
    for line in iter(lab.readline, ""):  # skip what came meanwhile, keep the last yaw
        if " pos " in line:
            last = yaw(line)[0]
poseoff("0")
os.unlink("/tmp/q1-labellog")
for f in ("ref.ppm", "cur.ppm"):
    os.unlink(os.path.join(D, f))
