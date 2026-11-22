# Changes to the SDK

`sdk/` is a copy of `ports/fellowship/fotrxbox/sdk` from [Athelas](https://github.com/Shagg88/Athelas) by Shagg, commit
aefcfe2, itself a fork of [XboxXBERecomp](https://github.com/TGP482/XboxXBERecomp) by TGP482. Both are MIT licensed:
`sdk/LICENSE` and `sdk/ATHELAS-LICENSE`.

What this port changed:

- `sdk/xdk/4432/layout.toml`: new.
- `sdk/cmake/XbeRecomp.cmake`: `XBE_USE_GENERATED` builds from the existing `generated/<dir>` with no recompile step and no tool targets, for a
  cross build to Windows from a Mac. The game's `CMakeLists.txt` and `CMakePresets.json` (`win-x64-mingw`) are at the top of the repository.
- The recompiled code in a DLL, for the Windows build, from [XboxXBERecomp](https://github.com/TGP482/XboxXBERecomp) commit 42563979
  ("Game code builds into a DLL the first start compiles", with 90bccea3), changed to build for the mingw ABI with llvm-mingw
  rather than MSVC's. The executable holds none of the game's code, so it can be shared, and the DLL is compiled from the
  player's own xbe. What changed here:
  - `sdk/runtime/cpu/x86_ops.h`: `X86_GAME_CODE` (the DLL's compile) and `XBE_GAME_CODE_DLL` (the executable it belongs to) make
    `X86_HOST_API` and `X86_FUNC` import and export, and `X86_WEAK_FUNC` an `/alternatename` in the DLL. `X86_CATCH_RESUME` is the
    catch that generated functions end with: by type as before (the same machine code on macOS), and in the DLL a `catch (...)`
    that asks `x86_try_resume`. `X86_TRACED_FUNC` makes a plain function and a weak alias of it.
  - `sdk/runtime/cpu/dispatch.cpp`: `dispatch::Load` (the DLL's tables, by name), `EntryPoint` and `InCode` for the callers that read
    `X86CodeBase`, `X86CodeEnd` and `X86EntryPoint` (`kernel/boot.cpp`, `xdk/d3d8.cpp`), and `x86_try_resume`, with the resume that
    `x86_icall` threw kept per thread. The exception that reaches it must be an `X86Resume`, so another one passing through the
    DLL's frames is rethrown. `x86_context.h` declares the tables `extern "C"`.
  - `sdk/runtime/cpu/libc_exports.cpp`, `host_exports.def.in`: the executable's exports that are not its own API: the C++ exception
    runtime and wrappers for the C library calls the DLL leaves undefined (an upstream list adapted to what this code calls), and
    for every call the recompiler can write for any game (`remainder` for FPREM1, `___chkstk_ms` for a frame over 4 KB), which
    `tools/library_calls.cpp` uses in a synthetic compile and link that `tools/check_game_code_dll.sh` runs, with no game data.
  - `sdk/runtime/cpu/freestanding/`: stand-ins for `<cstdint>`, `<cstring>`, `<cmath>`, `<limits>` and `<type_traits>`, for the
    compile without a C++ library.
  - `sdk/runtime/app/game_code.cpp`, `game_build.cpp`/`.h`, `game_code_recipe.h`, `pe_export.h`, `app.h`, `main.cpp`: the first start
    builds the DLL on the player's PC from their own disc (adapted from the same upstream commit's `game_code.cpp`, which started
    clang and lld and showed a progress window; here the steps are split from the Windows). `game_build.cpp` is the build: the disc's
    `Default.xbe` and `final.map`, `buffy_config`, `xbe_recomp`, a compile of each file (biggest first, as many at once as there is
    memory and cores for, each into a `.tmp` and then its name, kept with a hash of source, flags and compiler and reused by content
    because the recompiler rewrites every file), `lld-link`, the stamp read from the new DLL, the move into `<data>\build`, progress
    by bytes of source, and the failures with their step, hint and the log. It uses no Windows or runtime calls and starts programs
    through a `Runner`, so `tools/first_start_check.cpp` runs it on a Mac over the release's folders with a POSIX one. `pe_export.h`
    reads an exported string from a PE file's bytes (the stamp, without loading anything). `game_code.cpp` is the Windows side: the
    runner (`CreateProcessW`, no window, below normal priority, a kill-on-close job, one `compile.log`), the GDI progress window
    (Start / Exit, a delete box for the work folder, controller support through `input::HeldOnAnyController`), the build lock and
    the failure box. `PrepareGameCode` loads `<data>\build\<IMAGE_LIBRARY>` when its stamp is current and builds it when not (a
    relaunched or `--no-dialogs` run never builds: it ends with `kExitNoGameCode` or `kExitStaleGameCode`). `Sha1` is public.
  - `sdk/runtime/app/main.cpp`, `app.h`, `settings.cpp`, `achievements.cpp`, `gpu/d3d12.cpp`, `system_info.cpp`; `sdk/host/main.cpp` and
    `sdk/harness/app_headless.cpp`: `app::DataDir` (`--data`, `RECOMPLAUNCHER_DATA`, else the folder of the exe) is where the settings
    file, hard disk, logs, screenshots, shader cache, texture packs, crash dumps and `achievements.txt` go, so one folder holds all that
    a game writes; the explicit options still win and the default layout is the one it was. The Mac host and the harness define it as
    the exe's folder. Options take `--name=value` as well as `--name value`; `--prepare`, `--relaunched` and `--clean-work` are
    new. `PrintOut`/`PrintErr` write to a launcher's pipe or the parent's console from a GUI program.
  - `sdk/runtime/app/supervisor.cpp`, `supervisor_args.h`, `main.cpp`, `app.h` (`app::Supervise`, Windows): a start by RecompLauncher
    (`RECOMPLAUNCHER=1`, a normal play start) runs the game as a child of the same executable inside a job object (kill on close, a completion
    port) that only the first process holds, and ends with the code of the last process of the chain: `kernel::Relaunch` starts a new
    process at each level load, and a launcher's Stop kills only the process it started. `HELLMOUTH_SUPERVISED` marks the child; `--prepare`,
    `--relaunched`, `--version` and `--describe-settings` are never supervised. The decision is in `supervisor_args.h` (no Windows code), checked on a
    Mac by `tools/supervisor_args_check.cpp` from `tools/sim_first_start.sh`.
  - `sdk/runtime/kernel/keeper.{h,cpp}`, `sdk/host/main.cpp`, `kernel/boot.cpp` (`kernel::Relaunch`), `supervisor_args.h` (a Mac): the same for a Mac,
    as a keeper that runs the game as its child, starts each next instance for it over a pipe (so every instance is the keeper's child, as
    Launch Services' and a process's own start of the next were not good enough for), and ends every game process when it is killed (a pipe
    only it holds the write end of). `HELLMOUTH_SUPERVISED` holds the three pipes' descriptors; one-shot modes also include `--check-game-code`,
    `--print-paths` and `--title-id`. Checked without a game or a window by `tools/mac/keeper_check.{cpp,sh}`.
  - `sdk/runtime/app/game_choice.cpp`, `game_files.cpp`: `ChooseGame` (Windows) picks the game as the Mac app does: `--game`, else
    the image remembered in `<data>\disc_image.txt` (kept only once accepted), else `assets`, else `IFileOpenDialog`. `FindGame` is
    split, so `FindGameFrom` can try a place without ending the run; with `XBE_GAME_CODE_DLL` another release is refused.
  - `sdk/runtime/app/settings.cpp`, `settings_list.cpp`: `[graphics] renderer` defaults to Vulkan on Windows too (Direct3D 12 stays
    selectable and is used when Vulkan cannot start); the Windows help says what Vulkan lacks.
  - `sdk/runtime/core/input.cpp`: `HeldOnAnyController` (the face buttons, as XInput's masks) for the first start's window, and `Init`
    can be called twice.
  - `--prepare` (`app::PrepareOnly`, in `game_code.cpp`, called from `main.cpp` before the settings are read): the same build with no
    window, `progress` lines on stdout, exit 0 or `kExitBuildFailed` with the reason on stderr.
  - `sdk/tools/recomp`: `[xbe] library`, which only a DLL build needs, makes the recompiler write `exports.def` (what the DLL exports: its tables and one
    `__imp__sub_XXXXXXXX` name for each function) and `imports.def` (the same, with `sub_XXXXXXXX` aliases, for the executable's
    import library). `IMAGE_LIBRARY` and `IMAGE_SECTIONS_HASH` are in `image_symbols.h`, so the executable has them without the DLL: the
    hash used to come from the generated `x86_func_mapping.cpp`, which the DLL now holds. Hooks are declared `X86_HOST_API` (no
    separate export header: the declaration exports them under mingw). `sdk/tools/symbols` takes an optional `--library`.
  - The ABI stamp, `X86GameCodeStamp` in the DLL (`xbe-game-code abi=<token> image=<hash> symbols=<hash>`), which `dispatch::Load`
    compares with the text `app/game_code.cpp` builds from `game_code_flags.h` and `image_symbols.h`, failing with `kExitStaleGameCode`.
    `XbeRecomp.cmake` computes the token (a SHA-1 of the flags and of the files that define the executable's ABI and replacements,
    listed in `game_code/abi_inputs.txt`) and `sdk/tools/recomp` writes the rest: `IMAGE_CODE_STAMP` in `image_symbols.h`, and the
    stamp in `x86_func_mapping.cpp` (compiled only for the DLL), which needs `-DXBE_GAME_CODE_ABI=<token>`.
  - `sdk/runtime/app/game_build.cpp`/`.h`, `game_code.cpp`, `game_code_recipe.h`, `achievements.cpp`, `window_icon.cpp`, `app.h`, `main.cpp`:
    the first start also makes a game's pictures from the player's disc (the SDK knows only a tool, a recipe version and two disc files): `MakePictures`
    runs `buffy_config --pictures` over files it takes from the disc into `<data>`, behind a marker of its own (`<data>\pictures.stamp`, the
    recipe's version, `kPicturesRecipe`), so a start with a current DLL makes them too (`EnsurePictures`, in a normal start and in `--prepare`).
    A picture that cannot be made is a line in `logs\compile.log` and never a failure. `Icon()` looks in the data folder after the folder beside the
    exe, and `app::SetWindowIcons` (Windows) sets `<data>\Buffy.ico` on the game window. `shortcut.cpp` (`app::UpdateShortcut`, Windows, from `main.cpp`
    after `PrepareGameCode`) keeps `<title>.lnk` beside the exe, with the data folder's icon and a `--data` when the data folder is not the exe's, written again when its target or
    arguments are not this start's.
  - `sdk/tools/LLVM-LICENSE.txt`: the licence text for the clang and lld the first start runs.
  - `sdk/tools/tool.manifest`: the manifest the Windows command line tools carry (UTF-8 command line and file names, long paths).
  - `sdk/cmake/XbeRecomp.cmake`: `XBE_ICON` (an .ico a game sets before `add_xbe_image`) is the executable's icon resource (ICON 1 beside the manifest); a
    game that sets none has no icon, as before.
  - `sdk/cmake/XbeRecomp.cmake`: `XBE_GAME_CODE_DLL` leaves the generated sources out of the executable, gives it the import library, the
    delay loading and the module definition file, and adds the `game_code_dll` target, which builds the DLL with the first start's
    commands for checking. The flags are in one place, written to `game_code_flags.h` for the executable.
- `tools/package_windows.sh`, `audit_windows_package.py`, `windows_player_readme.txt`, `sdk/THIRD_PARTY.txt`: the Windows release and its audit,
  and the notices of the compiler and runtime it carries. `tools/sim_first_start.sh` and `first_start_check.cpp` play the first start on a Mac.
- `sdk/runtime/gpu/state.h`: render state numbers for XDK 4432.
- `sdk/runtime/kernel/memory.cpp`: macOS shared memory for the physical window, and commits on 16 KB host pages.
- `sdk/runtime/kernel/threads.cpp`: macOS thread names. Binding to a core does nothing, as macOS has no way to do it.
- `sdk/runtime/kernel/boot.cpp`: restarting into the game finds the executable the macOS way, keeps its arguments, says
  it's a restart through `XBE_RELAUNCHED`, and logs a failed restart.
- `sdk/host/`: new. The macOS program: SDL window, events, sound, and the software renderer's picture as the fallback.
- `sdk/runtime/gpu/vk.cpp`, `vk_loader.cpp`, `vk_loader.h`: run on macOS: MoltenVK opened directly, a Metal surface, the
  portability extension and BC textures turned on, the menu drawn with ImGui as on Windows, and screenshots.
- `sdk/runtime/app/overlay.cpp`: builds outside Windows, showing only the settings that the Vulkan renderer and SDL host
  act on. Its controller reading no longer takes the stick clicks that open the menu as closing it again.
- `sdk/host/ui_events.cpp`: new. SDL's events into ImGui, as v1.91.0's SDL3 backend doesn't build against SDL 3.2 or later.
- `sdk/runtime/gpu/vk.cpp`: the picture reaches the window through `kBlitHlsl` when anything changes it (the game's gamma
  ramp, `[graphics] brightness`, sharpening, the colour look, SMAA), averaged down first above 100% render resolution, as `d3d12.cpp` does.
  With nothing to do it keeps the plain blit. It also takes `smooth_2d`, the HUD's sharpening, `console_textures` and texture packs from `d3d12.cpp`.
- `sdk/runtime/gpu/vertex.cpp`, `vertex.h`, `vk.cpp`: `LinesToQuads` draws pretransformed lines as the console pixels they cover, as a target's own lines
  leave gaps once it's upscaled. The menus' unselected text is made of them. `d3d12.cpp` doesn't call it yet.
- `sdk/runtime/gpu/common.cpp`, `common.h`: the colour looks and SMAA's source moved here from `d3d12.cpp` for both renderers.
- `sdk/runtime/gpu/backend.h`, `sdk/runtime/xdk/d3d8.cpp`: `g_swaps` counts frames whichever backend shows them; the
  fallback's log line no longer names Direct3D 12.
- `sdk/runtime/xdk/d3d8.cpp`: `DrawIndexedVerticesUP` no longer adds the base vertex from `SetIndices`, as the XDK's own
  routine doesn't. It read vertices from the wrong memory, drawn as large flickering polygons.
- `sdk/runtime/xdk/d3d8.cpp`: a host `D3DDevice_IsBusy`, as the library's reads GPU registers that don't exist here.
- `sdk/runtime/app/settings.cpp`, `app.h`: outside Windows, widescreen makes a 4:3 window size wide rather than turning
  itself off. `g_frame_43` lets a game show 4:3 for now, and `WideAspect` is the shape it would otherwise have.
- `sdk/runtime/gpu/d3d12.cpp`: `ApplySettings` leaves the render targets alone while the menu holds the game (`app::Held`), as the
  Vulkan renderer does, so a change of render resolution or widescreen from the menu doesn't blank the picture behind it.
- `sdk/runtime/app/`: `[display] high_frame_rate` and `g_high_frame_rate`, which `Pace` in `sdk/host/main.cpp` follows.
- `sdk/runtime/app/app.h`: `MetalLayer` and `g_pixel_height`, for the renderer and the render scale.
- `sdk/runtime/core/input.cpp`, `input.h`: outside Windows the window thread takes SDL's events and tells of controllers
  through `PadEvent`; the guest's threads only read them. Macs may only pump events on the first thread.
- `sdk/runtime/app/keyboard.cpp`, `core/keys.h`: outside Windows the window thread reports keys and the mouse.
- `sdk/runtime/app/settings.cpp`: builds outside Windows, where the first start opens a 960x720 window on Vulkan, and the
  render scale follows the window's height in pixels.
- `sdk/runtime/app/game_files.cpp`: builds outside Windows, where another release of the game needs `--any-release`.
- `sdk/harness/build.sh`: builds on macOS and arm64, leaves out the Vulkan sources, and takes `HOST_OPT`, `HOST_FLAGS` and
  `LINK_FLAGS`.
- `sdk/harness/README.md`: says macOS as well as Linux.
- `sdk/harness/null_backend.cpp`: stand-ins for two names `xdk/d3d8.cpp` calls directly, drawn frames go to
  `harness::g_show` when a host sets it, and with `XBE_HOST_VULKAN` the renderer setting picks Vulkan.
- `sdk/harness/harness.h`: `g_show` and `g_show_start`.
- `sdk/harness/reference_renderer.cpp`, `.h`: `ReadBgra`, a surface's pixels for the window.
- `sdk/runtime/app/achievements.cpp`: built on macOS, with the note showing a picture where the game supplies one, and notes carried
  across the restart into a level. `sdk/runtime/app/app.h`, `overlay.cpp`, `sdk/runtime/gpu/vk.cpp`: `app::g_ui_picture` for those pictures.
- `sdk/licenses/LiberationSans-OFL.txt`: new, the licence of the menu font.
- `sdk/runtime/kernel/threads.cpp`, `kernel.h`, `objects.cpp`, `xboxkrnl.cpp`: for rewinding the guest in its process (`src/game/rewind.cpp`).
  Each `Thread` reports where it is (`kstate`, the guest esp and return address of the call that blocked it, its native
  stack pointer, its start routine, the guest critical sections it holds) at `Wait`, `KeDelayExecutionThread`,
  `NtYieldExecution` and `SuspendPoint`; the delay and yield now check `SuspendPoint` on the way out as well. A guest thread
  can be held still with `QuiesceGuestThreads` (it stops at its next `SuspendPoint`), and the host threads that write guest
  memory run inside a `HostWriter` that `StopHostWriters` waits for. `RunApcs` waits for the hold before it runs each APC's guest
  code, in the state of the call it was blocked in (a wait or delay that took an APC could otherwise run guest code, Bink's wait
  among it, while memory was being copied).
  The clock, vertical blank (`xdk/d3d8.cpp`), DPC, timer, pad poller (`xdk/xapi.cpp`), sound pump and mixer (`xdk/dsound.cpp`) threads use it.
  None of this bookkeeping runs until a game calls `kernel::EnableRewinding()` (before its first thread starts): the host
  writer gate, `EnterBlocked` and `LeaveBlocked`, and the counts of critical sections and locks held, all test `kernel::Rewinding()`
  first, one relaxed load. `SuspendPoint` itself reads two more flags (the hold, a thread to stop); the native frame address
  it takes (`__builtin_frame_address(1)`, which needs frame pointers) is read only when a thread actually parks.
- `sdk/runtime/kernel/memory.cpp`, `memory.h`: `CaptureSnapshot` copies every committed range, the allocated part of the
  physical window (once) and the allocator tables.
- `sdk/runtime/kernel/rewind.h`, `rewind.cpp`, `objects.cpp`, `files.cpp`, `threads.cpp`, `xboxkrnl.cpp`, `memory.cpp`, `cpu/dispatch.cpp`: a restore
  of a snapshot, for a game that rewinds in the process (`src/game/rewind.cpp`). `mem::RestoreSnapshot` makes the commit state and the
  allocator tables the snapshot's and copies the bytes back. `CaptureKernel` and `RestoreKernel` keep the handle table (the objects it names
  stay alive), the pointer table, each object's signal, owner and recursion, the open files' positions and directory listings, the
  timers and DPCs (their deadlines on the steady clock moved by the time between the two), each thread's context and APCs, and the
  guest clock and time stamp counter (set from the snapshot's absolute reading). `PlanRestore` compares the threads with the snapshot's
  and refuses before anything changes; a thread whose start routine the game names (a sound stream's) may be stopped (it leaves at its
  next `SuspendPoint` without writing guest memory) or started again from its entry, as the same `Thread`, with the same stack and handle.
  The timer, DPC and sound pump threads now enter the host writer gate before they take an item, so none is held across a restore.
- `sdk/runtime/xdk/rewind.h`, `xapi.cpp`, `bink.cpp`: `CaptureInput` and `RestoreInput` keep the XInput handle table (a handle is guest memory, the pad
  it names is on the host: one closed after the snapshot would leave the title polling a handle that is gone), and `ForgetMovies` clears
  the Bink frames held back for the 120 fps pacing, which are named by guest address.
- `sdk/runtime/xdk/rewind.h`, `d3d8.cpp`, `dsound.cpp`: `CaptureDevice` and `RestoreDevice` copy the host's Direct3D state (shader tables, `g_state`,
  surfaces, the immediate mode and user pointer vertex state; three function statics moved to file scope for it), `CaptureSound` and
  `RestoreSound` the voices and the callbacks waiting. `gpu::Backend::ResetResources` (no-op by default; the Vulkan backend drops its
  textures and targets after the GPU is idle, the threaded renderer forwards it) makes everything made from guest memory again.

  Host state read through and left alone, as a restore needs nothing of it: `gpu/texture.cpp`'s dirty epochs (they only change what a
  texture cache compares, and `ResetResources` empties the caches), `gpu/vertex.cpp`'s `g_frame_sides` (set again every frame), `kernel/boot.cpp`'s
  launch and display hand-over (the front end and relaunches only), `kernel/disc.cpp` (the mounted image, read only), and `xnet.cpp` (the
  sockets close at a relaunch; a title that opens them in a level would need them kept). `VkBackend::ResetResources` has been read but not run: the
  headless harness has no Vulkan device, and `ResetResources` is not written for the Direct3D 12 backend.
- `sdk/runtime/kernel/rewind.h`, `rewind.cpp`: `PlanRestore` takes `check_main`; false leaves the calling thread's own place out, so a game can ask
  from anywhere whether the other threads allow a restore, and decide before it commits to one. `kernel::g_relaunch_hook` (`boot.cpp`) is called
  first in `Relaunch`, so a game that keeps snapshots frees them before the next instance starts.
- `sdk/runtime/kernel/memory.h`, `memory.cpp`: `mem::SnapshotSize` (what a snapshot would copy now), `ReserveSnapshot` (a buffer of that size with every
  page touched, so the first snapshots do not wait for the system to supply pages) and `ReleaseSnapshot`. A snapshot that finds its buffer too small
  makes a new one, 12.5% larger than it needs (`Snapshot::reused` says which). `mem::DiffSnapshot` counts the bytes that differ between the
  guest's memory and a snapshot, for checking a restore.
- Review fixes to the rewind (`kernel/threads.cpp`, `objects.cpp`, `rewind.cpp`, `xboxkrnl.cpp`, `xdk/*`, `gpu/*`):
  - `xdk::FlushDevice` finishes what the renderer was given while the guest is held, before a restore replaces memory. `gpu::Backend::SupportsReset`
    (false by default; the Vulkan backend, the threaded renderer over it and the harness's null backend say true) and `kernel::CanMeasureStack`
    (false on Windows, where `HostStackBase` cannot ask) let a game keep the rewind off where it cannot be done right.
  - `kernel::HoldPoint` is for a thread that waits by polling guest memory (the CLocker spin): it says it runs before it looks at the hold and waits,
    in its blocked state, while the guest is held. `PlanRestore` refuses when a thread would come out holding a different number of critical
    sections or locks than the snapshot had, or sits inside host work (`Thread::host_work`, set around DirectSound's completed callbacks).
  - `CaptureKernel` takes a callback that runs with the dispatcher and the tables locked; the game copies guest memory there, so the objects and the
    memory agree. Waits and delays look at `Thread::doomed`, so `StopThreads` can stop a thread blocked in one; when it fails, the threads that had
    already left start again from their entry (`g_rewind_fail_stop` makes it fail, for a test). `KernelSnapshot::Clear` lets go of the objects, files and
    thread references a snapshot holds.
  - `EnsureDpcThread` starts the DPC thread (its stack and KPCR are guest memory) before the first snapshot. The device snapshot keeps
    `SwitchTexture`'s lazily made header table. `TakeGateWaitNs` reports the longest a host writer waited at the gate.
- The game code as a Mach-O dylib for the macOS host (`sdk/host/build.sh --game-code-dylib`): `cpu/x86_ops.h` has `__APPLE__` branches (`X86_WEAK_FUNC` defines
  nothing in the dylib, the host's `X86_HOST_API`/`X86_FUNC` are default visibility, `X86_EXTERN_FUNC` goes through a slot `dispatch::Load` fills by name,
  `X86_TRACED_FUNC` is refused); `cpu/dispatch.cpp` has the dlopen loader (`dispatch::Load(path, stamp)`, `Describe`), `cpu/libc_exports.cpp` is Windows only;
  `app/game_build.cpp` builds a dylib when `Recipe::target` is `Target::MachO` (tools without `.exe`, `ld64.lld`, `WriteAliasResponse`, `app/macho_export.h` for the
  stamp) and has `BuildFromSources`; `cmake/XbeAbi.cmake` is the ABI token and flags for both platforms (`cmake/xbe_abi.cmake` runs it as a script);
  `harness/build.sh` takes `NO_GEN=1`. `tools/check_game_code_dylib.sh` checks the result. The Windows token changes once with `x86_ops.h`.
- The Mac first start for that build (`sdk/host/first_start.cpp`, `first_start_window.mm`, `app/posix_runner.h`, `host.h`; `sdk/host/main.cpp`):
  `app::PrepareGameCode` and `PrepareOnly` as on Windows, over the same `game_build.cpp`; `--prepare` (windowless, before SDL), `--clean-work`, `--data` and
  `RECOMPLAUNCHER_DATA` (`app::DataDir`: with one named the layout is Windows's, otherwise the Mac app's folder in an app and the exe's folder outside),
  `--option=value`, `--print-paths`. `app::AppFolder` is where the compiler folder is (beside the exe; in an app `Contents/Resources`). The
  achievement pictures and the icon are read from the data folder first (`ach::` icons, the Dock icon from `Buffy.png`). `sdk/host/game_code_dylib.sh`
  also builds `xbe_recomp`, `buffy_config` and `sdk_names.txt` into `<out>/compiler`. `tools/mac/sim_first_start_mac.sh` plays it with no window.
  `sdk/host/bundle.sh` (the old installer's) does not put `compiler/` into an app, so an app made by it stops at its first start with "The app is incomplete"; `tools/package_mac.sh` makes the Mac release's app (milestone M5).
  The build's programs run in process groups of their own, and are ended with their groups on a stop (SIGTERM and SIGINT cancel the build, with the
  window or under `--prepare`); a build killed with SIGKILL leaves its programs listed in `cache/game-code.children`, which the next build (holding
  `cache/game-code.lock`) ends before it starts. Never point `--data` or `RECOMPLAUNCHER_DATA` at `~/Library/Application Support/Buffy` for an
  existing install: a data folder named gives the Windows layout, and `achievements.txt` is then looked for in the folder itself, not in `logs/`, so the
  earned achievements of the app's own layout would not be found.
- The Mac release (`tools/package_mac.sh`, milestone M5): with `MAC_DEPS` the executable is built with `-ffile-prefix-map` (the repository's path and
  `MAC_DEPS`'s are made relative: no `__FILE__` or debug path names the build machine), the link takes `-Wl,-S` (no debug map: one entry per object
  file, each with this machine's path) and a second search path, `@executable_path/../Frameworks`, so that the one built executable finds its
  libraries beside it (`<out>/Frameworks`) and in an app (`Contents/Frameworks`) (`sdk/host/build.sh`); the tools `game_code_dylib.sh` builds
  get the same prefix map. `tools/mac/check_mac_deps.sh` reads an app's `Contents/Resources/compiler` and `licenses`.
