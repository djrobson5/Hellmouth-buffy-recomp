#!/usr/bin/env python3
"""Checks the structure of an .ico file and lists what is in it: the header, a directory entry for each image, each image inside the
file, and that an image is a PNG or a 32 bit DIB with the size its entry says. Needs only the standard library.

    tools/check_ico.py <file.ico> [size ...]     (with sizes: those, in that order, must be the images)

Used by tools/check_config_tool.sh and tools/sim_first_start.sh on the icon buffy_config --pictures writes, as nothing here can
load it the way Windows does. Exit 0 when it is sound, 1 when not.
"""
import struct
import sys


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    with open(sys.argv[1], "rb") as f:
        data = f.read()
    problems, found = [], []
    reserved, kind, count = struct.unpack_from("<HHH", data, 0)
    if (reserved, kind) != (0, 1):
        problems.append("not an icon file header")
    end = 6 + 16 * count
    for i in range(count):
        w, h, colours, zero, planes, bits, size, offset = struct.unpack_from("<BBBBHHII", data, 6 + 16 * i)
        w, h = w or 256, h or 256
        found.append(w)
        if offset < end or offset + size > len(data):
            problems.append("image %d lies outside the file" % i)
            continue
        image = data[offset:offset + size]
        if image[:8] == b"\x89PNG\r\n\x1a\n":
            pw, ph = struct.unpack_from(">II", image, 16)
            kindname = "PNG"
            if (pw, ph) != (w, h):
                problems.append("image %d: the entry says %dx%d, the PNG %dx%d" % (i, w, h, pw, ph))
        else:
            header, dw, dh, dplanes, dbits, compression = struct.unpack_from("<IiiHHI", image, 0)
            kindname = "DIB"
            mask = ((w + 31) // 32) * 4 * h
            if (header, dw, dh, dplanes, dbits, compression) != (40, w, 2 * h, 1, 32, 0):
                problems.append("image %d: not a 32 bit DIB of %dx%d" % (i, w, h))
            if size != 40 + w * h * 4 + mask:
                problems.append("image %d: the DIB is %d bytes, not %d" % (i, size, 40 + w * h * 4 + mask))
        print("  %3dx%-3d %s, %d bytes at %d (%d bits, %d plane)" % (w, h, kindname, size, offset, bits, planes))
    if len(sys.argv) > 2 and found != [int(s) for s in sys.argv[2:]]:
        problems.append("the sizes are %s, not %s" % (found, sys.argv[2:]))
    for p in problems:
        print("  problem: " + p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
