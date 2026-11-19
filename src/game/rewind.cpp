// Takes snapshots of the guest and restores them on a test schedule, to show a rewind can be done in the process:
//   HELLMOUTH_REWIND_TEST="<snap_secs>:<restore_secs>[,...]"   pair k takes snapshot k at the first frame at or after
//       snap_secs (since the first frame) and restores it at the first frame at or after restore_secs
//   a file `rewind_cmd` in the run folder, read every 15 frames: lines `snap [N]` and `restore [N]`, N the slot (0), and
//       `strict 0|1`: 1 refuses any restore where a sound stream thread would be stopped or started again
// Each snapshot and each restore logs one line. A restore the threads or the clocks do not allow is refused, and the game goes
// on from where it was. See kernel/rewind.h for what is put back.
#include "game/rewind.h"

#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "kernel/rewind.h"
#include "xdk/hle.h"
#include "xdk/rewind.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kStreamThread = 0x670C0;  // the sound streams' MultiThread_Callback: its state is the stream object it polls

double Ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

struct Slot {
    mem::Snapshot memory;
    kernel::KernelSnapshot kernel;
    xdk::DeviceSnapshot device;
    xdk::SoundSnapshot sound;
    xdk::InputSnapshot input;
    bool valid = false;
    double taken_s = 0;
    uint64_t taken_frame = 0;
};

struct Pair {
    double snap_s, restore_s;
    bool snapped = false, restored = false;
};

struct State {
    bool enabled = false;
    std::vector<Pair> pairs;
    std::vector<std::unique_ptr<Slot>> slots;
    Clock::time_point origin;
    bool started = false;
    uint64_t frames = 0;
    int restores = 0, ok = 0, refused = 0;
    uint32_t epoch = 0;  // each restore moves it on
    bool strict = false;  // no thread may be stopped or started again: any difference of threads refuses
};
State g;

Slot& SlotAt(size_t n) {
    while (g.slots.size() <= n) g.slots.push_back(std::make_unique<Slot>());
    return *g.slots[n];
}

const char* StateName(uint8_t s) {
    using T = kernel::Thread;
    switch (s) {
        case T::kStarting: return "starting";
        case T::kRunning: return "running";
        case T::kWaiting: return "wait";
        case T::kDelaying: return "delay";
        case T::kYielding: return "yield";
        case T::kSpinning: return "lockspin";
        case T::kParked: return "parked";
    }
    return "?";
}

bool Hold(const char* what, int n, std::vector<kernel::ThreadRef>& loose) {
    if (!kernel::StopHostWriters(500)) {
        LOGW("rewind: %s %d refused: a thread of the host that writes guest memory did not finish in 500 ms", what, n);
        return false;
    }
    loose = kernel::QuiesceGuestThreads(500);
    if (!loose.empty()) {
        kernel::ResumeGuestThreads();
        kernel::StartHostWriters();
        for (const kernel::ThreadRef& t : loose)
            LOGW("rewind: %s %d refused: thread %X (start %08X) kept running for 500 ms", what, n, t->id, t->start_routine);
        return false;
    }
    return true;
}

void Release() {
    kernel::ResumeGuestThreads();
    kernel::StartHostWriters();
}

void Snapshot(size_t n, X86Context& ctx, uint64_t depth) {
    Slot& s = SlotAt(n);
    const auto t0 = Clock::now();
    std::vector<kernel::ThreadRef> loose;
    if (!Hold("snapshot", int(n), loose)) return;
    const auto t_held = Clock::now();
    xdk::CaptureDevice(s.device);
    xdk::CaptureSound(s.sound);
    xdk::CaptureInput(s.input);
    mem::CaptureSnapshot(s.memory);
    kernel::CaptureKernel(s.kernel, ctx, depth);
    const auto t_copy = Clock::now();
    Release();
    s.valid = true;
    s.taken_s = std::chrono::duration<double>(t0 - g.origin).count();
    s.taken_frame = g.frames;
    int stream = 0;
    for (const kernel::ThreadSnap& t : s.kernel.threads) stream += t.start == kStreamThread;
    LOGI("rewind: snapshot %zu at %.1f s (frame %llu): %.1f MB in %.1f ms (held %.1f ms), %zu threads (%d stream)", n,
         s.taken_s, (unsigned long long)g.frames, s.memory.data_bytes / 1048576.0, Ms(t_copy - t_held), Ms(Clock::now() - t0), s.kernel.threads.size(),
         stream);
    for (const kernel::ThreadSnap& t : s.kernel.threads)
        LOGI("rewind: snapshot %zu thread %X start %08X%s site %08X esp %08X native depth %llu%s", n, t.id, t.start, t.start == kStreamThread ? " (stream)" : "",
             t.ret, t.esp, (unsigned long long)t.depth, t.main ? " main" : "");
    logx::Flush();
}

