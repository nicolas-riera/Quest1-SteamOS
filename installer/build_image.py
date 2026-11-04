#!/usr/bin/env python3
"""Installer step 2 (Linux, root): build the SteamOS root file system for the Quest 1's userdata partition.

    build_image.py --stage DIR --out userdata.simg [--scratch DIR] [--size BYTES] [--only STEPS]

Runs on the headset, in the micro Linux the app fastboot-boots (the app streams prepare.py's stage/ to it and
copies the result back); for development it runs the same way in WSL 2 (arm64 chroot steps through qemu-user).
The file system has the size of the userdata partition and is written out as an Android sparse image (only
the used blocks), which `fastboot flash userdata` takes as is.
Progress for the app: "@step {json}" / "@progress {json}" lines, as in prepare.py.

Steps:
    format  ext4 the size of userdata, no metadata_csum (kernel 4.4)
    holo    SteamOS Holo core aarch64 (tar.zst, with xattrs: file capabilities)
    setup   our base setup in an arm64 chroot (device/native-setup.sh)
    frame   SteamVR (/opt/steamvr) and GTK 2 from the Steam Frame's recovery image
    steam   the Steam client into ~steamos/.local/share/Steam
    meta    Meta's images (/usr/lib/quest1/meta, loop-mounted by device/holo-android-blobs.sh)
    ours    this repository (/usr/lib/quest1/repo) and the version file
    finish  unmount, e2fsck
    sparse  Android sparse image
"""
import argparse, bz2, json, os, shutil, stat, struct, subprocess, sys, tempfile, time, zipfile

USERDATA_BYTES = 56092471296  # Quest 1 userdata partition (GPT of the 64 GB model)
GB = 1 << 30


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def event(kind, **kw):
    print("@%s %s" % (kind, json.dumps(kw)), flush=True)


class Progress:
    def __init__(self, step, total):
        self.step, self.total, self.done, self.last = step, max(total, 1), 0, 0

    def add(self, n):
        self.done += n
        if time.time() - self.last > 0.5 or self.done >= self.total:
            self.last = time.time()
            event("progress", step=self.step, done=self.done, total=self.total)


def run(*cmd, **kw):
    log("$", " ".join(cmd))
    subprocess.run(cmd, check=True, **kw)


def du(path):
    n = 0
    for root, _, files in os.walk(path):
        for f in files:
            try:
                n += os.lstat(os.path.join(root, f)).st_size
            except OSError:
                pass
    return n


def feed(src_path, cmd, prog, decompress=None):
    """Stream a file into a command's stdin (progress = bytes of the file read)."""
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    with open(src_path, "rb") as f:
        for b in iter(lambda: f.read(4 << 20), b""):
            prog.add(len(b))
            p.stdin.write(decompress.decompress(b) if decompress else b)
    p.stdin.close()
    if p.wait():
        sys.exit("%s failed (%d)" % (cmd[0], p.returncode))


def copytree(src, dst, prog, uid=None):
    for root, dirs, files in os.walk(src):
        rel = os.path.relpath(root, src)
        d = os.path.join(dst, rel) if rel != "." else dst
        os.makedirs(d, exist_ok=True)
        for f in files:
            s = os.path.join(root, f)
            shutil.copyfile(s, os.path.join(d, f), follow_symlinks=False)
            if not os.path.islink(s):
                os.chmod(os.path.join(d, f), os.stat(s).st_mode & 0o777)
            prog.add(os.lstat(s).st_size)
    if uid is not None:
        run("chown", "-R", "%d:%d" % (uid, uid), dst)


def unzip(path, dst, strip=0):
    """Extract a zip keeping the Unix modes and symlinks it stores. Entries made on Windows (Valve's client
    packages: everything 666) get 755 when they are ELF files or scripts, 644 otherwise, as the client does.
    strip: leading path components to drop (GitHub's zip has one top folder)."""
    with zipfile.ZipFile(path) as f:
        for m in f.infolist():
            rel = "/".join(m.filename.split("/")[strip:])
            if not rel:
                continue
            out = os.path.join(dst, rel)
            mode = m.external_attr >> 16 if m.create_system == 3 else 0
            if m.is_dir():
                os.makedirs(out, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(out), exist_ok=True)
            if os.path.lexists(out):
                os.remove(out)
            if stat.S_ISLNK(mode):
                os.symlink(f.read(m).decode(), out)
                continue
            with f.open(m) as a, open(out, "wb") as b:
                shutil.copyfileobj(a, b, 1 << 20)
            if not mode & 0o777:
                with open(out, "rb") as b:
                    magic = b.read(4)
                mode = 0o755 if magic == b"\x7fELF" or magic[:2] == b"#!" else 0o644
            os.chmod(out, mode & 0o777)


