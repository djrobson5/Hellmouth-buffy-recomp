#!/bin/bash
# Assembles the Windows release, out/Hellmouth-<version>-windows-x64/ and out/Hellmouth-<version>-windows-x64.zip, from what the
# cross builds made, and audits it: the release holds no file made from the game (no generated code, no config/game.toml, no
# assets, no Buffy.dll, no image_symbols.h), and nothing that names this machine. Everything is built first:
#   export LLVM_MINGW=<llvm-mingw for this machine>
#   cmake --preset win-x64-mingw && cmake --build out/build/win-x64-mingw && cmake --build out/build/win-x64-mingw --target game_code_dll
#   (cd tools && cmake --preset win-x64-mingw && cmake --build --preset win-x64-mingw)        # the first start's two tools
#   tools/package_windows.sh [-v <version>]
# env: RECOMPLAUNCHER_GUIDE  RecompLauncher's developer guide (recomplauncher-agent.md): its JSON schema checks recomplauncher.json (optional)
#      LLVM_MINGW_WIN  the Windows-hosted llvm-mingw (default ~/Projects/toolchains/llvm-mingw-win-x86_64): clang, lld and their DLLs
#      LLVM_MINGW_SRC  its sources, for the licence texts (default ~/Projects/toolchains/llvm-mingw-src)
#      LLVM_MINGW      llvm-mingw for this machine (llvm-strings and the zip's checks); default ~/Projects/toolchains/llvm-mingw
# The layout is the one the first start expects (sdk/runtime/app/game_build.h): <app>\Buffy.exe and <app>\compiler\... Nothing is
# uploaded, and the DLL the build made is only read, to audit the release against it.
set -euo pipefail
cd "$(dirname "$0")/.."
REPO=$PWD
VERSION=$(sed -n 's/.*\.version = "buffy-port \(.*\)",.*/\1/p' src/common/game_info.cpp)
while getopts v: opt; do
    case $opt in v) VERSION=$OPTARG ;; *) sed -n 2,14p "$0"; exit 2 ;; esac
done
[ -n "$VERSION" ] || { echo "no version: pass -v"; exit 2; }
B=$REPO/out/build/win-x64-mingw
T=$REPO/out/build/win-x64-mingw-tools
WT=${LLVM_MINGW_WIN:-$HOME/Projects/toolchains/llvm-mingw-win-x86_64}/bin
SRC=${LLVM_MINGW_SRC:-$HOME/Projects/toolchains/llvm-mingw-src}
TC=${LLVM_MINGW:-$HOME/Projects/toolchains/llvm-mingw}/bin
NAME=Hellmouth-$VERSION-windows-x64
P=$REPO/out/$NAME
need() { [ -f "$1" ] || { echo "missing $1: build it first (see the top of this script)"; exit 1; }; }
for f in "$B/Buffy.exe" "$B/game_code/Buffy.lib" "$B/game_code/Buffy.dll" "$B/game_code/abi_inputs.txt" \
         "$T/xbe_recomp.exe" "$T/buffy_config.exe" "$T/sdk_names.txt" \
         "$WT/clang-23.exe" "$WT/ld.lld.exe" "$WT/libLLVM-23.dll" "$WT/libclang-cpp.dll" "$WT/libc++.dll" "$WT/libunwind.dll" \
         "$SRC/llvm-project/libcxx/LICENSE.TXT" "$SRC/llvm-project/libcxxabi/LICENSE.TXT" "$SRC/llvm-project/libunwind/LICENSE.TXT" \
         "$SRC/mingw-w64/COPYING.MinGW-w64-runtime/COPYING.MinGW-w64-runtime.txt" "$SRC/mingw-w64/COPYING.MinGW-w64/COPYING.MinGW-w64.txt" \
         "$REPO/assets/Default.xbe"; do need "$f"; done
# the program and the DLL the build made must be a pair (they would not be, if one was rebuilt without the other)
exe_stamp=$("$TC/llvm-strings" "$B/Buffy.exe" | grep '^xbe-game-code ' | sort -u)
dll_stamp=$("$TC/llvm-strings" "$B/game_code/Buffy.dll" | grep '^xbe-game-code ' | sort -u)
[ -n "$exe_stamp" ] && [ "$exe_stamp" = "$dll_stamp" ] || { echo "Buffy.exe and game_code/Buffy.dll are not a pair: rebuild the game_code_dll target"; exit 1; }

