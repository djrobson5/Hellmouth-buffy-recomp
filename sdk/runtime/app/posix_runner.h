#pragma once
// Starting and waiting on the build's programs on a POSIX system (macOS): the gamebuild::Runner that tools/first_start_check.cpp (the
// check of the Windows first start, and the development build of the game's code dylib) and the Mac host's first start
// (sdk/host/first_start.cpp) share. Their output and the build's own notes go to the build log (compile.log). Header only.
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "app/game_build.h"

extern char** environ;

namespace gamebuild {

class PosixRunner : public Runner {
public:
    // append: the log is added to, as for a start that only makes the pictures
    explicit PosixRunner(const fs::path& log, bool append = false) {
        std::error_code ec;
        fs::create_directories(log.parent_path(), ec);
        fd_ = open(log.c_str(), O_WRONLY | O_CREAT | (append ? 0 : O_TRUNC) | O_APPEND, 0644);
    }
    ~PosixRunner() override {
        KillAll();
        if (fd_ >= 0) close(fd_);
    }
    int Start(const fs::path& program, const std::vector<Str>& args, const fs::path& cwd, std::string& why) override {
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

}  // namespace gamebuild
