#!/usr/bin/env python3
"""Copies Default.xbe and final.map from the player's disc image into assets/, reading the image only.

    extract_assets.py <image.iso> [--out assets]
"""
import argparse
import os
import struct

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# where the game partition starts in a whole disc dump (XGD1, XGD2, XGD3), or 0 for the partition alone
PARTITIONS = (0, 0x18300000, 0x0FD90000, 0x02080000)
WANTED = {"default.xbe": "Default.xbe", "final.map": "final.map"}


def find_partition(f):
    for offset in PARTITIONS:
        f.seek(offset + 32 * SECTOR)
        if f.read(len(MAGIC)) == MAGIC:
            return offset
    raise SystemExit("not an Xbox disc image: no XDVDFS header found")


def walk_dir(f, base, dir_sector, dir_size):
    """A directory as (name, sector, size, attributes). The directory is a binary tree of 4-byte aligned entries."""
    f.seek(base + dir_sector * SECTOR)
    table = f.read(dir_size)
    found = []

    def node(index):
        at = index * 4
        if at + 14 > len(table):
            return
        left, right, sector, size, attr, name_len = struct.unpack_from("<HHIIBB", table, at)
        if left == 0xFFFF and right == 0xFFFF:
            return
        if left:
            node(left)
        found.append((table[at + 14 : at + 14 + name_len].decode("latin1"), sector, size, attr))
        if right:
            node(right)

    node(0)
    return found


def root_dir(f, base):
    f.seek(base + 32 * SECTOR)
    root_sector, root_size = struct.unpack_from("<II", f.read(SECTOR), 20)
    return walk_dir(f, base, root_sector, root_size)


def walk_root(f, base):
    """The root directory's files as (name, sector, size)."""
    return [(name, sector, size) for name, sector, size, attr in root_dir(f, base) if not attr & 0x10]


def read_file(image, path):
    """One file's bytes from the disc, by its path ("interfc/attract.wmv")."""
    with open(image, "rb") as f:
        base = find_partition(f)
        entries = root_dir(f, base)
        parts = path.lower().split("/")
        for depth, part in enumerate(parts):
            entry = next((e for e in entries if e[0].lower() == part), None)
            if entry is None:
                raise SystemExit(f"{path} isn't on this disc")
            _, sector, size, _ = entry
            if depth + 1 < len(parts):
                entries = walk_dir(f, base, sector, size)
        f.seek(base + sector * SECTOR)
        return f.read(size)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--out", default="assets")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    with open(args.image, "rb") as f:
        base = find_partition(f)
        got = 0
        for name, sector, size in walk_root(f, base):
            target = WANTED.get(name.lower())
            if not target:
                continue
            f.seek(base + sector * SECTOR)
            with open(os.path.join(args.out, target), "wb") as out:
                out.write(f.read(size))
            print(f"{target}: {size} bytes")
            got += 1
    if got != len(WANTED):
        raise SystemExit(f"found {got} of {len(WANTED)} files: this isn't the expected release")


if __name__ == "__main__":
    main()
