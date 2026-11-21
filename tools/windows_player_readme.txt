Hellmouth @VERSION@ for Windows
================================

Buffy the Vampire Slayer (the original Xbox game), running on Windows 10 or 11 (64 bit). You need your own copy of the game:
a disc image (.iso) of the USA release, made from your own disc. Nothing of the game is in this download.

To play
-------
1. Unzip this folder anywhere you can write to (Documents or a folder on a drive of your own; not "Program Files", not a
   folder inside the zip). A short path such as C:\Hellmouth is the safest. Keep the folders together, and keep the
   name Buffy.exe: the game restarts itself by that name to load a level.
2. Run Buffy.exe. The first time it asks for your disc image.
3. It then builds the game for your PC from your disc, which takes a few minutes (a window shows the progress) and happens
   once, and again only after an update of this download. It needs about 1 GB of free space while it works. When it is done, choose Start
   (or press A on a controller). If you tick "Delete the build files", the working files (about 300 MB) are removed, and the
   next build after an update takes longer.
   The same first start makes the achievement pictures and the game's icon from your disc (they are not part of this download).
4. Next time Buffy.exe starts the game at once.

Your saves, settings and logs
-----------------------------
They are beside Buffy.exe (hdd\, settings.toml, logs\, Screenshots\, and achievements\ and Buffy.ico, the pictures made from your
disc), unless you start it with --data "<folder>" to keep them elsewhere. To start again from the disc, delete the build folder (and cache\game-code).

If something goes wrong
-----------------------
The window or a message says what failed and where. Three things cause most trouble:
  - Windows saying it "protected your PC" when Buffy.exe first starts: the program is not signed. Choose More info, then Run anyway.
  - Antivirus software removing or blocking a file in the compiler folder. Allow the whole folder, unzip again, and start the game.
  - A long or protected folder. Move it to a short path you can write to, such as C:\Hellmouth.
logs\compile.log has the details of the build, and logs\buffy.log those of the game.

Options (for launchers and tests)
---------------------------------
  Buffy.exe --game "<disc image or folder>"   use this copy of the game (otherwise the one you chose last time)
  Buffy.exe --data "<folder>"                  keep settings, saves, logs and the built game there
  Buffy.exe --prepare --game "<image>"         build the game with no window, printing "progress <0 to 1> <what it is doing>"
                                               lines, then exit with 0 when it is built (the game is not started)
  Buffy.exe --no-dialogs                       report failures with an exit code instead of a message box
RECOMPLAUNCHER_DATA in the environment does what --data does.

What is in this folder
----------------------
  Buffy.exe     the game's program and the runtime it runs on (no game code, no game data)
  compiler\     the programs that build the game from your disc at the first start: clang and lld (the LLVM project), and the
                two recompiler tools of this project. They run only from Buffy.exe and need no installation.
  licenses\     the licences of what is included. THIRD_PARTY.txt lists it; LICENSE is this project's (MIT).

The recompilation is the work of the Hellmouth project (https://github.com/djrobson5/Hellmouth) and the XboxXBERecomp SDK it is based on.
Buffy the Vampire Slayer is owned by its rights holders; this project is unofficial.
