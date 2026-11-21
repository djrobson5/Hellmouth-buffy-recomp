#!/bin/bash
# Assembles the Mac release, out/Hellmouth-<version>-macos-arm64/ and out/Hellmouth-<version>-macos-arm64.zip, from the game-code-dylib build
# made against the source-built libraries, and audits it: the release holds no file made from the game (no generated code, no config/game.toml,
# no assets, no Buffy.dylib, no achievement pictures, no icon of the game's), and nothing that names this machine. Everything is built first:
#   export MAC_DEPS=~/Projects/toolchains/mac-deps          (tools/mac/build_toolchain.sh and build_libs.sh make it)
#   sdk/host/build.sh -r . -g generated/game -o build/app-dylib-deps -n Buffy --game-code-dylib
#   tools/package_mac.sh [-v <version>] [-b <that build folder>]
# env: MAC_DEPS             the source-built libraries and compiler: their licences go in the app (required)
#      RECOMPLAUNCHER_GUIDE RecompLauncher's developer guide (recomplauncher-agent.md): its JSON schema checks recomplauncher.json (optional)
# The release folder holds Buffy.app, recomplauncher.json and README.txt, the Windows release's counterpart (tools/package_windows.sh). The app:
#   Contents/MacOS/Buffy               the executable (no game code: the first start builds it from the player's disc)
#   Contents/Frameworks/               libSDL3.0.dylib and libMoltenVK.dylib (the executable finds them from MacOS/ by @executable_path/../Frameworks)
#   Contents/Resources/compiler/       what the first start builds with: clang, ld64.lld, xbe_recomp, buffy_config, the headers, xdk/4432
#                                      (app::AppFolder() is Contents/Resources in an app, so the first start looks for compiler/ there)
#   Contents/Resources/settings.json   what the program lists with --describe-settings, for a launcher; the manifest is checked against it
#   Contents/Resources/licenses/       every third party component's licence, and this project's own
#   Contents/Resources/AppIcon.icns    a plain icon of this project's own (tools/make_app_icon.py); the game's own icon is made from the player's
#                                      disc at the first start, in the data folder, and set on the Dock at run time
# Signed ad hoc, inner code first, then the app; no hardened runtime. Nothing is uploaded; nothing is opened or run but `Buffy --describe-settings
# --no-dialogs` and `--version`, which end before there is a window.
set -euo pipefail
cd "$(dirname "$0")/.."
REPO=$PWD
VERSION=$(sed -n 's/.*\.version = "buffy-port \(.*\)",.*/\1/p' src/common/game_info.cpp)
B=$REPO/build/app-dylib-deps
while getopts v:b: opt; do
    case $opt in v) VERSION=$OPTARG ;; b) B=$OPTARG ;; *) sed -n 2,26p "$0"; exit 2 ;; esac
done
[ -n "$VERSION" ] || { echo "no version: pass -v"; exit 2; }
[ -n "${MAC_DEPS:-}" ] && [ -d "$MAC_DEPS/licenses" ] || { echo "MAC_DEPS is not set to the folder tools/mac/build_libs.sh made (its licenses/ go in the app)"; exit 1; }
MAC_DEPS=$(cd "$MAC_DEPS" && pwd -P)
export MAC_DEPS   # check_mac_deps.sh looks for the licences and the Vulkan headers there
[ -d "$B" ] && B=$(cd "$B" && pwd -P)
NAME=Hellmouth-$VERSION-macos-arm64
P=$REPO/out/$NAME
APP=$P/Buffy.app
C=$APP/Contents
need() { [ -f "$1" ] || { echo "missing $1: build it first (see the top of this script)"; exit 1; }; }
for f in "$B/Buffy" "$B/Frameworks/libSDL3.0.dylib" "$B/Frameworks/libMoltenVK.dylib" "$B/build/Buffy.dylib" "$B/game_code/abi_inputs.txt" \
         "$B/compiler/clang" "$B/compiler/ld64.lld" "$B/compiler/xbe_recomp" "$B/compiler/buffy_config" "$B/compiler/sdk_names.txt" \
         "$B/compiler/overrides.txt" "$B/compiler/include/cpu/x86_ops.h" "$B/compiler/include/cpu/x86_context.h" \
         "$B/compiler/xdk/4432/layout.toml" "$REPO/assets/Default.xbe" LICENSE sdk/LICENSE sdk/ATHELAS-LICENSE sdk/THIRD_PARTY.txt \
         sdk/licenses/LiberationSans-OFL.txt sdk/runtime/third_party/smaa/LICENSE.txt; do need "$f"; done