void Restore(size_t n, X86Context& ctx, uint64_t depth) {
    g.restores++;
    const int seq = g.restores;
    if (n >= g.slots.size() || !g.slots[n]->valid) {
        g.refused++;
        LOGW("rewind: restore %d refused: no snapshot %zu", seq, n);
        return;
    }
    Slot& s = *g.slots[n];
    const auto t0 = Clock::now();
    std::vector<kernel::ThreadRef> loose;
    if (!Hold("restore", seq, loose)) {
        g.refused++;
        return;
    }
    kernel::RestorePlan plan = kernel::PlanRestore(s.kernel, ctx, depth, g.strict ? 0 : kStreamThread);
    if (!plan.refuse.empty()) {
        Release();
        g.refused++;
        LOGW("rewind: restore %d refused: %s (%.1f ms; snapshot %zu from %.1f s earlier)", seq, plan.refuse.c_str(), Ms(Clock::now() - t0), n,
             std::chrono::duration<double>(Clock::now() - g.origin).count() - s.taken_s);
        for (const kernel::ThreadRef& t : kernel::Threads())
            LOGW("rewind: restore %d thread %X start %08X %s site %08X esp %08X", seq, t->id, t->start_routine, StateName(t->kstate.load()), t->k_ret.load(),
                 t->k_esp.load());
        logx::Flush();
        return;
    }
    const auto t_plan = Clock::now();
    kernel::RestoreResult r = kernel::RestoreKernel(s.kernel, s.memory, plan, ctx);
    if (!r.error.empty()) {
        Release();
        g.refused++;
        LOGE("rewind: restore %d FAILED part way: %s", seq, r.error.c_str());
        logx::Flush();
        return;
    }
    xdk::RestoreDevice(s.device);
    xdk::RestoreSound(s.sound);
    xdk::RestoreInput(s.input);
    xdk::ForgetMovies();
    g.epoch++;
    const auto t_done = Clock::now();
    Release();
    g.ok++;
    LOGI("rewind: restore %d OK: snapshot %zu from %.1f s earlier, %.1f ms (plan %.1f, state %.1f, release %.1f), %.1f MB, threads %d restored, %d restarted, %d stopped, "
         "clock skew %.1f ms",
         seq, n, std::chrono::duration<double>(t0 - g.origin).count() - s.taken_s, Ms(Clock::now() - t0), Ms(t_plan - t0), Ms(t_done - t_plan),
         Ms(Clock::now() - t_done), r.bytes / 1048576.0, r.restored, r.restarted, r.stopped, r.skew_ns / 1e6);
    for (const kernel::ThreadRef& t : plan.stop) LOGI("rewind: restore %d stopped thread %X (start %08X)", seq, t->id, t->start_routine);
    for (size_t i : plan.restart) LOGI("rewind: restore %d started thread %X again (start %08X, parameter %08X)", seq, s.kernel.threads[i].id, s.kernel.threads[i].start, s.kernel.threads[i].start_context);
    logx::Flush();
}

