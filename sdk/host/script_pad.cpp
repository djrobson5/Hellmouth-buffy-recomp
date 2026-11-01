// SCRIPT in the environment: lines of "frame buttons [frames held]" as the harness's pad script (held 8 frames by
// default, # comments, buttons joined by +). The buttons press an SDL virtual controller, and KEY:<SDL scancode name>
// (KEY:Return) a key by way of SDL's event queue, so a run without anyone at the controls goes through the same code as
// a player's. SIZE:<w>x<h> resizes the window, FULL:<0 or 1> leaves or enters full screen and
// CLICK:<x>x<y> clicks the left mouse button at a place in the window's points, and SUB:<text> shows a subtitle for five
// seconds, once at the line's frame.
// Frames start again after a restart, as the script does.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "host.h"

namespace host {

namespace {

struct Press {
    double at;
    std::string buttons;
    double len;
};

std::vector<Press> g_script;
SDL_Joystick* g_pad = nullptr;
bool g_buttons[SDL_GAMEPAD_BUTTON_COUNT] = {};
int g_axes[SDL_GAMEPAD_AXIS_COUNT] = {};
std::vector<std::string> g_keys;  // down now
std::vector<std::string> g_window_done;

struct Want {
    bool buttons[SDL_GAMEPAD_BUTTON_COUNT] = {};
    int axes[SDL_GAMEPAD_AXIS_COUNT] = {};
    std::vector<std::string> keys, window;
};

void Apply(const std::string& s, Want& w) {
    size_t at = 0;
    while (at <= s.size()) {
        size_t e = s.find('+', at);
        if (e == std::string::npos) e = s.size();
        std::string b = s.substr(at, e - at);
        at = e + 1;
        if (b.rfind("KEY:", 0) == 0) w.keys.push_back(b.substr(4));
        else if (b.rfind("SIZE:", 0) == 0 || b.rfind("FULL:", 0) == 0 || b.rfind("CLICK:", 0) == 0 || b.rfind("SUB:", 0) == 0) w.window.push_back(b);
        else if (b == "UP") w.buttons[SDL_GAMEPAD_BUTTON_DPAD_UP] = true;
        else if (b == "DOWN") w.buttons[SDL_GAMEPAD_BUTTON_DPAD_DOWN] = true;
        else if (b == "LEFT") w.buttons[SDL_GAMEPAD_BUTTON_DPAD_LEFT] = true;
        else if (b == "RIGHT") w.buttons[SDL_GAMEPAD_BUTTON_DPAD_RIGHT] = true;
        else if (b == "START") w.buttons[SDL_GAMEPAD_BUTTON_START] = true;
        else if (b == "LSB") w.buttons[SDL_GAMEPAD_BUTTON_LEFT_STICK] = true;
        else if (b == "RSB") w.buttons[SDL_GAMEPAD_BUTTON_RIGHT_STICK] = true;
        else if (b == "BACK") w.buttons[SDL_GAMEPAD_BUTTON_BACK] = true;
        else if (b == "A") w.buttons[SDL_GAMEPAD_BUTTON_SOUTH] = true;
        else if (b == "B") w.buttons[SDL_GAMEPAD_BUTTON_EAST] = true;
        else if (b == "X") w.buttons[SDL_GAMEPAD_BUTTON_WEST] = true;
        else if (b == "Y") w.buttons[SDL_GAMEPAD_BUTTON_NORTH] = true;
        else if (b == "BLACK") w.buttons[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] = true;
        else if (b == "WHITE") w.buttons[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = true;
        else if (b == "LT") w.axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = 32767;
        else if (b == "RT") w.axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = 32767;
        // SDL's sticks have y down
        else if (b == "LS") w.axes[SDL_GAMEPAD_AXIS_LEFTY] = -32768;
        else if (b == "LSD") w.axes[SDL_GAMEPAD_AXIS_LEFTY] = 32767;
        else if (b == "LSL") w.axes[SDL_GAMEPAD_AXIS_LEFTX] = -32768;
        else if (b == "LSR") w.axes[SDL_GAMEPAD_AXIS_LEFTX] = 32767;
        else if (b == "RSU") w.axes[SDL_GAMEPAD_AXIS_RIGHTY] = -32768;
        else if (b == "RSD") w.axes[SDL_GAMEPAD_AXIS_RIGHTY] = 32767;
        else if (b == "RSL") w.axes[SDL_GAMEPAD_AXIS_RIGHTX] = -32768;
        else if (b == "RSR") w.axes[SDL_GAMEPAD_AXIS_RIGHTX] = 32767;
    }
}

void PushKey(const std::string& name, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = SDL_GetScancodeFromName(name.c_str());
    e.key.down = down;
    SDL_PushEvent(&e);
}

}  // namespace

void ScriptStart() {
    const char* path = std::getenv("SCRIPT");
    FILE* f = path ? std::fopen(path, "r") : nullptr;
    if (!f) return;
    char line[160];
    while (std::fgets(line, sizeof line, f)) {
        char b[64];
        double t, len = 8;
        if (line[0] != '#' && std::sscanf(line, "%lf %63s %lf", &t, b, &len) >= 2) g_script.push_back({t, b, len});
    }
    std::fclose(f);
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "Script pad";
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    g_pad = id ? SDL_OpenJoystick(id) : nullptr;
    if (!g_pad) LOGE("script: no virtual controller: %s", SDL_GetError());
    LOGR("script: %s, %zu presses", path, g_script.size());
}

void ScriptTick() {
    if (g_script.empty()) return;
    // frames, which stop while the game stands still (the menu): 60 a second go on being counted then
    static uint64_t last_ms = SDL_GetTicks();
    static double held = 0;
    const uint64_t now_ms = SDL_GetTicks();
    if (app::Held()) held += double(now_ms - last_ms) * 60.0 / 1000.0;
    last_ms = now_ms;
    double t = double(gpu::g_swaps.load()) + held;
    Want w;
    for (const Press& s : g_script)
        if (t >= s.at && t <= s.at + s.len) Apply(s.buttons, w);
    if (g_pad) {
        for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
            if (w.buttons[i] != g_buttons[i]) SDL_SetJoystickVirtualButton(g_pad, i, g_buttons[i] = w.buttons[i]);
        for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; i++)
            if (w.axes[i] != g_axes[i]) SDL_SetJoystickVirtualAxis(g_pad, i, int16_t(g_axes[i] = w.axes[i]));
    }
    for (const std::string& k : w.keys)
        if (std::find(g_keys.begin(), g_keys.end(), k) == g_keys.end()) PushKey(k, true);
    for (const std::string& k : g_keys)
        if (std::find(w.keys.begin(), w.keys.end(), k) == w.keys.end()) PushKey(k, false);
    g_keys = std::move(w.keys);
    for (const std::string& c : w.window) {
        if (std::find(g_window_done.begin(), g_window_done.end(), c) != g_window_done.end()) continue;
        g_window_done.push_back(c);
        SDL_Window* win = SDL_GetWindows(nullptr)[0];
        int width = 0, height = 0;
        if (std::sscanf(c.c_str(), "SIZE:%dx%d", &width, &height) == 2) SDL_SetWindowSize(win, width, height);
        else if (float cx, cy; std::sscanf(c.c_str(), "CLICK:%fx%f", &cx, &cy) == 2) {
            SDL_Event m{};
            m.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
            m.button.windowID = SDL_GetWindowID(win);
            m.button.button = SDL_BUTTON_LEFT;
            m.button.down = true;
            m.button.x = cx;
            m.button.y = cy;
            SDL_PushEvent(&m);
            m.type = SDL_EVENT_MOUSE_BUTTON_UP;
            m.button.down = false;
            SDL_PushEvent(&m);
        }
        else if (c.rfind("SUB:", 0) == 0) app::Subtitle(c.substr(4), 5);
        else if (c == "FULL:1") SDL_SetWindowFullscreen(win, true);
        else if (c == "FULL:0") SDL_SetWindowFullscreen(win, false);
        LOGR("script: window %s", c.c_str());
    }
}

}  // namespace host
