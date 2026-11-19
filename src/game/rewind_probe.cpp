// A measurement for rewinding the game inside its process, which never restores anything. With HELLMOUTH_REWIND_PROBE=1,
// every HELLMOUTH_REWIND_PROBE_SECS seconds (30) at the top of TGameManagerImplementation::Frame, called from ExecuteGame's
// loop, it holds the guest still, copies every committed range of guest memory and the allocator tables, and logs
//   - the bytes and milliseconds, and how long the frame stood still;
//   - one line per guest thread: id, start routine, where it waits, its guest esp, the word on top of it, how deep its
//     native stack is, and whether it holds a guest critical section or one of the engine's locks;
//   - a summary of how many later snapshots have the same threads in the same places as each earlier one, which is what
//     a restore would need: a thread's call chain is on its native stack, which memory alone doesn't bring back.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kFrameReturn = 0x6423B;  // ExecuteGame's `call [eax+0x14]` at 0x64238 returns here
constexpr uint32_t kTransientStart = 0x670C0;  // the sound streams' MultiThread_Callback, one thread per stream

double Ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

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

struct Sig {
    uint32_t id = 0, start = 0, sys = 0, ret = 0, esp = 0;
    uint64_t depth = 0;  // native stack bytes in use
    uint8_t state = 0;
    int cs = 0, locks = 0;
    bool main = false;
    bool Transient() const { return start == kTransientStart; }
    bool Same(const Sig& o) const { return start == o.start && ret == o.ret && esp == o.esp && depth == o.depth; }
};
using ThreadSet = std::vector<Sig>;

struct Taken {
    int seq = 0;
    double at_s = 0;
    bool complete = true;
    ThreadSet threads;
};

struct Probe {
    bool enabled = false, started = false;
    int secs = 30;
    Clock::time_point origin, last, next_try;
    uint64_t frames = 0, frames_since = 0;
    double dt_sum_ms = 0;
    Clock::time_point last_frame;
    std::unique_ptr<mem::Snapshot> slot[2];
    std::vector<Taken> taken;
    int failed = 0, torn = 0;
    // sums for the summary
    double stall_sum = 0, stall_max = 0, copy_sum = 0, copy_max = 0;
    uint64_t bytes_max = 0;
};
Probe g;

const char* Cmp(const ThreadSet& a, const ThreadSet& b, bool by_id, bool steady_only, bool sigs, std::map<std::string, int>* why) {
    auto keep = [&](const Sig& s) { return !steady_only || !s.Transient(); };
    bool ok = true;
    auto note = [&](const std::string& r) {
        ok = false;
        if (why) (*why)[r]++;
    };
    char buf[96];
    if (by_id) {
        for (const Sig& x : a) {
            if (!keep(x)) continue;
            const Sig* y = nullptr;
            for (const Sig& c : b)
                if (c.id == x.id) y = &c;
            if (!y) {
                std::snprintf(buf, sizeof buf, "gone %08X", x.start);
                note(buf);
            } else if (sigs && !x.Same(*y)) {
                std::snprintf(buf, sizeof buf, "moved %08X (%s%s%s%s)", x.start, x.ret != y->ret ? "site " : "", x.esp != y->esp ? "esp " : "",
                              x.depth != y->depth ? "depth " : "", x.start != y->start ? "start" : "");
                note(buf);
            }
        }
        for (const Sig& y : b) {
            if (!keep(y)) continue;
            bool found = false;
            for (const Sig& x : a) found = found || x.id == y.id;
            if (!found) {
                std::snprintf(buf, sizeof buf, "new %08X", y.start);
                note(buf);
            }
        }
    } else {  // threads by what they are and where, not which one
        auto key = [&](const Sig& s) { return std::to_string(s.start) + "/" + std::to_string(s.ret) + "/" + std::to_string(s.esp) + "/" + std::to_string(s.depth); };
        std::multiset<std::string> ka, kb;
        for (const Sig& x : a) if (keep(x)) ka.insert(key(x));
        for (const Sig& y : b) if (keep(y)) kb.insert(key(y));
        if (ka != kb) note("multiset differs");
    }
    return ok ? "y" : "n";
}