# only the program and the libraries a release holds are built against the source-built libraries; one from Homebrew would not run elsewhere
"$REPO/tools/mac/check_mac_deps.sh" "$B" > /dev/null || { echo "tools/mac/check_mac_deps.sh fails on $B: build it with MAC_DEPS set"; exit 1; }
[ "$("$B/Buffy" --version --no-dialogs)" = "buffy-port $VERSION" ] || { echo "$B/Buffy is not version $VERSION: rebuild it"; exit 1; }
BUNDLE_ID=org.xboxsdk.$("$B/Buffy" --title-id --no-dialogs)
[[ "$BUNDLE_ID" =~ ^org\.xboxsdk\.[0-9a-f]{8}$ ]] || { echo "$B/Buffy gave no title ID: $BUNDLE_ID"; exit 1; }   # as sdk/host/bundle.sh
# the program and the dylib the build made must be a pair (they would not be, if one was rebuilt without the other)
stamp() { grep -a -o 'xbe-game-code abi=[0-9a-z]* image=[0-9a-f]* symbols=[0-9a-f]*' "$1" | sort -u; }
[ -n "$(stamp "$B/Buffy")" ] && [ "$(stamp "$B/Buffy")" = "$(stamp "$B/build/Buffy.dylib")" ] ||
    { echo "Buffy and build/Buffy.dylib are not a pair: rebuild with --game-code-dylib"; exit 1; }
# as old a macOS as everything in it allows
MINOS=$(for f in "$B/Buffy" "$B/Frameworks/"*.dylib "$B/compiler/clang" "$B/compiler/ld64.lld" "$B/compiler/xbe_recomp" "$B/compiler/buffy_config"; do
    vtool -show-build "$f" | awk '$1 == "minos" { print $2 }'; done | sort -V | tail -1)
[ "$MINOS" = 13.0 ] || { echo "the newest macOS something in it needs is $MINOS, not 13.0"; exit 1; }

rm -rf "$P" "$P.zip"
T=$REPO/build/package-mac-tmp
rm -rf "$T"; mkdir -p "$T"
trap 'rm -rf "$T"' EXIT
mkdir -p "$C/MacOS" "$C/Frameworks" "$C/Resources/compiler/include/cpu" "$C/Resources/compiler/include/freestanding" \
         "$C/Resources/compiler/xdk/4432" "$C/Resources/licenses"
cp "$B/Buffy" "$C/MacOS/Buffy"
cp "$B/Frameworks/libSDL3.0.dylib" "$B/Frameworks/libMoltenVK.dylib" "$C/Frameworks/"
cp "$B/compiler/clang" "$B/compiler/ld64.lld" "$B/compiler/xbe_recomp" "$B/compiler/buffy_config" "$B/compiler/sdk_names.txt" \
   "$B/compiler/overrides.txt" "$C/Resources/compiler/"
