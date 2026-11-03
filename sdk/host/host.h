#pragma once
#include <SDL3/SDL.h>

#include <string>
#include <vector>

// the parts of the SDL host (main.cpp) that live apart
namespace host {

// the picture from the CPU: each frame the reference renderer draws (preview.cpp). Window thread.
void PreviewStart(SDL_Window* window);
void PreviewDraw(bool again);  // the newest frame if one has come, or again the one showing

// SCRIPT in the environment, as the harness's: an SDL virtual controller presses its buttons, so the run goes through
// the same code as a real one (script_pad.cpp). Window thread.
void ScriptStart();
void ScriptTick();

// ImGui's window side, for the in-game menu (ui_events.cpp). Window thread.
void UiStart();
void UiEvent(const SDL_Event& e);

// the frame rate in the corner of the window (fps_overlay.mm). Window thread.
bool FpsOverlayShown();
void FpsOverlayShow(void* metal_layer, bool show);  // nothing without a Metal layer, as with the picture from the CPU
void FpsOverlayUpdate(double fps);

// a level restart's window (window_mac.mm). Window thread.
void WindowSeen(SDL_Window* window, bool seen);  // there and drawing either way; also opens without macOS's animation
void YieldActivation(int pid);                   // the next instance may come forward
void Activate();
void ActivateOther(int pid);       // the old instance, before it goes
void TakeKeys(SDL_Window* window);  // each frame: the window is key once the app is in front
void TakeFullScreenControls(SDL_Window* window, void (*toggle)());  // the green button and the menu's full screen
void HidePointer(SDL_Window* window, bool want);  // hidden while over the picture and the app is in front
int OpenInstance(const std::string& exe, const std::vector<std::string>& args);  // any thread; its pid, -1 outside an app

}  // namespace host