class Build:
    def __init__(self, a):
        self.a, self.stage = a, a.stage
        self.raw = os.path.join(a.scratch, "userdata.raw")
        self.mnt = os.path.join(a.scratch, "root")
        self.report = json.load(open(os.path.join(a.stage, "report.json")))
        self._repo = None

    @property
    def repo(self):
        """Our source tree with its Unix modes: unpacked here from GitHub's zip (stage/repo.zip), since the
        copy prepare.py unpacked on Windows lost them; stage/repo when there is no zip."""
        if self._repo is None:
            z = os.path.join(self.stage, "repo.zip")
            self._repo = os.path.join(self.stage, "repo")
            if os.path.exists(z):
                self._repo = os.path.join(self.a.scratch, "repo")
                shutil.rmtree(self._repo, ignore_errors=True)
                unzip(z, self._repo, strip=1)
        return self._repo

    def mounted(self):
        return os.path.ismount(self.mnt)

    def mount(self):
        if not self.mounted():
            os.makedirs(self.mnt, exist_ok=True)
            run("mount", "-o", "loop", self.raw, self.mnt)

    # --- steps ---------------------------------------------------------------------------------------------

    def format(self, prog):
        if self.mounted():
            run("umount", self.mnt)
        if os.path.exists(self.raw):
            os.remove(self.raw)
        with open(self.raw, "wb") as f:
            f.truncate(self.a.size)
        run("mkfs.ext4", "-q", "-F", "-L", "steamos", "-O", "^orphan_file,^metadata_csum_seed,^metadata_csum", self.raw)
        prog.add(prog.total)
        self.mount()

    def holo(self, prog):
        z = os.path.join(self.stage, "holo", "system.rootfs.zst")
        prog.total = os.path.getsize(z)
        p = subprocess.Popen(["tar", "-xp", "--xattrs", "--xattrs-include=*", "--numeric-owner", "-C", self.mnt],
                             stdin=subprocess.PIPE)
        z1 =subprocess.Popen(["zstd", "-dc", "-"], stdin=subprocess.PIPE, stdout=p.stdin)
        with open(z, "rb") as f:
            for b in iter(lambda: f.read(4 << 20), b""):
                z1.stdin.write(b)
                prog.add(len(b))
        z1.stdin.close()
        p.stdin.close()
        if z1.wait() or p.wait():
            sys.exit("unpacking Holo failed")

    def chroot(self, *cmd):
        m = self.mnt
        binds = [("proc", "proc"), ("sysfs", "sys"), ("devtmpfs", "dev"), ("devpts", "dev/pts")]
        done = []
        try:
            for fs, d in binds:
                os.makedirs(os.path.join(m, d), exist_ok=True)
                run("mount", "-t", fs, fs, os.path.join(m, d))
                done.append(os.path.join(m, d))
            run("chroot", m, *cmd)
        finally:
            for d in reversed(done):
                subprocess.run(["umount", "-l", d])

    def setup(self, prog):
        repo = self.repo
        tmp = os.path.join(self.mnt, "root", "q1setup")
        shutil.copytree(os.path.join(repo, "device"), tmp, dirs_exist_ok=True)
        self.chroot("/bin/sh", "-e", "/root/q1setup/native-setup.sh")
        shutil.rmtree(tmp)
        prog.add(prog.total)

    def frame(self, prog):
        """The Frame image is a GPT disk (esp, efi-A, rootfs-A (btrfs), var-A, home); only rootfs-A is kept, cut
        out of the bz2 stream. SteamVR and GTK 2 are copied from it; mounted read-only, or read with
        `btrfs restore` when the kernel cannot mount it."""
        d = os.path.join(self.stage, "frame")
        bz = os.path.join(d, [f for f in os.listdir(d) if f.endswith(".img.bz2")][0])
        prog.total = os.path.getsize(bz)
        part = os.path.join(self.a.scratch, "frame-rootfs.img")
        dec, head, pos, rng = bz2.BZ2Decompressor(), b"", 0, None
        with open(bz, "rb") as f, open(part, "wb") as o:
            for b in iter(lambda: f.read(1 << 20), b""):
                prog.add(len(b))
                out = dec.decompress(b)
                if rng is None:
                    head += out
                    if len(head) < 34 * 512:
                        continue
                    rng = gpt_find(head, "rootfs-A")
                    out, pos = head, 0
                s, e = max(rng[0], pos), min(rng[1], pos + len(out))
                if s < e:
                    o.write(out[s - pos:e - pos])
                pos += len(out)
                if pos >= rng[1]:
                    break
        m = os.path.join(self.a.scratch, "frame")
        os.makedirs(m, exist_ok=True)
        local = os.path.join(self.mnt, "usr", "local", "lib")
        os.makedirs(local, exist_ok=True)
        gtk = ("libgtk-x11-2.0.so", "libgdk-x11-2.0.so", "libgailutil.so")
        if subprocess.run(["mount", "-o", "loop,ro", part, m]).returncode == 0:
            try:
                run("cp", "-a", os.path.join(m, "opt", "steamvr"), os.path.join(self.mnt, "opt") + "/")
                for f in os.listdir(os.path.join(m, "usr", "lib")):
                    if f.startswith(gtk):
                        run("cp", "-a", os.path.join(m, "usr", "lib", f), local + "/")
            finally:
                run("umount", m)
        else:
            run("btrfs", "restore", "-i", "--path-regex", r"^/(|opt(|/steamvr(|/.*)))$", part, self.mnt)
            run("btrfs", "restore", "-i", "--path-regex", r"^/(|usr(|/lib(|/lib(gtk|gdk)-x11-2\.0\.so.*|/libgailutil\.so.*)))$",
                part, os.path.join(self.a.scratch, "frame-usr"))
            for f in os.listdir(os.path.join(self.a.scratch, "frame-usr", "usr", "lib")):
                run("cp", "-a", os.path.join(self.a.scratch, "frame-usr", "usr", "lib", f), local + "/")
        os.remove(part)
        # our SteamVR edits and steamvr-q1 (device/holo-steamvr.sh), from outside (Holo core has no python3)
        run("sh", os.path.join(self.repo, "device", "holo-steamvr.sh"), "install",
            env=dict(os.environ, ROOT=self.mnt))

    def steam(self, prog):
        """Valve's client packages (zips + manifest, as prepare.py staged them) unpacked into ~steamos: a few big
        files to read instead of thousands of small ones. package/ is kept, the client checks it on start."""
        pkg = os.path.join(self.stage, "steam", "package")
        zips = sorted(f for f in os.listdir(pkg) if ".zip" in f)  # name.zip.<sha1>, as the client keeps them
        prog.total = sum(os.path.getsize(os.path.join(pkg, z)) for z in zips)
        home = os.path.join(self.mnt, "home", "steamos")
        dst = os.path.join(home, ".local", "share", "Steam")
        os.makedirs(os.path.join(dst, "package"), exist_ok=True)
        for z in zips:
            unzip(os.path.join(pkg, z), dst)
            shutil.copyfile(os.path.join(pkg, z), os.path.join(dst, "package", z))
            prog.add(os.path.getsize(os.path.join(pkg, z)))
        for f in os.listdir(pkg):
            if f.endswith(".manifest"):
                shutil.copyfile(os.path.join(pkg, f), os.path.join(dst, "package", f))
        run("chown", "-R", "1000:1000", home)

    def meta(self, prog):
        d = os.path.join(self.mnt, "usr", "lib", "quest1", "meta")
        os.makedirs(d, exist_ok=True)
        imgs = ["system.img", "modem.img"]
        prog.total = sum(os.path.getsize(os.path.join(self.stage, "meta", i)) for i in imgs)
        for i in imgs:
            shutil.copyfile(os.path.join(self.stage, "meta", i), os.path.join(d, i))
            prog.add(os.path.getsize(os.path.join(d, i)))

    def ours(self, prog):
        src = self.repo
        prog.total = du(src)
        dst = os.path.join(self.mnt, "usr", "lib", "quest1", "repo")
        shutil.rmtree(dst, ignore_errors=True)
        copytree(src, dst, prog)
        with open(os.path.join(self.mnt, "usr", "lib", "quest1", "version"), "w") as f:
            json.dump({"repo": self.report.get("repo"), "built": time.strftime("%Y-%m-%dT%H:%M:%S")}, f)

    def finish(self, prog):
        if self.mounted():
            run("umount", self.mnt)
        r = subprocess.run(["e2fsck", "-fy", self.raw])
        if r.returncode > 1:
            sys.exit("e2fsck: %d" % r.returncode)
        prog.add(prog.total)

    def sparse(self, prog):
        write_sparse(self.raw, self.a.out, prog)


