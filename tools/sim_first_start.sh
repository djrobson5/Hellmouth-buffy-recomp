#!/bin/bash
# The player's PC at its first start, played on this Mac over the release as it is packaged: the zip tools/package_windows.sh made is
# unpacked into a folder whose name has a space and an accent, and the programs that ship as .exe (clang, lld-link, xbe_recomp,
# buffy_config) are replaced, under the same names, by this Mac's own builds of them, as nothing here can run a Windows program.
# Everything else is the zip's: Buffy.lib, the headers, the XDK layout, sdk_names.txt, Buffy.exe (its stamp is read from its bytes).
# The first start's own code (sdk/runtime/app/game_build.cpp: the steps, the order, the object reuse, the stamp check, the failure
# messages) is built for this Mac into build/fsc/first_start_check (tools/first_start_check.cpp), which differs from the Windows
# executable in how it starts a program and in having no window, and which reads the disc image through kernel::Disc, as the
# executable does. What is played:
#   a first build from the disc with the data beside the exe (the default layout), the sources, the config and the DLL compared with
#   what the build made; the second start (the DLL is current: nothing runs); a deleted DLL (every object is reused: only the link
#   runs); a build stopped half way and carried on; an update (a new ABI token: all compiled again); a damaged DLL; what the
#   player is told when a compiler fails, a tool is missing, a tool will not start, the disc is not there; a data folder of its own
#   with the compiler folder read only; the game from a folder of extracted files; the achievement pictures and the icon the first
#   start makes in the data folder (the pictures decode to the pixels tools/make_achievement_icons.py makes), made on their own marker
#   when the DLL is current, made again by a newer recipe, and never what stops a start (a disc without the files, a tool that fails).
#   tools/sim_first_start.sh              (needs out/Hellmouth-<version>-windows-x64.zip: tools/package_windows.sh)
#   needs: build/tools/{xbe_recomp,buffy_config,sdk_names.txt} (tools/regen.sh and tools/check_config_tool.sh build them), the disc
#   image in "Buffy the Vampire Slayer (USA)/" (DISC=<image> to name another), out/build/win-x64-mingw (the exe's flags and
#   its import library, and the game_code_dll target's DLL, which the new DLL is compared with)
set -uo pipefail
die() { echo "FAIL  $1"; exit 1; }
cd "$(dirname "$0")/.."
REPO=$PWD
B=$REPO/out/build/win-x64-mingw
TC=${LLVM_MINGW:-$HOME/Projects/toolchains/llvm-mingw}/bin
ZIP=$(ls "$REPO"/out/Hellmouth-*-windows-x64.zip 2>/dev/null | tail -1)
[ -n "$ZIP" ] || die "no release zip in out/: run tools/package_windows.sh first"
DISC=${DISC:-$(ls "$REPO"/Buffy*USA*/*.iso 2>/dev/null | head -1 || true)}
[ -n "$DISC" ] || die "no disc image found: DISC=<image>"
D="$REPO/build/sim dir é"
fail=0
chk() { if [ "$1" = 0 ]; then echo "PASS  $2"; else echo "FAIL  $2"; fail=1; fi; }
SLOTS=$(sysctl -n hw.ncpu)

echo "== the release, unpacked"
rm -rf "$D"; mkdir -p "$D"
unzip -q "$ZIP" -d "$D" || die "unzip"
APP="$D/Hellmouth é"; mv "$D"/Hellmouth-*-windows-x64 "$APP"
echo "$ZIP -> $APP"
ls "$APP" "$APP/compiler"
# this Mac's programs under the names the zip gives them (the Mac programs find their libraries in <app>/lib)
mkdir -p "$APP/lib"
cp "$TC/clang" "$APP/compiler/clang.exe"; cp "$TC/lld" "$APP/compiler/lld-link.exe"
cp -c "$TC"/../lib/libclang-cpp.*.dylib "$TC"/../lib/libLLVM.*.dylib "$APP/lib/"
cp "$REPO/build/tools/xbe_recomp" "$APP/compiler/xbe_recomp.exe"; cp "$REPO/build/tools/buffy_config" "$APP/compiler/buffy_config.exe"
cmp -s "$REPO/build/tools/sdk_names.txt" "$APP/compiler/sdk_names.txt"; chk $? "the Mac's sdk_names.txt is the one the zip ships (the names the config binds)"
"$APP/compiler/clang.exe" --version | head -1; "$APP/compiler/lld-link.exe" --version

echo "== the first start's code, for this Mac"
mkdir -p "$REPO/build/fsc"
clang++ -std=c++20 -O1 -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -I"$REPO/sdk/runtime" -I"$B/game_code" -I"$REPO/generated/game" \
    "$REPO/tools/first_start_check.cpp" "$REPO/sdk/runtime/app/game_build.cpp" "$REPO/sdk/runtime/kernel/disc.cpp" "$REPO/sdk/runtime/kernel/crypto.cpp" \
    -o "$REPO/build/fsc/first_start_check" || die "first_start_check does not compile"
FSC="$REPO/build/fsc/first_start_check"
# the stamp the first start expects (built from the headers the exe was built from) is the one in the exe's bytes
exe_stamp=$("$TC/llvm-strings" "$APP/Buffy.exe" | grep '^xbe-game-code ' | sort -u)
recipe_stamp=$("$FSC" --print-stamp)
[ -n "$exe_stamp" ] && [ "$exe_stamp" = "$recipe_stamp" ]; chk $? "the stamp the first start builds from the flags and image_symbols.h is the one in Buffy.exe's bytes"
echo "$recipe_stamp"

echo "== 1. the first build, from the disc image, with the data beside the exe (the default layout)"
DATA=$APP
"$FSC" --app "$APP" --data "$DATA" --game "$DISC" --check > /dev/null; [ $? = 1 ]; chk $? "before it: the DLL is missing, so a build is needed"
t0=$SECONDS
"$FSC" --app "$APP" --data "$DATA" --game "$DISC" --slots "$SLOTS" > "$D/first.out" 2> "$D/first.err"; code=$?
echo "$((SECONDS - t0)) s with $SLOTS compiles at a time"; chk $code "the build ran"; tail -2 "$D/first.out"; cat "$D/first.err"
W="$DATA/cache/game-code"; DLL="$DATA/build/Buffy.dll"
[ -f "$DLL" ]; chk $? "the DLL is in <data>/build/"
cmp -s "$W/Default.xbe" "$REPO/assets/Default.xbe"; chk $? "Default.xbe read from the disc image is the one in assets/ (its SHA-1 and sections hash are the executable's)"
cmp -s "$W/final.map" "$REPO/assets/final.map"; chk $? "final.map read from the disc image is the one in assets/"
cmp -s "$W/game.toml" "$REPO/config/game.toml"; chk $? "game.toml is the config/game.toml the build used"
same=1
for g in "$W"/x86_func_mapping.cpp "$W"/x86_recomp.*.cpp "$W"/x86_recomp_shared.h "$W"/image_symbols.h "$W"/exports.def "$W"/imports.def; do
    cmp -s "$g" "$REPO/generated/game/$(basename "$g")" || { echo "differs from generated/game: $(basename "$g")"; same=0; }
done
[ $same = 1 ]; chk $? "the sources are the ones in generated/game"
dll_stamp=$("$TC/llvm-strings" "$DLL" | grep '^xbe-game-code ' | sort -u)
[ "$dll_stamp" = "$exe_stamp" ]; chk $? "the DLL says what the executable expects"
[ "$(cat "$DATA/build/Buffy.dll.stamp")" = "$exe_stamp" ]; chk $? "build/Buffy.dll.stamp has it too"
[ ! -d "$DATA/build/new" ]; chk $? "no staging folder is left"
[ -z "$(find "$W" -name '*.tmp')" ]; chk $? "no temporary file is left"
grep -q '^run: ' "$DATA/logs/compile.log" && grep -q '^built: ' "$DATA/logs/compile.log"; chk $? "logs/compile.log has the commands and the result ($(wc -l < "$DATA/logs/compile.log" | tr -d ' ') lines)"
# the pictures and the icon the first start made, in the data folder
[ "$(cat "$DATA/pictures.stamp")" = "pictures 1" ]; chk $? "pictures.stamp says the recipe's version (pictures 1)"
[ "$(ls "$DATA/achievements" | grep -c '\.png$')" = 28 ] && [ -f "$DATA/Buffy.ico" ] && [ -f "$DATA/Buffy.png" ]; chk $? "the first start made 28 achievement pictures, Buffy.png and Buffy.ico in <data>"
python3 "$REPO/tools/make_achievement_icons.py" "$DISC" --out "$D/py-icons" > /dev/null || die "make_achievement_icons.py"
python3 "$REPO/tools/compare_pngs.py" "$DATA/achievements" "$D/py-icons"; chk $? "the achievement pictures decode pixel for pixel to the ones make_achievement_icons.py makes"
python3 "$REPO/tools/check_ico.py" "$DATA/Buffy.ico" 256 48 32 16; chk $? "Buffy.ico holds 256 (PNG), 48, 32 and 16 (DIB), each whole"
file "$DATA/Buffy.png" | grep -q "PNG image data, 256 x 256"; chk $? "Buffy.png is 256 x 256 ($(stat -f %z "$DATA/Buffy.png") bytes)"
mkdir -p "$REPO/out/hellmouth-icon-check"; cp "$DATA/Buffy.png" "$DATA/Buffy.ico" "$REPO/out/hellmouth-icon-check/"
[ ! -d "$DATA/cache/pictures" ]; chk $? "the scratch folder of the two disc files is gone"
grep -q '^pictures: made$' "$DATA/logs/compile.log"; chk $? "compile.log says the pictures were made"
grep '^time: ' "$DATA/logs/compile.log"
timed=1
for phase in "disc read" config translate pictures plan compile link "check and install" total; do [ "$(grep -c "^time: $phase [0-9]* ms" "$DATA/logs/compile.log")" = 1 ] || { echo "not one time for: $phase"; timed=0; }; done
[ $timed = 1 ]; chk $? "compile.log has a time for each phase (disc read, config, translate, pictures, plan, compile, link, check and install), once, and the total"
grep -q '^time: compile [0-9]* ms (129 files compiled, ' "$DATA/logs/compile.log"; chk $? "the compile's time is for all 129 files, and says so"
if [ -f "$B/game_code/Buffy.dll" ]; then
    # the PE time stamp (four bytes at e_lfanew + 8) is the only difference expected
    pe=$(od -An -tu4 -j60 -N4 "$DLL" | tr -d ' ')
    { cmp -l "$DLL" "$B/game_code/Buffy.dll" || true; } | awk -v lo=$((pe + 8)) '{o=$1-1; if (o >= lo && o < lo + 4) t++; else other++} END {printf "%d bytes differ: %d in the PE time stamp, %d elsewhere\n", t + other, t, other; exit ((other + 0) > 0 ? 1 : 0)}'
    chk $? "the DLL is the game_code_dll target's except for its PE time stamp"
fi

echo "== 2. the second start: the DLL is current, nothing runs"
log_before=$(stat -f %m "$DATA/logs/compile.log")
"$FSC" --app "$APP" --data "$DATA" --game "$DISC" | tee "$D/second.out"
grep -q "^dll: current" "$D/second.out" && [ "$log_before" = "$(stat -f %m "$DATA/logs/compile.log")" ]; chk $? "it is current, and the build log was not touched"
"$FSC" --app "$APP" --data "$DATA" --game "$DISC" --check > /dev/null; chk $? "--check says current"

echo "== 3. the DLL deleted, the work folder kept: every object is reused, only the link runs"
rm "$DLL"
t0=$SECONDS
"$FSC" --app "$APP" --data "$DATA" --game "$DISC" --slots "$SLOTS" > "$D/third.out" 2>&1; chk $? "rebuilt in $((SECONDS - t0)) s"
grep -q "compile: 0 files to compile, 129 reused" "$DATA/logs/compile.log"; chk $? "129 objects reused, none compiled again"
[ "$(stat -f %z "$DLL")" = "$(stat -f %z "$B/game_code/Buffy.dll")" ]; chk $? "and the DLL is the same size ($(stat -f %z "$DLL") bytes)"

echo "== 4. a build stopped half way (a closed window), then carried on"
rm -rf "$DATA/build" "$W"
"$FSC" --app "$APP" --data "$DATA" --game "$DISC" --slots 4 --stop-after 30 > "$D/stop.out" 2> "$D/stop.err"; code=$?
[ $code = 3 ]; chk $? "it stopped (exit code $code: $(cat "$D/stop.err"))"
keys=$(ls "$W" | grep -c '\.obj\.key$'); echo "$keys objects kept"
[ "$keys" -ge 30 ] && [ -z "$(find "$W" -name '*.tmp')" ]; chk $? "the finished objects are kept and no half written one is left"
tail -2 "$DATA/logs/compile.log" | grep -q '^time: total [0-9]* ms$'; chk $? "a stopped build still ends its log with the total time ($(tail -3 "$DATA/logs/compile.log" | tr '\n' ' '))"
[ ! -f "$DLL" ]; chk $? "and there is no DLL"
"$FSC" --app "$APP" --data "$DATA" --game "$DISC" --slots "$SLOTS" > "$D/carry.out" 2>&1; chk $? "carried on"
grep "compile:" "$DATA/logs/compile.log" | head -1
grep -q "compile: $((129 - keys)) files to compile, $keys reused" "$DATA/logs/compile.log"; chk $? "only the $((129 - keys)) that were not done were compiled"
[ "$("$TC/llvm-strings" "$DLL" | grep '^xbe-game-code ' | sort -u)" = "$exe_stamp" ]; chk $? "and the DLL is the executable's"

echo "== 5. an update: another ABI token, so the DLL is stale and every object is compiled again"
out=$("$FSC" --app "$APP" --data "$DATA" --game "$DISC" --abi abi00000000000000000000000000000000 --check); code=$?
[ $code = 1 ] && [[ "$out" == "dll: stale"* ]]; chk $? "a program with another token finds the DLL stale: ${out%%$'\n'*}"
"$FSC" --app "$APP" --data "$DATA" --game "$DISC" --slots "$SLOTS" --abi abi00000000000000000000000000000000 > "$D/update.out" 2>&1; chk $? "built for the new token"
grep -q "compile: 129 files to compile, 0 reused" "$DATA/logs/compile.log"; chk $? "all 129 compiled again"
"$TC/llvm-strings" "$DLL" > "$D/dll.strings"; grep -q "abi=abi00000000000000000000000000000000" "$D/dll.strings"; chk $? "the DLL says the new token"
"$FSC" --app "$APP" --data "$DATA" --game "$DISC" --slots "$SLOTS" > "$D/back.out" 2>&1; chk $? "and back to the real token (stale again, built again)"
grep -q "compile: 129 files to compile, 0 reused" "$DATA/logs/compile.log"; chk $? "all compiled again"
[ "$("$TC/llvm-strings" "$DLL" | grep '^xbe-game-code ' | sort -u)" = "$exe_stamp" ]; chk $? "the DLL is the executable's again"

echo "== 6. a damaged DLL, and one that is not a DLL"
cp "$DLL" "$D/good.dll"
head -c 100000 "$D/good.dll" > "$DLL"
out=$("$FSC" --app "$APP" --data "$DATA" --game "$DISC" --check); [[ "$out" == "dll: unreadable"* ]]; chk $? "a cut off DLL is unreadable (and so built again): $out"
echo "not a program" > "$DLL"
out=$("$FSC" --app "$APP" --data "$DATA" --game "$DISC" --check); [[ "$out" == "dll: unreadable"* ]]; chk $? "a text file is unreadable: $out"
cp "$D/good.dll" "$DLL"
"$FSC" --app "$APP" --data "$DATA" --game "$DISC" --check > /dev/null; chk $? "the good one is current"

echo "== 7. what the player is told when it goes wrong (on a copy of the folder)"
cp -c -R "$APP" "$D/appfail"; rm -rf "$D/appfail/cache" "$D/appfail/build" "$D/appfail/logs"
printf '#!/bin/sh\necho "x86_recomp.cpp:1:1: error: simulated compiler failure" >&2\nexit 1\n' > "$D/appfail/compiler/clang.exe"; chmod +x "$D/appfail/compiler/clang.exe"
"$FSC" --app "$D/appfail" --data "$D/appfail" --game "$DISC" --slots 4 > "$D/f1.out" 2> "$D/f1.err"; code=$?
head -4 "$D/f1.err"
[ $code = 9 ] && grep -q "^error: Compiling x86_.*\.cpp: clang.exe failed (exit code 1)" "$D/f1.err" && grep -q "simulated compiler failure" "$D/f1.err"
chk $? "a failing compiler: exit 9, the step and file named, the compiler's own message in the log tail"
grep -q '^time: compile [0-9]* ms' "$D/appfail/logs/compile.log" && grep -q '^time: total [0-9]* ms' "$D/appfail/logs/compile.log"; chk $? "and the log still has the time of the phase that failed and the total"
rm "$D/appfail/compiler/buffy_config.exe"
"$FSC" --app "$D/appfail" --data "$D/appfail" --game "$DISC" > "$D/f2.out" 2> "$D/f2.err"; code=$?
head -2 "$D/f2.err"
[ $code = 9 ] && grep -q "missing buffy_config.exe" "$D/f2.err"; chk $? "a missing tool: exit 9 and the file named, before anything is run"
cp "$APP/compiler/buffy_config.exe" "$D/appfail/compiler/buffy_config.exe"; chmod -x "$D/appfail/compiler/xbe_recomp.exe"
"$FSC" --app "$D/appfail" --data "$D/appfail" --game "$DISC" > "$D/f3.out" 2> "$D/f3.err"; code=$?
head -3 "$D/f3.err"
[ $code = 9 ] && grep -q "xbe_recomp.exe could not be started" "$D/f3.err" && grep -qi "antivirus" "$D/f3.err"; chk $? "a tool that will not start: exit 9, the step named, antivirus suggested"
chmod +x "$D/appfail/compiler/xbe_recomp.exe"
"$FSC" --app "$D/appfail" --data "$D/appfail" --game "$D/nothing.iso" > "$D/f4.out" 2> "$D/f4.err"; code=$?
head -2 "$D/f4.err"
[ $code = 9 ] && grep -q "Reading the game from your disc" "$D/f4.err"; chk $? "a disc image that is not there: exit 9, the step named"
head -c 200000 "$DISC" > "$D/short.iso"
"$FSC" --app "$D/appfail" --data "$D/appfail" --game "$D/short.iso" > "$D/f5.out" 2> "$D/f5.err"; code=$?
head -2 "$D/f5.err"
[ $code = 9 ] && grep -q "Reading the game from your disc" "$D/f5.err"; chk $? "a file that is not a disc image: exit 9, the step named"
rm -rf "$D/appfail" "$D/short.iso"

echo "== 8. a data folder of its own, with a space and an accent, and the compiler folder read only"
DATA2="$D/données de jeu"
chmod -R a-w "$APP/compiler"
"$FSC" --app "$APP" --data "$DATA2" --game "$DISC" --slots "$SLOTS" > "$D/data2.out" 2>&1; chk $? "built with the data folder elsewhere and the compiler folder read only (nothing is written beside the program)"
chmod -R u+w "$APP/compiler"
[ -f "$DATA2/build/Buffy.dll" ] && [ -f "$DATA2/logs/compile.log" ] && [ -d "$DATA2/cache/game-code" ]; chk $? "build/, logs/ and cache/game-code/ are in the data folder"
[ "$(ls "$DATA2/achievements" | grep -c '\.png$')" = 28 ] && [ -f "$DATA2/Buffy.ico" ] && [ -f "$DATA2/Buffy.png" ] && [ -f "$DATA2/pictures.stamp" ]; chk $? "the pictures and the icon are in the data folder too, written with the compiler folder read only"
python3 "$REPO/tools/compare_pngs.py" "$DATA2/achievements" "$D/py-icons" > /dev/null; chk $? "and they are the same pictures"
[ "$(stat -f %z "$DATA2/build/Buffy.dll")" = "$(stat -f %z "$DLL")" ]; chk $? "its DLL is the same size ($(stat -f %z "$DATA2/build/Buffy.dll") bytes)"

echo "== 9. the game from a folder of extracted files instead of the image"
mkdir -p "$D/extracted"
python3 "$REPO/tools/extract_assets.py" "$DISC" --out "$D/extracted" > /dev/null || die "extract_assets"
rm "$DATA2/build/Buffy.dll"
"$FSC" --app "$APP" --data "$DATA2" --game "$D/extracted" --slots "$SLOTS" > "$D/folder.out" 2>&1; chk $? "built from the folder"
grep -q "compile: 0 files to compile, 129 reused" "$DATA2/logs/compile.log"; chk $? "the same sources: all objects reused"
echo "a folder with no final.map:"
mkdir -p "$D/nomap"; cp "$D/extracted/Default.xbe" "$D/nomap/"; rm "$DATA2/build/Buffy.dll"
"$FSC" --app "$APP" --data "$DATA2" --game "$D/nomap" > "$D/nomap.out" 2> "$D/nomap.err"; code=$?
head -1 "$D/nomap.err"; [ $code = 9 ]; chk $? "refused with the step named"

echo "== 10. the pictures on their own: the DLL is current, the pictures are not made (or are made by an older recipe)"
"$FSC" --app "$APP" --data "$DATA2" --game "$DISC" --slots "$SLOTS" > "$D/p0.out" 2>&1   # the DLL there since section 9 is gone: built again from the kept objects
"$FSC" --app "$APP" --data "$DATA2" --game "$DISC" --check > /dev/null; chk $? "a start whose DLL is current"
cp "$DATA2/build/Buffy.dll" "$D/before.dll"; cp "$DATA2/achievements/kills_1.png" "$D/kills_1.before.png"
lines_before=$(wc -l < "$DATA2/logs/compile.log"); compiles_before=$(grep -c '^run: .*clang' "$DATA2/logs/compile.log")
rm -rf "$DATA2/achievements" "$DATA2/Buffy.ico" "$DATA2/Buffy.png" "$DATA2/pictures.stamp"
"$FSC" --app "$APP" --data "$DATA2" --game "$DISC" > "$D/p1.out" 2>&1; code=$?
grep -q '^pictures: made$' "$D/p1.out" && [ $code = 0 ]; chk $? "with no pictures at all it makes them, with exit 0 ($(grep '^pictures' "$D/p1.out"))"
cmp -s "$DATA2/build/Buffy.dll" "$D/before.dll"; chk $? "and the DLL is not built again (the same bytes)"
[ "$(grep -c '^run: .*clang' "$DATA2/logs/compile.log")" = "$compiles_before" ] && grep -q '^built: ' "$DATA2/logs/compile.log"; chk $? "no compile or link ran, and the earlier build's lines are still in compile.log (it is added to, not started again)"
[ "$(wc -l < "$DATA2/logs/compile.log")" -gt "$lines_before" ] && tail -8 "$DATA2/logs/compile.log" | grep -q '^time: pictures [0-9]* ms$'; chk $? "the log has the pictures' own lines and time at its end"
python3 "$REPO/tools/compare_pngs.py" "$DATA2/achievements" "$D/py-icons" > /dev/null && [ "$(cat "$DATA2/pictures.stamp")" = "pictures 1" ]; chk $? "the same 28 pictures and the marker are back"
rm -rf "$DATA2/achievements"
"$FSC" --app "$APP" --data "$DATA2" --game "$DISC" > "$D/p2.out" 2>&1; grep -q '^pictures: made$' "$D/p2.out"; chk $? "a missing achievements folder alone is made again (the marker is not enough)"
"$FSC" --app "$APP" --data "$DATA2" --game "$DISC" > "$D/p3.out" 2>&1; grep -q '^pictures: current$' "$D/p3.out" && [ "$(wc -l < "$DATA2/logs/compile.log")" -gt 0 ]; chk $? "and with everything there the next start makes nothing ($(grep '^pictures' "$D/p3.out"))"
lines_now=$(wc -l < "$DATA2/logs/compile.log")
"$FSC" --app "$APP" --data "$DATA2" --game "$DISC" --pictures 2 > "$D/p4.out" 2>&1; grep -q '^pictures: made$' "$D/p4.out" && [ "$(cat "$DATA2/pictures.stamp")" = "pictures 2" ]; chk $? "an executable with a newer picture recipe (2) makes them again over the old ones: the marker says 2"
cmp -s "$DATA2/build/Buffy.dll" "$D/before.dll"; chk $? "and the DLL is still the same bytes"
"$FSC" --app "$APP" --data "$DATA2" --game "$DISC" --pictures 2 > "$D/p5.out" 2>&1; [ "$(wc -l < "$DATA2/logs/compile.log")" -gt "$lines_now" ] && grep -q '^pictures: current$' "$D/p5.out"; chk $? "and the start after that makes nothing"
"$FSC" --app "$APP" --data "$DATA2" --game "$DISC" > "$D/p6.out" 2>&1; cmp -s "$D/kills_1.before.png" "$DATA2/achievements/kills_1.png"; chk $? "the recipe back at 1 gives the first pictures again ($(grep '^pictures' "$D/p6.out"))"

echo "== 11. a picture that cannot be made is never what stops a start (on copies of the data folder)"
rm -rf "$D/p11"; mkdir -p "$D/p11"
# a: the disc of extracted files has no interfc/ or media/: the build goes on, the log says why, and the next start tries again
rm -rf "$DATA2/achievements" "$DATA2/Buffy.ico" "$DATA2/Buffy.png" "$DATA2/pictures.stamp"
"$FSC" --app "$APP" --data "$DATA2" --game "$D/extracted" > "$D/q1.out" 2>&1; code=$?
[ $code = 0 ] && grep -q '^pictures: not made$' "$D/q1.out" && grep -q '^pictures: not made: .*has no interfc/levelThumbnails.mtx' "$DATA2/logs/compile.log"; chk $? "a folder with no interfc/: a start with a current DLL goes on (exit $code) and compile.log says why ($(grep -o 'not made: .*' "$DATA2/logs/compile.log" | tail -1))"
[ ! -f "$DATA2/pictures.stamp" ] && [ ! -f "$DATA2/Buffy.ico" ]; chk $? "and no marker is written, so the next start tries again"
rm "$DATA2/build/Buffy.dll"
"$FSC" --app "$APP" --data "$DATA2" --game "$D/extracted" --slots "$SLOTS" > "$D/q2.out" 2> "$D/q2.err"; code=$?
[ $code = 0 ] && [ -f "$DATA2/build/Buffy.dll" ] && grep -q '^built: ' "$DATA2/logs/compile.log" && grep -q '^pictures: not made: ' "$DATA2/logs/compile.log" && [ ! -s "$D/q2.err" ]; chk $? "the same in a build: the DLL is built, exit 0, no error, the log says the pictures were not made"
grep -q '^time: pictures ' "$DATA2/logs/compile.log" && grep -q '^time: total ' "$DATA2/logs/compile.log"; chk $? "and its phases are timed all the same"
# b: the disc image again
"$FSC" --app "$APP" --data "$DATA2" --game "$DISC" > "$D/q3.out" 2>&1; grep -q '^pictures: made$' "$D/q3.out"; chk $? "the disc image again: the pictures are made after all"
# c: the tool fails or is not there: on a copy of the app folder
cp -c -R "$APP" "$D/p11/app"; rm -rf "$D/p11/app/achievements" "$D/p11/app/Buffy.ico" "$D/p11/app/Buffy.png" "$D/p11/app/pictures.stamp" "$D/p11/app/cache" "$D/p11/app/logs"
printf '#!/bin/sh\necho "buffy_config: simulated failure" >&2\nexit 3\n' > "$D/p11/app/compiler/buffy_config.exe"; chmod +x "$D/p11/app/compiler/buffy_config.exe"
"$FSC" --app "$D/p11/app" --data "$D/p11/app" --game "$DISC" > "$D/q4.out" 2>&1; code=$?
[ $code = 0 ] && grep -q '^pictures: not made: buffy_config.exe failed (exit code 3)' "$D/p11/app/logs/compile.log" && grep -q 'simulated failure' "$D/p11/app/logs/compile.log" && [ ! -f "$D/p11/app/pictures.stamp" ]; chk $? "a picture tool that fails: the start goes on (exit $code), the log has its message and the exit code, no marker"
rm "$D/p11/app/compiler/buffy_config.exe"
"$FSC" --app "$D/p11/app" --data "$D/p11/app" --game "$DISC" > "$D/q5.out" 2>&1; code=$?
[ $code = 0 ] && grep -q '^pictures: not made: buffy_config.exe could not be started' "$D/p11/app/logs/compile.log"; chk $? "a picture tool that is not there: the start goes on (exit $code), the log says it could not be started"
rm -rf "$D/p11"

echo
[ $fail = 0 ] && echo "all checks passed" || echo "SOME CHECKS FAILED"
exit $fail
