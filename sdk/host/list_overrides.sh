#!/bin/bash
# list_overrides.sh: the recompiled functions a game code dylib's host replaces, from the host executable that was built for it
#   list_overrides.sh <host executable> <out file>      NM names the nm to use (default nm: Xcode's, or llvm-nm; it must read arm64 Mach-O)
# In a build whose recompiled code is in a dylib (build.sh --game-code-dylib) the executable holds no recompiled function, so every
# sub_XXXXXXXX it defines is an override (X86_FUNC in src/ and sdk/runtime/). One name a line, sorted. The dylib's link reads the
# list (app/game_build.cpp, WriteAliasResponse) to leave those names to the executable; a name left out of it would make the dylib
# define a function that the executable's replacement can never win over (dispatch::Load refuses such a dylib). An empty list is an error:
# the executable always has overrides, so an empty one means nm did not read the file.
set -e -o pipefail
EXE=$1; OUT=$2
[ -f "$EXE" ] && [ -n "$OUT" ] || { sed -n 2,3p "$0"; exit 2; }
NM=${NM:-nm}
command -v "$NM" > /dev/null || { echo "list_overrides: no $NM (Xcode's command line tools have one, or set NM=<llvm-nm>)" >&2; exit 1; }
"$NM" -g --defined-only "$EXE" > "$OUT.nm" || { echo "list_overrides: $NM could not read $EXE" >&2; rm -f "$OUT.nm"; exit 1; }
awk '$3 ~ /^_sub_[0-9A-F]{8}$/ { print substr($3, 2) }' "$OUT.nm" | sort -u > "$OUT.tmp"
rm -f "$OUT.nm"
if [ ! -s "$OUT.tmp" ]; then
    rm -f "$OUT.tmp"
    echo "list_overrides: $EXE defines no sub_XXXXXXXX function: it is not a host built with --game-code-dylib, or $NM does not read arm64 Mach-O" >&2
    exit 1
fi
mv "$OUT.tmp" "$OUT"
echo "overrides: $(wc -l < "$OUT" | tr -d ' ') in $OUT"
