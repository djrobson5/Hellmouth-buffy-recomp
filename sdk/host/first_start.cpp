// The first start on a Mac, for a build whose game code is a dylib (XBE_GAME_CODE_DLL): the counterpart of sdk/runtime/app/game_code.cpp.
//
// The game's code is made from the player's own disc, so a release carries none of it. At the first start (and again when
// <data>/build/Buffy.dylib is missing or its stamp is not the one this executable expects) the tools in the compiler folder translate
// the game's xbe to C++, compile it and link it (sdk/runtime/app/game_build.cpp has the steps, the same code the Windows first start runs),
// behind a progress window; the achievement pictures and the icon are made from the disc in the same way, whatever state the code is in.
// This file starts the programs (gamebuild::PosixRunner), reads the disc, and shows what the build is doing; main.cpp then loads the
// dylib (its LoadGameCode). With --no-dialogs, or in a restart for a level, nothing is built and no window is made: a code that isn't
// current ends the run (kExitNoGameCode, kExitStaleGameCode), as on Windows.
#include <fcntl.h>
#include <mach/mach.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <thread>

#include <SDL3/SDL.h>

#include "app/app.h"
#include "app/game_build.h"
#include "app/game_code_recipe.h"
#include "app/posix_runner.h"
#include "core/log.h"
#include "host.h"
#include "kernel/disc.h"

namespace app {

void PrintOut(const std::string& line) {
    std::fputs((line + "\n").c_str(), stdout);
    std::fflush(stdout);  // a launcher's pipe is block buffered, and the run may end in _Exit
}
void PrintErr(const std::string& line) {
    std::fputs((line + "\n").c_str(), stderr);
    std::fflush(stderr);
}

namespace {

namespace fs = std::filesystem;
namespace gb = gamebuild;

constexpr uint64_t kJobMemory = 384ull << 20;  // clang's peak on the biggest file is about 250 MB (measured on a Mac); room for more

std::wstring Wide(const std::string& utf8) { return fs::path(utf8).wstring(); }
std::string Narrow(const std::wstring& w) { return fs::path(w).string(); }

gb::Layout MakeLayout() { return gb::Layout{AppFolder(), DataDir(), LogFolder()}; }

// the programs of a build that is running (gamebuild::PosixRunner): where a later build finds the ones a killed build left
fs::path ChildrenFile(const gb::Layout& layout) { return layout.data / "cache" / "game-code.children"; }

// compilers that run at once: one a logical processor, as many as the free memory holds
size_t Slots() {
    size_t by_cpu = size_t(std::max<long>(1, sysconf(_SC_NPROCESSORS_ONLN)));
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) != KERN_SUCCESS) return by_cpu;
    const uint64_t available = (uint64_t(vm.free_count) + vm.inactive_count) * uint64_t(vm_page_size);
    return std::clamp<size_t>(std::min<size_t>(size_t(available / kJobMemory), by_cpu), 1, by_cpu);
}

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
    std::ifstream f(game.dir / "final.map", std::ios::binary);
    if (!f) {
        why = game.dir.string() + " has no final.map, which the game's code is made from";
        return false;
    }
    in.map.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

// the two disc files the pictures are made from. A disc that lacks them still builds the game, so this fails apart from ReadDisc
bool ReadPictureFiles(const GameFiles& game, gb::PictureInputs& in, std::string& why) {
    struct File {
        const char* disc_path;  // the disc's, with backslashes
        const char* folder_path;
        std::vector<uint8_t>& into;
    };
    for (const File& file : {File{"interfc\\levelThumbnails.mtx", "interfc/levelThumbnails.mtx", in.thumbnails},
                             File{"media\\launch_buffy.xpr", "media/launch_buffy.xpr", in.launch}}) {
        if (const kernel::Disc* disc = kernel::MountedDisc()) {
            if (!disc->Load(file.disc_path, file.into)) {
                why = std::string("the disc image has no ") + file.disc_path;
                return false;
            }
            continue;
        }
        std::ifstream f(game.dir / file.folder_path, std::ios::binary);
        if (!f) {
            why = game.dir.string() + " has no " + file.folder_path;
            return false;
        }
        file.into.assign(std::istreambuf_iterator<char>(f), {});
    }
    return true;
}

