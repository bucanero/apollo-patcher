#!/usr/bin/env python3
"""
Bake the code-type icons into a C header.

The GUI's type column shows one small picture per interpreter — Save Wizard,
Python, BSD. A font cannot supply them: the atlas is Noto Sans JP over a fixed
set of ranges and carries no pictographs, ImWchar is 16 bits in that build so
an emoji codepoint cannot even be indexed, and colour emoji would want
FreeType, which the build does not use.

So they are images. This reads the three PNGs, decodes them, box-filters them
down to the size the column actually draws, deflates the result and writes
gui/src/type_icons.h — exactly the recipe src/icon_rgba_z.h already uses for
the app icon, so the binary stays self-contained and nothing has to be found
beside the executable at run time.

    python3 tools/make-type-icons.py gui/assets/icons gui/src/type_icons.h

Decoding is done here rather than in the app because the app already has
core/png.c and does not need a second path, and rather than with Pillow
because nothing else in tools/ needs a dependency. The subset below is the
one the input spec allows: 8-bit RGBA or RGB, non-interlaced. Anything else
is refused by name rather than guessed at.

WHY A BOX FILTER AND NOT THE GPU. The floor this app targets is OpenGL 1.1,
which has no automatic mipmap generation, so a 128px source handed to
GL_LINEAR and drawn at 20px shimmers as it is sampled. Downsampling once here,
with every source pixel contributing, is both better looking and free at run
time.
"""
import os
import struct
import sys
import zlib

# How tall the art is stored. The column draws one line of text, 20px; this is
# doubled so a HiDPI display has real pixels rather than a smeared upscale.
#
# WIDTH IS NOT FIXED. Art is scaled to this height and keeps its aspect, so a
# wide mark stays wide and is drawn wider in the column rather than squashed
# into a square and left a third the height of everything else.
OUT_H = 40

# ...but the TEXTURE it lands in is padded to powers of two, because OpenGL
# 1.1 takes nothing else, and 1.1 is this app's floor -- it is exactly what
# Microsoft's software renderer offers on a GPU-less or Remote Desktop host.
# The app draws the art back out with UVs that cut the padding off, the same
# thing core/png.c's callers do for save icons. Without this the icons would
# silently fail to appear on precisely the machines least able to say why.
def next_pot(n):
    p = 1
    while p < n:
        p *= 2
    return p

ICONS = [
    ("SAVEWIZARD", "type-savewizard.png"),
    ("PYTHON",     "type-python.png"),
    ("BSD",        "type-bsd.png"),
]


def die(msg):
    sys.exit("make-type-icons.py: " + msg)


