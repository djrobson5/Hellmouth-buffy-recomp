#pragma once
// Starting and waiting on the build's programs on a POSIX system (macOS): the gamebuild::Runner that tools/first_start_check.cpp (the
// check of the Windows first start, and the development build of the game's code dylib) and the Mac host's first start
// (sdk/host/first_start.cpp) share. Their output and the build's own notes go to the build log (compile.log). Header only.
//
// Each program runs in a process group of its own, and an end of the build (KillAll) ends the group, so that what a compiler started goes
// with it. SIGKILL of the build itself cannot be caught: when the caller names a children file, every program running is listed in it
// (process id and start time, which tell a program of ours from a process that only has the same number later), and the next runner built
// over the same file ends the groups an earlier, killed build left running. The caller holds the build's lock then.
#include <fcntl.h>
#include <libproc.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "app/game_build.h"

extern char** environ;

namespace gamebuild {

class PosixRunner : public Runner {
public:
    // append: the log is added to, as for a start that only makes the pictures. children: the file that lists the programs running (see
    // above), or empty for none; the programs an earlier build left in it are ended here, after the log is open (a line says so there).
    explicit PosixRunner(const fs::path& log, bool append = false, const fs::path& children = {}) : children_(children) {
        std::error_code ec;
        fs::create_directories(log.parent_path(), ec);
        fd_ = open(log.c_str(), O_WRONLY | O_CREAT | (append ? 0 : O_TRUNC) | O_APPEND | O_CLOEXEC, 0644);
        if (fd_ < 0) log_problem_ = "the build log " + log.string() + " could not be opened (" + std::strerror(errno) + ")";
        if (!children_.empty()) EndOrphans();
    }
    ~PosixRunner() override {
        KillAll();
        if (fd_ >= 0) close(fd_);
    }
    int Start(const fs::path& program, const std::vector<Str>& args, const fs::path& cwd, std::string& why) override {
        if (fd_ < 0) {
            why = log_problem_;  // the programs' output has nowhere to go
            return 0;
        }
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
        // a group of its own (so that KillAll ends what the program started too), with SIGPIPE as it is by default (a caller may ignore it)
        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        sigset_t defaults;
        sigemptyset(&defaults);
        sigaddset(&defaults, SIGPIPE);
        posix_spawnattr_setsigdefault(&attr, &defaults);
        posix_spawnattr_setpgroup(&attr, 0);
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF);
        pid_t pid = 0;
        int error = posix_spawn(&pid, program.c_str(), &actions, &attr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attr);
        if (error) {
            why = std::strerror(error);
            return 0;
        }
        Child child;
        child.id = next_++;
        proc_bsdinfo info{};
        if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof info) == int(sizeof info)) {
            child.sec = int64_t(info.pbi_start_tvsec);
            child.usec = int64_t(info.pbi_start_tvusec);
        }
        running_[pid] = child;
        SaveChildren();
        return child.id;
    }
    Ended Wait(int ms) override {
        for (int waited = 0;; waited += 5) {
            // only our own children: the process may have others (the player's window, a launcher's helpers)
            for (auto it = running_.begin(); it != running_.end(); ++it) {
                int status = 0;
                pid_t pid = waitpid(it->first, &status, WNOHANG);
                if (pid == 0) continue;
                Ended e;
                e.id = it->second.id;
                if (pid < 0) e.code = 255;  // gone and not ours to wait for (ECHILD): a build must not wait for it for ever
                else e.code = WIFEXITED(status) ? uint32_t(WEXITSTATUS(status)) : 128u + uint32_t(WTERMSIG(status));
                running_.erase(it);
                SaveChildren();
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
        for (auto& [pid, child] : running_) {
            kill(-pid, SIGKILL);  // the program's group: what it started too
            kill(pid, SIGKILL);
            int status = 0;
            waitpid(pid, &status, 0);
        }
        running_.clear();
        SaveChildren();
    }
    void Note(const std::string& line) override {
        std::string text = line + "\n";
        if (fd_ >= 0) (void)!write(fd_, text.data(), text.size());
    }

private:
    struct Child {
        int id = 0;
        int64_t sec = 0, usec = 0;  // when the process started
    };

    // what is running goes in the children file (rewritten at each change; gone when nothing runs)
    void SaveChildren() {
        if (children_.empty()) return;
        if (running_.empty()) {
            unlink(children_.c_str());
            return;
        }
        std::string text;
        for (auto& [pid, child] : running_) text += std::to_string(pid) + " " + std::to_string(child.sec) + " " + std::to_string(child.usec) + "\n";
        int fd = open(children_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) return;
        (void)!write(fd, text.data(), text.size());
        close(fd);
    }
    // the programs a build that was killed (SIGKILL: nothing ran to end them) left running: their groups end. A program is taken for ours
    // when it is still there with the start time that was written, or is gone and its group is not (what it started is left); a process
    // that has the number now and started at another time is somebody else's. Only the groups of this user can be signalled at all.
    void EndOrphans() {
        std::ifstream f(children_);
        long long pid = 0, sec = 0, usec = 0;
        int ended = 0;
        while (f >> pid >> sec >> usec) {
            if (pid <= 1 || pid_t(pid) == getpgrp() || pid_t(pid) == getpid()) continue;
            proc_bsdinfo info{};
            const bool alive = proc_pidinfo(pid_t(pid), PROC_PIDTBSDINFO, 0, &info, sizeof info) == int(sizeof info);
            if (alive && (static_cast<long long>(info.pbi_start_tvsec) != sec || static_cast<long long>(info.pbi_start_tvusec) != usec)) continue;
            if (alive && getpgid(pid_t(pid)) != pid_t(pid)) continue;
            if (kill(-pid_t(pid), SIGKILL) == 0) ended++;
        }
        f.close();
        unlink(children_.c_str());
        if (ended) Note("orphans: ended " + std::to_string(ended) + " group(s) of programs that a killed build left running");
    }

    fs::path children_;
    int fd_ = -1;
    std::string log_problem_;
    int next_ = 1;
    std::map<pid_t, Child> running_;
};

}  // namespace gamebuild
