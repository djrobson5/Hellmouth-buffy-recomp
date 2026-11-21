#!/bin/bash
# Makes a Hellmouth release: checks, builds and audits what is going out, and creates the GitHub release with both zips.
#   tools/release.sh [--dry-run] [--publish] <notes.md>
#
#   <notes.md>   the release notes, written by hand. @SHA256_WINDOWS@ and @SHA256_MAC@ in it are replaced by the two zips' SHA-256.
#                The notes must name both assets (Hellmouth-<version>-windows-x64.zip and Hellmouth-<version>-macos-arm64.zip) and
#                no asset of another version (a name copied forward from the last release).
#   --publish    create the release published and as the latest one. The default is a draft, which only the repository's owners see.
#   --dry-run    do everything up to the release, print the command that would create it, and stop. Nothing is created.
#   -h           this text
#
# The version is the one in src/common/game_info.cpp; the release is v<version>, made from HEAD. In order, stopping at the first failure:
#   1. preconditions: a clean working tree; HEAD is origin/main (this script never pushes); v<version> is not a tag, here or on origin,
#      and no release has its name; gh is signed in; LLVM_MINGW (llvm-mingw for this machine) and RECOMPLAUNCHER_GUIDE (RecompLauncher's
#      developer guide, recomplauncher-agent.md) are set and exist; the notes file exists
#   2. gates, each logged under out/release-<version>/: the Windows build, the first start's tools, tools/check_game_code_dll.sh,
#      tools/check_config_tool.sh, tools/package_windows.sh (which makes the Windows zip), tools/sim_first_start.sh, the Mac build
#   3. the Mac zip, git archive of HEAD, and a check that it holds no game data and nothing naming this machine
#   4. the notes, filled in, in out/release-<version>/notes.md
#   5. gh release create (the target is HEAD, from git rev-parse)
#   6. checks after: both assets downloaded back and compared by SHA-256; a published release: the public download URLs answer and
#      releases/latest is the new tag; a draft: its page answers 404 to someone signed out
#
# Tests of this script only (each is refused without --dry-run, and none changes what the checks of a real release do):
#   HELLMOUTH_RELEASE_SKIP_CLEAN_CHECK=1   do not stop for a working tree that is not clean (the files are still listed)
#   HELLMOUTH_RELEASE_SKIP_TAG_CHECK=1     do not stop for a tag or a release that exists (so that an old version can be played through)
#   --skip-gates                           skip step 2; the Windows zip of the version must already be in out/
# Needs: git, gh, curl, python3, zip, unzip, and what the gates need (see tools/package_windows.sh and tools/sim_first_start.sh).
set -euo pipefail

DRY=0 PUBLISH=0 SKIP_GATES=0 NOTES_IN=
die() { echo "release: $*" >&2; exit 1; }

parse_args() {
    local a
    for a in "$@"; do
        case $a in
            -h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 0 ;;
            --dry-run) DRY=1 ;;
            --publish) PUBLISH=1 ;;
            --skip-gates) SKIP_GATES=1 ;;
            -*) echo "release: unknown option $a (-h for help)" >&2; exit 2 ;;
            *) [ -z "$NOTES_IN" ] || { echo "release: one notes file only (-h for help)" >&2; exit 2; }; NOTES_IN=$a ;;
        esac
    done
    [ -n "$NOTES_IN" ] || { echo "usage: tools/release.sh [--dry-run] [--publish] <notes.md>   (-h for help)" >&2; exit 2; }
    if [ "$DRY" = 0 ]; then
        [ "$SKIP_GATES" = 0 ] || die "--skip-gates only goes with --dry-run"
        [ -z "${HELLMOUTH_RELEASE_SKIP_TAG_CHECK:-}" ] && [ -z "${HELLMOUTH_RELEASE_SKIP_CLEAN_CHECK:-}" ] ||
            die "HELLMOUTH_RELEASE_SKIP_TAG_CHECK and HELLMOUTH_RELEASE_SKIP_CLEAN_CHECK only go with --dry-run"
    fi
}

