#!/bin/bash
# The checks to run after a change, in one go: each prints one line, its log goes in the log folder, and the first that fails stops
# the run (later checks use what earlier ones built). Run from anywhere; it works in the repository this script is in.
#
#   tools/gates.sh [--logs <folder>] [--windows] [--mac]
#
#   --logs <folder>  where the logs go (default out/gates)
#   --windows        only the Windows checks       --mac   only the Mac checks       (default: both)
#
# Needs: LLVM_MINGW (llvm-mingw for this Mac) and RECOMPLAUNCHER_GUIDE (RecompLauncher's developer guide, for the check of
# recomplauncher.json). With MAC_DEPS (tools/mac/build_libs.sh's output) the Mac checks also build and check the game-code-dylib app
# against the source-built libraries; without it that part is skipped and says so.
#
# Windows: the exe and the game code DLL, the tools, check_game_code_dll.sh, check_config_tool.sh, package_windows.sh (with the
# recomplauncher.json check and both audits), sim_first_start.sh.
# Mac: tools/mac/keeper_check.sh (the keeper that a start by RecompLauncher runs under, without the game), the static build (build/app),
# the game-code-dylib build with llvm-mingw (build/app-dylib), check_game_code_dylib.sh and
# tools/mac/sim_first_start_mac.sh (the Mac first start, without a window, from the disc), and with MAC_DEPS the same build against the
# source-built libraries (build/app-dylib-deps), check_game_code_dylib.sh, the first start again, Buffy --check-game-code (loads the game
# code without a window) and tools/mac/check_mac_deps.sh.
set -uo pipefail

LOGS= WIN=1 MAC=1
while [ $# -gt 0 ]; do
    case $1 in
        --logs) [ $# -ge 2 ] || { echo "--logs needs a folder" >&2; exit 2; }; LOGS=$2; shift 2 ;;
        --windows) MAC=0; shift ;;
        --mac) WIN=0; shift ;;
        -h | --help) sed -n 2,20p "$0" | cut -c3-; exit 0 ;;
        *) echo "unknown argument: $1 (see --help)" >&2; exit 2 ;;
    esac
done
[ "$WIN$MAC" != 00 ] || { echo "--windows and --mac together leave nothing to run; give one or neither" >&2; exit 2; }

cd "$(dirname "$0")/.." || exit 1
LOGS=${LOGS:-out/gates}
mkdir -p "$LOGS" || exit 1
LOGS=$(cd "$LOGS" && pwd -P)

die() { echo "gates: $*" >&2; exit 1; }
[ -n "${LLVM_MINGW:-}" ] && [ -d "$LLVM_MINGW" ] || die "LLVM_MINGW is not set to a folder: llvm-mingw for this Mac"
if [ "$WIN" = 1 ]; then
    [ -n "${RECOMPLAUNCHER_GUIDE:-}" ] && [ -f "$RECOMPLAUNCHER_GUIDE" ] || die "RECOMPLAUNCHER_GUIDE is not set to a file: RecompLauncher's developer guide"
fi
export LLVM_MINGW RECOMPLAUNCHER_GUIDE

# fail <name> <log> [why]: says which check failed, shows the end of its log, and stops
fail() {
    echo "FAIL $1${3:+: $3}" >&2
    [ -f "$2" ] && tail -15 "$2" | sed 's/^/    /' >&2
    echo "     the whole log: $2" >&2
    exit 1
}
# run <name> <log> <command>: the command's output goes to the log; bash -e so that a failing step inside it stops it
run() {
    local name=$1 log=$2
    shift 2
    bash -ec "$1" > "$log" 2>&1 || fail "$name" "$log" "exit status $?"
}
# counted <label> <log> <command>: a check script that prints PASS and FAIL lines and exits non-zero on a FAIL
counted() {
    local label=$1 log=$2 rc=0 pass bad
    bash -ec "$3" > "$log" 2>&1 || rc=$?
    pass=$(grep -c PASS "$log" || true)
    bad=$(grep -c FAIL "$log" || true)
    echo "$label: $pass PASS, $bad FAIL"
    [ "$rc" = 0 ] && [ "$bad" = 0 ] && [ "$pass" -gt 0 ] || fail "$label" "$log" "exit status $rc"
}

