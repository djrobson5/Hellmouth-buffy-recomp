// The keeper (keeper.h): a Mac's counterpart of the Windows supervisor. Only libSystem and the C++ library are used.
#ifdef __APPLE__
#include "kernel/keeper.h"

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace keeper {

namespace {

struct Pipes {
    int life_r = -1, life_w = -1, req_r = -1, req_w = -1, rep_r = -1, rep_w = -1;
};

// the game's own: which descriptors, and the value of kEnv they came from
Pipes g_game;
std::string g_marker;
bool g_active = false;

void CloseFd(int& fd) {
    if (fd >= 0) close(fd);
    fd = -1;
}

// a pipe whose ends are close-on-exec and not among the first ten descriptors (a child gets them under the same numbers, and 0 to 2 are its own)
bool MakePipe(int& read_end, int& write_end) {
    int p[2];
    if (pipe(p) != 0) return false;
    int d[2] = {fcntl(p[0], F_DUPFD_CLOEXEC, 10), fcntl(p[1], F_DUPFD_CLOEXEC, 10)};
    close(p[0]);
    close(p[1]);
    if (d[0] < 0 || d[1] < 0) {
        if (d[0] >= 0) close(d[0]);
        if (d[1] >= 0) close(d[1]);
        return false;
    }
    read_end = d[0], write_end = d[1];
    return true;
}

bool WriteAll(int fd, const void* data, size_t size) {
    const char* p = static_cast<const char*>(data);
    while (size) {
        const ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n, size -= size_t(n);
    }
    return true;
}

// size bytes within ms milliseconds (all of the time for them, not for each read); false: end of file, an error or the time
bool ReadAll(int fd, void* data, size_t size, int ms) {
    using clock = std::chrono::steady_clock;
    const auto end = clock::now() + std::chrono::milliseconds(ms);
    char* p = static_cast<char*>(data);
    while (size) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - clock::now()).count();
        if (left <= 0) return false;
        pollfd pf{fd, POLLIN, 0};
        const int ready = poll(&pf, 1, int(left));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return false;
        const ssize_t n = read(fd, p, size);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) return false;
        p += n, size -= size_t(n);
    }
    return true;
}

// ---- the keeper's side

// a few lines in the keeper's log (the game owns its own); best effort
class Note {
public:
    explicit Note(const char* path) {
        if (path && *path) fd_ = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        began_ = std::chrono::steady_clock::now();
    }
    ~Note() { CloseFd(fd_); }
    Note(const Note&) = delete;
    Note& operator=(const Note&) = delete;
    void operator()(const char* format, long a = 0, long b = 0) {
        if (fd_ < 0) return;
        char line[200];
        const long ms = long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began_).count());
        const int head = std::snprintf(line, sizeof line, "%8ld ms  ", ms);
        const int body = std::snprintf(line + head, sizeof line - size_t(head) - 1, format, a, b);
        if (body < 0) return;
        const size_t n = std::min(sizeof line - 2, size_t(head + body));
        line[n] = '\n';
        (void)!write(fd_, line, n + 1);
    }

private:
    int fd_ = -1;
    std::chrono::steady_clock::time_point began_;
};

std::string Marker(const Pipes& p) { return std::to_string(p.life_r) + "," + std::to_string(p.req_w) + "," + std::to_string(p.rep_r); }

// Starts path as a game process of this keeper: the same standard streams and, of the keeper's descriptors, the game's three ends only
// (everything else is close-on-exec). The environment is the one given, with the marker. 0 and the pid, or an error number.
int StartChild(const Pipes& p, const char* path, const std::vector<std::string>& argv, const std::vector<std::string>& env, pid_t& pid) {
    std::vector<std::string> own;
    const std::string prefix = std::string(kEnv) + "=";
    for (const std::string& e : env)
        if (e.compare(0, prefix.size(), prefix) != 0) own.push_back(e);
    own.push_back(prefix + Marker(p));
    std::vector<char*> a, e;
    for (const std::string& s : argv) a.push_back(const_cast<char*>(s.c_str()));
    for (const std::string& s : own) e.push_back(const_cast<char*>(s.c_str()));
    a.push_back(nullptr), e.push_back(nullptr);

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
    posix_spawn_file_actions_t files;
    posix_spawn_file_actions_init(&files);
    for (int fd : {0, 1, 2})
        if (fcntl(fd, F_GETFD) != -1) posix_spawn_file_actions_addinherit_np(&files, fd);
    for (int fd : {p.life_r, p.req_w, p.rep_r}) posix_spawn_file_actions_addinherit_np(&files, fd);
    const int failed = posix_spawn(&pid, path, &files, &attr, a.data(), e.data());
    posix_spawn_file_actions_destroy(&files);
    posix_spawnattr_destroy(&attr);
    return failed;
}

