// the host outside Windows, through SDL: window, events, files beside the exe; app/main.cpp's part. All of SDL's events
// are taken here on the first thread, which macOS asks of a window, and which is also what brings the controllers' state
// up to date. The menu is overlay.cpp's, given its window side by ui_events.cpp. No splash or handover yet.
#include <mach-o/dyld.h>
#include <crt_externs.h>
#include <fcntl.h>
#include <pwd.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <thread>

#include "app/app.h"
#include "core/input.h"
#include "core/keys.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/texture_pack.h"
#include "harness.h"
#include "host.h"
#include "image_symbols.h"
#include "kernel/kernel.h"

namespace app {

namespace {

SDL_Window* g_window = nullptr;
void* g_metal_layer = nullptr;

std::string Narrow(const std::wstring& s) { return std::filesystem::path(s).string(); }

// A level restart replaces this process while its window is in a full-screen Space. macOS is still closing that Space when
// the new process asks for full screen, and drops the request: SDL says it went through and no event ever comes. So it's
// asked again until the window gets there, which took about a second. Once it has, leaving full screen is the player's.
uint64_t g_full_screen_until = 0, g_full_screen_next = 0;

// window thread
void RetryFullScreen() {
    if (!g_full_screen_until) return;
    const uint64_t now = SDL_GetTicks();
    const bool there = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0;
    if (there || now >= g_full_screen_until) {
        if (!there) LOGW("window: full screen wasn't entered");
        g_full_screen_until = 0;
    } else if (now >= g_full_screen_next) {
        g_full_screen_next = now + 500;
        SDL_SetWindowFullscreen(g_window, true);
    }
}

// Full screen entered or left by the player (the green button, Control-Command-F) rather than by the setting. The
// environment carries it over the level restarts, as the window it was done to doesn't outlive the process. It's never
// saved: the next session starts as the setting says.
constexpr const char* kFullScreenEnv = "BUFFY_FULL_SCREEN";

// the window to the settings, or to full screen or not when `full` is 0 or 1; window thread
void ApplyNow(int full = -1) {
    const Settings& s = Get();
    if (full < 0) full = s.display_mode != DisplayMode::Windowed;
    if (!full) {
        g_full_screen_until = 0;
        SDL_SetWindowFullscreen(g_window, false);
        Mode out = Output();
        SDL_SetWindowSize(g_window, int(out.width), int(out.height));
        SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    } else {
        // macOS's own full screen, at the desktop's mode: [display] width and height don't switch modes here
        SDL_SetWindowFullscreen(g_window, true);
        g_full_screen_next = SDL_GetTicks() + 1000;  // a request that's working takes about 0.6 s
        g_full_screen_until = g_full_screen_next + 5000;
    }
}

}  // namespace

std::filesystem::path ExeDir() {
    static const std::filesystem::path dir = [] {
        char p[4096];
        uint32_t size = sizeof p;
        return _NSGetExecutablePath(p, &size) == 0 ? std::filesystem::weakly_canonical(p).parent_path()
                                                   : std::filesystem::current_path();
    }();
    return dir;
}

namespace {

// running from inside a Mac app (sdk/host/bundle.sh): nothing is written beside the executable, and the player is asked
// for the disc image
bool InApp() {
    static const bool in = [] {
        const std::filesystem::path contents = ExeDir().parent_path();
        return ExeDir().filename() == "MacOS" && contents.filename() == "Contents" &&
               contents.parent_path().extension() == ".app";
    }();
    return in;
}

// the app's settings, hard disk and logs
std::filesystem::path AppData() {
    const char* home = std::getenv("HOME");
    if (!home || !*home)
        if (const passwd* user = getpwuid(getuid())) home = user->pw_dir;
    return std::filesystem::path(home ? home : "/tmp") / "Library" / "Application Support" / "Buffy";
}

CommandLine g_args;
bool g_print_title_id = false;  // --title-id, which only bundle.sh asks for

// A crash closes the log with "Exit (signal N)", the way every other end closes it: a launcher reading the log can then
// tell it from a force quit, which can write nothing. Only async-signal-safe calls in here.
int g_crash_log = -1;

void OnCrash(int sig) {
    char line[32] = "Exit (signal ";
    size_t n = std::strlen(line);
    if (sig >= 10) line[n++] = char('0' + sig / 10);
    line[n++] = char('0' + sig % 10);
    line[n++] = ')';
    line[n++] = '\n';
    (void)!write(g_crash_log, line, n);
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

void CloseLogOnCrash() {
    g_crash_log = open(logx::Path().c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
    if (g_crash_log < 0) return;
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) std::signal(sig, OnCrash);
}

// A build outside an app has nothing for macOS to take an icon from, and the Dock would show a bare executable's. SDL's
// window icon is the app's on this platform. bundle.sh gives the app its own.
void SetDevelopmentIcon(SDL_Window* window) {
    gpu::pack::Image image;
    if (InApp() || !gpu::pack::ReadPng(ExeDir() / "icon.png", image)) return;
    if (SDL_Surface* surface = SDL_CreateSurfaceFrom(int(image.width), int(image.height), SDL_PIXELFORMAT_RGBA32,
                                                     image.levels[0].data(), int(image.width) * 4)) {
        SDL_SetWindowIcon(window, surface);
        SDL_DestroySurface(surface);
    }
}

}  // namespace

// the same options as on Windows (app.h); an argument without dashes is the game
const CommandLine& Args() {
    static const bool parsed = [] {
        CommandLine& c = g_args;
        int n = *_NSGetArgc();
        char** v = *_NSGetArgv();
        for (int i = 1; i < n; i++) {
            std::string a = v[i];
            auto next = [&]() -> std::filesystem::path { return i + 1 < n ? std::filesystem::path(v[++i]) : std::filesystem::path(); };
            if (a == "--game") c.game = next();
            else if (a == "--config") c.config = next();
            else if (a == "--textures") c.textures = next();
            else if (a == "--log") c.log = next();
            else if (a == "--saves") c.saves = next();
            else if (a == "--shader-cache") c.shader_cache = next();
            else if (a == "--screenshots") c.screenshots = next();
            else if (a == "--instance") c.instance = std::max(0, std::min(8, std::atoi(next().c_str())));
            else if (a == "--no-dialogs") c.no_dialogs = true;
            else if (a == "--any-release") c.any_release = true;
            else if (a == "--version") c.version = true;
            else if (a == "--title-id") g_print_title_id = true;
            else if (a == "--describe-settings") c.describe = true;
            else if (a.rfind("--", 0) != 0 && a.rfind("-psn_", 0) != 0) c.game = a;
        }
        if (InApp()) {
            const std::filesystem::path data = AppData();
            auto fill = [](std::filesystem::path& p, const std::filesystem::path& value) {
                if (p.empty()) p = value;
            };
            fill(c.config, data / kGame.settings);
            fill(c.textures, data / "textures");
            fill(c.log, data / "logs" / (std::string(kGame.log) + ".log"));
            fill(c.saves, data / "hdd");
            fill(c.shader_cache, data / "shadercache");
            fill(c.screenshots, data / "Screenshots");
        }
        std::error_code ec;
        for (auto* p : {&c.game, &c.config, &c.textures, &c.log, &c.saves, &c.shader_cache, &c.screenshots})
            if (!p->empty()) *p = std::filesystem::absolute(*p, ec);
        return true;
    }();
    (void)parsed;
    return g_args;
}

int Instance() { return Args().instance; }
std::filesystem::path TextureDir() { return Args().textures.empty() ? ExeDir() / "textures" : Args().textures; }
std::filesystem::path LogFolder() { return Args().log.empty() ? ExeDir() / "logs" : Args().log.parent_path(); }
std::filesystem::path SavesDir() {
    std::filesystem::path base = Args().saves.empty() ? ExeDir() / "hdd" : Args().saves;
    if (Instance() > 1) base += std::to_string(Instance());
    return base;
}
std::filesystem::path ScreenshotDir() { return Args().screenshots.empty() ? ExeDir() / "Screenshots" : Args().screenshots; }
std::filesystem::path ShaderCacheDir() { return Args().shader_cache.empty() ? ExeDir() / "shadercache" : Args().shader_cache; }

void Fail(int code, const std::wstring& message) {
    std::string text = Narrow(message);
    LOGE("Exit (%d): %s", code, text.c_str());
    logx::Flush();
    // SDL's message boxes are the first thread's, and guest threads fail too: this waits for it to show one
    if (!Args().no_dialogs)
        SDL_RunOnMainThread(
            [](void* p) {
                static const std::string title = Narrow(kGame.short_title);
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title.c_str(), static_cast<const std::string*>(p)->c_str(), g_window);
            },
            &text, true);
    std::_Exit(code);
}

void* Window() { return g_window; }
void* MetalLayer() { return g_metal_layer; }
std::atomic<uint32_t> g_pixel_height{0};

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

void ApplyDisplay() {
    SDL_RunOnMainThread([](void*) { ApplyNow(); }, nullptr, false);
}

Mode Desktop() {
    static const Mode m = [] {
        const SDL_DisplayMode* d = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
        return d ? Mode{uint32_t(d->w), uint32_t(d->h)} : Mode{1280, 720};
    }();
    return m;
}
const std::vector<std::string>& ScreenNames() {
    static const std::vector<std::string> n{"main"};
    return n;
}
void* ScreenMonitor() { return nullptr; }

// no splash, and a restart opens a new window: nothing outlives the process that a handover could keep showing
bool OpenSplash() { return false; }
void SplashFirstFrame() {}
void CloseSplash() {}
void OpenGameWindow(bool) {}
bool StartHandover() { return false; }
bool HandingOver() { return false; }
void FinishHandover() {}

// the console's 60 frames a second unless [display] sets a cap; FPS=N in the environment instead, 0 for none. The
// preview has no display to wait for.
void Pace() {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    static const char* env = std::getenv("FPS");
    uint32_t cap = env ? uint32_t(std::atoi(env)) : g_high_frame_rate ? 120 : Get().fps_cap ? Get().fps_cap : 60;
    auto now = clock::now();
    if (!cap) {
        next = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / cap);
    next += period;
    if (next < now - period) next = now;
    std::this_thread::sleep_until(next);
}

}  // namespace app

