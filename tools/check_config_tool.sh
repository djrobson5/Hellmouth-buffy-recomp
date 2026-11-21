#!/bin/bash
# Golden test of tools/buffy_config (C++, what the player's PC runs) against tools/make_config.py (what the Mac flow runs): built
# natively here, run on the local assets (assets/final.map and assets/Default.xbe, from the disc), both outputs written under build/
# (the config is game derived, so no expected file is kept), and compared byte for byte. The list of the SDK's names the tool binds
# is compared too: the C++ scan against make_config.py's sdk_names.
#   tools/check_config_tool.sh
cd "$(dirname "$0")/.."
T=build/config-test
fail=0
chk() { if [ "$1" = 0 ]; then echo "PASS  $2"; else echo "FAIL  $2"; fail=1; fi; }
[ -d build/tools ] || cmake -S tools -B build/tools -DCMAKE_POLICY_VERSION_MINIMUM=3.5 > /dev/null
cmake --build build/tools -j"$(sysctl -n hw.ncpu)" --target buffy_config buffy_config_names > /dev/null; chk $? "buffy_config builds, and writes sdk_names.txt from the SDK's sources"
rm -rf $T; mkdir -p $T
python3 -c "
import sys; sys.path.insert(0, 'tools')
import make_config
print('\n'.join(sorted(make_config.sdk_names('sdk'))))" > $T/py_names.txt
cmp -s $T/py_names.txt build/tools/sdk_names.txt; chk $? "the names found in the sources: $(wc -l < build/tools/sdk_names.txt | tr -d ' ') names, the same as make_config.py's sdk_names"
( time python3 tools/make_config.py --map assets/final.map --xbe assets/Default.xbe --out $T/py.toml ) 2>&1 | grep -E "functions|real" | tr '\n' ' '; echo
( time build/tools/buffy_config --map assets/final.map --xbe assets/Default.xbe --out $T/cpp.toml ) 2>&1 | grep -E "functions|real" | tr '\n' ' '; echo
cmp $T/py.toml $T/cpp.toml; chk $? "game.toml from buffy_config is byte for byte the one make_config.py writes ($(wc -c < $T/cpp.toml | tr -d ' ') bytes, $(wc -l < $T/cpp.toml | tr -d ' ') lines)"
cmp -s $T/cpp.toml config/game.toml; chk $? "and the config/game.toml in this checkout (what tools/regen.sh wrote)"
# the same through the names file beside the program, as the player's PC runs it (no --names)
mkdir -p "$T/folder é"; cp build/tools/buffy_config build/tools/sdk_names.txt "$T/folder é/"
cp assets/final.map assets/Default.xbe $T/
( cd $T && "folder é/buffy_config" --map final.map --xbe Default.xbe --out cpp2.toml > /dev/null ); cmp -s $T/cpp.toml $T/cpp2.toml; chk $? "the same from a folder with an accented name, with the names file found beside the program"
rm -f $T/final.map $T/Default.xbe
exit $fail
