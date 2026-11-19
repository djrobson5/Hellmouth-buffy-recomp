// Mid-level checkpoints: when Buffy dies and the player chooses to continue on the death screen, the game goes back to a recent
// snapshot of the whole guest kept in memory, in place of reloading the level from its start. See kernel/rewind.h for what a
// snapshot holds and what a restore puts back.
//
// A snapshot ("rewind point") is taken at the top of TGameManagerImplementation::Frame (called from ExecuteGame's loop only):
// about every minute of a level, at a frame where it is safe (the player alive, on the ground, unhurt for a few seconds, no menu,
// cutscene or movie). The last two are kept. TGO::PlayerDeathReload, which the death screen's Continue calls, is taken over when
// there is a point: the call returns at once and the restore is done at the next Frame. When there is none, or the restore
// cannot be done, the original PlayerDeathReload runs and the game reloads the level as it always did.
//
// Test triggers (tests only, work whatever the setting):
//   HELLMOUTH_REWIND_TEST="<snap_secs>:<restore_secs>[,...]"   pair k takes test snapshot k at the first frame at or after
//       snap_secs (since the first frame) and restores it at the first frame at or after restore_secs
//   a file `rewind_cmd` in the run folder, read every 15 frames, one line each: `snap [N]` and `restore [N]` (test slot N, 0),
//       `strict 0|1` (1: a restore that would stop or start a sound stream thread is refused), `kill` (the player is hit hard
//       through the game's own damage, so the death screen comes), `failplan 0|1` (1: the plan made at the next Frame refuses, as
//       if a thread had changed since the death), `cadence <secs>` (the time between rewind points), `failstop` (the next restore that has to stop a thread fails once that thread has gone: the failure path puts it back), `checkpoint` (one at the
//       next Frame, safe or not), `state` (logs the player's fields the safe test reads)
//   HELLMOUTH_REWIND_VERIFY=1 after each restore, the guest's memory is compared with the snapshot's (logged: "verified" or "VERIFY")
//   HELLMOUTH_CHECKPOINT_SECS=<n> the time between rewind points; HELLMOUTH_CHECKPOINT_NOPREFAULT=1 leaves the buffers to be made at
//       the first snapshots (to measure what the prefault saves)
// Each rewind point taken, each rewind and each fallback logs one line, with the reason.
#include "game/rewind.h"

#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/log.h"
#include "game/extras.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "kernel/rewind.h"
#include "xdk/hle.h"
#include "xdk/rewind.h"

namespace snapback {
bool g_checkpoints = true;
}

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kStreamThread = 0x670C0;  // the sound streams' MultiThread_Callback: its state is the stream object it polls
constexpr uint32_t kFrameReturn = 0x6423B;   // the `call [eax+0x14]` in ExecuteGame's loop at 0x64238
constexpr uint32_t kPlayerDeathReload = 0x939C0;

// the game's own words for what the safe test reads
constexpr uint32_t kGame = 0x7D0090;         // TGame::gGame: +0x10 frozen, +0x40 map manager (+0x3E map active), +0xF0 the player
constexpr uint32_t kSlayerVtable = 0x67A0CC;
constexpr uint32_t kCineRunning = 0x943E0;   // TGO::Cine_IsCutsceneRunning
constexpr uint32_t kMovieThreadUp = 0x7715E0, kMovieCurrent = 0x7F30E8;  // what Movie_Playing reads (widescreen.cpp)

constexpr double kQuietAfterHurt = 3.0;  // seconds with no damage before a point may be taken
constexpr double kRecentRewind = 10.0;   // a death this soon after a rewind goes back to the older point

double Ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }
double Secs(Clock::duration d) { return std::chrono::duration<double>(d).count(); }

struct Slot {
    mem::Snapshot memory;
    kernel::KernelSnapshot kernel;
    xdk::DeviceSnapshot device;
    xdk::SoundSnapshot sound;
    xdk::InputSnapshot input;
    bool valid = false;
    Clock::time_point taken;
    uint64_t taken_frame = 0;
    float pos[3] = {};
    int number = 0;  // the checkpoint's number in this level
    // lets go of what the snapshot holds but the memory buffer (kept to be filled again): the kernel's objects, open files and thread
    // references, the device, sound and input state
    void Invalidate() {
        valid = false;
        kernel.Clear();
        device.Clear();
        sound.Clear();
        input.Clear();
    }
};

struct Pair {
    double snap_s, restore_s;
    bool snapped = false, restored = false;
};

struct Pending {
    bool on = false;
    int slot = 0;
    uint32_t arg = 0;  // PlayerDeathReload's argument
    uint64_t frames = 0;
};