fs::path CodeDylib(const gb::Layout& layout, const gb::Recipe& recipe) { return layout.Build() / recipe.library; }

// ---- the window --------------------------------------------------------------------------------------------------------------

gb::Progress g_progress;  // the build's, for the signal handler too

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// from the rate the files compiled so far went at, once every compiler has finished one
std::string TimeLeftText() {
    if (g_progress.stage != gb::Stage::Compiling) return {};
    uint64_t done = g_progress.bytes_done;
    if (g_progress.files_done < g_progress.first_wave || !done) return "Working out the time left";
    double elapsed = double(NowMs() - g_progress.compile_start_ms);
    double left = elapsed * double(g_progress.bytes_total - done) / double(done);
    int minutes = int(left / 60000.0 + 0.5);
    if (minutes < 1) return "Less than a minute left";
    return "About " + std::to_string(minutes) + (minutes == 1 ? " minute left" : " minutes left");
}

// the player's text for a failure: the step, what happened, what to try, and the last lines of the log, where a tool's own error is
std::string FailureText(const gb::Layout& layout, const gb::Failure& failure) {
    std::string text = "Getting the game ready failed.\n\n" + failure.step + ": " + failure.reason;
    if (!failure.hint.empty()) text += "\n\n" + failure.hint;
    if (std::string tail = gb::LogTail(layout.Log(), 1500); !tail.empty()) text += "\n\nThe last lines of the log:\n\n" + tail;
    text += "\n\nThe full details are in " + layout.Log().string() + ".";
    return text;
}

void ShowMessage(const std::string& text) {
    static const std::string title = Narrow(kGame.title);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title.c_str(), text.c_str(), nullptr);
}

// A stop from outside (SIGTERM, SIGINT: a launcher's Stop, a timeout, Ctrl-C, the system logging out) cancels the build, which ends the compilers.
// Only while a build runs: the game that starts afterwards has the handlers it had. Under --prepare the run then ends with kExitBuildFailed
// and "stopped" like any other failure; with the window, as the player's own Stop does.
void OnStop(int) { g_progress.cancel = true; }

class StopSignals {
public:
    StopSignals() : term_(std::signal(SIGTERM, OnStop)), int_(std::signal(SIGINT, OnStop)) {}
    ~StopSignals() {
        if (term_ != SIG_ERR) std::signal(SIGTERM, term_);
        if (int_ != SIG_ERR) std::signal(SIGINT, int_);
    }
    StopSignals(const StopSignals&) = delete;
    StopSignals& operator=(const StopSignals&) = delete;

private:
    void (*term_)(int);
    void (*int_)(int);
};

// a build on its own thread, the window on this one
enum class Result { Start, Cancelled, Failed };

