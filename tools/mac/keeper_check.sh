#!/bin/bash
# The Mac keeper (sdk/runtime/kernel/keeper.h) without a window or the game: tools/mac/keeper_check.cpp plays the keeper and stand-ins for the
# game's instances, which hand over to one another as kernel::Relaunch does. It is built here (clang++, the same keeper.cpp the game links)
# and run in a scratch folder under the ignored build/ with HOME pointing into it, so nothing can reach ~/Library/Application Support/Buffy.
# The decision of which starts get a keeper (tools/supervisor_args_check.cpp) is run too.
#   tools/mac/keeper_check.sh                 the keeper's own checks (no build needed)
#   tools/mac/keeper_check.sh <build folder>  the built Buffy under RECOMPLAUNCHER=1, over starts that end before there is a window (--no-dialogs,
#                                             --version, --prepare with no game): which get a keeper, what it logs, that it passes the code
#                                             and the streams on, and that a start without RecompLauncher has none
# Prints PASS or FAIL for each check; exits 1 on a FAIL. Opens no window, runs no game.
set -uo pipefail
die() { echo "FAIL  $1"; exit 1; }
cd "$(dirname "$0")/../.." || exit 1
REPO=$PWD
O="$REPO/build/keeper"
rm -rf "$O"; mkdir -p "$O/home" || die "cannot make $O"
export HOME="$O/home"
unset RECOMPLAUNCHER RECOMPLAUNCHER_DATA HELLMOUTH_SUPERVISED XBE_RELAUNCHED
fail=0
chk() { if [ "$1" = 0 ]; then echo "PASS  $2"; else echo "FAIL  $2"; fail=1; fi; }

