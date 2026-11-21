#!/bin/bash
# build.sh: the game as a macOS program with a window, through SDL and Vulkan on Metal:
#   brew install sdl3 molten-vk vulkan-headers glslang
#   -r repo (holding sdk/, src/ and assets/ with the xbe)   -g generated folder of the image   -o out folder
#   -n exe name (the game restarts itself by this name)   -G folder for the generated objects, shared with other builds
# The harness's build is used, without its stand ins for the window, controllers and sound; its reference renderer stays
# as the picture when Vulkan can't start.
#   --game-code-dylib   (anywhere on the line) the executable without the game's code: it loads <out>/build/<name>.dylib (or --game-code)
#                       as the Windows build loads Buffy.dll. Builds, in <out>: the executable; game_code/ (the ABI token and flags,
#                       sdk/cmake/xbe_abi.cmake); compiler/ (what the first start compiles with: clang and ld64.lld from
#                       $LLVM_MINGW, the headers, overrides.txt, the executable's overrides); and, for development, the dylib itself
#                       from the generated sources (build/<name>.dylib, tools/first_start_check.cpp --sources). Use another <out> than
#                       the default build's: the objects differ. tools/check_game_code_dylib.sh checks the result.
set -e
DYLIB=; HOST_EXTRA=; LINK_EXTRA=
for a in "$@"; do shift; if [ "$a" = --game-code-dylib ]; then DYLIB=1; else set -- "$@" "$a"; fi; done
while getopts r:g:o:n:G: opt; do
    case $opt in
        r) REPO=$OPTARG ;; g) GEN=$OPTARG ;; o) OUT=$OPTARG ;; n) NAME=$OPTARG ;; G) GEN_OBJ=$OPTARG ;;
        *) sed -n 2,7p "$0"; exit 2 ;;
    esac
done
[ -n "$REPO" ] && [ -n "$GEN" ] && [ -n "$OUT" ] && [ -n "$NAME" ] || { sed -n 2,7p "$0"; exit 2; }
HERE=$(dirname "$(realpath "$0")"); RT=$HERE/../runtime
mkdir -p "$OUT"
# the xbe's SHA-1: the executable checks the game it finds is the release it was made from (as XbeRecomp.cmake)
sha=$(shasum -a 1 "$REPO"/assets/[Dd]efault.xbe | head -1 | cut -d' ' -f1)
hash=$OUT/xbe_hash.cpp
want="extern const char kImageSha1[] = \"$sha\";"
[ "$(cat "$hash" 2>/dev/null)" = "$want" ] || echo "$want" > "$hash"
BREW=$(brew --prefix)
# the Dock's icon for this build (tools/make_icon.py), which has no app around it
if [ -f "$REPO/assets/$NAME.png" ]; then cp "$REPO/assets/$NAME.png" "$OUT/icon.png"; fi
# the achievement icons, if the game has any (a game's tools make them), which bundle.sh puts in the app
if [ -d "$REPO/assets/achievements" ]; then mkdir -p "$OUT/achievements"; cp "$REPO"/assets/achievements/*.png "$OUT/achievements/"; fi
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
export HOST_OPT=-O2 HOST_FLAGS="$(pkg-config --cflags sdl3) -I$HERE -I$BREW/include -I$BREW/include/glslang -I$IMGUI -DXBE_HOST_VULKAN -DIMGUI_IMPL_VULKAN_NO_PROTOTYPES$HOST_EXTRA"
export LINK_FLAGS="$(pkg-config --libs sdl3) -L$BREW/lib -Wl,-rpath,$BREW/lib -Wl,-headerpad_max_install_names -framework AppKit -framework QuartzCore -framework CoreGraphics -framework Foundation -lglslang -lSPIRV -lglslang-default-resource-limits$LINK_EXTRA"
ARGS=(-r "$REPO" -g "$GEN" -o "$OUT" -n "$NAME" ${GEN_OBJ:+-G "$GEN_OBJ"}
    -x '/harness/(app_headless|main_test|pad_script|null_audio)\.cpp'
    -e "$HERE/main.cpp" -e "$HERE/preview.cpp" -e "$HERE/script_pad.cpp" -e "$HERE/audio_sdl.cpp" -e "$HERE/fps_overlay.mm" -e "$HERE/window_mac.mm" -e "$HERE/ui_events.cpp"
    -e "$RT/app/achievements.cpp" -e "$RT/app/overlay.cpp" -e "$RT/app/subtitles.cpp"
    -e "$IMGUI/imgui.cpp" -e "$IMGUI/imgui_draw.cpp" -e "$IMGUI/imgui_tables.cpp" -e "$IMGUI/imgui_widgets.cpp" -e "$IMGUI/backends/imgui_impl_vulkan.cpp"
    -e "$RT/core/input.cpp" -e "$RT/app/keyboard.cpp" -e "$RT/app/game_files.cpp" -e "$RT/app/settings.cpp"
    -e "$RT/gpu/vk.cpp" -e "$RT/gpu/vk_loader.cpp" -e "$RT/gpu/vk_probe.cpp" -e "$RT/gpu/spirv.cpp"
    -e "$hash")
if [ -n "$DYLIB" ]; then
    NO_GEN=1 "$HERE/../harness/build.sh" "${ARGS[@]}"
    exec bash "$HERE/game_code_dylib.sh" -r "$REPO" -g "$GEN" -o "$OUT" -n "$NAME"
fi
exec "$HERE/../harness/build.sh" "${ARGS[@]}"
