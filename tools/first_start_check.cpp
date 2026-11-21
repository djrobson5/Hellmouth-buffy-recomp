// The first start's own build code (sdk/runtime/app/game_build.cpp: the steps, the object reuse, the progress, the stamp check)
// run on this Mac over a folder laid out as the release zip is, with this Mac's programs standing in for the Windows ones under
// the names the zip gives them (compiler/clang.exe is this Mac's clang, and so on). Only starting programs differs from the
// Windows executable: a POSIX runner here, CreateProcess there. It reads the disc image through kernel::Disc, as the executable does.
// tools/sim_first_start.sh builds it and drives it.
//
//   first_start_check --app <folder with compiler/> --data <data folder> --game <disc image or folder with Default.xbe>
//       [--slots N] [--stop-after N] [--abi <token>] [--check] [--print-stamp]
//   --check        only say whether the DLL is current (exit 0 current, 1 not)
//   --stop-after   cancel after N files are compiled (a closed window)
//   --abi          pretend the executable has another ABI token (an update)
//   --print-stamp  print the stamp the executable expects, as built from the headers here, and exit
// Prints what --prepare prints ("progress <0 to 1> <text>" lines) on stdout and "error: <step>: <reason>" on stderr.
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>

#include "app/game_build.h"
#include "app/game_code_recipe.h"
#include "kernel/disc.h"
#include "xbe/image_hash.h"

extern char** environ;

