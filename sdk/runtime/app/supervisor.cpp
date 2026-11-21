// The supervisor: a start by RecompLauncher runs the game as a child of this same executable, inside a job object that only this
// process holds.
//
// Why: the game starts a new process at each level load (kernel::Relaunch) and the old one exits, while RecompLauncher keeps the
// process it started and stops only that one, so after the first level load its Stop would find nothing to stop. The supervisor is
// that process for the whole session: it lives until the last process of the chain has gone, and when it is killed (by Stop) the job
// handle closes and Windows ends every process in the job (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE). Each relaunched process joins the
// job because a child inherits it (Relaunch does not ask to break away). Its exit code is the last game process's, since the ones
// before it exit 0 at the handover.
//
// It runs at the very top of wWinMain and loads nothing: no log, settings, window or game code. Only a normal play start under
// RecompLauncher is supervised (supervisor_args.h); if anything fails before the child exists, the game runs in this process as it
// would without a supervisor. It never shows a message box. Windows only: a Mac's counterpart is the keeper (kernel/keeper.h), which
// the decision of supervisor_args.h also serves.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "app/app.h"
#include "app/supervisor_args.h"

namespace app {

namespace {

const wchar_t kMarker[] = L"HELLMOUTH_SUPERVISED";  // in the game process's environment: it is not to supervise again

bool EnvironmentIs(const wchar_t* name, wchar_t want) {
    wchar_t value[4];
    const DWORD n = GetEnvironmentVariableW(name, value, DWORD(sizeof(value) / sizeof(value[0])));
    return n == 1 && value[0] == want;
}

bool EnvironmentSet(const wchar_t* name) { return GetEnvironmentVariableW(name, nullptr, 0) != 0; }

// a few lines in <data>\logs\supervisor.log (the game owns buffy.log); best effort, and nothing if the folder cannot be made
class Note {
public:
    Note() {
        std::error_code ec;
        const std::filesystem::path folder = LogFolder();
        std::filesystem::create_directories(folder, ec);
        const std::wstring name = Instance() > 1 ? L"supervisor-" + std::to_wstring(Instance()) + L".log" : L"supervisor.log";
        file_ = CreateFileW((folder / name).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        began_ = GetTickCount64();
    }
    ~Note() {
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    }
    Note(const Note&) = delete;
    Note& operator=(const Note&) = delete;
    void operator()(const char* format, unsigned long a = 0, unsigned long b = 0) {
        if (file_ == INVALID_HANDLE_VALUE) return;
        char line[160];
        const int head = std::snprintf(line, sizeof line, "%8llu ms  ", (unsigned long long)(GetTickCount64() - began_));
        const int body = std::snprintf(line + head, sizeof line - size_t(head) - 1, format, a, b);
        if (body < 0) return;
        const size_t n = std::min(sizeof line - 2, size_t(head + body));
        line[n] = '\n';
        DWORD written = 0;
        WriteFile(file_, line, DWORD(n + 1), &written, nullptr);
    }

private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
    ULONGLONG began_ = 0;
};

// the exit code of a process, once it has ended (the job's message can come a moment before the process object is signalled)
std::optional<DWORD> ExitCodeOf(HANDLE process, DWORD wait_ms) {
    DWORD code = 0;
    if (WaitForSingleObject(process, wait_ms) == WAIT_OBJECT_0 && GetExitCodeProcess(process, &code)) return code;
    return std::nullopt;
}

// the processes the job has now; all ones if that cannot be asked
DWORD ActiveProcesses(HANDLE job) {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
    return QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &info, sizeof info, nullptr) ? info.ActiveProcesses : DWORD(-1);
}

// Runs until the job has no process left; the code of the last one that ended. nullopt: none could be read.
std::optional<DWORD> WaitForJob(HANDLE job, HANDLE port, const PROCESS_INFORMATION& first, Note& note) {
    std::map<DWORD, HANDLE> known;  // the processes of the job whose exit code is still to be read
    known[first.dwProcessId] = first.hProcess;
    std::optional<DWORD> first_code, last_code;
    bool none_left = false;  // the job said so: what is still queued (an exit that came after it) is read, then done
    for (;;) {
        DWORD message = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* data = nullptr;
        // the job's messages are not guaranteed: with none for a while the process count is asked, so a lost one cannot leave this waiting for ever
        if (!GetQueuedCompletionStatus(port, &message, &key, &data, none_left ? 200 : 2000)) {
            const DWORD error = GetLastError();
            if (!data && error == WAIT_TIMEOUT) {
                if (none_left) break;
                if (ActiveProcesses(job) == 0) none_left = true;
                continue;
            }
            note("the job's port failed (%lu): polling", error);
            for (DWORD n; (n = ActiveProcesses(job)) != 0 && n != DWORD(-1);) Sleep(500);
            break;
        }
        const DWORD pid = DWORD(reinterpret_cast<ULONG_PTR>(data));
        if (message == JOB_OBJECT_MSG_NEW_PROCESS) {
            note("process %lu joined", pid);
            if (!known.count(pid))
                if (HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) known[pid] = h;
        } else if (message == JOB_OBJECT_MSG_EXIT_PROCESS || message == JOB_OBJECT_MSG_ABNORMAL_EXIT_PROCESS) {
            std::optional<DWORD> code;
            if (auto it = known.find(pid); it != known.end()) {
                code = ExitCodeOf(it->second, 5000);
                CloseHandle(it->second);
                known.erase(it);
            }
            note(message == JOB_OBJECT_MSG_EXIT_PROCESS ? "process %lu exited, code %lu" : "process %lu exited abnormally, code %lu", pid,
                 code ? *code : 0xFFFFFFFFul);
            if (pid == first.dwProcessId) first_code = code;
            if (code) last_code = code;
        } else if (message == JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO) {
            none_left = true;
        }
    }
    for (auto& entry : known) CloseHandle(entry.second);
    return last_code ? last_code : first_code;
}

}  // namespace

void Supervise() {
    if (!ShouldSupervise(GetCommandLineW(), EnvironmentIs(L"RECOMPLAUNCHER", L'1'), EnvironmentSet(kMarker))) return;

    HANDLE job = CreateJobObjectW(nullptr, nullptr);  // no security attributes: the handle cannot be inherited
    if (!job) return;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT association{job, port};  // the key is the job's handle value
    if (!port || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits) ||
        !SetInformationJobObject(job, JobObjectAssociateCompletionPortInformation, &association, sizeof association)) {
        if (port) CloseHandle(port);
        CloseHandle(job);
        return;
    }

