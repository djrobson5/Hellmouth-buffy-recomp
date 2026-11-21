#!/bin/bash
# Checks that what the Mac release would ship runs on macOS 13.0 and later and needs nothing from Homebrew:
#   tools/mac/check_mac_deps.sh <folder> [licence folder]
# <folder> is one of
#   the output of tools/mac/build_toolchain.sh and build_libs.sh (default ~/Projects/toolchains/mac-deps): bin/clang, bin/ld64.lld, lib/*.dylib,
#       and the static libraries lib/*.a; its licenses/ are the ones checked
#   a build of sdk/host/build.sh (a folder with the executable, or a .app): the executable, Frameworks/*.dylib, compiler/clang and
#       compiler/ld64.lld, build/*.dylib (the game code); the licences are <folder>/licenses, or the second argument, or $MAC_DEPS/licenses
# For each Mach-O that would ship: arm64; every minos 13.0 or lower (every slice); it loads only /usr/lib/*, /System/*, and @rpath/,
# @executable_path/ and @loader_path/ names, each @rpath one a file that is shipped; its search paths (LC_RPATH) are all @-relative; nothing
# names /opt/homebrew or /usr/local, and no /opt/homebrew string is inside it; it is signed (ad hoc is enough). Licences: each component the files come from has its licence and a
# SOURCE.txt with its version and commit. NAME (default Buffy) is the executable. Opens no window, runs no game. Exit 1 on a FAIL.
D=${1:-$HOME/Projects/toolchains/mac-deps}
[ -d "$D" ] || { echo "FAIL  no folder $D"; exit 1; }
D=$(cd "$D" && pwd -P)
NAME=${NAME:-Buffy}
fail=0; TMPS=(); trap 'for t in "${TMPS[@]+"${TMPS[@]}"}"; do rm -rf -- "$t"; done' EXIT
chk() { if [ "$1" = 0 ]; then echo "PASS  $2"; else echo "FAIL  $2"; fail=1; fi; }
vcmp_le() { [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1)" = "$2" ]; }   # $1 <= $2

files=(); statics=(); libdirs=(); need=()
if [ -x "$D/bin/clang" ] || [ -d "$D/lib" ]; then
    LAYOUT=deps; LIC=$D/licenses
    for f in "$D"/bin/clang "$D"/bin/ld64.lld "$D"/lib/*.dylib; do [ -f "$f" ] && [ ! -L "$f" ] && files+=("$f"); done
    for f in "$D"/lib/*.a; do [ -f "$f" ] && statics+=("$f"); done
    libdirs=("$D/lib"); need=(llvm sdl3 moltenvk glslang spirv-tools spirv-headers vulkan-headers)
else
    LAYOUT=app
    if [[ "$D" == *.app ]]; then X=$D/Contents/MacOS; FW=$D/Contents/Frameworks; else X=$D; FW=$D/Frameworks; fi
    LIC=${2:-}; [ -z "$LIC" ] && [ -d "$D/licenses" ] && LIC=$D/licenses; [ -z "$LIC" ] && [ -n "${MAC_DEPS:-}" ] && LIC=$MAC_DEPS/licenses
    [ -f "$X/$NAME" ] && files+=("$X/$NAME")
    for f in "$FW"/*.dylib "$D"/compiler/clang "$D"/compiler/ld64.lld "$D"/build/*.dylib; do [ -f "$f" ] && [ ! -L "$f" ] && files+=("$f"); done
    libdirs=("$FW" "$X"); need=(sdl3 moltenvk glslang spirv-tools)
    [ -f "$D/compiler/clang" ] && need+=(llvm)
    [ -f "$X/$NAME" ] || echo "FAIL  no executable $X/$NAME"
    [ -f "$X/$NAME" ] || fail=1
fi
echo "layout: $LAYOUT, $D: ${#files[@]} programs and libraries, ${#statics[@]} static libraries"
[ ${#files[@]} -gt 0 ] || { echo "FAIL  nothing to check"; exit 1; }

in_ship() {   # an @rpath/@loader_path/@executable_path name: is that file one of the shipped files, or next to them?
    local base d; base=$(basename "$1")
    for d in "${libdirs[@]}"; do [ -f "$d/$base" ] && return 0; done
    return 1
}
for f in "${files[@]}"; do
    rel=${f#"$D"/}
    archs=$(lipo -archs "$f" 2> /dev/null); [ "$archs" = arm64 ]; chk $? "$rel: arm64 only (${archs:-not a Mach-O})"
    [ "$archs" = arm64 ] || continue
    # LC_BUILD_VERSION (or the older LC_VERSION_MIN_MACOSX) of every slice
    minos=$(vtool -show-build "$f" 2> /dev/null | awk '$1 == "minos" { print $2 }'); [ -n "$minos" ]
    bad=""; for m in $minos; do vcmp_le "$m" 13.0 || bad="$bad $m"; done
    [ -n "$minos" ] && [ -z "$bad" ]; chk $? "$rel: minos $(echo $minos | tr ' ' ',') (13.0 or lower)"
    deps=$(otool -L "$f" | tail -n +2 | awk '{print $1}')
    own=$(otool -D "$f" 2> /dev/null | tail -n +2)   # a library's own name
    left=""
    for dep in $deps; do
        case $dep in
            /usr/lib/*|/System/*) ;;
            @rpath/*|@executable_path/*|@loader_path/*) in_ship "$dep" || [ "$dep" = "$own" ] || left="$left $dep(not shipped)" ;;
            *) left="$left $dep" ;;
        esac
    done
    rp=$(otool -l "$f" | awk '/cmd LC_RPATH/ { getline; getline; print $2 }' | grep -Ev '^@(loader|executable)_path' || true)
    [ -z "$left" ] && [ -z "$rp" ]; chk $? "$rel: loads only the system and shipped @ names; search paths all @-relative (outside:${left:- none}; search paths:${rp:- none})"
    otool -l "$f" | grep -E '^ +(name|path) ' | grep -Eq '/opt/homebrew|/usr/local'; [ $? != 0 ]; chk $? "$rel: no load command names /opt/homebrew or /usr/local"
    strings -a "$f" | grep -q '/opt/homebrew'; [ $? != 0 ]; chk $? "$rel: no /opt/homebrew string inside it (a search path or a library name)"
    codesign --verify "$f" > /dev/null 2>&1; chk $? "$rel: signed"
done
for f in "${statics[@]}"; do
    rel=${f#"$D"/}
    minos=$(otool -l "$f" 2> /dev/null | awk '$1 == "minos" { print $2 }' | sort -uV | tr '\n' ' ')
    bad=""; for m in $minos; do vcmp_le "$m" 13.0 || bad="$bad $m"; done
    [ -n "$minos" ] && [ -z "$bad" ]; chk $? "$rel: static library, minos ${minos:-none} (13.0 or lower)"
done

# MoltenVK as the renderer opens it, without a window: an instance and the GPU (tools/mac/probe_moltenvk.c, built here with Xcode's clang)
MVK=; for d in "${libdirs[@]}"; do [ -f "$d/libMoltenVK.dylib" ] && MVK=$d/libMoltenVK.dylib && break; done
HDR=; for d in "$D/include" "${MAC_DEPS:-/nonexistent}/include"; do [ -f "$d/vulkan/vulkan.h" ] && HDR=$d && break; done
if [ -n "$MVK" ] && [ -n "$HDR" ]; then
    P=$(mktemp -d) && TMPS+=("$P")
    if /usr/bin/clang -arch arm64 -I"$HDR" "$(dirname "$0")/probe_moltenvk.c" -o "$P/probe" 2> "$P/cc.err"; then
        "$P/probe" "$MVK" > "$P/out.txt" 2>&1; code=$?
        chk $code "headless MoltenVK probe: dlopen, create an instance, list the GPU ($(grep -E '^(instance version|GPU )' "$P/out.txt" | tr '\n' ';'))"
    else
        chk 1 "headless MoltenVK probe did not build: $(head -3 "$P/cc.err")"
    fi
else
    echo "SKIP  headless MoltenVK probe (needs libMoltenVK.dylib and the Vulkan headers: MAC_DEPS)"
fi

# glslang and SPIRV-Tools as the executable links them (static, the optimizer on): GLSL to SPIR-V (tools/mac/probe_glslang.cpp)
GL=; for d in "$D" "${MAC_DEPS:-/nonexistent}"; do [ -f "$d/lib/libglslang.a" ] && GL=$d && break; done
if [ -n "$GL" ]; then
    P=$(mktemp -d) && TMPS+=("$P")
    if /usr/bin/clang++ -std=c++17 -arch arm64 -mmacosx-version-min=13.0 -I"$GL/include" -I"$GL/include/glslang" "$(dirname "$0")/probe_glslang.cpp" \
        -L"$GL/lib" -lglslang -lSPIRV -lglslang-default-resource-limits -lSPIRV-Tools-opt -lSPIRV-Tools -o "$P/probe_glslang" 2> "$P/cc.err"; then
        "$P/probe_glslang" > "$P/out.txt" 2>&1; chk $? "glslang probe: GLSL to optimized SPIR-V with the static libraries ($(head -1 "$P/out.txt"))"
    else
        chk 1 "glslang probe did not build or link: $(head -3 "$P/cc.err")"
    fi
else
    echo "SKIP  glslang probe (needs lib/libglslang.a: MAC_DEPS)"
fi

# the licences
if [ -z "$LIC" ] || [ ! -d "$LIC" ]; then chk 1 "licences: no folder to check (<folder>/licenses, a second argument or MAC_DEPS)"; else
    for c in "${need[@]}"; do
        n=$(ls "$LIC/$c" 2> /dev/null | grep -v '^SOURCE.txt$' | wc -l | tr -d ' ')
        ok=1; [ "$n" -gt 0 ] && [ -f "$LIC/$c/SOURCE.txt" ] && grep -q '^commit: [0-9a-f]\{40\}$' "$LIC/$c/SOURCE.txt" && grep -q '^version: .' "$LIC/$c/SOURCE.txt" && ok=0
        chk $ok "licences: $c has $n licence file(s) and a SOURCE.txt with a version and a commit ($(sed -n 's/^version: //p' "$LIC/$c/SOURCE.txt" 2> /dev/null | head -1))"
    done
fi
[ $fail = 0 ] && echo "ALL PASSED" || echo "FAILED"
exit $fail
