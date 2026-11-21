// The recompiled game code when it is not in the executable: <data>\build\<exe>.dll, which the executable delay loads, so it
// must be loaded before the first call into it. Without XBE_GAME_CODE_DLL the code is linked in and there is nothing to do.
//
// The game's code is made from the player's own disc, so a release carries none of it. At the first start (and again when the
// DLL is missing or its stamp is not the one this executable expects) the tools beside the executable translate the game's
// xbe to C++, compile it and link it, behind a progress window (game_build.cpp has the steps; this file starts the programs and
// shows them). Adapted from XboxXBERecomp's runtime/app/game_code.cpp (MIT), for llvm-mingw and this port's layout.
#include "app/app.h"

#ifdef XBE_GAME_CODE_DLL
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>

#include "app/game_build.h"
#include "app/game_code_recipe.h"
#include "core/input.h"
#include "core/log.h"
#include "cpu/dispatch.h"
#include "kernel/disc.h"
#endif

namespace app {

#ifdef XBE_GAME_CODE_DLL
namespace {

namespace fs = std::filesystem;
namespace gb = gamebuild;

constexpr uint64_t kJobMemory = 384ull << 20;  // clang's peak on the biggest file is about 250 MB (measured on a Mac); room for more

std::wstring Wide(const std::string& utf8) {
    std::wstring w(size_t(MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0)), L'\0');
    if (!w.empty()) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), w.data(), int(w.size()));
    return w;
}

std::string Utf8(const std::wstring& w) {
    std::string s(size_t(WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr)), '\0');
    if (!s.empty()) WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), int(s.size()), nullptr, nullptr);
    return s;
}

std::string PathUtf8(const fs::path& p) { return Utf8(p.wstring()); }

gb::Layout MakeLayout() { return gb::Layout{ExeDir(), DataDir(), LogFolder()}; }

// compilers that run at once: one a logical processor, as many as the free memory holds (and at most what one wait can take)
size_t Slots() {
    MEMORYSTATUSEX mem{sizeof(mem)};
    GlobalMemoryStatusEx(&mem);
    size_t by_memory = size_t(mem.ullAvailPhys / kJobMemory);
    size_t by_cpu = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return std::clamp<size_t>(std::min(by_memory, by_cpu), 1, MAXIMUM_WAIT_OBJECTS - 4);
}

// a command line argument as CommandLineToArgvW and the C runtime read it back
std::wstring Quote(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') out.append(backslashes * 2 + 1, L'\\');
        else out.append(backslashes, L'\\');
        out.push_back(*it);
    }
    out.push_back(L'"');
    return out;
}

// starts and waits on the build's programs, with no window of their own, below the game's priority; closing the job, or this
// process ending, ends them all. Their output and the build's own notes go to logs\compile.log
class WinRunner : public gb::Runner {
public:
    // append: the log is added to, not started again (a start that only makes the pictures must not wipe the build's log)
    explicit WinRunner(const fs::path& log, bool append = false) {
        job_ = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (job_) SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limit, sizeof(limit));
        std::error_code ec;
        fs::create_directories(log.parent_path(), ec);
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        log_ = CreateFileW(log.c_str(), FILE_APPEND_DATA | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, append ? OPEN_ALWAYS : CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log_ == INVALID_HANDLE_VALUE) LOGE("game code: cannot write %s (%lu)", PathUtf8(log).c_str(), GetLastError());
        nul_ = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    ~WinRunner() override {
        KillAll();
        if (job_) CloseHandle(job_);
        if (log_ != INVALID_HANDLE_VALUE) CloseHandle(log_);
        if (nul_ != INVALID_HANDLE_VALUE) CloseHandle(nul_);
    }

    int Start(const fs::path& program, const std::vector<gb::Str>& args, const fs::path& cwd, std::string& why) override {
        std::wstring line = Quote(program.wstring());
        for (const std::wstring& a : args) line += L" " + Quote(a);
        STARTUPINFOW si{sizeof(si)};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nul_;
        si.hStdOutput = log_;
        si.hStdError = log_;
        PROCESS_INFORMATION pi{};
        DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS;
        if (!CreateProcessW(program.c_str(), line.data(), nullptr, nullptr, TRUE, flags, nullptr, cwd.c_str(), &si, &pi)) {
            why = "Windows error " + std::to_string(GetLastError());
            return 0;
        }
        if (job_ && !AssignProcessToJobObject(job_, pi.hProcess))
            Note("note: the program could not be put in the build's job (" + std::to_string(GetLastError()) + ")");
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        int id = next_++;
        processes_.push_back({id, pi.hProcess});
        return id;
    }

