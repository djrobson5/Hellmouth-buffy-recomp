#!/bin/bash
# The Mac keeper (sdk/runtime/kernel/keeper.h) without a window or the game: tools/mac/keeper_check.cpp plays the keeper and stand-ins for the
# game's instances, which hand over to one another as kernel::Relaunch does. It is built here (clang++, the same keeper.cpp the game links)
# and run in a scratch folder under the ignored build/ with HOME pointing into it, so nothing can reach ~/Library/Application Support/Buffy.
# The decision of which starts get a keeper (tools/supervisor_args_check.cpp) is run too.
#   tools/mac/keeper_check.sh
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