Result BuildWithWindow(const GameFiles& game, const gb::Layout& layout, const gb::Recipe& recipe) {
    if (std::string blocker = gb::Blocker(layout, recipe); !blocker.empty()) {
        LOGE("Game code: %s", blocker.c_str());
        ShowMessage(blocker);
        return Result::Failed;
    }
    const std::string title = Narrow(kGame.title);
    const bool shown = host::ProgressOpen(title);
    if (!shown) LOGW("first start: the progress window could not be opened, the build goes on without one");
    gb::PosixRunner runner(layout.Log(), false, ChildrenFile(layout));  // (the caller holds the build lock: orphans of a killed build end here)
    const StopSignals stop_on_signal;
    gb::Failure failure;
    gb::Outcome outcome = gb::Outcome::Failed;
    std::atomic<bool> finished{false};
    std::thread worker([&] {
        outcome = gb::Build(
            layout, recipe, [&](gb::Inputs& in, std::string& why) { return ReadDisc(game, in, why); },
            [&](gb::PictureInputs& in, std::string& why) { return ReadPictureFiles(game, in, why); }, runner, g_progress, failure);
        finished = true;
    });
    while (!finished) {
        // SDL takes the application's events (the window's close button, Command-Q) on this thread
        for (SDL_Event e; SDL_PollEvent(&e);)
            if (e.type == SDL_EVENT_QUIT) g_progress.cancel = true;
        if (host::ProgressCloseRequested() &&
            host::ProgressAskStop(title, "Stop getting the game ready? What is done so far is kept, and the next start carries on."))
            g_progress.cancel = true;
        host::ProgressShow("Getting the game ready for this Mac. This takes a few minutes.", g_progress.Text(), g_progress.Fraction(),
                           TimeLeftText());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    worker.join();
    host::ProgressClose();
    if (outcome == gb::Outcome::Failed) {
        LOGE("Game code: %s", failure.Line().c_str());
        ShowMessage(FailureText(layout, failure));
        return Result::Failed;
    }
    return outcome == gb::Outcome::Cancelled ? Result::Cancelled : Result::Start;
}

// one build at a time: another copy started meanwhile (or a launcher's --prepare) waits for it, then finds the dylib current. flock locks the
// open file description, so a second BuildLock over the same file in this process would wait for the first for ever: whoever holds one
// passes the fact on (EnsurePictures' `locked`), never opens another. (main.cpp's Fail names the same file.)
class BuildLock {
public:
    explicit BuildLock(const fs::path& data) {
        std::error_code ec;
        fs::create_directories(data / "cache", ec);
        const std::string path = (data / "cache" / "game-code.lock").string();
        fd_ = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd_ < 0) {
            LOGW("Game code: the lock file %s could not be opened (%s): another copy's build is not waited for", path.c_str(), std::strerror(errno));
            return;
        }
        int locked;
        while ((locked = flock(fd_, LOCK_EX)) != 0 && errno == EINTR) {
        }
        if (locked != 0) LOGW("Game code: the lock %s could not be taken (%s): another copy's build is not waited for", path.c_str(), std::strerror(errno));
    }
    ~BuildLock() {
        if (fd_ >= 0) close(fd_);  // the lock goes with it
    }

private:
    int fd_ = -1;
};

gb::DllState State(const gb::Layout& layout, const gb::Recipe& recipe) {
    std::string found;
    gb::DllState state = gb::CheckDll(CodeDylib(layout, recipe), recipe.stamp, &found, recipe.target);
    static const char* const kNames[] = {"missing", "current", "stale", "unreadable"};
    const std::string path = CodeDylib(layout, recipe).string();
    if (state == gb::DllState::Current) LOGR("Game code: %s is current", path.c_str());
    else LOGR("Game code: %s is %s%s%s", path.c_str(), kNames[int(state)], found.empty() ? "" : ": ", found.c_str());
    return state;
}

// The achievement pictures and the icon, made when the marker says they are not: on a start whose dylib is current (the build makes them
// otherwise). No window: it takes a moment. Never fatal; what went wrong is in logs/compile.log, which is added to. `locked`: the caller holds the
// build lock already (it is not taken again, see BuildLock).
void EnsurePictures(const GameFiles& game, const gb::Layout& layout, const gb::Recipe& recipe, bool locked) {
    if (gb::PicturesCurrent(layout, recipe)) return;
    std::optional<BuildLock> lock;
    if (!locked) lock.emplace(layout.data);  // a copy started at the same time is making them
    if (gb::PicturesCurrent(layout, recipe)) return;
    gb::PosixRunner runner(layout.Log(), true, ChildrenFile(layout));
    gb::MakePictures(layout, recipe, [&](gb::PictureInputs& in, std::string& why) { return ReadPictureFiles(game, in, why); }, runner);
}

// the dylib is all the game needs; the work folder only makes a later build quicker
void DeleteWork(const gb::Layout& layout) {
    std::error_code ec;
    fs::remove_all(layout.Work(), ec);
    if (ec) LOGE("Game code: deleting the work folder failed: %s", ec.message().c_str());
    else LOGR("Game code: work folder deleted");
}

}  // namespace

