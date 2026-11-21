#!/bin/bash
# The player's PC at its first start, played on this Mac: from the two files on the disc to Buffy.dll, with the commands and
# flags the first start will run, in a folder whose name has a space and an accented letter, with copies of the toolchain programs
# named as they ship (clang, lld-link), and only what ships beside the executable:
#   0. the disc's Default.xbe and final.map (what the first start reads from the player's disc; here tools/extract_assets.py does it)
#   1. buffy_config: final.map, Default.xbe and sdk_names.txt (beside it) -> game.toml
#   2. xbe_recomp: game.toml and Default.xbe, with the XDK layout -> the C++ sources, exports.def, imports.def
#   3. clang: each of the 129 sources -> .obj
#   4. lld-link: the objects and Buffy.lib, the executable's import library -> Buffy.dll
#   5. the stamp: what Buffy.exe expects, read from its bytes, against what the new DLL says
# The programs that run are the ones built for this Mac (the Windows programs, out/build/win-x64-mingw-tools, cannot run here; see
# README.md for how they are built). The flags come from the build's game_code_flags.h. Writes into build/sim dir é/ and compares
# the sources with generated/game and the DLL with the one the game_code_dll target built.
#   tools/sim_first_start.sh [build folder]    default out/build/win-x64-mingw
#   needs: build/tools/{xbe_recomp,buffy_config,sdk_names.txt} (tools/regen.sh and tools/check_config_tool.sh build them) and the
#   disc image in "Buffy the Vampire Slayer (USA)/" (DISC=<image> to name another; without one the files in assets/ stand in)
set -uo pipefail
die() { echo "FAIL  $1"; exit 1; }
cd "$(dirname "$0")/.."
REPO=$PWD
B=$REPO/${1:-out/build/win-x64-mingw}
TC=${LLVM_MINGW:-$HOME/Projects/toolchains/llvm-mingw}/bin
D="$REPO/build/sim dir é"
fail=0
chk() { if [ "$1" = 0 ]; then echo "PASS  $2"; else echo "FAIL  $2"; fail=1; fi; }
DISC=${DISC:-$(ls "$REPO"/Buffy*USA*/*.iso 2>/dev/null | head -1 || true)}

rm -rf "$D"; mkdir -p "$D/compiler/include/cpu" "$D/compiler/include/freestanding" "$D/compiler/xdk/4432" "$D/work/Buffy" "$D/lib"
# the program: the stamp is read from its bytes, which is what ships
cp "$B/Buffy.exe" "$D/Buffy.exe"
# copies named like the Windows ones (the Mac programs find their libraries through @loader_path/../lib, so lib/ holds the two
# they link against and nothing else: no compiler headers beyond the ones the build ships)
cp "$TC/clang" "$D/compiler/clang"; cp "$TC/lld" "$D/compiler/lld-link"
cp -c "$TC"/../lib/libclang-cpp.*.dylib "$TC"/../lib/libLLVM.*.dylib "$D/lib/"
cp "$REPO/build/tools/xbe_recomp" "$REPO/build/tools/buffy_config" "$REPO/build/tools/sdk_names.txt" "$D/compiler/"
cp "$REPO/sdk/runtime/cpu/x86_ops.h" "$REPO/sdk/runtime/cpu/x86_context.h" "$D/compiler/include/cpu/"
cp "$REPO"/sdk/runtime/cpu/freestanding/* "$D/compiler/include/freestanding/"
cp "$REPO/sdk/xdk/4432/layout.toml" "$D/compiler/xdk/4432/"
cp "$B/game_code/Buffy.lib" "$D/compiler/Buffy.lib"
CF=$(sed -n 's/#define XBE_GAME_CODE_COMPILE_FLAGS L"\(.*\)"/\1/p' "$B/game_code/game_code_flags.h")
LF=$(sed -n 's/#define XBE_GAME_CODE_LINK_FLAGS L"\(.*\)"/\1/p' "$B/game_code/game_code_flags.h")
"$D/compiler/clang" --version | head -1; "$D/compiler/lld-link" --version
echo "COMPILE FLAGS: $CF"; echo "LINK FLAGS: $LF"
W="$D/work/Buffy"
cd "$W"

echo "== 0. the disc's files"
if [ -n "$DISC" ]; then
    echo "disc image: $DISC"
    python3 "$REPO/tools/extract_assets.py" "$DISC" --out "$W" || die "reading the disc image"
else
    echo "no disc image found: the files in assets/ stand in for it"
    cp "$REPO/assets/Default.xbe" "$REPO/assets/final.map" "$W/"
fi
cmp -s Default.xbe "$REPO/assets/Default.xbe"; chk $? "Default.xbe is the one in assets/ (the SHA-1 and sections hash FindGame checks are then the executable's)"
cmp -s final.map "$REPO/assets/final.map"; chk $? "final.map is the one in assets/"

echo "== 1. config"
time "$D/compiler/buffy_config" --map "$W/final.map" --xbe "$W/Default.xbe" --out "$W/game.toml" || die "buffy_config"
cmp -s "$W/game.toml" "$REPO/config/game.toml"; chk $? "game.toml is the config/game.toml the build used (tools/make_config.py's, from the same files)"
grep -q '^library = "Buffy.dll"' game.toml; chk $? "and names the DLL Buffy.dll, as the executable's CMake target needs"

echo "== 2. recompile"
time "$D/compiler/xbe_recomp" "$W/game.toml" "$W/Default.xbe" "$W" --xdk "$D/compiler/xdk" 2> recomp.log || { cat recomp.log; die "xbe_recomp"; }
tail -2 recomp.log
same=1
for f in x86_func_mapping.cpp x86_recomp.*.cpp x86_recomp_shared.h image_symbols.h exports.def imports.def; do
    cmp -s "$f" "$REPO/generated/game/$f" || { echo "differs from generated/game: $f"; same=0; }
done
[ $same = 1 ]; chk $? "the sources are the ones in generated/game ($(ls x86_recomp.*.cpp | wc -l | tr -d ' ') partitions, mapping, shared header, image_symbols.h, exports.def, imports.def)"

echo "== 3. compile $(ls x86_recomp.*.cpp x86_func_mapping.cpp | wc -l | tr -d ' ') files, as many at once as there are cores"
export CF D
time (ls x86_func_mapping.cpp x86_recomp.*.cpp | xargs -P "$(sysctl -n hw.ncpu)" -n1 sh -c \
    '"$D/compiler/clang" $CF -isystem "$D/compiler/include/freestanding" "-I$D/compiler/include" -c "$0" -o "${0%.cpp}.obj.tmp" && mv "${0%.cpp}.obj.tmp" "${0%.cpp}.obj"') || die "a compile"

echo "== 4. link"
OBJS="x86_func_mapping.obj"; for i in $(seq 0 127); do OBJS="$OBJS x86_recomp.$i.obj"; done
time "$D/compiler/lld-link" $LF /def:exports.def /implib:code.lib "/out:$D/Buffy.dll" "$D/compiler/Buffy.lib" $OBJS || die "the link"
ls -l "$D/Buffy.dll"

echo "== 5. the stamp, from the bytes of Buffy.exe and of the new Buffy.dll"
exe_stamp=$("$TC/llvm-strings" "$D/Buffy.exe" | grep '^xbe-game-code ' | sort -u)
dll_stamp=$("$TC/llvm-strings" "$D/Buffy.dll" | grep '^xbe-game-code ' | sort -u)
echo "Buffy.exe expects: $exe_stamp"; echo "Buffy.dll says:    $dll_stamp"
[ -n "$exe_stamp" ] && [ "$exe_stamp" = "$dll_stamp" ]; chk $? "the DLL's stamp is the one the executable expects: it would be accepted"
# a control: a DLL compiled for another executable (another ABI token) must be recognised
"$D/compiler/clang" $CF -DXBE_GAME_CODE_ABI=abi00000000000000000000000000000000 -isystem "$D/compiler/include/freestanding" "-I$D/compiler/include" \
    -c x86_func_mapping.cpp -o stale_mapping.obj 2> stale.log || true   # -D twice: the last one wins, with a redefinition warning (-w hides it)
OBJS_STALE=${OBJS/x86_func_mapping.obj/stale_mapping.obj}
"$D/compiler/lld-link" $LF /def:exports.def /implib:stale.lib "/out:$W/Buffy.stale.dll" "$D/compiler/Buffy.lib" $OBJS_STALE > /dev/null
stale_stamp=$("$TC/llvm-strings" "$W/Buffy.stale.dll" | grep '^xbe-game-code ' | sort -u)
echo "a DLL for another ABI says: $stale_stamp"
[ -n "$stale_stamp" ] && [ "$stale_stamp" != "$exe_stamp" ]; chk $? "control: a DLL built with another ABI token has a stamp the executable would refuse"
rm -f "$W/Buffy.stale.dll" stale.lib stale_mapping.obj

echo "== 6. against the game_code_dll target's DLL"
if [ -f "$B/game_code/Buffy.dll" ]; then
    # the PE time stamp (four bytes at e_lfanew + 8) is the only difference expected
    pe=$(od -An -tu4 -j60 -N4 "$D/Buffy.dll" | tr -d ' ')
    { cmp -l "$D/Buffy.dll" "$B/game_code/Buffy.dll" || true; } | awk -v lo=$((pe + 8)) '{o=$1-1; if (o >= lo && o < lo + 4) t++; else other++} END {printf "%d bytes differ: %d in the PE time stamp, %d elsewhere\n", t + other, t, other; exit ((other + 0) > 0 ? 1 : 0)}'
    chk $? "Buffy.dll is the game_code_dll target's DLL except for its PE time stamp"
    [ "$(stat -f %z "$D/Buffy.dll")" = "$(stat -f %z "$B/game_code/Buffy.dll")" ]; chk $? "and is the same size ($(stat -f %z "$D/Buffy.dll") bytes)"
fi
exit $fail