    // this executable again, with the same command line; the marker goes into its environment, which is a copy of this one's
    std::vector<wchar_t> exe(32768);
    const DWORD exe_length = GetModuleFileNameW(nullptr, exe.data(), DWORD(exe.size()));
    const wchar_t* line = GetCommandLineW();
    std::vector<wchar_t> command(line, line + wcslen(line) + 1);  // CreateProcessW may write to it
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    bool created = false;
    if (exe_length > 0 && exe_length < exe.size() && SetEnvironmentVariableW(kMarker, L"1"))
        created = CreateProcessW(exe.data(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &startup, &child);
    if (!created) {
        SetEnvironmentVariableW(kMarker, nullptr);
        CloseHandle(port);
        CloseHandle(job);
        return;  // the game runs here, as it would without a supervisor
    }

    Note note;
    note("supervising process %lu", child.dwProcessId);
    const bool joined = AssignProcessToJobObject(job, child.hProcess) != 0;
    AllowSetForegroundWindow(child.dwProcessId);
    if (ResumeThread(child.hThread) == DWORD(-1)) {
        note("the game process could not be resumed (%lu)", GetLastError());
        TerminateProcess(child.hProcess, kExitBootFailed);
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        CloseHandle(port);
        CloseHandle(job);
        SetEnvironmentVariableW(kMarker, nullptr);
        return;
    }
    CloseHandle(child.hThread);

    std::optional<DWORD> code;
    if (joined) {
        code = WaitForJob(job, port, child, note);
    } else {
        // not in the job: no better than without a supervisor, but the code is still passed on
        note("the game process could not join the job (%lu): waiting for it alone", GetLastError());
        code = ExitCodeOf(child.hProcess, INFINITE);
        CloseHandle(child.hProcess);
    }
    note("done, exit code %lu", code ? *code : DWORD(kExitOk));
    ExitProcess(code ? *code : DWORD(kExitOk));
}

}  // namespace app
#endif
