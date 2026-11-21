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
//   sleepexit:N waits half a second, then ends with exit code N
//   sig         ends itself with SIGUSR1 (as a crash would; no crash report is made for this one)
//   kill        ends itself with SIGKILL
//   next        starts the next instance, waits a moment (the old window stays up until the next has drawn) and exits 0
//   overlap     starts the next instance and stays alive
//   hang        stays alive
//   leave       starts programs of its own, with whatever they inherit (one that checks for the pipes' descriptors, one that outlives it), exits 2
//   leaker      the same, but after KeepAcrossExec: the check must find the descriptors open (the control for `leave`), exits 2
//   termcode:N  hangs; SIGTERM, SIGINT and SIGHUP end it with exit code N (what a game does to stop itself in order)
//   junknext, partialnext, badsumnext   writes what is not a request first (bytes that are no frame; the start of a frame that is never
//               finished; a whole frame whose checksum is wrong), then does `next`
//   nonsense    sends a frame that checks out but is no request, and wants the keeper's refusal at once (exit 4)
//   badspawn    asks for a program that is not there (must be refused), then goes on with the rest of the plan
//   execself    Relaunch's last resort: keeps the pipes (KeepAcrossExec) and replaces itself with this program, which goes on with the rest
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#include <poll.h>
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

// ---- processes by their identity (the pid and when it started: a pid that has been given to another process is not ours to signal)

long StartTime(pid_t pid) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, int(pid)};
    struct kinfo_proc info;
    size_t size = sizeof info;
    if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0 || size == 0) return -1;
    return long(info.kp_proc.p_starttime.tv_sec) * 1000000L + long(info.kp_proc.p_starttime.tv_usec);
}

struct Rec {
    int pid;
    long start;
};

bool Alive(const Rec& r) { return kill(r.pid, 0) == 0 && StartTime(r.pid) == r.start; }

void KillRec(const Rec& r) {
    if (r.pid > 1 && Alive(r)) kill(r.pid, SIGKILL);
}

// ---- a stand-in for the game's instance

std::string g_marker;  // kEnv as the keeper gave it, which Attach takes out: the descriptor numbers
int g_term_code = 0;

void OnTerm(int) { _exit(g_term_code); }

void NotePid(const std::string& folder, const char* what) {
    const int fd = open((folder + "/pids").c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    char line[96];
    const int n = std::snprintf(line, sizeof line, "%d %ld %s\n", int(getpid()), StartTime(getpid()), what);
    if (fd >= 0) (void)!write(fd, line, size_t(n));
    if (fd >= 0) close(fd);
}

// the descriptor numbers of the keeper's pipes: lifeline, requests, replies
bool Numbers(int fds[3]) { return std::sscanf(g_marker.c_str(), "%d,%d,%d", &fds[0], &fds[1], &fds[2]) == 3; }

void Frame(int fd, const void* payload, uint32_t length, uint32_t sum_xor) {
    uint32_t header[3] = {keeper::kFrameMagic, length, 2166136261u};
    for (uint32_t i = 0; i < length; i++) header[2] = (header[2] ^ static_cast<const unsigned char*>(payload)[i]) * 16777619u;
    header[2] ^= sum_xor;
    (void)!write(fd, header, sizeof header);
    if (length) (void)!write(fd, payload, length);
}

// a program that says whether the keeper's descriptors are open in it: exit 0 when none is (what a compiler started by the game must find)
int FdCheck(const std::string& numbers) {
    int fds[3];
    if (std::sscanf(numbers.c_str(), "%d,%d,%d", &fds[0], &fds[1], &fds[2]) != 3) return 99;
    for (int fd : fds)
        if (fcntl(fd, F_GETFD) != -1) return 1;
    return 0;
}

// starts the check as any program would be (posix_spawn with nothing asked: it inherits what is not close-on-exec) and returns its code
int RunFdCheck() {
    int fds[3];
    if (!Numbers(fds)) return 99;
    std::string arg = std::to_string(fds[0]) + "," + std::to_string(fds[1]) + "," + std::to_string(fds[2]);
    char* argv[] = {g_self.data(), const_cast<char*>("--fdcheck"), arg.data(), nullptr};
    pid_t pid = 0;
    if (posix_spawn(&pid, g_self.c_str(), nullptr, nullptr, argv, environ) != 0) return 98;
    int status = 0;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) return 97;
    return WEXITSTATUS(status);
}