    Ended Wait(int ms) override {
        if (processes_.empty()) {
            Sleep(DWORD(ms));
            return {};
        }
        std::vector<HANDLE> handles;
        for (const Process& p : processes_) handles.push_back(p.handle);
        DWORD r = WaitForMultipleObjects(DWORD(handles.size()), handles.data(), FALSE, DWORD(ms));
        if (r == WAIT_FAILED) Sleep(DWORD(ms));  // not to spin on a wait that cannot be made
        if (r < WAIT_OBJECT_0 || r >= WAIT_OBJECT_0 + handles.size()) return {};
        Process done = processes_[r - WAIT_OBJECT_0];
        processes_.erase(processes_.begin() + (r - WAIT_OBJECT_0));
        DWORD code = 1;
        GetExitCodeProcess(done.handle, &code);
        CloseHandle(done.handle);
        return {done.id, code};
    }

    void KillAll() override {
        if (processes_.empty()) return;
        if (job_) TerminateJobObject(job_, 1);
        std::vector<HANDLE> handles;
        for (const Process& p : processes_) handles.push_back(p.handle);
        WaitForMultipleObjects(DWORD(handles.size()), handles.data(), TRUE, 5000);  // so that their files can be removed
        for (HANDLE h : handles) CloseHandle(h);
        processes_.clear();
    }

    void Note(const std::string& line) override {
        if (log_ == INVALID_HANDLE_VALUE) return;
        std::string text = line + "\r\n";
        DWORD written = 0;
        WriteFile(log_, text.data(), DWORD(text.size()), &written, nullptr);
    }

private:
    struct Process {
        int id;
        HANDLE handle;
    };
    HANDLE job_ = nullptr;
    HANDLE log_ = INVALID_HANDLE_VALUE;
    HANDLE nul_ = INVALID_HANDLE_VALUE;
    int next_ = 1;
    std::vector<Process> processes_;
};

