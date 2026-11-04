"""Read-only btrfs, enough to copy files out of an image file: the Steam Frame's rootfs, which the headset's 4.4
kernel cannot mount and which Alpine's `btrfs restore` reads wrong (small files fail: an inline extent's zstd
frame holds a whole zero-padded page, more than the extent's size; then "zstd frame incomplete"). Single-device
images (SINGLE/DUP chunks), zstd and zlib compression; no checksum verification. Python only, plus the system's
libzstd through ctypes. Checked against the kernel's own reading of the Frame image (same files, contents,
modes, owners, times, links).

    python3 btrfsread.py IMAGE /opt/steamvr DEST     copies the tree to DEST/opt/steamvr, as `cp -a` would
"""
import bisect, ctypes, ctypes.util, os, stat, struct, sys, zlib

SUPER, HDR, MAX = 0x10000, 0x65, (1 << 64) - 1
KEY = struct.Struct("<QBQ")
INODE_ITEM, XATTR_ITEM, DIR_INDEX, EXTENT_DATA, ROOT_ITEM, DIR_ITEM, CHUNK_ITEM = 1, 24, 96, 108, 132, 84, 228


class Error(Exception):
    pass


_zstd = None


def zstd_decompress(data, size):
    """One zstd frame (what btrfs writes per extent), the padding after it ignored."""
    global _zstd
    if _zstd is None:
        lib = ctypes.CDLL(ctypes.util.find_library("zstd") or "libzstd.so.1")
        lib.ZSTD_findFrameCompressedSize.restype = ctypes.c_size_t
        lib.ZSTD_findFrameCompressedSize.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
        lib.ZSTD_decompress.restype = ctypes.c_size_t
        lib.ZSTD_decompress.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
        lib.ZSTD_isError.restype = ctypes.c_uint
        lib.ZSTD_isError.argtypes = [ctypes.c_size_t]
        lib.ZSTD_getFrameContentSize.restype = ctypes.c_ulonglong
        lib.ZSTD_getFrameContentSize.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
        _zstd = lib
    n = _zstd.ZSTD_findFrameCompressedSize(data, len(data))
    if _zstd.ZSTD_isError(n):
        raise Error("zstd: no frame")
    # the frame can hold more than the extent's size: an inline extent's frame is a whole zero-padded page
    content = _zstd.ZSTD_getFrameContentSize(data, n)
    cap = max(size, content if content < (1 << 64) - 2 else -(-size // (128 << 10)) * (128 << 10), 1)
    out = ctypes.create_string_buffer(cap)
    r = _zstd.ZSTD_decompress(out, cap, data, n)
    if _zstd.ZSTD_isError(r):
        raise Error("zstd: corrupt extent")
    return out.raw[:min(r, size)]


def decompress(kind, data, size):
    if kind == 3:
        return zstd_decompress(data, size)
    if kind == 1:
        return zlib.decompressobj().decompress(data, size)
    raise Error("compression %d not supported (lzo)" % kind)


def entries(data):
    """DIR_ITEM / DIR_INDEX / XATTR_ITEM payload: (location key, name, type, data) for each entry."""
    p = 0
    while p < len(data):
        loc = KEY.unpack_from(data, p)
        _, dlen, nlen, typ = struct.unpack_from("<QHHB", data, p + 17)
        name = data[p + 30:p + 30 + nlen]
        yield loc, name, typ, data[p + 30 + nlen:p + 30 + nlen + dlen]
        p += 30 + nlen + dlen


class Image:
    def __init__(self, path):
        self.f = open(path, "rb")
        sb = self.pread(SUPER, 4096)
        if sb[0x40:0x48] != b"_BHRfS_M":
            raise Error("%s: not btrfs" % path)
        root, chunk_root = struct.unpack_from("<QQ", sb, 0x50)
        self.nodesize = struct.unpack_from("<I", sb, 0x94)[0]
        self.chunks = {}  # logical start -> (length, physical start of stripe 0)
        arr, p = sb[0x32B:0x32B + struct.unpack_from("<I", sb, 0xA0)[0]], 0
        while p < len(arr):
            logical = KEY.unpack_from(arr, p)[2]
            self.add_chunk(logical, arr, p + 17)
            p += 17 + 48 + 32 * struct.unpack_from("<H", arr, p + 17 + 44)[0]
        self.cache = {}
        for key, data in self.items(chunk_root, (256, CHUNK_ITEM, 0), (256, CHUNK_ITEM, MAX)):
            self.add_chunk(key[2], data, 0)
        self.root_tree = root
        sub = 5  # the default subvolume: "default" in the root tree's directory, else the top level
        for _, data in self.items(root, (6, DIR_ITEM, 0), (6, DIR_ITEM, MAX)):
            for loc, name, _, _ in entries(data):
                if name == b"default":
                    sub = loc[0]
        self.top = (self.tree(sub), 256)

    def add_chunk(self, logical, buf, p):
        self.chunks[logical] = (struct.unpack_from("<Q", buf, p)[0], struct.unpack_from("<Q", buf, p + 48 + 8)[0])
        self.starts = sorted(self.chunks)

    def pread(self, off, n):
        self.f.seek(off)
        b = self.f.read(n)
        if len(b) != n:
            raise Error("short read at %d" % off)
        return b

    def read(self, logical, n):
        i = bisect.bisect_right(self.starts, logical) - 1
        if i < 0:
            raise Error("no chunk for %d" % logical)
        start = self.starts[i]
        length, phys = self.chunks[start]
        if logical + n > start + length:
            raise Error("read across a chunk at %d" % logical)
        return self.pread(phys + logical - start, n)

    def node(self, bytenr):
        b = self.cache.get(bytenr)
        if b is None:
            if len(self.cache) > 4096:
                self.cache.clear()
            b = self.cache[bytenr] = self.read(bytenr, self.nodesize)
        return b

    def items(self, bytenr, lo, hi):
        """The items of the tree at bytenr with lo <= key <= hi, in key order: (key, data)."""
        b = self.node(bytenr)
        n, level = struct.unpack_from("<IB", b, 0x60)
        if level == 0:
            for i in range(n):
                o = HDR + 25 * i
                key = KEY.unpack_from(b, o)
                if key < lo:
                    continue
                if key > hi:
                    return
                off, size = struct.unpack_from("<II", b, o + 17)
                yield key, b[HDR + off:HDR + off + size]
            return
        ptrs = [(KEY.unpack_from(b, HDR + 33 * i), struct.unpack_from("<Q", b, HDR + 33 * i + 17)[0])
                for i in range(n)]
        for i, (key, ptr) in enumerate(ptrs):
            if key > hi:
                return
            if i + 1 < n and ptrs[i + 1][0] <= lo:
                continue
            yield from self.items(ptr, lo, hi)

    def tree(self, objectid):
        found = None
        for _, data in self.items(self.root_tree, (objectid, ROOT_ITEM, 0), (objectid, ROOT_ITEM, MAX)):
            found = struct.unpack_from("<Q", data, 176)[0]  # the newest root item wins
        if found is None:
            raise Error("no tree %d" % objectid)
        return found

    # --- files ------------------------------------------------------------------------------------------------
    def inode(self, node):
        tree, ino = node
        for _, d in self.items(tree, (ino, INODE_ITEM, 0), (ino, INODE_ITEM, MAX)):
            size, = struct.unpack_from("<Q", d, 16)
            nlink, uid, gid, mode = struct.unpack_from("<IIII", d, 40)
            atime, = struct.unpack_from("<Q", d, 112)
            mtime, mns = struct.unpack_from("<QI", d, 136)
            return {"size": size, "nlink": nlink, "uid": uid, "gid": gid, "mode": mode,
                    "atime": atime, "mtime": mtime + mns / 1e9}
        raise Error("no inode %d" % ino)

    def children(self, node):
        """(name, node) of a directory, in creation order; a subvolume is entered at its top directory."""
        tree, ino = node
        for _, d in self.items(tree, (ino, DIR_INDEX, 0), (ino, DIR_INDEX, MAX)):
            for loc, name, _, _ in entries(d):
                yield name, ((self.tree(loc[0]), 256) if loc[1] == ROOT_ITEM else (tree, loc[0]))

    def lookup(self, path):
        node = self.top
        for part in [p for p in path.split("/") if p]:
            for name, child in self.children(node):
                if name == part.encode():
                    node = child
                    break
            else:
                raise Error("%s: not found" % path)
        return node

    def xattrs(self, node):
        tree, ino = node
        for _, d in self.items(tree, (ino, XATTR_ITEM, 0), (ino, XATTR_ITEM, MAX)):
            for _, name, _, value in entries(d):
                yield name, value

    def extents(self, node):
        """(file offset, bytes) of a file's data, holes left out; pieces of at most 8 MB."""
        tree, ino = node
        for key, d in self.items(tree, (ino, EXTENT_DATA, 0), (ino, EXTENT_DATA, MAX)):
            pos = key[2]
            ram, comp, _, _, typ = struct.unpack_from("<QBBHB", d, 8)
            if typ == 0:  # inline
                data = d[21:]
                yield pos, decompress(comp, data, ram) if comp else data
                continue
            bytenr, disk_len, off, num = struct.unpack_from("<QQQQ", d, 21)
            if bytenr == 0 or typ == 2:  # a hole, or preallocated: zeros
                continue
            if comp:
                yield pos, decompress(comp, self.read(bytenr, disk_len), ram)[off:off + num]
            else:
                for o in range(0, num, 8 << 20):
                    yield pos + o, self.read(bytenr + off + o, min(8 << 20, num - o))

    def copy(self, node, dst, links=None):
        """node to dst (dst must not exist), like `cp -a`: modes, owners, times, xattrs, symlinks, hard links,
        holes."""
        links = {} if links is None else links
        st = self.inode(node)
        mode = st["mode"]
        if stat.S_ISDIR(mode):
            os.mkdir(dst)
            for name, child in self.children(node):
                self.copy(child, os.path.join(dst, os.fsdecode(name)), links)
        elif node in links:
            os.link(links[node], dst)
            return
        elif stat.S_ISLNK(mode):
            os.symlink(b"".join(b for _, b in self.extents(node))[:st["size"]], dst)
        elif stat.S_ISREG(mode):
            with open(dst, "wb") as out:
                for pos, data in self.extents(node):
                    if data.count(0) != len(data):  # zeros stay holes
                        out.seek(pos)
                        out.write(data)
                out.truncate(st["size"])
        else:
            return  # devices, fifos, sockets: none in what we copy
        if st["nlink"] > 1 and not stat.S_ISDIR(mode):
            links[node] = dst
        os.lchown(dst, st["uid"], st["gid"])
        for name, value in self.xattrs(node):
            try:
                os.setxattr(dst, name, value, follow_symlinks=False)
            except OSError:
                pass  # a namespace the target does not take
        if not stat.S_ISLNK(mode):
            os.chmod(dst, stat.S_IMODE(mode))
        os.utime(dst, (st["atime"], st["mtime"]), follow_symlinks=False)


def main():
    img, src, dest = sys.argv[1:4]
    im = Image(img)
    out = os.path.join(dest, src.strip("/"))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    im.copy(im.lookup(src), out)


if __name__ == "__main__":
    main()