# The files in a git archive of HEAD that have the extension of a game file but are the repository's own: the screenshots of the
# README and the icon of Buffy.exe (resources/exe_icon.ico). This is the list of every such file tracked in 0.2.1; a new one fails
# the release until it is looked at and added here.
ALLOWED_MAC_FILES=(
    docs/screenshots/01-main-menu.png
    docs/screenshots/02-alt-giles-and-buffy.png
    docs/screenshots/02-giles-cutscene.png
    docs/screenshots/03-player-takes-over.png
    resources/exe_icon.ico
)
# Extensions of what is made from, or holds, the game: none may be in the Mac zip but the files above.
GAME_EXTENSIONS='xbe|iso|xiso|dll|map|xpr|mtx|png|ico|icns|wav|xwb|xmv|bik'

# temporary folders, removed when the script ends, however it ends
TEMPS=()
cleanup() { [ "${#TEMPS[@]}" = 0 ] || rm -rf "${TEMPS[@]}"; }
trap cleanup EXIT

sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }

# gate <name> <log> <command>: runs the command (bash -e) with its output in <log>; returns its exit status
gate_run() {
    local rc=0
    bash -ec "$3" > "$2" 2>&1 || rc=$?
    return $rc
}
gate_fail() { # <name> <log> [why]
    echo "FAIL $1${3:+: $3}"
    echo "--- last lines of $2:"; tail -n 15 "$2" | sed 's/^/    /'
    die "gate \"$1\" failed: stopping. The whole log is $2"
}
# a gate with a command only: OK <label> or stop
gate_simple() { # <name> <ok label> <log> <command>
    gate_run "$1" "$3" "$4" || gate_fail "$1" "$3"
    echo "OK $2"
}
# a gate that prints PASS and FAIL lines, and exits non-zero on a FAIL: "<prefix>: N PASS, M FAIL"
gate_counts() { # <name> <prefix> <log> <command>
    local rc=0 pass fail
    gate_run "$1" "$3" "$4" || rc=$?
    pass=$(grep -c '^PASS' "$3" || true); fail=$(grep -c '^FAIL' "$3" || true)
    echo "$2: $pass PASS, $fail FAIL"
    [ "$rc" = 0 ] && [ "$fail" = 0 ] && [ "$pass" -gt 0 ] || gate_fail "$1" "$3" "exit status $rc, $pass PASS, $fail FAIL"
}

