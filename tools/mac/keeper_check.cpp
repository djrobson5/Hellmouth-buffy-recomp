// The Mac keeper (sdk/runtime/kernel/keeper.h) without the game: this program plays both parts, as the keeper and as stand-ins for the game's
// instances, which hand over to one another as kernel::Relaunch does (the next is started through keeper::Spawn, and the old one ends after
// it). tools/mac/keeper_check.sh builds and runs it; nothing is opened, no window, no game.
//
//   keeper_check <scratch folder>               the checks; prints PASS or FAIL for each, exits 1 on a FAIL
//   keeper_check --keeper <folder> <plan>       (what the checks start) the keeper, running stand-ins by <plan>, exits with its code
//   keeper_check --standin <folder> <plan> [later]   (what the keeper starts) a stand-in
//
// A plan is a list of steps, one for each instance of the chain, comma separated: the instance does the first step, and the others are the
// next instance's plan.
//   exit:N      ends with exit code N
//   sig         ends itself with SIGUSR1 (as a crash would; no crash report is made for this one)
//   kill        ends itself with SIGKILL
//   next        starts the next instance, waits a moment (the old window stays up until the next has drawn) and exits 0
//   overlap     starts the next instance and stays alive
//   hang        stays alive
//   leave       starts /bin/sleep (a program of its own, with whatever it would inherit) and exits 2
//   badspawn    asks for a program that is not there (must be refused), then goes on with the rest of the plan
//   execself    Relaunch's last resort: keeps the pipes (KeepAcrossExec) and replaces itself with this program, which goes on with the rest
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "kernel/keeper.h"

extern char** environ;

namespace {

using clock_type = std::chrono::steady_clock;

std::string g_self;
int g_failed = 0, g_passed = 0;

void Check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    std::fflush(stdout);
    (ok ? g_passed : g_failed)++;
}

long Since(clock_type::time_point t) {
    return long(std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - t).count());
}

std::string Rest(const std::string& plan) {
    const size_t comma = plan.find(',');
    return comma == std::string::npos ? "" : plan.substr(comma + 1);
}

// ---- a stand-in for the game's instance

