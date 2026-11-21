#!/bin/bash
# The Mac release's zip played as a player would get it, on this Mac with no window (the counterpart of tools/sim_first_start.sh for the
# Windows release; tools/mac/sim_first_start_mac.sh plays the first start's many cases over the build folder). The zip tools/package_mac.sh
# made is extracted into a folder with a space and an accent in its name, and everything runs from the app inside it, with HOME pointing into
# the scratch folder so that nothing can reach the real ~/Library/Application Support/Buffy (which holds the player's saves).
#   tools/mac/sim_package_mac.sh [zip]      default out/Hellmouth-<version>-macos-arm64.zip (the version of src/common/game_info.cpp)
#   env: DISC=<the USA disc image> (default: the .iso in "Buffy the Vampire Slayer (USA)/" in the repository)   KEEP=1 keeps the scratch folder
# What is played, all with no window (--prepare, --check-game-code, --print-paths; the app is never opened, and Launch Services is never asked):
#   the zip extracts to Hellmouth-<version>-macos-arm64/ holding Buffy.app, README.txt and recomplauncher.json, and the app verifies
#   in the app: --print-paths puts the compiler in Contents/Resources/compiler and the data folder where it should be; the compiler's programs run
#   a first --prepare from the disc (as RecompLauncher's setup step is run): progress lines, exit 0, the dylib there; --check-game-code loads it
#   into the app's program (the executable exports what the dylib binds to, which the release's strip must not have removed); a second --prepare
#   finds everything current; the app is byte for byte what was extracted (it verifies still, no file is new in it) and the home folder is empty
#   the same zip extracted with a quarantine attribute on every file, as a browser leaves it (com.apple.quarantine, "0083;...;Safari;"), run
#   directly as a launcher or Terminal would run it: --prepare with most of the objects of the first run in its cache (so that a minute of
#   compiling is not played again) and some left out, so that the quarantined clang does compile and the quarantined ld64.lld does link. What a
#   quarantined app does when Finder opens it (Gatekeeper's dialog, App Translocation) needs a window and is not played here.
# Prints PASS or FAIL for each check, and the time of the first start; exits 1 on a FAIL. Opens no window, runs no game.
set -uo pipefail
die() { echo "FAIL  $1"; exit 1; }
cd "$(dirname "$0")/../.." || exit 1
REPO=$PWD
VERSION=$(sed -n 's/.*\.version = "buffy-port \(.*\)",.*/\1/p' src/common/game_info.cpp)
NAME=Hellmouth-$VERSION-macos-arm64
ZIP=${1:-$REPO/out/$NAME.zip}
[ -f "$ZIP" ] || die "no $ZIP: tools/package_mac.sh makes it"
ZIP=$(cd "$(dirname "$ZIP")" && pwd -P)/$(basename "$ZIP")
DISC=${DISC:-$(ls "$REPO"/Buffy*USA*/*.iso 2> /dev/null | head -1 || true)}
[ -f "$DISC" ] || die "no disc image: DISC=<the USA release's image>"
case $DISC in *Europe* | *Chaos*) die "DISC is not the USA release: $DISC" ;; esac
D="$REPO/build/sim-package-mac dir é"
rm -rf "$D"; mkdir -p "$D/home"; D=$(cd "$D" && pwd -P) || die "cannot make the scratch folder $D"
cleanup() { [ -n "${KEEP:-}" ] || rm -rf -- "$D"; }
trap cleanup EXIT
export HOME="$D/home"
unset RECOMPLAUNCHER_DATA RECOMPLAUNCHER
fail=0
chk() { if [ "$1" = 0 ]; then echo "PASS  $2"; else echo "FAIL  $2"; fail=1; fi; }
now() { perl -MTime::HiRes=time -e 'printf "%.3f\n", time'; }
secs() { perl -e 'printf "%.1f", $ARGV[1] - $ARGV[0]' "$1" "$2"; }
nothing_made() { [ -z "$(find "$D/home" -mindepth 1 2> /dev/null | head -1)" ]; }
# alarm <seconds> <command...>: the command's exit status, or 142 when it ran too long (a window or a dialog that nobody answers ends here)
alarm() { local s=$1; shift; perl -e 'alarm shift; exec @ARGV' "$s" "$@"; }

echo "== the zip, extracted into a folder with a space and an accent"
X="$D/extracted é"
mkdir -p "$X"
ditto -x -k --noqtn "$ZIP" "$X"; chk $? "ditto -x -k $(basename "$ZIP")"
R="$X/$NAME"
APP="$R/Buffy.app"
[ -d "$APP" ] || die "the zip does not hold $NAME/Buffy.app"
[ "$(ls "$X")" = "$NAME" ]; chk $? "the zip holds one folder, $NAME"
[ "$(ls "$R" | tr '\n' ' ')" = "Buffy.app README.txt recomplauncher.json " ]; chk $? "and it holds Buffy.app, README.txt and recomplauncher.json, nothing else ($(ls "$R" | tr '\n' ' '))"
[ -z "$(find "$R" -type l | head -1)" ]; chk $? "no link in it"
codesign --verify --deep --strict "$APP" 2> "$D/cs.err"; chk $? "codesign --verify --deep --strict on the extracted app"
EXE=$APP/Contents/MacOS/Buffy
CC=$APP/Contents/Resources/compiler
[ -x "$EXE" ] && [ -x "$CC/clang" ] && [ -x "$CC/ld64.lld" ] && [ -x "$CC/xbe_recomp" ] && [ -x "$CC/buffy_config" ]; chk $? "the executable and the compiler's four programs are executable (permissions kept by the zip)"
( cd "$R" && find . -type f -o -type l | sort | xargs shasum -a 256 ) > "$D/app.before"
touch "$D/marker"
sleep 1

echo "== where everything goes, from the app (--print-paths; nothing is made)"
A="$HOME/Library/Application Support/Buffy"
out=$("$EXE" --print-paths --no-dialogs < /dev/null 2> /dev/null)
for line in "app folder: $APP/Contents/Resources" "compiler: $CC" "data: $A" "settings: $A/settings.toml" "game code: $A/build/Buffy.dylib"; do
    printf '%s\n' "$out" | grep -qxF -- "$line"; chk $? "--print-paths: $line"
done
Y="$D/data é"
out=$("$EXE" --print-paths --no-dialogs --data "$Y" < /dev/null 2> /dev/null)
for line in "data: $Y" "compiler: $CC" "game code: $Y/build/Buffy.dylib" "achievements: $Y/achievements.txt"; do
    printf '%s\n' "$out" | grep -qxF -- "$line"; chk $? "--print-paths --data: $line"
done
nothing_made && [ ! -e "$Y" ]; chk $? "--print-paths made nothing (the home folder is empty, the data folder does not exist)"
"$EXE" --version --no-dialogs > "$D/v.out"; chk $? "--version: $(cat "$D/v.out")"
[ "$(cat "$D/v.out")" = "buffy-port $VERSION" ]; chk $? "it is the release's version"
"$CC/clang" --version > "$D/clang.out" 2>&1; chk $? "compiler/clang runs from there: $(head -1 "$D/clang.out" | cut -c1-40)"
"$CC/ld64.lld" --version > "$D/lld.out" 2>&1; chk $? "compiler/ld64.lld runs: $(head -1 "$D/lld.out")"
"$CC/xbe_recomp" > "$D/x.out" 2>&1; [ -s "$D/x.out" ]; chk $? "compiler/xbe_recomp runs (says how it is used)"
"$CC/buffy_config" > "$D/c.out" 2>&1; [ -s "$D/c.out" ]; chk $? "compiler/buffy_config runs"

echo "== the first start, from the disc, as RecompLauncher runs its setup step"
t0=$(now)
alarm 900 "$EXE" --prepare "--game=$DISC" --data "$Y" > "$D/p1.out" 2> "$D/p1.err"; code=$?
t1=$(now)
chk $code "--prepare --game=<disc> --data <folder>: exit $code (142 is the alarm)"
echo "first start: $(secs "$t0" "$t1") s ($(wc -l < "$D/p1.out" | tr -d ' ') progress lines; $(grep -c . "$D/p1.err") lines on stderr)"
grep -qvE '^progress [01]\.[0-9]{3} .' "$D/p1.out"; [ $? != 0 ] && [ -s "$D/p1.out" ]; chk $? "stdout holds nothing but \"progress <0-1> <text>\" lines"
head -1 "$D/p1.out" | grep -q '^progress 0\.000 ' && tail -1 "$D/p1.out" | grep -q '^progress 1\.000 Done$'; chk $? "it starts at 0 and ends at 1 (\"$(tail -1 "$D/p1.out")\")"
grep -q '^error: ' "$D/p1.err"; [ $? != 0 ]; chk $? "no error on stderr"
[ -f "$Y/build/Buffy.dylib" ] && [ -f "$Y/build/Buffy.dylib.stamp" ]; chk $? "build/Buffy.dylib and its stamp are in the data folder"
alarm 120 "$EXE" --check-game-code --data "$Y" > "$D/cg.out" 2> "$D/cg.err"; chk $? "--check-game-code loads it into the app's program: $(grep '^externs' "$D/cg.out"), $(grep '^load' "$D/cg.out")"
n=$(ls "$Y/achievements"/*.png 2> /dev/null | wc -l | tr -d ' '); [ "$n" = 28 ]; chk $? "28 achievement pictures in the data folder ($n)"
[ -s "$Y/Buffy.png" ] && [ -s "$Y/Buffy.ico" ]; chk $? "Buffy.png and Buffy.ico made from the disc in the data folder, not in the app"
alarm 120 "$EXE" --prepare "--game=$DISC" --data "$Y" > "$D/p2.out" 2> /dev/null; code=$?
[ "$code" = 0 ] && [ "$(cat "$D/p2.out")" = "progress 1.000 The game's code is up to date" ]; chk $? "--prepare again: exit $code, \"$(cat "$D/p2.out")\""
# the app is what the zip held: nothing was written into it, and it still verifies
codesign --verify --deep --strict "$APP" 2> /dev/null; chk $? "the app still verifies after the first start"
( cd "$R" && find . -type f -o -type l | sort | xargs shasum -a 256 ) > "$D/app.after"
cmp -s "$D/app.before" "$D/app.after"; chk $? "the app's files are the same bytes as before: the first start wrote nothing into it"
[ -z "$(find "$R" -newer "$D/marker" 2> /dev/null | head -1)" ]; chk $? "and no file or folder in the release folder is newer than the start of the run"
nothing_made; chk $? "the home folder is empty (the data folder was named)"
[ ! -e "$APP/Contents/Resources/compiler/build" ] && [ ! -e "$APP/Contents/Resources/compiler/cache" ]; chk $? "no build folder was made beside the compiler"
du -sh "$Y" | awk '{ print "data folder after the first start: " $1 }'

echo "== the same zip with the quarantine attribute a browser leaves, run directly"
Z="$D/quarantined é"
mkdir -p "$Z"
ditto -x -k --noqtn "$ZIP" "$Z"
QR="$Z/$NAME"
xattr -rw com.apple.quarantine "0083;$(printf '%x' "$(date +%s)");Safari;" "$QR"
q=$(xattr -p com.apple.quarantine "$QR/Buffy.app/Contents/MacOS/Buffy" 2> /dev/null)
[ -n "$q" ] && [ -n "$(xattr -p com.apple.quarantine "$QR/Buffy.app/Contents/Resources/compiler/clang" 2> /dev/null)" ]; chk $? "every file has the attribute (\"$q\")"
QE=$QR/Buffy.app/Contents/MacOS/Buffy
QD="$D/quarantined data é"
mkdir -p "$QD/cache"
# the objects of the first run, except every fifth: those compile again, with the quarantined clang, and the link is the quarantined ld64.lld's
if [ -d "$Y/cache/game-code" ]; then
    cp -Rc "$Y/cache/game-code" "$QD/cache/game-code"
    kept=0; dropped=0
    while IFS= read -r f; do
        if [ $(( (dropped + kept) % 5 )) = 0 ]; then rm -f "$f"; dropped=$((dropped + 1)); else kept=$((kept + 1)); fi
    done < <(find "$QD/cache/game-code" -name '*.o' | sort)
    echo "objects of the first run carried over: $kept kept, $dropped left out"
fi
t0=$(now)
alarm 300 "$QE" --prepare "--game=$DISC" --data "$QD" > "$D/q1.out" 2> "$D/q1.err"; code=$?
t1=$(now)
chk $code "quarantined: --prepare, exit $code in $(secs "$t0" "$t1") s (142: stopped after 300 s; 137: killed: by Gatekeeper, say)"
[ "$code" = 0 ] || echo "      stderr: $(head -c 400 "$D/q1.err" | tr '\n' ' ')"
tail -1 "$D/q1.out" | grep -q '^progress 1\.000 '; chk $? "it ended at 1 (\"$(tail -1 "$D/q1.out")\")"
grep -q '^time: compile .* files compiled' "$QD/logs/compile.log" 2> /dev/null && echo "      $(grep '^time: compile' "$QD/logs/compile.log")"
cmp -s "$Y/build/Buffy.dylib" "$QD/build/Buffy.dylib"; chk $? "the dylib it built is the same bytes as the first run's (the same objects and compiler)"
alarm 120 "$QE" --check-game-code --data "$QD" > "$D/qc.out" 2> "$D/qc.err"; chk $? "quarantined: --check-game-code loads it: $(grep '^load' "$D/qc.out")"
q2=$(xattr -p com.apple.quarantine "$QR/Buffy.app/Contents/Resources/compiler/clang" 2> /dev/null)
[ "$q2" = "$q" ]; chk $? "the attribute is still on the files after the run (\"$q2\"): nothing cleared or approved it"
nothing_made; chk $? "the home folder is still empty"

echo
if [ "$fail" = 0 ]; then echo "the Mac release's zip: all checks passed"; else echo "some check did not pass"; fi
exit $fail
