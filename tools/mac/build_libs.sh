#!/bin/bash
# build_libs.sh: the libraries the Mac app runs on, built from source for macOS 13.0 and later, arm64:
#   SDL3, MoltenVK (dynamic: SDL3 is linked by name, and MoltenVK is opened by the renderer as @rpath/libMoltenVK.dylib),
#   glslang with SPIRV-Tools and their optimizer (static: only the executable uses them, so they are linked into it, which saves five
#   dylibs and their version-numbered names; Apache-2.0 and BSD licences, no obligation a static link adds), and the build time headers
#   (Vulkan-Headers, SPIRV-Headers).
#   tools/mac/build_libs.sh [-o <out>] [-s <work>]      out: default ~/Projects/toolchains/mac-deps; work: ~/Projects/toolchains/mac-deps-src
# The versions are those Homebrew had when this was written (brew list --versions sdl3 molten-vk glslang spirv-tools vulkan-headers).
# Leaves in <out>: lib/libSDL3.dylib (and .0.dylib), lib/libMoltenVK.dylib, lib/*.a, include/, and the licences in licenses/<name>/. Skips
# what is already built from the pin (FORCE=1 builds all again). MoltenVK needs Xcode and fetches its own sources (fetchDependencies).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
source "$HERE/mac_deps_common.sh"
parse_args "$@"
KHR=https://github.com/KhronosGroup

# name, tag, commit
VKH_TAG=vulkan-sdk-1.4.363.0;  VKH_COMMIT=6802bb4733b63ed5efd3adb308a6c885ef180ea1     # Vulkan-Headers   (Homebrew vulkan-headers 1.4.363.0)
SPH_TAG=vulkan-sdk-1.4.363.0;  SPH_COMMIT=496543121ce6419f23d6fa5d7194ba66c36212d2     # SPIRV-Headers    (the one Homebrew's spirv-tools is built with)
SPT_TAG=vulkan-sdk-1.4.363.0;  SPT_COMMIT=ef96ed763b43b59b33b31b362f09a02b729fa1c9     # SPIRV-Tools      (Homebrew spirv-tools 1.4.363.0)
GLS_TAG=16.6.0;                GLS_COMMIT=e1b562a8bed273a02f30b59b66a5d499793cede5     # glslang          (Homebrew glslang 16.6.0)
SDL_TAG=release-3.4.18;        SDL_COMMIT=829a65d769d935c4852f8159e964312c0957260a     # SDL3             (Homebrew sdl3 3.4.18 is what is installed here)
MVK_TAG=v1.4.2;                MVK_COMMIT=db66022459ffb663aa2b50f6b018bc2e124f5edf     # MoltenVK         (Homebrew molten-vk 1.4.2)
STATIC_LIBS="libglslang.a libSPIRV.a libglslang-default-resource-limits.a libSPIRV-Tools-opt.a libSPIRV-Tools.a"

skip() { [ -z "${FORCE:-}" ] && stamp_current "$1" "$2" && shift 2 && for f in "$@"; do [ -e "$OUT/$f" ] || return 1; done; }

# cmake_build <name> <source dir> <extra cmake options...>: configure, build, install into <out>
cmake_build() {
    local name=$1 src=$2; shift 2
    local b=$WORK/build-$name
    rm -rf "$b"
    cmake -S "$src" -B "$b" "${CMAKE_COMMON[@]}" "$@"
    no_homebrew "$b"
    cmake --build "$b" -j "$JOBS"
    cmake --install "$b" > /dev/null
}

# Vulkan-Headers: headers only (the app's vk.cpp, imgui's Vulkan backend)
PIN="$VKH_TAG $VKH_COMMIT"
if skip vulkan-headers "$PIN" include/vulkan/vulkan.h; then echo "vulkan-headers: $VKH_TAG already built"; else
    T0=$(now); fetch Vulkan-Headers $KHR/Vulkan-Headers.git "$VKH_TAG" "$VKH_COMMIT"
    cmake_build vulkan-headers "$WORK/Vulkan-Headers"
    record vulkan-headers "${VKH_TAG#vulkan-sdk-}" $KHR/Vulkan-Headers "$VKH_COMMIT" "$PIN" "$WORK/Vulkan-Headers/LICENSE.md" "$WORK/Vulkan-Headers/LICENSES/Apache-2.0.txt" "$WORK/Vulkan-Headers/LICENSES/MIT.txt"
    echo "vulkan-headers: $(elapsed "$T0")"
