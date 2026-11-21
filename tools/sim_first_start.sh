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
#   with the compiler folder read only; the game from a folder of extracted files.
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

echo
[ $fail = 0 ] && echo "all checks passed" || echo "SOME CHECKS FAILED"
exit $fail
