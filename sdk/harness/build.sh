#!/bin/bash
# build.sh: the headless harness exe of one image, on macOS or Linux with clang
#   -r repo (holding sdk/ and src/)   -g generated folder of the image   -o out folder
#   -n exe name (default: the generated folder's name)
#   -x regex of sources to leave out (repeatable)   -e extra source file (repeatable)
#   -G folder for the generated objects (default <out>/gen), shared between builds of the same generated code
# env: CXX (clang++), JOBS (the core count), ARCH (-march=x86-64-v2 on x86-64; another target's flags for a cross build),
#      HOST_OPT (-O1) and HOST_FLAGS for the host sources, LINK_FLAGS for the link (sdk/host/build.sh sets them)
# Leaves out the window, SDL input, XAudio2 and D3D12 parts of the runtime and the sources of the repo's other images
# (src/<name> for each other folder beside the generated one); the harness supplies their stand ins.
set -e
EXCL=(); EXTRA=()
while getopts r:g:o:n:x:e:G: opt; do
    case $opt in
        r) REPO=$OPTARG ;; g) GEN=$OPTARG ;; o) OUT=$OPTARG ;; n) EXE=$OPTARG ;;
        x) EXCL+=("$OPTARG") ;; e) EXTRA+=("$(realpath "$OPTARG")") ;; G) GEN_OBJ=$OPTARG ;;
        *) sed -n 2,9p "$0"; exit 2 ;;
    esac
done
[ -n "$REPO" ] && [ -n "$GEN" ] && [ -n "$OUT" ] || { sed -n 2,9p "$0"; exit 2; }
REPO=$(realpath "$REPO"); GEN=$(realpath "$GEN"); mkdir -p "$OUT"; OUT=$(realpath "$OUT")
EXE=${EXE:-$(basename "$GEN")}; GEN_OBJ=${GEN_OBJ:-$OUT/gen}; mkdir -p "$GEN_OBJ" "$OUT/host"
H=$(dirname "$(realpath "$0")"); RT=$REPO/sdk/runtime
CXX=${CXX:-clang++}; JOBS=${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}
if [ -z "${ARCH+x}" ] && [ "$(uname -m)" = x86_64 ]; then ARCH=-march=x86-64-v2; fi
BASE="-std=c++20 $ARCH -fno-strict-aliasing"
# toml++ (header only) for the settings file code: TOML_INCLUDE, or the copy a CMake build of the repo fetched
TOML_INCLUDE=${TOML_INCLUDE:-$(ls -d "$REPO"/out/build/*/_deps/tomlplusplus-src/include "$REPO"/build/*/_deps/tomlplusplus-src/include 2>/dev/null | head -1)}
HOST="$BASE ${HOST_OPT:--O1} -g $HOST_FLAGS -I$REPO/src -I$RT -I$GEN -I$H -I$H/compat -I$TOML_INCLUDE -DXBE_RAW_VERTICES=0 -DXBE_RENDER_THREAD=0"

# generated code, an object rebuilt only when its partition is newer
export CXX BASE RT GEN GEN_OBJ HOST
ls "$GEN"/x86_recomp.*.cpp "$GEN"/x86_func_mapping.cpp | xargs -P "$JOBS" -n1 sh -c '
    o="$GEN_OBJ/$(basename "$0" .cpp).o"; [ "$o" -nt "$0" ] && exit 0
    $CXX $BASE -O1 -g0 -w -I"$RT" -I"$GEN" -c "$0" -o "$o.tmp" && mv "$o.tmp" "$o"'

others=$(ls "$GEN/.." | grep -vx "$(basename "$GEN")" | paste -sd'|' - || true)
SRC=$(ls "$RT"/*/*.cpp | grep -Ev "/runtime/app/|/core/input.cpp|/gpu/d3d12.cpp|/gpu/vk.cpp|/gpu/vk_loader.cpp|/gpu/vk_probe.cpp|/gpu/backend.cpp|/gpu/spirv.cpp|/gpu/upscale.cpp|/audio/xaudio2.cpp")
SRC="$SRC $RT/app/settings_list.cpp"
SRC="$SRC $(ls "$REPO"/src/*/*.cpp | grep -Ev "/src/(${others:-^$})/" || true)"
SRC="$SRC $(ls "$H"/*.cpp) ${EXTRA[*]}"
for x in "${EXCL[@]}"; do SRC=$(echo $SRC | tr ' ' '\n' | grep -Ev -- "$x"); done

# host objects, keyed by the flags; rebuilt when the source or any header of the repo, harness or image is newer
stamp=$(echo "$HOST" | cksum | cut -d' ' -f1)
newest=$(find "$RT" "$REPO/src" "$H" "$REPO/sdk/host" "$GEN/image_symbols.h" -name '*.h' -print0 | xargs -0 ls -t | head -1)
objs=(); stale=()
for f in $SRC; do
    o=$OUT/host/$(echo "${f#/}" | tr / _ | sed -E "s/\.(cpp|mm)$/.$stamp.o/"); objs+=("$o")
    [ "$o" -nt "$f" ] && [ "$o" -nt "$newest" ] && continue
    rm -f "$o"
    stale+=("$f $o")
done
# macOS's bash 3.2 has no wait -n, so xargs runs them in parallel; a failed compile fails the build here
if [ ${#stale[@]} -gt 0 ]; then
    printf '%s\n' "${stale[@]}" | xargs -P "$JOBS" -L1 sh -c 'echo "compile $0"; $CXX $HOST -c "$0" -o "$1"'
fi
$CXX -o "$OUT/$EXE" "${objs[@]}" "$GEN_OBJ"/*.o -lpthread $LINK_FLAGS
echo "built $OUT/$EXE"
