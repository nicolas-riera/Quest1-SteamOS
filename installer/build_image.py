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
    setup   our base setup in an arm64 chroot (device/native-setup.sh, full layout: the root is userdata)
    pkgs    Holo packages (device/packages.txt + dependencies, staged by prepare.py), pacman offline
    frame   SteamVR (/opt/steamvr) and GTK 2 from the Steam Frame's recovery image
    steam   the Steam client into ~steamos/.local/share/Steam
    meta    Meta's files in /usr/lib/quest1/meta: the system tree without what native Holo never uses
            (installer/meta-exclude.txt) and the modem image, mounted by device/holo-android-blobs.sh
    native  our prebuilt binaries (stage/prebuilt/native-arm64.tar.zst, tools/build-native.sh), then every
            install script in the chroot (device/install-image.sh)
    ours    this repository (/usr/lib/quest1/repo) and the version file
    finish  unmount, e2fsck
    sparse  Android sparse image
"""
import argparse, bz2, json, os, re, shutil, stat, struct, subprocess, sys, tempfile, time, zipfile

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

    CONSUMES = {"holo": ["holo"], "pkgs": ["pkgs"], "frame": ["frame"], "steam": ["steam"], "meta": ["meta"],
                "native": ["prebuilt/native-arm64.tar.zst"]}

    def consume(self, step):
        for rel in self.CONSUMES.get(step, []):
            p = os.path.join(self.stage, rel)
            if os.path.isdir(p):
                shutil.rmtree(p, ignore_errors=True)
            elif os.path.exists(p):
                os.remove(p)

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

    def chroot(self, *cmd, on_line=None):
        m = self.mnt
        binds = [("proc", "proc"), ("sysfs", "sys"), ("devtmpfs", "dev"), ("devpts", "dev/pts")]
        done = []
        try:
            for fs, d in binds:
                os.makedirs(os.path.join(m, d), exist_ok=True)
                run("mount", "-t", fs, fs, os.path.join(m, d))
                done.append(os.path.join(m, d))
            env = {"PATH": "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin", "HOME": "/root", "LANG": "C.UTF-8",
                   "QUEST1_LAYOUT": "full"}
            log("$ chroot", m, *cmd)
            p = subprocess.Popen(["chroot", m, *cmd], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, errors="replace")
            for line in p.stdout:
                print(line, end="", flush=True)
                if on_line:
                    on_line(line)
            if p.wait():
                sys.exit("%s failed (%d)" % (" ".join(cmd), p.returncode))
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
                    piece = out[s - pos:e - pos]
                    if piece.count(0) == len(piece):
                        o.seek(len(piece), 1)  # empty space of the partition: a hole
                    else:
                        o.write(piece)
                pos += len(out)
                if pos >= rng[1]:
                    break
            o.truncate()
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
        zips = sorted(f for f in os.listdir(pkg) if re.search(r"\.zip\.[0-9a-f]{40}$", f))  # name.zip.<sha1>
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

    def pkgs(self, prog):
        """Offline pacman: the repositories' databases and the downloaded packages in the image's own cache."""
        src = os.path.join(self.stage, "pkgs")
        sync = os.path.join(self.mnt, "var", "lib", "pacman", "sync")
        cache = os.path.join(self.mnt, "var", "cache", "pacman", "pkg")
        os.makedirs(sync, exist_ok=True)
        os.makedirs(cache, exist_ok=True)
        for f in os.listdir(src):
            if f.endswith(".db"):
                shutil.copyfile(os.path.join(src, f), os.path.join(sync, f))
            elif f.endswith((".pkg.tar.zst", ".pkg.tar.xz")):
                shutil.copyfile(os.path.join(src, f), os.path.join(cache, f.replace("%3A", ":")))
        wanted = open(os.path.join(src, "packages.txt")).read().split()
        # offline for real: the repositories point nowhere, so a package missing from the staged ones fails the
        # build here (on the headset there is no network for pacman anyway)
        conf = os.path.join(self.mnt, "etc", "pacman.conf")
        with open(conf) as f:
            text = f.read()
        off = re.sub(r"(?m)^Include\s*=.*mirrorlist\s*$", "Server = file:///nonexistent", text)
        # Holo's repositories are unsigned (SigLevel Optional): pacman then wants each package's .sig and
        # downloads the package again to get it. Integrity: every file was checked against the repository
        # database's sha256 when downloaded, and pacman checks it again
        off = re.sub(r"(?m)^SigLevel\s*=.*$", "SigLevel = Never", off)
        with open(os.path.join(self.mnt, "etc", "pacman-offline.conf"), "w") as f:
            f.write(off)

        def on_line(line):  # "(12/367) installing foo"
            m = re.match(r"\((\d+)/(\d+)\) installing", line)
            if m:
                prog.total = int(m.group(2))
                prog.add(int(m.group(1)) - prog.done)
        self.chroot("pacman", "--config", "/etc/pacman-offline.conf", "-S", "--needed", "--noconfirm",
                    "--noprogressbar", *wanted, on_line=on_line)
        os.remove(os.path.join(self.mnt, "etc", "pacman-offline.conf"))
        shutil.rmtree(cache)
        os.makedirs(cache)

    def meta(self, prog):
        """Meta's system tree minus what native Holo never runs (apps, Java, 32-bit, fonts...), and the modem image."""
        d = os.path.join(self.mnt, "usr", "lib", "quest1", "meta")
        os.makedirs(d, exist_ok=True)
        sysimg, modem = (os.path.join(self.stage, "meta", i) for i in ("system.img", "modem.img"))
        prog.total = os.path.getsize(modem) + (600 << 20)
        m = os.path.join(self.a.scratch, "meta-system")
        os.makedirs(m, exist_ok=True)
        run("mount", "-t", "ext4", "-o", "loop,ro,noload", sysimg, m)
        ex = os.path.join(self.a.scratch, "meta-exclude")
        with open(os.path.join(self.repo, "installer", "meta-exclude.txt")) as f, open(ex, "w") as o:
            o.write("".join(l for l in f if l.strip() and not l.startswith("#")))
        out = os.path.join(d, "system")
        shutil.rmtree(out, ignore_errors=True)
        os.makedirs(out)
        try:
            src = subprocess.Popen(["tar", "-C", m, "-c", "--xattrs", "--numeric-owner", "-X", ex, "."],
                                   stdout=subprocess.PIPE)
            dst = subprocess.Popen(["tar", "-C", out, "-x", "--xattrs", "--numeric-owner", "-p"], stdin=subprocess.PIPE)
            for b in iter(lambda: src.stdout.read(1 << 20), b""):
                dst.stdin.write(b)
                prog.add(len(b))
            dst.stdin.close()
            if src.wait() or dst.wait():
                sys.exit("copying Meta's system tree failed")
        finally:
            run("umount", m)
        shutil.copyfile(modem, os.path.join(d, "modem.img"))
        prog.add(os.path.getsize(modem))

    def native(self, prog):
        """Our binaries, then every install script inside the image (device/install-image.sh)."""
        tz = os.path.join(self.stage, "prebuilt", "native-arm64.tar.zst")
        prog.total = os.path.getsize(tz) * 2
        z = subprocess.Popen(["zstd", "-dc", tz], stdout=subprocess.PIPE)
        run("tar", "-C", self.mnt, "-xp", "--numeric-owner", "--no-overwrite-dir", stdin=z.stdout)
        if z.wait():
            sys.exit("unpacking %s failed" % tz)
        prog.add(prog.total // 2)
        tmp = os.path.join(self.mnt, "root", "q1src")
        shutil.rmtree(tmp, ignore_errors=True)
        for sub in ("device", "tools"):
            copytree(os.path.join(self.repo, sub), os.path.join(tmp, sub), Progress("native", 1))
        self.chroot("/bin/sh", "-e", "/root/q1src/device/install-image.sh")
        shutil.rmtree(tmp)
        prog.add(prog.total // 2)

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


def sparse_plan(raw, blk=4096, chunk=64 << 20):
    """The chunks of an Android sparse image of raw: RAW for the data regions, DONT_CARE for the holes.
    Returns (chunks, length of the sparse image, bytes of data)."""
    size = os.path.getsize(raw)
    assert size % blk == 0
    regions = []
    for d, n in data_extents(raw):
        s, e = d // blk * blk, min(size, (d + n + blk - 1) // blk * blk)
        if regions and s <= regions[-1][1]:
            regions[-1][1] = max(regions[-1][1], e)
        else:
            regions.append([s, e])
    chunks, pos = [], 0
    for s, e in regions:
        if s > pos:
            chunks.append(("skip", pos, s))
        for c in range(s, e, chunk):
            chunks.append(("raw", c, min(e, c + chunk)))
        pos = e
    if pos < size:
        chunks.append(("skip", pos, size))
    data = sum(e - s for k, s, e in chunks if k == "raw")
    return chunks, 28 + 12 * len(chunks) + data, data


def sparse_stream(raw, chunks, write, prog=None, blk=4096):
    """Write the sparse image of raw through write(bytes): to a file, or straight to the PC (agent.py)."""
    size = os.path.getsize(raw)
    write(struct.pack("<IHHHHIIII", 0xED26FF3A, 1, 0, 28, 12, blk, size // blk, len(chunks), 0))
    with open(raw, "rb") as r:
        for kind, s, e in chunks:
            nblk = (e - s) // blk
            if kind == "skip":
                write(struct.pack("<HHII", 0xCAC3, 0, nblk, 12))
                continue
            write(struct.pack("<HHII", 0xCAC1, 0, nblk, 12 + (e - s)))
            r.seek(s)
            left = e - s
            while left:
                b = r.read(min(left, 8 << 20))
                write(b)
                left -= len(b)
                if prog:
                    prog.add(len(b))


def write_sparse(raw, out, prog):
    chunks, length, data = sparse_plan(raw)
    prog.total = data
    with open(out, "wb") as o:
        sparse_stream(raw, chunks, o.write, prog)
    log("sparse image: %s, %.2f GB of data in %d chunks" % (out, data / GB, len(chunks)))


STEPS = ["format", "holo", "setup", "pkgs", "frame", "steam", "meta", "native", "ours", "finish", "sparse"]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--stage", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--scratch", default=tempfile.gettempdir())
    ap.add_argument("--size", type=int, default=USERDATA_BYTES)
    ap.add_argument("--only", help="comma-separated steps: " + ",".join(STEPS))
    ap.add_argument("--consume", action="store_true", help="delete each staged input once its step used it")
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
            if a.consume:
                b.consume(name)
    finally:
        if b.mounted():
            subprocess.run(["umount", b.mnt])


if __name__ == "__main__":
    main()