namespace {

// the virtual-key code keys::kKeys names a key by
uint32_t Vk(SDL_Scancode sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return 'A' + (sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return '1' + (sc - SDL_SCANCODE_1);
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return 0x70 + (sc - SDL_SCANCODE_F1);
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return 0x61 + (sc - SDL_SCANCODE_KP_1);
    switch (sc) {
        case SDL_SCANCODE_0: return '0';
        case SDL_SCANCODE_SPACE: return 0x20;
        case SDL_SCANCODE_RETURN: return 0x0D;
        case SDL_SCANCODE_ESCAPE: return 0x1B;
        case SDL_SCANCODE_TAB: return 0x09;
        case SDL_SCANCODE_BACKSPACE: return 0x08;
        case SDL_SCANCODE_CAPSLOCK: return 0x14;
        case SDL_SCANCODE_LSHIFT: return 0xA0;
        case SDL_SCANCODE_RSHIFT: return 0xA1;
        case SDL_SCANCODE_LCTRL: return 0xA2;
        case SDL_SCANCODE_RCTRL: return 0xA3;
        case SDL_SCANCODE_LALT: return 0xA4;
        case SDL_SCANCODE_RALT: return 0xA5;
        case SDL_SCANCODE_UP: return 0x26;
        case SDL_SCANCODE_DOWN: return 0x28;
        case SDL_SCANCODE_LEFT: return 0x25;
        case SDL_SCANCODE_RIGHT: return 0x27;
        case SDL_SCANCODE_INSERT: return 0x2D;
        case SDL_SCANCODE_DELETE: return 0x2E;
        case SDL_SCANCODE_HOME: return 0x24;
        case SDL_SCANCODE_END: return 0x23;
        case SDL_SCANCODE_PAGEUP: return 0x21;
        case SDL_SCANCODE_PAGEDOWN: return 0x22;
        case SDL_SCANCODE_KP_0: return 0x60;
        case SDL_SCANCODE_KP_MULTIPLY: return 0x6A;
        case SDL_SCANCODE_KP_PLUS: return 0x6B;
        case SDL_SCANCODE_KP_MINUS: return 0x6D;
        case SDL_SCANCODE_KP_PERIOD: return 0x6E;
        case SDL_SCANCODE_KP_DIVIDE: return 0x6F;
        case SDL_SCANCODE_MINUS: return 0xBD;
        case SDL_SCANCODE_EQUALS: return 0xBB;
        case SDL_SCANCODE_COMMA: return 0xBC;
        case SDL_SCANCODE_PERIOD: return 0xBE;
        case SDL_SCANCODE_SLASH: return 0xBF;
        case SDL_SCANCODE_SEMICOLON: return 0xBA;
        case SDL_SCANCODE_APOSTROPHE: return 0xDE;
        case SDL_SCANCODE_GRAVE: return 0xC0;
        default: return 256;  // none
    }
}

uint32_t MouseVk(uint8_t button) {
    switch (button) {
        case SDL_BUTTON_LEFT: return 0x01;
        case SDL_BUTTON_RIGHT: return 0x02;
        case SDL_BUTTON_MIDDLE: return 0x04;
        case SDL_BUTTON_X1: return 0x05;
        case SDL_BUTTON_X2: return 0x06;
        default: return 256;
    }
}

// F6 or the menu. The environment carries it over the level restarts; the setting is changed so that the menu shows the same,
// and only the menu's closing saves it.
void SetHighFrameRate(bool high) {
    app::g_high_frame_rate = high;
    setenv("BUFFY_HIGH_FRAME_RATE", high ? "1" : "0", 1);
    for (const cfg::Desc& d : app::SettingsList())
        if (d.flag == &app::Get().high_frame_rate) *d.flag = high;
    LOGR("frame rate: %d", high ? 120 : 60);
}

// the menu changed widescreen: the window takes the picture's new shape at the same height, in the same place. Full screen
// stays as it is, with bars or without.
void FitWindowToShape(bool wide) {
    if (SDL_GetWindowFlags(app::g_window) & SDL_WINDOW_FULLSCREEN) return;
    int x = 0, y = 0, w = 0, h = 0;
    SDL_GetWindowPosition(app::g_window, &x, &y);
    SDL_GetWindowSize(app::g_window, &w, &h);
    const int width = wide ? h * 16 / 9 : h * 4 / 3;
    SDL_SetWindowSize(app::g_window, width, h);
    SDL_SetWindowPosition(app::g_window, x + (w - width) / 2, y);
}

void Event(const SDL_Event& e, bool& redraw) {
    host::UiEvent(e);
    switch (e.type) {
        case SDL_EVENT_QUIT:
            kernel::Shutdown(0);
            break;
        case SDL_EVENT_KEY_DOWN:
            if (e.key.repeat) break;
            if (e.key.scancode == SDL_SCANCODE_F6) {
                SetHighFrameRate(!app::g_high_frame_rate);
                break;
            }
            if (e.key.scancode == SDL_SCANCODE_F7) {
                // for this session, as F6
                host::FpsOverlayShow(app::MetalLayer(), !host::FpsOverlayShown());
                setenv("BUFFY_FPS_OVERLAY", host::FpsOverlayShown() ? "1" : "0", 1);
                LOGR("frame rate overlay: %s", host::FpsOverlayShown() ? "on" : "off");
                break;
            }
            if (e.key.scancode == SDL_SCANCODE_F10) {
                app::OverlayToggle();
                break;
            }
            if (e.key.scancode == SDL_SCANCODE_BACKSLASH) {
                gpu::g_screenshot = true;
                break;
            }
            // F8: texture packs off and on, to compare (Windows's F7 pair isn't here: F7 is the frame rate on a Mac)
            if (e.key.scancode == SDL_SCANCODE_F8) {
                gpu::g_pack_off = !gpu::g_pack_off;
                LOGR("texture packs %s (F8)", gpu::g_pack_off ? "off" : "on");
                break;
            }
            // ]: the next colour look; [: sharpening off and on, to compare
            if (e.key.scancode == SDL_SCANCODE_RIGHTBRACKET) {
                static const char* const kNames[] = {"original", "film", "vivid", "dark fantasy"};
                const uint32_t n = (gpu::g_colour_look + 1) % gpu::kColourLooks;
                gpu::g_colour_look = n;
                LOGR("colour look %s (])", kNames[n]);
                break;
            }
            if (e.key.scancode == SDL_SCANCODE_LEFTBRACKET && app::Get().sharpening) {
                gpu::g_sharpen_off = !gpu::g_sharpen_off;
                LOGR("sharpening %s ([)", gpu::g_sharpen_off ? "off" : "on");
                break;
            }
            // before the menu sees it: closing keeps keys that are down from the game until they're let go
            keys::Held(Vk(e.key.scancode), true);
            if (e.key.scancode == SDL_SCANCODE_ESCAPE && app::OverlayEscape()) break;
            keys::Latch(Vk(e.key.scancode));
            break;
        case SDL_EVENT_KEY_UP:
            keys::Held(Vk(e.key.scancode), false);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            keys::Held(MouseVk(e.button.button), true);
            keys::Latch(MouseVk(e.button.button));
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            keys::Held(MouseVk(e.button.button), false);
            break;
        case SDL_EVENT_MOUSE_MOTION:
        {
            // SDL gives fractions of a count: kept for the next event, or a slow trackpad reads as no movement at all
            static float rest_x = 0, rest_y = 0;
            rest_x += e.motion.xrel;
            rest_y += e.motion.yrel;
            int32_t dx = int32_t(rest_x), dy = int32_t(rest_y);
            rest_x -= float(dx);
            rest_y -= float(dy);
            keys::MouseMoved(dx, dy);
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL:
            keys::Wheel(e.wheel.integer_y);
            break;
        case SDL_EVENT_WINDOW_MOUSE_ENTER:
            keys::PointerOver(true);
            break;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            keys::PointerOver(false);
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            app::SetFocus(true);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            app::SetFocus(false);
            break;
        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
            setenv(app::kFullScreenEnv, e.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ? "1" : "0", 1);
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            app::g_pixel_height = uint32_t(e.window.data2);
            redraw = true;
            break;
        case SDL_EVENT_WINDOW_EXPOSED:
        case SDL_EVENT_WINDOW_RESIZED:
            redraw = true;
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            input::PadEvent(true, e.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            input::PadEvent(false, e.gdevice.which);
            break;
        default:
            break;
    }
}

// The app's disc image: only a path that FindGame has accepted is kept, so a wrong choice is asked about again next time
// instead of failing at every start. The file is also how a level restart finds it, as the arguments it carries over are
// the ones the app started with.
std::filesystem::path KeptGameFile() { return app::AppData() / "disc_image.txt"; }

std::filesystem::path TakeKeptGame() {
    std::string path;
    std::ifstream f(KeptGameFile());
    std::getline(f, path);
    std::error_code ec;
    std::filesystem::remove(KeptGameFile(), ec);
    return std::filesystem::is_regular_file(path, ec) ? std::filesystem::path(path) : std::filesystem::path();
}

void KeepGame(const std::filesystem::path& path) {
    std::ofstream f(KeptGameFile());
    f << path.string() << '\n';
    if (!f) LOGW("disc image: could not write %s", KeptGameFile().string().c_str());
}

// asks the player, empty if they cancel. SDL calls back from its panel on this thread, or after it, as events are taken.
std::filesystem::path PickGame() {
    struct Pick {
        std::atomic<bool> done{false};
        std::string path;
    };
    static Pick pick;
    static const SDL_DialogFileFilter filter{"Xbox disc image", "iso"};
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, const_cast<SDL_DialogFileFilter*>(&filter));
    SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, 1);
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Choose your Buffy the Vampire Slayer disc image");
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, "Play");
    SDL_ShowFileDialogWithProperties(
        SDL_FILEDIALOG_OPENFILE,
        [](void*, const char* const* files, int) {
            if (files && files[0]) pick.path = files[0];
            else if (!files) LOGW("disc image: the file panel failed: %s", SDL_GetError());
            pick.done = true;
        },
        nullptr, props);
    SDL_DestroyProperties(props);
    while (!pick.done) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 50) && e.type == SDL_EVENT_QUIT) break;
    }
    return pick.path;
}

}  // namespace