check_preconditions() {
    # 1a. a clean tree: nothing staged, changed or untracked (ignored files do not count)
    local dirty
    dirty=$(git status --porcelain)
    if [ -n "$dirty" ]; then
        if [ -n "${HELLMOUTH_RELEASE_SKIP_CLEAN_CHECK:-}" ]; then
            echo "note: the working tree is not clean (HELLMOUTH_RELEASE_SKIP_CLEAN_CHECK): $(echo "$dirty" | wc -l | tr -d ' ') path(s)"
        else
            echo "$dirty" | head -20 | sed 's/^/    /' >&2
            die "the working tree is not clean (see above): commit, stash or remove these first"
        fi
    fi
    # 1b. the version
    VERSION=$(sed -n 's/.*\.version = "buffy-port \(.*\)",.*/\1/p' src/common/game_info.cpp)
    [[ $VERSION =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "no version of the form X.Y.Z in src/common/game_info.cpp (found \"$VERSION\")"
    TAG=v$VERSION
    WNAME=Hellmouth-$VERSION-windows-x64.zip
    MNAME=Hellmouth-$VERSION-macos-arm64.zip
    TITLE="Hellmouth $VERSION (experimental)"
    # 1c. HEAD is origin/main
    git fetch --quiet --no-tags origin main || die "git fetch origin main failed"
    HEAD_SHA=$(git rev-parse HEAD)
    local main_sha
    main_sha=$(git rev-parse --verify --quiet refs/remotes/origin/main) || die "origin/main not found"
    [ "$HEAD_SHA" = "$main_sha" ] ||
        die "HEAD (${HEAD_SHA:0:7}) is not origin/main (${main_sha:0:7}): push main first (this script never pushes)"
    # 1d. gh, then the tag and the release names
    command -v gh > /dev/null || die "gh is not installed"
    gh auth status > /dev/null 2>&1 || die "gh is not signed in: run gh auth login"
    SLUG=$(gh repo view --json nameWithOwner --jq .nameWithOwner) || die "cannot read the repository's name with gh repo view"
    if [ -z "${HELLMOUTH_RELEASE_SKIP_TAG_CHECK:-}" ]; then
        [ -z "$(git ls-remote --tags origin "refs/tags/$TAG")" ] || die "tag $TAG exists on origin"
        if git rev-parse --verify --quiet "refs/tags/$TAG" > /dev/null; then die "tag $TAG exists locally"; fi
        # a draft has no tag yet: look through the releases too
        local clash
        clash=$(gh api "repos/$SLUG/releases" --paginate --jq ".[] | select(.tag_name == \"$TAG\" or .name == \"$TITLE\") | .html_url") ||
            die "cannot list the releases with gh api"
        [ -z "$clash" ] || die "a release for $TAG exists ($(echo "$clash" | head -1))"
    else
        echo "note: tag and release checks skipped (HELLMOUTH_RELEASE_SKIP_TAG_CHECK)"
    fi
    # 1e. the environment and the notes
    [ -n "${LLVM_MINGW:-}" ] || die "LLVM_MINGW is not set: llvm-mingw for this machine"
    [ -d "$LLVM_MINGW" ] || die "LLVM_MINGW is not a folder: $LLVM_MINGW"
    [ -n "${RECOMPLAUNCHER_GUIDE:-}" ] || die "RECOMPLAUNCHER_GUIDE is not set: RecompLauncher's developer guide (recomplauncher-agent.md)"
    [ -f "$RECOMPLAUNCHER_GUIDE" ] || die "RECOMPLAUNCHER_GUIDE is not a file: $RECOMPLAUNCHER_GUIDE"
    export LLVM_MINGW RECOMPLAUNCHER_GUIDE
    [ -f "$NOTES_IN" ] || die "the notes file does not exist: $NOTES_IN"
    echo "OK preconditions: $TAG from ${HEAD_SHA:0:7} (origin/main) in $SLUG"
}

run_gates() {
    local L=$1
    gate_simple "win build" "win build" "$L/win-build.log" \
        'cmake --preset win-x64-mingw && cmake --build out/build/win-x64-mingw && cmake --build out/build/win-x64-mingw --target game_code_dll'
    gate_simple "tools build" "tools build" "$L/tools-build.log" \
        '( cd tools && cmake --preset win-x64-mingw && cmake --build ../out/build/win-x64-mingw-tools )'
    gate_counts "check_game_code_dll" "dll" "$L/check-dll.log" 'bash tools/check_game_code_dll.sh'
    gate_counts "check_config_tool" "cfg" "$L/check-config.log" 'bash tools/check_config_tool.sh'
    # package: the exit status, and what it says about the file RecompLauncher reads and the audits of the folder and of the zip
    local rc=0 rl au
    gate_run "package_windows" "$L/package.log" 'bash tools/package_windows.sh' || rc=$?
    rl=$(grep -c 'RECOMPLAUNCHER.JSON PASSED' "$L/package.log" || true)
    au=$(grep -c 'AUDIT PASSED' "$L/package.log" || true)
    echo "package: $rl RECOMPLAUNCHER.JSON PASSED, $au AUDIT PASSED"
    [ "$rc" = 0 ] && [ "$rl" -ge 1 ] && [ "$au" = 2 ] && ! grep -qi 'not checked' "$L/package.log" && ! grep -q 'AUDIT FAILED' "$L/package.log" &&
        [ -f "$WZIP" ] || gate_fail "package_windows" "$L/package.log" "exit status $rc; needs RECOMPLAUNCHER.JSON PASSED, 2 AUDIT PASSED, no \"NOT checked\", and $WZIP"
    # sim_first_start.sh plays the last of ls out/Hellmouth-*-windows-x64.zip: ours must be that one (0.2.9 sorts after 0.2.10)
    local played
    played=$(ls out/Hellmouth-*-windows-x64.zip | tail -1)
    [ "$played" = "out/$WNAME" ] || die "tools/sim_first_start.sh would play $played, not out/$WNAME: move the other zips out of out/ first"
    gate_counts "sim_first_start" "sim" "$L/sim.log" 'bash tools/sim_first_start.sh'
    gate_simple "mac build" "mac build" "$L/mac-build.log" 'sdk/host/build.sh -r . -g generated/game -o build/app -n Buffy'
    [ -x build/app/Buffy ] || gate_fail "mac build" "$L/mac-build.log" "build/app/Buffy was not made"
}

make_mac_zip() {
    git archive --format=zip --prefix="Hellmouth-$VERSION/" -o "$MZIP" HEAD
    local names bad="" f ext allowed
    names=$(unzip -Z1 "$MZIP")
    # files with the extension of a game file: only the repository's own, listed at the top of this script
    while IFS= read -r f; do
        case $f in */) continue ;; esac
        ext=${f##*.}; [ "$ext" != "$f" ] || continue
        if printf '%s' "$ext" | grep -qiE "^($GAME_EXTENSIONS)\$"; then
            allowed=0
            for a in "${ALLOWED_MAC_FILES[@]}"; do [ "${f#Hellmouth-$VERSION/}" = "$a" ] && allowed=1; done
            [ "$allowed" = 1 ] || bad="$bad"$'\n'"    $f"
        fi
    done <<< "$names"
    [ -z "$bad" ] || die "the Mac zip holds files that could be game data:$bad"
    # the same names as tools/audit_windows_package.py looks for in the Windows release: /Users/<name>/, the home folder and the user
    # name, and the e-mail's name and the host's name too. Each is looked for in every file of the extracted zip.
    local tmp patterns=() labels=() v
    tmp=$(mktemp -d "${TMPDIR:-/tmp}/hellmouth-mac-zip.XXXXXX"); TEMPS+=("$tmp")
    unzip -q "$MZIP" -d "$tmp"
    add() { if [ "${#2}" -ge 4 ]; then labels+=("$1"); patterns+=("$2"); fi; }
    add "the home folder" "$HOME"
    add "the user name" "$(id -un)"
    add "the name part of git's user.email" "$(git config user.email | sed 's/@.*//')"
    add "the host's name" "$(hostname)"
    add "the host's short name" "$(hostname -s)"
    local hits="" i
    for i in "${!patterns[@]}"; do
        v=$(grep -rlaF -e "${patterns[$i]}" "$tmp" | sed "s|^$tmp/||" | head -5 || true)
        [ -z "$v" ] || hits="$hits"$'\n'"    ${labels[$i]}: $(echo "$v" | tr '\n' ' ')"
    done
    v=$(grep -rlaE -e '/Users/[A-Za-z0-9._-]+/' -e '[A-Za-z]:\\Users\\' "$tmp" | sed "s|^$tmp/||" | head -5 || true)
    [ -z "$v" ] || hits="$hits"$'\n'"    a /Users/<name>/ path: $(echo "$v" | tr '\n' ' ')"
    [ -z "$hits" ] || die "the Mac zip names the machine or the user that made it (files, not the text):$hits"
    echo "OK mac zip: $(echo "$names" | grep -vc '/$') files, only the repository's own files with a game extension (${#ALLOWED_MAC_FILES[@]}), nothing naming this machine"
}

fill_notes() {
    sed -e "s/@SHA256_WINDOWS@/$(sha256 "$WZIP")/g" -e "s/@SHA256_MAC@/$(sha256 "$MZIP")/g" "$NOTES_IN" > "$NOTES"
    local left other
    left=$(grep -oE '@[A-Z][A-Z0-9_]{2,}@' "$NOTES" | sort -u | tr '\n' ' ' || true)
    [ -z "$left" ] || die "the notes have a placeholder left: $left"
    grep -qF "$WNAME" "$NOTES" || die "the notes do not name $WNAME"
    grep -qF "$MNAME" "$NOTES" || die "the notes do not name $MNAME"
    other=$(grep -oE 'Hellmouth-[0-9]+\.[0-9]+\.[0-9]+-[A-Za-z0-9_.-]*' "$NOTES" | grep -vF "Hellmouth-$VERSION-" | sort -u | tr '\n' ' ' || true)
    [ -z "$other" ] || die "the notes name an asset of another version (copied forward?): $other"
    grep -qF "$(sha256 "$WZIP")" "$NOTES" || echo "note: the notes do not hold the Windows zip's SHA-256 (no @SHA256_WINDOWS@ in them)"
    grep -qF "$(sha256 "$MZIP")" "$NOTES" || echo "note: the notes do not hold the Mac zip's SHA-256 (no @SHA256_MAC@ in them)"
    echo "OK notes: $NOTES"
}

# After the release is made: the two assets are downloaded back through the API and compared with the local zips.
verify_release() { # uses SLUG TAG TITLE PUBLISH WNAME MNAME WZIP MZIP
    local json id html tmp n
    tmp=$(mktemp -d "${TMPDIR:-/tmp}/hellmouth-verify.XXXXXX"); TEMPS+=("$tmp")
    if [ "$PUBLISH" = 1 ]; then
        json=$(gh api "repos/$SLUG/releases/tags/$TAG") || die "no release for $TAG after the create"
    else
        # a draft has no tag yet: the release is found by its name among the drafts
        id=$(gh api "repos/$SLUG/releases" --paginate --jq ".[] | select(.draft == true and .name == \"$TITLE\") | .id" | head -1)
        [ -n "$id" ] || die "no draft named \"$TITLE\" after the create"
        json=$(gh api "repos/$SLUG/releases/$id")
    fi
    html=$(echo "$json" | python3 -I -c 'import json,sys; print(json.load(sys.stdin)["html_url"])')
    for n in "$WNAME:$WZIP" "$MNAME:$MZIP"; do
        local name=${n%%:*} file=${n#*:} aid remote
        aid=$(echo "$json" | python3 -I -c 'import json,sys; print(next((a["id"] for a in json.load(sys.stdin)["assets"] if a["name"] == sys.argv[1]), ""))' "$name")
        [ -n "$aid" ] || die "the release has no asset $name"
        gh api -H "Accept: application/octet-stream" "repos/$SLUG/releases/assets/$aid" > "$tmp/$name"
        remote=$(sha256 "$tmp/$name")
        [ "$remote" = "$(sha256 "$file")" ] || die "$name downloaded back is not the file that was uploaded (SHA-256 ${remote:0:12}... against $(sha256 "$file" | cut -c1-12)...)"
        echo "OK $name: downloaded back, same SHA-256 ($remote)"
    done
    if [ "$PUBLISH" = 1 ]; then
        local url code
        for n in "$WNAME" "$MNAME"; do
            url=$(echo "$json" | python3 -I -c 'import json,sys; print(next(a["browser_download_url"] for a in json.load(sys.stdin)["assets"] if a["name"] == sys.argv[1]))' "$n")
            # the first byte only: the status is 206 for a range, 200 if the server ignores it
            code=$(curl -s -L -r 0-0 -o /dev/null -w '%{http_code}' "$url")
            [ "$code" = 200 ] || [ "$code" = 206 ] || die "the public download of $n answers $code: $url"
            echo "OK public download of $n answers $code"
        done
        local latest
        latest=$(gh api "repos/$SLUG/releases/latest" --jq .tag_name)
        [ "$latest" = "$TAG" ] || die "releases/latest is $latest, not $TAG"
        echo "OK releases/latest is $TAG"
    else
        local code
        code=$(curl -s -o /dev/null -w '%{http_code}' "$html")
        [ "$code" = 404 ] || die "the draft's page answers $code to someone signed out (404 is right): $html"
        echo "OK the draft's page answers 404 signed out"
    fi
    echo "release: $html"
}

main() {
    parse_args "$@"
    cd "$(dirname "$0")/.."
    REPO=$PWD
    NOTES_IN=$(cd "$(dirname "$NOTES_IN")" 2>/dev/null && printf '%s/%s' "$PWD" "$(basename "$NOTES_IN")") || die "the notes file does not exist: $NOTES_IN"
    check_preconditions
    WZIP=$REPO/out/$WNAME MZIP=$REPO/out/$MNAME
    L=$REPO/out/release-$VERSION NOTES=$L/notes.md
    mkdir -p "$L"
    if [ "$SKIP_GATES" = 1 ]; then
        echo "note: gates skipped (--skip-gates)"
        [ -f "$WZIP" ] || die "--skip-gates needs $WZIP, made by tools/package_windows.sh"
    else
        run_gates "$L"
    fi
    make_mac_zip
    fill_notes
    local cmd=(gh release create "$TAG" --target "$(git rev-parse HEAD)" --title "$TITLE" --notes-file "$NOTES")
    if [ "$PUBLISH" = 1 ]; then cmd+=(--latest); else cmd+=(--draft); fi
    cmd+=("$WZIP" "$MZIP")
    if [ "$DRY" = 1 ]; then
        echo "dry run: this would be run, and is not:"
        printf ' %q' "${cmd[@]}"; echo
        exit 0
    fi
    echo "creating $TAG ($([ "$PUBLISH" = 1 ] && echo published || echo draft))"
    "${cmd[@]}"
    verify_release
}

# sourced (to try a function on its own) or run
if [ "${BASH_SOURCE[0]}" = "$0" ]; then main "$@"; fi
