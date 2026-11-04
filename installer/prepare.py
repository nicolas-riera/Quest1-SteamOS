#!/usr/bin/env python3
"""Installer step 1 (PC): fetch and verify everything the SteamOS image is built from, into a work folder.

    prepare.py [--workdir DIR] [--only repo,holo,steam,frame,meta] [--cleanup]

Work folder (default %TEMP%/quest1-steamos): dl/ holds the downloads (kept between runs, re-used when their hash
still matches), stage/ what the image build (build_image.py, on the headset) takes:
    stage/repo/       this repository's source tree (sources.json "repo"), repo.zip the same with Unix modes
    stage/holo/       system.rootfs.zst, SteamOS Holo core aarch64 (Valve)
    stage/steam/      the arm64 Steam client: package/ holds Valve's zips + manifest (unpacked by the build)
    stage/frame/      the Steam Frame's recovery image (.img.bz2, Valve): SteamVR and GTK 2, extracted by the build
    stage/meta/       system.img and modem.img from Meta's OTA (tracking stack, GPU driver, firmware), boot.img (restore)
    stage/report.json versions and hashes of all of the above
Progress for the app: lines "@step {json}" (a step starts/ends) and "@progress {json}" (bytes done/total of the
step), at most twice a second; other lines are a human log. Before anything, the free space of the work folder's
drive is checked against what is still to download, to stage and to build (the image comes back to the PC).
Standard library only. --cleanup deletes the work folder (after flashing).
Nothing here is ever shipped: every file comes from its owner's server on the user's machine.
"""
import argparse, hashlib, importlib.util, json, os, re, shutil, sys, tempfile, time, urllib.request, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
UA = {"User-Agent": "Quest1-SteamOS-installer"}
GB = 1 << 30
IMAGE_BYTES = 12 * GB    # the sparse userdata image that comes back from the headset (~8-10 GB)
MARGIN = 1.15            # +15 % on top of the estimate


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def event(kind, **kw):
    print("@%s %s" % (kind, json.dumps(kw)), flush=True)


class Progress:
    """@progress lines for the current step, throttled."""

    def __init__(self, step, total):
        self.step, self.total, self.done, self.last = step, total, 0, 0

    def add(self, n):
        self.done += n
        if time.time() - self.last > 0.5 or self.done >= self.total:
            self.last = time.time()
            event("progress", step=self.step, done=self.done, total=self.total)


def sha256(path, prog=None):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
            if prog:
                prog.add(len(b))
    return h.hexdigest()


def fetch(url, dest, want=None, prog=None, size=None):
    """Download url to dest unless dest already has the wanted hash; returns the file's sha256."""
    if os.path.exists(dest) and want and sha256(dest) == want:
        log("cached", os.path.basename(dest))
        if prog:
            prog.add(size or os.path.getsize(dest))
        return want
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    tmp, h = dest + ".part", hashlib.sha256()
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=120) as r, open(tmp, "wb") as f:
        for b in iter(lambda: r.read(1 << 20), b""):
            f.write(b)
            h.update(b)
            if prog:
                prog.add(len(b))
    got = h.hexdigest()
    if want and got != want:
        os.remove(tmp)
        sys.exit("%s: sha256 %s, expected %s" % (url, got, want))
    os.replace(tmp, dest)
    log("downloaded", os.path.basename(dest), "" if want else "(unpinned sha256 %s)" % got)
    return got


def place(src, dst):
    """Put a downloaded file into stage/ without a second copy (hard link, else copy)."""
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.exists(dst):
        os.remove(dst)
    try:
        os.link(src, dst)
    except OSError:
        shutil.copyfile(src, dst)


def head_size(url):
    req = urllib.request.Request(url, method="HEAD", headers=UA)
    with urllib.request.urlopen(req, timeout=60) as r:
        return int(r.headers.get("Content-Length") or 0)


def vdf(text):
    """Valve KeyValues text (the client manifest) -> nested dicts."""
    toks = re.findall(r'"((?:[^"\\]|\\.)*)"|([{}])', text)
    stack, cur, key = [], {}, None
    for s, brace in toks:
        if brace == "{":
            new = {}
            cur[key] = new
            stack.append(cur)
            cur, key = new, None
        elif brace == "}":
            cur = stack.pop()
        elif key is None:
            key = s
        else:
            cur[key], key = s, None
    return cur


def steam_manifest(src):
    with urllib.request.urlopen(urllib.request.Request(src["cdn"] + src["channel"], headers=UA), timeout=60) as r:
        text = r.read().decode()
    plat = next(iter(vdf(text).values()))
    return text, plat, {k: v for k, v in plat.items() if isinstance(v, dict) and "file" in v}


# --- steps: each returns nothing, fills rep[...] -------------------------------------------------------------

def step_repo(src, dl, stage, rep, prog):
    url = src["zip_url"].format(ref=src["ref"])
    z = os.path.join(dl, "repo.zip")
    if os.path.exists(z):
        os.remove(z)  # a branch moves: always fetch
    rep["repo"] = {"ref": src["ref"], "sha256": fetch(url, z, prog=prog)}
    place(z, os.path.join(stage, "repo.zip"))  # the build unpacks it again: Windows loses the Unix modes
    out = os.path.join(stage, "repo")
    shutil.rmtree(out, ignore_errors=True)
    with zipfile.ZipFile(z) as f:
        root = f.namelist()[0].split("/")[0] + "/"
        for m in f.infolist():
            rel = m.filename[len(root):]
            if rel and not m.is_dir():
                p = os.path.join(out, rel)
                os.makedirs(os.path.dirname(p), exist_ok=True)
                with f.open(m) as a, open(p, "wb") as b:
                    shutil.copyfileobj(a, b)