void Summary() {
    const size_t n = g.taken.size();
    int pairs = 0, a_ok = 0, b_ok = 0, b_set_ok = 0, c_ok = 0, d_ok = 0;
    int by_lag_pairs[3] = {0, 0, 0}, by_lag_ok[3] = {0, 0, 0};
    std::map<std::string, int> why_b, why_a;
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++) {
            pairs++;
            const ThreadSet &A = g.taken[i].threads, &B = g.taken[j].threads;
            bool a = *Cmp(A, B, true, false, true, &why_a) == 'y';
            bool bset = *Cmp(A, B, true, true, false, nullptr) == 'y';
            bool b = *Cmp(A, B, true, true, true, &why_b) == 'y';
            bool c = *Cmp(A, B, false, false, true, nullptr) == 'y';
            bool d = *Cmp(A, B, false, true, true, nullptr) == 'y';
            a_ok += a, b_set_ok += bset, b_ok += b, c_ok += c, d_ok += d;
            size_t lag = std::min<size_t>(j - i, 3) - 1;
            by_lag_pairs[lag]++;
            by_lag_ok[lag] += b;
        }
    LOGI("rewind: summary after %zu snapshots (%d failed, %d with a thread that did not stop): %d pairs", n, g.failed, g.torn, pairs);
    if (pairs) {
        LOGI("rewind: summary match by id and place, all threads: %d/%d (%.0f%%)", a_ok, pairs, 100.0 * a_ok / pairs);
        LOGI("rewind: summary match by id, steady threads only, place ignored: %d/%d (%.0f%%)", b_set_ok, pairs, 100.0 * b_set_ok / pairs);
        LOGI("rewind: summary match by id and place, steady threads only: %d/%d (%.0f%%)", b_ok, pairs, 100.0 * b_ok / pairs);
        LOGI("rewind: summary match by start and place (any id), all threads: %d/%d (%.0f%%); steady only: %d/%d (%.0f%%)", c_ok, pairs,
             100.0 * c_ok / pairs, d_ok, pairs, 100.0 * d_ok / pairs);
        for (int l = 0; l < 3; l++)
            if (by_lag_pairs[l])
                LOGI("rewind: summary steady match by id and place, snapshots %s%d apart: %d/%d", l == 2 ? ">=" : "", l + 1, by_lag_ok[l], by_lag_pairs[l]);
        for (auto& [r, c] : why_b) LOGI("rewind: summary steady mismatch '%s' in %d pairs", r.c_str(), c);
        for (auto& [r, c] : why_a) LOGI("rewind: summary all-thread mismatch '%s' in %d pairs", r.c_str(), c);
    }
    if (n) {
        LOGI("rewind: summary stall ms mean %.1f max %.1f, copy ms mean %.1f max %.1f, bytes max %.1f MB", g.stall_sum / n, g.stall_max, g.copy_sum / n,
             g.copy_max, g.bytes_max / 1048576.0);
    }
    logx::Flush();
}

// Pages of 4 KB that differ between this snapshot and the one before it, over the addresses both hold.
void Dirty(const mem::Snapshot& cur, const mem::Snapshot& prev, uint64_t& same, uint64_t& diff, uint64_t& fresh) {
    same = diff = fresh = 0;
    size_t j = 0;
    for (const mem::SnapshotRange& r : cur.ranges) {
        for (uint32_t va = r.va; va < r.va + r.size; va += 0x1000) {
            while (j < prev.ranges.size() && uint64_t(prev.ranges[j].va) + prev.ranges[j].size <= va) j++;
            const uint32_t len = std::min<uint32_t>(0x1000, r.va + r.size - va);
            if (j < prev.ranges.size() && prev.ranges[j].va <= va && uint64_t(va) + len <= uint64_t(prev.ranges[j].va) + prev.ranges[j].size) {
                const mem::SnapshotRange& p = prev.ranges[j];
                const bool eq = !std::memcmp(cur.data.get() + r.offset + (va - r.va), prev.data.get() + p.offset + (va - p.va), len);
                (eq ? same : diff)++;
            } else {
                fresh++;
            }
        }
    }
}

