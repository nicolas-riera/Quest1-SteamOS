# pt-state.py [i=v ...]: print / set bytes of the VR_CameraPassthroughState shm (25088 bytes)
import os, sys, mmap
for n in os.listdir("/dev/shm"):
    p = "/dev/shm/" + n
    if n.startswith("u") and "-Shm_" in n and os.path.getsize(p) == 25088:
        f = open(p, "r+b"); m = mmap.mmap(f.fileno(), 25088)
        for a in sys.argv[1:]:
            i, v = a.split("="); m[int(i)] = int(v)
        print(n, list(m[0:12]))
