#!/bin/bash
# build.sh: the game as a macOS program with a window, through SDL and Vulkan on Metal:
#   brew install sdl3 molten-vk vulkan-headers glslang
#   -r repo (holding sdk/, src/ and assets/ with the xbe)   -g generated folder of the image   -o out folder
#   -n exe name (the game restarts itself by this name)   -G folder for the generated objects, shared with other builds
# The harness's build is used, without its stand ins for the window, controllers and sound; its reference renderer stays
# as the picture when Vulkan can't start.
#   MAC_DEPS=<folder>   (environment) the libraries and the compiler built from source by tools/mac/build_toolchain.sh and build_libs.sh,
#                       instead of Homebrew's: macOS 13.0 and later, nothing from Homebrew, SDL3 and MoltenVK as dylibs copied to
#                       <out>/Frameworks (the executable finds them there), glslang and SPIRV-Tools linked in. With --game-code-dylib, the
#                       compiler/ folder gets that clang and ld64.lld (real files), not scripts that run $LLVM_MINGW, and the Mac builds of
#                       xbe_recomp and buffy_config are made for macOS 13.0. Unset, the build is
#                       the one with Homebrew's libraries, as always. tools/mac/check_mac_deps.sh checks the result.
#   --game-code-dylib   (anywhere on the line) the executable without the game's code: it loads <out>/build/<name>.dylib (or --game-code)
#                       as the Windows build loads Buffy.dll. Builds, in <out>: the executable; game_code/ (the ABI token and flags,
#                       sdk/cmake/xbe_abi.cmake); compiler/ (what the first start builds with: clang and ld64.lld from $LLVM_MINGW,
#                       the headers, overrides.txt (the executable's overrides), xbe_recomp, buffy_config, sdk_names.txt, xdk/4432);
#                       and, for development, the dylib itself
#                       from the generated sources (build/<name>.dylib, tools/first_start_check.cpp --sources). Use another <out> than
#                       the default build's: the objects differ. tools/check_game_code_dylib.sh checks the result.
set -e
DYLIB=; HOST_EXTRA=; LINK_EXTRA=
for a in "$@"; do shift; if [ "$a" = --game-code-dylib ]; then DYLIB=1; else set -- "$@" "$a"; fi; done
while getopts r:g:o:n:G: opt; do
    case $opt in
        r) REPO=$OPTARG ;; g) GEN=$OPTARG ;; o) OUT=$OPTARG ;; n) NAME=$OPTARG ;; G) GEN_OBJ=$OPTARG ;;
        *) sed -n 2,13p "$0"; exit 2 ;;
    esac
done
[ -n "$REPO" ] && [ -n "$GEN" ] && [ -n "$OUT" ] && [ -n "$NAME" ] || { sed -n 2,13p "$0"; exit 2; }
HERE=$(dirname "$(realpath "$0")"); RT=$HERE/../runtime
mkdir -p "$OUT"
# the xbe's SHA-1: the executable checks the game it finds is the release it was made from (as XbeRecomp.cmake)
sha=$(shasum -a 1 "$REPO"/assets/[Dd]efault.xbe | head -1 | cut -d' ' -f1)
hash=$OUT/xbe_hash.cpp
want="extern const char kImageSha1[] = \"$sha\";"
[ "$(cat "$hash" 2>/dev/null)" = "$want" ] || echo "$want" > "$hash"
MAC_DEPS=${MAC_DEPS:-}
if [ -n "$MAC_DEPS" ]; then
    [ -d "$MAC_DEPS/lib" ] || { echo "MAC_DEPS=$MAC_DEPS: no lib folder (tools/mac/build_libs.sh makes it)" >&2; exit 1; }
    MAC_DEPS=$(cd "$MAC_DEPS" && pwd -P)
    case $MAC_DEPS in *[[:space:]]*) echo "MAC_DEPS=$MAC_DEPS: the compiler flags take no spaces in the path" >&2; exit 1 ;; esac
else
    BREW=$(brew --prefix)
fi
if [ -n "$DYLIB" ]; then
    # nothing made from the game goes in: like a release, the first start makes the icon and the achievement pictures from the player's
    # disc in the data folder (buffy_config --pictures); what an earlier build left here would hide that
    rm -rf "$OUT/icon.png" "$OUT/achievements"
