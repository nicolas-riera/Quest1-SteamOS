#!/usr/bin/env python3
"""Installer agent: runs in the micro Linux the PC app fastboot-boots on the headset (tools/build-installer.sh) and
lets the app build the SteamOS image there, over the USB network (http://192.168.77.1:8077). Standard library only.

Nothing is written to the headset until the app asks for the scratch space (POST /scratch with the confirmation
text): then `userdata` (wiped by the install anyway, flashed by the app at the end) is formatted and mounted as
/scratch. Every other partition is left alone.

    GET  /info                 {"agent", "userdata": bytes, "mem": bytes, "scratch": bool, "build": state, "rc"}
    POST /scratch              body {"confirm": "erase userdata"}: format (once) and mount the scratch space
    HEAD /stage/<path>         X-Size / X-Sha256 of a file already uploaded (the app skips it)
    PUT  /stage/<path>         upload a file of prepare.py's stage/ (header X-Sha256 required, checked)
    POST /build                body {"only": "steps" (optional)}: run build_image.py on the stage
    GET  /events?from=N        {"next": M, "lines": [...]}: the build's output from line N (the @step/@progress
                               lines of build_image.py drive the app's progress bars)
    GET  /image                the sparse userdata image (for `fastboot flash userdata`)
    POST /reboot               body {"target": "bootloader" | ""}
For development: agent.py --userdata <file or device> --listen 127.0.0.1 (anything else stays the same).
"""
import argparse, hashlib, json, os, re, subprocess, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

HERE = os.path.dirname(os.path.abspath(__file__))
VERSION = 1
CONFIRM = "erase userdata"
A = None
state = {"build": "idle", "rc": None}
lines, lock = [], threading.Lock()


def userdata_dev():
    for d in sorted(os.listdir("/sys/class/block")):
        try:
            if "PARTNAME=userdata\n" in open("/sys/class/block/%s/uevent" % d).read():
                return "/dev/" + d
        except OSError:
            pass
    return None


def size_of(path):
    with open(path, "rb") as f:
        return f.seek(0, 2)


def mem_total():
    for l in open("/proc/meminfo"):
        if l.startswith("MemTotal:"):
            return int(l.split()[1]) * 1024
    return 0


def scratch_ready():
    return os.path.ismount(A.scratch)


def prepare_scratch():
    if scratch_ready():
        return
    dev = A.userdata
    os.makedirs(A.scratch, exist_ok=True)
    loop = ["-o", "loop"] if os.path.isfile(dev) else []
    label = subprocess.run(["blkid", "-s", "LABEL", "-o", "value", dev], capture_output=True, text=True).stdout.strip()
    if label != "q1scratch":  # first time: format (no metadata_csum etc.: the 4.4 kernel mounts it)
        subprocess.run(["mkfs.ext4", "-q", "-F", "-L", "q1scratch", "-O", "^orphan_file,^metadata_csum_seed,^metadata_csum",
                        dev], check=True)
    subprocess.run(["mount", *loop, dev, A.scratch], check=True)


def safe_path(rel):
    parts = [p for p in rel.split("/") if p]
    if not parts or any(p in (".", "..") for p in parts):
        return None
    return os.path.join(A.scratch, "stage", *parts)


