"""Print kernel version and appended DTB summary of an Image.gz-dtb."""
import sys, zlib, re, struct

def fdt_props(blob, names):
    """Walk the FDT structure block of the root node and return selected props."""
    off_struct, off_strings = struct.unpack('>II', blob[8:16])
    i, depth, out = off_struct, 0, {}
    while True:
        tok = struct.unpack('>I', blob[i:i+4])[0]; i += 4
        if tok == 1:
            e = blob.index(b'\0', i); i = (e + 4) & ~3; depth += 1
        elif tok == 2:
            depth -= 1
            if depth == 0: break
        elif tok == 3:
            ln, no = struct.unpack('>II', blob[i:i+8]); i += 8
            val = blob[i:i+ln]; i = (i + ln + 3) & ~3
            nm = blob[off_strings+no:blob.index(b'\0', off_strings+no)].decode()
            if depth == 1 and nm in names:
                out[nm] = val.rstrip(b'\0').decode(errors='replace') if nm == 'model' else val.hex()
        elif tok == 9: break
    return out

def info(path):
    d = zlib.decompressobj(31); k = d.decompress(open(path, 'rb').read()); dt = d.unused_data
    print(path, 'Image', len(k), 'dtbs', len(dt))
    print(' ', re.search(rb'Linux version [^\n]{0,70}', k).group(0).decode())
    i = 0
    while (i := dt.find(b'\xd0\x0d\xfe\xed', i)) >= 0:
        size = struct.unpack('>I', dt[i+4:i+8])[0]
        print('  dtb', size, fdt_props(dt[i:i+size], {'model', 'qcom,msm-id', 'qcom,board-id'}))
        i += size

for p in sys.argv[1:]: info(p)