namespace {

namespace fs = std::filesystem;

class PosixRunner : public gamebuild::Runner {
public:
    explicit PosixRunner(const fs::path& log) {
        std::error_code ec;
        fs::create_directories(log.parent_path(), ec);
        fd_ = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    }
    ~PosixRunner() override {
        KillAll();
        if (fd_ >= 0) close(fd_);
    }
    int Start(const fs::path& program, const std::vector<gamebuild::Str>& args, const fs::path& cwd, std::string& why) override {
        std::vector<std::string> storage{program.string()};
        for (const std::string& a : args) storage.push_back(a);
        std::vector<char*> argv;
        for (std::string& s : storage) argv.push_back(s.data());
        argv.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addchdir_np(&actions, cwd.c_str());
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_adddup2(&actions, fd_, 1);
        posix_spawn_file_actions_adddup2(&actions, fd_, 2);
        pid_t pid = 0;
        int error = posix_spawn(&pid, program.c_str(), &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        if (error) {
            why = std::strerror(error);
            return 0;
        }
        int id = next_++;
        running_[pid] = id;
        return id;
    }
    Ended Wait(int ms) override {
        for (int waited = 0;; waited += 5) {
            int status = 0;
            pid_t pid = waitpid(-1, &status, WNOHANG);
            if (pid > 0) {
                auto it = running_.find(pid);
                if (it == running_.end()) continue;
                Ended e;
                e.id = it->second;
                e.code = WIFEXITED(status) ? uint32_t(WEXITSTATUS(status)) : 128u + uint32_t(WTERMSIG(status));
                running_.erase(it);
                return e;
            }
            if (waited >= ms || running_.empty()) {
                if (running_.empty() && waited < ms) usleep(unsigned(ms - waited) * 1000);
                return {};
            }
            usleep(5000);
        }
    }
    void KillAll() override {
        for (auto& [pid, id] : running_) {
            kill(pid, SIGKILL);
            int status = 0;
            waitpid(pid, &status, 0);
        }
        running_.clear();
    }
    void Note(const std::string& line) override {
        std::string text = line + "\n";
        if (fd_ >= 0) (void)!write(fd_, text.data(), text.size());
    }

private:
    int fd_ = -1;
    int next_ = 1;
    std::map<pid_t, int> running_;
};

// what FindGame does with the disc: the xbe must be this program's release; the disc then gives final.map
bool ReadDisc(const fs::path& game, gamebuild::Inputs& in, std::string& why) {
    std::error_code ec;
    std::unique_ptr<kernel::Disc> disc;
    if (fs::is_directory(game, ec)) {
        auto read = [&](const char* name, std::vector<uint8_t>& out) {
            std::ifstream f(game / name, std::ios::binary);
            out.assign(std::istreambuf_iterator<char>(f), {});
            return bool(f);
        };
        if (!read(IMAGE_XBE, in.xbe) || !read("final.map", in.map)) {
            why = "the folder has no " IMAGE_XBE " and final.map";
            return false;
        }
    } else {
        disc = std::make_unique<kernel::Disc>();
        if (!disc->Open(game, why)) return false;
        if (!disc->Load(IMAGE_XBE, in.xbe)) {
            why = "the disc has no " IMAGE_XBE;
            return false;
        }
        if (!disc->Load("final.map", in.map)) {
            why = "the disc has no final.map";
            return false;
        }
    }
    if (xbe::SectionsHash(in.xbe) != IMAGE_SECTIONS_HASH) {
        why = "that is not the release this program was made for";
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    fs::path app, data, game;
    size_t slots = 4;
    long stop_after = -1;
    bool check_only = false, print_stamp = false;
    std::string abi;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--app") app = next();
        else if (a == "--data") data = next();
        else if (a == "--game") game = next();
        else if (a == "--slots") slots = size_t(std::atol(next().c_str()));
        else if (a == "--stop-after") stop_after = std::atol(next().c_str());
        else if (a == "--abi") abi = next();
        else if (a == "--check") check_only = true;
        else if (a == "--print-stamp") print_stamp = true;
    }
    if (print_stamp) {
        std::printf("%s\n", gamecode::Stamp().c_str());
        return 0;
    }
    if (app.empty() || data.empty() || game.empty()) {
        std::fprintf(stderr, "usage: first_start_check --app <folder> --data <folder> --game <disc image> [--slots N] [--stop-after N] [--abi token] [--check]\n");
        return 2;
    }
    gamebuild::Layout layout{fs::absolute(app), fs::absolute(data), fs::absolute(data) / "logs"};
    gamebuild::Recipe recipe = gamecode::Recipe(slots);
    if (!abi.empty()) {  // an update that changed the ABI: the stamp and the compile's token say so
        auto swap = [&](std::string& s) {
            size_t at = s.find(gamecode::AbiToken());
            while (at != std::string::npos) {
                s.replace(at, std::strlen(gamecode::AbiToken()), abi);
                at = s.find(gamecode::AbiToken(), at + abi.size());
            }
        };
        swap(recipe.stamp);
        swap(recipe.compile_flags);
    }
    fs::path dll = layout.Build() / recipe.library;
    std::string found;
    gamebuild::DllState state = gamebuild::CheckDll(dll, recipe.stamp, &found);
    const char* names[] = {"missing", "current", "stale", "unreadable"};
    std::printf("dll: %s (%s)\n", names[int(state)], found.c_str());
    if (state == gamebuild::DllState::Current) {
        std::printf("progress 1.000 The game's code is up to date\n");
        return 0;
    }
    if (check_only) return 1;
    if (std::string blocker = gamebuild::Blocker(layout, recipe); !blocker.empty()) {
        std::fprintf(stderr, "error: %s\n", blocker.c_str());
        return 9;
    }
    PosixRunner runner(layout.Log());
    gamebuild::Progress progress;
    std::string last_text;
    int last_percent = -1;
    progress.changed = [&] {
        int percent = int(progress.Fraction() * 100.0);
        std::string text = progress.Text();
        if (percent != last_percent || text != last_text) {
            std::printf("progress %.3f %s\n", progress.Fraction(), text.c_str());
            std::fflush(stdout);
            last_percent = percent;
            last_text = text;
        }
        if (stop_after >= 0 && progress.stage == gamebuild::Stage::Compiling && long(progress.files_done.load()) >= stop_after)
            progress.cancel = true;
    };
    gamebuild::Failure failure;
    gamebuild::Outcome outcome = gamebuild::Build(
        layout, recipe, [&](gamebuild::Inputs& in, std::string& why) { return ReadDisc(game, in, why); }, runner, progress, failure);
    if (outcome == gamebuild::Outcome::Built) {
        std::printf("progress 1.000 Done\n");
        return 0;
    }
    if (outcome == gamebuild::Outcome::Cancelled) {
        std::fprintf(stderr, "cancelled after %u files\n", progress.files_done.load());
        return 3;
    }
    std::fprintf(stderr, "error: %s\n", failure.Line().c_str());
    if (!failure.hint.empty()) std::fprintf(stderr, "hint: %s\n", failure.hint.c_str());
    std::string tail = gamebuild::LogTail(layout.Log(), 600);
    std::fprintf(stderr, "log tail:\n%s\n", tail.c_str());
    return 9;
}
