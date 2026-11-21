#!/bin/bash
# Checks the Windows executable and the game code DLL that the win-x64-mingw preset builds on a Mac (the game_code_dll target),
# as the player's PC will link them: nothing is left undefined, and the DLL imports exactly what the executable exports.
#   tools/check_game_code_dll.sh [build folder]    default out/build/win-x64-mingw; LLVM_MINGW names the toolchain
# Prints PASS or FAIL for each check and the lists behind them; exits 1 on a FAIL.
# The last group of checks compiles tools/library_calls.cpp, a synthetic stand-in for recompiled code that uses every library
# call the recompiler can emit (not only the ones this game's code uses), and links it against the executable's import library.
cd "$(dirname "$0")/.."
B=${1:-out/build/win-x64-mingw}
TC=${LLVM_MINGW:-$HOME/Projects/toolchains/llvm-mingw}/bin
EXE=$B/Buffy.exe DLL=$B/game_code/Buffy.dll
W=$(mktemp -d)
fail=0
chk() { if [ "$1" = 0 ]; then echo "PASS  $2"; else echo "FAIL  $2"; fail=1; fi; }

# every name the DLL's objects leave undefined, with dllimport's __imp_ prefix taken off
$TC/llvm-nm -u $B/game_code/*.obj | awk 'NF==2{print $2}' | sed 's/^__imp__Z/_Z/' | sort -u > $W/undef_obj.txt
$TC/llvm-nm --defined-only $B/game_code/*.obj | awk 'NF==3{print $3}' | sort -u > $W/defined_obj.txt
# what the DLL imports, from the import table, and what the executable exports
$TC/llvm-objdump -p $DLL | awk '/DLL Name:/{d=$3} /^ +[0-9]+ +[^ ]+$/ && d=="Buffy.exe"{print $2}' | sort -u > $W/dll_imports.txt
$TC/llvm-objdump -p $EXE | awk '/^Export Table/{e=1} e && /^ +[0-9]+ +0x[0-9a-f]+ +/{print $3}' | sort -u > $W/exe_exports.txt
# a recompiled function with no override in the executable is found in the DLL (the alternatename that X86_WEAK_FUNC says)
awk '/^__imp__sub_/{print substr($1,8)}' $W/defined_obj.txt | sort -u > $W/local_names.txt
comm -12 $W/undef_obj.txt $W/local_names.txt | comm -23 - $W/exe_exports.txt > $W/resolved_locally.txt
comm -23 $W/undef_obj.txt $W/resolved_locally.txt > $W/needs_exe.txt

echo "objects: $(ls $B/game_code/*.obj | wc -l); undefined names $(wc -l < $W/undef_obj.txt), of which found in the DLL itself $(wc -l < $W/resolved_locally.txt)"
echo "DLL imports from Buffy.exe: $(wc -l < $W/dll_imports.txt); Buffy.exe exports: $(wc -l < $W/exe_exports.txt)"
echo "--- DLL imports that are not recompiled functions:"; grep -v '^sub_' $W/dll_imports.txt | sed 's/^/    /'
echo "--- exe exports that are recompiled functions (overrides): $(grep -c '^sub_' $W/exe_exports.txt)"

diff -q $W/needs_exe.txt $W/dll_imports.txt >/dev/null; chk $? "undefined in the DLL's objects, less what it has itself, == the DLL's import table"

# the synthetic code, compiled and linked as the game code DLL is: the flags of the executable's game_code_flags.h
CF=$(sed -n 's/#define XBE_GAME_CODE_COMPILE_FLAGS L"\(.*\)"/\1/p' $B/game_code/game_code_flags.h)
LF=$(sed -n 's/#define XBE_GAME_CODE_LINK_FLAGS L"\(.*\)"/\1/p' $B/game_code/game_code_flags.h)
LC=$W/libcalls; mkdir -p $LC
$TC/clang $CF -isystem $B/game_code/include/freestanding -I$B/game_code/include -c tools/library_calls.cpp -o $LC/lc.obj 2> $LC/compile.log; chk $? "tools/library_calls.cpp compiles as the game code does (the freestanding headers, no C or C++ library)"
$TC/lld-link $LF /implib:$LC/lc.lib /out:$LC/lc.dll $B/game_code/Buffy.lib $LC/lc.obj > $LC/link.log 2>&1; chk $? "and links against Buffy.lib with nothing undefined"
[ -s $LC/compile.log -o -s $LC/link.log ] && { cat $LC/compile.log $LC/link.log | head -20; }
$TC/llvm-nm -u $LC/lc.obj | awk 'NF==2{print $2}' | sed 's/^__imp__Z/_Z/' | sort -u > $LC/undef.txt
$TC/llvm-objdump -p $LC/lc.dll | awk '/DLL Name:/{d=$3} /^ +[0-9]+ +[^ ]+$/ && d=="Buffy.exe"{print $2}' | sort -u > $LC/imports.txt
# the calls the compiler must leave to the executable for what the generator writes (inline forms of sqrt and the rest vary
# with the code around them, so they are not asserted in this list; the link proves they are exported)
required="___chkstk_ms atan2 cos exp2 fmod frexp ldexp log2 memcpy memmove memset remainder sin sqrt sqrtf tan _ZSt9terminatev _Unwind_Resume __cxa_begin_catch __cxa_end_catch __cxa_rethrow __gxx_personality_seh0 _Z9x86_icallR10X86ContextPhj _Z10x86_kernelR10X86ContextPhj _Z14x86_try_resumeR10X86ContextPhj _Z8x86_privR10X86ContextjPKc _Z8x86_trapR10X86ContextjPKc _Z9x86_cpuidR10X86Context _Z9x86_rdtscv _Z9x86_traceR10X86Contextjj"
missing=""; for n in $required; do grep -qx "$n" $LC/undef.txt || missing="$missing $n"; done
[ -z "$missing" ]; chk $? "the synthetic object leaves undefined every library call the generator can need (not found:${missing:- none})"
echo "--- library calls and API the synthetic code imports: $(wc -l < $LC/imports.txt)"; sed 's/^/    /' $LC/imports.txt | tr '\n' ' ' | fold -s -w 150; echo
# every call the generator writes in its output, every builtin and every inline helper of x86_ops.h is used in the synthetic code
grep -ohE '"[^"]*"' sdk/tools/recomp/codegen*.cpp | grep -oE '(std::[a-z_0-9]+|__builtin_[a-z_0-9]+|__atomic_[a-z_0-9]+|(x87|sse|mmx|x86)_[a-z_0-9]+)\(' | sed 's/($//; s/^std:://' | sort -u > $W/gen_calls.txt
grep -hE '^(inline|constexpr) ' sdk/runtime/cpu/x86_ops.h | sed -E 's/\(.*//; s/.*[ *&]//' | sort -u > $W/ops_inline.txt
notused=""; for n in $(cat $W/gen_calls.txt $W/ops_inline.txt | sort -u); do grep -qE "(^|[^a-z_0-9])$n *[(<]" tools/library_calls.cpp || notused="$notused $n"; done
[ -z "$notused" ]; chk $? "tools/library_calls.cpp uses all $(cat $W/gen_calls.txt $W/ops_inline.txt | sort -u | wc -l | tr -d ' ') calls the generator writes and helpers x86_ops.h defines (not used:${notused:- none})"
missing=$(comm -13 $W/exe_exports.txt $W/dll_imports.txt); [ -z "$missing" ]; chk $? "every DLL import is exported by Buffy.exe (missing: ${missing:-none})"
# what the executable exports beyond that is what the synthetic code imports (library calls this game's code happens not to make,
# below), and the overrides of names this image does not have (sub_FFFFxxxx, from the SDK's own list of names, which no recompiled
# code calls): the check is exact in both directions
extra=$(comm -23 $W/exe_exports.txt <(sort -u $W/dll_imports.txt $LC/imports.txt) | grep -vE '^sub_FFFF[0-9A-F]{4}$')
[ -z "$extra" ]; chk $? "Buffy.exe exports nothing that neither this game's DLL nor the synthetic code imports (other: ${extra:-none})"
echo "--- exported, imported by the synthetic code only: $(comm -23 <(sort -u $LC/imports.txt) $W/dll_imports.txt | tr '\n' ' ')"
echo "--- exported, imported by neither: $(comm -23 $W/exe_exports.txt <(sort -u $W/dll_imports.txt $LC/imports.txt) | grep -c '^sub_FFFF') overrides of names the image lacks"
dups=$(comm -12 <(sort -u $W/local_names.txt) $W/dll_imports.txt | wc -l)
echo "--- overridden functions the DLL imports (so the exe's body is the one that runs): $dups"