// the player's disc gives Default.xbe (FindGame has read it, and checked it is this program's release) and final.map, which only
// the US disc has beside it
bool ReadDisc(const GameFiles& game, gb::Inputs& in, std::string& why) {
    in.xbe = game.xbe;
    if (const kernel::Disc* disc = kernel::MountedDisc()) {
        if (!disc->Load("final.map", in.map)) {
            why = "the disc image has no final.map, which the game's code is made from";
            return false;
        }
        return true;
    }
    std::ifstream f(game.dir / L"final.map", std::ios::binary);
    if (!f) {
        why = PathUtf8(game.dir) + " has no final.map, which the game's code is made from";
        return false;
    }
    in.map.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

// the two disc files the pictures are made from. A disc that lacks them still builds the game, so this fails apart from ReadDisc
bool ReadPictureFiles(const GameFiles& game, gb::PictureInputs& in, std::string& why) {
    struct File {
        const char* disc_path;  // the disc's, with backslashes
        const wchar_t* folder_path;
        std::vector<uint8_t>& into;
    };
    for (const File& file : {File{"interfc\\levelThumbnails.mtx", L"interfc/levelThumbnails.mtx", in.thumbnails},
                             File{"media\\launch_buffy.xpr", L"media/launch_buffy.xpr", in.launch}}) {
        if (const kernel::Disc* disc = kernel::MountedDisc()) {
            if (!disc->Load(file.disc_path, file.into)) {
                why = std::string("the disc image has no ") + file.disc_path;
                return false;
            }
            continue;
        }
        std::ifstream f(game.dir / file.folder_path, std::ios::binary);
        if (!f) {
            why = PathUtf8(game.dir) + " has no " + Utf8(file.folder_path);
            return false;
        }
        file.into.assign(std::istreambuf_iterator<char>(f), {});
    }
    return true;
}

fs::path CodeDll(const gb::Layout& layout) { return layout.Build() / IMAGE_LIBRARY; }

// the DLL there is, loaded by its full path before anything calls into it; false after telling the player why not
bool LoadCode(const fs::path& dll) {
    // the executable's name for this image (the DLL's imports name it)
    std::wstring exe_name = fs::path(IMAGE_LIBRARY).stem().wstring() + L".exe";
    for (auto& [xbe, exe] : kGame.images)
        if (_stricmp(xbe, IMAGE_XBE) == 0) exe_name = std::wstring(exe, exe + std::strlen(exe)) + L".exe";
    const std::string stamp = gamecode::Stamp();
    dispatch::LoadResult r = dispatch::Load(dll, exe_name, stamp.c_str());
    switch (r.status) {
        case dispatch::LoadStatus::Ok:
            break;
        case dispatch::LoadStatus::WrongExeName:
            Fail(kExitNoGameCode, L"This program must be named " + exe_name + L", because the game's code, " + dll.filename().wstring() +
                                      L", is built to work with a program of that name. Rename it back.");
        case dispatch::LoadStatus::NotLoaded:
            Fail(kExitNoGameCode, dll.filename().wstring() + L" would not load (Windows error " + std::to_wstring(r.error) +
                                      L"): it may be damaged, or made for another version of the game. Delete it and start the game to build it again.");
        case dispatch::LoadStatus::Stale:
            LOGE("Game code: %s says \"%s\"; this executable needs \"%s\"", dll.string().c_str(), r.dll_stamp, stamp.c_str());
            Fail(kExitStaleGameCode, dll.filename().wstring() + L" was built for another version of this program or of the game. Delete it and start the game to build it again.");
        case dispatch::LoadStatus::NoTables:
            Fail(kExitNoGameCode, dll.filename().wstring() + L" is not the game's code: it lacks the tables that describe it.");
    }
    LOGR("Game code: %s", dll.string().c_str());
    return true;
}

// ---- the window ------------------------------------------------------------------------------------------------------------

// the window's colours
constexpr COLORREF kBackground = RGB(10, 11, 10);
constexpr COLORREF kText = RGB(227, 232, 219);
constexpr COLORREF kDim = RGB(145, 153, 138);
constexpr COLORREF kBarEmpty = RGB(28, 33, 26);
constexpr COLORREF kBarFull = RGB(145, 158, 130);
constexpr UINT kBuildDone = WM_APP + 1;

gb::Progress g_progress;
HWND g_window = nullptr;
std::atomic<bool> g_ready{false};  // the build is done and the player has the choice
bool g_start = false;              // the player picked Start
bool g_delete_work = false;        // ticked on the ready page
uint64_t g_work_bytes = 0;
gb::Layout g_layout;

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::wstring StageText() {
    if (g_ready) return L"Start the game now?";
    return Wide(g_progress.Text());
}

// from the rate the files compiled so far went at, once every compiler has finished one
std::wstring TimeLeftText() {
    if (g_progress.stage != gb::Stage::Compiling) return {};
    uint64_t done = g_progress.bytes_done;
    if (g_progress.files_done < g_progress.first_wave || !done) return L"Working out the time left";
    double elapsed = double(NowMs() - g_progress.compile_start_ms);
    double left = elapsed * double(g_progress.bytes_total - done) / double(done);
    int minutes = int(left / 60000.0 + 0.5);
    if (minutes < 1) return L"Less than a minute left";
    return L"About " + std::to_wstring(minutes) + (minutes == 1 ? L" minute left" : L" minutes left");
}

float Fraction() { return g_ready ? 1.0f : float(g_progress.Fraction()); }

void DrawLine(HDC dc, RECT row, const std::wstring& text, COLORREF color, UINT align) {
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), int(text.size()), &row, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}

void FillBox(HDC dc, const RECT& box, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &box, brush);
    DeleteObject(brush);
}

// top to bottom: a heading, the stage, the bar, then the percentage and time left, or the buttons once ready, and a note
struct Layout {
    RECT heading, stage, bar, bottom, start, exit, delete_toggle, note;
};

