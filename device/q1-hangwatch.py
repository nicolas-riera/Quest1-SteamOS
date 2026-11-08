#!/usr/bin/python3
"""q1-hangwatch: log what blocks the headset, into the kernel log (kept across a reboot in pstore's console-ramoops,
which an Android `adb bugreport` shows as LAST KMSG).

The kernel has neither the hung task detector nor the lockup detector. On 2026-11-08 the headset stopped answering
SSH for 35 min while Monado and journald kept running, then froze when the panels came on, and nothing in the log said
why. Every 5 s this reads /proc and writes to /dev/kmsg only when something is wrong:
  - a user thread in uninterruptible sleep (D) in two samples in a row: its kernel stack (once per thread and
    minute; kernel threads are left out, many of them idle in D);
  - a real-time thread that used more than 80 % of a CPU over the last interval (a spinning RT thread starves the
    per-CPU kworkers of its CPU, and anything waiting for them hangs);
  - a heartbeat once a minute: load, available memory, swap in/out pages a second.
It runs SCHED_FIFO 1 (above every normal thread, below the display, tracking and audio threads: a scan takes ~50 ms)
with its memory locked, so it still runs when normal threads starve it or the disk stalls; it costs
~1 % of a CPU.
"""
import ctypes, os, threading, time

INTERVAL = 5.0
HZ = os.sysconf("SC_CLK_TCK")

libc = ctypes.CDLL(None, use_errno=True)
libc.mlockall(3)  # MCL_CURRENT | MCL_FUTURE
try:
    os.sched_setscheduler(0, os.SCHED_FIFO, os.sched_param(1))
except OSError:
    pass

kmsg = os.open("/dev/kmsg", os.O_WRONLY)


def log(msg):
    for line in msg.splitlines():
        try:
            os.write(kmsg, (("<4>q1hang: " + line)[:900] + "\n").encode())
        except OSError:
            pass


def read(path):
    try:
        with open(path, "rb") as f:
            return f.read().decode(errors="replace")
    except OSError:
        return ""


_stuck = []


def read_stack(path):
    """/proc/<pid>/task/<tid>/stack takes the task's exec lock: a task blocked in execve on a page from a stalled disk
    holds it, and a plain read would hang this loop too (2026-11-08). Read it in a helper thread, give up after 0.3 s."""
    global _stuck
    _stuck = [t for t in _stuck if t.is_alive()]
    if len(_stuck) >= 8:
        return "(not read: earlier reads still blocked)"
    box = []
    t = threading.Thread(target=lambda: box.append(read(path)), daemon=True)
    t.start()
    t.join(0.3)
    if box:
        return box[0]
    _stuck.append(t)
    return "(not readable: the task holds its exec lock)"


def threads():
    out = {}
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            tids = os.listdir("/proc/%s/task" % pid)
        except OSError:
            continue
        for tid in tids:
            s = read("/proc/%s/task/%s/stat" % (pid, tid))
            r = s.rfind(")")
            if r < 0:
                continue
            comm = s[s.find("(") + 1:r]
            f = s[r + 2:].split()
            # f[0] = state (field 3); flags 9, utime 14, stime 15, processor 39, rt_priority 40, policy 41.
            # Kernel threads (PF_KTHREAD) are left out of the D check: many idle in uninterruptible sleep.
            kthread = bool(int(f[6]) & 0x00200000)
            out[int(tid)] = (comm, "S" if kthread and f[0] == "D" else f[0], int(f[11]) + int(f[12]), int(f[36]),
                             int(f[37]), int(f[38]), int(pid))
    return out


def vm():
    v = {}
    for line in read("/proc/vmstat").splitlines():
        k, _, n = line.partition(" ")
        if k in ("pswpin", "pswpout"):
            v[k] = int(n)
    for line in read("/proc/meminfo").splitlines():
        if line.startswith("MemAvailable:"):
            v["avail"] = int(line.split()[1]) // 1024
    return v


prev, prev_t, prev_vm = threads(), time.monotonic(), vm()
reported = {}
last_beat = 0.0
log("started")
while True:
    time.sleep(INTERVAL)
    cur, now = threads(), time.monotonic()
    dt = now - prev_t
    for tid, (comm, state, ticks, cpu, rtprio, policy, pid) in cur.items():
        p = prev.get(tid)
        if not p:
            continue
        if state == "D" and p[1] == "D" and now - reported.get(tid, -1e9) > 60:
            reported[tid] = now
            stack = read_stack("/proc/%d/task/%d/stack" % (pid, tid)).replace("\n", " | ")
            log("D for >%.0f s: %s pid %d tid %d wchan %s: %s" %
                (dt, comm, pid, tid, read("/proc/%d/task/%d/wchan" % (pid, tid)), stack))
        if policy in (1, 2):
            use = (ticks - p[2]) / HZ / dt
            if use > 0.8:
                log("RT thread %s pid %d tid %d (%s %d, cpu %d) ran %.0f %% of a CPU over %.0f s" %
                    (comm, pid, tid, "FIFO" if policy == 1 else "RR", rtprio, cpu, use * 100, dt))
    if now - last_beat >= 60:
        v = vm()
        log("load %s, available %d MB, swap in %.0f out %.0f pages/s" %
            (" ".join(read("/proc/loadavg").split()[:3]), v.get("avail", 0),
             (v.get("pswpin", 0) - prev_vm.get("pswpin", 0)) / max(now - last_beat, 1),
             (v.get("pswpout", 0) - prev_vm.get("pswpout", 0)) / max(now - last_beat, 1)) if last_beat else
            "load %s, available %d MB" % (" ".join(read("/proc/loadavg").split()[:3]), v.get("avail", 0)))
        prev_vm, last_beat = v, now
    reported = {t: r for t, r in reported.items() if t in cur}
    prev, prev_t = cur, now
