#!/usr/bin/python3
# mkrendermodel.py <SteamVR runtime dir> <driver dir>: the right Quest controller's render model, with the Oculus
# button showing the Oculus logo. SteamVR's oculus_quest_controller_right texture has the menu button's glyph (three
# bars, as on the left controller) where the right controller's Oculus button is. This copies SteamVR's model into
# the driver's resources (rendermodels/quest1_controller_right, picked by controllers.cpp when present) and redraws
# only that glyph in the copy's texture: the bars are painted over with the button face around them, and the logo
# is taken from SteamVR's Quest 2 controller (oculus_quest2_controller_right, its Oculus button), at the same size
# relative to the button, in the bars' colour. Nothing else changes: same meshes, animations and specular map.
# Made on the headset from the installed SteamVR: none of Valve's files are in this repository.
# Pure Python (no PIL on Holo): PNG decode and encode with zlib.
import json, os, shutil, struct, sys, zlib

SRC_NAME, NAME = "oculus_quest_controller_right", "quest1_controller_right"
LOGO_NAME = "oculus_quest2_controller_right"


def png_read(path):
    d = open(path, "rb").read()
    assert d[:8] == b"\x89PNG\r\n\x1a\n", path
    pos, idat, w, plte = 8, [], None, None
    while pos < len(d):
        n, t = struct.unpack(">I4s", d[pos:pos + 8])
        body = d[pos + 8:pos + 8 + n]
        if t == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
            assert depth == 8 and ctype in (2, 3, 6) and not interlace, "8-bit RGB(A) or palette, not interlaced"
            bpp = {2: 3, 3: 1, 6: 4}[ctype]
        elif t == b"PLTE":
            plte = body
        elif t == b"IDAT":
            idat.append(body)
        pos += 12 + n
    raw = zlib.decompress(b"".join(idat))
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        cur = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f == 1:
            for i in range(bpp, stride):
                cur[i] = (cur[i] + cur[i - bpp]) & 255
        elif f == 2:
            cur = bytearray((a + b) & 255 for a, b in zip(cur, prev))
        elif f == 3:
            for i in range(stride):
                left = cur[i - bpp] if i >= bpp else 0
                cur[i] = (cur[i] + ((left + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = cur[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                cur[i] = (cur[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(cur)
        prev = cur
    if plte:  # palette images (the logo's source) come back as RGB
        rows = [bytearray(b"".join(plte[i * 3:i * 3 + 3] for i in r)) for r in rows]
        bpp = 3
    return w, h, bpp, rows


def png_write(path, w, h, bpp, rows):
    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xffffffff)
    raw = b"".join(b"\0" + bytes(r) for r in rows)
    hdr = struct.pack(">IIBBBBB", w, h, 8, 2 if bpp == 3 else 6, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", hdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def uv_box(obj):
    us, vs = [], []
    for line in open(obj):
        if line.startswith("vt "):
            u, v = map(float, line.split()[1:3])
            us.append(u)
            vs.append(v)
    return min(us), max(us), min(vs), max(vs)


def face_width(rows, bpp, x0, x1, cx, cy):
    """The button face's width along row cy, from the centre cx: lighter than the dark ring around it."""
    lum = lambda x: sum(rows[cy][x * bpp:x * bpp + 3]) / 3
    vals = sorted(lum(x) for x in range(x0, x1))
    dark, face = vals[0], [v for v in vals if v < 100][len([v for v in vals if v < 100]) // 2]
    thr = (dark + face) / 2
    a = b = cx
    while a > x0 and lum(a - 1) > thr:
        a -= 1
    while b < x1 - 1 and lum(b + 1) > thr:
        b += 1
    return b - a + 1


def redraw_button(w, h, bpp, rows, box, logo):
    u0, u1, v0, v1 = box
    x0, x1 = int(u0 * w), int(u1 * w) + 1
    y0, y1 = int((1 - v1) * h), int((1 - v0) * h) + 1
    px = lambda x, y: rows[y][x * bpp:x * bpp + 3]
    lum = lambda c: (c[0] + c[1] + c[2]) / 3
    # the glyph: the bright pixels on the button face
    bright = [(x, y) for y in range(y0, y1) for x in range(x0, x1) if lum(px(x, y)) > 100]
    assert len(bright) > 50, "no glyph found on the button"
    gx0, gx1 = min(x for x, _ in bright), max(x for x, _ in bright)
    gy0, gy1 = min(y for _, y in bright), max(y for _, y in bright)
    ink = [sum(px(x, y)[k] for x, y in bright) // len(bright) for k in range(3)]
    # the logo, scaled by the ratio of the two button faces' widths in their textures, centred on the glyph
    lface, alpha = logo
    cxf, cyf = (gx0 + gx1 + 1) / 2, (gy0 + gy1 + 1) / 2
    scale = face_width(rows, bpp, x0, x1, int(cxf), int(cyf)) / lface
    sw, sh = len(alpha[0]), len(alpha)
    # paint the glyph over, in a box holding the glyph and the logo (2 px margin): each pixel blends the face
    # colours at the box's edges
    m = 2
    bx0 = min(gx0, int(cxf - sw * scale / 2)) - m
    bx1 = max(gx1, int(cxf + sw * scale / 2) + 1) + m
    by0 = min(gy0, int(cyf - sh * scale / 2)) - m
    by1 = max(gy1, int(cyf + sh * scale / 2) + 1) + m
    left = {y: px(bx0 - 1, y) for y in range(by0, by1 + 1)}
    right = {y: px(bx1 + 1, y) for y in range(by0, by1 + 1)}
    top = {x: px(x, by0 - 1) for x in range(bx0, bx1 + 1)}
    bottom = {x: px(x, by1 + 1) for x in range(bx0, bx1 + 1)}
    face = {}
    for y in range(by0, by1 + 1):
        ty = (y - by0 + 1) / (by1 - by0 + 2)
        for x in range(bx0, bx1 + 1):
            tx = (x - bx0 + 1) / (bx1 - bx0 + 2)
            c = [((1 - tx) * left[y][k] + tx * right[y][k] + (1 - ty) * top[x][k] + ty * bottom[x][k]) / 2
                 for k in range(3)]
            face[x, y] = c
            rows[y][x * bpp:x * bpp + 3] = bytes(int(v + 0.5) for v in c)
    # the logo
    for y in range(by0, by1 + 1):
        for x in range(bx0, bx1 + 1):
            # this pixel's centre in the logo mask (bilinear)
            fx = (x + 0.5 - cxf) / scale + sw / 2 - 0.5
            fy = (y + 0.5 - cyf) / scale + sh / 2 - 0.5
            ix, iy = int(fx // 1), int(fy // 1)
            if ix < -1 or iy < -1 or ix >= sw or iy >= sh:
                continue
            tx, ty = fx - ix, fy - iy
            at = lambda i, j: alpha[j][i] if 0 <= i < sw and 0 <= j < sh else 0.0
            a = ((1 - tx) * (1 - ty) * at(ix, iy) + tx * (1 - ty) * at(ix + 1, iy) +
                 (1 - tx) * ty * at(ix, iy + 1) + tx * ty * at(ix + 1, iy + 1))
            if a > 0:
                c = face[x, y]
                rows[y][x * bpp:x * bpp + 3] = bytes(int(c[k] + (ink[k] - c[k]) * a + 0.5) for k in range(3))


def logo_mask(runtime):
    """The Quest 2 controller's Oculus logo: its coverage (0..1) per pixel, cropped to the logo."""
    d = os.path.join(runtime, "resources", "rendermodels", LOGO_NAME)
    w, h, bpp, rows = png_read(os.path.join(d, LOGO_NAME + ".png"))
    u0, u1, v0, v1 = uv_box(os.path.join(d, LOGO_NAME + "_button_menu.obj"))
    x0, x1, y0, y1 = int(u0 * w), int(u1 * w) + 1, int((1 - v1) * h), int((1 - v0) * h) + 1
    lum = lambda x, y: sum(rows[y][x * bpp:x * bpp + 3]) / 3
    # face and logo levels around the button's centre: the median (the face, finely striped) and the brightest
    cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
    r = (x1 - x0) // 4
    vals = sorted(lum(x, y) for y in range(cy - r, cy + r) for x in range(cx - r, cx + r))
    face, ink = vals[len(vals) // 2], vals[-len(vals) // 20]
    assert ink - face > 60, "no logo found on the Quest 2 button"
    cov = lambda v: 0.0 if v < 0.08 else min(v, 1.0)  # the face's stripes stay out
    a = [[cov((lum(x, y) - face) / (ink - face)) for x in range(cx - r, cx + r)]
         for y in range(cy - r, cy + r)]
    # crop to the logo, centred on its bounds
    ys = [j for j, row in enumerate(a) if max(row) > 0.05]
    xs = [i for i in range(2 * r) if max(row[i] for row in a) > 0.05]
    m = 1
    a = [row[xs[0] - m:xs[-1] + 1 + m] for row in a[ys[0] - m:ys[-1] + 1 + m]]
    return face_width(rows, bpp, x0, x1, cx, cy), a


def main():
    runtime, driver = sys.argv[1], sys.argv[2]
    src = os.path.join(runtime, "resources", "rendermodels", SRC_NAME)
    dst = os.path.join(driver, "resources", "rendermodels", NAME)
    tmp = dst + ".tmp"
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    for f in os.listdir(src):
        g = f.replace(SRC_NAME, NAME)
        shutil.copyfile(os.path.join(src, f), os.path.join(tmp, g))
    model = json.load(open(os.path.join(tmp, NAME + ".json")))
    model["thumbnail"] = NAME + "_thumbnail.png"
    json.dump(model, open(os.path.join(tmp, NAME + ".json"), "w"), indent=4)
    for f in os.listdir(tmp):  # the whole-controller mesh's material library is named after the model
        if f.endswith(".obj"):
            p = os.path.join(tmp, f)
            s = open(p).read()
            open(p, "w").write(s.replace("mtllib " + SRC_NAME + ".mtl", "mtllib " + NAME + ".mtl"))
    tex = os.path.join(tmp, "right_diff.png")
    w, h, bpp, rows = png_read(tex)
    redraw_button(w, h, bpp, rows, uv_box(os.path.join(tmp, "button_enter.obj")), logo_mask(runtime))
    png_write(tex, w, h, bpp, rows)
    shutil.rmtree(dst, ignore_errors=True)
    os.rename(tmp, dst)
    print("render model", dst)


if __name__ == "__main__":
    main()
