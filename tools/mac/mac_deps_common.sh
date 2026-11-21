# mac_deps_common.sh: what build_toolchain.sh and build_libs.sh share (sourced, not run).
#   -o <out>   where the finished pieces go (default ~/Projects/toolchains/mac-deps, outside the repository)
#   -s <work>  where sources and build folders go (default ~/Projects/toolchains/mac-deps-src)
# Everything is built from source at the versions pinned in the two scripts, for AArch64 and macOS 13.0 and later, with nothing from
# Homebrew: its prefixes are hidden from CMake, and a build folder that names one fails the script. cmake, ninja, git and python3 (build
# tools, not shipped) may come from anywhere.

DEPLOYMENT_TARGET=13.0
OUT=${MAC_DEPS:-$HOME/Projects/toolchains/mac-deps}
WORK=${MAC_DEPS_SRC:-$HOME/Projects/toolchains/mac-deps-src}
JOBS=${JOBS:-$(sysctl -n hw.ncpu)}

parse_args() {
    local opt OPTIND=1
    while getopts o:s: opt; do
        case $opt in o) OUT=$OPTARG ;; s) WORK=$OPTARG ;; *) echo "usage: $0 [-o out folder] [-s source and build folder]" >&2; exit 2 ;; esac
    done
    mkdir -p "$OUT" "$WORK"
    OUT=$(cd "$OUT" && pwd -P); WORK=$(cd "$WORK" && pwd -P)
    # nothing below may find Homebrew's libraries, headers or tools by accident
    export MACOSX_DEPLOYMENT_TARGET=$DEPLOYMENT_TARGET
    mkdir -p "$OUT/licenses" "$OUT/lib" "$OUT/include" "$OUT/bin"
    cmake_common_init
}

# fetch <name> <repo url> <tag> <commit> : <work>/<name> at the pinned commit (a shallow clone of the tag, then the commit is checked: a
# moved tag fails here and not in the build)
fetch() {
    local name=$1 url=$2 tag=$3 commit=$4 dir=$WORK/$1
    if [ ! -d "$dir/.git" ]; then
        rm -rf "$dir.tmp"
        git -c advice.detachedHead=false clone -q --depth 1 --branch "$tag" "$url" "$dir.tmp"
        mv "$dir.tmp" "$dir"
    fi
    local have; have=$(git -C "$dir" rev-parse 'HEAD^{commit}')
    [ "$have" = "$commit" ] || { echo "$name: $dir is at $have, not the pinned $commit ($tag)" >&2; exit 1; }
}

# stamp_current <name> <pin text> : true if <name> was built from exactly this pin and its files are still there
stamp_current() { [ "$(cat "$OUT/licenses/$1/SOURCE.txt" 2>/dev/null | sed -n 's/^pin: //p')" = "$2" ]; }

# record <name> <version> <url> <commit> <licence file>... : SOURCE.txt (what it was built from) and the licences, in <out>/licenses/<name>/
# Licence files are given by path under the source folder; a file that is not there fails the script (a licence is part of what ships).
record() {
    local name=$1 version=$2 url=$3 commit=$4 pin=$5; shift 5
    local d=$OUT/licenses/$name f
    rm -rf "$d"; mkdir -p "$d"
    for f in "$@"; do
        [ -f "$f" ] || { echo "$name: licence file $f is missing" >&2; exit 1; }
        # a name that can repeat across folders (LICENSE.txt in each external) is kept with its folder
        cp "$f" "$d/$(echo "${f#"$WORK"/}" | tr / _)"
    done
    {
        echo "name: $name"; echo "version: $version"; echo "source: $url"; echo "commit: $commit"
        echo "deployment target: $DEPLOYMENT_TARGET (arm64)"; echo "built: $(date -u +%Y-%m-%d)"; echo "pin: $pin"
    } > "$d/SOURCE.txt"
    refresh_versions
}

# VERSIONS.txt: one line per component, from the SOURCE.txt files
refresh_versions() {
    local f
    { for f in "$OUT"/licenses/*/SOURCE.txt; do
        [ -f "$f" ] || continue
        echo "$(sed -n 's/^name: //p' "$f") $(sed -n 's/^version: //p' "$f") $(sed -n 's/^commit: //p' "$f") $(sed -n 's/^source: //p' "$f")"
    done; } > "$OUT/VERSIONS.txt"
}

# no_homebrew <build folder> : a CMake build folder must not have found anything under Homebrew's prefixes (the tools that ran the build,
# cmake, ninja and python, may be there)
no_homebrew() {
    local hits
    hits=$(grep -E '/opt/homebrew|/usr/local' "$1/CMakeCache.txt" |
        grep -Ev '^//|^#|^(FIND_PACKAGE_MESSAGE_DETAILS_(Python[0-9]*|PkgConfig)|PKG_CONFIG_EXECUTABLE|_?Python[0-9]*_[A-Za-z_]*|CMAKE_COMMAND|CMAKE_MAKE_PROGRAM|CMAKE_CPACK_COMMAND|CMAKE_CTEST_COMMAND|CMAKE_EDIT_COMMAND|CMAKE_ROOT|CMAKE_IGNORE_PREFIX_PATH|CMAKE_GENERATOR_INSTANCE|PYTHON_EXECUTABLE|GIT_EXECUTABLE|CMAKE_GENERATOR)[:=]' || true)
    [ -z "$hits" ] || { echo "$1/CMakeCache.txt names Homebrew:" >&2; echo "$hits" >&2; exit 1; }
}

# CMAKE_COMMON, the CMake flags every build takes: arm64, the target, Homebrew's prefixes hidden, our own output the only prefix searched
CMAKE_COMMON=()
cmake_common_init() {
    CMAKE_COMMON=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 "-DCMAKE_OSX_DEPLOYMENT_TARGET=$DEPLOYMENT_TARGET"
        "-DCMAKE_INSTALL_PREFIX=$OUT" "-DCMAKE_PREFIX_PATH=$OUT" "-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local"
        "-DCMAKE_MAKE_PROGRAM=$(command -v ninja)" -DCMAKE_FIND_FRAMEWORK=LAST -DCMAKE_FIND_APPBUNDLE=LAST
        -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DCMAKE_INSTALL_LIBDIR=lib)
}

now() { date +%s; }
elapsed() { local s=$1 e; e=$(( $(now) - s )); printf '%d:%02d' $((e / 60)) $((e % 60)); }