struct State {
    bool active = false;  // the hooks do something: checkpoints on, or the test triggers
    bool test = false;    // the test triggers
    bool on = false;      // checkpoints on in this process (the setting, read at start)
    std::mutex lock;      // the slots: the Frame hook and a relaunch (another guest thread) both reach them
    kernel::Thread* main = nullptr;
    // the test slots
    std::vector<Pair> pairs;
    std::vector<std::unique_ptr<Slot>> slots;
    int restores = 0, ok = 0, refused = 0;
    // the rewind points: two slots, `order` from the oldest to the newest
    Slot point[2];
    std::vector<int> order;
    int taken = 0, rewinds = 0, fallbacks = 0, failures = 0;
    Pending pending;
    bool bypass = false;  // a fallback ran the original: later calls go to it too
    Clock::time_point origin, level_start, next_at, last_rewind, last_death, last_hurt;
    bool have_rewound = false, have_died = false;
    int report_gate = 0;  // a point whose hold's cost to the host's writers is to be logged at the next Frame
    bool in_level = false;  // MapSet has run: the front end has no map, and no rewind points
    uint64_t level_frames = 0;
    bool dead = false;  // DieAsPlayer has run and the death has not been settled (by a rewind or a reload): no point is taken
    int last_rewind_slot = -1;
    double interval = 60;
    bool prefault = true;
    bool verify = false;  // test: after a restore, compare the guest's memory with the snapshot's
    float last_hp = 0;
    uint64_t safe_frames = 0;
    std::string unsafe;  // the last reason a point was not taken, for the log
    bool started = false, want_checkpoint = false, force_refuse = false;
    uint64_t frames = 0;
    uint32_t epoch = 0;
    bool strict = false;
    uint64_t prefault_bytes = 0;
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

uint8_t R8(uint32_t va) { return *static_cast<uint8_t*>(kernel::HostPtr(va)); }
float RF(uint32_t va) { return std::bit_cast<float>(kernel::R32(va)); }

// ---- the player. Fields found by dumping the player (vtable 0x67A0CC, from TGO::GetPlayer, [gGame+0xF0]) while it stood, ran, jumped
// and was hit, and by reading what the game's own functions read (TGO::PlayerDeathReload, ICharacter::Health_Change, IObject::
// IsDeathInitiated, TAI::InAir); docs/xdk-4432.md says how each was checked.
uint32_t Player() {
    const uint32_t game = kernel::R32(kGame);
    if (!game) return 0;
    const uint32_t p = kernel::R32(game + 0xF0);
    return p && kernel::R32(p) == kSlayerVtable ? p : 0;
}
bool MapActive() {
    const uint32_t game = kernel::R32(kGame);
    const uint32_t maps = game ? kernel::R32(game + 0x40) : 0;
    return maps && R8(maps + 0x3E);
}
// TCharState::Category (0x6A1B0) of the character's state machine: the state's flags, less the machine's masked ones. TAI::InAir
// reads bits 0x80 and 0x08 of it
uint32_t Category(uint32_t player) {
    const uint32_t machine = kernel::R32(player + 0x4BC);
    if (!machine) return 0;
    uint32_t state = kernel::R32(machine + 0x28);
    if (!state) state = kernel::R32(machine + 0x30);
    if (!state) return 0;
    return (kernel::R32(machine + 0xE0) | kernel::R32(state + 0x34)) & ~kernel::R32(machine + 0xE4);
}
bool InAir(uint32_t player) { return (Category(player) & 0x88) != 0; }
// Standing, walking or running, the only states seen on the ground with nothing else going on: idle 0x1400, walking 0x100, running
// 0x01000500. A jump's take-off (0x21) is already off the ground before the in-air flags (0x88) are set, so what is allowed is listed,
// not what is not; 0 is a state still being entered
constexpr uint32_t kCalm = 0x01001500;
bool Calm(uint32_t player) {
    const uint32_t c = Category(player);
    return c != 0 && (c & ~kCalm) == 0;
}
float Health(uint32_t player) { return RF(player + 0x90); }
bool Dying(uint32_t player) { return R8(player + 0x1F7) != 0; }  // IObject::IsDeathInitiated

// ---- the safe test. Null when a point may be taken now, else why not.
const char* Unsafe(X86Context& ctx, uint32_t player) {
    if (g.dead) return "the player has died";
    if (!MapActive()) return "no map is active";
    const uint32_t game = kernel::R32(kGame);
    if (!player) return "no player";
    if (R8(game + 0x10)) return "the game is frozen";
    if (Dying(player) || Health(player) <= 0) return "the player is dying";
    if (InAir(player)) return "the player is in the air";
    if (!Calm(player)) return "the player is not standing, walking or running";
    if (kernel::R32(kMovieThreadUp) && kernel::R32(kMovieCurrent)) return "a movie is playing";
    // cutscenes, talk scenes and the pause menu all read as running here (the pause menu was seen to)
    if (kernel::CallGuest(ctx, kCineRunning, {}) & 0xFF) return "a cutscene, talk scene or menu is up";
    return nullptr;
}

// ---- holding the guest still
// The guest held still for as long as this lives (an exception leaves it let go, not held for ever). The time is how long to wait for
// the host's writers and for the guest's threads to stop: a restore may wait for long, a checkpoint only a frame or two.
struct Held {
    bool on = false;
    Held(const char* what, int n, int timeout_ms) {
        if (!kernel::StopHostWriters(timeout_ms)) {
            LOGW("rewind: %s %d refused: a thread of the host that writes guest memory did not finish in %d ms", what, n, timeout_ms);
            return;
        }
        const std::vector<kernel::ThreadRef> loose = kernel::QuiesceGuestThreads(timeout_ms);
        if (!loose.empty()) {
            kernel::ResumeGuestThreads();
            kernel::StartHostWriters();
            for (const kernel::ThreadRef& t : loose)
                LOGW("rewind: %s %d refused: thread %X (start %08X) kept running for %d ms", what, n, t->id, t->start_routine, timeout_ms);
            return;
        }
        on = true;
    }
    ~Held() { Release(); }
    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;
    void Release() {
        if (!on) return;
        on = false;
        kernel::ResumeGuestThreads();
        kernel::StartHostWriters();
    }
    explicit operator bool() const { return on; }
};

constexpr int kRestoreHoldMs = 500;     // a restore is worth waiting for
constexpr int kCheckpointHoldMs = 25;   // a checkpoint is not: its two waits together stay inside a frame or two

// Takes a snapshot into the slot; false when the guest could not be held still (the slot is left as it was)
struct Took {
    double total_ms = 0, held_ms = 0, copy_ms = 0;
    bool ok = false;
};
Took Capture(Slot& s, const char* what, int n, X86Context& ctx, uint64_t depth, int hold_ms) {
    Took r;
    const auto t0 = Clock::now();
    kernel::TakeGateWaitNs();  // what is measured below is this hold's
    Held hold(what, n, hold_ms);
    if (!hold) return r;
    const auto t_held = Clock::now();
    s.Invalidate();
    xdk::CaptureDevice(s.device);
    xdk::CaptureSound(s.sound);
    xdk::CaptureInput(s.input);
    // the guest's memory is copied under the lock the kernel's objects are taken under, so the two agree
    kernel::CaptureKernel(s.kernel, ctx, depth, [&] { mem::CaptureSnapshot(s.memory); });
    const auto t_copy = Clock::now();
    hold.Release();
    s.valid = true;
    s.taken = t0;
    s.taken_frame = g.frames;
    if (const uint32_t p = Player())
        for (int i = 0; i < 3; i++) s.pos[i] = RF(p + 0x100 + 4 * uint32_t(i));
    r.ok = true;
    r.total_ms = Ms(Clock::now() - t0);
    r.held_ms = Ms(t_held - t0);
    r.copy_ms = Ms(t_copy - t_held);
    return r;
}

int StreamThreads(const Slot& s) {
    int n = 0;
    for (const kernel::ThreadSnap& t : s.kernel.threads) n += t.start == kStreamThread;
    return n;
}

void LogThreads(const char* what, int n, const Slot& s) {
    for (const kernel::ThreadSnap& t : s.kernel.threads)
        LOGI("rewind: %s %d thread %X start %08X%s site %08X esp %08X native depth %llu%s", what, n, t.id, t.start, t.start == kStreamThread ? " (stream)" : "", t.ret,
             t.esp, (unsigned long long)t.depth, t.main ? " main" : "");
}

// Restores the slot; an empty string when it was done, else why not (nothing was changed)
struct Put {
    std::string refuse;
    kernel::RestoreResult result;
    kernel::RestorePlan plan;
    double total_ms = 0, plan_ms = 0, state_ms = 0, release_ms = 0;
};
Put Restore(Slot& s, const char* what, int n, X86Context& ctx, uint64_t depth) {
    Put r;
    const auto t0 = Clock::now();
    Held hold(what, n, kRestoreHoldMs);
    if (!hold) {
        r.refuse = "the guest could not be held still";
        return r;
    }
    // a render thread may still be reading or writing guest pages: it finishes while the guest is held, before memory is replaced
    xdk::FlushDevice();
    r.plan = kernel::PlanRestore(s.kernel, ctx, depth, g.strict ? 0 : kStreamThread);
    if (r.plan.refuse.empty() && g.force_refuse) {
        r.plan.refuse = "refused on the test's order (failplan)";
        g.force_refuse = false;
    }
    if (!r.plan.refuse.empty()) {
        hold.Release();
        r.refuse = r.plan.refuse;
        for (const kernel::ThreadRef& t : kernel::Threads())
            LOGW("rewind: %s %d thread %X start %08X %s site %08X esp %08X", what, n, t->id, t->start_routine, StateName(t->kstate.load()), t->k_ret.load(), t->k_esp.load());
        return r;
    }
    const auto t_plan = Clock::now();
    r.result = kernel::RestoreKernel(s.kernel, s.memory, r.plan, ctx);
    if (!r.result.error.empty()) {
        hold.Release();
        r.refuse = "the restore failed part way: " + r.result.error;
        return r;
    }
    if (g.verify) {  // the guest is held and nothing has run since: it must be what the snapshot held
        uint32_t first = 0;
        const uint64_t diff = mem::DiffSnapshot(s.memory, &first);
        if (diff) LOGE("rewind: %s %d VERIFY: %llu bytes differ from the snapshot after the restore, the first at %08X", what, n, (unsigned long long)diff, first);
        else LOGI("rewind: %s %d verified: guest memory is the snapshot's", what, n);
    }
    xdk::RestoreDevice(s.device);
    xdk::RestoreSound(s.sound);
    xdk::RestoreInput(s.input);
    xdk::ForgetMovies();
    g.epoch++;
    const auto t_done = Clock::now();
    hold.Release();
    r.total_ms = Ms(Clock::now() - t0);
    r.plan_ms = Ms(t_plan - t0);
    r.state_ms = Ms(t_done - t_plan);
    r.release_ms = Ms(Clock::now() - t_done);
    return r;
}

void LogRestored(const char* what, int seq, const Slot& s, const Put& p) {
    const kernel::RestoreResult& r = p.result;
    LOGI("%s %d: %.1f ms (plan %.1f, state %.1f, release %.1f), %.1f MB, threads %d restored, %d restarted, %d stopped, clock skew %.1f ms", what, seq, p.total_ms, p.plan_ms,
         p.state_ms, p.release_ms, r.bytes / 1048576.0, r.restored, r.restarted, r.stopped, r.skew_ns / 1e6);
    for (const kernel::ThreadRef& t : p.plan.stop) LOGI("rewind: %s %d stopped thread %X (start %08X)", what, seq, t->id, t->start_routine);
    for (size_t i : p.plan.restart)
        LOGI("rewind: %s %d started thread %X again (start %08X, parameter %08X)", what, seq, s.kernel.threads[i].id, s.kernel.threads[i].start, s.kernel.threads[i].start_context);
}

// ---- the test slots (rewind_cmd, HELLMOUTH_REWIND_TEST)
void TestSnapshot(size_t n, X86Context& ctx, uint64_t depth) {
    Slot& s = SlotAt(n);
    const Took t = Capture(s, "snapshot", int(n), ctx, depth, kRestoreHoldMs);
    if (!t.ok) return;
    LOGI("rewind: snapshot %zu at %.1f s (frame %llu): %.1f MB in %.1f ms (held %.1f ms), %zu threads (%d stream)", n, Secs(s.taken - g.origin), (unsigned long long)g.frames,
         s.memory.data_bytes / 1048576.0, t.copy_ms, t.total_ms, s.kernel.threads.size(), StreamThreads(s));
    LogThreads("snapshot", int(n), s);
    logx::Flush();
}

void TestRestore(size_t n, X86Context& ctx, uint64_t depth) {
    g.restores++;
    const int seq = g.restores;
    if (n >= g.slots.size() || !g.slots[n]->valid) {
        g.refused++;
        LOGW("rewind: restore %d refused: no snapshot %zu", seq, n);
        return;
    }
    Slot& s = *g.slots[n];
    const Put p = Restore(s, "restore", seq, ctx, depth);
    if (!p.refuse.empty()) {
        g.refused++;
        LOGW("rewind: restore %d refused: %s (snapshot %zu from %.1f s earlier)", seq, p.refuse.c_str(), n, Secs(Clock::now() - s.taken));
        logx::Flush();
        return;
    }
    g.ok++;
    char head[64];
    std::snprintf(head, sizeof head, "rewind: restore %d OK: snapshot %zu from %.1f s earlier,", seq, n, Secs(Clock::now() - s.taken));
    LOGI("%s", head);
    LogRestored("rewind: restore", seq, s, p);
    logx::Flush();
}

// Test only: the player dies as ICharacter::Event_Die makes it, by calling DieAsPlayer (vtable +0x50C) with a zeroed event, so that the
// game spawns its death manager, which fades out and shows the death screen (TMenuResurrect), whose Continue calls PlayerDeathReload.
// It is called from inside the player's own Tick (the hook at the end of this file), where the game makes its deaths: called from the
// top of Frame, outside any tick, it spawned the manager into lists the object manager was not ready for and the game crashed. (Damage
// alone did not do it either: ICharacter::Health_Change took the health to 0 and the player stood there; and Event_InitiateDeath
// with an event of zeros stopped the game.)
std::atomic<bool> g_kill_pending{false};

void Kill() {
    LOGI("rewind: kill: the player dies at its next Tick");
    g_kill_pending = true;
}

void KillNow(X86Context& ctx) {
    g_kill_pending = false;
    const uint32_t player = ctx.ecx, sp = ctx.esp;
    ctx.esp -= 32;
    for (uint32_t i = 0; i < 8; i++) kernel::W32(ctx.esp + 4 * i, 0);
    const uint32_t event = ctx.esp;
    kernel::CallGuest(ctx, kernel::R32(kernel::R32(player) + 0x50C), {event});  // thiscall: ecx is the player, as the Tick was called
    ctx.esp = sp;
    LOGI("rewind: kill: the player %08X dies", player);
}

void LogState(X86Context& ctx) {
    const uint32_t player = Player();
    const char* why = Unsafe(ctx, player);
    LOGI("rewind: state: map active %d, player %08X, health %.1f, dying %d, category %08X (in air %d), cutscene %u, movie %d; unsafe: %s", MapActive(), player,
         player ? Health(player) : 0.0f, player ? Dying(player) : 0, player ? Category(player) : 0, player ? InAir(player) : 0, kernel::CallGuest(ctx, kCineRunning, {}) & 0xFF,
         kernel::R32(kMovieThreadUp) && kernel::R32(kMovieCurrent), why ? why : "none");
}

// ---- the rewind points
void DropPoints(const char* why) {
    int dropped = 0;
    for (Slot& s : g.point) {
        dropped += s.valid;
        s.Invalidate();
        mem::ReleaseSnapshot(s.memory);
    }
    g.order.clear();
    g.pending = {};
    g.have_rewound = false;
    LOGI("rewind: %s: %d rewind point%s dropped, the buffers freed", why, dropped, dropped == 1 ? "" : "s");
}

void TakePoint(X86Context& ctx, uint64_t depth, const char* why) {
    int idx;
    if (g.order.size() < 2) idx = g.order.empty() ? 0 : 1 - g.order[0];
    else idx = g.order[0];  // the older one is given up
    Slot& s = g.point[idx];
    const bool had = s.valid;
    const Took t = Capture(s, "checkpoint", g.taken + 1, ctx, depth, kCheckpointHoldMs);
    if (!t.ok) {
        // the guest was busy: ask again later, and later still each time it fails (2, 4, 8 ... 30 s), so a bad stretch costs little
        g.failures++;
        const int wait_s = std::min(30, 1 << std::min(g.failures, 5));
        g.next_at = Clock::now() + std::chrono::seconds(wait_s);
        LOGI("rewind: checkpoint attempt %d failed (the guest did not stop within %d ms); the next try in %d s", g.failures, kCheckpointHoldMs, wait_s);
        return;
    }
    g.failures = 0;
    if (had) g.order.erase(g.order.begin());
    g.order.push_back(idx);
    s.number = ++g.taken;
    g.report_gate = s.number;
    g.next_at = Clock::now() + std::chrono::milliseconds(int64_t(g.interval * 1000));
    const uint32_t p = Player();
    LOGI("rewind: checkpoint %d taken (%s) %.1f s into the level, frame %llu, player at %.2f %.2f %.2f, category %08X, health %.0f, unhurt for %.1f s: %.1f MB in %.1f ms (copy %.1f ms, "
         "held %.1f ms), buffer %s, %zu threads (%d stream), %zu kept",
         s.number, why, Secs(s.taken - g.level_start), (unsigned long long)g.frames, s.pos[0], s.pos[1], s.pos[2], p ? Category(p) : 0, p ? Health(p) : 0.0f, Secs(s.taken - g.last_hurt),
         s.memory.data_bytes / 1048576.0, t.total_ms, t.copy_ms, t.held_ms, s.memory.reused ? "reused" : "newly made", s.kernel.threads.size(), StreamThreads(s), g.order.size());
    logx::Flush();
}

// which point a death goes back to: -1 for none
int ChoosePoint(const char** why) {
    if (g.order.empty()) {
        *why = "there is no rewind point yet";
        return -1;
    }
    const int newest = g.order.back(), oldest = g.order.front();
    if (g.have_rewound && g.order.size() == 2 && g.last_rewind_slot == newest) {
        const Clock::time_point died = g.have_died && g.last_death >= g.last_rewind ? g.last_death : Clock::now();
        if (Secs(died - g.last_rewind) < kRecentRewind) {
            *why = "the death came within 10 s of the last rewind";
            return oldest;
        }
    }
    *why = "the newest point";
    return newest;
}

// Between PlayerDeathReload and the next Frame; true when the call is taken over (the caller returns at once)
bool Death(X86Context& ctx, uint32_t arg) {
    if (!g.on || g.bypass || kernel::Current() != g.main) return false;
    std::lock_guard<std::mutex> lock(g.lock);
    if (g.pending.on) return true;  // the choice was made twice in a frame
    // only a death goes back: DieAsPlayer has run, or the player is dying. Any other caller gets the game's own call.
    if (!g.dead && !(Player() && Dying(Player()))) {
        LOGI("rewind: PlayerDeathReload was called with the player not dead: left to the game");
        return false;
    }
    const char* why = "";
    const int idx = ChoosePoint(&why);
    if (idx < 0) {
        g.fallbacks++;
        LOGI("rewind: fallback %d: %s: the game reloads the level from its start", g.fallbacks, why);
        return false;
    }
    // the threads must be as they were; the main thread's place is checked at the Frame, where the restore is made
    {
        Held hold("rewind", g.rewinds + 1, kRestoreHoldMs);
        if (!hold) {
            g.fallbacks++;
            LOGW("rewind: fallback %d: the guest could not be held still to check: the game reloads the level from its start", g.fallbacks);
            return false;
        }
        const kernel::RestorePlan plan = kernel::PlanRestore(g.point[idx].kernel, ctx, 0, g.strict ? 0 : kStreamThread, false);
        hold.Release();
        if (!plan.refuse.empty()) {
            g.fallbacks++;
            LOGI("rewind: fallback %d: %s: the game reloads the level from its start", g.fallbacks, plan.refuse.c_str());
            return false;
        }
    }
    g.pending = {true, idx, arg, 0};
    LOGI("rewind: death: going back to point %d (%.1f s old, %s) at the next frame", g.point[idx].number, Secs(Clock::now() - g.point[idx].taken), why);
    return true;
}

// at the Frame: the restore a death asked for, or the original PlayerDeathReload when it cannot be done
void Rewind(X86Context& ctx, uint64_t depth) {
    const Pending p = g.pending;
    std::string refuse;
    {
        std::lock_guard<std::mutex> lock(g.lock);
        g.pending = {};
        Slot& s = g.point[p.slot];
        const double age = Secs(Clock::now() - s.taken);
        const Put put = s.valid ? Restore(s, "rewind", g.rewinds + 1, ctx, depth) : Put{"the point is gone", {}, {}, 0, 0, 0, 0};
        if (put.refuse.empty()) {
            g.rewinds++;
            // a point newer than the one gone back to belongs to the life that just ended
            while (!g.order.empty() && g.order.back() != p.slot) {
                g.point[g.order.back()].Invalidate();  // and what it holds: objects, files, threads
                g.order.pop_back();
            }
            g.have_rewound = true;
            g.dead = false;
            g.last_rewind = Clock::now();
            g.last_rewind_slot = p.slot;
            g.next_at = g.last_rewind + std::chrono::milliseconds(int64_t(g.interval * 1000));
            g.last_hurt = g.last_rewind;  // the health put back is not a hurt
            g.last_hp = Player() ? Health(Player()) : 0;
            char head[96];
            std::snprintf(head, sizeof head, "rewind: rewind %d OK: point %d, %.1f s old, back at %.2f %.2f %.2f:", g.rewinds, s.number, age, s.pos[0], s.pos[1], s.pos[2]);
            LOGI("%s", head);
            LogRestored("rewind: rewind", g.rewinds, s, put);
            LOGI("rewind: rewind %d: %zu rewind point%s kept", g.rewinds, g.order.size(), g.order.size() == 1 ? "" : "s");
            logx::Flush();
            return;
        }
        refuse = put.refuse;
        g.fallbacks++;
        g.bypass = true;
        LOGW("rewind: fallback %d: %s: the game reloads the level from its start", g.fallbacks, refuse.c_str());
        logx::Flush();
    }
    kernel::CallGuest(ctx, kPlayerDeathReload, {p.arg});  // what the death screen asked for, as it would have been (the lock is not held: it relaunches)
}

// ---- commands and schedules for tests
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
        double secs = 0;
        if (std::sscanf(l.c_str(), "%31s %d", word, &n) < 1 || n < 0) continue;
        const std::string w = word;
        if (w == "strict") {
            g.strict = n != 0;
            LOGI("rewind: strict %d (%s)", n, g.strict ? "a restore that would stop or start a sound stream thread is refused" : "a sound stream thread may be stopped or started again");
        } else if (w == "failstop") {
            kernel::g_rewind_fail_stop = n != 0;
            LOGI("rewind: failstop %d (the next restore that stops a thread fails after it has stopped it)", n);
        } else if (w == "failplan") {
            g.force_refuse = n != 0;
            LOGI("rewind: failplan %d", n);
        } else if (w == "cadence" && std::sscanf(l.c_str(), "%*s %lf", &secs) == 1 && secs > 0) {
            g.interval = secs;
            g.next_at = Clock::now() + std::chrono::milliseconds(int64_t(secs * 1000));
            LOGI("rewind: cadence %.1f s", secs);
        } else if (w == "checkpoint") {
            g.want_checkpoint = true;
        } else if (w == "state") {
            LogState(ctx);
        } else if (w == "kill") {
            Kill();
        } else if (w == "snap") {
            TestSnapshot(size_t(n), ctx, depth);
        } else if (w == "restore") {
            TestRestore(size_t(n), ctx, depth);
        }
    }
    return true;
}

