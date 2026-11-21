#!/bin/bash
# The Mac first start (sdk/host/first_start.cpp, the Mac counterpart of tools/sim_first_start.sh), played on this Mac with no window. The
# program is the game-code-dylib build (sdk/host/build.sh --game-code-dylib); only what a release would hold of it is used, copied to a
# folder with a space and an accent in its name: the executable, Frameworks/ and compiler/ (clang, ld64.lld, xbe_recomp, buffy_config,
# sdk_names.txt, overrides.txt, the headers, xdk/4432). Nothing made from the game comes along (not build/, achievements/, icon.png, cache/),
# so every file the first start needs of the game is made here from the disc. Everything runs with HOME pointing into the scratch folder,
# so that nothing can reach the real ~/Library/Application Support/Buffy (which holds the player's saves).
#   tools/mac/sim_first_start_mac.sh [build folder]      default build/app-dylib-deps, else build/app-dylib
#   env: DISC=<the USA disc image> (default: the .iso in "Buffy the Vampire Slayer (USA)/" in the repository)   KEEP=1 keeps the scratch folder
# What is played, all with no window (--prepare, --check-game-code, --print-paths, and starts under --no-dialogs that end before there is one):
#   the data folder: --data, --data=, RECOMPLAUNCHER_DATA (and --data over it) give the Windows layout under that folder; with neither the
#   layout is what it always was (beside the executable; in an app ~/Library/Application Support/Buffy, played with --print-paths over a copy
#   of the executable inside a fake app); nothing is made by --print-paths
#   no game, a missing one, a file that is no disc, a disc cut short, another release of the game (a copy of Default.xbe with a byte of its
#   code changed, in a folder) end --prepare with 2, 2, 2, 2, 3 and the reason on stderr and in compile.log
#   a first --prepare from the disc: progress lines, exit 0, the dylib there and current (--check-game-code loads it), the 28 achievement
#   pictures (the same pixels as assets/achievements, when that is there), Buffy.png and Buffy.ico; the second --prepare: nothing runs
#   a damaged dylib and one built for another version are made again (the objects are reused); --prepare stopped by a signal ends with 9 and
#   the next one carries on; --clean-work; RECOMPLAUNCHER_DATA with the same data; starts under --no-dialogs: no dylib 7, a stale one 8, no
#   game 2, the disc kept in <data>/disc_image.txt found again
#   and that the progress window is never opened by any of it (its line is not in any log)
# Prints PASS or FAIL for each check, and the time of the first start; exits 1 on a FAIL. Opens no window, runs no game.
set -uo pipefail
die() { echo "FAIL  $1"; exit 1; }
cd "$(dirname "$0")/../.." || exit 1
REPO=$PWD
B=${1:-}
if [ -z "$B" ]; then if [ -x build/app-dylib-deps/Buffy ]; then B=build/app-dylib-deps; else B=build/app-dylib; fi; fi
[ -x "$B/Buffy" ] && [ -d "$B/compiler" ] || die "no $B/Buffy and compiler/: sdk/host/build.sh -r . -g generated/game -o $B -n Buffy --game-code-dylib"
B=$(cd "$B" && pwd -P)
DISC=${DISC:-$(ls "$REPO"/Buffy*USA*/*.iso 2> /dev/null | head -1 || true)}
[ -f "$DISC" ] || die "no disc image: DISC=<the USA release's image>"
case $DISC in *Europe* | *Chaos*) die "DISC is not the USA release: $DISC" ;; esac
[ -f assets/Default.xbe ] || die "no assets/Default.xbe (tools/regen.sh): the wrong-release check changes a copy of it"
D="$REPO/build/sim-mac dir é"
rm -rf "$D"; mkdir -p "$D/home"; D=$(cd "$D" && pwd -P)
cleanup() { [ -n "${KEEP:-}" ] || rm -rf -- "$D"; }
trap cleanup EXIT
export HOME="$D/home"
unset RECOMPLAUNCHER_DATA
fail=0
chk() { if [ "$1" = 0 ]; then echo "PASS  $2"; else echo "FAIL  $2"; fail=1; fi; }
now() { perl -MTime::HiRes=time -e 'printf "%.3f\n", time'; }
secs() { perl -e 'printf "%.1f", $ARGV[1] - $ARGV[0]' "$1" "$2"; }

echo "== the release's files, copied into a folder with a space and an accent"
P="$D/Hellmouth é"
mkdir -p "$P"
cp -c "$B/Buffy" "$P/Buffy"
[ -d "$B/Frameworks" ] && cp -Rc "$B/Frameworks" "$P/Frameworks"
cp -Rc "$B/compiler" "$P/compiler"
EXE=$P/Buffy
ls "$P" "$P/compiler"
for f in Buffy compiler/clang compiler/ld64.lld compiler/xbe_recomp compiler/buffy_config compiler/sdk_names.txt compiler/overrides.txt \
         compiler/include/cpu/x86_ops.h compiler/include/cpu/x86_context.h compiler/include/freestanding/cstdint compiler/xdk/4432/layout.toml; do
    [ -f "$P/$f" ] || { echo "FAIL  the release folder lacks $f"; fail=1; }
done
left=$(cd "$P" && ls | grep -vxE 'Buffy|Frameworks|compiler' || true)
[ -z "$left" ]; chk $? "the folder holds the executable, Frameworks and compiler, nothing else (made from the game: none) (extra: ${left:-none})"
"$P/compiler/xbe_recomp" > "$D/x.out" 2>&1; [ -s "$D/x.out" ]; chk $? "compiler/xbe_recomp runs from there (says how it is used)"
"$P/compiler/buffy_config" > "$D/c.out" 2>&1; [ -s "$D/c.out" ]; chk $? "compiler/buffy_config runs from there"
"$P/compiler/clang" --version > "$D/clang.out" 2>&1; chk $? "compiler/clang runs: $(head -1 "$D/clang.out")"
"$EXE" --version > "$D/v.out"; chk $? "--version: $(cat "$D/v.out")"

# expect <label> <command...>: stdin has the expected "name: value" lines; each must be one of the command's lines
expect() {
    local label=$1; shift
    local out; out=$("$@" 2> /dev/null < /dev/null) || { echo "FAIL  $label: the command failed"; fail=1; cat > /dev/null; return; }
    local missing="" line
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        printf '%s\n' "$out" | grep -qxF -- "$line" || missing="$missing
      $line"
    done
    [ -z "$missing" ]; chk $? "$label${missing:+ (not printed:$missing)}"
}
nothing_made() { [ -z "$(find "$D/home" -mindepth 1 2> /dev/null | head -1)" ]; }

echo "== where everything goes (--print-paths; nothing is made)"
X="$D/data folder é"
expect "no data folder named, a bare executable: everything beside it, as it always was" "$EXE" --print-paths <<EOF
executable folder: $P
app folder: $P
data: $P
settings: $P/settings.toml
hard disk: $P/hdd
logs: $P/logs
achievements: $P/achievements.txt
textures: $P/textures
screenshots: $P/Screenshots
shader cache: $P/shadercache
disc image: $P/disc_image.txt
compiler: $P/compiler
game code: $P/build/Buffy.dylib
pictures: $P/achievements
icon: $P/Buffy.png
EOF
for how in "--data" "--data=" "env"; do
    case $how in
        --data) set -- "$EXE" --print-paths --data "$X" ;;
        --data=) set -- "$EXE" --print-paths "--data=$X" ;;
        env) set -- env "RECOMPLAUNCHER_DATA=$X" "$EXE" --print-paths ;;
    esac
    expect "with a data folder ($how): Windows's layout under it (settings.toml, hdd, logs, achievements.txt, build/Buffy.dylib, cache/...), the compiler beside the executable" "$@" <<EOF
data: $X
settings: $X/settings.toml
hard disk: $X/hdd
logs: $X/logs
achievements: $X/achievements.txt
textures: $X/textures
screenshots: $X/Screenshots
shader cache: $X/shadercache
disc image: $X/disc_image.txt
compiler: $P/compiler
game code: $X/build/Buffy.dylib
build work: $X/cache/game-code
build log: $X/logs/compile.log
pictures: $X/achievements
icon: $X/Buffy.png
pictures marker: $X/pictures.stamp
EOF
done
expect "--data beats RECOMPLAUNCHER_DATA" env "RECOMPLAUNCHER_DATA=$D/other" "$EXE" --print-paths --data "$X" <<EOF
data: $X
EOF
# in an app (Contents/MacOS), a copy of the executable, with HOME in the scratch folder
APP="$D/Fake é.app"
mkdir -p "$APP/Contents/MacOS"
cp -c "$B/Buffy" "$APP/Contents/MacOS/Buffy"
[ -d "$B/Frameworks" ] && ln -s "$B/Frameworks" "$APP/Contents/MacOS/Frameworks"
AE=$APP/Contents/MacOS/Buffy
A="$HOME/Library/Application Support/Buffy"
expect "in an app with no data folder named: the app's own folder, as it always was (settings.toml, hdd, logs/, logs/achievements.txt, ...), the compiler in Contents/Resources" "$AE" --print-paths <<EOF
data: $A
app folder: $APP/Contents/Resources
settings: $A/settings.toml
hard disk: $A/hdd
logs: $A/logs
achievements: $A/logs/achievements.txt
textures: $A/textures
screenshots: $A/Screenshots
shader cache: $A/shadercache
disc image: $A/disc_image.txt
compiler: $APP/Contents/Resources/compiler
game code: $A/build/Buffy.dylib
pictures: $A/achievements
icon: $A/Buffy.png
EOF
"$AE" --print-paths | grep -qE "^log: $A/logs/buffy(_NNN)?\.log$"; chk $? "and its log is logs/buffy.log there"
expect "in an app with a data folder named: Windows's layout, the compiler still in Contents/Resources" "$AE" --print-paths --data "$X" <<EOF
data: $X
settings: $X/settings.toml
achievements: $X/achievements.txt
compiler: $APP/Contents/Resources/compiler
EOF
nothing_made; chk $? "--print-paths made nothing (the home folder is empty, and so are the data folders)"
[ ! -e "$X" ]; chk $? "--print-paths did not make the data folder"

echo "== no game, a wrong game"
mkdir -p "$D/wrong"
python3 -I - "$REPO/assets/Default.xbe" "$D/wrong/Default.xbe" <<'PY'
import sys
b = bytearray(open(sys.argv[1], "rb").read())
b[0x200000] ^= 0xFF      # a byte of a section: another release (a certificate or header byte would be the same code)
open(sys.argv[2], "wb").write(b)
PY
printf 'not a disc image\n' > "$D/notadisc.iso"
head -c 200000 "$DISC" > "$D/short.iso"
# prep_code <label> <expected code> <args...>: --prepare with a data folder of its own, the reason on stderr and in compile.log
prep_code() {
    local label=$1 want=$2; shift 2
    local d="$D/data-$want-$RANDOM"
    "$EXE" --prepare --data "$d" "$@" > "$D/o.out" 2> "$D/o.err"; local code=$?
    [ "$code" = "$want" ] && grep -q '^error: ' "$D/o.err" && grep -q '^FAILED: ' "$d/logs/compile.log" 2> /dev/null; chk $? "$label: exit $code (wanted $want), the reason on stderr (\"$(grep -m1 '^error: ' "$D/o.err" | cut -c8-80)...\") and in compile.log"
    [ ! -s "$D/o.out" ]; chk $? "  and no progress line"
}
prep_code "--prepare with no game" 2
prep_code "--prepare with a game that is not there" 2 --game "$D/nothing.iso"
prep_code "--prepare with a file that is no disc image" 2 --game "$D/notadisc.iso"
prep_code "--prepare with a disc image cut short" 2 --game "$D/short.iso"
prep_code "--prepare with another release of the game (a byte of the code changed)" 3 --game "$D/wrong"
rm -rf "$D/wrong" "$D/short.iso" "$D/notadisc.iso"
"$EXE" --no-dialogs --data "$D/data-nodialogs-nogame" > /dev/null 2>&1; [ $? = 2 ]; chk $? "a start with --no-dialogs and no game, none kept: exit 2, nobody asked"

echo "== the first start, from the disc, into a data folder with a space and an accent"
LOG=$X/logs/compile.log
t0=$(now)
"$EXE" --prepare --game "$DISC" --data "$X" > "$D/p1.out" 2> "$D/p1.err"; code=$?
t1=$(now)
chk $code "--prepare from the disc: exit $code"
echo "first start: $(secs "$t0" "$t1") s ($(wc -l < "$D/p1.out" | tr -d ' ') progress lines; $(grep -c . "$D/p1.err") lines on stderr)"
grep -qvE '^progress [01]\.[0-9]{3} .' "$D/p1.out"; [ $? != 0 ] && [ -s "$D/p1.out" ]; chk $? "stdout holds nothing but \"progress <0-1> <text>\" lines"
awk '{ if ($2 + 0 < last - 0.0005) bad = 1; last = $2 + 0 } END { exit bad }' "$D/p1.out"; chk $? "the fraction never goes back"
head -1 "$D/p1.out" | grep -q '^progress 0\.000 '; chk $? "it starts at 0 ($(head -1 "$D/p1.out"))"
tail -1 "$D/p1.out" | grep -q '^progress 1\.000 Done$'; chk $? "and ends at 1 ($(tail -1 "$D/p1.out"))"
grep -q '^progress .* Compiling the game' "$D/p1.out"; chk $? "with the compiling step in between ($(grep -c 'Compiling the game' "$D/p1.out") lines)"
grep -q '^error: ' "$D/p1.err"; [ $? != 0 ]; chk $? "no error on stderr"
grep '^time: ' "$LOG" | tr '\n' ';' | sed 's/;/; /g'; echo
[ -f "$X/build/Buffy.dylib" ] && [ -f "$X/build/Buffy.dylib.stamp" ]; chk $? "build/Buffy.dylib and its stamp are in the data folder"
"$EXE" --check-game-code --data "$X" > "$D/cg.out" 2> "$D/cg.err"; chk $? "--check-game-code --data loads it: $(grep '^externs' "$D/cg.out"), $(grep '^load' "$D/cg.out")"
n=$(ls "$X/achievements"/*.png 2> /dev/null | wc -l | tr -d ' '); [ "$n" = 28 ]; chk $? "28 achievement pictures in <data>/achievements ($n)"
[ -s "$X/Buffy.png" ] && [ -s "$X/Buffy.ico" ] && [ "$(cat "$X/pictures.stamp")" = "pictures 1" ]; chk $? "Buffy.png, Buffy.ico and the marker pictures.stamp (\"$(cat "$X/pictures.stamp" 2> /dev/null)\")"
if [ -d assets/achievements ]; then python3 -I tools/compare_pngs.py "$X/achievements" assets/achievements > /dev/null; chk $? "the pictures are the same pixels as assets/achievements"; fi
[ ! -e "$P/build" ] && [ ! -e "$P/achievements" ] && [ ! -e "$P/logs" ] && [ ! -e "$P/cache" ]; chk $? "nothing was written beside the executable (build, achievements, logs, cache)"
nothing_made; chk $? "nor into the home folder"
[ -z "$(find "$X" -maxdepth 1 \( -name hdd -o -name settings.toml \) | head -1)" ]; chk $? "--prepare made no settings.toml or hdd (it plays nothing)"
grep -q '^time: total' "$LOG"; chk $? "compile.log has the build's times"
mtime=$(stat -f %m "$X/build/Buffy.dylib"); logtime=$(stat -f %m "$LOG")

echo "== the second start: nothing runs"
t0=$(now)
"$EXE" --prepare --game "$DISC" --data "$X" > "$D/p2.out" 2> "$D/p2.err"; code=$?
t1=$(now)
[ "$code" = 0 ] && [ "$(cat "$D/p2.out")" = "progress 1.000 The game's code is up to date" ]; chk $? "--prepare again: exit $code, \"$(cat "$D/p2.out")\" in $(secs "$t0" "$t1") s"
[ "$(stat -f %m "$X/build/Buffy.dylib")" = "$mtime" ] && [ "$(stat -f %m "$LOG")" = "$logtime" ]; chk $? "the dylib and compile.log were not touched"

echo "== a damaged dylib, and one made for another version"
cp -c "$X/build/Buffy.dylib" "$D/good.dylib"
head -c 100000 "$D/good.dylib" > "$X/build/Buffy.dylib"
"$EXE" --check-game-code --data "$X" > /dev/null 2>&1; [ $? = 7 ]; chk $? "a cut dylib does not load (--check-game-code: 7)"
t0=$(now)
"$EXE" --prepare --game "$DISC" --data "$X" > "$D/p3.out" 2> "$D/p3.err"; code=$?
t1=$(now)
chk $code "--prepare makes it again: exit $code in $(secs "$t0" "$t1") s (the objects are reused: only the link runs)"
cmp -s "$X/build/Buffy.dylib" "$D/good.dylib"; chk $? "and the same bytes come out"
grep -q '^time: compile .* (0 files compiled' "$LOG"; chk $? "no file was compiled again ($(grep '^time: compile' "$LOG"))"
python3 -I - "$X/build/Buffy.dylib" <<'PY'
import sys
p = sys.argv[1]
b = bytearray(open(p, "rb").read())
i = b.index(b"xbe-game-code abi=abi")
b[i + 18] = ord("x")   # "abi=xbi...": another version's stamp, the same length
open(p, "wb").write(b)
PY
"$EXE" --no-dialogs --game "$DISC" --data "$X" > /dev/null 2> "$D/s.err"; code=$?
[ "$code" = 8 ]; chk $? "a start with --no-dialogs on a dylib of another version: exit $code (wanted 8), no build, no window"
"$EXE" --prepare --game "$DISC" --data "$X" > "$D/p4.out" 2> "$D/p4.err"; code=$?
chk $code "--prepare makes it again: exit $code"
cmp -s "$X/build/Buffy.dylib" "$D/good.dylib"; chk $? "the same bytes come out"
rm -f "$D/good.dylib"

echo "== RECOMPLAUNCHER_DATA, with the same data"
RDATA="$D/launcher data é"
cp -Rc "$X" "$RDATA"
RECOMPLAUNCHER_DATA="$RDATA" "$EXE" --prepare --game "$DISC" > "$D/r.out" 2> /dev/null; code=$?
[ "$code" = 0 ] && [ "$(cat "$D/r.out")" = "progress 1.000 The game's code is up to date" ]; chk $? "--prepare with RECOMPLAUNCHER_DATA and no --data: exit $code, up to date"
RECOMPLAUNCHER_DATA="$RDATA" "$EXE" --check-game-code > /dev/null 2>&1; chk $? "--check-game-code with RECOMPLAUNCHER_DATA loads the dylib there"
"$EXE" --check-game-code > /dev/null 2>&1; [ $? = 7 ]; chk $? "and without a data folder it looks beside the executable and finds none (7)"
rm -rf "$RDATA"

echo "== starts under --no-dialogs that end before there is a window"
N="$D/data-nodialogs"
mkdir -p "$N"
"$EXE" --no-dialogs --game "$DISC" --data "$N" > /dev/null 2> "$D/n.err"; code=$?
[ "$code" = 7 ] && [ ! -e "$N/build" ] && [ ! -e "$N/cache" ]; chk $? "no dylib: exit $code (wanted 7), nothing built"
printf '%s\n' "$DISC" > "$N/disc_image.txt"
"$EXE" --no-dialogs --data "$N" > /dev/null 2> "$D/n.err"; code=$?
[ "$code" = 7 ] && [ "$(cat "$N/disc_image.txt")" = "$DISC" ]; chk $? "the disc kept in <data>/disc_image.txt is found again with no --game (7: no dylib), and kept"

echo "== a build stopped by a signal, and carried on; --clean-work"
S="$D/data-stopped"
"$EXE" --prepare --game "$DISC" --data "$S" > "$D/k.out" 2> "$D/k.err" &
pid=$!
# when a good part of the files are compiled, so that the next run has something to reuse
for _ in $(seq 1 600); do grep -qE 'Compiling the game.*file [4-9][0-9] of' "$D/k.out" 2> /dev/null && break; sleep 0.1; done
kill -TERM $pid 2> /dev/null
wait $pid; code=$?
[ "$code" = 9 ] && grep -q '^error: stopped' "$D/k.err"; chk $? "SIGTERM while compiling (after about 40 files): exit $code (wanted 9), \"$(grep -m1 '^error: ' "$D/k.err" | cut -c1-60)...\""
sleep 0.5
! pgrep -f "$P/compiler/clang" > /dev/null; chk $? "no compiler is left running"
[ ! -e "$S/build/Buffy.dylib" ]; chk $? "and no dylib was made"
t0=$(now)
"$EXE" --prepare --game "$DISC" --data "$S" --clean-work > "$D/k2.out" 2> "$D/k2.err"; code=$?
t1=$(now)
compiled=$(sed -n 's/^time: compile [0-9]* ms (\([0-9]*\) files compiled.*/\1/p' "$S/logs/compile.log")
[ "$code" = 0 ] && [ -n "$compiled" ] && [ "$compiled" -lt 129 ]; chk $? "the next --prepare carries on: exit $code in $(secs "$t0" "$t1") s ($(grep '^time: compile' "$S/logs/compile.log"): the files compiled before the stop are reused)"
[ -f "$S/build/Buffy.dylib" ] && [ ! -e "$S/cache/game-code" ]; chk $? "--clean-work: the dylib is there and cache/game-code is gone"
"$EXE" --check-game-code --data "$S" > /dev/null 2>&1; chk $? "and it loads"

echo "== the progress window was never opened"
! grep -rl 'the progress window is open' "$D" --include='*.log' --include='*.err' 2> /dev/null | grep -q .; chk $? "none of the logs says it was (every run above was windowless)"

echo
if [ "$fail" = 0 ]; then echo "the Mac first start: all checks passed"; else echo "some check did not pass"; fi
exit $fail