int Decode(int status) { return WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1; }

uint32_t Get32(const std::string& s, size_t at) {
    uint32_t v = 0;
    std::memcpy(&v, s.data() + at, sizeof v);
    return v;
}

// a request is a 32-bit length, then that many bytes: the id (32 bits), the number of arguments and of environment entries (32 bits
// each), then the program, the arguments and the entries, each ended by a zero byte. False: it is not one.
bool ParseRequest(const std::string& payload, uint32_t& id, std::string& path, std::vector<std::string>& argv, std::vector<std::string>& env) {
    if (payload.size() < 12) return false;
    id = Get32(payload, 0);
    const uint32_t argc = Get32(payload, 4), envc = Get32(payload, 8);
    if (argc > 100000 || envc > 100000) return false;
    size_t at = 12;
    auto next = [&](std::string& out) {
        const size_t end = payload.find('\0', at);
        if (end == std::string::npos) return false;
        out.assign(payload, at, end - at);
        at = end + 1;
        return true;
    };
    if (!next(path)) return false;
    for (uint32_t i = 0; i < argc + envc; i++) {
        std::string s;
        if (!next(s)) return false;
        (i < argc ? argv : env).push_back(std::move(s));
    }
    return at == payload.size();
}

}  // namespace

std::optional<int> Run(const char* exe, char* const* argv, char* const* envp, const char* log_path) {
    Pipes p;
    if (!MakePipe(p.life_r, p.life_w) || !MakePipe(p.req_r, p.req_w) || !MakePipe(p.rep_r, p.rep_w)) {
        for (int* fd : {&p.life_r, &p.life_w, &p.req_r, &p.req_w, &p.rep_r, &p.rep_w}) CloseFd(*fd);
        return std::nullopt;
    }
    fcntl(p.rep_w, F_SETNOSIGPIPE, 1);  // a game process that has gone before the answer is read is not this one's end
    std::vector<std::string> args, env;
    for (char* const* a = argv; a && *a; a++) args.push_back(*a);
    for (char* const* e = envp; e && *e; e++) env.push_back(*e);

    pid_t last = 0;
    if (StartChild(p, exe, args, env, last) != 0) {
        for (int* fd : {&p.life_r, &p.life_w, &p.req_r, &p.req_w, &p.rep_r, &p.rep_w}) CloseFd(*fd);
        return std::nullopt;
    }
    Note note(log_path);
    note("supervising process %ld", long(last));
    // The keeper keeps the game's three ends too: every later instance is started from here and is given them. Only the keeper holds the
    // lifeline's write end, and it never writes: it closes with this process, however that ends, which is what the game processes see.
    std::set<pid_t> live{last};
    std::map<pid_t, int> ended;  // statuses of the children that have been reaped
    auto reap = [&] {
        int status = 0;
        for (pid_t r; (r = waitpid(-1, &status, WNOHANG)) > 0;) {
            ended[r] = status;
            live.erase(r);
            note(WIFSIGNALED(status) ? "process %ld ended by a signal, code %ld" : "process %ld exited, code %ld", long(r), long(Decode(status)));
        }
    };

    std::string in;
    // until no child is left: an instance that asks for the next has it started before it goes, so the one is never missing in between
    while (!live.empty()) {
        pollfd pf{p.req_r, POLLIN, 0};
        const int ready = poll(&pf, 1, 100);  // a short wait: a child that ends before it has shown anything is looked for by its pid
        reap();
        if (ready <= 0) continue;
        char buffer[65536];
        const ssize_t n = read(p.req_r, buffer, sizeof buffer);
        if (n > 0) in.append(buffer, size_t(n));
        while (in.size() >= 4) {
            const uint32_t length = Get32(in, 0);
            if (length > (4u << 20)) {  // not a request: nothing after it can be trusted
                in.clear();
                break;
            }
            if (in.size() < 4 + size_t(length)) break;
            const std::string payload = in.substr(4, length);
            in.erase(0, 4 + size_t(length));
            uint32_t id = 0;
            std::string path;
            std::vector<std::string> child_args, child_env;
            struct {
                uint32_t id;
                int32_t pid;
            } reply = {0, -EINVAL};
            if (ParseRequest(payload, id, path, child_args, child_env)) {
                reply.id = id;
                pid_t pid = 0;
                if (const int failed = StartChild(p, path.c_str(), child_args, child_env, pid)) {
                    note("cannot start a game process, error %ld", failed);
                    reply.pid = -failed;
                } else {
                    last = pid;
                    live.insert(pid);
                    reply.pid = int32_t(pid);
                    note("process %ld started, for a level", long(pid));
                }
            }
            WriteAll(p.rep_w, &reply, sizeof reply);
        }
    }

    const int code = ended.count(last) ? Decode(ended[last]) : 1;
    note("done, exit code %ld", long(code));
    for (int* fd : {&p.life_r, &p.life_w, &p.req_r, &p.req_w, &p.rep_r, &p.rep_w}) CloseFd(*fd);
    return code;
}

