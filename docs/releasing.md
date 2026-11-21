# Releasing

`tools/release.sh` makes a release: the checks, the two zips (Windows, and Mac: the signed `Buffy.app`) and the GitHub release. It never pushes and never makes a tag by itself
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
3. Set `LLVM_MINGW` (llvm-mingw for this machine), `MAC_DEPS` (the source-built Mac compiler and libraries,
   `tools/mac/build_toolchain.sh` and `build_libs.sh`: the app is made from them, and their licences go in it) and
   `RECOMPLAUNCHER_GUIDE` (RecompLauncher's developer guide, `recomplauncher-agent.md`), and sign `gh` in. Nothing else is needed from
   the environment.

## Run

    tools/release.sh --dry-run notes.md     # everything but the release; prints the gh command
    tools/release.sh notes.md               # the same, then a draft release (only the owners see it)
    tools/release.sh --publish notes.md     # a published release, marked as the latest

The gates are `tools/gates.sh`, the same checks as after any change, and take several minutes (two cross builds, the checks, the
packaging and the audit of the Windows release, the simulated first start, the Mac builds, the Mac build against the source-built
libraries, `tools/package_mac.sh` and `tools/mac/sim_package_mac.sh`). Their logs are in `out/release-X.Y.Z/`, and so are the filled in
notes (`notes.md`). The Windows zip is `out/Hellmouth-X.Y.Z-windows-x64.zip` and the Mac one `out/Hellmouth-X.Y.Z-macos-arm64.zip`
(`Buffy.app`, `recomplauncher.json` and `README.txt` in a folder `Hellmouth-X.Y.Z-macos-arm64`, made by `ditto`). The script audits the
Mac zip's contents once more before it goes out.

When the release is made, the script downloads both assets back through the API and compares their SHA-256 with the local zips. For a
published release it also checks the public download URLs and that `releases/latest` is the new tag; for a draft, that its page
answers 404 to someone signed out. Look at a draft on GitHub and publish it there.

`tools/sim_first_start.sh` plays the last `out/Hellmouth-*-windows-x64.zip` in name order (so 0.2.9 comes after 0.2.10); the script
stops if that is not the zip it just made.

## The Mac zip

`tools/package_mac.sh` makes it from the build in `build/app-dylib-deps` (`sdk/host/build.sh ... --game-code-dylib` with `MAC_DEPS` set)
and audits it (`tools/audit_mac_package.py`, on the folder and on the zip's contents extracted): only the files it lists, none made from
the game (no generated code, `Buffy.dylib`, `Default.xbe`, achievement pictures, or the game's own icon), nothing that names the machine
that built it (the sources' paths are made relative when the executable is built, `-ffile-prefix-map` and `-Wl,-S`), every Mach-O arm64
for macOS 13.0, and the app signed ad hoc (verified with `codesign --verify --deep --strict`). A new file in the app fails the audit until
it is added to its list.

The app is signed ad hoc, not by a developer account, so a Mac that downloaded it in a browser asks the player to allow it once (the
zip's `README.txt` says how); an install by RecompLauncher does not. Notarization would need a Developer ID certificate, the hardened
runtime (`-o runtime`), the entitlement `com.apple.security.cs.disable-library-validation` (the dylib the first start builds is signed
ad hoc, with no team) and an absolute path in the executable's `dlopen` of it; none of that is done.

The Mac `recomplauncher.json` is `tools/recomplauncher-mac.json.in` (its own template: the program, the game's code file, the settings
and the exit-code texts differ from Windows's). `tools/check_recomplauncher.py` checks it against RecompLauncher's schema and, for each
setting, against what the Mac program lists with `--describe-settings` (the app's `Contents/Resources/settings.json`).

`install.sh` and `sdk/host/bundle.sh` (the old install: a Homebrew build, put in `/Applications`) are retired in the README but are
still in the repository.
