#!/bin/bash
# list_overrides.sh: the recompiled functions a game code dylib's host replaces, from the host executable that was built for it
#   list_overrides.sh <host executable> <out file>
# In a build whose recompiled code is in a dylib (build.sh --game-code-dylib) the executable holds no recompiled function, so every
# sub_XXXXXXXX it defines is an override (X86_FUNC in src/ and sdk/runtime/). One name a line, sorted. The dylib's link reads the
# list (app/game_build.cpp, WriteAliasResponse) to leave those names to the executable; a name left out of it would make the dylib
# define a function that the executable's replacement can never win over.
set -e
EXE=$1; OUT=$2
[ -f "$EXE" ] && [ -n "$OUT" ] || { sed -n 2,3p "$0"; exit 2; }
NM=${NM:-nm}
"$NM" -g --defined-only "$EXE" | awk '$3 ~ /^_sub_[0-9A-F]{8}$/ { print substr($3, 2) }' | sort -u > "$OUT.tmp"
mv "$OUT.tmp" "$OUT"
echo "overrides: $(wc -l < "$OUT" | tr -d ' ') in $OUT"