void Take(X86Context& ctx) {
    const auto t0 = Clock::now();
    const double frame_ms = g.frames_since ? g.dt_sum_ms / double(g.frames_since) : 0;
    const int seq = int(g.taken.size()) + 1;
    if (!kernel::StopHostWriters(500)) {
        g.failed++;
        g.next_try = t0 + std::chrono::seconds(1);
        LOGW("rewind: snapshot %d skipped: a thread of the host that writes guest memory did not finish in 500 ms", seq);
        logx::Flush();
        return;
    }
    const auto t_gate = Clock::now();
    std::vector<kernel::ThreadRef> loose = kernel::QuiesceGuestThreads(500);
    const auto t_park = Clock::now();

    Taken tk;
    tk.seq = seq;
    tk.at_s = std::chrono::duration<double>(t0 - g.origin).count();
    tk.complete = loose.empty();
    kernel::Thread* self = kernel::Current();
    for (const kernel::ThreadRef& t : kernel::Threads()) {
        Sig s;
        s.id = t->id;
        s.start = t->start_routine;
        s.sys = t->system_routine;
        s.state = t->kstate.load();
        s.cs = t->cs_held.load();
        s.locks = t->locks_held.load();
        if (t.get() == self) {
            s.main = true;
            s.state = kernel::Thread::kRunning;
            s.esp = ctx.esp;
            s.ret = kernel::R32(ctx.esp);
            s.depth = t->host_base ? t->host_base - uintptr_t(__builtin_frame_address(0)) : 0;
        } else {
            s.esp = t->k_esp.load();
            s.ret = t->k_ret.load();
            const uintptr_t sp = t->host_sp.load();
            s.depth = t->host_base && sp ? t->host_base - sp : 0;
        }
        tk.threads.push_back(s);
    }
    std::sort(tk.threads.begin(), tk.threads.end(), [](const Sig& a, const Sig& b) { return a.id < b.id; });
    const auto t_sig = Clock::now();

    mem::Snapshot& cur = *g.slot[seq % 2];
    mem::CaptureSnapshot(cur);
    const auto t_copy = Clock::now();

    kernel::ResumeGuestThreads();
    kernel::StartHostWriters();
    const auto t_end = Clock::now();

    const double stall = Ms(t_end - t0), gate = Ms(t_gate - t0), park = Ms(t_park - t_gate), sig = Ms(t_sig - t_park), copy = Ms(t_copy - t_sig);
    if (!tk.complete) g.torn++;
    g.stall_sum += stall, g.stall_max = std::max(g.stall_max, stall);
    g.copy_sum += copy, g.copy_max = std::max(g.copy_max, copy);
    g.bytes_max = std::max<uint64_t>(g.bytes_max, cur.data_bytes);
    g.last = t_end;
    g.frames_since = 0;
    g.dt_sum_ms = 0;
    g.last_frame = t_end;

    constexpr double MB = 1048576.0;
    LOGI("rewind: snapshot %d at %.1f s: %.1f MB in %zu ranges (user %.1f, system %.1f, stacks %.1f, physical %.1f, host physical %.1f) + tables %.2f MB%s",
         seq, tk.at_s, cur.data_bytes / MB, cur.ranges.size(), cur.BytesOf(mem::RangeKind::User) / MB, cur.BytesOf(mem::RangeKind::System) / MB,
         cur.BytesOf(mem::RangeKind::Stack) / MB, cur.BytesOf(mem::RangeKind::Physical) / MB, cur.BytesOf(mem::RangeKind::HostPhysical) / MB,
         cur.table_bytes / MB, cur.reused ? ", buffer reused" : ", new buffer");
    LOGI("rewind: snapshot %d stall %.1f ms (host writers %.1f, guest threads %.1f, thread read %.1f, copy %.1f, release %.1f); frames run %.2f ms; "
         "%zu threads (%zu contexts %.1f KB)%s",
         seq, stall, gate, park, sig, copy, Ms(t_end - t_copy), frame_ms, tk.threads.size(), tk.threads.size(),
         tk.threads.size() * sizeof(X86Context) / 1024.0, tk.complete ? "" : ", NOT ALL STOPPED");
    for (const kernel::ThreadRef& t : loose) LOGW("rewind: snapshot %d: thread %X (start %08X) kept running", seq, t->id, t->start_routine);
    for (const Sig& s : tk.threads) {
        LOGI("rewind: snapshot %d thread %X start %08X%s %s site %08X esp %08X native depth %llu cs %d locks %d%s", seq, s.id, s.start,
             s.Transient() ? " (stream)" : "", StateName(s.state), s.ret, s.esp, (unsigned long long)s.depth, s.cs, s.locks, s.main ? " main" : "");
    }
    if (seq == 1) {
        mem::Stats q = mem::Query();
        LOGI("rewind: mem::Query: %u of %u pages; committed %u (%.1f MB) in user space, contiguous %u (%.1f MB), system %u (%.1f MB), available %u",
             q.total_pages - q.available_pages, q.total_pages, q.virtual_committed, q.virtual_committed * 4096 / MB, q.contiguous_used,
             q.contiguous_used * 4096 / MB, q.system_used, q.system_used * 4096 / MB, q.available_pages);
    }
    if (seq >= 2) {
        const mem::Snapshot& prev = *g.slot[(seq - 1) % 2];
        const auto d0 = Clock::now();
        uint64_t same, diff, fresh;
        Dirty(cur, prev, same, diff, fresh);
        LOGI("rewind: snapshot %d against %d: %llu pages same, %llu changed (%.1f MB), %llu not there before (%.1f MB); compared in %.1f ms", seq, seq - 1,
             (unsigned long long)same, (unsigned long long)diff, diff * 4096 / MB, (unsigned long long)fresh, fresh * 4096 / MB, Ms(Clock::now() - d0));
    }
    g.taken.push_back(std::move(tk));
    Summary();
}

}  // namespace