fi

# SPIRV-Headers: what SPIRV-Tools and glslang are built against
PIN="$SPH_TAG $SPH_COMMIT"
if skip spirv-headers "$PIN" include/spirv/unified1/spirv.hpp; then echo "spirv-headers: $SPH_TAG already built"; else
    T0=$(now); fetch SPIRV-Headers $KHR/SPIRV-Headers.git "$SPH_TAG" "$SPH_COMMIT"
    cmake_build spirv-headers "$WORK/SPIRV-Headers" -DSPIRV_HEADERS_ENABLE_TESTS=OFF -DSPIRV_HEADERS_ENABLE_INSTALL=ON
    record spirv-headers "${SPH_TAG#vulkan-sdk-}" $KHR/SPIRV-Headers "$SPH_COMMIT" "$PIN" "$WORK/SPIRV-Headers/LICENSE"
    echo "spirv-headers: $(elapsed "$T0")"
fi

# SPIRV-Tools (static): glslang's optimizer
PIN="$SPT_TAG $SPT_COMMIT static headers=$SPH_COMMIT"
if skip spirv-tools "$PIN" lib/libSPIRV-Tools.a lib/libSPIRV-Tools-opt.a; then echo "spirv-tools: $SPT_TAG already built"; else
    T0=$(now); fetch SPIRV-Tools $KHR/SPIRV-Tools.git "$SPT_TAG" "$SPT_COMMIT"
    cmake_build spirv-tools "$WORK/SPIRV-Tools" -DBUILD_SHARED_LIBS=OFF -DSPIRV_TOOLS_BUILD_STATIC=ON -DSPIRV_BUILD_COMPRESSION=OFF \
        -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON -DSPIRV_WERROR=OFF -DSPIRV-Headers_SOURCE_DIR="$WORK/SPIRV-Headers"
    record spirv-tools "${SPT_TAG#vulkan-sdk-}" $KHR/SPIRV-Tools "$SPT_COMMIT" "$PIN" "$WORK/SPIRV-Tools/LICENSE"
    echo "spirv-tools: $(elapsed "$T0")"
fi

# glslang (static, with the optimizer: ENABLE_OPT, the installed SPIRV-Tools)
PIN="$GLS_TAG $GLS_COMMIT static opt tools=$SPT_COMMIT"
if skip glslang "$PIN" lib/libglslang.a lib/libSPIRV.a lib/libglslang-default-resource-limits.a; then echo "glslang: $GLS_TAG already built"; else
    T0=$(now); fetch glslang $KHR/glslang.git "$GLS_TAG" "$GLS_COMMIT"
    cmake_build glslang "$WORK/glslang" -DBUILD_SHARED_LIBS=OFF -DENABLE_OPT=ON -DALLOW_EXTERNAL_SPIRV_TOOLS=ON -DENABLE_GLSLANG_BINARIES=OFF \
        -DGLSLANG_TESTS=OFF -DENABLE_CTEST=OFF -DBUILD_EXTERNAL=OFF
    record glslang "$GLS_TAG" $KHR/glslang "$GLS_COMMIT" "$PIN" "$WORK/glslang/LICENSE.txt"
    echo "glslang: $(elapsed "$T0")"
fi
for l in $STATIC_LIBS; do [ -f "$OUT/lib/$l" ] || { echo "$OUT/lib/$l is missing after the glslang build: STATIC_LIBS in this script is out of date" >&2; exit 1; }; done

