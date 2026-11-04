#!/usr/bin/env python3
"""Installer (PC side): have the headset download everything and build the image. Talks to installer/agent.py
in the micro Linux (fastboot-booted, USB network): scratch space, prepare.py run on the headset (its downloads go
through tunnels this side opens: no incoming connection on the PC), build with progress, download of the sparse
image. The reference for the app; standard library only.

    remote_build.py --out userdata.simg [--agent http://192.168.77.1:8077] [--prepare-only STEPS] [--only STEPS]
    remote_build.py --stage DIR ...    development: upload a stage prepare.py made on the PC instead

Uploaded files already on the headset with the same sha256 are not sent again (an interrupted install resumes).
Progress for the app: the build's own "@step"/"@progress" lines are passed through, plus "@step"/"@progress"
for the upload ("upload") and the download ("download").
"""
import argparse, hashlib, http.client, json, os, socket, sys, threading, time
from urllib.parse import urlparse, quote

CONFIRM = "erase userdata"


def event(kind, **kw):
    print("@%s %s" % (kind, json.dumps(kw)), flush=True)


class Agent:
    def __init__(self, url):
        u = urlparse(url)
        self.host, self.port = u.hostname, u.port or 80

    def req(self, method, path, body=None, headers=None, stream=False, timeout=60):
        c = http.client.HTTPConnection(self.host, self.port, timeout=timeout)
        h = dict(headers or {})
        if isinstance(body, dict):
            body = json.dumps(body).encode()
            h["Content-Type"] = "application/json"
        c.request(method, path, body=body, headers=h)
        r = c.getresponse()
        if stream:
            return r
        data = r.read()
        return r.status, (json.loads(data) if data else None), r

    def ok(self, method, path, body=None, **kw):
        st, js, _ = self.req(method, path, body, **kw)
        if st >= 300:
            sys.exit("agent %s %s: %d %s" % (method, path, st, js))
        return js


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def stage_files(stage):
    """What the build reads: the repo zip (Unix modes), Holo, packages, Frame image, Steam zips, Meta's images."""
    out = []
    for root, dirs, files in os.walk(stage):
        rel = os.path.relpath(root, stage).replace(os.sep, "/")
        top = rel.split("/")[0]
        if top == "repo" and os.path.exists(os.path.join(stage, "repo.zip")):
            continue  # the unpacked copy (no Unix modes): the zip is sent
        if top == "steam" and rel != "steam/package":
            continue
        for f in files:
            if f.endswith((".part", ".sha256")) or (top == "meta" and f == "boot.img") or \
                    (top == "prebuilt" and f != "native-arm64.tar.zst"):  # the boot images stay on the PC
                continue
            out.append(f if rel == "." else rel + "/" + f)
    return sorted(out)


class Progress:
    def __init__(self, step, total):
        self.step, self.total, self.done, self.last = step, total, 0, 0

    def add(self, n):
        self.done += n
        if time.time() - self.last > 0.5 or self.done >= self.total:
            self.last = time.time()
            event("progress", step=self.step, done=self.done, total=self.total)


def upload(ag, stage):
    files = stage_files(stage)
    prog = Progress("upload", sum(os.path.getsize(os.path.join(stage, f)) for f in files))
    event("step", step="upload", status="start")
    for rel in files:
        p = os.path.join(stage, rel)
        size, h = os.path.getsize(p), sha256(p)
        st, _, r = ag.req("HEAD", "/stage/" + quote(rel))
        if st == 200 and r.getheader("X-Sha256") == h:
            prog.add(size)
            continue
        c = http.client.HTTPConnection(ag.host, ag.port, timeout=600)
        c.putrequest("PUT", "/stage/" + quote(rel))
        c.putheader("Content-Length", str(size))
        c.putheader("X-Sha256", h)
        c.endheaders()
        with open(p, "rb") as f:
            for b in iter(lambda: f.read(1 << 20), b""):
                c.send(b)
                prog.add(len(b))
        r = c.getresponse()
        if r.status != 200:
            sys.exit("upload %s: %d %s" % (rel, r.status, r.read()))
        r.read()
    event("step", step="upload", status="done")