void NotePid(const std::string& folder, const char* what) {
    const int fd = open((folder + "/pids").c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    char line[64];
    const int n = std::snprintf(line, sizeof line, "%d %s\n", int(getpid()), what);
    if (fd >= 0) (void)!write(fd, line, size_t(n));
    if (fd >= 0) close(fd);
}

int StandIn(const std::string& folder, const std::string& plan, bool later) {
    if (!keeper::Attach() || !keeper::Active()) return 99;  // started by a keeper: there is one
    if (std::getenv(keeper::kEnv)) return 97;               // and the marker is not left for what this process starts
    const std::string step = plan.substr(0, plan.find(','));
    if (later && std::getenv("KEEPER_CHECK_FROM") == nullptr) return 98;  // the environment given to Spawn arrived
    NotePid(folder, step.c_str());

    auto start_next = [&]() -> int {
        const std::string rest = Rest(plan);
        const std::string from = "KEEPER_CHECK_FROM=" + std::to_string(getpid());
        std::vector<std::string> env_strings;
        for (char** e = environ; *e; e++) env_strings.push_back(*e);
        env_strings.push_back(from);
        std::vector<char*> env;
        for (std::string& s : env_strings) env.push_back(s.data());
        env.push_back(nullptr);
        std::vector<std::string> arg_strings = {g_self, "--standin", folder, rest, "later"};
        std::vector<char*> argv;
        for (std::string& s : arg_strings) argv.push_back(s.data());
        argv.push_back(nullptr);
        return keeper::Spawn(g_self.c_str(), argv.data(), env.data());
    };

    if (step.rfind("exit:", 0) == 0) _exit(std::atoi(step.c_str() + 5));
    if (step == "sig") raise(SIGUSR1);
    if (step == "kill") raise(SIGKILL);
    if (step == "next" || step == "overlap") {
        if (start_next() < 0) _exit(96);
        if (step == "next") {
            usleep(300000);
            _exit(0);
        }
    }
    if (step == "leave") {
        // plain posix_spawn: it inherits every descriptor that is not close-on-exec
        char* argv[] = {const_cast<char*>("/bin/sleep"), const_cast<char*>("4"), nullptr};
        pid_t pid = 0;
        if (posix_spawn(&pid, "/bin/sleep", nullptr, nullptr, argv, environ) != 0) _exit(95);
        char line[32];
        std::snprintf(line, sizeof line, "%d", int(pid));
        std::ofstream(folder + "/leftover") << line;
        _exit(2);
    }
    if (step == "execself") {
        keeper::KeepAcrossExec();
        const std::string rest = Rest(plan);
        char* argv[] = {g_self.data(), const_cast<char*>("--standin"), const_cast<char*>(folder.c_str()), const_cast<char*>(rest.c_str()), nullptr};
        execv(g_self.c_str(), argv);
        _exit(92);
    }
    if (step == "badspawn") {
        char* argv[] = {const_cast<char*>("/nonexistent/program"), nullptr};
        const int pid = keeper::Spawn("/nonexistent/program", argv, environ);
        if (pid != -1 || errno != ENOENT) _exit(94);
        const std::string rest = Rest(plan);
        if (rest.rfind("exit:", 0) == 0) _exit(std::atoi(rest.c_str() + 5));
        _exit(93);
    }
    for (;;) pause();  // hang, overlap
}

// ---- the checks

pid_t StartKeeper(const std::string& folder, const std::string& plan) {
    std::vector<std::string> a = {g_self, "--keeper", folder, plan};
    std::vector<char*> argv;
    for (std::string& s : a) argv.push_back(s.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    return posix_spawn(&pid, g_self.c_str(), nullptr, nullptr, argv.data(), environ) == 0 ? pid : -1;
}

// the keeper's exit code (128 + n for a signal) within ms; -1 when it has not ended
int WaitKeeper(pid_t pid, int ms, long* took = nullptr) {
    const auto t0 = clock_type::now();
    for (;;) {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            if (took) *took = Since(t0);
            return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        }
        if (Since(t0) > ms) return -1;
        usleep(5000);
    }
}

std::vector<int> Pids(const std::string& folder) {
    std::vector<int> out;
    std::ifstream in(folder + "/pids");
    for (std::string line; std::getline(in, line);) out.push_back(std::atoi(line.c_str()));
    return out;
}

bool Alive(int pid) { return kill(pid, 0) == 0; }

std::string FileText(const std::string& path) {
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string Fresh(const std::string& root, const std::string& name) {
    const std::string folder = root + "/" + name;
    std::string cmd = "rm -rf '" + folder + "' && mkdir -p '" + folder + "'";
    if (std::system(cmd.c_str()) != 0) std::exit(2);
    return folder;
}

// a plan run to its end: the keeper's exit code, the instances that were made, and the keeper's log
void Plan(const std::string& root, const std::string& name, const std::string& plan, int want, size_t instances) {
    const std::string folder = Fresh(root, name);
    long took = 0;
    const pid_t keeper_pid = StartKeeper(folder, plan);
    const int code = keeper_pid > 0 ? WaitKeeper(keeper_pid, 15000, &took) : -2;
    const size_t made = Pids(folder).size();
    const std::string log = FileText(folder + "/supervisor.log");
    const bool logged = log.find("done, exit code " + std::to_string(want)) != std::string::npos;
    Check(code == want && made == instances && logged,
          name + ": exit code " + std::to_string(code) + " (wanted " + std::to_string(want) + "), " + std::to_string(made) + " instance(s) (wanted " +
              std::to_string(instances) + "), " + std::to_string(took) + " ms, " + (logged ? "logged" : "not in supervisor.log"));
}

// the keeper is killed while its instances live: all of them are gone within a second
void KillTest(const std::string& root, const std::string& name, const std::string& plan, size_t instances, size_t alive_at_kill) {
    const std::string folder = Fresh(root, name);
    const pid_t keeper_pid = StartKeeper(folder, plan);
    const auto t0 = clock_type::now();
    while (Pids(folder).size() < instances && Since(t0) < 10000) usleep(5000);
    const std::vector<int> pids = Pids(folder);
    size_t alive = 0;
    for (int p : pids) alive += Alive(p);
    const auto killed = clock_type::now();
    kill(keeper_pid, SIGKILL);
    const int code = WaitKeeper(keeper_pid, 5000);
    bool all_gone = false;
    while (!all_gone && Since(killed) < 5000) {
        all_gone = true;
        for (int p : pids) all_gone = all_gone && !Alive(p);
        if (!all_gone) usleep(5000);
    }
    const long took = Since(killed);
    for (int p : pids) kill(p, SIGKILL);  // whatever the check leaves is not left
    Check(pids.size() == instances && alive >= alive_at_kill && code == 128 + SIGKILL && all_gone && took <= 1000,
          name + ": " + std::to_string(pids.size()) + " instance(s), " + std::to_string(alive) + " alive when the keeper was killed, all gone after " +
              std::to_string(took) + " ms (wanted 1000 at most)");
}

// Attach with what the environment may hold: it must say no, and take the variable out either way
void AttachChecks(const std::string& root) {
    const std::string folder = Fresh(root, "attach");
    struct Case {
        const char* value;  // null: not set
        const char* why;
    };
    const Case cases[] = {{nullptr, "no marker"}, {"garbage", "text"}, {"99,98,97", "descriptors that are not open"}, {"1,2,3", "the standard streams' neighbours"},
                          {"3,4", "two numbers"}, {"3,4,5,6", "four numbers"}, {"-5,4,5", "a negative one"}, {"", "empty"}};
    for (const Case& c : cases) {
        pid_t pid = 0;
        std::vector<std::string> env;
        for (char** e = environ; *e; e++)
            if (std::strncmp(*e, keeper::kEnv, std::strlen(keeper::kEnv)) != 0) env.push_back(*e);
        if (c.value) env.push_back(std::string(keeper::kEnv) + "=" + c.value);
        std::vector<char*> envp;
        for (std::string& s : env) envp.push_back(s.data());
        envp.push_back(nullptr);
        std::vector<std::string> a = {g_self, "--attach", "none"};
        std::vector<char*> argv;
        for (std::string& s : a) argv.push_back(s.data());
        argv.push_back(nullptr);
        posix_spawn(&pid, g_self.c_str(), nullptr, nullptr, argv.data(), envp.data());
        Check(WaitKeeper(pid, 5000) == 0, std::string("Attach says no, and leaves no marker: ") + c.why);
    }
    // a descriptor that is open but is not a pipe (a file) is not taken either
    pid_t pid = 0;
    std::vector<std::string> a = {g_self, "--attach", "file", folder};
    std::vector<char*> argv;
    for (std::string& s : a) argv.push_back(s.data());
    argv.push_back(nullptr);
    posix_spawn(&pid, g_self.c_str(), nullptr, nullptr, argv.data(), environ);
    Check(WaitKeeper(pid, 5000) == 0, "Attach says no to descriptors that are open but are files, not pipes");
}

int AttachMode(const std::string& kind) {
    if (kind == "file") {
        const int fd = open("/dev/null", O_RDWR);
        const int fd2 = fcntl(fd, F_DUPFD, 20), fd3 = fcntl(fd, F_DUPFD, 21);
        const std::string value = std::to_string(fd) + "," + std::to_string(fd2) + "," + std::to_string(fd3);
        setenv(keeper::kEnv, value.c_str(), 1);
    }
    const bool attached = keeper::Attach();
    if (attached || keeper::Active() || std::getenv(keeper::kEnv)) return 1;
    return 0;
}

int Keeper(const std::string& folder, const std::string& plan) {
    std::vector<std::string> a = {g_self, "--standin", folder, plan};
    std::vector<char*> argv;
    for (std::string& s : a) argv.push_back(s.data());
    argv.push_back(nullptr);
    const std::string log = folder + "/supervisor.log";
    const std::optional<int> code = keeper::Run(g_self.c_str(), argv.data(), environ, log.c_str());
    return code ? *code : 99;
}

}  // namespace

int main(int argc, char** argv) {
    char path[4096];
    uint32_t size = sizeof path;
    if (_NSGetExecutablePath(path, &size) != 0) return 2;
    g_self = path;
    if (argc >= 4 && std::strcmp(argv[1], "--keeper") == 0) return Keeper(argv[2], argv[3]);
    if (argc >= 4 && std::strcmp(argv[1], "--standin") == 0) return StandIn(argv[2], argv[3], argc >= 5);
    if (argc >= 3 && std::strcmp(argv[1], "--attach") == 0) return AttachMode(argv[2]);
    if (argc != 2) {
        std::fprintf(stderr, "usage: keeper_check <scratch folder>\n");
        return 2;
    }
    alarm(240);  // a check that hangs is a failure, not a wait for ever
    const std::string root = argv[1];
    std::system(("mkdir -p '" + root + "'").c_str());

    std::printf("== the keeper and a chain of instances\n");
    Plan(root, "one", "exit:0", 0, 1);
    Plan(root, "one-code", "exit:5", 5, 1);
    Plan(root, "chain", "next,next,next,exit:7", 7, 4);
    Plan(root, "chain-first-code-ignored", "next,exit:3", 3, 2);
    Plan(root, "chain-ends-0", "next,next,exit:0", 0, 3);
    Plan(root, "spawn-refused", "badspawn,exit:4", 4, 1);
    Plan(root, "exec-last-resort", "execself,exit:6", 6, 2);
    Plan(root, "exec-then-chain", "execself,next,exit:8", 8, 3);
    std::printf("== a crash gives a code that is not 0\n");
    Plan(root, "crash-first", "sig", 128 + SIGUSR1, 1);
    Plan(root, "crash-later", "next,next,sig", 128 + SIGUSR1, 3);
    Plan(root, "kill-later", "next,kill", 128 + SIGKILL, 2);
    std::printf("== a program the game started that outlives it does not keep the keeper\n");
    {
        const std::string folder = Fresh(root, "leaver");
        long took = 0;
        const pid_t k = StartKeeper(folder, "leave");
        const int code = WaitKeeper(k, 15000, &took);
        const int sleeper = std::atoi(FileText(folder + "/leftover").c_str());
        const bool still = sleeper > 0 && Alive(sleeper);
        if (sleeper > 0) kill(sleeper, SIGKILL);
        Check(code == 2 && took < 2500 && still, "leaver: exit code " + std::to_string(code) + " (wanted 2) after " + std::to_string(took) +
                                                   " ms (wanted under 2500: the /bin/sleep 4 it started holds no end of the pipes, and was still running: " +
                                                   (still ? "yes" : "no") + ")");
    }
    std::printf("== the keeper is killed (what RecompLauncher's Stop does)\n");
    KillTest(root, "kill-one", "hang", 1, 1);
    KillTest(root, "kill-handover", "overlap,hang", 2, 2);
    KillTest(root, "kill-three", "overlap,overlap,hang", 3, 3);
    KillTest(root, "kill-during-handover", "next,hang", 2, 1);
    KillTest(root, "kill-after-handover", "next,next,hang", 3, 1);
    std::printf("== a game process with no keeper, or with a marker that is wrong\n");
    AttachChecks(root);
    std::printf("%d checks passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
