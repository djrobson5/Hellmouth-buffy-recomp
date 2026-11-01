#!/bin/bash
# bundle.sh: a built game (sdk/host/build.sh) as a Mac app, with every Homebrew library it needs inside it
#   bundle.sh <built executable> <out>.app
# Its icon is assets/<name>.icns, if there is one: made from the player's own disc, so it isn't in the repository.
# The executable keeps its name, which the game's restart looks for, and isn't changed; the app gets its own copy. Signed
# ad hoc, which runs on this Mac: another Mac's Gatekeeper would refuse it. The app holds the recompiled game, so it's for
# the disc's owner and not for passing around.
set -e
EXE=$1; APP=$2
if [ ! -f "$EXE" ] || [[ "$APP" != *.app ]]; then sed -n 2,6p "$0"; exit 2; fi
BREW=$(brew --prefix)
NAME=$(basename "$EXE")
VERSION=$("$EXE" --version | awk '{print $NF}')
BUNDLE_ID=org.xboxsdk.$("$EXE" --title-id)  # the same for every build of a game, so a launcher can find it
[[ "$BUNDLE_ID" =~ ^org\.xboxsdk\.[0-9a-f]{8}$ ]] || { echo "$EXE gave no title ID: $BUNDLE_ID" >&2; exit 1; }

TMP=$APP.tmp
rm -rf "$TMP"
mkdir -p "$TMP/Contents/MacOS" "$TMP/Contents/Frameworks" "$TMP/Contents/Resources"
"$EXE" --describe-settings > "$TMP/Contents/Resources/settings.json"  # the settings, for a launcher to show
plutil -convert json -o /dev/null "$TMP/Contents/Resources/settings.json"
ICON=$(dirname "$0")/../../assets/$NAME.icns
ICON_KEY=
if [ -f "$ICON" ]; then
    cp "$ICON" "$TMP/Contents/Resources/$NAME.icns"
    ICON_KEY="<key>CFBundleIconFile</key><string>$NAME</string>"
else
    echo "no $ICON: the app will have the system's blank icon (tools/make_icon.py makes it)" >&2
fi
# <achievement id>.png for each: the game's note reads them from here. Made from the player's disc as well.
ACHIEVEMENT_ICONS=$(dirname "$0")/../../assets/achievements
if [ -d "$ACHIEVEMENT_ICONS" ]; then
    mkdir "$TMP/Contents/Resources/achievements"
    cp "$ACHIEVEMENT_ICONS"/*.png "$TMP/Contents/Resources/achievements/"
else
    echo "no $ACHIEVEMENT_ICONS: achievement notes will have no picture" >&2
fi
FW=$TMP/Contents/Frameworks
MAIN=$TMP/Contents/MacOS/$NAME
cp "$EXE" "$MAIN"
chmod u+w "$MAIN"

rpaths() { otool -l "$1" | awk '/cmd LC_RPATH/ { getline; getline; print $2 }'; }
# without its warning at every change that the signature is now invalid: everything is signed again below
install_name_tool() { command install_name_tool "$@" 2> >(grep -v 'invalidate the code signature' >&2); }

# one file's references to Homebrew become @rpath ones, the libraries they name copied in first; its own search paths go,
# or this Mac would quietly go on using Homebrew's copies
localize() {
    local file=$1 dep base
    for dep in $(otool -L "$file" | tail -n +2 | awk '{print $1}'); do
        case $dep in "$BREW"/*) ;; *) continue ;; esac
        base=$(basename "$dep")
        [ "$base" = "$(basename "$file")" ] && continue  # a library's own name
        add_library "$dep"
        install_name_tool -change "$dep" "@rpath/$base" "$file"
    done
    for dep in $(rpaths "$file"); do install_name_tool -delete_rpath "$dep" "$file"; done
}

add_library() {
    local out; out=$FW/$(basename "$1")
    [ -e "$out" ] && return 0
    cp -L "$1" "$out"
    chmod u+w "$out"
    install_name_tool -id "@rpath/$(basename "$1")" "$out"
    localize "$out"
    install_name_tool -add_rpath @loader_path "$out"
}

localize "$MAIN"
add_library "$BREW/lib/libMoltenVK.dylib"  # opened by name when the renderer starts, so no reference shows it
install_name_tool -add_rpath @executable_path/../Frameworks "$MAIN"

# nothing outside the app but the system's
for f in "$MAIN" "$FW"/*.dylib; do
    left=$(otool -L "$f" | tail -n +2 | awk '{print $1}' | grep -Ev '^(/usr/lib/|/System/|@rpath/)' || true)
    left="$left$(rpaths "$f" | grep -Ev '^@(loader|executable)_path' || true)"
    if [ -n "$left" ]; then echo "$f still points outside the app: $left" >&2; exit 1; fi
done

# as old a macOS as everything in it allows; Homebrew's libraries are built for the macOS they were installed on
MINOS=$(for f in "$MAIN" "$FW"/*.dylib; do vtool -show-build "$f" | awk '$1 == "minos" { print $2 }'; done | sort -V | tail -1)

cat > "$TMP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key><string>en</string>
    <key>CFBundleDisplayName</key><string>Buffy the Vampire Slayer</string>
    <key>CFBundleExecutable</key><string>$NAME</string>
    $ICON_KEY
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
plutil -lint -s "$TMP/Contents/Info.plist"

# install_name_tool broke their signatures, and Apple Silicon won't run unsigned code: the libraries first, then the app
xattr -cr "$TMP"
for f in "$FW"/*.dylib; do codesign --force -s - "$f" 2>/dev/null; done
codesign --force -s - "$TMP" 2>/dev/null
codesign --verify --deep --strict "$TMP"

rm -rf "$APP"
mv "$TMP" "$APP"
echo "$APP: macOS $MINOS or later, $(ls "$APP/Contents/Frameworks" | wc -l | tr -d ' ') libraries, $(du -sh "$APP" | cut -f1)"
