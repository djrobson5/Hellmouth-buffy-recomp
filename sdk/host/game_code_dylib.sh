#!/bin/bash
# game_code_dylib.sh: the second half of sdk/host/build.sh --game-code-dylib, which runs it after the executable is built.
#   -r repo   -g generated folder of the image   -o out folder   -n exe name
# Makes <out>/compiler, laid out as the first start finds its tools (app/game_build.h, Layout::Compiler), and builds the dylib from the
# generated sources with the first start's own build code (tools/first_start_check.cpp --sources over app/game_build.cpp):
#   compiler/clang, compiler/ld64.lld   scripts that run the llvm-mingw toolchain at $LLVM_MINGW (default ~/Projects/toolchains/llvm-mingw),
#                                       which is built from source (its lld has the Mach-O port); a release carries its own copies.
#                                       With MAC_DEPS=<folder> (tools/mac/build_toolchain.sh): copies of its bin/clang and bin/ld64.lld,
#                                       the real programs a release carries, and no need for $LLVM_MINGW
#   compiler/include/{cpu,freestanding} the headers the generated code is compiled against
#   compiler/overrides.txt              the functions the executable replaces (list_overrides.sh), which the dylib must not define
#   game_code/first_start_check         the build, compiled here
#   build/<name>.dylib                  the game's code (the exe loads it from there), objects in cache/game-code, log in logs/compile.log
set -e
while getopts r:g:o:n: opt; do
    case $opt in r) REPO=$OPTARG ;; g) GEN=$OPTARG ;; o) OUT=$OPTARG ;; n) NAME=$OPTARG ;; *) sed -n 2,3p "$0"; exit 2 ;; esac
done
[ -n "$REPO" ] && [ -n "$GEN" ] && [ -n "$OUT" ] && [ -n "$NAME" ] || { sed -n 2,3p "$0"; exit 2; }
HERE=$(dirname "$(realpath "$0")")
REPO=$(realpath "$REPO"); GEN=$(realpath "$GEN"); OUT=$(realpath "$OUT"); RT=$REPO/sdk/runtime
CODE=$OUT/game_code; C=$OUT/compiler
MAC_DEPS=${MAC_DEPS:-}
if [ -n "$MAC_DEPS" ]; then
    MAC_DEPS=$(cd "$MAC_DEPS" 2> /dev/null && pwd -P) || { echo "MAC_DEPS is not a folder" >&2; exit 1; }
    [ -x "$MAC_DEPS/bin/clang" ] && [ -x "$MAC_DEPS/bin/ld64.lld" ] || { echo "MAC_DEPS=$MAC_DEPS has no bin/clang and bin/ld64.lld (tools/mac/build_toolchain.sh makes them)" >&2; exit 1; }
else
    TC_DIR=${LLVM_MINGW:-$HOME/Projects/toolchains/llvm-mingw}
    [ -d "$TC_DIR/bin" ] || { echo "no toolchain at $TC_DIR (LLVM_MINGW): the llvm-mingw built from source, whose lld has the Mach-O port (see the README)" >&2; exit 1; }
    TC=$(realpath "$TC_DIR")/bin
    [ -x "$TC/clang" ] && [ -x "$TC/lld" ] || { echo "LLVM_MINGW=$TC_DIR has no clang and lld: the toolchain built from source (see the README)" >&2; exit 1; }
fi
JOBS=${JOBS:-$(sysctl -n hw.ncpu)}

rm -rf "$C/include"  # what the headers were, not what they are
mkdir -p "$C/include/cpu" "$C/include/freestanding"
cp "$RT/cpu/x86_ops.h" "$RT/cpu/x86_context.h" "$C/include/cpu/"
cp "$RT"/cpu/freestanding/* "$C/include/freestanding/"
if [ -n "$MAC_DEPS" ]; then
    # the real programs (the first start salts its object reuse with their bytes, so a new compiler gives new objects), no wrappers
    rm -f "$C/clang" "$C/ld64.lld"
    cp -f "$MAC_DEPS/bin/clang" "$C/clang"; cp -f "$MAC_DEPS/bin/ld64.lld" "$C/ld64.lld"
    chmod u+w,+x "$C/clang" "$C/ld64.lld"
else
    # The tools are scripts that run the toolchain. The first start salts its object reuse with a tool's bytes, and a script says nothing of the
    # program it runs, so each carries the real program's size and date: a new compiler gives new objects.
    clang_id=$(stat -f "%z %m" "$(realpath "$TC/clang")")
    lld_id=$(stat -f "%z %m" "$(realpath "$TC/lld")")
    { echo '#!/bin/sh'; echo "# clang $clang_id"; printf 'exec %q "$@"\n' "$TC/clang"; } > "$C/clang"
    { echo '#!/bin/sh'; echo "# lld $lld_id"; printf 'exec %q -flavor darwin "$@"\n' "$TC/lld"; } > "$C/ld64.lld"
    chmod +x "$C/clang" "$C/ld64.lld"
fi
"$HERE/list_overrides.sh" "$OUT/$NAME" "$C/overrides.txt"

# the first start's build code, for this Mac: with the Mach-O flags (game_code_flags.h) it builds a dylib
mkdir -p "$CODE"
${CXX:-clang++} -std=c++20 -O1 -Wno-deprecated-declarations -I"$RT" -I"$CODE" -I"$GEN" \
    "$REPO/tools/first_start_check.cpp" "$RT/app/game_build.cpp" "$RT/kernel/disc.cpp" "$RT/kernel/crypto.cpp" -o "$CODE/first_start_check"
"$CODE/first_start_check" --app "$OUT" --data "$OUT" --sources "$GEN" --slots "$JOBS"
echo "built $(ls "$OUT"/build/*.dylib)"