def gpt_find(head, name):
    """(start, end) byte range of the GPT partition called name, from the first bytes of a 512-byte-sector disk."""
    assert head[512:520] == b"EFI PART", "not a GPT disk"
    ent_lba, n, esz = struct.unpack("<QII", head[512 + 72:512 + 88])
    for i in range(n):
        e = head[ent_lba * 512 + i * esz:ent_lba * 512 + (i + 1) * esz]
        if e[56:128].decode("utf-16le").rstrip("\0") == name:
            first, last = struct.unpack("<QQ", e[32:48])
            return first * 512, (last + 1) * 512
    sys.exit("no partition %s" % name)


def data_extents(path):
    """(offset, length) of the data regions of a sparse file (SEEK_DATA / SEEK_HOLE)."""
    fd = os.open(path, os.O_RDONLY)
    try:
        end, off = os.fstat(fd).st_size, 0
        while off < end:
            try:
                d = os.lseek(fd, off, os.SEEK_DATA)
            except OSError:
                break
            h = os.lseek(fd, d, os.SEEK_HOLE)
            yield d, h - d
            off = h
    finally:
        os.close(fd)


def write_sparse(raw, out, prog, blk=4096, chunk=64 << 20):
    """Android sparse image: RAW chunks for the data regions, DONT_CARE for the holes."""
    size = os.path.getsize(raw)
    assert size % blk == 0
    regions = []
    for d, n in data_extents(raw):
        s, e = d // blk * blk, min(size, (d + n + blk - 1) // blk * blk)
        if regions and s <= regions[-1][1]:
            regions[-1][1] = max(regions[-1][1], e)
        else:
            regions.append([s, e])
    prog.total = sum(e - s for s, e in regions)
    chunks, pos = [], 0
    for s, e in regions:
        if s > pos:
            chunks.append(("skip", pos, s))
        for c in range(s, e, chunk):
            chunks.append(("raw", c, min(e, c + chunk)))
        pos = e
    if pos < size:
        chunks.append(("skip", pos, size))
    with open(raw, "rb") as r, open(out, "wb") as o:
        o.write(struct.pack("<IHHHHIIII", 0xED26FF3A, 1, 0, 28, 12, blk, size // blk, len(chunks), 0))
        for kind, s, e in chunks:
            nblk = (e - s) // blk
            if kind == "skip":
                o.write(struct.pack("<HHII", 0xCAC3, 0, nblk, 12))
            else:
                o.write(struct.pack("<HHII", 0xCAC1, 0, nblk, 12 + (e - s)))
                r.seek(s)
                left = e - s
                while left:
                    b = r.read(min(left, 8 << 20))
                    o.write(b)
                    left -= len(b)
                    prog.add(len(b))
    log("sparse image: %s, %.2f GB of data in %d chunks" % (out, prog.total / GB, len(chunks)))


STEPS = ["format", "holo", "setup", "frame", "steam", "meta", "ours", "finish", "sparse"]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--stage", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--scratch", default=tempfile.gettempdir())
    ap.add_argument("--size", type=int, default=USERDATA_BYTES)
    ap.add_argument("--only", help="comma-separated steps: " + ",".join(STEPS))
    a = ap.parse_args()
    if os.geteuid() != 0:
        sys.exit("run as root")
    b = Build(a)
    only = a.only.split(",") if a.only else STEPS
    try:
        for name in STEPS:
            if name not in only:
                continue
            if name not in ("format", "finish", "sparse"):
                b.mount()
            event("step", step=name, status="start")
            log("==", name)
            getattr(b, name)(Progress(name, 1))
            event("step", step=name, status="done")
    finally:
        if b.mounted():
            subprocess.run(["umount", b.mnt])


if __name__ == "__main__":
    main()
