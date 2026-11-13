# mjit.py N X,Y [X,Y...]: as root, Monado's warped output (linear outputs: QUEST1_WARP_USAGE=0, fake-motion-monado.patch
# applied): a still reference (no fake yaw), then N snapshots with the fake yaw on (4 deg 2 Hz); per region (200x120),
# the shift against the reference (README.md). Expects shift (shift.c) in /tmp/q1hd; the snapshots are deleted.
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
fake("0 0"); time.sleep(1.5); snap(D + "/ref.ppm")
fake("4 2"); time.sleep(1.5)
for _ in range(n):
    snap(D + "/cur.ppm")
    print(" ".join(shift(r) for r in regions), flush=True)
fake("0 0")
for f in ("ref.ppm", "cur.ppm"): os.unlink(D + "/" + f)