// the rewind points' bookkeeping at each frame: damage seen, and a point when one is due and it is safe
void Checkpoints(X86Context& ctx, uint64_t depth) {
    const auto now = Clock::now();
    if (g.report_gate) {  // a writer that was kept out of the gate wakes only after the hold is let go: its wait is read a frame later
        LOGI("rewind: checkpoint %d: a host writer (the sound mixer among them) waited at the gate for at most %.1f ms", g.report_gate, double(kernel::TakeGateWaitNs()) / 1e6);
        g.report_gate = 0;
    }
    const uint32_t player = Player();
    if (player) {
        const float hp = Health(player);
        if (hp < g.last_hp - 0.001f) g.last_hurt = now;
        g.last_hp = hp;
    }
    if (g.pending.on || (now < g.next_at && !g.want_checkpoint)) return;
    const char* why = nullptr;
    if (!g.want_checkpoint) {
        why = now - g.last_hurt < std::chrono::milliseconds(int64_t(kQuietAfterHurt * 1000)) ? "the player was hurt in the last 3 s" : Unsafe(ctx, player);
    }
    if (why) {
        if (g.unsafe != why) LOGI("rewind: a checkpoint is due (%.1f s since the last, %zu kept) and waits: %s", Secs(now - (g.order.empty() ? g.level_start : g.point[g.order.back()].taken)), g.order.size(), why);
        g.unsafe = why;
        g.safe_frames = 0;
        return;
    }
    // the first frame of the state being safe, not the last of the one before: two frames in a row
    if (!g.want_checkpoint && g.safe_frames++ < 1) return;
    g.safe_frames = 0;
    g.unsafe.clear();
    std::lock_guard<std::mutex> lock(g.lock);
    if (!g.on) return;
    TakePoint(ctx, depth, g.want_checkpoint ? "asked for" : g.order.empty() ? "the first" : "the next");
    g.want_checkpoint = false;
}

