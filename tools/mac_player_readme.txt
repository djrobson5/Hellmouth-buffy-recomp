Hellmouth @VERSION@ for Mac
===========================

Buffy the Vampire Slayer (the original Xbox game), running on Macs with Apple silicon (M1 or later) and macOS 13 (Ventura) or later.
You need your own copy of the game: a disc image (.iso) of the USA release, made from your own disc. Nothing of the game is in this
download.

To play
-------
1. Drag Buffy.app out of this folder to your Applications folder (or any folder you can write to; not a folder inside the zip).
   If you used the old install.sh, this replaces the app it put there: your saves and settings are not in the app and stay.
2. Open Buffy.app. macOS may say it cannot check the app for malware: it is signed only on this project's own machine, not by an
   Apple developer account. To open it anyway:
     - macOS 15 and later: open it once and close the message, then System Settings > Privacy & Security, scroll down to
       Security, and choose Open Anyway beside "Buffy". Enter your password, then Open.
     - macOS 13 and 14: Control-click (right-click) Buffy.app, choose Open, then Open in the message.
     - Or, in Terminal:  xattr -dr com.apple.quarantine /Applications/Buffy.app   (with the path where you put it)
   You do this once. If you install Hellmouth with RecompLauncher (see below), none of it is needed.
3. The first time it asks for your disc image. It then builds the game for your Mac from your disc, which takes a minute or so (a
   window shows the progress) and happens once, and again only after an update of this download. It needs about 500 MB of free space
   while it works, and no network. When it is done, the game starts. The same first start makes the achievement pictures and the
   game's own icon from your disc; they are not part of this download.
4. Next time Buffy.app starts the game at once.

Your saves, settings and logs
-----------------------------
When you open Buffy.app yourself they are in ~/Library/Application Support/Buffy:
  hdd/               your saves
  settings.toml      the settings (the game's own menu, F10, changes them too)
  logs/              buffy.log, compile.log (the build of the game), and achievements.txt (what you have earned)
  Screenshots/       pictures taken with the \ key
  build/Buffy.dylib  the game built from your disc, and achievements/, Buffy.png (made from it)
To open that folder: in Finder, Go > Go to Folder, and paste the path. To start again from the disc, delete the build folder.

Carrying your saves over
------------------------
From the old installed app (install.sh): nothing to do. Its data is in that same folder, and the new Buffy.app, opened on its
own, uses it as it is.
To or from RecompLauncher: it keeps everything in a folder named recompdata inside the game's folder, with the same names but one
difference: achievements.txt is in recompdata itself, not in logs/. To move your game between the two, copy hdd, settings.toml and
Screenshots, and achievements.txt (logs/achievements.txt on one side, achievements.txt on the other). Close the game first.

With RecompLauncher
-------------------
If you use RecompLauncher (https://recomplauncher.com), install Hellmouth from it: it asks for your disc image, builds the game from it,
starts it, and updates it for you, and macOS asks nothing about the app. It keeps everything the game writes in recompdata inside
the game's folder, and an update leaves that folder alone. Keep your disc image where it is: the game reads it while you play.

Updating to a new version
-------------------------
Replace Buffy.app with the new one. Everything you keep is outside the app (above). The first start after an update may build the
game again, as on the first day.

If something goes wrong
-----------------------
The window or a message says what failed and where. logs/compile.log has the details of the build, logs/buffy.log those of the game.
  - "Buffy is damaged and can't be opened", or it will not open at all: the download was changed on the way, or macOS holds it
    in quarantine. Unzip it again with Finder, or run the xattr command above.
  - The game says it needs your disc image again: choose the .iso. It must be the USA release (not Chaos Bleeds).
  - If you moved Buffy.app after the first start nothing is lost: it builds nothing again, the game's code is in the data folder.

Options (for launchers and tests)
---------------------------------
  Buffy.app/Contents/MacOS/Buffy --game "<disc image or folder>"   use this copy of the game (otherwise the one you chose last time)
  Buffy.app/Contents/MacOS/Buffy --data "<folder>"                  keep settings, saves, logs and the built game there. A folder named
                                                                    like this has achievements.txt in itself, not in logs/
  Buffy.app/Contents/MacOS/Buffy --prepare --game "<image>"         build the game with no window, printing "progress <0 to 1> <what it
                                                                    is doing>" lines, then exit with 0 when it is built
  Buffy.app/Contents/MacOS/Buffy --no-dialogs                       report failures with an exit code instead of a message
RECOMPLAUNCHER_DATA in the environment does what --data does.

What is in Buffy.app
--------------------
  Contents/MacOS/Buffy        the game's program and the runtime it runs on (no game code, no game data)
  Contents/Frameworks/        SDL3 and MoltenVK (Vulkan on Metal), built from source for this project
  Contents/Resources/compiler/  the programs that build the game from your disc at the first start: clang and lld (the LLVM project)
                              and the two recompiler tools of this project. They run only from Buffy, and need no installation or
                              developer tools.
  Contents/Resources/licenses/  the licences of what is included. THIRD_PARTY.txt lists it; Hellmouth-LICENSE.txt is this project's (MIT).
  recomplauncher.json (beside the app)  what RecompLauncher reads to set the game up and show its settings; nothing else uses it

The recompilation is the work of the Hellmouth project (https://github.com/djrobson5/Hellmouth-buffy-recomp) and the XboxXBERecomp SDK it is based on.
Buffy the Vampire Slayer is owned by its rights holders; this project is unofficial.