// Test only: the player dies as ICharacter::Event_Die makes it, by calling DieAsPlayer (vtable +0x50C) with a zeroed event, so that the
// game spawns its death manager, which fades out and shows the death screen (TMenuResurrect), whose Continue calls PlayerDeathReload.
// (Damage alone did not do it: ICharacter::Health_Change took the health to 0 and the player stood there; and Event_InitiateDeath
// with an event of zeros stopped the game.)
void Kill(X86Context& ctx) {
    const uint32_t game = kernel::R32(0x7D0090), player = game ? kernel::R32(game + 0xF0) : 0;
    if (!player) {
        LOGW("rewind: kill: there is no player");
        return;
    }
    const uint32_t sp = ctx.esp;
    ctx.esp -= 32;
    for (uint32_t i = 0; i < 8; i++) kernel::W32(ctx.esp + 4 * i, 0);
    const uint32_t event = ctx.esp;
    ctx.ecx = player;  // restored by CallGuest
    kernel::CallGuest(ctx, kernel::R32(kernel::R32(player) + 0x50C), {event});
    ctx.esp = sp;
    LOGI("rewind: kill: the player %08X dies", player);
}

// `snap` or `restore`, then the slot
bool Command(X86Context& ctx, uint64_t depth) {
    std::ifstream f("rewind_cmd");
    if (!f) return false;
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(f, line)) lines.push_back(line);
    f.close();
    std::remove("rewind_cmd");
    for (const std::string& l : lines) {
        char word[32] = {};
        int n = 0;
        if (std::sscanf(l.c_str(), "%31s %d", word, &n) < 1 || n < 0) continue;
        if (std::string(word) == "strict") {
            g.strict = n != 0;
            LOGI("rewind: strict %d (%s)", n, g.strict ? "a restore that would stop or start a sound stream thread is refused" : "a sound stream thread may be stopped or started again");
        } else if (std::string(word) == "kill") {
            Kill(ctx);
        } else if (std::string(word) == "snap") Snapshot(size_t(n), ctx, depth);
        else if (std::string(word) == "restore") Restore(size_t(n), ctx, depth);
    }
    return true;
}

bool Init() {
    const char* e = std::getenv("HELLMOUTH_REWIND_TEST");
    if (!e) return false;
    g.enabled = true;
    kernel::EnableRewinding();  // from the start: a host writer begun before it is on would not be waited for
    std::string v = e;
    size_t at = 0;
    while (at < v.size()) {
        size_t comma = v.find(',', at);
        if (comma == std::string::npos) comma = v.size();
        double a, b;
        if (std::sscanf(v.substr(at, comma - at).c_str(), "%lf:%lf", &a, &b) == 2) g.pairs.push_back({a, b});
        at = comma + 1;
    }
    return true;
}
const bool g_on = Init();  // before the game runs: the gate is on from the first thread

}  // namespace

namespace snapback {

bool Enabled() { return g_on; }
uint32_t Epoch() { return g.epoch; }

void Frame(X86Context& ctx, uint64_t depth) {
    const auto now = Clock::now();
    if (!g.started) {
        g.started = true;
        g.origin = now;
        LOGI("rewind: test on, %zu scheduled pairs, commands from the file rewind_cmd", g.pairs.size());  // (the log is not open when Init runs)
    }
    g.frames++;
    if (g.frames % 15 == 0 && Command(ctx, depth)) return;  // one thing a frame
    const double t = std::chrono::duration<double>(now - g.origin).count();
    for (size_t i = 0; i < g.pairs.size(); i++) {
        Pair& p = g.pairs[i];
        if (!p.snapped && t >= p.snap_s) {
            p.snapped = true;
            Snapshot(i, ctx, depth);
            return;
        }
        if (p.snapped && !p.restored && t >= p.restore_s) {
            p.restored = true;
            Restore(i, ctx, depth);
            return;
        }
    }
}

}  // namespace snapback

// TGameManagerImplementation::Frame, from ExecuteGame's loop only: Swap and the other callers come from loading and menus
constexpr uint32_t kFrameReturn = 0x6423B;  // the `call [eax+0x14]` in ExecuteGame's loop at 0x64238
X86_EXTERN_FUNC(__imp__sub_000183A0);
X86_FUNC(sub_000183A0) {
    if (snapback::Enabled() && kernel::R32(ctx.esp) == kFrameReturn) {
        // the native stack in use here, the same at every call of this: a restore only goes back to the same call chain
        kernel::Thread* self = kernel::Current();
        snapback::Frame(ctx, self && self->host_base ? self->host_base - uintptr_t(__builtin_frame_address(0)) : 0);
    }
    __imp__sub_000183A0(ctx, base);
}
