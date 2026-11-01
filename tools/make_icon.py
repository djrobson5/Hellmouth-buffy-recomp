#!/usr/bin/env python3
"""The Dock icon, from the player's own disc: the moon and name over the purple sky, as on the game's Xbox dashboard icon.

    tools/make_icon.py <image.iso> [--out assets]

Writes <out>/Buffy.png and <out>/Buffy.icns. The dashboard icon itself is 128x128, too small for a Mac, so it's rebuilt from
the disc's launch screen (media/launch_buffy.xpr), which has the same pieces at 1024x512. It needs ffmpeg; sips and iconutil
come with macOS.
"""
import argparse
import array
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(__file__))
from extract_assets import read_file

LAUNCH_SCREEN = "media/launch_buffy.xpr"
WIDTH, HEIGHT, DATA = 1024, 512, 0x800  # a swizzled X8R8G8B8 texture, its pixels after the file's header

# Apple's grid: a shape of 824 in the 1024 canvas, with corners of 185
SIZE, SHAPE, CORNER = 1024, 824, 185

# the launch screen's pieces, as (width, height, x, y) there
SKY = 170, 170, 45, 165
MOON = 122, 122, 334, 26
LOGO = 216, 134, 28, 24
# and where they go in the shape
SKY_BRIGHTNESS = 0.55
MOON_SIZE, MOON_X, MOON_Y = 520, 152, 50
LOGO_WIDTH, LOGO_X, LOGO_Y = 770, 27, 300
LOGO_SHADOW = 16


def unswizzle(data):
    """Morton order over the square part, with the extra bits of the longer side (here x) above it."""
    def spread(n):
        return sum(((n >> bit) & 1) << (2 * bit) for bit in range(9))
    xs = [spread(x & 511) | ((x >> 9) << 18) for x in range(WIDTH)]
    ys = [spread(y) << 1 for y in range(HEIGHT)]
    swizzled = array.array("I")
    swizzled.frombytes(data[DATA : DATA + WIDTH * HEIGHT * 4])
    linear = array.array("I", bytes(WIDTH * HEIGHT * 4))
    at = 0
    for y in ys:
        for x in xs:
            linear[at] = swizzled[x + y]
            at += 1
    return linear.tobytes()


def ffmpeg_filter():
    margin = (SIZE - SHAPE) // 2
    half = SIZE / 2
    inner = SHAPE / 2 - CORNER
    dx, dy = f"max(abs(X-{half})-{inner},0)", f"max(abs(Y-{half})-{inner},0)"
    moon_r = MOON_SIZE / 2
    logo_h = LOGO_WIDTH * LOGO[1] // LOGO[0]
    return "".join([
        "[0]format=gbrap,split=3[a][b][c];",
        f"[a]crop={SKY[0]}:{SKY[1]}:{SKY[2]}:{SKY[3]},scale={SHAPE}:{SHAPE}:flags=bicubic,gblur=sigma=8,",
        f"geq=r='r(X,Y)*{SKY_BRIGHTNESS}':g='g(X,Y)*{SKY_BRIGHTNESS}':b='b(X,Y)*{SKY_BRIGHTNESS}':a=255[sky];",
        # the moon, cut out as a circle
        f"[b]crop={MOON[0]}:{MOON[1]}:{MOON[2]}:{MOON[3]},scale={MOON_SIZE}:{MOON_SIZE}:flags=lanczos,",
        f"geq=r='r(X,Y)':g='g(X,Y)':b='b(X,Y)':a='255*clip(({moon_r}-3-hypot(X-{moon_r},Y-{moon_r}))/4,0,1)'[moon];",
        # the name: near white, where the sky behind it has little green, so green tells them apart
        f"[c]crop={LOGO[0]}:{LOGO[1]}:{LOGO[2]}:{LOGO[3]},scale={LOGO_WIDTH}:{logo_h}:flags=lanczos,",
        "geq=r='r(X,Y)':g='g(X,Y)':b='b(X,Y)':a='255*clip((g(X,Y)-70)/50,0,1)',split[name][dark];",
        f"[dark]geq=r=0:g=0:b=0:a='alpha(X,Y)',pad={LOGO_WIDTH + 80}:{logo_h + 80}:40:40:color=black@0,gblur=sigma={LOGO_SHADOW}:planes=8[shadow];",
        f"[sky][moon]overlay={MOON_X}:{MOON_Y}:format=gbrp[m];",
        f"[m][shadow]overlay={LOGO_X - 40}:{LOGO_Y - 40}:format=gbrp[s];",
        f"[s][name]overlay={LOGO_X}:{LOGO_Y}:format=gbrp,",
        f"format=rgba,pad={SIZE}:{SIZE}:{margin}:{margin}:color=black@0,",
        # the corners, with a pixel of soft edge
        f"geq=r='r(X,Y)':g='g(X,Y)':b='b(X,Y)':a='255*clip({CORNER}+0.5-hypot({dx},{dy}),0,1)'",
    ])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--out", default="assets")
    args = ap.parse_args()
    if not shutil.which("ffmpeg"):
        raise SystemExit("this needs ffmpeg: brew install ffmpeg")
    os.makedirs(args.out, exist_ok=True)
    png = os.path.join(args.out, "Buffy.png")
    with tempfile.TemporaryDirectory() as tmp:
        raw = os.path.join(tmp, "launch.bgr0")
        with open(raw, "wb") as out:
            out.write(unswizzle(read_file(args.image, LAUNCH_SCREEN)))
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pixel_format", "bgr0", "-video_size", f"{WIDTH}x{HEIGHT}",
                        "-i", raw, "-filter_complex", ffmpeg_filter(), "-update", "1", "-frames:v", "1", png], check=True)
        iconset = os.path.join(tmp, "Buffy.iconset")
        os.mkdir(iconset)
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                name = f"icon_{size}x{size}{'@2x' if scale == 2 else ''}.png"
                subprocess.run(["sips", "-z", str(size * scale), str(size * scale), png, "--out", os.path.join(iconset, name)],
                               check=True, stdout=subprocess.DEVNULL)
        subprocess.run(["iconutil", "-c", "icns", iconset, "-o", os.path.join(args.out, "Buffy.icns")], check=True)
    print(f"{png} and Buffy.icns")


if __name__ == "__main__":
    main()
