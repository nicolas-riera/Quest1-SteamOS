#!/usr/bin/env python3
"""Minimal Android A/B payload.bin extractor (full OTA only, no deps).

Usage: payload_extract.py <payload.bin|ota.zip> <outdir> [partition ...]
"""
import bz2, hashlib, lzma, os, struct, sys, zipfile


def varint(b, i):
    r = s = 0
    while True:
        c = b[i]; i += 1
        r |= (c & 0x7F) << s; s += 7
        if c < 0x80:
            return r, i


def pb(b):
    """Decode a protobuf message into {field: [values]} (bytes for len-delimited)."""
    out, i = {}, 0
    while i < len(b):
        key, i = varint(b, i)
        f, wt = key >> 3, key & 7
        if wt == 0:
            v, i = varint(b, i)
        elif wt == 1:
            v = struct.unpack_from("<Q", b, i)[0]; i += 8
        elif wt == 2:
            n, i = varint(b, i); v = b[i:i + n]; i += n
        elif wt == 5:
            v = struct.unpack_from("<I", b, i)[0]; i += 4
        else:
            raise ValueError("wire type %d" % wt)
        out.setdefault(f, []).append(v)
    return out


# InstallOperation.Type
REPLACE, REPLACE_BZ, ZERO, DISCARD, REPLACE_XZ = 0, 1, 6, 7, 8


def open_payload(path):
    if path.endswith(".zip"):
        z = zipfile.ZipFile(path)
        info = z.getinfo("payload.bin")
        f = open(path, "rb")
        f.seek(info.header_offset)
        hdr = f.read(30)
        n, m = struct.unpack_from("<HH", hdr, 26)
        base = info.header_offset + 30 + n + m
        assert info.compress_type == zipfile.ZIP_STORED
        return f, base
    return open(path, "rb"), 0


def main():
    src, outdir, want = sys.argv[1], sys.argv[2], set(sys.argv[3:])
    found = set()
    f, base = open_payload(src)
    f.seek(base)
    magic, ver, msize = struct.unpack(">4sQQ", f.read(20))
    assert magic == b"CrAU", magic
    sigsize = struct.unpack(">I", f.read(4))[0] if ver >= 2 else 0
    man = pb(f.read(msize))
    data_off = base + 20 + (4 if ver >= 2 else 0) + msize + sigsize
    bs = man.get(3, [4096])[0]
    os.makedirs(outdir, exist_ok=True)
    for p in man.get(13, []):
        part = pb(p)
        name = part[1][0].decode()
        newinfo = pb(part[7][0]) if 7 in part else {}
        size = newinfo.get(1, [0])[0]
        h = newinfo.get(2, [b""])[0]
        if want and name not in want:
            print("skip %-14s %10d" % (name, size)); continue
        found.add(name)
        ops = [pb(o) for o in part.get(8, [])]
        with open(os.path.join(outdir, name + ".img"), "wb") as o:
            o.truncate(size)
            for op in ops:
                t = op[1][0]
                exts = [pb(e) for e in op.get(6, [])]
                if t in (REPLACE, REPLACE_BZ, REPLACE_XZ):
                    f.seek(data_off + op[2][0]); d = f.read(op[3][0])
                    if t == REPLACE_BZ: d = bz2.decompress(d)
                    elif t == REPLACE_XZ: d = lzma.decompress(d)
                    for e in exts:
                        o.seek(e[1][0] * bs); n = e[2][0] * bs
                        o.write(d[:n]); d = d[n:]
                elif t in (ZERO, DISCARD):
                    for e in exts:
                        o.seek(e[1][0] * bs); o.write(b"\0" * (e[2][0] * bs))
                else:
                    sys.exit("%s: unsupported op %d (delta OTA?)" % (name, t))
        # The system partition can be several GiB. Hash it incrementally and fail
        # closed: prepare.py calls this function directly and must not stage a
        # corrupt image after merely printing a warning.
        digest = hashlib.sha256()
        with open(os.path.join(outdir, name + ".img"), "rb") as image:
            for chunk in iter(lambda: image.read(1024 * 1024), b""):
                digest.update(chunk)
        if not h or digest.digest() != h:
            sys.exit("%s: HASH MISMATCH" % name)
        print("%-14s %10d OK" % (name, size))

    missing = want - found
    if missing:
        sys.exit("partitions not found: %s" % ", ".join(sorted(missing)))


if __name__ == "__main__":
    main()