// ---- the game's side

bool Attach() {
    const char* value = std::getenv(kEnv);
    if (!value) return false;
    const std::string marker = value;
    unsetenv(kEnv);  // what this process starts is not a game process of the keeper unless Spawn makes it one
    long fds[3] = {};
    {
        const char* at = marker.c_str();
        for (int i = 0; i < 3; i++) {
            char* end = nullptr;
            fds[i] = std::strtol(at, &end, 10);
            if (end == at || (i < 2 ? *end != ',' : *end != '\0') || fds[i] < 3 || fds[i] > 1000000) return false;
            at = end + 1;
        }
    }
    // each must be an open pipe: a stale name in the environment is not trusted with some other descriptor
    for (long fd : fds) {
        struct stat st;
        if (fcntl(int(fd), F_GETFD) == -1 || fstat(int(fd), &st) != 0 || !S_ISFIFO(st.st_mode)) return false;
    }
    g_game.life_r = int(fds[0]), g_game.req_w = int(fds[1]), g_game.rep_r = int(fds[2]);
    // not inherited by what this process starts (compilers at the first start): a process holding the write end of the requests would keep
    // the keeper waiting after the game has gone. Spawn passes them to the next instance alone.
    for (int fd : {g_game.life_r, g_game.req_w, g_game.rep_r}) fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(g_game.req_w, F_SETNOSIGPIPE, 1);
    g_marker = marker;
    g_active = true;
    std::thread([fd = g_game.life_r] {
        char byte;
        for (;;) {
            const ssize_t n = read(fd, &byte, 1);
            if (n == 0) _exit(kExitKeeperGone);
            if (n < 0 && errno != EINTR) return;  // not a pipe that can be watched: the game goes on
        }
    }).detach();
    return true;
}

bool Active() { return g_active; }

int Spawn(const char* path, char* const* argv, char* const* envp) {
    static std::mutex one_at_a_time;
    static uint32_t counter = 0;
    if (!g_active) {
        errno = ENOTSUP;
        return -1;
    }
    std::lock_guard<std::mutex> lock(one_at_a_time);
    uint32_t argc = 0, envc = 0;
    for (char* const* a = argv; a && *a; a++) argc++;
    for (char* const* e = envp; e && *e; e++) envc++;
    const uint32_t id = (uint32_t(getpid()) << 8) ^ ++counter;
    std::string payload(12, '\0');
    std::memcpy(&payload[0], &id, 4);
    std::memcpy(&payload[4], &argc, 4);
    std::memcpy(&payload[8], &envc, 4);
    payload.append(path).push_back('\0');
    for (char* const* a = argv; a && *a; a++) payload.append(*a).push_back('\0');
    for (char* const* e = envp; e && *e; e++) payload.append(*e).push_back('\0');
    const uint32_t length = uint32_t(payload.size());
    std::string message(4, '\0');
    std::memcpy(&message[0], &length, 4);
    message += payload;
    if (!WriteAll(g_game.req_w, message.data(), message.size())) {
        errno = EPIPE;
        return -1;
    }
    // the answer to this request (one an earlier request left unread, when it gave up, is passed over)
    for (;;) {
        struct {
            uint32_t id;
            int32_t pid;
        } reply;
        if (!ReadAll(g_game.rep_r, &reply, sizeof reply, 15000)) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (reply.id != id) continue;
        if (reply.pid > 0) return reply.pid;
        errno = reply.pid < 0 ? -reply.pid : EINVAL;
        return -1;
    }
}

void KeepAcrossExec() {
    if (!g_active) return;
    for (int fd : {g_game.life_r, g_game.req_w, g_game.rep_r}) fcntl(fd, F_SETFD, 0);
    setenv(kEnv, g_marker.c_str(), 1);
}

}  // namespace keeper
#endif
