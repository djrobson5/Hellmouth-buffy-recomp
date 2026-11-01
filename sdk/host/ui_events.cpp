// ImGui's window side for the in-game menu (app/overlay.cpp), in place of a backend: ImGui v1.91.0's SDL3 backend was
// written before SDL 3.2 and doesn't build against it, and the menu needs little. Its positions are in the swapchain's
// pixels (the renderer sets the display size), so the window's points are scaled to them. The controllers come by way of
// overlay.cpp's FeedPads.
#include <imgui.h>

#include <mutex>

#include "app/app.h"
#include "host.h"

namespace host {

namespace {

ImGuiKey Key(SDL_Scancode sc) {
    switch (sc) {
        case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
        case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
        case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
        case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
        case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
        case SDL_SCANCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
        case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
        case SDL_SCANCODE_ESCAPE: return ImGuiKey_Escape;
        case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
        case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
        case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
        case SDL_SCANCODE_HOME: return ImGuiKey_Home;
        case SDL_SCANCODE_END: return ImGuiKey_End;
        case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
        case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;
        default: return ImGuiKey_None;
    }
}

void MousePos(ImGuiIO& io, SDL_WindowID id, float x, float y) {
    SDL_Window* w = SDL_GetWindowFromID(id);
    const float density = w ? SDL_GetWindowPixelDensity(w) : 1.0f;
    io.AddMousePosEvent(x * density, y * density);
}

}  // namespace

void UiStart() {
    std::lock_guard lock(app::UiLock());
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    app::OverlayInit();
}

void UiEvent(const SDL_Event& e) {
    // releases always, so nothing stays down in ImGui after the menu closes; the rest only while it's open, as no frames
    // may be taking them otherwise
    const bool release = e.type == SDL_EVENT_KEY_UP || e.type == SDL_EVENT_MOUSE_BUTTON_UP;
    if (!app::g_ui_ready || (!app::OverlayOpen() && !release)) return;
    std::lock_guard lock(app::UiLock());
    ImGuiIO& io = ImGui::GetIO();
    switch (e.type) {
        case SDL_EVENT_MOUSE_MOTION:
            MousePos(io, e.motion.windowID, e.motion.x, e.motion.y);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            // the position too: the pointer may have moved while the menu was closed
            MousePos(io, e.button.windowID, e.button.x, e.button.y);
            if (e.button.button == SDL_BUTTON_LEFT) io.AddMouseButtonEvent(0, e.button.down);
            else if (e.button.button == SDL_BUTTON_RIGHT) io.AddMouseButtonEvent(1, e.button.down);
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            io.AddMouseWheelEvent(e.wheel.x, e.wheel.y);
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            if (e.key.scancode == SDL_SCANCODE_LSHIFT || e.key.scancode == SDL_SCANCODE_RSHIFT) io.AddKeyEvent(ImGuiMod_Shift, e.key.down);
            if (const ImGuiKey k = Key(e.key.scancode); k != ImGuiKey_None) io.AddKeyEvent(k, e.key.down);
            // Enter on a slider asks for a number
            if (e.key.down && ((e.key.key >= '0' && e.key.key <= '9') || e.key.key == '.' || e.key.key == '-')) io.AddInputCharacter(e.key.key);
            break;
        default:
            break;
    }
}

}  // namespace host