// TGameManagerImplementation::Frame, from ExecuteGame's loop only: Swap and the other callers come from loading and menus
X86_EXTERN_FUNC(__imp__sub_000183A0);
X86_FUNC(sub_000183A0) {
    static const bool on = [] {
        const char* e = std::getenv("HELLMOUTH_REWIND_PROBE");
        if (!e || e[0] != '1') return false;
        g.enabled = true;
        if (const char* s = std::getenv("HELLMOUTH_REWIND_PROBE_SECS")) g.secs = std::max(1, std::atoi(s));
        kernel::EnableHostWriterGate();
        g.slot[0] = std::make_unique<mem::Snapshot>();
        g.slot[1] = std::make_unique<mem::Snapshot>();
        LOGI("rewind: probe on, a snapshot every %d s", g.secs);
        return true;
    }();
    if (on && kernel::R32(ctx.esp) == kFrameReturn) {
        const auto now = Clock::now();
        if (!g.started) {
            g.started = true;
            g.origin = g.last = g.last_frame = g.next_try = now;
        }
        g.frames++;
        g.frames_since++;
        g.dt_sum_ms += Ms(now - g.last_frame);
        g.last_frame = now;
        if (now - g.last >= std::chrono::seconds(g.secs) && now >= g.next_try) Take(ctx);
    }
    __imp__sub_000183A0(ctx, base);
}