rm -rf "$P" "$P.zip"
mkdir -p "$P/compiler/include/cpu" "$P/compiler/include/freestanding" "$P/compiler/xdk/4432" "$P/licenses"
cp "$B/Buffy.exe" "$P/Buffy.exe"
# the compiler: the real clang driver (clang-23.exe is what the clang.exe wrapper chain ends in), lld as lld-link.exe (lld picks its
# COFF driver by that name), and the four DLLs they import (nothing else: see llvm-readobj --coff-imports)
cp -L "$WT/clang-23.exe" "$P/compiler/clang.exe"
cp -L "$WT/ld.lld.exe" "$P/compiler/lld-link.exe"
for d in libLLVM-23.dll libclang-cpp.dll libc++.dll libunwind.dll; do cp -L "$WT/$d" "$P/compiler/$d"; done
cp "$T/xbe_recomp.exe" "$T/buffy_config.exe" "$T/sdk_names.txt" "$P/compiler/"
cp "$B/game_code/Buffy.lib" "$P/compiler/Buffy.lib"
cp sdk/runtime/cpu/x86_ops.h sdk/runtime/cpu/x86_context.h "$P/compiler/include/cpu/"
cp sdk/runtime/cpu/freestanding/* "$P/compiler/include/freestanding/"
cp sdk/xdk/4432/layout.toml "$P/compiler/xdk/4432/"
cp sdk/tools/LLVM-LICENSE.txt "$P/compiler/LLVM-LICENSE.txt"
# licences and the player's README
cp sdk/tools/LLVM-LICENSE.txt "$P/licenses/LLVM-LICENSE.txt"
cp "$SRC/llvm-project/libcxx/LICENSE.TXT" "$P/licenses/libcxx-LICENSE.txt"
cp "$SRC/llvm-project/libcxxabi/LICENSE.TXT" "$P/licenses/libcxxabi-LICENSE.txt"
cp "$SRC/llvm-project/libunwind/LICENSE.TXT" "$P/licenses/libunwind-LICENSE.txt"
cp "$SRC/mingw-w64/COPYING.MinGW-w64-runtime/COPYING.MinGW-w64-runtime.txt" "$P/licenses/mingw-w64-runtime-COPYING.txt"
cp "$SRC/mingw-w64/COPYING.MinGW-w64/COPYING.MinGW-w64.txt" "$P/licenses/mingw-w64-COPYING.txt"
cp sdk/licenses/* "$P/licenses/"
cp sdk/THIRD_PARTY.txt "$P/THIRD_PARTY.txt"
cp LICENSE "$P/LICENSE"
sed "s/@VERSION@/$VERSION/" tools/windows_player_readme.txt | sed 's/$/\r/' > "$P/README.txt"   # CRLF: it is opened in Notepad
# RecompLauncher reads recomplauncher.json from the top of the release, beside Buffy.exe: how it sets the game up from the player's disc,
# builds it (--prepare), starts it and shows its settings. The release's version goes in as the setup step's version, so that a player who
# set the game up with an older release has it built again. It is checked before anything is zipped: against RecompLauncher's schema (in its
# developer guide, not kept here: RECOMPLAUNCHER_GUIDE says where the guide is) and against this game's own settings and options.
sed "s/@VERSION@/$VERSION/" tools/recomplauncher.json.in > "$P/recomplauncher.json"
echo "== recomplauncher.json:"
if [ -n "${RECOMPLAUNCHER_GUIDE:-}" ]; then
    python3 -I tools/check_recomplauncher.py "$P/recomplauncher.json" --guide "$RECOMPLAUNCHER_GUIDE" --version "$VERSION" --self-test
else
    echo "  RECOMPLAUNCHER_GUIDE is not set: the schema is NOT checked, only the file against this game (set it to RecompLauncher's recomplauncher-agent.md)"
    python3 -I tools/check_recomplauncher.py "$P/recomplauncher.json" --no-schema --version "$VERSION"
fi

# The toolchain here was built in a home folder, and its libraries carry the build's paths (/Users/<name>/Projects/..., in their
# debug information and in the assertion messages of libc++abi). Each is made a same length neutral path, so that nothing else in
# the file moves, and the audit below fails if any is left. Only text inside files that are copies is changed.
echo "== names of the build machine in the copies:"
python3 -I - "$P" <<'PY'
import os, re, sys
total = 0
for root, _, names in os.walk(sys.argv[1]):
    for n in names:
        p = os.path.join(root, n)
        data = open(p, "rb").read()
        count = 0
        def blank(m):
            global count
            count += 1
            return b"/src" + b"/" * (len(m.group(0)) - 4)
        out = re.sub(rb"/Users/[A-Za-z0-9._-]+", blank, data)
        if count:
            open(p, "wb").write(out)
            print("  %-45s %d path(s) made neutral" % (os.path.relpath(p, sys.argv[1]), count))
            total += count
print("  %d in all" % total)
PY

# the copies are structurally what they were: the same headers, sections, imports and exports (only text inside them changed)
echo "== structure of the patched copies against the originals (llvm-readobj: headers, sections, imports, exports):"
same_structure() {
    diff <("$TC/llvm-readobj" --file-headers --sections --coff-imports --coff-exports "$1" | grep -v '^File:') \
         <("$TC/llvm-readobj" --file-headers --sections --coff-imports --coff-exports "$2" | grep -v '^File:') > /dev/null \
        && echo "  same: $3" || { echo "  DIFFERENT: $3"; exit 1; }
}
same_structure "$B/Buffy.exe" "$P/Buffy.exe" Buffy.exe
same_structure "$T/xbe_recomp.exe" "$P/compiler/xbe_recomp.exe" compiler/xbe_recomp.exe
same_structure "$T/buffy_config.exe" "$P/compiler/buffy_config.exe" compiler/buffy_config.exe
same_structure "$WT/libc++.dll" "$P/compiler/libc++.dll" compiler/libc++.dll
same_structure "$WT/libunwind.dll" "$P/compiler/libunwind.dll" compiler/libunwind.dll

python3 -I tools/audit_windows_package.py "$P" --dll "$B/game_code/Buffy.dll" --xbe "$REPO/assets/Default.xbe" \
    --generated "$REPO/generated/game" --abi-inputs "$B/game_code/abi_inputs.txt" --version "$VERSION"
# one folder in the zip, no Mac metadata, no links, times and owners not kept
(cd out && zip -r -X -q "$NAME.zip" "$NAME")
python3 -I tools/audit_windows_package.py "$P" --dll "$B/game_code/Buffy.dll" --xbe "$REPO/assets/Default.xbe" \
    --generated "$REPO/generated/game" --abi-inputs "$B/game_code/abi_inputs.txt" --version "$VERSION" --zip "$P.zip" | tail -4
echo
ls -l "$P.zip"
du -sh "$P"
