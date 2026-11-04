#!/usr/bin/env python3
"""Installer step 1: fetch and verify everything the SteamOS image is built from, into a work folder.

    prepare.py [--workdir DIR] [--only repo,holo,steam,gtk2,meta] [--cleanup]

Work folder (default %TEMP%/quest1-steamos, or $TMPDIR): dl/ holds the downloads (kept between runs, re-used when
their hash still matches), stage/ what the image build takes:
    stage/repo/       this repository's source tree (sources.json "repo")
    stage/holo/       system.rootfs.zst, SteamOS Holo core aarch64 (Valve)
    stage/steam/      the arm64 Steam client, unpacked (Valve's client CDN), package/ keeps the zips + manifest
    stage/gtk2/       GTK 2 libraries for steamui.so (Debian arm64)
    stage/meta/       system.img and modem.img from Meta's OTA (tracking stack, GPU driver, firmware), boot.img (restore)
    stage/report.json versions and hashes of all of the above
Standard library only (the PC app embeds Python or ports it). --cleanup deletes the work folder (after flashing).
Nothing here is ever shipped: every file comes from its owner's server on the user's machine.
"""
import argparse, hashlib, importlib.util, io, json, os, re, shutil, sys, tarfile, tempfile, time, urllib.request, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
UA = {"User-Agent": "Quest1-SteamOS-installer"}


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def fetch(url, dest, want=None):
    """Download url to dest unless dest already has the wanted hash; returns the file's sha256."""
    if os.path.exists(dest) and want and sha256(dest) == want:
        log("cached", os.path.basename(dest))
        return want
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    tmp = dest + ".part"
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=120) as r, open(tmp, "wb") as f:
        total, done, last = int(r.headers.get("Content-Length") or 0), 0, 0
        for b in iter(lambda: r.read(1 << 20), b""):
            f.write(b)
            done += len(b)
            if total and time.time() - last > 5:
                last = time.time()
                log("  %s %d%%" % (os.path.basename(dest), done * 100 // total))
    got = sha256(tmp)
    if want and got != want:
        os.remove(tmp)
        sys.exit("%s: sha256 %s, expected %s" % (url, got, want))
    os.replace(tmp, dest)
    log("downloaded", os.path.basename(dest), "" if want else "(unpinned sha256 %s)" % got)
    return got


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


def step_repo(src, dl, stage, rep):
    url = src["zip_url"].format(ref=src["ref"])
    z = os.path.join(dl, "repo.zip")
    if os.path.exists(z):
        os.remove(z)  # a branch moves: always fetch
    rep["repo"] = {"ref": src["ref"], "sha256": fetch(url, z)}
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
    log("repo unpacked:", len(os.listdir(out)), "top-level entries")


def step_holo(src, dl, stage, rep):
    d = os.path.join(dl, "system.rootfs.zst")
    rep["holo"] = {"url": src["url"], "sha256": fetch(src["url"], d, src.get("sha256"))}
    os.makedirs(os.path.join(stage, "holo"), exist_ok=True)
    dst = os.path.join(stage, "holo", "system.rootfs.zst")
    if not os.path.exists(dst):
        shutil.copyfile(d, dst)


def step_steam(src, dl, stage, rep):
    ch = src["channel"]
    with urllib.request.urlopen(urllib.request.Request(src["cdn"] + ch, headers=UA), timeout=60) as r:
        text = r.read().decode()
    man = vdf(text)
    plat = next(iter(man.values()))
    pkgs = {k: v for k, v in plat.items() if isinstance(v, dict) and "file" in v}
    total = sum(int(v["size"]) for v in pkgs.values())
    log("Steam client %s version %s: %d packages, %.0f MB" % (ch, plat.get("version"), len(pkgs), total / 1e6))
    out = os.path.join(stage, "steam")
    pkgdir = os.path.join(out, "package")
    os.makedirs(pkgdir, exist_ok=True)
    for name, p in sorted(pkgs.items()):
        z = os.path.join(dl, "steam", p["file"])
        fetch(src["cdn"] + p["file"], z, p["sha2"])
        with zipfile.ZipFile(z) as f:
            f.extractall(out)
        shutil.copyfile(z, os.path.join(pkgdir, p["file"]))
    with open(os.path.join(pkgdir, ch + ".manifest"), "w", newline="\n") as f:
        f.write(text)
    rep["steam"] = {"channel": ch, "version": plat.get("version"), "packages": len(pkgs)}


def deb_data(path):
    """The data.tar.* member of a .deb (ar archive)."""
    with open(path, "rb") as f:
        assert f.read(8) == b"!<arch>\n"
        while True:
            h = f.read(60)
            if len(h) < 60:
                break
            name, size = h[:16].decode().strip().rstrip("/"), int(h[48:58])
            data = f.read(size)
            f.read(size & 1)
            if name.startswith("data.tar"):
                return tarfile.open(fileobj=io.BytesIO(data), mode="r:*")
    sys.exit(path + ": no data.tar")


def step_gtk2(src, dl, stage, rep):
    d = os.path.join(dl, os.path.basename(src["url"]))
    rep["gtk2"] = {"url": src["url"], "sha256": fetch(src["url"], d, src.get("sha256"))}
    out = os.path.join(stage, "gtk2")
    os.makedirs(out, exist_ok=True)
    n = 0
    with deb_data(d) as t:
        for m in t.getmembers():
            base = os.path.basename(m.name)
            if not any(base.startswith(p) for p in src["members"]):
                continue
            if m.issym():
                with open(os.path.join(out, base + ".symlink"), "w") as f:
                    f.write(m.linkname)  # links are made in the image (Windows cannot be trusted with them)
            elif m.isfile():
                with t.extractfile(m) as a, open(os.path.join(out, base), "wb") as b:
                    shutil.copyfileobj(a, b)
            n += 1
    log("gtk2:", n, "files")


def step_meta(src, dl, stage, rep):
    z = os.path.join(dl, os.path.basename(src["url"]))
    fetch(src["url"], z, src["sha256"])
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


STEPS = [("repo", step_repo, "repo"), ("holo", step_holo, "holo_rootfs"), ("steam", step_steam, "steam_client"),
         ("gtk2", step_gtk2, "gtk2"), ("meta", step_meta, "meta_ota")]


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
    os.makedirs(stage, exist_ok=True)
    rp = os.path.join(stage, "report.json")
    rep = json.load(open(rp)) if os.path.exists(rp) else {}
    only = set(a.only.split(",")) if a.only else None
    for name, fn, key in STEPS:
        if only and name not in only:
            continue
        log("==", name)
        fn(src[key], dl, stage, rep)
        with open(rp, "w") as f:
            json.dump(rep, f, indent=2)
    log("SteamVR: not fetched (no public arm64 download yet, see sources.json)")
    log("done:", stage)


if __name__ == "__main__":
    main()
