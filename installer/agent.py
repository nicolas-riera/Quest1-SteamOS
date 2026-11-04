#!/usr/bin/env python3
"""Installer agent: runs in the micro Linux the PC app fastboot-boots on the headset (tools/build-boot.sh installer)
and lets the app download everything and build the SteamOS image there, over the USB network
(http://192.168.77.1:8077). Standard library only.

Nothing is ever written to the headset's storage: the scratch space (/scratch: downloads, the image being built)
is a disk on the PC, a sparse file the app serves as a network block device (NBD) through one of its tunnels.
The headset only computes; the app flashes the result at the end.

    GET  /info                 {"agent", "userdata": bytes, "mem": bytes, "scratch": bool, "job", "state", "rc"}
    POST /scratch              attach the PC's disk (NBD export "scratch" through a tunnel), format it the
                               first time (it keeps its contents between attempts), mount it on /scratch;
                               body {"time": the PC's Unix time}: the headset has no clock at boot (1970)
    POST /prepare              body {"only": steps}: run prepare.py here; it downloads through the tunnels the
                               app keeps open (GET /tunnel): the headset has no other way out
    GET  /tunnel               (Upgrade) a connection the app lends for one download: we send "host:port\n",
                               the app connects there and answers "OK\n", then it carries the bytes
    HEAD /stage/<path>         X-Size / X-Sha256 of a file already in the stage (development: upload instead)
    PUT  /stage/<path>         upload a file of prepare.py's stage/ (header X-Sha256 required, checked)
    POST /build                body {"only": "steps" (optional)}: run build_image.py on the stage
    GET  /events?from=N        {"next": M, "lines": [...], "state"}: the running job's output from line N (the
                               @step/@progress lines of prepare.py and build_image.py drive the progress bars)
    GET  /image                the sparse userdata image (for `fastboot flash userdata`), made on the fly from
                               the built file system: it is never stored on the scratch space
    POST /reboot               body {"target": "bootloader" | ""}
For development: agent.py --userdata <file> --listen 127.0.0.1 (the size of the image to build).
"""
import argparse, fcntl, hashlib, json, os, queue, re, socket, struct, subprocess, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs, unquote

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build_image  # noqa: E402 (sparse_plan / sparse_stream for GET /image)
VERSION = 1
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


# --- the scratch space: a disk on the PC (NBD) --------------------------------------------------------------
NBD_SET_SOCK, NBD_SET_BLKSIZE, NBD_DO_IT, NBD_CLEAR_SOCK, NBD_CLEAR_QUE = 0xab00, 0xab01, 0xab03, 0xab04, 0xab05
NBD_SET_SIZE_BLOCKS, NBD_DISCONNECT, NBD_SET_TIMEOUT, NBD_SET_FLAGS = 0xab07, 0xab08, 0xab09, 0xab0a
nbd = {}


