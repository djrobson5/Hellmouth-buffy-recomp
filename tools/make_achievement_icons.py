#!/usr/bin/env python3
"""The achievement icons, from the player's own disc: square crops of the level thumbnails and the launch screen.

    tools/make_achievement_icons.py <image.iso> [--out assets/achievements]

Writes <out>/<achievement id>.png. Launchers and the game draw them in a circle, so what matters sits in the middle. Some show
a boss, so nothing may show an icon before it's earned. To change a pick, edit PICKS: the thumbnails' pictures fill about 170 x
128 of each 256 x 128 texture, with a painted purple edge.
"""
import argparse
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(__file__))
from extract_assets import read_file
from make_icon import HEIGHT, LAUNCH_SCREEN, WIDTH, unswizzle

THUMBNAILS = "interfc/levelThumbnails.mtx"
LAUNCH = "launch"  # as a source in PICKS: the launch screen, 1024 x 512

# id: (source, centre x, centre y, size)
PICKS = {
    "ch_training": ("m00_spanishmission_3", 75, 50, 100),
    "boss_malik": ("m01_highschool_4", 120, 60, 100),
    "boss_spike": ("m02_bronze_4", 100, 70, 100),
    "boss_scylla": ("m03_cemetery_4", 120, 60, 100),
    "boss_matereani": ("m05_sunken_4", 60, 60, 100),
    "boss_scylla_snake": ("m06_mansion_2", 100, 60, 100),
    "boss_urd": ("m07_docks_5", 80, 70, 100),
    "boss_malik_tiger": ("m08_highschool_4", 85, 65, 100),
    "boss_angel": ("m09_foundry_5", 100, 60, 100),
    "boss_dark_buffy": ("m11_dreamrealm_3", 100, 60, 100),
    "boss_dreamers": ("m11_dreamrealm_4", 80, 60, 100),
    "boss_master": ("m12_apse", 95, 60, 100),
    "game_finished": ("m12_bronze-ending", 95, 60, 100),
    "game_hard": (LAUNCH, 395, 86, 132),  # the moon
    "kills_1": ("m00_spanishmission_1", 88, 50, 90),
    "kills_100": ("m03_cemetery_1", 100, 70, 100),
    "kills_250": ("m10_sunken-church_1", 80, 70, 90),
    "kills_500": (LAUNCH, 450, 140, 220),  # Buffy under the moon
    "secret_1": ("hub01", 90, 60, 100),
    "secret_level": ("m04_mausoleum_2", 88, 60, 100),
    "secret_30": ("m04_mausoleum_1", 85, 55, 100),
    "secret_all": ("hub01", 136, 68, 70),  # Giles
    "die_once": ("m03_cemetery_2", 110, 50, 100),
    "chapter_deathless": ("m10_sunken-church_0", 88, 70, 100),
    "history": ("m01_highschool_1", 100, 70, 100),
}


def thumbnail_table(mtx):
    """Name to data offset. A MULTITEX has a 0x80 byte record for each texture: its name at 0x3C, then 0x7C on its width,
    height, format, offset and size."""
    if mtx[:8] != b"MULTITEX":
        raise SystemExit(f"{THUMBNAILS} isn't a MULTITEX file")
    table = {}
    for i in range(struct.unpack_from("<I", mtx, 0x14)[0]):
        name = mtx[0x3C + i * 0x80 : 0x3C + i * 0x80 + 32].split(b"\0")[0].decode("latin1")
        fields = 0x7C + i * 0x80
        width, height = struct.unpack_from("<II", mtx, fields)
        fmt, offset, size = struct.unpack_from("<III", mtx, fields + 0x1C)
        if (width, height, fmt, size) != (256, 128, 5, 0x8000):
            raise SystemExit(f"{name} in {THUMBNAILS} isn't the expected 256 x 128 DXT5")
        table[name] = offset
    return table


def decode_dxt5(data):
    """A 256 x 128 DXT5 as rows of (r, g, b). The alpha block is skipped: the thumbnails don't use it."""
    rows = [[None] * 256 for _ in range(128)]

    def rgb(v):
        return (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)

    at = 8
    for y in range(0, 128, 4):
        for x in range(0, 256, 4):
            c0, c1, bits = struct.unpack_from("<HHI", data, at)
            at += 16
            a, b = rgb(c0), rgb(c1)
            colours = (a, b, tuple((2 * i + j) // 3 for i, j in zip(a, b)), tuple((i + 2 * j) // 3 for i, j in zip(a, b)))
            for j in range(4):
                for i in range(4):
                    rows[y + j][x + i] = colours[(bits >> (2 * (i + 4 * j))) & 3]
    return rows


def decode_launch(data):
    pixels = unswizzle(data)  # blue, green, red, unused
    return [[(pixels[at + 2], pixels[at + 1], pixels[at]) for at in range(y * WIDTH * 4, (y + 1) * WIDTH * 4, 4)]
            for y in range(HEIGHT)]


def write_png(path, rows):
    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body))

    raw = b"".join(b"\0" + bytes(value for pixel in row for value in pixel) for row in rows)
    with open(path, "wb") as out:
        out.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", len(rows[0]), len(rows), 8, 2, 0, 0, 0))
                  + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--out", default="assets/achievements")
    args = ap.parse_args()
    mtx = read_file(args.image, THUMBNAILS)
    offsets = thumbnail_table(mtx)
    decoded = {}
    for source, _, _, _ in PICKS.values():
        if source in decoded:
            continue
        if source == LAUNCH:
            decoded[source] = decode_launch(read_file(args.image, LAUNCH_SCREEN))
        elif source in offsets:
            decoded[source] = decode_dxt5(mtx[offsets[source] : offsets[source] + 0x8000])
        else:
            raise SystemExit(f"{source} isn't in {THUMBNAILS}")
    os.makedirs(args.out, exist_ok=True)
    for name, (source, cx, cy, size) in PICKS.items():
        rows = decoded[source]
        left, top = cx - size // 2, cy - size // 2
        if left < 0 or top < 0 or top + size > len(rows) or left + size > len(rows[0]):
            raise SystemExit(f"{name}'s crop doesn't fit in {source}")
        write_png(os.path.join(args.out, f"{name}.png"), [row[left : left + size] for row in rows[top : top + size]])
    print(f"{args.out}: {len(PICKS)} icons")


if __name__ == "__main__":
    main()