int StandIn(const std::string& folder, const std::string& plan, bool later) {
    if (const char* marker = std::getenv(keeper::kEnv)) g_marker = marker;
    if (!keeper::Attach() || !keeper::Active()) return 99;  // started by a keeper: there is one
    if (std::getenv(keeper::kEnv)) return 97;               // and the marker is not left for what this process starts
    const std::string step = plan.substr(0, plan.find(','));
    if (later && std::getenv("KEEPER_CHECK_FROM") == nullptr) return 98;  // the environment given to Spawn arrived
    if (step.rfind("termcode:", 0) == 0) {
        g_term_code = std::atoi(step.c_str() + 9);
        for (int sig : {SIGTERM, SIGINT, SIGHUP}) signal(sig, OnTerm);
    }
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
    if (step.rfind("sleepexit:", 0) == 0) {
        usleep(450000);
        _exit(std::atoi(step.c_str() + 10));
    }
    if (step == "sig") raise(SIGUSR1);
    if (step == "kill") raise(SIGKILL);
    int fds[3] = {-1, -1, -1};
    Numbers(fds);
    if (step == "junknext") {
        unsigned char junk[37];
        std::memset(junk, 0xAB, sizeof junk);
        (void)!write(fds[1], junk, sizeof junk);
    }
    if (step == "partialnext") {  // the start of a frame that says 3000 bytes follow, and 200 do
        uint32_t header[3] = {keeper::kFrameMagic, 3000, 0};
        unsigned char body[200];
        std::memset(body, 0x5A, sizeof body);
        (void)!write(fds[1], header, sizeof header);
        (void)!write(fds[1], body, sizeof body);
    }
    if (step == "badsumnext") {
        const char eight[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        Frame(fds[1], eight, sizeof eight, 0x1234);
    }
    if (step == "nonsense") {
        // the id, 1 argument, 0 entries, and a program name that is never ended
        unsigned char payload[16] = {0x78, 0x56, 0x34, 0x12, 1, 0, 0, 0, 0, 0, 0, 0, 'x', 'y', 'z', 'w'};
        Frame(fds[1], payload, sizeof payload, 0);
        struct {
            uint32_t id;
            int32_t pid;
        } reply = {};
        pollfd pf{fds[2], POLLIN, 0};
        if (poll(&pf, 1, 3000) != 1 || read(fds[2], &reply, sizeof reply) != sizeof reply) _exit(91);
        _exit(reply.id == 0x12345678u && reply.pid == -EINVAL ? 4 : 90);
    }
    if (step == "next" || step == "overlap" || step == "junknext" || step == "partialnext" || step == "badsumnext") {
        if (start_next() < 0) _exit(96);
        if (step != "overlap") {
            usleep(300000);
            _exit(0);
        }
    }
    if (step == "leave" || step == "leaker") {
        if (step == "leaker") keeper::KeepAcrossExec();  // the control: with the descriptors left open, the check has to find them
        const int found = RunFdCheck();                  // 0: none open (what `leave` must get), 1: some open (what `leaker` must get)
        // and a program of its own that outlives this one, as a compiler left behind would (plain posix_spawn)
        char* argv[] = {const_cast<char*>("/bin/sleep"), const_cast<char*>("4"), nullptr};
        pid_t pid = 0;
        if (posix_spawn(&pid, "/bin/sleep", nullptr, nullptr, argv, environ) != 0) _exit(95);
        std::ofstream(folder + "/leftover") << pid << " " << StartTime(pid);
        _exit(found == (step == "leaker" ? 1 : 0) ? 2 : 90);
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
    for (;;) pause();  // hang, overlap, termcode
}

// ---- the checks

pid_t StartKeeper(const std::string& folder, const std::string& plan, bool sigchld_ignored = false) {
    std::vector<std::string> a = {g_self, sigchld_ignored ? "--sigign" : "--keeper", folder, plan};
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

std::vector<Rec> Pids(const std::string& folder) {
    std::vector<Rec> out;
    std::ifstream in(folder + "/pids");
    for (std::string line; std::getline(in, line);) {
        Rec r{0, 0};
        if (std::sscanf(line.c_str(), "%d %ld", &r.pid, &r.start) == 2) out.push_back(r);
    }
    return out;
}

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

// what a keeper that did not end, or a check that is over, leaves: the keeper (its death ends the stand-ins) and every instance recorded,
// each only if it is still the process that was recorded
void Clean(pid_t keeper_pid, const std::string& folder) {
    if (keeper_pid > 1) kill(keeper_pid, SIGKILL);
    for (const Rec& r : Pids(folder)) KillRec(r);
}

// a plan run to its end: the keeper's exit code, the instances that were made, and the keeper's log (which must hold `in_log` too)
void Plan(const std::string& root, const std::string& name, const std::string& plan, int want, size_t instances, bool sigchld_ignored = false,
          const std::string& in_log = "") {
    const std::string folder = Fresh(root, name);
    long took = 0;
    const pid_t keeper_pid = StartKeeper(folder, plan, sigchld_ignored);
    const int code = keeper_pid > 0 ? WaitKeeper(keeper_pid, 15000, &took) : -2;
    if (code == -1) Clean(keeper_pid, folder);  // it has not ended: it and what it started are not left
    const size_t made = Pids(folder).size();
    const std::string log = FileText(folder + "/supervisor.log");
    const bool logged = log.find("done, exit code " + std::to_string(want)) != std::string::npos;
    const bool said = in_log.empty() || log.find(in_log) != std::string::npos;
    Check(code == want && made == instances && logged && said,
          name + ": exit code " + std::to_string(code) + " (wanted " + std::to_string(want) + "), " + std::to_string(made) + " instance(s) (wanted " +
              std::to_string(instances) + "), " + std::to_string(took) + " ms, " + (logged ? "logged" : "not in supervisor.log") +
              (in_log.empty() ? "" : said ? ", \"" + in_log + "\" in the log" : ", no \"" + in_log + "\" in the log"));
}

// The keeper is sent a signal while its instances live: it goes on to them, they end by it (the one that has a handler, with the code it
// gives), and the keeper ends with that code, not killed. All are gone.
void SignalTest(const std::string& root, const std::string& name, const std::string& plan, size_t instances, int signo, int want) {
    const std::string folder = Fresh(root, name);
    const pid_t keeper_pid = StartKeeper(folder, plan);
    const auto t0 = clock_type::now();
    while (Pids(folder).size() < instances && Since(t0) < 10000) usleep(5000);
    usleep(50000);  // the last one is in its pause, with its handler
    const std::vector<Rec> pids = Pids(folder);
    kill(keeper_pid, signo);
    long took = 0;
    const int code = WaitKeeper(keeper_pid, 5000, &took);
    if (code == -1) Clean(keeper_pid, folder);
    bool all_gone = true;
    for (const Rec& r : pids) all_gone = all_gone && !Alive(r);
    const std::string log = FileText(folder + "/supervisor.log");
    const bool said = log.find("signal " + std::to_string(signo) + " goes on") != std::string::npos;
    Check(pids.size() == instances && code == want && all_gone && said,
          name + ": signal " + std::to_string(signo) + " to the keeper, exit code " + std::to_string(code) + " (wanted " + std::to_string(want) + ") after " +
              std::to_string(took) + " ms, " + std::to_string(pids.size()) + " instance(s), " + (all_gone ? "all gone" : "some left") + ", " +
              (said ? "logged" : "not logged"));
}

// the keeper is killed while its instances live: all of them are gone within a second
void KillTest(const std::string& root, const std::string& name, const std::string& plan, size_t instances, size_t alive_at_kill) {
    const std::string folder = Fresh(root, name);
    const pid_t keeper_pid = StartKeeper(folder, plan);
    const auto t0 = clock_type::now();
    while (Pids(folder).size() < instances && Since(t0) < 10000) usleep(5000);
    const std::vector<Rec> pids = Pids(folder);
    size_t alive = 0;
    for (const Rec& r : pids) alive += Alive(r);
    const auto killed = clock_type::now();
    kill(keeper_pid, SIGKILL);
    const int code = WaitKeeper(keeper_pid, 5000);
    bool all_gone = false;
    while (!all_gone && Since(killed) < 5000) {
        all_gone = true;
        for (const Rec& r : pids) all_gone = all_gone && !Alive(r);
        if (!all_gone) usleep(5000);
    }
    const long took = Since(killed);
    for (const Rec& r : pids) KillRec(r);  // whatever the check leaves is not left
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
    if (argc >= 4 && std::strcmp(argv[1], "--sigign") == 0) {
        // a launcher that ignores SIGCHLD (the setting survives exec): the keeper must not depend on it
        signal(SIGCHLD, SIG_IGN);
        argv[1] = const_cast<char*>("--keeper");
        execv(g_self.c_str(), argv);
        return 2;
    }
    if (argc >= 4 && std::strcmp(argv[1], "--standin") == 0) return StandIn(argv[2], argv[3], argc >= 5);
    if (argc >= 3 && std::strcmp(argv[1], "--attach") == 0) return AttachMode(argv[2]);
    if (argc >= 3 && std::strcmp(argv[1], "--fdcheck") == 0) return FdCheck(argv[2]);
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
    std::printf("== a request that is damaged, cut short or no request at all\n");
    Plan(root, "frame-junk", "junknext,exit:7", 7, 2, false, "dropped");
    Plan(root, "frame-cut-short", "partialnext,exit:8", 8, 2, false, "dropped");
    Plan(root, "frame-bad-checksum", "badsumnext,exit:9", 9, 2, false, "dropped");
    Plan(root, "frame-no-request", "nonsense", 4, 1, false, "makes no sense");
    std::printf("== the launcher ignores SIGCHLD\n");
    Plan(root, "sigchld-ignored", "next,exit:9", 9, 2, true);
    Plan(root, "sigchld-ignored-crash", "sig", 128 + SIGUSR1, 1, true);
    Plan(root, "sigchld-ignored-chain", "next,next,next,exit:7", 7, 4, true);
    Plan(root, "sigchld-ignored-one-code", "exit:5", 5, 1, true);
    Plan(root, "sigchld-ignored-slow", "next,next,sleepexit:3", 3, 3, true);
    std::printf("== a crash gives a code that is not 0\n");
    Plan(root, "crash-first", "sig", 128 + SIGUSR1, 1);
    Plan(root, "crash-later", "next,next,sig", 128 + SIGUSR1, 3);
    Plan(root, "kill-later", "next,kill", 128 + SIGKILL, 2);
    std::printf("== what the game starts holds none of the keeper's pipes\n");
    for (const char* step : {"leave", "leaker"}) {
        const std::string folder = Fresh(root, step);
        long took = 0;
        const pid_t k = StartKeeper(folder, step);
        const int code = WaitKeeper(k, 15000, &took);
        if (code == -1) Clean(k, folder);
        Rec sleeper{0, 0};
        std::ifstream(folder + "/leftover") >> sleeper.pid >> sleeper.start;
        const bool still = sleeper.pid > 0 && Alive(sleeper);
        KillRec(sleeper);
        // `leave` exits 2 when the program it started (a copy of this one, with nothing asked of posix_spawn) found none of the three descriptors
        // open; `leaker` exits 2 when, after KeepAcrossExec, it found them: the check can tell
        Check(code == 2 && took < 2500 && still,
              std::string(step) + ": exit code " + std::to_string(code) + " (wanted 2: " +
                  (std::strcmp(step, "leave") == 0 ? "a program the game starts finds the keeper's descriptors closed" :
                                                     "with the descriptors left open the same check finds them, so it can fail") +
                  ") after " + std::to_string(took) + " ms; a /bin/sleep 4 left behind did not keep the keeper (still running: " + (still ? "yes" : "no") + ")");
    }
    std::printf("== the keeper is sent SIGTERM, SIGINT or SIGHUP: the game is told, and ends in its own way\n");
    SignalTest(root, "term-chain", "next,termcode:11", 2, SIGTERM, 11);
    SignalTest(root, "term-two-alive", "overlap,termcode:12", 2, SIGTERM, 12);
    SignalTest(root, "int", "termcode:13", 1, SIGINT, 13);
    SignalTest(root, "hup", "next,next,termcode:14", 3, SIGHUP, 14);
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