Layout Arrange(const RECT& client, int unit) {
    Layout l;
    l.heading = {client.left + unit, client.top + unit, client.right - unit, client.top + unit * 3};
    l.stage = l.heading;
    OffsetRect(&l.stage, 0, unit * 2);
    l.bar = {l.stage.left, l.stage.bottom + unit / 2, l.stage.right, l.stage.bottom + unit / 2 + unit};
    l.bottom = {l.bar.left, l.bar.bottom + unit / 2, l.bar.right, l.bar.bottom + unit / 2 + unit * 2};
    l.exit = {l.bottom.right - unit * 8, l.bottom.top, l.bottom.right, l.bottom.bottom};
    l.start = {l.exit.left - unit * 9, l.bottom.top, l.exit.left - unit, l.bottom.bottom};
    l.delete_toggle = {l.bottom.left, l.bottom.top, l.start.left - unit, l.bottom.bottom};
    l.note = {l.bottom.left, l.bottom.bottom + unit / 2, l.bottom.right, l.bottom.bottom + unit / 2 + unit * 2};
    return l;
}

// the ready page's buttons, with the controller button that presses each
void PaintChoices(HDC dc, const Layout& l, int unit) {
    FillBox(dc, l.start, kBarFull);
    DrawLine(dc, l.start, L"Start (A)", kBackground, DT_CENTER);
    FillBox(dc, l.exit, kBarEmpty);
    DrawLine(dc, l.exit, L"Exit (B)", kText, DT_CENTER);
    int middle = (l.delete_toggle.top + l.delete_toggle.bottom) / 2;
    RECT box{l.delete_toggle.left, middle - unit / 2, l.delete_toggle.left + unit, middle + unit / 2};
    FillBox(dc, box, kBarEmpty);
    if (g_delete_work) {
        InflateRect(&box, -unit / 4, -unit / 4);
        FillBox(dc, box, kBarFull);
    }
    RECT label = l.delete_toggle;
    label.left += unit * 3 / 2;
    DrawLine(dc, label, L"Delete the build files, " + std::to_wstring(g_work_bytes >> 20) + L" MB (X)", kText, DT_LEFT);
    DrawLine(dc, l.note, L"Keeping them makes the next build, after an update, much faster.", kDim, DT_LEFT);
}

void Paint(HDC dc, const RECT& client, int unit) {
    Layout l = Arrange(client, unit);
    FillBox(dc, client, kBackground);
    SetBkMode(dc, TRANSPARENT);
    const wchar_t* heading = g_ready ? L"The game is ready." : L"Getting the game ready for this PC. This takes a few minutes.";
    DrawLine(dc, l.heading, heading, kText, DT_LEFT);
    DrawLine(dc, l.stage, StageText(), kDim, DT_LEFT);
    FillBox(dc, l.bar, kBarEmpty);
    RECT filled = l.bar;
    filled.right = l.bar.left + LONG(float(l.bar.right - l.bar.left) * Fraction());
    FillBox(dc, filled, kBarFull);
    if (g_ready) {
        PaintChoices(dc, l, unit);
        return;
    }
    DrawLine(dc, l.bottom, std::to_wstring(int(Fraction() * 100.0f)) + L"%", kDim, DT_LEFT);
    DrawLine(dc, l.bottom, TimeLeftText(), kDim, DT_RIGHT);
}

int Unit(HWND hwnd) { return MulDiv(10, int(GetDpiForWindow(hwnd)), 96); }

// painted off screen, then copied, so the bar does not flicker
void PaintWindow(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client;
    GetClientRect(hwnd, &client);
    HDC memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
    HGDIOBJ old_bitmap = SelectObject(memory, bitmap);
    HGDIOBJ old_font = SelectObject(memory, reinterpret_cast<HFONT>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)));
    Paint(memory, client, Unit(hwnd));
    BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
    SelectObject(memory, old_font);
    SelectObject(memory, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    EndPaint(hwnd, &ps);
}

// what the work folder holds, shown beside the delete box
uint64_t FolderBytes(const fs::path& dir) {
    uint64_t bytes = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2)) bytes += it->file_size(e2);
    }
    return bytes;
}

// the DLL is all the game needs; the work folder only makes a later build quicker
void DeleteWork(const gb::Layout& layout) {
    std::error_code ec;
    fs::remove_all(layout.Work(), ec);
    if (ec) LOGE("Game code: deleting the work folder failed: %s", ec.message().c_str());
    else LOGR("Game code: work folder deleted");
}