// Both buffers, sized from what a snapshot copies and with room to grow, every page touched. Done at the level's first Frame: the
// level is not all in memory at MapSet (a snapshot was 40 MB there and 66 MB at the first Frame), and nothing is on screen before it.
void Prefault() {
    if (!g.prefault) return;
    std::lock_guard<std::mutex> lock(g.lock);
    const auto t0 = Clock::now();
    const uint64_t size = mem::SnapshotSize(), cap = size + size / 4;
    for (Slot& s : g.point) mem::ReserveSnapshot(s.memory, cap);
    g.prefault_bytes = cap;
    LOGI("rewind: prefault: two buffers of %.1f MB (a snapshot is %.1f MB now) made and touched in %.1f ms", cap / 1048576.0, size / 1048576.0, Ms(Clock::now() - t0));
}

bool Init() {
    const char* e = std::getenv("HELLMOUTH_REWIND_TEST");
    g.test = e != nullptr;
    g.on = snapback::g_checkpoints;
    if (const char* s = std::getenv("HELLMOUTH_CHECKPOINT_SECS"))
        if (std::atof(s) > 0) g.interval = std::atof(s);
    if (const char* s = std::getenv("HELLMOUTH_CHECKPOINT_NOPREFAULT")) g.prefault = std::atoi(s) == 0;
    g.verify = std::getenv("HELLMOUTH_REWIND_VERIFY") != nullptr;
    if (g.test && e[0]) {
        std::string v = e;
        size_t at = 0;
        while (at < v.size()) {
            size_t comma = v.find(',', at);
            if (comma == std::string::npos) comma = v.size();
            double a, b;
            if (std::sscanf(v.substr(at, comma - at).c_str(), "%lf:%lf", &a, &b) == 2) g.pairs.push_back({a, b});
            at = comma + 1;
        }
    }
    g.active = g.on || g.test;
    return g.active;
}

}  // namespace