if [ "$WIN" = 1 ]; then
    run "win build" "$LOGS/win-build.log" \
        'cmake --preset win-x64-mingw && cmake --build out/build/win-x64-mingw && cmake --build out/build/win-x64-mingw --target game_code_dll'
    echo "OK win build"
    run "tools build" "$LOGS/tools-build.log" '( cd tools && cmake --preset win-x64-mingw && cmake --build ../out/build/win-x64-mingw-tools )'
    echo "OK tools build"
    counted dll "$LOGS/check-dll.log" 'bash tools/check_game_code_dll.sh'
    counted cfg "$LOGS/check-config.log" 'bash tools/check_config_tool.sh'
    rc=0
    bash tools/package_windows.sh > "$LOGS/package.log" 2>&1 || rc=$?
    rl=$(grep -c 'RECOMPLAUNCHER.JSON PASSED' "$LOGS/package.log" || true)
    au=$(grep -c 'AUDIT PASSED' "$LOGS/package.log" || true)
    echo "package: $rl RECOMPLAUNCHER.JSON PASSED, $au AUDIT PASSED"
    [ "$rc" = 0 ] && [ "$rl" -ge 1 ] && [ "$au" = 2 ] && ! grep -qi 'not checked' "$LOGS/package.log" && ! grep -q 'AUDIT FAILED' "$LOGS/package.log" ||
        fail package "$LOGS/package.log" "exit status $rc; needs RECOMPLAUNCHER.JSON PASSED, 2 AUDIT PASSED and no \"not checked\""
    # sim_first_start.sh plays the last of `ls out/Hellmouth-*-windows-x64.zip`, which sorts 0.2.10 before 0.2.9: it must be this version's
    version=$(sed -n 's/.*\.version = "buffy-port \([0-9][0-9.]*\)".*/\1/p' src/common/game_info.cpp | head -1)
    [ -n "$version" ] || die "no version in src/common/game_info.cpp"
    played=$(ls out/Hellmouth-*-windows-x64.zip | tail -1)
    [ "$played" = "out/Hellmouth-$version-windows-x64.zip" ] ||
        die "tools/sim_first_start.sh would play $played, not out/Hellmouth-$version-windows-x64.zip: move the other zips out of out/"
    counted sim "$LOGS/sim.log" 'bash tools/sim_first_start.sh'
fi

if [ "$MAC" = 1 ]; then
    counted "mac keeper" "$LOGS/keeper.log" 'bash tools/mac/keeper_check.sh'
    run "mac build" "$LOGS/mac-build.log" 'env -u MAC_DEPS sdk/host/build.sh -r . -g generated/game -o build/app -n Buffy'
    [ -x build/app/Buffy ] || fail "mac build" "$LOGS/mac-build.log" "build/app/Buffy was not made"
    echo "OK mac build"
    run "mac dylib build" "$LOGS/mac-dylib-build.log" 'env -u MAC_DEPS sdk/host/build.sh -r . -g generated/game -o build/app-dylib -n Buffy --game-code-dylib'
    echo "OK mac dylib build"
    counted "dylib" "$LOGS/check-dylib.log" 'bash tools/check_game_code_dylib.sh build/app-dylib'
    counted "mac first start" "$LOGS/sim-mac.log" 'bash tools/mac/sim_first_start_mac.sh build/app-dylib'
    if [ -n "${MAC_DEPS:-}" ]; then
        export MAC_DEPS
        run "mac deps build" "$LOGS/mac-deps-build.log" 'sdk/host/build.sh -r . -g generated/game -o build/app-dylib-deps -n Buffy --game-code-dylib'
        echo "OK mac deps build"
        counted "dylib (deps)" "$LOGS/check-dylib-deps.log" 'bash tools/check_game_code_dylib.sh build/app-dylib-deps'
        counted "mac first start (deps)" "$LOGS/sim-mac-deps.log" 'bash tools/mac/sim_first_start_mac.sh build/app-dylib-deps'
        run "check-game-code" "$LOGS/check-game-code.log" 'build/app-dylib-deps/Buffy --check-game-code'
        echo "OK $(grep -m1 '^overrides:' "$LOGS/check-game-code.log" || echo 'check-game-code')"
        rc=0
        bash tools/mac/check_mac_deps.sh build/app-dylib-deps > "$LOGS/check-mac-deps.log" 2>&1 || rc=$?
        [ "$rc" = 0 ] && grep -q '^ALL PASSED' "$LOGS/check-mac-deps.log" || fail "mac deps" "$LOGS/check-mac-deps.log" "exit status $rc"
        echo "mac deps: $(grep -c '^PASS' "$LOGS/check-mac-deps.log") PASS, ALL PASSED"
    else
        echo "skipped: the build against the source-built libraries (set MAC_DEPS to the output of tools/mac/build_libs.sh)"
    fi
fi
echo "all gates passed (logs in $LOGS)"