void CloseWindowWith(HWND hwnd, bool start) {
    g_start = start;
    if (g_ready && g_delete_work) DeleteWork(g_layout);
    DeleteObject(reinterpret_cast<HFONT>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)));
    DestroyWindow(hwnd);
}

// once ready: Start or Enter starts the game, Exit, Escape or the close button leaves, Space ticks the delete box
void OnClick(HWND hwnd, LPARAM lp) {
    if (!g_ready) return;
    RECT client;
    GetClientRect(hwnd, &client);
    Layout l = Arrange(client, Unit(hwnd));
    POINT at{short(LOWORD(lp)), short(HIWORD(lp))};
    if (PtInRect(&l.start, at)) CloseWindowWith(hwnd, true);
    else if (PtInRect(&l.exit, at)) CloseWindowWith(hwnd, false);
    else if (PtInRect(&l.delete_toggle, at)) g_delete_work = !g_delete_work;
}

// a controller's A starts, B leaves and X ticks the delete box, once ready; on the press, not the hold
void PollController(HWND hwnd) {
    static uint16_t held = 0;
    uint16_t now = input::HeldOnAnyController();
    uint16_t pressed = now & ~held;
    held = now;
    if (!g_ready) return;
    if (pressed & XINPUT_GAMEPAD_X) g_delete_work = !g_delete_work;
    if (pressed & XINPUT_GAMEPAD_A) CloseWindowWith(hwnd, true);
    else if (pressed & XINPUT_GAMEPAD_B) CloseWindowWith(hwnd, false);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT:
            PaintWindow(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_TIMER:
            InvalidateRect(hwnd, nullptr, FALSE);
            PollController(hwnd);
            return 0;
        case WM_LBUTTONUP:
            OnClick(hwnd, lp);
            return 0;
        case WM_KEYDOWN:
            if (g_ready && wp == VK_SPACE) g_delete_work = !g_delete_work;
            if (g_ready && (wp == VK_RETURN || wp == VK_ESCAPE)) CloseWindowWith(hwnd, wp == VK_RETURN);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_CLOSE: {
            if (g_ready) {
                CloseWindowWith(hwnd, false);
                return 0;
            }
            const wchar_t* question = L"Stop getting the game ready? What is done so far is kept, and the next start carries on.";
            if (MessageBoxW(hwnd, question, kGame.title, MB_YESNO | MB_ICONQUESTION) == IDYES) g_progress.cancel = true;
            return 0;
        }
        case kBuildDone:
            if (!wp) {
                CloseWindowWith(hwnd, false);  // failed or stopped; ready stays up for the player's pick
                return 0;
            }
            g_work_bytes = FolderBytes(g_layout.Work());
            FlashWindow(hwnd, TRUE);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// centred on the primary monitor, sized for its DPI, in the message font
HWND OpenWindow() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    std::wstring class_name = std::wstring(kGame.name) + L"GameCode";
    wc.lpszClassName = class_name.c_str();
    RegisterClassExW(&wc);
    UINT dpi = GetDpiForSystem();
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r{0, 0, MulDiv(560, int(dpi), 96), MulDiv(160, int(dpi), 96)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
    int w = r.right - r.left;
    int h = r.bottom - r.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, kGame.title, style, x, y, w, h, nullptr, nullptr, inst, nullptr);
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(CreateFontIndirectW(&metrics.lfMessageFont)));
    SetTimer(hwnd, 1, 50, nullptr);  // the controller is read with each repaint
    ShowWindow(hwnd, SW_SHOW);
    return hwnd;
}

// the player's text for a failure: the step, what happened, what to try, and the last lines of the log, where a tool's own error is
std::wstring FailureText(const gb::Layout& layout, const gb::Failure& failure) {
    std::wstring text = L"Getting the game ready failed.\n\n" + Wide(failure.step) + L": " + Wide(failure.reason);
    if (!failure.hint.empty()) text += L"\n\n" + Wide(failure.hint);
    if (std::string tail = gb::LogTail(layout.Log(), 1500); !tail.empty()) text += L"\n\nThe last lines of the log:\n\n" + Wide(tail);
    text += L"\n\nThe full details are in " + layout.Log().wstring() + L".";
    return text;
}

