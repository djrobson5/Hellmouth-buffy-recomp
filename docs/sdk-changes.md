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
