#!/usr/bin/env python3
"""Makes the Mac app's own icon, AppIcon.icns: the plain letter H of Buffy.exe's icon (tools/make_exe_icon.py draws it), at the sizes a Mac
app has. Nothing in it comes from the game, its disc or its art, so it ships. The game's own icon is made from the player's disc at the first
start (Buffy.png in the data folder) and set on the Dock at run time, over this one.

    tools/make_app_icon.py <out.icns>

Standard library, and iconutil (part of macOS). The drawing is the .ico's, so the two stay the same design; the sizes above 256 are drawn
with 2 x 2 samples a pixel (the shapes are rectangles), the smaller ones as the .ico's are.
"""
import importlib.util
import os
import subprocess
import sys
import tempfile

# the iconset's files: name -> the pixel size (a @2x file is twice the size in its name)
FILES = {
    "icon_16x16.png": 16, "icon_16x16@2x.png": 32, "icon_32x32.png": 32, "icon_32x32@2x.png": 64,
    "icon_128x128.png": 128, "icon_128x128@2x.png": 256, "icon_256x256.png": 256, "icon_256x256@2x.png": 512,
    "icon_512x512.png": 512, "icon_512x512@2x.png": 1024,
}


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    here = os.path.dirname(os.path.abspath(__file__))
    spec = importlib.util.spec_from_file_location("make_exe_icon", os.path.join(here, "make_exe_icon.py"))
    draw = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(draw)
    images = {}
    for size in sorted(set(FILES.values())):
        draw.SAMPLES = 4 if size <= 256 else 2
        images[size] = draw.png(draw.render(size))
    out = os.path.abspath(sys.argv[1])
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        iconset = os.path.join(tmp, "AppIcon.iconset")
        os.mkdir(iconset)
        for name, size in FILES.items():
            with open(os.path.join(iconset, name), "wb") as f:
                f.write(images[size])
        subprocess.run(["iconutil", "-c", "icns", iconset, "-o", out], check=True)
    print("%s: %d bytes, sizes %s" % (out, os.path.getsize(out), ", ".join(str(s) for s in sorted(images))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