// a build on its own thread, the window on this one
enum class Result { Start, Exit, Cancelled, Failed };

Result BuildWithWindow(const GameFiles& game, const gb::Layout& layout, const gb::Recipe& recipe) {
    if (std::string blocker = gb::Blocker(layout, recipe); !blocker.empty()) {
        LOGE("Game code: %s", blocker.c_str());
        MessageBoxW(nullptr, Wide(blocker).c_str(), kGame.title, MB_ICONERROR);
        return Result::Failed;
    }
    g_layout = layout;
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);  // no sleep while it works
    input::Init();
    g_window = OpenWindow();
    WinRunner runner(layout.Log());
    gb::Failure failure;
    gb::Outcome outcome = gb::Outcome::Failed;
    std::thread worker([&] {
        outcome = gb::Build(
            layout, recipe, [&](gb::Inputs& in, std::string& why) { return ReadDisc(game, in, why); },
            [&](gb::PictureInputs& in, std::string& why) { return ReadPictureFiles(game, in, why); }, runner, g_progress, failure);
        g_ready = outcome == gb::Outcome::Built;
        PostMessageW(g_window, kBuildDone, outcome == gb::Outcome::Built, 0);
    });
    MSG msg;
    while (IsWindow(g_window) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_progress.cancel = true;  // a window closed some other way
    worker.join();
    SetThreadExecutionState(ES_CONTINUOUS);
    if (outcome == gb::Outcome::Failed) {
        LOGE("Game code: %s", failure.Line().c_str());
        MessageBoxW(nullptr, FailureText(layout, failure).c_str(), kGame.title, MB_ICONERROR);
        return Result::Failed;
    }
    if (outcome == gb::Outcome::Cancelled) return Result::Cancelled;
    return g_start ? Result::Start : Result::Exit;
}

// one build at a time: another copy started meanwhile (or a launcher's --prepare) waits for it, then finds the DLL current
class BuildLock {
public:
    BuildLock() {
        std::wstring name = std::wstring(kGame.name) + L"-GameCode";
        lock_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (lock_) WaitForSingleObject(lock_, INFINITE);
    }
    ~BuildLock() {
        if (!lock_) return;
        ReleaseMutex(lock_);
        CloseHandle(lock_);
    }

private:
    HANDLE lock_ = nullptr;
};

gb::DllState State(const gb::Layout& layout, const gb::Recipe& recipe) {
    std::string found;
    gb::DllState state = gb::CheckDll(CodeDll(layout), recipe.stamp, &found);
    static const char* const kNames[] = {"missing", "current", "stale", "unreadable"};
    if (state == gb::DllState::Current) LOGR("Game code: %s is current", PathUtf8(CodeDll(layout)).c_str());
    else LOGR("Game code: %s is %s%s%s", PathUtf8(CodeDll(layout)).c_str(), kNames[int(state)], found.empty() ? "" : ": ", found.c_str());
    return state;
}

// The achievement pictures and the icon, made when the marker says they are not: on a start whose DLL is current (the build makes them
// otherwise). No window: it takes a moment. Never fatal; what went wrong is in logs\compile.log, which is added to.
void EnsurePictures(const GameFiles& game, const gb::Layout& layout, const gb::Recipe& recipe) {
    if (gb::PicturesCurrent(layout, recipe)) return;
    BuildLock lock;  // a copy started at the same time is making them
    if (gb::PicturesCurrent(layout, recipe)) return;
    WinRunner runner(layout.Log(), true);
    gb::MakePictures(layout, recipe, [&](gb::PictureInputs& in, std::string& why) { return ReadPictureFiles(game, in, why); }, runner);
}

}  // namespace

