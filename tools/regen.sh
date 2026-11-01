#!/bin/sh
# From the disc image to generated/game: the tools, assets/, config/game.toml, the recompile, the checks.
#   tools/regen.sh <image.iso>    the image is only needed while assets/ is missing
set -e
cd "$(dirname "$0")/.."
if [ ! -f assets/Default.xbe ] || [ ! -f assets/final.map ]; then
    [ -n "$1" ] || { echo "usage: tools/regen.sh <image.iso>"; exit 2; }
    python3 tools/extract_assets.py "$1"
fi
if [ ! -f assets/Buffy.icns ] && [ -n "$1" ]; then python3 tools/make_icon.py "$1"; fi
if [ ! -d assets/achievements ] && [ -n "$1" ]; then python3 tools/make_achievement_icons.py "$1"; fi
if [ ! -x build/tools/xbe_recomp ]; then
    cmake -S tools -B build/tools -DCMAKE_POLICY_VERSION_MINIMUM=3.5 > /dev/null
    cmake --build build/tools -j"$(sysctl -n hw.ncpu)" --target xbe_recomp xbe_symbols > /dev/null
fi
mkdir -p config build
python3 tools/make_config.py
if ! build/tools/xbe_recomp config/game.toml --xdk sdk/xdk > build/recomp.log 2>&1; then
    cat build/recomp.log
    exit 1
fi
grep -E '^total:' build/recomp.log
if grep -q '^unimplemented' build/recomp.log; then
    echo "instructions the recompiler can't translate:"
    grep '^unimplemented' build/recomp.log
fi
python3 tools/check_layout.py
