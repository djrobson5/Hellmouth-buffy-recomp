# Releasing

`tools/release.sh` makes a release: the checks, the two zips and the GitHub release. It never pushes and never makes a tag by itself
(`gh release create` makes the tag, at the commit the script names).

## Before

1. Set the version in `src/common/game_info.cpp` (`.version = "buffy-port X.Y.Z"`), commit it and **push main**. The script stops unless
   HEAD is `origin/main` and the working tree is clean.
2. Write the release notes by hand, in a file outside the repository (or anywhere that is not in the way of the clean tree check).
   Two placeholders in them are filled in by the script with the SHA-256 of each zip:

   | placeholder         | replaced by                                  |
   |---------------------|----------------------------------------------|
   | `@SHA256_WINDOWS@`  | SHA-256 of `Hellmouth-X.Y.Z-windows-x64.zip` |
   | `@SHA256_MAC@`      | SHA-256 of `Hellmouth-X.Y.Z-macos-arm64.zip` |

   The notes must name both assets by their file names, and no asset of another version (an old name copied forward fails the
   script). Any other `@NAME@` left in them fails it too.
3. Set `LLVM_MINGW` (llvm-mingw for this machine) and `RECOMPLAUNCHER_GUIDE` (RecompLauncher's developer guide,
   `recomplauncher-agent.md`), and sign `gh` in. Nothing else is needed from the environment.

## Run

    tools/release.sh --dry-run notes.md     # everything but the release; prints the gh command
    tools/release.sh notes.md               # the same, then a draft release (only the owners see it)
    tools/release.sh --publish notes.md     # a published release, marked as the latest

The gates are `tools/gates.sh`, the same checks as after any change, and take several minutes (two cross builds, the checks, the
packaging and the audit of the Windows release, the simulated first start, the Mac builds; with `MAC_DEPS` set, also the Mac build
against the source-built libraries). Their logs are in `out/release-X.Y.Z/`, and so are the filled in notes (`notes.md`). The Windows zip is
`out/Hellmouth-X.Y.Z-windows-x64.zip` and the Mac one `out/Hellmouth-X.Y.Z-macos-arm64.zip` (a `git archive` of HEAD, checked for
game data and for anything that names the machine).

When the release is made, the script downloads both assets back through the API and compares their SHA-256 with the local zips. For a
published release it also checks the public download URLs and that `releases/latest` is the new tag; for a draft, that its page
answers 404 to someone signed out. Look at a draft on GitHub and publish it there.

`tools/sim_first_start.sh` plays the last `out/Hellmouth-*-windows-x64.zip` in name order (so 0.2.9 comes after 0.2.10); the script
stops if that is not the zip it just made.

## A Mac zip file with the extension of a game file

The Mac zip may hold only the repository's own files that look like game files by extension (`.png`, `.ico`, ...). They are listed in
`ALLOWED_MAC_FILES` at the top of the script: the README's screenshots and `resources/exe_icon.ico`. A new one fails the release until
it has been looked at and added there.