int main() {
    using namespace app;
    if (Args().version) {
        std::printf("%s\n", kGame.version);
        return kExitOk;
    }
    if (g_print_title_id) {  // for bundle.sh: the app's bundle ID is made from it
        std::printf("%08x\n", kGame.title_id);
        return kExitOk;
    }
    if (Args().describe) {  // bundle.sh keeps this in the app for a launcher: only what this host acts on
        std::vector<cfg::Desc> list;
        for (const cfg::Desc& d : SettingsList())
            if (!LauncherFields(d).empty()) list.push_back(d);
        std::printf("%s", cfg::Describe(list, kGame.presets, LauncherFields).c_str());
        return kExitOk;
    }
    std::error_code data_error;
    if (InApp()) std::filesystem::create_directories(AppData(), data_error);
    // one log a session: a restart into the level carries on its predecessor's
    bool relaunched = std::getenv("XBE_RELAUNCHED") != nullptr;
    std::string log = kGame.log;
    if (Instance() > 1) log += "-" + std::to_string(Instance());
    if (!Args().log.empty()) log = Args().log.stem().string();
    logx::Init(LogFolder().string(), log, relaunched);
    CloseLogOnCrash();

    SDL_SetHint(SDL_HINT_APP_NAME, "Buffy the Vampire Slayer");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO))
        Fail(kExitNoD3D12, L"The window couldn't be opened: " + std::filesystem::path(SDL_GetError()).wstring());
    int v = SDL_GetVersion();
    LOGR("SDL %d.%d.%d, %s", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v), SDL_GetCurrentVideoDriver());

    Load();
    const char* high_frame_rate = std::getenv("BUFFY_HIGH_FRAME_RATE");
    if (high_frame_rate) SetHighFrameRate(high_frame_rate[0] == '1');
    else g_high_frame_rate = Get().high_frame_rate;
    ach::Start();
    if (kGame.init) kGame.init();
    const bool keep_game = InApp() && Args().game.empty();
    if (keep_game) {
        g_args.game = TakeKeptGame();
        if (g_args.game.empty()) g_args.game = PickGame();
        if (g_args.game.empty()) {
            LOGR("Exit: no disc image chosen");
            logx::Flush();
            return kExitOk;
        }
    }
    if (Args().game.empty()) Fail(kExitNoGame, kGame.missing);
    GameFiles game;
    if (!FindGame(game)) return kExitNoGame;
    if (keep_game) KeepGame(Args().game);

    std::string title = std::filesystem::path(kGame.title).string();
    if (Instance()) title += " [" + std::to_string(Instance()) + "]";
    Mode out = Output();
    g_window = SDL_CreateWindow(title.c_str(), int(out.width), int(out.height),
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_METAL | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!g_window) Fail(kExitNoD3D12, L"The window couldn't be opened: " + std::filesystem::path(SDL_GetError()).wstring());
    SetDevelopmentIcon(g_window);
    const char* full_screen = std::getenv(kFullScreenEnv);
    if (full_screen) LOGR("window: full screen %s, as before the restart", full_screen[0] == '1' ? "on" : "off");
    ApplyNow(full_screen ? full_screen[0] == '1' : -1);
    SDL_ShowWindow(g_window);
    SDL_RaiseWindow(g_window);
    int pixel_w = 0, pixel_h = 0;
    SDL_GetWindowSizeInPixels(g_window, &pixel_w, &pixel_h);
    g_pixel_height = uint32_t(pixel_h);
    LOGR("window: %dx%d, %dx%d pixels", int(out.width), int(out.height), pixel_w, pixel_h);
    // the Metal layer for Vulkan is made here as macOS wants it made on this thread. The picture from the CPU starts if
    // that renderer is the one chosen or Vulkan can't start, from the title's thread.
    if (Get().renderer == 1)
        if (SDL_MetalView view = SDL_Metal_CreateView(g_window)) g_metal_layer = SDL_Metal_GetLayer(view);
    const char* fps_overlay = std::getenv("BUFFY_FPS_OVERLAY");
    if (fps_overlay ? fps_overlay[0] == '1' : Get().show_fps) host::FpsOverlayShow(g_metal_layer, true);
    if (g_metal_layer) host::UiStart();
    harness::g_show_start = [] {
        SDL_RunOnMainThread([](void*) { host::PreviewStart(g_window); }, nullptr, true);
    };

    input::Init();
    host::ScriptStart();
    if (kGame.window) kGame.window(g_window);
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), SavesDir().string()))
        Fail(1, L"The game failed to start, see the log in the logs folder.");

    // [display] show_fps: the counter in the corner, or in the window's title for the picture from the CPU, which has no
    // Metal layer to put it on. FPS_LOG in the environment logs them too.
    bool show_fps = Get().show_fps;
    bool widescreen = Get().widescreen;
    static const bool fps_log = std::getenv("FPS_LOG") != nullptr;
    uint64_t swaps = 0;
    uint64_t flushed = SDL_GetTicks();
    for (;;) {
        bool redraw = false;
        SDL_Event e;
        // a short wait: this is also what reads the controllers
        if (SDL_WaitEventTimeout(&e, 2)) {
            Event(e, redraw);
            while (SDL_PollEvent(&e)) Event(e, redraw);
        }
        host::ScriptTick();
        RetryFullScreen();
        host::PreviewDraw(redraw);
        if (Get().show_fps != show_fps) {  // changed in the menu
            show_fps = Get().show_fps;
            host::FpsOverlayShow(g_metal_layer, show_fps);
            setenv("BUFFY_FPS_OVERLAY", show_fps ? "1" : "0", 1);
        }
        if (Get().high_frame_rate != g_high_frame_rate) SetHighFrameRate(Get().high_frame_rate);
        if (Get().widescreen != widescreen) {
            widescreen = Get().widescreen;
            FitWindowToShape(widescreen);
        }
        if (uint64_t now = SDL_GetTicks(); now - flushed >= 1000) {
            double fps = double(gpu::g_swaps.load() - swaps) * 1000.0 / double(now - flushed);
            swaps = gpu::g_swaps.load();
            flushed = now;
            if (Get().show_fps && !g_metal_layer) SDL_SetWindowTitle(g_window, (title + " (" + std::to_string(int(fps + 0.5)) + " fps)").c_str());
            host::FpsOverlayUpdate(fps);
            if (fps_log) LOGR("fps: %.1f", fps);
            logx::Flush();
        }
    }
}