def recvn(s, n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise OSError("NBD: connection closed during the handshake")
        b += c
    return b


def free_nbd():
    """The first NBD device not connected (a crashed attempt may leave one behind)."""
    for i in range(16):
        if os.path.exists("/dev/nbd%d" % i) and not os.path.exists("/sys/block/nbd%d/pid" % i):
            return "/dev/nbd%d" % i
    raise OSError("no free NBD device")


def nbd_attach(export, dev=None):
    """The PC's export through a tunnel: fixed newstyle handshake here, then the kernel's NBD client takes the
    socket (NBD_DO_IT runs in a thread for as long as the device is attached)."""
    subprocess.run(["modprobe", "nbd"], stderr=subprocess.DEVNULL)  # built in on the headset
    dev = dev or free_nbd()
    s, done = take_tunnel()
    s.sendall(b"nbd:" + export.encode() + b"\n")
    if recvn(s, 3) != b"OK\n":
        raise OSError("NBD: the PC refused the export")
    if recvn(s, 16) != b"NBDMAGICIHAVEOPT":
        raise OSError("NBD: not a newstyle server")
    recvn(s, 2)
    s.sendall(struct.pack(">I", 3))  # FIXED_NEWSTYLE | NO_ZEROES
    s.sendall(b"IHAVEOPT" + struct.pack(">II", 1, len(export)) + export.encode())  # NBD_OPT_EXPORT_NAME
    size, flags = struct.unpack(">QH", recvn(s, 10))
    fd = os.open(dev, os.O_RDWR)
    fcntl.ioctl(fd, NBD_SET_BLKSIZE, 4096)
    fcntl.ioctl(fd, NBD_SET_SIZE_BLOCKS, size // 4096)
    fcntl.ioctl(fd, NBD_SET_FLAGS, flags)
    fcntl.ioctl(fd, NBD_SET_SOCK, s.fileno())

    def serve():
        try:
            fcntl.ioctl(fd, NBD_DO_IT)
        except OSError:
            pass
        for op in (NBD_CLEAR_QUE, NBD_CLEAR_SOCK):
            try:
                fcntl.ioctl(fd, op)
            except OSError:
                pass
        with lock:
            lines.append("@agent " + json.dumps({"nbd": export, "detached": True}))
        nbd.pop(export, None)
        done.set()
    nbd[export] = (dev, fd, s)
    threading.Thread(target=serve, daemon=True).start()
    for _ in range(50):  # the kernel reads the partition table first
        if os.path.exists("/sys/block/%s/size" % os.path.basename(dev)) and \
                int(open("/sys/block/%s/size" % os.path.basename(dev)).read()) > 0:
            break
        time.sleep(0.1)
    return dev


def prepare_scratch():
    if scratch_ready():
        return
    dev = nbd_attach("scratch")
    os.makedirs(A.scratch, exist_ok=True)
    label = subprocess.run(["blkid", "-s", "LABEL", "-o", "value", dev], capture_output=True, text=True).stdout.strip()
    if label != "q1scratch":  # a new disk: format it (no metadata_csum etc.: the 4.4 kernel mounts it)
        subprocess.run(["mkfs.ext4", "-q", "-F", "-L", "q1scratch", "-O", "^orphan_file,^metadata_csum_seed,^metadata_csum",
                        dev], check=True)
    # discard: what the build deletes is given back on the PC (the sparse file shrinks)
    subprocess.run(["mount", "-o", "discard,noatime", dev, A.scratch], check=True)


def safe_path(rel, base="stage"):
    """A stage file from an upload URL (percent-decoded: package names have ':' and '+'); the uploads' sha256
    sidecars live apart (base "sums") so that nothing but the files themselves is in the stage."""
    parts = [p for p in unquote(rel).split("/") if p]
    if not parts or any(p in (".", "..") for p in parts):
        return None
    return os.path.join(A.scratch, "work", base, *parts)


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
    # every step but "sparse": GET /image makes the sparse image on the fly; inputs deleted once used
    steps = only or ",".join(s for s in build_image.STEPS if s != "sparse")
    cmd = [sys.executable, os.path.join(HERE, "build_image.py"), "--stage", os.path.join(w, "stage"),
           "--scratch", os.path.join(A.scratch, "build"), "--out", os.path.join(A.scratch, "userdata.simg"),
           "--size", str(size_of(A.userdata)), "--consume", "--only", steps]
    os.makedirs(os.path.join(A.scratch, "build"), exist_ok=True)
    return cmd


def prepare_cmd(only):
    cmd = [sys.executable, os.path.join(HERE, "prepare.py"), "--workdir", os.path.join(A.scratch, "work"), "--move"]
    return cmd + (["--only", only] if only else [])


# --- the way out: tunnels the PC opens to us (GET /tunnel), used by a local proxy for prepare.py -------------
# The headset has no route to the internet and the PC must not have to accept connections (firewall prompts on
# the USB network), so the PC keeps a few connections open to the agent; for each connection prepare.py makes
# through the proxy, one tunnel gets "host:port\n", the PC connects there, answers "OK\n", and bytes flow.
tunnels = queue.Queue()
PROXY = ("127.0.0.1", 3128)


def take_tunnel(timeout=60):
    """A tunnel that is still open: those whose PC side went away (the app restarted, a run was killed) are
    dropped here instead of failing the request that would use them. queue.Empty when none comes."""
    end = time.time() + timeout
    while True:
        t, done = tunnels.get(timeout=max(0.1, end - time.time()))
        try:
            alive = t.recv(1, socket.MSG_PEEK | socket.MSG_DONTWAIT) != b""  # the PC never speaks first
        except BlockingIOError:
            alive = True
        except OSError:
            alive = False
        if alive:
            return t, done
        done.set()


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
            t, done = take_tunnel()
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
            raw = os.path.join(A.scratch, "build", "userdata.raw")
            if state["job"] != "build" or state["state"] != "done" or not os.path.exists(raw):
                return self.reply(409, {"error": "no image built"})
            chunks, length, _ = build_image.sparse_plan(raw)
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(length))
            self.end_headers()
            build_image.sparse_stream(raw, chunks, self.wfile.write)
            return
        self.reply(404, {"error": "not found"})

    def do_HEAD(self):
        u = urlparse(self.path)
        rel = u.path[len("/stage/"):]
        p = safe_path(rel) if u.path.startswith("/stage/") and scratch_ready() else None
        if not p or not os.path.exists(p) or not os.path.exists(safe_path(rel, "sums")):
            return self.reply(404)
        self.reply(200, None, {"X-Size": str(os.path.getsize(p)), "X-Sha256": open(safe_path(rel, "sums")).read().strip()})

    def do_PUT(self):
        u = urlparse(self.path)
        if not u.path.startswith("/stage/") or not scratch_ready():
            return self.reply(409, {"error": "no scratch space (POST /scratch first)"})
        p, sums = safe_path(u.path[len("/stage/"):]), safe_path(u.path[len("/stage/"):], "sums")
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
        os.makedirs(os.path.dirname(sums), exist_ok=True)
        with open(sums, "w") as f:
            f.write(want)
        self.reply(200, {"ok": True})

    def do_POST(self):
        u = urlparse(self.path)
        if u.path == "/scratch":
            t = self.body().get("time")
            if t and abs(time.time() - t) > 60:  # else the files the build writes are dated 1970
                time.clock_settime(time.CLOCK_REALTIME, t)
            try:
                prepare_scratch()
            except (OSError, queue.Empty, subprocess.CalledProcessError) as e:
                return self.reply(500, {"error": "scratch space: %s" % e})
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
            for dev, fd, _ in list(nbd.values()):
                try:
                    fcntl.ioctl(fd, NBD_DISCONNECT)
                except OSError:
                    pass
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
    A.userdata = A.userdata or userdata_dev()  # only its size is read
    if not A.userdata:
        sys.exit("no userdata partition")
    threading.Thread(target=proxy_server, daemon=True).start()
    ThreadingHTTPServer((A.listen, A.port), H).serve_forever()


if __name__ == "__main__":
    main()
