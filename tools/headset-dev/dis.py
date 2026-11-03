#!/usr/bin/env python3
# dis.py BIN ADDR [BEFORE AFTER]: disassemble around ADDR (hex vaddr), annotate adrp+add string refs
import re, subprocess, sys
b = sys.argv[1]; a = int(sys.argv[2], 16)
before = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x180
after = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0x20
data = open(b, 'rb').read()
segs = []
for l in subprocess.run(['readelf', '-lW', b], capture_output=True, text=True).stdout.splitlines():
    f = l.split()
    if f and f[0] == 'LOAD':
        off, va, fs = int(f[1], 16), int(f[2], 16), int(f[4], 16)
        segs.append((va, off, fs))
def rd(va):
    for v, o, s in segs:
        if v <= va < v + s:
            e = data.index(b'\0', o + va - v)
            t = data[o + va - v:e]
            if 3 <= len(t) < 200 and all(32 <= c < 127 for c in t):
                return t.decode()
    return None
out = subprocess.run(['objdump', '-d', '--no-show-raw-insn', '--start-address=%#x' % (a - before), '--stop-address=%#x' % (a + after), b], capture_output=True, text=True).stdout
pages = {}
for l in out.splitlines():
    m = re.match(r'\s*([0-9a-f]+):\s+(\S+)\s+(.*)', l)
    if not m:
        continue
    pc, op, args = int(m.group(1), 16), m.group(2), m.group(3)
    note = ''
    if op == 'adrp':
        r, t = args.split(',')[0], int(args.split(',')[1].split()[0], 16)
        pages[r] = t
    elif op == 'add':
        p = [x.strip() for x in args.split(',')]
        if len(p) == 3 and p[1] in pages and p[2].startswith('#'):
            t = pages[p[1]] + int(p[2][1:], 16 if p[2].startswith('#0x') else 10) if not p[2].startswith('#0x') else pages[p[1]] + int(p[2][3:], 16)
            s = rd(t)
            note = '   <%#x%s>' % (t, ' "%s"' % s if s else '')
    print('%s%x: %-6s %s%s' % ('>' if pc == a else ' ', pc, op, args, note))
