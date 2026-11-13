# mjit.py N X,Y [X,Y...]: as root, Monado's warped output (linear outputs: QUEST1_WARP_USAGE=0, fake-motion-monado.patch
# applied): a still reference (no fake yaw), then N snapshots with the fake motion on ($MOTION, "DEG HZ [M]", default
# 4 deg 2 Hz; or with $REPLAY "S", replay-head-monado.patch's recorded head scaled by S, the seconds since it started
# last on each line); per region (200x120), the shift against the reference (README.md). Expects shift (shift.c) in /tmp/q1hd; the snapshots are deleted.
import os, subprocess, sys, time
n = int(sys.argv[1]); regions = [a.split(",") for a in sys.argv[2:]]
D = "/tmp/q1hd"
def snap(path):
    try: os.unlink("/tmp/quest1-snap-monado.ppm")
    except OSError: pass
    open("/tmp/quest1-snap-monado", "w").close()
    for _ in range(300):
        if os.path.exists("/tmp/quest1-snap-monado.ppm"): break
        time.sleep(0.01)
    time.sleep(0.05)
    os.rename("/tmp/quest1-snap-monado.ppm", path)
def fake(s):
    subprocess.run(["runuser", "-u", "steamos", "--", "sh", "-c", "echo %s > /tmp/q1-fake-yaw" % s])
def shift(xy):
    r = subprocess.run([D + "/shift", D + "/ref.ppm", D + "/cur.ppm", xy[0], xy[1], "200", "120", "85"], capture_output=True, text=True).stdout
    return r.split("shift dx ")[1].split(" (")[0].replace(" dy", "") if "shift dx" in r else "? ?"
rep = os.environ.get("REPLAY")
def replay(s):
    subprocess.run(["runuser", "-u", "steamos", "--", "sh", "-c", "echo %s > /tmp/q1-replay" % s if s else "rm -f /tmp/q1-replay"])
fake("0 0"); replay(None); time.sleep(1.5); snap(D + "/ref.ppm")
if rep: replay(rep)
else: fake(os.environ.get("MOTION", "4 2"))
t0 = time.monotonic(); time.sleep(0 if rep else 1.5)
for _ in range(n):
    snap(D + "/cur.ppm")
    print(" ".join(shift(r) for r in regions) + (" %.2f" % (time.monotonic() - t0) if rep else ""), flush=True)
fake("0 0"); replay(None)
for f in ("ref.ppm", "cur.ppm"): os.unlink(D + "/" + f)
