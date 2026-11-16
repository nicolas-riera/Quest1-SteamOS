#!/bin/sh
# Keep Meta's tracking on CPUs 0-3 for itself, and everything else in userspace on CPUs 4-7 (run as root on the native
# Holo system; idempotent). Install: sh holo-cpuset.sh
#
# Why: trackingservice needs ~3 of the four little cores (IOTcam ~1.2, controllers ~0.85, the SyncBoss thread, the
# sensors HAL). Android keeps everything else off them; here vrserver, Steam and its webhelpers ran there too, and the
# engine answered with "Map tracking did not run" and "Bad map tracking" 30-50 times a minute, then lost its map and
# opened new ones (2026-11-12/13, docs/boundary.md "Tracking starved of CPU"). A live `taskset` of everything to 4-7
# fixed it (0-1 bad frames a minute) but was lost at each restart, and Steam pins its own webhelpers to CPUs 2-6 by
# itself, which no CPUAffinity= can beat.
#
# How: a cpuset v1 hierarchy (compiled into the 4.4 kernel; cgroup2 only has memory+pids, no AllowedCPUs), mounted on
# /run/q1cpuset (/dev/cpuset is the RAM-file stand-in of Android's cpusets for libosutils, leave it). Two sets:
#   tracking  CPUs 0-3  the android-* units (Meta's stack)
#   rest      CPUs 4-7  every other userspace task, Steam, SteamVR, Monado, q1apps, sshd...
# A cpuset is a hard limit: when Steam calls sched_setaffinity(2-6) the kernel cuts it to 4-6. Children inherit the set,
# and PID 1 is in `rest`, so every unit systemd starts later is born there. Kernel threads stay in the root set
# (per-CPU kthreads cannot move and must not). The cgroup2 tree systemd uses is untouched: a v1 hierarchy with a
# controller cgroup2 does not own coexists with it.
# Two exceptions in `rest` although they are android-* units: q1trackd (feeds the poses to Monado, which presents at
# 72 Hz, and itself only waits on binder and copies; on the little cores it would queue behind trackingservice's 3
# busy cores) and q1anchord (a poll loop). Monado stays in `rest` with its RT threads (vsync 98, compositor 51-53).
# android-* units must not set CPUAffinity=0-3 any more: from inside `rest` that mask is empty and the start fails.
# Instead they run `q1-cpuset quick` as ExecStartPost, which moves the new unit's tasks to `tracking`.
set -e

cat > /usr/local/sbin/q1-cpuset <<'EOF'
#!/usr/bin/python3
# q1-cpuset setup | reconcile | quick | status: see device/holo-cpuset.sh
import os, sys

M = "/run/q1cpuset"
SETS = {"tracking": "0-3", "rest": "4-7"}
# android-* units that belong in `rest` (see holo-cpuset.sh)
REST_UNITS = ("android-q1trackd.service", "android-q1anchord.service", "android-q1anchord-restore.service")


def mount():
    if not os.path.isdir(M + "/tracking"):
        os.makedirs(M, exist_ok=True)
        if os.system("mountpoint -q " + M) != 0:
            if os.system("mount -t cgroup -o cpuset none " + M) != 0:
                sys.exit("q1-cpuset: cannot mount the cpuset hierarchy")
    for name, cpus in SETS.items():
        d = "%s/%s" % (M, name)
        os.makedirs(d, exist_ok=True)
        open(d + "/cpuset.cpus", "w").write(cpus)
        open(d + "/cpuset.mems", "w").write("0")


def target(pid):
    try:
        with open("/proc/%s/stat" % pid) as f:
            st = f.read().rsplit(")", 1)[1].split()
        if st[0] == "Z" or int(st[6]) & 0x200000:  # zombie, PF_KTHREAD
            return None
        for line in open("/proc/%s/cgroup" % pid):
            path = line.rstrip().split(":", 2)[2]
            # every component: a service's sub-cgroup too
            for part in path.split("/"):
                if part.startswith("android-") and part.endswith(".service"):
                    return "rest" if part in REST_UNITS else "tracking"
        return "rest"
    except (OSError, IndexError, ValueError):
        return None


def cur(tid):
    try:
        return os.path.basename(open("/proc/%s/cpuset" % tid).read().strip()) or "/"
    except OSError:
        return None


def move(pid, want):
    # all threads (the tasks file is per thread); again until nothing is left behind: a thread created while
    # we were moving inherits the creator's old set
    n = 0
    for _ in range(4):
        try:
            tids = os.listdir("/proc/%s/task" % pid)
        except OSError:
            break
        k = 0
        for t in tids:
            if cur(t) == want:
                continue
            try:
                with open("%s/%s/tasks" % (M, want), "w") as f:
                    f.write(t)
                k += 1
            except OSError:
                pass  # exited meanwhile, or not movable
        n += k
        if k == 0:
            break
    return n


def run(full, passes):
    mount()
    total = 0
    for _ in range(passes):
        n = 0
        for pid in os.listdir("/proc"):
            if not pid.isdigit():
                continue
            want = target(pid)
            if want is None:
                continue
            # quick: only look at the process leader (new threads inherit the set of their creator)
            if full or cur(pid) != want:
                n += move(pid, want)
        total += n
        if n == 0:
            break
    return total


def status():
    out = {}
    for pid in os.listdir("/proc"):
        if pid.isdigit() and target(pid):
            out.setdefault(cur(pid), set()).add(pid)
    for k, v in sorted(out.items()):
        print("%s: %d processes" % (k, len(v)))


cmd = sys.argv[1] if len(sys.argv) > 1 else "setup"
if cmd in ("setup", "reconcile"):
    print("q1-cpuset: moved %d tasks" % run(True, 3))
elif cmd == "quick":
    run(False, 1)
elif cmd == "status":
    status()
else:
    sys.exit("usage: q1-cpuset setup|reconcile|quick|status")
EOF
chmod 755 /usr/local/sbin/q1-cpuset

# Before the other units, so that none starts in the root set; the timer is the safety net (a process whose
# parent was outside the sets, a unit started by hand before the boot service ran)
cat > /etc/systemd/system/q1-cpuset.service <<'EOF'
[Unit]
Description=Quest 1 cpusets: Meta's tracking on CPUs 0-3, everything else on 4-7
DefaultDependencies=no
After=local-fs.target
Before=sysinit.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/q1-cpuset setup

[Install]
WantedBy=sysinit.target
EOF
cat > /etc/systemd/system/q1-cpuset-quick.service <<'EOF'
[Unit]
Description=Quest 1 cpusets: put strays back in their set
After=q1-cpuset.service

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/q1-cpuset quick
EOF
cat > /etc/systemd/system/q1-cpuset-quick.timer <<'EOF'
[Unit]
Description=Quest 1 cpusets: put strays back in their set

[Timer]
OnBootSec=30
OnUnitActiveSec=30
AccuracySec=1s

[Install]
WantedBy=timers.target
EOF
systemctl daemon-reload
systemctl enable q1-cpuset.service q1-cpuset-quick.timer
echo "holo-cpuset: installed (q1-cpuset setup moves everything now; q1-cpuset status)"
