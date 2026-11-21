#!/bin/bash
# Golden test of tools/buffy_config (C++, what the player's PC runs) against tools/make_config.py (what the Mac flow runs): built
# natively here, run on the local assets (assets/final.map and assets/Default.xbe, from the disc), both outputs written under build/
# (the config is game derived, so no expected file is kept), and compared byte for byte. The list of the SDK's names the tool binds
# is compared too: the C++ scan against make_config.py's sdk_names.
# And its --pictures (the achievement pictures and the icon the first start makes from the disc, in place of
# tools/make_achievement_icons.py and tools/make_icon.py): the two disc files read from the disc image (DISC=<image>, else the one in
# "Buffy the Vampire Slayer (USA)/"), the pictures decoded and compared pixel for pixel with the script's and with assets/achievements/, the
# icon file checked for structure, and what happens with a file that is not the right one.
#   tools/check_config_tool.sh
cd "$(dirname "$0")/.."
T=build/config-test
fail=0
chk() { if [ "$1" = 0 ]; then echo "PASS  $2"; else echo "FAIL  $2"; fail=1; fi; }
[ -d build/tools ] || cmake -S tools -B build/tools -DCMAKE_POLICY_VERSION_MINIMUM=3.5 > /dev/null
cmake --build build/tools -j"$(sysctl -n hw.ncpu)" --target buffy_config buffy_config_names > /dev/null; chk $? "buffy_config builds, and writes sdk_names.txt from the SDK's sources"
rm -rf $T; mkdir -p $T
python3 -c "
import sys; sys.path.insert(0, 'tools')
import make_config
print('\n'.join(sorted(make_config.sdk_names('sdk'))))" > $T/py_names.txt
cmp -s $T/py_names.txt build/tools/sdk_names.txt; chk $? "the names found in the sources: $(wc -l < build/tools/sdk_names.txt | tr -d ' ') names, the same as make_config.py's sdk_names"
( time python3 tools/make_config.py --map assets/final.map --xbe assets/Default.xbe --out $T/py.toml ) 2>&1 | grep -E "functions|real" | tr '\n' ' '; echo
( time build/tools/buffy_config --map assets/final.map --xbe assets/Default.xbe --out $T/cpp.toml ) 2>&1 | grep -E "functions|real" | tr '\n' ' '; echo
cmp $T/py.toml $T/cpp.toml; chk $? "game.toml from buffy_config is byte for byte the one make_config.py writes ($(wc -c < $T/cpp.toml | tr -d ' ') bytes, $(wc -l < $T/cpp.toml | tr -d ' ') lines)"
cmp -s $T/cpp.toml config/game.toml; chk $? "and the config/game.toml in this checkout (what tools/regen.sh wrote)"
# the same through the names file beside the program, as the player's PC runs it (no --names)
mkdir -p "$T/folder é"; cp build/tools/buffy_config build/tools/sdk_names.txt "$T/folder é/"
cp assets/final.map assets/Default.xbe $T/
( cd $T && "folder é/buffy_config" --map final.map --xbe Default.xbe --out cpp2.toml > /dev/null ); cmp -s $T/cpp.toml $T/cpp2.toml; chk $? "the same from a folder with an accented name, with the names file found beside the program"
rm -f $T/final.map $T/Default.xbe

# --pictures
DISC=${DISC:-$(ls Buffy*USA*/*.iso 2>/dev/null | head -1)}
if [ -z "$DISC" ]; then echo "FAIL  no disc image found for --pictures: DISC=<image>"; exit 1; fi
mkdir -p "$T/disc files"
python3 -c "
import sys; sys.path.insert(0, 'tools')
from extract_assets import read_file
for src, dst in (('interfc/levelThumbnails.mtx', 'levelThumbnails.mtx'), ('media/launch_buffy.xpr', 'launch_buffy.xpr')):
    open(sys.argv[2] + '/' + dst, 'wb').write(read_file(sys.argv[1], src))" "$DISC" "$T/disc files" || { echo "FAIL  reading the two files from the disc image"; exit 1; }
( time build/tools/buffy_config --pictures "$T/disc files/levelThumbnails.mtx" "$T/disc files/launch_buffy.xpr" "$T/pictures é" > $T/pictures.out ) 2>&1 | grep real
cat $T/pictures.out
[ "$(ls "$T/pictures é/achievements" | grep -c '\.png$')" = 28 ]; chk $? "--pictures writes 28 achievement pictures into <out>/achievements, into a folder with an accent that did not exist"
python3 tools/make_achievement_icons.py "$DISC" --out $T/py-icons > /dev/null
python3 tools/compare_pngs.py "$T/pictures é/achievements" $T/py-icons; chk $? "they decode pixel for pixel to make_achievement_icons.py's"
if [ -d assets/achievements ]; then python3 tools/compare_pngs.py "$T/pictures é/achievements" assets/achievements > /dev/null; chk $? "and to the ones in assets/achievements"; fi
file "$T/pictures é/Buffy.png" | grep -q "PNG image data, 256 x 256, 8-bit/color RGBA"; chk $? "Buffy.png is 256 x 256 RGBA"
python3 tools/check_ico.py "$T/pictures é/Buffy.ico" 256 48 32 16; chk $? "Buffy.ico holds 256 (PNG), 48, 32 and 16 (DIB), each whole"
build/tools/buffy_config --pictures "$T/disc files/levelThumbnails.mtx" "$T/disc files/launch_buffy.xpr" "$T/pictures2" > /dev/null; cmp -s "$T/pictures é/Buffy.ico" "$T/pictures2/Buffy.ico" && cmp -s "$T/pictures é/achievements/boss_urd.png" "$T/pictures2/achievements/boss_urd.png"; chk $? "a second run writes the same bytes"
# a file that is not what it should be: a failure with a reason on stderr, exit 1, nothing made that looks whole
build/tools/buffy_config --pictures "$T/disc files/launch_buffy.xpr" "$T/disc files/launch_buffy.xpr" "$T/bad1" > /dev/null 2> $T/bad1.err; code=$?
[ $code = 1 ] && grep -q "is not a MULTITEX file" $T/bad1.err; chk $? "a thumbnails file that is not a MULTITEX: exit 1, \"$(cat $T/bad1.err)\""
head -c 100000 "$T/disc files/launch_buffy.xpr" > $T/short.xpr
build/tools/buffy_config --pictures "$T/disc files/levelThumbnails.mtx" $T/short.xpr "$T/bad2" > /dev/null 2> $T/bad2.err; code=$?
[ $code = 1 ] && grep -q "too short to be the launch screen" $T/bad2.err; chk $? "a launch screen cut short: exit 1, \"$(cat $T/bad2.err)\""
build/tools/buffy_config --pictures "$T/disc files/levelThumbnails.mtx" "$T/nothing.xpr" "$T/bad3" > /dev/null 2> $T/bad3.err; code=$?
[ $code = 1 ] && grep -q "cannot read" $T/bad3.err; chk $? "a file that is not there: exit 1, \"$(cat $T/bad3.err)\""
head -c 5000 "$T/disc files/levelThumbnails.mtx" > $T/short.mtx
build/tools/buffy_config --pictures $T/short.mtx "$T/disc files/launch_buffy.xpr" "$T/bad4" > /dev/null 2> $T/bad4.err; code=$?
[ $code = 1 ] && [ -s $T/bad4.err ]; chk $? "a thumbnails file cut short: exit 1, \"$(cat $T/bad4.err)\""
exit $fail
