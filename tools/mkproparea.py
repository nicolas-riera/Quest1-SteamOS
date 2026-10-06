#!/usr/bin/env python3
"""Write a bionic (Android 10) system property area as a single "pre-split" file, so Android
blobs loaded through libhybris on native Linux can read properties without Android's init.

  mkproparea.py OUT [prop files...] [-s key=value ...]

bionic maps /dev/__properties__ as one prop_area when it is a regular file (ContextsPreSplit);
it must be owned by root:root and not group/other-writable. Layout follows
bionic/libc/system_properties/{prop_area,prop_info}.h."""
import struct
import sys

PA_SIZE = 128 * 1024
MAGIC, VERSION = 0x504F5250, 0xFC6ED0AB
HDR = 128                  # bytes_used, serial, magic, version, reserved[28]
PROP_VALUE_MAX = 92


class Area:
    def __init__(self):
        self.data = bytearray(PA_SIZE - HDR)
        self.used = 20     # root prop_bt (namelen 0) at offset 0

    def alloc(self, size):
        size = (size + 3) & ~3
        off = self.used
        if off + size > len(self.data):
            raise RuntimeError("property area full")
        self.used += size
        return off

    def u32(self, off, v=None):
        if v is None:
            return struct.unpack_from("<I", self.data, off)[0]
        struct.pack_into("<I", self.data, off, v)

    def new_bt(self, name):
        n = name.encode()
        off = self.alloc(20 + len(n) + 1)
        self.u32(off, len(n))
        self.data[off + 20:off + 20 + len(n)] = n
        return off

    def bt_name(self, off):
        n = self.u32(off)
        return bytes(self.data[off + 20:off + 20 + n])

    def find_or_add(self, parent, seg):
        # children of `parent` form a BST ordered by (length, bytes)
        key = seg.encode()
        slot = parent + 16          # &parent->children
        cur = self.u32(slot)
        while cur:
            name = self.bt_name(cur)
            c = (len(key) > len(name)) - (len(key) < len(name)) or (key > name) - (key < name)
            if c == 0:
                return cur
            slot = cur + (8 if c < 0 else 12)   # left / right
            cur = self.u32(slot)
        new = self.new_bt(seg)
        self.u32(slot, new)
        return new

    def add(self, name, value):
        v = value.encode()
        if len(v) >= PROP_VALUE_MAX:
            return False
        node = 0
        for seg in name.split("."):
            node = self.find_or_add(node, seg)
        if self.u32(node + 4):
            return False             # first definition wins
        n = name.encode()
        pi = self.alloc(4 + PROP_VALUE_MAX + len(n) + 1)
        self.u32(pi, len(v) << 24)   # serial: value length in the top byte
        self.data[pi + 4:pi + 4 + len(v)] = v
        self.data[pi + 4 + PROP_VALUE_MAX:pi + 4 + PROP_VALUE_MAX + len(n)] = n
        self.u32(node + 4, pi)
        return True

    def blob(self):
        hdr = struct.pack("<IIII", self.used, 0, MAGIC, VERSION) + b"\0" * (HDR - 16)
        return hdr + bytes(self.data)


def main():
    out, args = sys.argv[1], sys.argv[2:]
    props = []
    i = 0
    while i < len(args):
        if args[i] == "-s":
            k, _, v = args[i + 1].partition("=")
            props.append((k, v))
            i += 2
            continue
        try:
            for line in open(args[i], errors="replace"):
                line = line.strip()
                if line and not line.startswith("#") and "=" in line and not line.startswith("import "):
                    k, _, v = line.partition("=")
                    props.append((k.strip(), v.strip()))
        except FileNotFoundError:
            pass
        i += 1
    a = Area()
    # -s overrides come last on the command line but must win: add them first
    overrides = [p for p in props if p in [(args[j + 1].partition("=")[0], args[j + 1].partition("=")[2])
                                            for j in range(len(args)) if args[j] == "-s"]]
    n = sum(a.add(k, v) for k, v in overrides + props)
    with open(out, "wb") as f:
        f.write(a.blob())
    print(f"{out}: {n} properties, {a.used} of {PA_SIZE - HDR} bytes used")


if __name__ == "__main__":
    main()