def decode_png(path):
    """8-bit RGB/RGBA, non-interlaced -> (w, h, bytearray RGBA)."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        die(f"{path} is not a PNG")

    pos, idat, w = 8, bytearray(), None
    plte, trns = b"", b""
    while pos < len(data):
        (length,) = struct.unpack_from(">I", data, pos)
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            w, h, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8:
                die(f"{path} is {depth}-bit; this wants 8 bits a channel")
            if colour not in (2, 3, 6):
                die(f"{path} has colour type {colour}; this wants RGB (2), "
                    f"palette (3) or RGBA (6)")
            if interlace:
                die(f"{path} is interlaced; save it without Adam7")
        elif kind == b"PLTE":
            plte = body
        elif kind == b"tRNS":
            trns = body
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
        pos += 12 + length

    if w is None:
        die(f"{path} has no IHDR")

    # A palette image stores one index a pixel; the colour comes out of PLTE
    # and the alpha, when there is any, out of tRNS. Exporters reach for this
    # format on their own for flat artwork, so refusing it would refuse a
    # perfectly ordinary icon.
    if colour == 3 and not plte:
        die(f"{path} is a palette image with no PLTE chunk")
    channels = {2: 3, 3: 1, 6: 4}[colour]
    raw = zlib.decompress(bytes(idat))
    stride = w * channels
    out = bytearray(w * h * 4)
    prev = bytearray(stride)

    # The five PNG row filters. Nothing exotic, but they have to be undone in
    # order because each row is predicted from the one above it.
    k = 0
    for y in range(h):
        ft = raw[k]; k += 1
        row = bytearray(raw[k:k + stride]); k += stride
        for i in range(stride):
            a = row[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if   ft == 1: row[i] = (row[i] + a) & 0xFF
            elif ft == 2: row[i] = (row[i] + b) & 0xFF
            elif ft == 3: row[i] = (row[i] + ((a + b) >> 1)) & 0xFF
            elif ft == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                row[i] = (row[i] + pr) & 0xFF
            elif ft != 0:
                die(f"{path} row {y} uses filter {ft}")
        for x in range(w):
            d = (y * w + x) * 4
            if colour == 3:
                idx = row[x]
                out[d:d + 3] = plte[idx * 3:idx * 3 + 3]
                out[d + 3] = trns[idx] if idx < len(trns) else 255
            else:
                s = x * channels
                out[d:d + 3] = row[s:s + 3]
                out[d + 3] = row[s + 3] if channels == 4 else 255
        prev = row

    return w, h, out


def box_down(w, h, rgba, out_w, out_h, pad_w, pad_h):
    """Scale to out_w x out_h, into the top-left of a pad_w x pad_h buffer.

    Averages every source pixel that lands in each destination pixel.

    Premultiplied by alpha, because averaging the colour of a transparent
    pixel with an opaque one otherwise drags the edge towards whatever colour
    the transparent pixels happen to carry -- usually black, which is how an
    icon ends up with a dark halo.
    """
    out = bytearray(pad_w * pad_h * 4)

    for dy in range(out_h):
        y0, y1 = dy * h // out_h, max(dy * h // out_h + 1, (dy + 1) * h // out_h)
        for dx in range(out_w):
            x0, x1 = dx * w // out_w, max(dx * w // out_w + 1, (dx + 1) * w // out_w)
            r = g = b = a = n = 0
            for sy in range(y0, y1):
                for sx in range(x0, x1):
                    s = (sy * w + sx) * 4
                    av = rgba[s + 3]
                    r += rgba[s] * av; g += rgba[s + 1] * av; b += rgba[s + 2] * av
                    a += av; n += 1
            d = (dy * pad_w + dx) * 4
            if a:
                out[d] = min(255, r // a); out[d + 1] = min(255, g // a)
                out[d + 2] = min(255, b // a)
            out[d + 3] = a // n
    return out


def main(argv):
    if len(argv) != 3:
        sys.exit("usage: make-type-icons.py ASSET_DIR OUTPUT.h")
    src_dir, dst = argv[1], argv[2]

    blobs, missing = [], []
    for name, filename in ICONS:
        path = os.path.join(src_dir, filename)
        if not os.path.isfile(path):
            missing.append(filename)
            continue
        w, h, rgba = decode_png(path)
        if h < OUT_H:
            die(f"{path} is {w}x{h}; it wants to be at least {OUT_H}px tall")

        # Scale to the stored height, keep the aspect, then pad to powers of
        # two for the texture itself.
        out_h = OUT_H
        out_w = max(1, round(w * OUT_H / h))
        pad_w, pad_h = next_pot(out_w), next_pot(out_h)
        small = box_down(w, h, rgba, out_w, out_h, pad_w, pad_h)

        z = zlib.compress(bytes(small), 9)
        blobs.append((name, filename, w, h, out_w, out_h, pad_w, pad_h, z))
        print(f"  {filename:22} {w}x{h} -> {out_w}x{out_h} art "
              f"in a {pad_w}x{pad_h} texture, {len(z)} bytes deflated")

    # Always write the header, even with nothing to put in it.
    #
    # It used to be written only when all three images were present, and that
    # is a trap: main.cpp reaches it through __has_include, so a compile that
    # happened while it did not exist recorded no dependency on it, and
    # dropping the art in afterwards rebuilt nothing. The app looked exactly
    # as before and no part of the build said why. A header that is always
    # there is always a dependency.
    if missing:
        with open(dst, "w") as fh:
            fh.write("/* Generated by tools/make-type-icons.py -- do not edit.\n"
                     " *\n"
                     " * No code-type icons were found, so none are baked in and\n"
                     " * APOLLO_HAVE_TYPE_ICONS is not defined. The type column falls\n"
                     " * back to its coloured letters. Missing:\n"
                     + "".join(f" *   {m}\n" for m in missing) +
                     " */\n"
                     "#ifndef APOLLO_TYPE_ICONS_H\n#define APOLLO_TYPE_ICONS_H\n"
                     "#endif /* APOLLO_TYPE_ICONS_H */\n")
        print("no icons found (" + ", ".join(missing) + "); wrote an empty " + dst)
        return

    with open(dst, "w") as fh:
        fh.write("/* Generated by tools/make-type-icons.py -- do not edit.\n"
                 " *\n"
                 " * The code-type icons, decoded to RGBA and zlib-deflated. Inflated and\n"
                 " * uploaded once at startup; see draw_type_icon() in main.cpp.\n"
                 " */\n"
                 "#ifndef APOLLO_TYPE_ICONS_H\n#define APOLLO_TYPE_ICONS_H\n\n")
        fh.write("#define APOLLO_HAVE_TYPE_ICONS 1\n\n")
        fh.write("/* Art size, texture size (both powers of two, for OpenGL\n"
                 " * 1.1) and the UV the art ends at inside it. */\n")
        fh.write("typedef struct {\n"
                 "    int            art_w, art_h;   /* pixels of real art   */\n"
                 "    int            tex_w, tex_h;   /* the padded texture   */\n"
                 "    unsigned       rgba_bytes;     /* tex_w * tex_h * 4    */\n"
                 "    const unsigned char *z;\n"
                 "    unsigned       z_len;\n"
                 "} type_icon_t;\n\n")
        for name, filename, w, h, ow, oh, pw, ph, z in blobs:
            fh.write(f"/* {filename}, from {w}x{h} */\n")
            fh.write(f"static const unsigned char type_icon_{name.lower()}_z[] = {{\n")
            for i in range(0, len(z), 16):
                fh.write("    " + ",".join(f"0x{b:02x}" for b in z[i:i + 16]) + ",\n")
            fh.write("};\n\n")
        fh.write("static const type_icon_t TYPE_ICONS[] = {\n")
        for name, filename, w, h, ow, oh, pw, ph, z in blobs:
            fh.write(f"    {{ {ow}, {oh}, {pw}, {ph}, {pw * ph * 4}u, "
                     f"type_icon_{name.lower()}_z, {len(z)}u }},  /* {name} */\n")
        fh.write("};\n\n")
        fh.write("#endif /* APOLLO_TYPE_ICONS_H */\n")
    print(f"wrote {dst}")


if __name__ == "__main__":
    main(sys.argv)
