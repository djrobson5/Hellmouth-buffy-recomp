#!/bin/bash
# game_code_dylib.sh: the second half of sdk/host/build.sh --game-code-dylib, which runs it after the executable is built.
#   -r repo   -g generated folder of the image   -o out folder   -n exe name
# Makes <out>/compiler, laid out as the first start finds its tools (app/game_build.h, Layout::Compiler), and builds the dylib from the
# generated sources with the first start's own build code (tools/first_start_check.cpp --sources over app/game_build.cpp):
#   compiler/clang, compiler/ld64.lld   scripts that run the llvm-mingw toolchain at $LLVM_MINGW (default ~/Projects/toolchains/llvm-mingw),
#                                       which is built from source (its lld has the Mach-O port); a release carries its own copies
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
TC=$(realpath "${LLVM_MINGW:-$HOME/Projects/toolchains/llvm-mingw}")/bin
[ -x "$TC/clang" ] && [ -x "$TC/lld" ] || { echo "LLVM_MINGW=$TC has no clang and lld: the toolchain built from source (see the README)" >&2; exit 1; }
JOBS=${JOBS:-$(sysctl -n hw.ncpu)}

mkdir -p "$C/include/cpu" "$C/include/freestanding"
cp "$RT/cpu/x86_ops.h" "$RT/cpu/x86_context.h" "$C/include/cpu/"
cp "$RT"/cpu/freestanding/* "$C/include/freestanding/"
printf '#!/bin/sh\nexec "%s" "$@"\n' "$TC/clang" > "$C/clang"
printf '#!/bin/sh\nexec "%s" -flavor darwin "$@"\n' "$TC/lld" > "$C/ld64.lld"
chmod +x "$C/clang" "$C/ld64.lld"
"$HERE/list_overrides.sh" "$OUT/$NAME" "$C/overrides.txt"

# the first start's build code, for this Mac: with the Mach-O flags (game_code_flags.h) it builds a dylib
mkdir -p "$CODE"
${CXX:-clang++} -std=c++20 -O1 -Wno-deprecated-declarations -I"$RT" -I"$CODE" -I"$GEN" \
    "$REPO/tools/first_start_check.cpp" "$RT/app/game_build.cpp" "$RT/kernel/disc.cpp" "$RT/kernel/crypto.cpp" -o "$CODE/first_start_check"
"$CODE/first_start_check" --app "$OUT" --data "$OUT" --sources "$GEN" --slots "$JOBS"
echo "built $(ls "$OUT"/build/*.dylib)"
