# Changes to the SDK

`sdk/` is a copy of `ports/fellowship/fotrxbox/sdk` from [Athelas](https://github.com/Shagg88/Athelas) by Shagg, commit
aefcfe2, itself a fork of [XboxXBERecomp](https://github.com/TGP482/XboxXBERecomp) by TGP482. Both are MIT licensed:
`sdk/LICENSE` and `sdk/ATHELAS-LICENSE`.

What this port changed:

- `sdk/xdk/4432/layout.toml`: new.
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
