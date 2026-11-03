#!/usr/bin/env python3
# q1-audioring: diagnostic for the random crackling. While something plays (a pulse sink-input exists),
# keep the last 90 s of what PipeWire sends to the speakers (the sink monitor) and the ALSA PCM status
# every 20 ms in memory. SIGUSR1 dumps both, plus the kernel log and pw-top, to /root/audioring-<time>/.
# Recording only while something plays keeps the sink's own suspend/resume as it is.
# Install: install -m755 q1-audioring.py /usr/local/sbin/q1-audioring; start: systemd-run --unit q1-audioring /usr/local/sbin/q1-audioring
import collections, os, signal, subprocess, threading, time, wave

KEEP = 90.0
RATE, CH, BPS = 48000, 2, 2
ST = "/proc/asound/card0/pcm0p/sub0/status"
ENV = dict(os.environ, HOME="/home/steamos", XDG_RUNTIME_DIR="/run/user/1000",PULSE_SERVER="unix:/run/user/1000/pulse/native")
AS_USER = ["setpriv", "--reuid=1000", "--regid=1000", "--clear-groups"]

lock = threading.Lock()
audio = collections.deque()   # (monotonic time of the chunk's end, bytes)
status = collections.deque()  # (monotonic, wall, text)
rec = None
dump_req = threading.Event()


def trim(dq, now):
    while dq and dq[0][0] < now - KEEP:
        dq.popleft()


def playing():
    try:
        out = subprocess.run(AS_USER + ["pactl", "list", "short", "sink-inputs"], env=ENV,
                             capture_output=True, text=True, timeout=3).stdout
        return bool(out.strip())
    except Exception:
        return False


def reader(p):
    while True:
        b = p.stdout.read(RATE * CH * BPS // 50)
        if not b:
            return
        now = time.monotonic()
        with lock:
            audio.append((now, b))
            trim(audio, now)


def poll_status():
    while True:
        now = time.monotonic()
        try:
            with open(ST) as f:
                s = f.read()
            # state / hw_ptr / appl_ptr / avail / delay, one line
            fields = {}
            for line in s.splitlines():
                k, _, v = line.partition(":")
                fields[k.strip()] = v.strip()
            text = " ".join(f"{k}={fields[k]}" for k in ("state", "hw_ptr", "appl_ptr", "avail", "delay", "avail_max")
                            if k in fields) or s.strip()
        except OSError as e:
            text = f"err {e}"
        with lock:
            status.append((now, time.time(), text))
            trim(status, now)
        time.sleep(0.02)


def dump():
    d = time.strftime("/root/audioring-%H%M%S")
    os.makedirs(d, exist_ok=True)
    with lock:
        chunks = list(audio)
        st = list(status)
    t_end = time.time()
    with wave.open(d + "/monitor.wav", "wb") as w:
        w.setnchannels(CH); w.setsampwidth(BPS); w.setframerate(RATE)
        for _, b in chunks:
            w.writeframes(b)
    with open(d + "/monitor-chunks.txt", "w") as f:
        m0 = time.monotonic()
        for t, b in chunks:
            f.write(f"{t_end - (m0 - t):.3f} {len(b)}\n")
    with open(d + "/alsa-status.txt", "w") as f:
        for _, wall, text in st:
            f.write(f"{wall:.3f} {text}\n")
    os.system(f"journalctl -k --since -3min --no-pager > {d}/kernel.txt 2>&1")
    os.system(f"journalctl --user-unit pipewire --since -3min --no-pager > {d}/pipewire.txt 2>&1")
    os.system(f"cat /proc/meminfo /proc/loadavg /proc/asound/card0/pcm0p/sub0/hw_params > {d}/system.txt 2>&1")
    subprocess.run(AS_USER + ["timeout", "3", "pw-top", "-b", "-n", "2"], env=ENV,
                   stdout=open(d + "/pwtop.txt", "w"), stderr=subprocess.STDOUT)
    print(f"q1-audioring: dumped {len(chunks) * 20 / 1000:.0f} s of audio to {d}", flush=True)


def main():
    global rec
    signal.signal(signal.SIGUSR1, lambda *a: dump_req.set())
    threading.Thread(target=poll_status, daemon=True).start()
    idle = 0
    while True:
        if dump_req.is_set():
            dump_req.clear()
            dump()
        if rec is None or rec.poll() is not None:
            if playing():
                rec = subprocess.Popen(AS_USER + ["pw-record", "--target", "quest1-output.monitor", "--format", "s16",
                                                  "--rate", str(RATE), "--channels", str(CH), "--raw", "-"],
                                       env=ENV, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
                threading.Thread(target=reader, args=(rec,), daemon=True).start()
                idle = 0
        else:
            idle = 0 if playing() else idle + 1
            if idle >= 3:  # nothing plays for ~3 s: let the sink suspend
                rec.terminate()
                rec.wait()
                rec = None
        dump_req.wait(1.0)


main()
