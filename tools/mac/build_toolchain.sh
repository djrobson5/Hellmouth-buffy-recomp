#!/bin/bash
# build_toolchain.sh: the compiler the game's first start runs on the player's Mac, built from source: clang and lld (as ld64.lld, the Mach-O
# linker), AArch64 only, static (nothing but libSystem and libc++ at run time), for macOS 13.0 and later.
#   tools/mac/build_toolchain.sh [-o <out>] [-s <work>]      out: default ~/Projects/toolchains/mac-deps; work: ~/Projects/toolchains/mac-deps-src
# Leaves <out>/bin/clang and <out>/bin/ld64.lld (real, stripped, ad hoc signed binaries) and the licences in <out>/licenses/llvm/. Skips what
# is already built from the pinned version. The tools aren't wrappers: a release ships these files. Clang is told -nostdinc by the first start,
# so no lib/clang/<version>/include is needed. The prefix and the C library of the build come from Xcode (/usr/bin/clang): nothing from Homebrew.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
source "$HERE/mac_deps_common.sh"
parse_args "$@"

# the same LLVM as the llvm-mingw the Windows toolchain is built from (llvm-mingw 20261104)
LLVM_URL=https://github.com/llvm/llvm-project
LLVM_TAG=llvmorg-23.1.5
LLVM_COMMIT=392dfff97d29b59d77e5cac6d4b6ef2585fe7117
PIN="$LLVM_TAG $LLVM_COMMIT target=$DEPLOYMENT_TARGET static zstd=off zlib=off arm64"

if stamp_current llvm "$PIN" && [ -x "$OUT/bin/clang" ] && [ -x "$OUT/bin/ld64.lld" ]; then
    echo "llvm: $LLVM_TAG already built in $OUT/bin"
    exit 0
fi

T0=$(now)
fetch llvm-project "$LLVM_URL" "$LLVM_TAG" "$LLVM_COMMIT"
SRC=$WORK/llvm-project; B=$WORK/build-llvm
rm -rf "$B"; mkdir -p "$B"
# the build of the prior test (3:48 here): no zstd, zlib, libxml2 or libedit (nothing but libSystem and libc++ to link), no static analyzer
cmake -S "$SRC/llvm" -B "$B" "${CMAKE_COMMON[@]}" \
    -DLLVM_ENABLE_PROJECTS="clang;lld" -DLLVM_TARGETS_TO_BUILD=AArch64 -DLLVM_LINK_LLVM_DYLIB=OFF -DBUILD_SHARED_LIBS=OFF \
    -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_LIBEDIT=OFF \
    -DCLANG_ENABLE_STATIC_ANALYZER=OFF -DCLANG_ENABLE_ARCMT=OFF -DCLANG_ENABLE_CIR=OFF \
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF \
    -DCLANG_INCLUDE_TESTS=OFF -DCLANG_INCLUDE_DOCS=OFF -DLLD_INCLUDE_TESTS=OFF \
    -DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_ENABLE_BINDINGS=OFF -DLLVM_ENABLE_OCAMLDOC=OFF -DLLVM_ENABLE_PLUGINS=OFF
no_homebrew "$B"
cmake --build "$B" --target clang lld -j "$JOBS"

# real binaries: the driver (bin/clang is a link to clang-23) and lld, which is a Mach-O linker when it is called ld64.lld
cp -f "$(realpath "$B/bin/clang")" "$OUT/bin/clang"
cp -f "$(realpath "$B/bin/lld")" "$OUT/bin/ld64.lld"
chmod u+w "$OUT/bin/clang" "$OUT/bin/ld64.lld"
for f in clang ld64.lld; do
    strip -x "$OUT/bin/$f"
    codesign --force -s - "$OUT/bin/$f" 2> /dev/null   # strip invalidates the linker's signature
    codesign --verify "$OUT/bin/$f"
done
# it must link and run as it will in a release: a Mach-O dylib from an object, by the name ld64.lld
T=$(mktemp -d "$WORK/smoke.XXXXXX")
printf 'int answer(void) { return 42; }\n' > "$T/a.c"
"$OUT/bin/clang" --target=arm64-apple-macos$DEPLOYMENT_TARGET -nostdinc -ffreestanding -O1 -c "$T/a.c" -o "$T/a.o"
"$OUT/bin/ld64.lld" -dylib -arch arm64 -platform_version macos $DEPLOYMENT_TARGET $DEPLOYMENT_TARGET -install_name @rpath/a.dylib "$T/a.o" -o "$T/a.dylib"
file "$T/a.dylib" | grep -q 'arm64'
rm -rf "$T"

record llvm "${LLVM_TAG#llvmorg-} (clang, lld)" "$LLVM_URL" "$LLVM_COMMIT" "$PIN" \
    "$SRC/LICENSE.TXT" "$SRC/llvm/LICENSE.TXT" "$SRC/clang/LICENSE.TXT" "$SRC/lld/LICENSE.TXT" \
    "$SRC/llvm/include/llvm/Support/LICENSE.TXT" "$SRC/llvm/lib/Support/COPYRIGHT.regex"
echo "llvm: built in $(elapsed "$T0"): clang $(du -h "$OUT/bin/clang" | cut -f1), ld64.lld $(du -h "$OUT/bin/ld64.lld" | cut -f1) -> $OUT/bin"
