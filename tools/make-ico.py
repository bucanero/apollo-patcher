#!/usr/bin/env python3
"""Pack square PNGs into a Windows .ico, each stored as PNG.

    tools/make-ico.py out.ico icon-16.png icon-32.png ... icon-256.png

PNG-compressed entries are what Windows has read since Vista, and what keeps a
256px icon from costing a quarter of a megabyte. The sizes are read out of each
PNG's own header, so the inputs only have to be square and at most 256px; make
them with any resizer -- `sips -z N N icon.png --out icon-N.png` on macOS.

Used for gui/assets/icon.ico, which the Windows executable and its installer
carry. Standard library only, like the other tools here.
"""
import struct
import sys


def png_size(data, name):
    if data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        sys.exit(f"{name}: not a PNG")
    w, h = struct.unpack(">II", data[16:24])
    if w != h or w > 256:
        sys.exit(f"{name}: {w}x{h} -- an icon entry must be square, at most 256px")
    return w


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    out, paths = argv[1], argv[2:]

    entries = []
    for p in paths:
        with open(p, "rb") as f:
            data = f.read()
        entries.append((png_size(data, p), data))
    entries.sort(key=lambda e: e[0])

    header = struct.pack("<HHH", 0, 1, len(entries))
    offset = len(header) + 16 * len(entries)
    directory, blobs = b"", b""
    for size, data in entries:
        # 256 is written as 0: the field is one byte.
        directory += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0,
                                 1, 32, len(data), offset)
        blobs += data
        offset += len(data)

    with open(out, "wb") as f:
        f.write(header + directory + blobs)


if __name__ == "__main__":
    main(sys.argv)