bool PrepareGameCode(const GameFiles& game) {
    const gb::Layout layout = MakeLayout();
    const gb::Recipe recipe = gamecode::Recipe(Slots());
    gb::DllState state = State(layout, recipe);
    const bool quiet = Args().relaunched || Args().no_dialogs;
    if (state == gb::DllState::Current) {
        if (!quiet) EnsurePictures(game, layout, recipe);
        return LoadCode(CodeDll(layout));
    }

    // asked for with no window and no questions (a launcher, or the game restarting for a level), the DLL has to be there: the
    // launcher builds it first (--prepare), and a restart is not the time to start a build
    if (quiet) {
        std::wstring dll = Wide(IMAGE_LIBRARY);
        if (state == gb::DllState::Missing)
            Fail(kExitNoGameCode, L"The game's code, " + dll + L", has not been built yet in " + layout.Build().wstring() +
                                      L". Start the program on its own to build it, or run it with --prepare.");
        Fail(kExitStaleGameCode, L"The game's code, " + dll + L", was built for another version of this program or of the game, and has to be "
                                 L"built again. Start the program on its own to do that, or run it with --prepare.");
    }

    BuildLock lock;
    const bool built_here = State(layout, recipe) != gb::DllState::Current;  // else a copy started earlier built it while this one waited
    if (built_here) {
        switch (BuildWithWindow(game, layout, recipe)) {
            case Result::Start:
                break;
            case Result::Exit:
                LOGR("Exit: the player chose Exit after the game's code was built");
                logx::Flush();
                ExitProcess(kExitOk);
            case Result::Cancelled:
                LOGR("Exit: the player stopped the build of the game's code");
                logx::Flush();
                ExitProcess(kExitOk);
            case Result::Failed:
                return false;
        }
    }
    // this process's build made the pictures, or tried and logged why not (and tries again at the next start): only a DLL that a copy started
    // earlier built has not had the step looked at here
    if (!built_here) EnsurePictures(game, layout, recipe);
    return LoadCode(CodeDll(layout));
}

// --prepare: the same build with no window, for a launcher. progress lines on stdout, the reason on stderr
int PrepareOnly(const GameFiles& game) {
    const gb::Layout layout = MakeLayout();
    const gb::Recipe recipe = gamecode::Recipe(Slots());
    BuildLock lock;
    auto clean = [&] {
        if (Args().clean_work) DeleteWork(layout);
    };
    if (State(layout, recipe) == gb::DllState::Current) {
        if (!gb::PicturesCurrent(layout, recipe)) PrintOut("progress 0.000 Making the achievement pictures and the icon");
        EnsurePictures(game, layout, recipe);  // the DLL being current says nothing of the pictures
        PrintOut("progress 1.000 The game's code is up to date");
        clean();
        return kExitOk;
    }
    WinRunner runner(layout.Log());  // a fresh compile.log: every reason below is in it as well as on stderr
    auto failed = [&](const std::string& line, bool in_log) {
        LOGE("Game code: %s", line.c_str());
        logx::Flush();
        if (!in_log) runner.Note("FAILED: " + line);
        PrintErr("error: " + line);
        return kExitBuildFailed;
    };
    if (std::string blocker = gb::Blocker(layout, recipe); !blocker.empty()) return failed(blocker, false);
    gb::Progress progress;
    int last_per_mille = -1;
    std::string last_text;
    auto report = [&] {
        std::string text = progress.Text();
        int per_mille = int(progress.Fraction() * 1000.0);
        if (per_mille == last_per_mille && text == last_text) return;
        last_per_mille = per_mille;
        last_text = text;
        char fraction[16];
        std::snprintf(fraction, sizeof fraction, "%.3f", progress.Fraction());
        PrintOut(std::string("progress ") + fraction + " " + text);
    };
    progress.changed = report;
    report();
    gb::Failure failure;
    gb::Outcome outcome = gb::Build(
        layout, recipe, [&](gb::Inputs& in, std::string& why) { return ReadDisc(game, in, why); },
        [&](gb::PictureInputs& in, std::string& why) { return ReadPictureFiles(game, in, why); }, runner, progress, failure);
    if (outcome != gb::Outcome::Built) {
        std::string line = outcome == gb::Outcome::Cancelled ? std::string("stopped") : failure.Line();
        if (!failure.hint.empty()) line += " " + failure.hint;
        return failed(line + " (details: " + PathUtf8(layout.Log()) + ")", true);  // the build noted its own failure
    }
    PrintOut("progress 1.000 Done");
    LOGR("Game code: built %s", PathUtf8(CodeDll(layout)).c_str());
    clean();
    return kExitOk;
}
#else
bool PrepareGameCode(const GameFiles&) { return true; }
int PrepareOnly(const GameFiles&) { return kExitOk; }  // the code is in the executable: nothing to build
#endif

}  // namespace app