def build(only):
    out = os.path.join(A.scratch, "userdata.simg")
    cmd = [sys.executable, os.path.join(HERE, "build_image.py"), "--stage", os.path.join(A.scratch, "stage"),
           "--scratch", os.path.join(A.scratch, "build"), "--out", out, "--size", str(size_of(A.userdata))]
    if only:
        cmd += ["--only", only]
    os.makedirs(os.path.join(A.scratch, "build"), exist_ok=True)
    with lock:
        lines.append("@agent " + json.dumps({"cmd": cmd}))
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    for l in p.stdout:
        with lock:
            lines.append(l.rstrip("\n"))
    state["rc"] = p.wait()
    state["build"] = "done" if state["rc"] == 0 else "failed"


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def reply(self, code, obj=None, headers=None):
        body = json.dumps(obj).encode() if obj is not None else b""
        self.send_response(code)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return json.loads(self.rfile.read(n) or b"{}")

    def do_GET(self):
        u = urlparse(self.path)
        if u.path == "/info":
            return self.reply(200, {"agent": VERSION, "userdata": size_of(A.userdata), "mem": mem_total(),
                                    "scratch": scratch_ready(), "build": state["build"], "rc": state["rc"]})
        if u.path == "/events":
            n = int(parse_qs(u.query).get("from", ["0"])[0])
            with lock:
                chunk = lines[n:n + 2000]
            return self.reply(200, {"next": n + len(chunk), "lines": chunk, "build": state["build"]})
        if u.path == "/image":
            img = os.path.join(A.scratch, "userdata.simg")
            if state["build"] != "done" or not os.path.exists(img):
                return self.reply(409, {"error": "no image built"})
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(os.path.getsize(img)))
            self.end_headers()
            with open(img, "rb") as f:
                for b in iter(lambda: f.read(1 << 20), b""):
                    self.wfile.write(b)
            return
        self.reply(404, {"error": "not found"})

    def do_HEAD(self):
        u = urlparse(self.path)
        p = safe_path(u.path[len("/stage/"):]) if u.path.startswith("/stage/") and scratch_ready() else None
        if not p or not os.path.exists(p) or not os.path.exists(p + ".sha256"):
            return self.reply(404)
        self.reply(200, None, {"X-Size": str(os.path.getsize(p)), "X-Sha256": open(p + ".sha256").read().strip()})

    def do_PUT(self):
        u = urlparse(self.path)
        if not u.path.startswith("/stage/") or not scratch_ready():
            return self.reply(409, {"error": "no scratch space (POST /scratch first)"})
        p = safe_path(u.path[len("/stage/"):])
        want = (self.headers.get("X-Sha256") or "").lower()
        if not p or not re.fullmatch("[0-9a-f]{64}", want):
            return self.reply(400, {"error": "bad path or missing X-Sha256"})
        n = int(self.headers["Content-Length"])
        os.makedirs(os.path.dirname(p), exist_ok=True)
        h = hashlib.sha256()
        with open(p + ".part", "wb") as f:
            while n:
                b = self.rfile.read(min(n, 1 << 20))
                if not b:
                    break
                f.write(b)
                h.update(b)
                n -= len(b)
        if n or h.hexdigest() != want:
            os.remove(p + ".part")
            return self.reply(422, {"error": "upload incomplete or sha256 mismatch"})
        os.replace(p + ".part", p)
        with open(p + ".sha256", "w") as f:
            f.write(want)
        self.reply(200, {"ok": True})

    def do_POST(self):
        u = urlparse(self.path)
        if u.path == "/scratch":
            if self.body().get("confirm") != CONFIRM:
                return self.reply(403, {"error": "confirmation text missing"})
            try:
                prepare_scratch()
            except subprocess.CalledProcessError as e:
                return self.reply(500, {"error": str(e)})
            return self.reply(200, {"scratch": True})
        if u.path == "/build":
            if not scratch_ready():
                return self.reply(409, {"error": "no scratch space"})
            if state["build"] == "running":
                return self.reply(409, {"error": "already running"})
            state["build"], state["rc"] = "running", None
            threading.Thread(target=build, args=(self.body().get("only"),), daemon=True).start()
            return self.reply(202, {"build": "running"})
        if u.path == "/reboot":
            target = self.body().get("target") or ""
            self.reply(200, {"reboot": target})
            subprocess.run(["sync"])
            if scratch_ready():
                subprocess.run(["umount", A.scratch])
            time.sleep(0.5)
            os.execv("/bin/rb", ["rb"] + ([target] if target else [])) if os.path.exists("/bin/rb") else subprocess.run(["reboot", "-f"])
            return
        self.reply(404, {"error": "not found"})


def main():
    global A
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--listen", default="192.168.77.1")
    ap.add_argument("--port", type=int, default=8077)
    ap.add_argument("--userdata", help="device or file (default: the partition named userdata)")
    ap.add_argument("--scratch", default="/scratch")
    A = ap.parse_args()
    A.userdata = A.userdata or userdata_dev()
    if not A.userdata:
        sys.exit("no userdata partition")
    ThreadingHTTPServer((A.listen, A.port), H).serve_forever()


if __name__ == "__main__":
    main()
