#!/usr/bin/env python3
"""Draws Buffy.exe's own icon, the one Explorer shows for the program: a plain letter H (for Hellmouth) in white on dark purple.

    tools/make_exe_icon.py [--out resources/exe_icon.ico]

It is an original design, drawn here from rectangles; nothing in it comes from the game, its disc or its art, so it ships with the
release (the pictures made from the player's disc, Buffy.ico, are made at the first start and set on the window at run time instead).
CMake puts the file in the executable as its icon resource (XBE_ICON, root CMakeLists.txt). Standard library only; the output is the
same bytes every time. Run it again only to change the design, and commit the .ico.
"""
import argparse
import os
import struct
import zlib

SIZES = (256, 48, 32, 16)  # 256 as a PNG, the others as 32 bit DIBs, as Windows reads them all
SAMPLES = 4                # per pixel, each way

TOP, BOTTOM = (64, 28, 98), (24, 10, 42)  # the background's gradient, top to bottom
RING = (122, 78, 170)                     # the thin frame inside it
WHITE = (244, 240, 250)
# the letter, in units of the icon's side: two bars and a crossbar (x0, y0, x1, y1)
LETTER = ((0.25, 0.22, 0.375, 0.78), (0.625, 0.22, 0.75, 0.78), (0.375, 0.455, 0.625, 0.545))
FRAME = 0.04, 0.02  # the frame's distance from the edge, and its width


def sample(x, y):
    """The colour at a point (0 to 1 each way)."""
    for x0, y0, x1, y1 in LETTER:
        if x0 <= x <= x1 and y0 <= y <= y1:
            return WHITE
    inset, width = FRAME
    inside = min(x, 1 - x, y, 1 - y)
    if inset <= inside < inset + width:
        return RING
    return tuple(round(TOP[i] + (BOTTOM[i] - TOP[i]) * y) for i in range(3))


def render(size):
    """size x size rows of (r, g, b, a), each pixel the average of SAMPLES x SAMPLES points."""
    rows = []
    for py in range(size):
        row = []
        for px in range(size):
            total = [0, 0, 0]
            for sy in range(SAMPLES):
                for sx in range(SAMPLES):
                    c = sample((px + (sx + 0.5) / SAMPLES) / size, (py + (sy + 0.5) / SAMPLES) / size)
                    for i in range(3):
                        total[i] += c[i]
            n = SAMPLES * SAMPLES
            row.append(tuple((t + n // 2) // n for t in total) + (255,))
        rows.append(row)
    return rows


def png(rows):
    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body))
    raw = b"".join(b"\0" + bytes(v for p in row for v in p) for row in rows)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", len(rows[0]), len(rows), 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def dib(rows):
    """A 32 bit icon image: the header (the height counts the mask too), the pixels bottom up as blue green red alpha, the empty mask."""
    size = len(rows)
    out = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    for row in reversed(rows):
        out += b"".join(bytes((p[2], p[1], p[0], p[3])) for p in row)
    return out + bytes(((size + 31) // 32) * 4 * size)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "resources", "exe_icon.ico"))
    args = ap.parse_args()
    images = [(size, png(render(size)) if size >= 256 else dib(render(size))) for size in SIZES]
    data = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for size, image in images:
        data += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(image), offset)
        offset += len(image)
    data += b"".join(image for _, image in images)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(data)
    print("%s: %d bytes, sizes %s" % (os.path.normpath(args.out), len(data), ", ".join(str(s) for s in SIZES)))


if __name__ == "__main__":
    main()