def splice(a, b):
    def pump(src, dst):
        try:
            for chunk in iter(lambda: src.recv(1 << 16), b""):
                dst.sendall(chunk)
        except OSError:
            pass
        for x in (src, dst):
            try:
                x.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
    t = threading.Thread(target=pump, args=(b, a), daemon=True)
    t.start()
    pump(a, b)
    t.join()
    a.close()
    b.close()


def tunnel_worker(ag):
    """Lend the headset one connection at a time to the internet: only outgoing connections from the PC."""
    while True:
        try:
            s = socket.create_connection((ag.host, ag.port), timeout=30)
            s.sendall(b"GET /tunnel HTTP/1.1\r\nHost: headset\r\nUpgrade: q1tunnel\r\nConnection: Upgrade\r\n\r\n")
            buf = b""
            while b"\r\n\r\n" not in buf:
                b = s.recv(4096)
                if not b:
                    raise OSError("tunnel refused")
                buf += b
            head, buf = buf.split(b"\r\n\r\n", 1)
            if b" 101 " not in head.split(b"\r\n")[0]:
                raise OSError("tunnel refused")
            s.settimeout(None)  # idle until the headset needs it
            while b"\n" not in buf:
                b = s.recv(4096)
                if not b:
                    raise OSError("tunnel closed")
                buf += b
            target, buf = buf.split(b"\n", 1)
            host, port = target.decode().rsplit(":", 1)
            try:
                t = socket.create_connection((host.strip("[]"), int(port)), timeout=30)
                t.settimeout(None)
            except OSError:
                s.sendall(b"NO\n")
                s.close()
                continue
            s.sendall(b"OK\n")
            if buf:
                t.sendall(buf)
            threading.Thread(target=splice, args=(s, t), daemon=True).start()
        except OSError:
            time.sleep(1)


def start_tunnels(ag, n=4):
    for _ in range(n):
        threading.Thread(target=tunnel_worker, args=(ag,), daemon=True).start()


def follow(ag, path, body):
    """Start a job on the headset (/prepare, /build) and pass its output through until it ends."""
    n = ag.ok("GET", "/events?from=0")["total"]
    ag.ok("POST", path, body)
    while True:
        js = ag.ok("GET", "/events?from=%d" % n)
        for l in js["lines"]:
            print(l, flush=True)
        n = js["next"]
        if js["state"] in ("done", "failed") and not js["lines"]:
            if js["state"] == "failed":
                sys.exit("%s failed on the headset" % path)
            return
        if not js["lines"]:
            time.sleep(1)


def download(ag, out):
    r = ag.req("GET", "/image", stream=True, timeout=600)
    if r.status != 200:
        sys.exit("image: %d %s" % (r.status, r.read()))
    prog = Progress("download", int(r.getheader("Content-Length")))
    event("step", step="download", status="start")
    with open(out + ".part", "wb") as f:
        for b in iter(lambda: r.read(1 << 20), b""):
            f.write(b)
            prog.add(len(b))
    if prog.done != prog.total:
        sys.exit("image download incomplete")
    os.replace(out + ".part", out)
    event("step", step="download", status="done")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--agent", default="http://192.168.77.1:8077")
    ap.add_argument("--stage", help="development: upload this prepare.py stage (else the headset downloads)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--prepare-only", help="prepare.py steps run on the headset (default: all)")
    ap.add_argument("--only", help="build_image.py steps (default: all)")
    a = ap.parse_args()
    ag = Agent(a.agent)
    info = ag.ok("GET", "/info")
    print("agent:", info, flush=True)
    ag.ok("POST", "/scratch", {"confirm": CONFIRM}, timeout=600)
    if a.stage:
        upload(ag, a.stage)
    else:  # the app's way: the headset downloads everything itself, through tunnels the PC opens
        start_tunnels(ag)
        follow(ag, "/prepare", {"only": a.prepare_only})
    follow(ag, "/build", {"only": a.only} if a.only else {})
    download(ag, a.out)
    print("image:", a.out, os.path.getsize(a.out), "bytes", flush=True)


if __name__ == "__main__":
    main()
