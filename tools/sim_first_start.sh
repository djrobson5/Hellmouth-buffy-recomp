#!/bin/bash
# The player's PC at its first start, played on this Mac: recompile the xbe, compile each file, link Buffy.dll. The same
# commands and flags (the flags are read from the build's game_code_flags.h), run from a folder whose name has a space and an
# accented letter, with copies of the toolchain programs named as they ship (clang, lld-link), and only what ships: the
# recompiler's input, the XDK layouts, the freestanding headers and Buffy.lib, the executable's import library.
#   tools/sim_first_start.sh [build folder]    default out/build/win-x64-mingw; needs build/tools/xbe_recomp (tools/regen.sh)
# Writes into build/sim dir é/, and compares the DLL with the one the game_code_dll target built. config/game.toml comes from the
# disc's final.map, so whether the real first start ships it or makes it on the player's PC is open (step 3).
set -euo pipefail
cd "$(dirname "$0")/.."
REPO=$PWD
B=$REPO/${1:-out/build/win-x64-mingw}
TC=${LLVM_MINGW:-$HOME/Projects/toolchains/llvm-mingw}/bin
D="$REPO/build/sim dir é"
rm -rf "$D"; mkdir -p "$D/compiler/include/cpu" "$D/compiler/include/freestanding" "$D/compiler/config" "$D/compiler/xdk/4432" "$D/work" "$D/lib"
# copies named like the Windows ones (the Mac programs find their libraries through @loader_path/../lib, so lib/ holds the two
# they link against and nothing else: no compiler headers beyond the ones the build ships)
cp "$TC/clang" "$D/compiler/clang"; cp "$TC/lld" "$D/compiler/lld-link"
cp -c "$TC"/../lib/libclang-cpp.*.dylib "$TC"/../lib/libLLVM.*.dylib "$D/lib/"
cp "$REPO/build/tools/xbe_recomp" "$D/compiler/xbe_recomp"
cp "$REPO/sdk/runtime/cpu/x86_ops.h" "$REPO/sdk/runtime/cpu/x86_context.h" "$D/compiler/include/cpu/"
cp "$REPO"/sdk/runtime/cpu/freestanding/* "$D/compiler/include/freestanding/"
cp "$REPO/config/game.toml" "$D/compiler/config/"; cp "$REPO/sdk/xdk/4432/layout.toml" "$D/compiler/xdk/4432/"
cp "$B/game_code/Buffy.lib" "$D/compiler/Buffy.lib"
cp "$REPO/assets/Default.xbe" "$D/work/Default.xbe"
CF=$(sed -n 's/#define XBE_GAME_CODE_COMPILE_FLAGS L"\(.*\)"/\1/p' "$B/game_code/game_code_flags.h")
LF=$(sed -n 's/#define XBE_GAME_CODE_LINK_FLAGS L"\(.*\)"/\1/p' "$B/game_code/game_code_flags.h")
"$D/compiler/clang" --version | head -1; "$D/compiler/lld-link" --version
echo "COMPILE FLAGS: $CF"; echo "LINK FLAGS: $LF"
cd "$D/work"

echo "== 1. recompile"
time "$D/compiler/xbe_recomp" "$D/compiler/config/game.toml" "$D/work/Default.xbe" "$D/work" --xdk "$D/compiler/xdk" 2> recomp.log
tail -2 recomp.log

echo "== 2. compile $(ls x86_recomp.*.cpp x86_func_mapping.cpp | wc -l) files, 18 at a time"
export CF D
time (ls x86_func_mapping.cpp x86_recomp.*.cpp | xargs -P 18 -n1 sh -c \
    '"$D/compiler/clang" $CF -isystem "$D/compiler/include/freestanding" "-I$D/compiler/include" -c "$0" -o "${0%.cpp}.obj.tmp" && mv "${0%.cpp}.obj.tmp" "${0%.cpp}.obj"')

echo "== 3. link"
OBJS="x86_func_mapping.obj"; for i in $(seq 0 127); do OBJS="$OBJS x86_recomp.$i.obj"; done
time "$D/compiler/lld-link" $LF /def:exports.def /implib:code.lib "/out:$D/Buffy.dll" "$D/compiler/Buffy.lib" $OBJS
ls -l "$D/Buffy.dll"
if [ -f "$B/game_code/Buffy.dll" ]; then
    echo "differing bytes against the game_code_dll target's DLL (the PE time stamp is one): $(cmp -l "$D/Buffy.dll" "$B/game_code/Buffy.dll" | wc -l | tr -d ' ')"
fi