# SDL3 (dynamic): zlib licence
PIN="$SDL_TAG $SDL_COMMIT shared"
if skip sdl3 "$PIN" lib/libSDL3.0.dylib include/SDL3/SDL.h; then echo "sdl3: $SDL_TAG already built"; else
    T0=$(now); fetch SDL https://github.com/libsdl-org/SDL.git "$SDL_TAG" "$SDL_COMMIT"
    cmake_build sdl3 "$WORK/SDL" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_INSTALL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_TEST_LIBRARY=OFF \
        -DSDL_INSTALL_DOCS=OFF -DSDL_DEPS_SHARED=ON -DSDL_RPATH=OFF
    record sdl3 "${SDL_TAG#release-}" https://github.com/libsdl-org/SDL "$SDL_COMMIT" "$PIN" "$WORK/SDL/LICENSE.txt"
    echo "sdl3: $(elapsed "$T0")"
fi

# MoltenVK (dynamic, arm64 only): fetchDependencies clones SPIRV-Cross and the rest at the commits ExternalRevisions names and builds them
# with Xcode; the dylib is then built for 13.0 (its own project says 12.0) and for arm64 alone
PIN="$MVK_TAG $MVK_COMMIT dylib arm64"
if skip moltenvk "$PIN" lib/libMoltenVK.dylib; then echo "moltenvk: $MVK_TAG already built"; else
    T0=$(now); fetch MoltenVK $KHR/MoltenVK.git "$MVK_TAG" "$MVK_COMMIT"
    M=$WORK/MoltenVK
    ( cd "$M" && ./fetchDependencies --macos > "$WORK/moltenvk-fetch.log" 2>&1 )
    rm -rf "$M/Package"
    ( cd "$M" && xcodebuild build -quiet -project MoltenVKPackaging.xcodeproj -scheme "MoltenVK Package (macOS only)" \
        -destination "generic/platform=macOS" -configuration Release ARCHS=arm64 ONLY_ACTIVE_ARCH=NO \
        MACOSX_DEPLOYMENT_TARGET=$DEPLOYMENT_TARGET > "$WORK/moltenvk-build.log" 2>&1 ) || { tail -30 "$WORK/moltenvk-build.log" >&2; exit 1; }
    DYL=$(find "$M/Package/Release" -name libMoltenVK.dylib -path '*macOS*' | head -1)
    [ -n "$DYL" ] || { echo "no libMoltenVK.dylib under $M/Package/Release" >&2; exit 1; }
    cp -f "$DYL" "$OUT/lib/libMoltenVK.dylib"; chmod u+w "$OUT/lib/libMoltenVK.dylib"
    install_name_tool -id @rpath/libMoltenVK.dylib "$OUT/lib/libMoltenVK.dylib" 2> /dev/null
    strip -x "$OUT/lib/libMoltenVK.dylib"
    codesign --force -s - "$OUT/lib/libMoltenVK.dylib" 2> /dev/null
    # the dependencies' commits (ExternalRevisions) and licences, with MoltenVK's own
    revs=$(cd "$M/ExternalRevisions" && for f in *_repo_revision; do echo "${f%_repo_revision}=$(cat "$f")"; done | tr '\n' ' ')
    lic=("$M/LICENSE")
    for d in "$M"/External/*; do
        for f in "$d"/LICENSE* "$d"/COPYING*; do [ -f "$f" ] && lic+=("$f"); done
    done
    record moltenvk "${MVK_TAG#v}" $KHR/MoltenVK "$MVK_COMMIT" "$PIN" "${lic[@]}"
    echo "external: $revs" >> "$OUT/licenses/moltenvk/SOURCE.txt"
    echo "moltenvk: $(elapsed "$T0")"
fi

echo "done: $OUT"; cat "$OUT/VERSIONS.txt"
for f in "$OUT"/lib/libSDL3.0.dylib "$OUT"/lib/libMoltenVK.dylib $(for l in $STATIC_LIBS; do echo "$OUT/lib/$l"; done); do du -h "$f" | cut -f1 | tr '\n' ' '; echo "$(basename "$f")"; done