else
    # the Dock's icon for this build (tools/make_icon.py), which has no app around it
    if [ -f "$REPO/assets/$NAME.png" ]; then cp "$REPO/assets/$NAME.png" "$OUT/icon.png"; fi
    # the achievement icons, if the game has any (a game's tools make them), which bundle.sh puts in the app
    if [ -d "$REPO/assets/achievements" ]; then mkdir -p "$OUT/achievements"; cp "$REPO"/assets/achievements/*.png "$OUT/achievements/"; fi
fi
# Dear ImGui for the in-game menu, the version the CMake build uses
IMGUI=$REPO/build/_deps/imgui-src
[ -f "$IMGUI/imgui.h" ] || { mkdir -p "$(dirname "$IMGUI")"; git -c advice.detachedHead=false clone -q --depth 1 --branch v1.91.0 https://github.com/ocornut/imgui.git "$IMGUI"; }
if [ -n "$DYLIB" ]; then
    # the executable exports what the dylib binds to (X86_HOST_API and X86_FUNC, the only default visibility names of its own code),
    # and holds none of the game's code; the token and the flags the dylib is built with go in as game_code_flags.h
    CODE=$OUT/game_code
    command -v cmake > /dev/null || { echo "cmake is needed for --game-code-dylib (brew install cmake)" >&2; exit 1; }
    mkdir -p "$CODE"
    # absolute paths: the token holds each hashed file's path relative to the repo
    cmake -DPLATFORM=macos -DSDK_DIR="$(realpath "$HERE/..")" -DSOURCE_DIR="$(realpath "$REPO")" -DGAME_DIR="$(basename "$GEN")" -DOUT_DIR="$(realpath "$CODE")" \
        -P "$HERE/../cmake/xbe_abi.cmake"
    HOST_EXTRA=" -DXBE_GAME_CODE_DLL -fvisibility=hidden -I$(realpath "$CODE")"
    LINK_EXTRA=" -Wl,-export_dynamic"
fi
if [ -n "$MAC_DEPS" ]; then
    # the libraries of tools/mac/build_libs.sh, for macOS 13.0 and later; no brew, no pkg-config. SDL3 and MoltenVK (opened by name when the
    # renderer starts) are copied next to the executable, where its search path points; glslang and SPIRV-Tools are linked in.
    for f in lib/libSDL3.0.dylib lib/libMoltenVK.dylib lib/libglslang.a lib/libSPIRV.a lib/libglslang-default-resource-limits.a lib/libSPIRV-Tools-opt.a \
             lib/libSPIRV-Tools.a include/SDL3/SDL.h include/vulkan/vulkan.h include/glslang/Public/ShaderLang.h; do
        [ -e "$MAC_DEPS/$f" ] || { echo "MAC_DEPS=$MAC_DEPS has no $f (tools/mac/build_libs.sh makes it)" >&2; exit 1; }
    done
    export MACOSX_DEPLOYMENT_TARGET=13.0
    mkdir -p "$OUT/Frameworks"
    for l in libSDL3.0.dylib libMoltenVK.dylib; do cmp -s "$MAC_DEPS/lib/$l" "$OUT/Frameworks/$l" || { cp -fL "$MAC_DEPS/lib/$l" "$OUT/Frameworks/$l"; chmod u+w "$OUT/Frameworks/$l"; }; done
    # A release holds nothing that names this machine: the sources' paths (the __FILE__ of the log macros, and the debug information) are made
    # relative to the repository, and the link leaves out the debug map (-Wl,-S: the .o files' paths, one entry each). The executable finds the
    # libraries in Frameworks beside it, or in Contents/Frameworks when it is in an app (tools/package_mac.sh), by the one build.
    export HOST_OPT=-O2 HOST_FLAGS="-mmacosx-version-min=13.0 -ffile-prefix-map=$(realpath "$REPO")=. -ffile-prefix-map=$MAC_DEPS=/deps -I$MAC_DEPS/include -I$HERE -I$MAC_DEPS/include/glslang -I$IMGUI -DXBE_HOST_VULKAN -DIMGUI_IMPL_VULKAN_NO_PROTOTYPES$HOST_EXTRA"
    export LINK_FLAGS="-mmacosx-version-min=13.0 -L$MAC_DEPS/lib -lSDL3 -Wl,-rpath,@executable_path/Frameworks -Wl,-rpath,@executable_path/../Frameworks -Wl,-S -Wl,-headerpad_max_install_names -framework AppKit -framework QuartzCore -framework CoreGraphics -framework Foundation -lglslang -lSPIRV -lglslang-default-resource-limits -lSPIRV-Tools-opt -lSPIRV-Tools$LINK_EXTRA"
else
    export HOST_OPT=-O2 HOST_FLAGS="$(pkg-config --cflags sdl3) -I$HERE -I$BREW/include -I$BREW/include/glslang -I$IMGUI -DXBE_HOST_VULKAN -DIMGUI_IMPL_VULKAN_NO_PROTOTYPES$HOST_EXTRA"
    export LINK_FLAGS="$(pkg-config --libs sdl3) -L$BREW/lib -Wl,-rpath,$BREW/lib -Wl,-headerpad_max_install_names -framework AppKit -framework QuartzCore -framework CoreGraphics -framework Foundation -lglslang -lSPIRV -lglslang-default-resource-limits$LINK_EXTRA"
fi
ARGS=(-r "$REPO" -g "$GEN" -o "$OUT" -n "$NAME" ${GEN_OBJ:+-G "$GEN_OBJ"}
    -x '/harness/(app_headless|main_test|pad_script|null_audio)\.cpp'
    -e "$HERE/main.cpp" -e "$HERE/preview.cpp" -e "$HERE/script_pad.cpp" -e "$HERE/audio_sdl.cpp" -e "$HERE/fps_overlay.mm" -e "$HERE/window_mac.mm" -e "$HERE/ui_events.cpp"
    -e "$RT/app/achievements.cpp" -e "$RT/app/overlay.cpp" -e "$RT/app/subtitles.cpp"
    -e "$IMGUI/imgui.cpp" -e "$IMGUI/imgui_draw.cpp" -e "$IMGUI/imgui_tables.cpp" -e "$IMGUI/imgui_widgets.cpp" -e "$IMGUI/backends/imgui_impl_vulkan.cpp"
    -e "$RT/core/input.cpp" -e "$RT/app/keyboard.cpp" -e "$RT/app/game_files.cpp" -e "$RT/app/settings.cpp"
    -e "$RT/gpu/vk.cpp" -e "$RT/gpu/vk_loader.cpp" -e "$RT/gpu/vk_probe.cpp" -e "$RT/gpu/spirv.cpp"
    -e "$hash")
if [ -n "$DYLIB" ]; then
    # the first start: the build of the game's code from the player's disc, and its progress window
    ARGS+=(-e "$HERE/first_start.cpp" -e "$HERE/first_start_window.mm" -e "$RT/app/game_build.cpp")
    NO_GEN=1 "$HERE/../harness/build.sh" "${ARGS[@]}"
    exec bash "$HERE/game_code_dylib.sh" -r "$REPO" -g "$GEN" -o "$OUT" -n "$NAME"
fi
exec "$HERE/../harness/build.sh" "${ARGS[@]}"