def step_file(name):
    """holo / frame: one pinned file, placed in stage/<name>/."""
    def run(src, dl, stage, rep, prog):
        url = src["url"]
        d = os.path.join(dl, os.path.basename(url))
        rep[name] = {"url": url, "sha256": fetch(url, d, src.get("sha256"), prog)}
        place(d, os.path.join(stage, name, os.path.basename(url)))
    return run


def step_steam(src, dl, stage, rep, prog):
    text, plat, pkgs = steam_manifest(src)
    prog.total = sum(int(v["size"]) for v in pkgs.values())
    log("Steam client %s version %s: %d packages, %.0f MB" % (src["channel"], plat.get("version"), len(pkgs), prog.total / 1e6))
    pkgdir = os.path.join(stage, "steam", "package")
    os.makedirs(pkgdir, exist_ok=True)
    for name, p in sorted(pkgs.items()):
        z = os.path.join(dl, "steam", p["file"])
        fetch(src["cdn"] + p["file"], z, p["sha2"], prog, int(p["size"]))
        place(z, os.path.join(pkgdir, p["file"]))
    with open(os.path.join(pkgdir, src["channel"] + ".manifest"), "w", newline="\n") as f:
        f.write(text)
    rep["steam"] = {"channel": src["channel"], "version": plat.get("version"), "packages": len(pkgs)}


def step_meta(src, dl, stage, rep, prog):
    z = os.path.join(dl, os.path.basename(src["url"]))
    fetch(src["url"], z, src["sha256"], prog)
    with zipfile.ZipFile(z) as f:
        meta = f.read("META-INF/com/android/metadata").decode()
    post = dict(l.split("=", 1) for l in meta.splitlines() if "=" in l).get("post-build")
    if post != src["post_build"]:
        sys.exit("Meta OTA: build %s, expected %s" % (post, src["post_build"]))
    out = os.path.join(stage, "meta")
    os.makedirs(out, exist_ok=True)
    pe_path = os.path.join(stage, "repo", "tools", "payload_extract.py")
    if not os.path.exists(pe_path):
        pe_path = os.path.join(HERE, "..", "tools", "payload_extract.py")
    spec = importlib.util.spec_from_file_location("payload_extract", pe_path)
    pe = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(pe)
    argv = sys.argv
    try:
        sys.argv = ["payload_extract.py", z, out] + src["partitions"]
        pe.main()
    finally:
        sys.argv = argv
    rep["meta"] = {"build": post, "sha256": src["sha256"],
                   "images": {p: sha256(os.path.join(out, p + ".img")) for p in src["partitions"]}}


# name, function, sources.json key, download size estimate (bytes) or None (HEAD), staged bytes beyond the download
STEPS = [("repo", step_repo, "repo", 8 << 20, 16 << 20),
         ("holo", step_file("holo"), "holo_rootfs", None, 0),
         ("steam", step_steam, "steam_client", None, 0),
         ("frame", step_file("frame"), "steam_frame", None, 0),
         ("meta", step_meta, "meta_ota", None, int(2.8 * GB))]


def check_space(src, workdir, steps, dl):
    """Free space on the work folder's drive >= what is left to download + stage + the image, with a margin."""
    need = IMAGE_BYTES
    for name, _, key, est, staged in steps:
        s = src[key]
        if name == "steam":
            size = sum(int(v["size"]) for v in steam_manifest(s)[2].values())
        elif est is not None:
            size = est
        else:
            size = head_size(s["url"])
        d = os.path.join(dl, os.path.basename(s.get("url", "x")))
        if not os.path.exists(d):
            need += size
        need += staged
    need = int(need * MARGIN)
    os.makedirs(workdir, exist_ok=True)
    free = shutil.disk_usage(workdir).free
    event("space", need=need, free=free)
    log("disk space: %.1f GB needed (with margin), %.1f GB free" % (need / GB, free / GB))
    if free < need:
        sys.exit("not enough free space on the drive of %s: %.1f GB needed, %.1f GB free" % (workdir, need / GB, free / GB))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--workdir", default=os.path.join(tempfile.gettempdir(), "quest1-steamos"))
    ap.add_argument("--only", help="comma-separated steps: " + ",".join(s[0] for s in STEPS))
    ap.add_argument("--cleanup", action="store_true", help="delete the work folder and exit")
    a = ap.parse_args()
    if a.cleanup:
        shutil.rmtree(a.workdir, ignore_errors=True)
        log("removed", a.workdir)
        return
    src = json.load(open(os.path.join(HERE, "sources.json")))
    dl, stage = os.path.join(a.workdir, "dl"), os.path.join(a.workdir, "stage")
    only = set(a.only.split(",")) if a.only else None
    steps = [s for s in STEPS if not only or s[0] in only]
    check_space(src, a.workdir, steps, dl)
    os.makedirs(stage, exist_ok=True)
    rp = os.path.join(stage, "report.json")
    rep = json.load(open(rp)) if os.path.exists(rp) else {}
    for name, fn, key, est, _ in steps:
        s = src[key]
        total = est or (head_size(s["url"]) if "url" in s else 0)
        event("step", step=name, status="start")
        log("==", name)
        fn(s, dl, stage, rep, Progress(name, total))
        event("step", step=name, status="done")
        with open(rp, "w") as f:
            json.dump(rep, f, indent=2)
    log("done:", stage)


if __name__ == "__main__":
    main()
