#!/usr/bin/env python3
"""Installer agent: runs in the micro Linux the PC app fastboot-boots on the headset (tools/build-boot.sh installer)
and lets the app download everything and build the SteamOS image there, over the USB network (http://192.168.77.1:8077). Standard library only.

Nothing is written to the headset until the app asks for the scratch space (POST /scratch with the confirmation
text): then `userdata` (wiped by the install anyway, flashed by the app at the end) is formatted and mounted as
/scratch. Every other partition is left alone.

    GET  /info                 {"agent", "userdata": bytes, "mem": bytes, "scratch": bool, "job", "state", "rc"}
    POST /scratch              body {"confirm": "erase userdata"}: format (once) and mount the scratch space
    POST /prepare              body {"only": steps}: run prepare.py here; it downloads through the tunnels the
                               app keeps open (GET /tunnel): the headset has no other way out
    GET  /tunnel               (Upgrade) a connection the app lends for one download: we send "host:port\n",
                               the app connects there and answers "OK\n", then it carries the bytes
    HEAD /stage/<path>         X-Size / X-Sha256 of a file already in the stage (development: upload instead)
    PUT  /stage/<path>         upload a file of prepare.py's stage/ (header X-Sha256 required, checked)
    POST /build                body {"only": "steps" (optional)}: run build_image.py on the stage
    GET  /events?from=N        {"next": M, "lines": [...], "state"}: the running job's output from line N (the
                               @step/@progress lines of prepare.py and build_image.py drive the progress bars)
    GET  /image                the sparse userdata image (for `fastboot flash userdata`)
    POST /reboot               body {"target": "bootloader" | ""}
For development: agent.py --userdata <file or device> --listen 127.0.0.1 (anything else stays the same).
"""
import argparse, hashlib, json, os, queue, re, socket, subprocess, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

HERE = os.path.dirname(os.path.abspath(__file__))
VERSION = 1
CONFIRM = "erase userdata"
A = None
state = {"job": None, "state": "idle", "rc": None}
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
    return os.path.join(A.scratch, "work", "stage", *parts)


def run_job(name, cmd, env=None):
    with lock:
        lines.append("@agent " + json.dumps({"job": name, "cmd": cmd}))
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                         env=dict(os.environ, **(env or {})))
    for l in p.stdout:
        with lock:
            lines.append(l.rstrip("\n"))
    state["rc"] = p.wait()
    state["state"] = "done" if state["rc"] == 0 else "failed"


def start_job(name, cmd, env=None):
    if state["state"] == "running":
        return False
    state.update(job=name, state="running", rc=None)
    threading.Thread(target=run_job, args=(name, cmd, env), daemon=True).start()
    return True


def build_cmd(only):
    w = os.path.join(A.scratch, "work")
    cmd = [sys.executable, os.path.join(HERE, "build_image.py"), "--stage", os.path.join(w, "stage"),
           "--scratch", os.path.join(A.scratch, "build"), "--out", os.path.join(A.scratch, "userdata.simg"),
           "--size", str(size_of(A.userdata))]
    os.makedirs(os.path.join(A.scratch, "build"), exist_ok=True)
    return cmd + (["--only", only] if only else [])


def prepare_cmd(only):
    cmd = [sys.executable, os.path.join(HERE, "prepare.py"), "--workdir", os.path.join(A.scratch, "work")]
    return cmd + (["--only", only] if only else [])


# --- the way out: tunnels the PC opens to us (GET /tunnel), used by a local proxy for prepare.py -------------
# The headset has no route to the internet and the PC must not have to accept connections (firewall prompts on
# the USB network), so the PC keeps a few connections open to the agent; for each connection prepare.py makes
# through the proxy, one tunnel gets "host:port\n", the PC connects there, answers "OK\n", and bytes flow.
tunnels = queue.Queue()
PROXY = ("127.0.0.1", 3128)


def splice(a, b):
    def pump(src, dst):
        try:
            for chunk in iter(lambda: src.recv(1 << 16), b""):
                dst.sendall(chunk)
        except OSError:
            pass
        for s in (src, dst):
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
    t = threading.Thread(target=pump, args=(b, a), daemon=True)
    t.start()
    pump(a, b)
    t.join()


def proxy_client(c):
    try:
        head = b""
        while b"\r\n\r\n" not in head and len(head) < 65536:
            b = c.recv(4096)
            if not b:
                return
            head += b
        line = head.split(b"\r\n", 1)[0].decode("latin-1").split()
        if line[0] == "CONNECT":
            target, rest = line[1], b""
        else:  # plain http: absolute URI, forwarded as is
            u = urlparse(line[1])
            target, rest = "%s:%d" % (u.hostname, u.port or 80), head
        with lock:
            lines.append("@tunnel " + json.dumps({"to": target}))
        try:
            t, done = tunnels.get(timeout=60)
        except queue.Empty:
            c.sendall(b"HTTP/1.1 503 No tunnel from the PC\r\n\r\n")
            return
        try:
            t.sendall(target.encode() + b"\n")
            if t.recv(3) != b"OK\n":
                c.sendall(b"HTTP/1.1 502 Unreachable\r\n\r\n")
                return
            if line[0] == "CONNECT":
                c.sendall(b"HTTP/1.1 200 Connection established\r\n\r\n")
            else:
                t.sendall(rest)
            splice(c, t)
        finally:
            done.set()
    finally:
        c.close()


def proxy_server():
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(PROXY)
    s.listen(16)
    while True:
        c, _ = s.accept()
        threading.Thread(target=proxy_client, args=(c,), daemon=True).start()


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
        if u.path == "/tunnel":  # the PC's way in for our downloads: kept until a proxy client used it
            self.send_response(101)
            self.send_header("Upgrade", "q1tunnel")
            self.send_header("Connection", "Upgrade")
            self.end_headers()
            self.wfile.flush()
            done = threading.Event()
            tunnels.put((self.connection, done))
            done.wait()
            self.close_connection = True
            return
        if u.path == "/info":
            return self.reply(200, {"agent": VERSION, "userdata": size_of(A.userdata), "mem": mem_total(),
                                    "scratch": scratch_ready(), **state})
        if u.path == "/events":
            n = int(parse_qs(u.query).get("from", ["0"])[0])
            with lock:
                chunk = lines[n:n + 2000]
            return self.reply(200, {"next": n + len(chunk), "total": len(lines), "lines": chunk, **state})
        if u.path == "/image":
            img = os.path.join(A.scratch, "userdata.simg")
            if state["job"] != "build" or state["state"] != "done" or not os.path.exists(img):
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
        if u.path in ("/build", "/prepare"):
            if not scratch_ready():
                return self.reply(409, {"error": "no scratch space"})
            b = self.body()
            if u.path == "/build":
                ok = start_job("build", build_cmd(b.get("only")))
            else:
                px = "http://%s:%d" % PROXY if b.get("tunnel", True) else None
                env = {"http_proxy": px, "https_proxy": px, "HTTP_PROXY": px, "HTTPS_PROXY": px} if px else {}
                ok = start_job("prepare", prepare_cmd(b.get("only")), env)
            if not ok:
                return self.reply(409, {"error": "a job is running"})
            return self.reply(202, {"job": u.path[1:], "state": "running"})
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
    threading.Thread(target=proxy_server, daemon=True).start()
    ThreadingHTTPServer((A.listen, A.port), H).serve_forever()


if __name__ == "__main__":
    main()