bool PrepareGameCode(const GameFiles& game) {
    const gb::Layout layout = MakeLayout();
    const gb::Recipe recipe = gamecode::Recipe(Slots());
    gb::DllState state = State(layout, recipe);
    const bool quiet = Args().relaunched || Args().no_dialogs;
    if (state == gb::DllState::Current) {
        if (!quiet) EnsurePictures(game, layout, recipe, false);
        return true;
    }

    // asked for with no window and no questions (a launcher, or the game restarting for a level), the dylib has to be there: the
    // launcher builds it first (--prepare), and a restart is not the time to start a build
    if (quiet) {
        const std::wstring dylib = Wide(recipe.library);
        if (state == gb::DllState::Missing)
            Fail(kExitNoGameCode, L"The game's code, " + dylib + L", has not been built yet in " + layout.Build().wstring() +
                                      L". Start the program on its own to build it, or run it with --prepare.");
        Fail(kExitStaleGameCode, L"The game's code, " + dylib + L", was built for another version of this program or of the game, and has to be "
                                 L"built again. Start the program on its own to do that, or run it with --prepare.");
    }

    BuildLock lock(layout.data);
    const bool built_here = State(layout, recipe) != gb::DllState::Current;  // else a copy started earlier built it while this one waited
    if (built_here) {
        switch (BuildWithWindow(game, layout, recipe)) {
            case Result::Start:
                break;
            case Result::Cancelled:
                LOGR("Exit: the player stopped the build of the game's code");
                logx::Flush();
                std::_Exit(kExitOk);
            case Result::Failed:
                return false;
        }
    }
    // this process's build made the pictures, or tried and logged why not (and tries again at the next start): only a dylib that a copy started
    // earlier built has not had the step looked at here
    if (!built_here) EnsurePictures(game, layout, recipe, true);
    return true;
}

// --prepare: the same build with no window, for a launcher. progress lines on stdout, the reason on stderr
int PrepareOnly(const GameFiles& game) {
    std::signal(SIGPIPE, SIG_IGN);  // a launcher that has closed the pipe: the writes fail, the build is not killed by them
    const gb::Layout layout = MakeLayout();
    const gb::Recipe recipe = gamecode::Recipe(Slots());
    BuildLock lock(layout.data);
    auto clean = [&] {
        if (Args().clean_work) DeleteWork(layout);
    };
    if (State(layout, recipe) == gb::DllState::Current) {
        if (!gb::PicturesCurrent(layout, recipe)) PrintOut("progress 0.000 Making the achievement pictures and the icon");
        EnsurePictures(game, layout, recipe, true);  // the dylib being current says nothing of the pictures
        PrintOut("progress 1.000 The game's code is up to date");
        clean();
        return kExitOk;
    }
    gb::PosixRunner runner(layout.Log(), false, ChildrenFile(layout));  // a fresh compile.log: every reason below is in it as well as on stderr
    auto failed = [&](const std::string& line, bool in_log) {
        LOGE("Game code: %s", line.c_str());
        logx::Flush();
        if (!in_log) runner.Note("FAILED: " + line);
        PrintErr("error: " + line);
        return kExitBuildFailed;
    };
    if (std::string blocker = gb::Blocker(layout, recipe); !blocker.empty()) return failed(blocker, false);
    int last_per_mille = -1;
    std::string last_text;
    auto report = [&] {
        std::string text = g_progress.Text();
        int per_mille = int(g_progress.Fraction() * 1000.0);
        if (per_mille == last_per_mille && text == last_text) return;
        last_per_mille = per_mille;
        last_text = text;
        char fraction[16];
        std::snprintf(fraction, sizeof fraction, "%.3f", g_progress.Fraction());
        PrintOut(std::string("progress ") + fraction + " " + text);
    };
    g_progress.changed = report;
    const StopSignals stop_on_signal;
    report();
    gb::Failure failure;
    gb::Outcome outcome = gb::Build(
        layout, recipe, [&](gb::Inputs& in, std::string& why) { return ReadDisc(game, in, why); },
        [&](gb::PictureInputs& in, std::string& why) { return ReadPictureFiles(game, in, why); }, runner, g_progress, failure);
    if (outcome != gb::Outcome::Built) {
        std::string line = outcome == gb::Outcome::Cancelled ? std::string("stopped") : failure.Line();
        if (!failure.hint.empty()) line += " " + failure.hint;
        return failed(line + " (details: " + layout.Log().string() + ")", true);  // the build noted its own failure
    }
    PrintOut("progress 1.000 Done");
    LOGR("Game code: built %s", CodeDylib(layout, recipe).string().c_str());
    clean();
    return kExitOk;
}

}  // namespace app