cp "$B/compiler/include/cpu/x86_ops.h" "$B/compiler/include/cpu/x86_context.h" "$C/Resources/compiler/include/cpu/"
cp "$REPO"/sdk/runtime/cpu/freestanding/* "$C/Resources/compiler/include/freestanding/"
cp "$B/compiler/xdk/4432/layout.toml" "$C/Resources/compiler/xdk/4432/"
chmod u+w "$C/MacOS/Buffy" "$C/Frameworks/"*.dylib "$C/Resources/compiler/"*
# the settings a launcher can show: what this program acts on. Its output is written outside the app and copied in: with the output going to a
# file inside a bundle made a moment ago, the program's write came out empty here (macOS protects a new app's files from other writers).
"$B/Buffy" --describe-settings --no-dialogs > "$T/settings.json"
plutil -convert json -o /dev/null "$T/settings.json"
cp "$T/settings.json" "$C/Resources/settings.json"
python3 -I tools/make_app_icon.py "$C/Resources/AppIcon.icns" > /dev/null
cat > "$C/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key><string>en</string>
    <key>CFBundleDisplayName</key><string>Buffy the Vampire Slayer</string>
    <key>CFBundleExecutable</key><string>Buffy</string>
    <key>CFBundleIconFile</key><string>AppIcon</string>
    <key>CFBundleIdentifier</key><string>$BUNDLE_ID</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>CFBundleName</key><string>Buffy</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$VERSION</string>
    <key>CFBundleVersion</key><string>$VERSION</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.action-games</string>
    <key>LSMinimumSystemVersion</key><string>$MINOS</string>
    <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
EOF
plutil -lint -s "$C/Info.plist"

# Licences: derived from what is linked or shipped. tools/mac/build_libs.sh and build_toolchain.sh left each of their components' licences
# and a SOURCE.txt (version, commit) in $MAC_DEPS/licenses; Zydis and Zycore (inside xbe_recomp) come from the sources the tools were built
# from; the rest (Dear ImGui, toml++, stb, Liberation Sans, SMAA) are in the SDK's THIRD_PARTY.txt, whose Windows only parts (the FSR and DLSS headers,
# llvm-mingw) are left out here. No file of the SDK's list is needed for a component that is not linked on the Mac.
L=$C/Resources/licenses
for c in sdl3 moltenvk glslang spirv-tools spirv-headers vulkan-headers llvm; do
    [ -f "$MAC_DEPS/licenses/$c/SOURCE.txt" ] || { echo "$MAC_DEPS/licenses/$c is missing (tools/mac/build_libs.sh makes it)"; exit 1; }
    cp -R "$MAC_DEPS/licenses/$c" "$L/$c"
done
ZYDIS=; for d in "$B/game_code/tools/_deps/zydis-src" "$REPO/build/tools/_deps/zydis-src"; do [ -f "$d/LICENSE" ] && ZYDIS=$d && break; done
[ -n "$ZYDIS" ] && [ -f "$ZYDIS/dependencies/zycore/LICENSE" ] || { echo "no Zydis sources (their licences go in the app): tools/regen.sh or the build of the tools fetches them"; exit 1; }
mkdir "$L/zydis"
cp "$ZYDIS/LICENSE" "$L/zydis/Zydis-LICENSE.txt"
cp "$ZYDIS/dependencies/zycore/LICENSE" "$L/zydis/Zycore-LICENSE.txt"
{
    echo "name: zydis (and zycore)"
    echo "version: $(git -C "$ZYDIS" describe --tags) (zycore $(git -C "$ZYDIS/dependencies/zycore" describe --tags))"
    echo "source: https://github.com/zyantific/zydis"
    echo "commit: $(git -C "$ZYDIS" rev-parse HEAD) (zycore $(git -C "$ZYDIS/dependencies/zycore" rev-parse HEAD))"
    echo "used by: compiler/xbe_recomp (the recompiler tool of the first start)"
} > "$L/zydis/SOURCE.txt"
cp LICENSE "$L/Hellmouth-LICENSE.txt"
cp sdk/LICENSE "$L/XboxXBERecomp-LICENSE.txt"
cp sdk/ATHELAS-LICENSE "$L/Athelas-LICENSE.txt"
cp sdk/licenses/LiberationSans-OFL.txt "$L/LiberationSans-OFL.txt"
cp sdk/runtime/third_party/smaa/LICENSE.txt "$L/SMAA-LICENSE.txt"
{
    # the SDK's list up to the parts that are for Windows only (the FSR and DLSS headers, the Windows release's programs)
    sed '/^AMD FidelityFX \/ FSR SDK/,$d' sdk/THIRD_PARTY.txt
    echo "Libraries and programs of the Mac app, each with its licence and a SOURCE.txt (version, commit) in licenses/<folder>/"
    echo
    for c in sdl3:SDL3 moltenvk:MoltenVK glslang:glslang spirv-tools:SPIRV-Tools spirv-headers:SPIRV-Headers vulkan-headers:Vulkan-Headers llvm:LLVM zydis:Zydis; do
        echo "  ${c#*:} $(sed -n 's/^version: //p' "$L/${c%%:*}/SOURCE.txt" | head -1)  (licenses/${c%%:*}/)"
    done
    echo
    echo "SDL3 and MoltenVK are the libraries in Contents/Frameworks. MoltenVK carries SPIRV-Cross, SPIRV-Tools, Volk, Vulkan-Headers, Vulkan-Tools and cereal;"
    echo "their licences are in licenses/moltenvk/. glslang and SPIRV-Tools are linked into Contents/MacOS/Buffy, and the Vulkan and SPIR-V headers are what"
    echo "it is compiled against. LLVM (clang, lld) is Contents/Resources/compiler/clang and ld64.lld, built from source with AArch64 only and no library of"
    echo "its own. Zydis and Zycore are linked into compiler/xbe_recomp. Apache License v2.0 with LLVM Exceptions applies to LLVM."
    echo "Dear ImGui, toml++, stb, Liberation Sans and SMAA are above."
    echo "The Xbox SDK this project is based on (XboxXBERecomp by TGP482, and Athelas by Shagg), MIT: XboxXBERecomp-LICENSE.txt, Athelas-LICENSE.txt."
} > "$L/THIRD_PARTY.txt"
# the player's README, and RecompLauncher's file: how it sets the game up from the player's disc, builds it (--prepare), starts it and shows its
# settings. The release's version goes in as the setup step's version, so that a player who set the game up with an older release has it built again.
sed "s/@VERSION@/$VERSION/" tools/mac_player_readme.txt > "$P/README.txt"
sed "s/@VERSION@/$VERSION/" tools/recomplauncher-mac.json.in > "$P/recomplauncher.json"
echo "== recomplauncher.json:"
if [ -n "${RECOMPLAUNCHER_GUIDE:-}" ]; then
    python3 -I tools/check_recomplauncher.py "$P/recomplauncher.json" --guide "$RECOMPLAUNCHER_GUIDE" --version "$VERSION" --self-test --describe "$C/Resources/settings.json"
else
    echo "  RECOMPLAUNCHER_GUIDE is not set: the schema is NOT checked, only the file against this game (set it to RecompLauncher's recomplauncher-agent.md)"
    python3 -I tools/check_recomplauncher.py "$P/recomplauncher.json" --no-schema --version "$VERSION" --describe "$C/Resources/settings.json"
fi

# Signing: every change to the files is done. Extended attributes off (a file made here may carry some), then inner code first, then the app.
# Ad hoc, and no hardened runtime: the dylib the first start builds is ad hoc too, and has to load into this process. (Notarization, which
# needs a developer account, would want -o runtime, the entitlement com.apple.security.cs.disable-library-validation, and an absolute path in
# the executable's dlopen of the game's dylib; not done here.)
echo "== signing"
xattr -cr "$P"
for f in "$C/Frameworks/"*.dylib "$C/Resources/compiler/clang" "$C/Resources/compiler/ld64.lld" "$C/Resources/compiler/xbe_recomp" "$C/Resources/compiler/buffy_config"; do
    codesign --force -s - "$f" 2> /dev/null
done
codesign --force -s - "$APP" 2> /dev/null
codesign --verify --deep --strict --verbose=2 "$APP" 2>&1 | sed 's/^/  /'

AUDIT=(python3 -I tools/audit_mac_package.py --dylib "$B/build/Buffy.dylib" --xbe "$REPO/assets/Default.xbe" --generated "$REPO/generated/game"
       --abi-inputs "$B/game_code/abi_inputs.txt" --version "$VERSION")
"${AUDIT[@]}" "$P"
# the zip: one folder in it, links and permissions kept, no Mac metadata (no ._ files or __MACOSX: the xattrs are gone and ditto is told not to add any)
ditto -c -k --keepParent --norsrc --noextattr --noqtn "$P" "$P.zip"
# the zip's own contents are audited too: extracted, and compared with the folder's list
X=$REPO/build/package-mac-check
rm -rf "$X"; mkdir -p "$X"
ditto -x -k --noqtn "$P.zip" "$X"
rc=0
"${AUDIT[@]}" "$X/$NAME" --zip "$P.zip" > "$X/audit.log" 2>&1 || rc=$?
tail -8 "$X/audit.log"
[ "$rc" = 0 ] || { echo "the zip's contents fail the audit (the whole list: $X/audit.log)"; exit 1; }
rm -rf "$X"
echo
ls -l "$P.zip"
du -sh "$P" "$APP"