namespace snapback {

void Init() {
    ::Init();
    if (g.active) {
        kernel::EnableRewinding();  // before the game's first thread: a thread begun before it is on is not counted
        kernel::g_relaunch_hook = Relaunching;
    }
    LOGI("rewind: mid-level checkpoints %s%s (every %.0f s, last two kept)", g.on ? "on" : "off", g.test ? ", test triggers on" : "", g.interval);
}

bool Enabled() { return g.active; }
uint32_t Epoch() { return g.epoch; }

void LevelStarted(X86Context&) {
    if (!g.active) return;
    std::lock_guard<std::mutex> lock(g.lock);
    g.level_start = Clock::now();
    g.next_at = g.level_start + std::chrono::milliseconds(int64_t(g.interval * 1000));
    g.last_hurt = g.level_start;
    g.bypass = false;
    g.dead = false;
    g.in_level = true;
    g.level_frames = 0;
    DropPoints("a level begins");
    if (!g.on) return;
    // A restore needs the renderer to drop what it made from guest memory (gpu::Backend::ResetResources) and the native stack depth of
    // each thread to be known (HostStackBase); without them it is not done, so the setting does nothing here
    if (!xdk::DeviceCanReset() || !kernel::CanMeasureStack()) {
        g.on = false;
        LOGW("rewind: mid-level checkpoints are not supported on this renderer/platform (%s): off", kernel::CanMeasureStack() ? "the renderer cannot reset what it made from guest memory" : "the native stack depth is unknown");
        return;
    }
    kernel::EnsureDpcThread();  // its stack and KPCR are guest memory: made before the first snapshot, not after
    LOGI("rewind: checkpoints on for %s: the first is due after %.0f s", extras::CurrentMap().c_str(), g.interval);
}

void PlayerDied() {
    if (!g.active) return;
    g.last_death = Clock::now();
    g.have_died = true;
    g.dead = true;
}

void Relaunching() {
    if (!g.active) return;
    std::lock_guard<std::mutex> lock(g.lock);
    g.on = g.in_level = false;  // the old instance only waits for the next one now
    DropPoints("the game relaunches");
}

bool Frame(X86Context& ctx, uint64_t depth) {
    const auto now = Clock::now();
    if (!g.started) {
        g.started = true;
        g.origin = now;
        g.main = kernel::Current();
        if (g.test) LOGI("rewind: test on, %zu scheduled pairs, commands from the file rewind_cmd", g.pairs.size());
    }
    g.frames++;
    if (g.on && g.in_level && ++g.level_frames == 1) Prefault();
    if (g.pending.on) {
        if (++g.pending.frames > 60) {  // the Frame that restores should be the next one
            LOGW("rewind: the restore did not come within 60 frames");
        }
        Rewind(ctx, depth);
        return true;
    }
    if (g.test) {
        if (g.frames % 15 == 0 && Command(ctx, depth)) return true;  // one thing a frame
        const double t = Secs(now - g.origin);
        for (size_t i = 0; i < g.pairs.size(); i++) {
            Pair& p = g.pairs[i];
            if (!p.snapped && t >= p.snap_s) {
                p.snapped = true;
                TestSnapshot(i, ctx, depth);
                return true;
            }
            if (p.snapped && !p.restored && t >= p.restore_s) {
                p.restored = true;
                TestRestore(i, ctx, depth);
                return true;
            }
        }
    }
    if (g.on && g.in_level) Checkpoints(ctx, depth);
    return false;
}

bool Death(X86Context& ctx, uint32_t arg) { return ::Death(ctx, arg); }

}  // namespace snapback

