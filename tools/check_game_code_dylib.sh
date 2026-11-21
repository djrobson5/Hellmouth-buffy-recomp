#!/bin/bash
# Checks the macOS executable and the game code dylib that sdk/host/build.sh --game-code-dylib builds (the Mach-O counterpart of
# tools/check_game_code_dll.sh, which does the same for the Windows executable and DLL): nothing is left undefined that the executable and
# the system do not provide, every override is the executable's, every other function is the dylib's own, the stamps agree, and the
# executable loads the dylib (--check-game-code: no window, no game files) and refuses one that is stale or lacks a name.
#   tools/check_game_code_dylib.sh [build folder]    default build/app-dylib; LLVM_MINGW names the toolchain, GEN the generated folder
# Prints PASS or FAIL for each check and the lists behind them; exits 1 on a FAIL. Opens no window and starts no game.
cd "$(dirname "$0")/.."
B=$(realpath "${1:-build/app-dylib}") && [ -d "$B" ] || { echo "FAIL  no build folder ${1:-build/app-dylib}: run sdk/host/build.sh -r . -g generated/game -o build/app-dylib -n Buffy --game-code-dylib"; exit 1; }
GEN=$(realpath "${GEN:-generated/game}") || { echo "FAIL  no generated folder (GEN=)"; exit 1; }
TC=${LLVM_MINGW:-$HOME/Projects/toolchains/llvm-mingw}/bin
EXE=$B/Buffy DYL=$B/build/Buffy.dylib OVR=$B/compiler/overrides.txt CODE=$B/game_code
W=$(mktemp -d "$B/check.XXXXXX") || exit 1   # in the (ignored) build folder: the dylib is copied in here, and is made from the game
trap '[ -n "$W" ] && [ -d "$W" ] && rm -rf -- "$W"' EXIT
fail=0
chk() { if [ "$1" = 0 ]; then echo "PASS  $2"; else echo "FAIL  $2"; fail=1; fi; }
for f in "$EXE" "$DYL" "$OVR" "$CODE/abi_inputs.txt" "$GEN/exports.def"; do
    [ -f "$f" ] || { echo "FAIL  $f is not there: run sdk/host/build.sh -r . -g generated/game -o build/app-dylib -n Buffy --game-code-dylib"; exit 1; }
