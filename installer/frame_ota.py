"""The Steam Frame's rootfs rebuilt from Valve's public update server (installer/sources.json "steam_frame_ota").

The server has no full image: a RAUC bundle (.raucb, a squashfs holding manifest.raucm and rootfs.img.caibx) and a
casync chunk store (zstd-compressed chunks, the id of a chunk = SHA-512/256 of its uncompressed data). prepare.py
downloads the bundle and the chunks; this module (run on the headset by build_image.py, Python + libzstd through
btrfsread's ctypes helper) reads the two files out of the bundle and writes the 10 GiB rootfs image (btrfs) the
chunks make up, as a sparse file. Every chunk and the whole image are checked.

    python3 frame_ota.py FRAME_DIR OUT_IMAGE [ROOTFS_SHA256]
"""
import hashlib, os, struct, sys, time
from concurrent.futures import ThreadPoolExecutor

import btrfsread

CA_FORMAT_INDEX = 0x96824d9c7b129ff9
CA_FORMAT_TABLE = 0xe75b9e112f17417d
ZSTD = 6


class Error(Exception):
    pass


# --- squashfs 4 (just enough: files in the root directory, zstd) -----------------------------------------------

class Squash:
    def __init__(self, path):
        self.f = open(path, "rb")
        sb = self.f.read(96)
        if sb[:4] != b"hsqs":
            raise Error("%s: not a squashfs bundle (verity or crypt bundles are not supported)" % path)
        (_, _, _, self.bs, self.frags, comp, _, _, _, major, _, self.root, _, _, _, self.inode_start,
         self.dir_start, self.frag_start, _) = struct.unpack("<IIIIIHHHHHHQQQQQQQQ", sb[:96])
        if major != 4 or comp != ZSTD:
            raise Error("squashfs v%d compression %d: only v4 zstd is supported" % (major, comp))
        self.inodes = self.table(self.inode_start, self.dir_start)
        self.dirs = self.table(self.dir_start, self.frag_start)

    def meta(self, pos):
        """One metadata block at pos: (data, position after it)."""
        self.f.seek(pos)
        h = struct.unpack("<H", self.f.read(2))[0]
        n = h & 0x7FFF
        raw = self.f.read(n)
        return (raw if h & 0x8000 else btrfsread.zstd_decompress(raw, 8192)), pos + 2 + n

    def table(self, start, end):
        """A run of metadata blocks as one buffer, with {offset of a block from start: offset in the buffer}."""
        out, starts, pos = bytearray(), {}, start
        while pos < end:
            starts[pos - start] = len(out)
            d, nxt = self.meta(pos)
            out += d
            pos = nxt
        return bytes(out), starts

    def data(self, pos, size):
        self.f.seek(pos)
        raw = self.f.read(size & 0xFFFFFF)
        return raw if size & 0x1000000 else btrfsread.zstd_decompress(raw, self.bs)

    def fragment(self, idx):
        self.f.seek(self.frag_start + 8 * (idx // 512))
        ptr = struct.unpack("<Q", self.f.read(8))[0]
        ents, _ = self.meta(ptr)
        start, size = struct.unpack_from("<QI", ents, 16 * (idx % 512))
        return start, size

    def read_root_file(self, name):
        buf, starts = self.dirs
        ibuf, istarts = self.inodes
        ino = istarts[self.root >> 16] + (self.root & 0xFFFF)
        typ = struct.unpack_from("<H", ibuf, ino)[0]
        if typ == 1:
            start, _, size, off = struct.unpack_from("<IIHH", ibuf, ino + 16)
        elif typ == 8:
            _, size, start, _, _, off = struct.unpack_from("<IIIIHH", ibuf, ino + 16)
        else:
            raise Error("squashfs: root is not a directory")
        p, end = starts[start] + off, starts[start] + off + size - 3
        want, found = name.encode(), None
        while p < end and found is None:
            count, start, _ = struct.unpack_from("<III", buf, p)
            p += 12
            for _ in range(count + 1):
                eoff, _, _, nlen = struct.unpack_from("<HhHH", buf, p)
                n = buf[p + 8:p + 9 + nlen]
                p += 8 + nlen + 1
                if n == want:
                    found = (start << 16) | eoff
                    break
        if found is None:
            raise Error("%s not in the bundle" % name)
        o = istarts[found >> 16] + (found & 0xFFFF)
        typ = struct.unpack_from("<H", ibuf, o)[0]
        if typ == 2:
            blocks_at, (start, frag, foff, size) = o + 32, struct.unpack_from("<IIII", ibuf, o + 16)
        elif typ == 9:
            start, size, _, _, frag, foff, _ = struct.unpack_from("<QQQIIII", ibuf, o + 16)
            blocks_at = o + 56
        else:
            raise Error("%s: not a regular file (inode type %d)" % (name, typ))
        nblk = size // self.bs if frag != 0xFFFFFFFF else -(-size // self.bs)
        out, pos = bytearray(), start
        for i in range(nblk):
            sz = struct.unpack_from("<I", ibuf, blocks_at + 4 * i)[0]
            out += self.data(pos, sz) if sz & 0xFFFFFF else bytes(self.bs)
            pos += sz & 0xFFFFFF
        if frag != 0xFFFFFFFF:
            fstart, fsize = self.fragment(frag)
            out += self.data(fstart, fsize)[foff:foff + size - len(out)]
        return bytes(out[:size])


def read_bundle(raucb):
    """(manifest.raucm text, rootfs.img.caibx bytes) of a plain RAUC bundle."""
    s = Squash(raucb)
    try:
        return s.read_root_file("manifest.raucm").decode(), s.read_root_file("rootfs.img.caibx")
    finally:
        s.f.close()


def manifest_value(text, section, key):
    cur = None
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("[") and line.endswith("]"):
            cur = line[1:-1]
        elif cur == section and "=" in line and line.split("=", 1)[0].strip() == key:
            return line.split("=", 1)[1].strip()
    return None


# --- casync index -------------------------------------------------------------------------------------------

def parse_caibx(data):
    """[(end offset, chunk id hex)] of a casync index (FormatIndex, then FormatTable)."""
    size, typ, _, cmin, cavg, cmax = struct.unpack_from("<QQQQQQ", data, 0)
    if typ != CA_FORMAT_INDEX or size != 48:
        raise Error("not a casync index")
    p = 48
    marker, typ = struct.unpack_from("<QQ", data, p)
    if marker != (1 << 64) - 1 or typ != CA_FORMAT_TABLE:
        raise Error("casync index: no chunk table")
    p += 16
    items = []
    while p + 40 <= len(data):
        end = struct.unpack_from("<Q", data, p)[0]
        if end == 0:  # the table's terminator (a chunk never ends at offset 0)
            return items
        items.append((end, data[p + 8:p + 40].hex()))
        p += 40
    raise Error("casync index: table not terminated")


def chunk_path(chunks, cid):
    return os.path.join(chunks, cid[:4], cid + ".cacnk")


def assemble(frame_dir, out, want_sha=None, delete=False, prog=None, workers=None):
    """Rebuild the rootfs image from frame_dir (*.raucb + chunks/) into out, sparse. Returns (sha256, size,
    manifest text). delete: remove each chunk after its last use."""
    raucb = [f for f in os.listdir(frame_dir) if f.endswith(".raucb")][0]
    text, caibx = read_bundle(os.path.join(frame_dir, raucb))
    size = int(manifest_value(text, "image.rootfs", "size"))
    man_sha = manifest_value(text, "image.rootfs", "sha256")
    if want_sha and man_sha != want_sha:
        raise Error("bundle's rootfs sha256 %s, pinned %s" % (man_sha, want_sha))
    items = parse_caibx(caibx)
    if items[-1][0] != size:
        raise Error("casync index covers %d bytes, manifest says %d" % (items[-1][0], size))
    chunks = os.path.join(frame_dir, "chunks")
    last = {cid: i for i, (_, cid) in enumerate(items)}
    if prog:
        prog.total = sum(os.path.getsize(chunk_path(chunks, c)) for _, c in items)  # a repeated chunk counts each time
    starts = [0] + [e for e, _ in items[:-1]]

    def load(i):
        cid, n = items[i][1], items[i][0] - starts[i]
        with open(chunk_path(chunks, cid), "rb") as f:
            raw = f.read()
        data = btrfsread.zstd_decompress(raw, n)
        if len(data) != n or hashlib.new("sha512_256", data).hexdigest() != cid:
            raise Error("chunk %s is corrupt (%d bytes, expected %d)" % (cid, len(data), n))
        return data, len(raw)

    h = hashlib.sha256()
    zero = bytes(1 << 20)
    window = 64
    with open(out, "wb") as o, ThreadPoolExecutor(workers or os.cpu_count() or 2) as pool:
        for b in range(0, len(items), window):
            idx = range(b, min(b + window, len(items)))
            for i, (data, csize) in zip(idx, pool.map(load, idx)):
                if data.count(0) == len(data):
                    o.seek(len(data), 1)  # the empty chunk: a hole
                    for k in range(0, len(data), len(zero)):
                        h.update(zero[:min(len(zero), len(data) - k)])
                else:
                    o.write(data)
                    h.update(data)
                if prog:
                    prog.add(csize)
                cid = items[i][1]
                if delete and last[cid] == i:
                    os.remove(chunk_path(chunks, cid))
        o.truncate(size)
    got = h.hexdigest()
    if got != man_sha:
        raise Error("rebuilt image sha256 %s, manifest says %s" % (got, man_sha))
    return got, size, text


def main():
    t = time.time()
    sha, size, _ = assemble(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None)
    print("%s %d bytes, %.0f s" % (sha, size, time.time() - t))


if __name__ == "__main__":
    main()
