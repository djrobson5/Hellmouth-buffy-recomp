#!/usr/bin/env python3
"""Compares the pixels of the PNG files of two folders: the same names, the same sizes, every pixel equal. Needs only the standard library.

    tools/compare_pngs.py <folder a> <folder b>

tools/check_config_tool.sh and tools/sim_first_start.sh use it on the achievement pictures: the ones buffy_config --pictures writes (stb's
encoder, filters chosen for each row) against make_achievement_icons.py's (zlib, no filter), which differ in their bytes and must not in
their pixels. Reads 8 bit RGB and RGBA files, which is what both write. Exit 0 when all are equal, 1 when not, 2 for a file it cannot read.
"""
import os
import struct
import sys
import zlib


def read_png(path):
    """(width, height, channels, rows of bytes) of an 8 bit RGB or RGBA PNG, each row unfiltered."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    at, idat, header = 8, [], None
    while at < len(data):
        length, tag = struct.unpack_from(">I4s", data, at)
        body = data[at + 8:at + 8 + length]
        at += 12 + length
        if tag == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif tag == b"IDAT":
            idat.append(body)
        elif tag == b"IEND":
            break
    width, height, depth, kind, _, _, interlace = header
    if depth != 8 or kind not in (2, 6) or interlace:
        raise ValueError("not an 8 bit RGB or RGBA PNG without interlacing")
    channels = 3 if kind == 2 else 4
    raw = zlib.decompress(b"".join(idat))
    stride = width * channels
    rows, previous = [], bytearray(stride)
    for y in range(height):
        base = y * (stride + 1)
        method = raw[base]
        line = bytearray(raw[base + 1:base + 1 + stride])
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = previous[i]
            c = previous[i - channels] if i >= channels else 0
            if method == 1:
                line[i] = (line[i] + a) & 255
            elif method == 2:
                line[i] = (line[i] + b) & 255
            elif method == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif method == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
            elif method != 0:
                raise ValueError("unknown filter %d" % method)
        rows.append(bytes(line))
        previous = line
    return width, height, channels, rows


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    a, b = sys.argv[1], sys.argv[2]
    names_a = sorted(n for n in os.listdir(a) if n.endswith(".png"))
    names_b = sorted(n for n in os.listdir(b) if n.endswith(".png"))
    if names_a != names_b:
        print("different files: only in %s: %s; only in %s: %s" % (a, sorted(set(names_a) - set(names_b)), b, sorted(set(names_b) - set(names_a))))
        return 1
    bad = 0
    for n in names_a:
        try:
            pa, pb = read_png(os.path.join(a, n)), read_png(os.path.join(b, n))
        except (ValueError, OSError, zlib.error, struct.error) as e:
            print("cannot read %s: %s" % (n, e))
            return 2
        if pa != pb:
            print("differs: %s (%dx%d against %dx%d)" % (n, pa[0], pa[1], pb[0], pb[1]))
            bad += 1
    print("%d pictures compared: %d differ" % (len(names_a), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