done
OBJS=("$B"/cache/game-code/*.o)

# names (Mach-O adds an underscore to each C name). The sub_ functions: weak = the recompiled functions exports.def lists, the
# executable's overrides, and what the dylib defines and leaves undefined.
sed -n 's/^ *__imp__\(sub_[0-9A-F]*\) *$/\1/p' "$GEN/exports.def" | sort -u > "$W"/weak.txt
sort -u "$OVR" > "$W"/overrides.txt
"$TC/llvm-nm" -u "${OBJS[@]}" | awk 'NF==2{print $2}' | sort -u > "$W"/obj_undef.txt
"$TC/llvm-nm" -u "$DYL" | awk '{print $NF}' | sort -u > "$W"/dyl_undef.txt
"$TC/llvm-nm" -gU "$DYL" | awk 'NF==3{print $3}' | sort -u > "$W"/dyl_exports.txt
"$TC/llvm-nm" -gU "$EXE" | awk 'NF==3{print $3}' | sort -u > "$W"/exe_exports.txt
"$TC/llvm-nm" --defined-only "$EXE" | awk '$3 ~ /^_sub_[0-9A-F]{8}$/{print substr($3,2)}' | sort -u > "$W"/exe_sub_bodies.txt   # any binding
"$TC/llvm-nm" -gU "$EXE" | awk '$3 ~ /^_sub_[0-9A-F]{8}$/{print substr($3,2)}' | sort -u > "$W"/exe_sub_exported.txt
comm -12 "$W"/weak.txt "$W"/overrides.txt > "$W"/over_weak.txt           # overrides of functions this image has
comm -23 "$W"/weak.txt "$W"/overrides.txt > "$W"/own.txt                 # functions the dylib keeps
sed 's/^/_/' "$W"/overrides.txt > "$W"/overrides_u.txt
sed 's/^/_/' "$W"/own.txt > "$W"/own_u.txt

echo "objects: ${#OBJS[@]}; recompiled functions (exports.def) $(wc -l < "$W"/weak.txt | tr -d ' '); the executable's overrides $(wc -l < "$W"/overrides.txt | tr -d ' '), of this image's functions $(wc -l < "$W"/over_weak.txt | tr -d ' ')"
echo "dylib: $(wc -l < "$W"/dyl_undef.txt | tr -d ' ') undefined names; exports $(wc -l < "$W"/dyl_exports.txt | tr -d ' '); executable exports $(wc -l < "$W"/exe_exports.txt | tr -d ' ')"

# --- what the dylib leaves undefined
grep '^_sub_' "$W"/dyl_undef.txt > "$W"/dyl_undef_sub.txt
others=$(grep -v '^_sub_' "$W"/dyl_undef.txt | comm -23 - "$W"/exe_exports.txt)
echo "--- undefined in the dylib, not the executable's (libSystem and the C++ runtime, which the executable brings in): $(echo $others | wc -w | tr -d ' ')"; echo "$others" | sed 's/^/    /' | tr '\n' ' ' | fold -s -w 150; echo
# every one is a name the system's libraries are known to have; a new one means the code uses a library call that this list (and so the
# check that the dylib loads) has not seen
expected="___cxa_begin_catch ___cxa_end_catch ___cxa_rethrow ___gxx_personality_v0 ___sincos_stret __Unwind_Resume __ZSt9terminatev _atan2 _bzero _cos _exp2 _fmod _frexp _ldexp _log2 _memcpy _memmove _memset _memset_pattern16 _sin _tan"
missing=$(echo $others | tr ' ' '\n' | grep -vxF -f <(echo $expected | tr ' ' '\n')); [ -z "$missing" ]; chk $? "nothing is undefined in the dylib that the executable and libSystem do not provide (unexpected: ${missing:-none})"
# the host API the generated code calls: all exported by the executable
api=$(grep -v '^_sub_' "$W"/dyl_undef.txt | grep '^__Z' | grep -v '^__ZSt' | comm -23 - "$W"/exe_exports.txt); [ -z "$api" ]; chk $? "every runtime function the dylib calls (x86_icall and the rest) is exported by the executable (missing: ${api:-none})"
notexp=$(comm -23 "$W"/dyl_undef_sub.txt "$W"/exe_exports.txt); [ -z "$notexp" ]; chk $? "every sub_ name the dylib leaves undefined is exported by the executable (missing: ${notexp:-none})"
unover=$(comm -23 "$W"/dyl_undef_sub.txt "$W"/overrides_u.txt); [ -z "$unover" ]; chk $? "and every one of them is an override (a function with no override left undefined would fail the load): ${unover:-none}"
echo "--- overrides the dylib calls through its undefined names: $(comm -12 "$W"/dyl_undef_sub.txt "$W"/overrides_u.txt | wc -l | tr -d ' ') of $(wc -l < "$W"/over_weak.txt | tr -d ' ')"

# --- overrides
cmp -s "$W"/exe_sub_bodies.txt "$W"/exe_sub_exported.txt; chk $? "the sub_ functions the executable defines are all exported (a replacement that is hidden would be left unseen by the dylib)"
cmp -s "$W"/exe_sub_exported.txt "$W"/overrides.txt; chk $? "overrides.txt is exactly the sub_ functions the executable defines ($(wc -l < "$W"/overrides.txt | tr -d ' '))"
def=$(comm -12 "$W"/overrides_u.txt "$W"/dyl_exports.txt); [ -z "$def" ]; chk $? "the dylib defines none of the overrides (a definition there would win over the executable's: ${def:-none})"
[ "$(comm -12 "$W"/own.txt "$W"/overrides.txt | wc -l | tr -d ' ')" = 0 ]; chk $? "overrides.txt and the functions the dylib keeps do not overlap"
exe_bodies=$("$TC/llvm-nm" --defined-only "$EXE" | awk '$3 ~ /^___imp__sub_/{print $3}' | wc -l | tr -d ' '); [ "$exe_bodies" = 0 ]; chk $? "the executable holds no recompiled body (__imp__sub_*: $exe_bodies)"
"$TC/llvm-nm" "$EXE" | grep -q "_X86FuncMappings"; [ $? != 0 ]; chk $? "and none of the recompiler's tables (X86FuncMappings)"
# The sources of the overrides are in the folders the ABI token hashes, as on Windows. A host object is named by the path of its source
# (harness/build.sh turns each / into _), so each object that defines an override is matched to the source file of that name, and the
# source's path must lie under one of the hashed folders (whole path segments: src/game/, sdk/runtime/xdk/). No object, or one that
# matches no source, is a failure: the check would otherwise pass over nothing.
"$TC/llvm-nm" -A --defined-only "$B"/host/*.o | sed -nE 's/^(.*\.o): +[0-9a-f]+ [A-Za-z] _sub_[0-9A-F]{8}$/\1/p' | sed -E 's/^.*\///; s/\.[0-9]+\.o$//' | sort -u > "$W"/override_objects.txt
sed -n 's/^override-dir //p' "$CODE/abi_inputs.txt" > "$W"/hashed_dirs.txt
find "$PWD/src" "$PWD/sdk" \( -name '*.cpp' -o -name '*.mm' \) | while IFS= read -r f; do n=${f#/}; n=${n//\//_}; printf '%s\t%s\n' "${n%.*}" "${f#"$PWD"/}"; done | sort > "$W"/source_names.txt
outside=""; unmatched=""; nobj=0
while IFS= read -r o; do
    [ -n "$o" ] || continue
    nobj=$((nobj + 1))
    rel=$(awk -F'\t' -v o="$o" '$1 == o { print $2; exit }' "$W"/source_names.txt)
    if [ -z "$rel" ]; then unmatched="$unmatched $o"; continue; fi
    ok=1; while IFS= read -r d; do case "$rel" in "$d"/*) ok=0;; esac; done < "$W"/hashed_dirs.txt
    [ $ok = 0 ] || outside="$outside $rel"
done < "$W"/override_objects.txt
echo "--- host sources defining overrides: $nobj; folders the ABI token hashes: $(tr '\n' ' ' < "$W"/hashed_dirs.txt)"
[ -z "$outside" ] && [ -z "$unmatched" ] && [ "$nobj" -gt 0 ]; chk $? "every source that defines an override is in a folder the ABI token hashes ($nobj found; outside:${outside:- none}; no source for:${unmatched:- none})"

# --- what stays in the dylib
"$TC/llvm-nm" -gU "$DYL" | awk 'NF==3{print $1, $3}' | sort -k2 > "$W"/dyl_addr.txt
join -1 2 -2 2 <(grep -E ' _sub_' "$W"/dyl_addr.txt) <(grep -E ' ___imp__sub_' "$W"/dyl_addr.txt | sed 's/___imp__sub_/_sub_/') | awk '$2==$3' | wc -l | tr -d ' ' > "$W"/alias_same.txt
own=$(wc -l < "$W"/own.txt | tr -d ' ')
have=$(comm -12 "$W"/own_u.txt "$W"/dyl_exports.txt | wc -l | tr -d ' ')
[ "$have" = "$own" ]; chk $? "every function without an override is defined in the dylib under its own name ($have of $own)"
[ "$(cat "$W"/alias_same.txt)" = "$own" ]; chk $? "and is the same address as its body (an alias of __imp_sub_X: $(cat "$W"/alias_same.txt) of $own)"
grep -E '^_sub_' "$W"/dyl_exports.txt | sed 's/^_//' | sort -u > "$W"/dyl_sub_exports.txt
cmp -s "$W"/dyl_sub_exports.txt "$W"/own.txt; chk $? "the dylib defines no other sub_ name (it exports exactly the $own)"
tables=$(for n in X86FuncMappings X86Continuations X86CodeBase X86CodeEnd X86EntryPoint X86GameCodeStamp; do grep -qx "_$n" "$W"/dyl_exports.txt || echo $n; done); [ -z "$tables" ]; chk $? "the dylib exports the tables the loader looks up (missing: ${tables:-none})"
# what the executable calls in the dylib (X86_EXTERN_FUNC): each is a body the dylib exports
"$TC/llvm-strings" -a "$EXE" | grep -E '^__imp__sub_[0-9A-F]{8}$' | sort -u > "$W"/slots.txt
miss=$(sed 's/^/_/' "$W"/slots.txt | comm -23 - "$W"/dyl_exports.txt); [ -z "$miss" ] && [ -s "$W"/slots.txt ]; chk $? "the dylib exports the $(wc -l < "$W"/slots.txt | tr -d ' ') bodies the executable calls (X86_EXTERN_FUNC) (missing: ${miss:-none})"

# --- the stamp and the token
"$TC/llvm-strings" -a "$EXE" | grep '^xbe-game-code ' | sort -u > "$W"/exe_stamp.txt
"$TC/llvm-strings" -a "$DYL" | grep '^xbe-game-code ' | sort -u > "$W"/dyl_stamp.txt
echo "--- stamp in the executable: $(cat "$W"/exe_stamp.txt)"; echo "--- stamp in the dylib:     $(cat "$W"/dyl_stamp.txt)"
[ $(wc -l < "$W"/exe_stamp.txt) -eq 1 ] && [ $(wc -l < "$W"/dyl_stamp.txt) -eq 1 ]; chk $? "one stamp in the executable and one in the dylib"
cmp -s "$W"/exe_stamp.txt "$W"/dyl_stamp.txt; chk $? "the dylib's stamp is the executable's"
token=$(sed -n 's/^token //p' "$CODE/abi_inputs.txt")
grep -q "abi=$token " "$W"/exe_stamp.txt; chk $? "the stamp's ABI token is the one xbe_abi.cmake wrote to abi_inputs.txt ($token)"
cmake -DPLATFORM=macos -DSDK_DIR="$PWD/sdk" -DSOURCE_DIR="$PWD" -DGAME_DIR="$(basename "$GEN")" -DOUT_DIR="$W"/abi-macos -P sdk/cmake/xbe_abi.cmake 2>&1 | grep -q "token $token"; chk $? "the token is what the files give now (the dylib was built from the tree as it is)"
# one implementation of the token: the windows platform of the same script is the token of the Windows build of this tree
WB=out/build/win-x64-mingw/game_code
if [ -f "$WB/abi_inputs.txt" ]; then
    cmake -DPLATFORM=windows -DSDK_DIR="$PWD/sdk" -DSOURCE_DIR="$PWD" -DGAME_DIR="$(basename "$GEN")" -DOUT_DIR="$W"/abi-windows -P sdk/cmake/xbe_abi.cmake > /dev/null 2>&1
    cmp -s "$W"/abi-windows/abi_inputs.txt "$WB"/abi_inputs.txt && cmp -s "$W"/abi-windows/game_code_flags.h "$WB"/game_code_flags.h
    chk $? "xbe_abi.cmake run for windows gives the Windows build's own abi_inputs.txt and game_code_flags.h (one implementation: $(sed -n 's/^token //p' "$WB"/abi_inputs.txt)); if this fails after a change to a hashed file, run cmake --preset win-x64-mingw again first (it is the stale Windows build folder that differs)"
else
    echo "SKIP  no Windows build in out/build/win-x64-mingw to compare the token with"
fi

# --- the file
otool -hv "$DYL" | grep -q "ARM64.*DYLIB"; chk $? "the dylib is arm64 and a dynamic library"
otool -l "$DYL" | grep -A4 LC_BUILD_VERSION | grep -q "minos 13.0"; chk $? "it targets macOS 13.0"
deps=$(otool -L "$DYL" | tail -n +2 | grep -v "@rpath/Buffy.dylib" | wc -l | tr -d ' '); [ "$deps" = 0 ]; chk $? "it depends on no other library (its names bind to the executable and the system when it is loaded: $deps)"
otool -D "$DYL" | tail -1 | grep -qx "@rpath/Buffy.dylib"; chk $? "its install name is @rpath/Buffy.dylib"
codesign --verify "$DYL" 2>/dev/null; chk $? "it is signed (ad hoc, by the linker: an unsigned dylib will not load on arm64)"
otool -l "$DYL" | grep sectname | grep -qE "__mod_init_func|__init_offsets|__thread_|__mod_term_func"; [ $? != 0 ]; chk $? "no constructor, initializer or thread local section in the dylib (nothing needs running when it is loaded)"
otool -l "$DYL" | grep -q "LC_FUNCTION_STARTS"; [ $? = 0 ] || true
nm -u "$B/Buffy" 2>/dev/null | grep -q "_X86FuncMappings\|_sub_"; [ $? != 0 ]; chk $? "the executable has no undefined name of the game's code (it reaches the dylib by dlsym alone, so it starts without one)"
otool -L "$EXE" | grep -q "Buffy.dylib"; [ $? != 0 ]; chk $? "the dylib is not one of the executable's load-time dependencies"

# --- the executable loads it: no window, no game files, no disc (--check-game-code exits before the log, SDL and the game)
cd "$B"
t0=$SECONDS
./Buffy --check-game-code > "$W"/load.out 2> "$W"/load.err; code=$?
echo "--- --check-game-code:"; sed 's/^/    /' "$W"/load.out
[ $code = 0 ]; chk $code "the executable loads the dylib (exit $code; $(grep '^load:' "$W"/load.out))"
grep -q "^stamp: $(cat "$W"/dyl_stamp.txt)\$" "$W"/load.out; chk $? "and the stamp it says it holds is the dylib's"
fn=$(sed -n 's/^functions: //p' "$W"/load.out); cont=$(sed -n 's/^continuations: //p' "$W"/load.out)
# the tables the recompiler wrote, counted in the source: the loader must see all of them
want_fn=$(awk '/^const X86FuncMapping X86FuncMappings/{f=1;next} f&&/^\t\{0, nullptr\}/{f=0} f&&/^\t\{0x/{n++} END{print n+0}' "$GEN/x86_func_mapping.cpp")
want_cont=$(awk '/^const uint32_t X86Continuations/{f=1;next} f&&/^\t0,/{f=0} f&&/^\t0x/{n++} END{print n+0}' "$GEN/x86_func_mapping.cpp")
[ "$fn" = "$want_fn" ] && [ "${want_fn:-0}" -gt 0 ]; chk $? "its function table has $fn entries, as many as x86_func_mapping.cpp lists ($want_fn; the image's absent names have bodies but no entry)"
[ "$cont" = "$want_cont" ]; chk $? "its continuation table has $cont entries, as in the source ($want_cont)"
n_over=$("$TC/llvm-nm" -gU "$EXE" | awk '$3 ~ /^_sub_/' | wc -l | tr -d ' ')
grep -q "^overrides: $n_over defined here" "$W"/load.out; chk $? "the loader checked all $n_over sub_ functions the executable defines against the dylib ($(grep '^overrides:' "$W"/load.out))"
grep -q "^externs: $(wc -l < "$W"/slots.txt | tr -d ' ') of $(wc -l < "$W"/slots.txt | tr -d ' ') filled" "$W"/load.out; chk $? "every call the executable makes into the dylib was found by name ($(grep '^externs:' "$W"/load.out))"
# refusing: no dylib, one for another build, one that wants a name the executable does not have
./Buffy --check-game-code --game-code ""$W"/none.dylib" > /dev/null 2> "$W"/e1.err; code=$?
[ $code = 7 ] && grep -q "was not found" "$W"/e1.err; chk $? "a missing dylib is refused with exit 7 and a reason ($(cut -c1-60 "$W"/e1.err | head -1)... )"
cp "$DYL" "$W"/stale.dylib; perl -0777 -pi -e "s/abi=abi/abj=abi/" "$W"/stale.dylib; codesign -f -s - "$W"/stale.dylib 2>/dev/null
./Buffy --check-game-code --game-code "$W"/stale.dylib > /dev/null 2> "$W"/e2.err; code=$?
[ $code = 8 ] && grep -q "another version" "$W"/e2.err; chk $? "a dylib with another stamp is refused as stale, exit 8 ($(tail -1 "$W"/e2.err | cut -c1-60)...)"
victim=$(head -1 "$W"/dyl_undef_sub.txt)
cp "$DYL" "$W"/lacking.dylib; perl -0777 -pi -e "s/${victim}\\0/_sub_NoSuchNm\\0/" "$W"/lacking.dylib 2>/dev/null; codesign -f -s - "$W"/lacking.dylib 2>/dev/null
if ! cmp -s "$DYL" "$W"/lacking.dylib; then
    ./Buffy --check-game-code --game-code "$W"/lacking.dylib > /dev/null 2> "$W"/e3.err; code=$?
    [ $code = 8 ] && grep -q "_sub_NoSuchNm" "$W"/e3.err; chk $? "a dylib that needs a name the executable lacks fails to load naming it, exit 8 ($(grep -o "symbol not found[^)]*" "$W"/e3.err | head -1))"
else
    echo "FAIL  could not make the dylib that lacks a name (${victim} not patched)"; fail=1
fi
# a dylib that defines what the executable overrides (linked with a stale or empty overrides list) loads all the same, and its recompiled bodies
# would run in place of the replacements: refused, naming the first
awk '{ print "-alias ___imp__" $1 " _" $1 }' "$W"/weak.txt > "$W"/all_aliases.rsp
"$B"/compiler/ld64.lld -dylib -arch arm64 -platform_version macos 13.0 13.0 -undefined dynamic_lookup -install_name @rpath/Buffy.dylib \
    @"$W"/all_aliases.rsp -o "$W"/bypass.dylib "${OBJS[@]}" > "$W"/bypass.log 2>&1
if [ -f "$W"/bypass.dylib ]; then
    ./Buffy --check-game-code --game-code "$W"/bypass.dylib > /dev/null 2> "$W"/e4.err; code=$?
    [ $code = 8 ] && grep -q "which this executable overrides" "$W"/e4.err; chk $? "a dylib that defines an overridden function (an empty overrides list at its link) is refused, exit 8 ($(grep -o "the dylib defines [^,]*" "$W"/e4.err | head -1))"
else
    echo "FAIL  could not link the dylib with no overrides"; fail=1
fi
cd - > /dev/null
# the link refuses an empty list of overrides (the first start's own build code, over the objects already made), and so does the tool that makes it
mkdir -p "$W"/app "$W"/data && cp -R "$B"/compiler "$W"/app/ && : > "$W"/app/compiler/overrides.txt && cp -Rc "$B"/cache "$W"/data/cache
"$CODE"/first_start_check --app "$W"/app --data "$W"/data --sources "$GEN" > /dev/null 2> "$W"/e5.err; code=$?
[ $code = 9 ] && grep -q "names no override" "$W"/e5.err && [ ! -f "$W"/data/build/Buffy.dylib ]; chk $? "a build whose overrides list is empty fails at the link and installs nothing (exit $code: $(grep -o "names no override" "$W"/e5.err | head -1))"
bash sdk/host/list_overrides.sh /bin/ls "$W"/none.txt > /dev/null 2> "$W"/e6.err; code=$?
[ $code = 1 ] && [ ! -e "$W"/none.txt ] && grep -q "defines no sub_" "$W"/e6.err; chk $? "list_overrides.sh fails, and writes nothing, for a program that defines no override (exit $code)"
exit $fail
