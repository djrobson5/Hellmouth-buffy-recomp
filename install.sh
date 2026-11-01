#!/bin/bash
# Builds Hellmouth on this Mac from your own disc image, and installs the app. No game files come with this: the game's code
# is recompiled here, from your disc, and stays on this Mac.
#
#   bash install.sh [--to <folder>] [--yes] [<image.iso>]
#
# <image.iso>     your disc image; without it, a file panel asks for it
# --to <folder>   where the app goes: by default /Applications, or ~/Applications if that can't be written to
# --yes           install missing Homebrew packages without asking
set -u

XBE_SHA1=2583fb651fafc10d47e34daae91fab9a8b9e1494  # Default.xbe of the USA release, the build this port's hooks are made for
PACKAGES="cmake pkgconf ffmpeg sdl3 molten-vk vulkan-headers glslang"

ISO=; DEST=; YES=
while [ $# -gt 0 ]; do
    case $1 in
        --to) DEST=${2:-}; shift 2 || { echo "--to needs a folder"; exit 2; } ;;
        --yes) YES=1; shift ;;
        -h|--help) sed -n 5,9p "$0" | cut -c3-; exit 0 ;;
        -*) echo "unknown option: $1"; exit 2 ;;
        *) ISO=$1; shift ;;
    esac
done

fail() { echo; echo "Stopped: $*" >&2; exit 1; }
step() { echo; echo "==> $*"; }

# paths given are from where this was run, not from this script's folder
case $ISO in ''|/*) ;; *) ISO=$PWD/$ISO ;; esac
case $DEST in ''|/*) ;; *) DEST=$PWD/$DEST ;; esac

cd "$(dirname "$0")" || exit 1
ROOT=$(pwd)
LOG=$ROOT/build/install.log
mkdir -p build || fail "can't write to $ROOT"
: > "$LOG" || fail "can't write to $LOG"
# run <what> <command...>: the output goes to the log, and its end is shown if it fails
run() {
    local what=$1; shift
    if ! "$@" >> "$LOG" 2>&1; then
        tail -20 "$LOG" >&2
        fail "$what failed. The whole log is at $LOG"
    fi
}

step "Checking this Mac"
[ "$(uname -s)" = Darwin ] || fail "this needs macOS."
[ "$(uname -m)" = arm64 ] || fail "this needs an Apple Silicon Mac, in a Terminal that isn't running under Rosetta."
if pgrep -x Buffy > /dev/null; then fail "the game is running. Quit it and run this again."; fi
if ! xcode-select -p > /dev/null 2>&1; then
    xcode-select --install 2> /dev/null
    fail "this needs the Xcode Command Line Tools. Finish the install macOS just offered, then run this again."
fi
# a download's quarantine flag would stop the scripts below
xattr -dr com.apple.quarantine "$ROOT" 2> /dev/null

if ! command -v brew > /dev/null 2>&1; then
    [ -x /opt/homebrew/bin/brew ] || fail "this needs Homebrew. Install it from https://brew.sh, then run this again."
    eval "$(/opt/homebrew/bin/brew shellenv)"
fi

# only what's missing: installing over a package can upgrade it, and that can break other things built against it
MISSING=
for p in $PACKAGES; do
    [ -n "$(brew list --versions "$p" 2> /dev/null)" ] || MISSING="$MISSING $p"
done
if [ -n "$MISSING" ]; then
    step "Homebrew packages to install:$MISSING"
    if [ -z "$YES" ]; then
        printf 'Install them? [y/N] '
        read -r answer
        case $answer in [Yy]*) ;; *) fail "these packages are needed:$MISSING" ;; esac
    fi
    # shellcheck disable=SC2086
    brew install $MISSING || fail "Homebrew couldn't install:$MISSING"
else
    echo "Everything needed is installed."
fi

if [ ! -f assets/Default.xbe ] || [ ! -f assets/final.map ]; then
    if [ -z "$ISO" ]; then
        step "Choose your disc image"
        ISO=$(osascript -e 'POSIX path of (choose file with prompt "Choose your Buffy the Vampire Slayer disc image (.iso)")' 2> /dev/null) \
            || fail "no disc image chosen."
    fi
    [ -f "$ISO" ] || fail "there's no file at $ISO"
    step "Reading your disc"
    python3 tools/extract_assets.py "$ISO" >> "$LOG" 2>&1 \
        || fail "that doesn't look like this game's disc: Default.xbe and final.map couldn't be read from it. The log is at $LOG"
fi

# the hooks in src/game are at this build's addresses, so another release wouldn't just look wrong: it would misbehave
sha=$(shasum -a 1 assets/Default.xbe | cut -d' ' -f1)
if [ "$sha" != "$XBE_SHA1" ]; then
    rm -rf assets  # or the next run, with the right disc, would use these
    fail "this disc's Default.xbe isn't the USA release this port is made for.
  its SHA-1:  $sha
  expected:   $XBE_SHA1"
fi
echo "This is the USA release."

step "Recompiling the game, which can take a few minutes"
if [ -n "$ISO" ]; then run "Recompiling" tools/regen.sh "$ISO"; else run "Recompiling" tools/regen.sh; fi
if [ ! -f assets/Buffy.icns ] || [ ! -d assets/achievements ]; then
    echo "(The Dock icon or the achievement pictures are missing: give this script the disc image to make them.)"
fi

step "Building the app, which can take a few minutes"
run "Building" sdk/host/build.sh -r . -g generated/game -o build/installer -n Buffy
run "Making the app" sdk/host/bundle.sh build/installer/Buffy build/installer/Buffy.app

if [ -z "$DEST" ]; then
    if [ -w /Applications ]; then DEST=/Applications; else DEST=$HOME/Applications; fi
fi
step "Installing to $DEST"
mkdir -p "$DEST" || fail "can't write to $DEST"
rm -rf "$DEST/Buffy.app.new"
ditto build/installer/Buffy.app "$DEST/Buffy.app.new" || fail "can't write to $DEST"
rm -rf "$DEST/Buffy.app"
mv "$DEST/Buffy.app.new" "$DEST/Buffy.app" || fail "can't write to $DEST"
codesign --verify --deep --strict "$DEST/Buffy.app" || fail "the installed app's signature doesn't check out."

echo
echo "Done: $DEST/Buffy.app"
echo "Open it, and choose your disc image when it asks. Your saves and settings are kept in"
echo "~/Library/Application Support/Buffy, and installing again leaves them alone."
echo "The app contains the recompiled game, so it's for you and not for passing around."
echo "This folder holds files from your disc as well. You can delete it now, unless you want to change something and build again."