// TGameManagerImplementation::Frame, from ExecuteGame's loop only: Swap and the other callers come from loading and menus
X86_EXTERN_FUNC(__imp__sub_000183A0);
X86_FUNC(sub_000183A0) {
    if (snapback::Enabled() && kernel::R32(ctx.esp) == kFrameReturn) {
        // the native stack in use here, the same at every call of this: a restore only goes back to the same call chain
        kernel::Thread* self = kernel::Current();
        snapback::Frame(ctx, self && self->host_base ? self->host_base - uintptr_t(__builtin_frame_address(0)) : 0);
    }
    __imp__sub_000183A0(ctx, base);
}

// TGO::PlayerDeathReload(const char*): the death screen's Continue. It queues the current map to be loaded again, which relaunches
// the game into the level. With a rewind point it returns at once instead (cdecl, true), and the next Frame goes back.
X86_EXTERN_FUNC(__imp__sub_000939C0);
X86_FUNC(sub_000939C0) {
    if (snapback::Enabled() && snapback::Death(ctx, kernel::R32(ctx.esp + 4))) {
        ctx.eax = (ctx.eax & 0xFFFFFF00u) | 1;
        ctx.esp += 4;
        return;
    }
    __imp__sub_000939C0(ctx, base);
}

// ICharacterBtvS::Tick(params): the test's kill is made inside the player's own Tick
X86_EXTERN_FUNC(__imp__sub_002061F0);
X86_FUNC(sub_002061F0) {
    if (g_kill_pending.load(std::memory_order_relaxed) && ctx.ecx == Player()) KillNow(ctx);
    __imp__sub_002061F0(ctx, base);
}