B=${1:-}
if [ -n "$B" ]; then
    [ -x "$B/Buffy" ] || die "no $B/Buffy: sdk/host/build.sh -r . -g generated/game -o $B -n Buffy --game-code-dylib"
    EXE=$(cd "$B" && pwd -P)/Buffy
    D="$O/exe"
    mkdir -p "$D"
    export HOME="$D/home"; mkdir -p "$HOME"
    echo "== the built program under RECOMPLAUNCHER=1, starts that end before a window"
    # run <name> <data> <env words...> -- <arguments>: the exit code in $code, the streams in $D/<name>.out and .err
    run() {
        local name=$1 data=$2; shift 2
        local envs=()
        while [ "$1" != -- ]; do envs+=("$1"); shift; done
        shift
        env "${envs[@]}" perl -e 'alarm 60; exec @ARGV' "$EXE" "$@" > "$D/$name.out" 2> "$D/$name.err"; code=$?
    }
    log_has() { grep -q -- "$2" "$1/logs/supervisor.log" 2> /dev/null; }

    run plain "$D/d-plain" RECOMPLAUNCHER=0 -- --no-dialogs --data "$D/d-plain" --game "$D/nothing.iso"; plain=$code
    [ "$plain" = 2 ] && [ ! -e "$D/d-plain/logs/supervisor.log" ]; chk $? "a start without RecompLauncher has no keeper: exit $plain (no game: 2), no supervisor.log"

    run kept "$D/d-kept" RECOMPLAUNCHER=1 -- --no-dialogs --data "$D/d-kept" --game "$D/nothing.iso"
    [ "$code" = 2 ]; chk $? "a start by RecompLauncher passes the game's exit code through the keeper: exit $code (wanted 2)"
    log_has "$D/d-kept" "supervising process" && log_has "$D/d-kept" "exited, code 2" && log_has "$D/d-kept" "done, exit code 2"; chk $? "its supervisor.log says what was run and how it ended"
    # the same words but for the time of each line and the data folder's name
    same() { cmp -s <(sed -E 's/^\[ *[0-9.]+\]//; s/d-(kept|plain)/d-X/g' "$1") <(sed -E 's/^\[ *[0-9.]+\]//; s/d-(kept|plain)/d-X/g' "$2"); }
    [ -s "$D/kept.err" ] && same "$D/kept.err" "$D/plain.err" && same "$D/kept.out" "$D/plain.out"; chk $? "the game's output reaches the launcher through the keeper unchanged ($(wc -c < "$D/kept.err" | tr -d ' ') bytes on stderr)"
    ls "$D/d-kept/logs" | grep -v '^supervisor' | grep -q '\.log$'; chk $? "the game wrote its own log beside it (the keeper's is the other one): $(ls "$D/d-kept/logs" | tr '\n' ' ')"
    run second "$D/d-second" RECOMPLAUNCHER=1 -- --no-dialogs --data "$D/d-second" --instance 2 --game "$D/nothing.iso"
    [ "$code" = 2 ] && grep -q "done, exit code 2" "$D/d-second/logs/supervisor-2.log" && [ ! -e "$D/d-second/logs/supervisor.log" ]; chk $? "a second copy (--instance 2) keeps supervisor-2.log"
    run rel "$D/d-rel" RECOMPLAUNCHER=1 XBE_RELAUNCHED=1 -- --no-dialogs --data "$D/d-rel" --game "$D/nothing.iso"
    [ "$code" = 2 ] && [ ! -e "$D/d-rel/logs/supervisor.log" ]; chk $? "a restart for a level (XBE_RELAUNCHED) is not run under a keeper"
    run ver "$D/d-ver" RECOMPLAUNCHER=1 -- --version --no-dialogs --data "$D/d-ver"
    [ "$code" = 0 ] && [ -s "$D/ver.out" ] && [ ! -e "$D/d-ver" ]; chk $? "--version is not run under a keeper (and makes nothing)"
    run prep "$D/d-prep" RECOMPLAUNCHER=1 -- --prepare --data "$D/d-prep" --game "$D/nothing.iso"
    [ "$code" = 2 ] && [ ! -e "$D/d-prep/logs/supervisor.log" ]; chk $? "--prepare is not run under a keeper: exit $code (no game: 2)"
    run paths "$D/d-paths" RECOMPLAUNCHER=1 -- --print-paths --no-dialogs --data "$D/d-paths"
    [ "$code" = 0 ] && [ ! -e "$D/d-paths" ]; chk $? "--print-paths is not run under a keeper (and makes nothing)"
    run marked "$D/d-marked" RECOMPLAUNCHER=1 HELLMOUTH_SUPERVISED=40,41,42 -- --no-dialogs --data "$D/d-marked" --game "$D/nothing.iso"
    [ "$code" = 2 ] && [ ! -e "$D/d-marked/logs/supervisor.log" ]; chk $? "a stale marker in the environment is not trusted with descriptors, and no keeper is made inside it"
    left=$(ps -axo pid=,command= | grep -F "$EXE" | grep -v grep || true)
    [ -z "$left" ]; chk $? "no Buffy is left running${left:+: $left}"
    [ "$fail" = 0 ] || exit 1
    rm -rf "$D"
    exit 0
fi

clang++ -std=c++20 -O1 -g -Wall -Wextra -I"$REPO/sdk/runtime" "$REPO/tools/supervisor_args_check.cpp" -o "$O/supervisor_args_check" || die "supervisor_args_check does not compile"
"$O/supervisor_args_check" > "$O/sac.out" 2>&1; chk $? "which starts get a keeper (the Mac's argv too): $(tail -1 "$O/sac.out")"

clang++ -std=c++20 -O1 -g -Wall -Wextra -I"$REPO/sdk/runtime" "$REPO/tools/mac/keeper_check.cpp" "$REPO/sdk/runtime/kernel/keeper.cpp" -o "$O/keeper_check" || die "keeper_check does not compile"
perl -e 'alarm 280; exec @ARGV' "$O/keeper_check" "$O/scratch" > "$O/kc.out" 2>&1; code=$?
cat "$O/kc.out"
[ "$code" = 0 ] && ! grep -q '^FAIL' "$O/kc.out" && grep -q 'checks passed, 0 failed' "$O/kc.out"; chk $? "the keeper's own checks ended with $code (142 is the alarm)"
left=$(ps -axo pid=,command= | grep -F "$O/keeper_check" | grep -v grep || true)
[ -z "$left" ]; chk $? "no stand-in or keeper is left running${left:+: $left}"

[ "$fail" = 0 ] || exit 1
rm -rf "$O/scratch" "$O/home"