n=$($TC/llvm-objdump -p $DLL | grep -c "DLL Name:"); [ "$n" = 1 ]; chk $? "Buffy.dll has exactly one import module"
$TC/llvm-objdump -p $DLL | grep "DLL Name:" | grep -q "Buffy.exe"; chk $? "and it is Buffy.exe"
$TC/llvm-readobj --coff-imports $DLL | grep -q DelayImport; [ $? != 0 ]; chk $? "Buffy.dll has no delay imports"
# the exception seam: every unwind handler of the DLL points at one thunk, and it jumps through the import slot of the
# executable's __gxx_personality_seh0 (so a C++ exception thrown in the executable unwinds through the DLL's frames)
h=$($TC/llvm-readobj --unwind $DLL | awk '/Handler:/{gsub(/[()]/,"",$2); print $2}' | sort -u)
echo "--- distinct unwind handler addresses in the DLL: $(echo $h)"
[ $(echo "$h" | wc -w) -eq 1 ]; chk $? "all the DLL's unwind handlers are one address"
idx=$($TC/llvm-objdump -p $DLL | awk '/DLL Name: Buffy.exe/{f=1;next} f && /^ +[0-9]+ +[^ ]+$/{if($2=="__gxx_personality_seh0"){print n; exit} n++}' n=0)
iat=$($TC/llvm-objdump -p $DLL | awk '/^ +lookup .* addr [0-9a-f]+/{print $NF; exit}')
slot=$(printf '%x' $((0x$iat + 8*idx + 0x180000000)))
$TC/llvm-objdump -d $DLL --start-address=$h --stop-address=$(printf '0x%x' $((h + 6))) | tail -1 | grep -q "jmpq.*# 0x$slot"
chk $? "that handler is 'jmp *[0x$slot]', the import slot of __gxx_personality_seh0 (import index $idx)"
# nothing needs initialising: no constructors, C runtime sections or thread storage (the DLL has no entry point)
$TC/llvm-objdump -h $B/game_code/*.obj | grep -qE "\.ctors|\.CRT|init_array|\.tls"; [ $? != 0 ]; chk $? "no constructor, CRT or TLS sections in the DLL's objects"
$TC/llvm-objdump -p $DLL | grep -q "TLS directory"; [ $? != 0 ]; chk $? "Buffy.dll has no TLS directory"
$TC/llvm-objdump -p $DLL | grep -qE "^AddressOfEntryPoint\s+0*$"; chk $? "Buffy.dll has no entry point"

# the executable: system DLLs, and Buffy.dll only delay loaded
$TC/llvm-objdump -p $EXE | awk '/DLL Name:/{print $3}' | sort -u > $W/exe_dlls.txt
echo "--- Buffy.exe's load-time imports: $(wc -l < $W/exe_dlls.txt) DLLs"
grep -qi '^buffy.dll$' $W/exe_dlls.txt; [ $? != 0 ]; chk $? "Buffy.dll is not in the executable's load-time imports"
$TC/llvm-readobj --coff-imports $EXE | grep -A2 DelayImport | grep -q "Name: Buffy.dll"; chk $? "Buffy.dll is delay loaded"
bad=$(grep -viE '^(api-ms-win-[a-z0-9-]+|d3d12|dxgi|d3dcompiler_47|xaudio2_9|ole32|oleaut32|dbghelp|winmm|dwmapi|ws2_32|gdi32|advapi32|kernel32|user32|shell32|setupapi|imm32|version|windowscodecs)\.dll$' $W/exe_dlls.txt)
[ -z "$bad" ]; chk $? "every other import is a system DLL (not: ${bad:-none})"
grep -qiE "libc\+\+|libunwind|winpthread|libstdc" $W/exe_dlls.txt; [ $? != 0 ]; chk $? "no libc++, libunwind or winpthread DLL beside the executable"
# every name the executable imports from the DLL is one the DLL exports
$TC/llvm-readobj --coff-imports $EXE | awk '/DelayImport/{f=1} f && /Symbol:/{print $2}' | sort -u > $W/exe_wants.txt
$TC/llvm-objdump -p $DLL | awk '/Export Table/{e=1} e && /^ +[0-9]+ +0x[0-9a-f]+ +/{print $3}' | sort -u > $W/dll_exports.txt
lost=$(comm -23 $W/exe_wants.txt $W/dll_exports.txt)
echo "--- Buffy.exe delay-imports $(wc -l < $W/exe_wants.txt) names from Buffy.dll, which exports $(wc -l < $W/dll_exports.txt)"
[ -z "$lost" ]; chk $? "every delay import is in the DLL's export table (missing: ${lost:-none})"
[ "$(wc -l < $W/dll_exports.txt)" -le 65535 ]; chk $? "the DLL's exports fit in a PE's 65535"

# the executable holds no recompiled function: the sub_XXXXXXXX bodies it defines are overrides (X86_FUNC in src/ and sdk/runtime/),
# all exported, and the rest are the delay load thunks of the import library
$TC/llvm-nm --defined-only $EXE | awk '$3 ~ /^sub_[0-9A-F]{8}$/{print $3}' | sort -u > $W/exe_sub_bodies.txt
$TC/llvm-nm --defined-only $EXE | awk '$3 ~ /^__imp__sub_[0-9A-F]{8}$/{print $3}' | sort -u > $W/exe_thunks.txt
$TC/llvm-nm -A --defined-only $(ninja -C $B -t inputs Buffy.exe | grep '\.obj$' | sed "s|^|$B/|") | awk '$4 ~ /^sub_[0-9A-F]{8}$/{print $1}' | sed -E 's/^.*Buffy.dir\/(.*)\/[^\/]+\.obj:.*$/\1/' | sort | uniq -c > $W/override_dirs.txt
# the sources of those overrides: the ABI stamp hashes the folders in abi_inputs.txt, so each must be in one of them (a replacement
# defined anywhere else would change without the DLL's stamp saying so)
$TC/llvm-nm -A --defined-only $(ninja -C $B -t inputs Buffy.exe | grep '\.obj$' | sed "s|^|$B/|") | awk '$4 ~ /^sub_[0-9A-F]{8}$/{print $1}' | sed -E 's/^.*Buffy\.dir\/(.*)\.obj:.*$/\1/' | sort -u > $W/override_sources.txt
sed -n 's/^override-dir //p' $B/game_code/abi_inputs.txt > $W/override_dirs_hashed.txt
outside=""; for f in $(cat $W/override_sources.txt); do ok=1; for d in $(cat $W/override_dirs_hashed.txt); do case $f in $d/*) ok=0;; esac; done; [ $ok = 0 ] || outside="$outside $f"; done
echo "--- sources defining overrides: $(wc -l < $W/override_sources.txt); folders the ABI stamp hashes: $(tr '\n' ' ' < $W/override_dirs_hashed.txt)"
[ -z "$outside" ]; chk $? "every source that defines an override is in a folder the ABI stamp hashes (outside:${outside:- none})"
echo "--- sub_XXXXXXXX bodies in Buffy.exe: $(wc -l < $W/exe_sub_bodies.txt), all overrides, from:"; sed 's/^/    /' $W/override_dirs.txt
echo "    plus $(wc -l < $W/exe_thunks.txt) __imp__sub_XXXXXXXX delay load thunks (jumps into the DLL, no code of their own)"
grep '^sub_' $W/exe_exports.txt | sort -u > $W/exe_override_exports.txt
diff -q $W/exe_sub_bodies.txt $W/exe_override_exports.txt >/dev/null; chk $? "the sub_ functions the executable defines are exactly the ones it exports (its overrides)"
# (a COFF symbol has no size, so the thunks are matched by name: each is the import library's stub for one delay import)
$TC/llvm-readobj --coff-imports $EXE | awk '/DelayImport/{f=1} f && /Symbol:/{print $2}' | sort -u > $W/exe_delay_syms.txt
diff -q $W/exe_thunks.txt $W/exe_delay_syms.txt >/dev/null; chk $? "the __imp__sub_ symbols the executable defines are exactly its delay imports from the DLL ($(wc -l < $W/exe_thunks.txt))"
# the ABI stamp: the executable holds the text it expects as a string (game_code.cpp), the DLL the one it says (X86GameCodeStamp, from
# the recompiler and the -DXBE_GAME_CODE_ABI of its compile); read from the two files, they must be the same text
$TC/llvm-strings $EXE | grep '^xbe-game-code ' | sort -u > $W/exe_stamp.txt
$TC/llvm-strings $DLL | grep '^xbe-game-code ' | sort -u > $W/dll_stamp.txt
echo "--- stamp in Buffy.exe: $(cat $W/exe_stamp.txt)"; echo "--- stamp in Buffy.dll: $(cat $W/dll_stamp.txt)"
[ $(wc -l < $W/exe_stamp.txt) -eq 1 ] && [ $(wc -l < $W/dll_stamp.txt) -eq 1 ]; chk $? "one stamp in the executable and one in the DLL"
cmp -s $W/exe_stamp.txt $W/dll_stamp.txt; chk $? "the DLL's stamp is the executable's"
grep -qx X86GameCodeStamp $W/dll_exports.txt; chk $? "the DLL exports X86GameCodeStamp"
grep -q "abi=$(sed -n 's/^token //p' $B/game_code/abi_inputs.txt) " $W/exe_stamp.txt; chk $? "the stamp's ABI token is the one CMake wrote to abi_inputs.txt"
mkdir -p "${KEEP:-/dev/null}" 2>/dev/null; [ -n "$KEEP" ] && cp $W/*.txt "$KEEP"/
rm -rf $W
exit $fail
