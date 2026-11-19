#include "kernel/rewind.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace kernel {

namespace {

int64_t SteadyNowNs() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

void SetContext(X86Context& to, const X86Context& from) {
    void* host = to.host;  // runtime owned: the thread's own
    uint32_t handle = to.thread_handle;
    to = from;
    to.host = host;
    to.thread_handle = handle;
}

}  // namespace

void CaptureKernel(KernelSnapshot& out, X86Context& main_ctx, uint64_t main_depth) {
    CaptureThreads(out.threads, main_ctx, main_depth);
    CaptureObjects(out);
    CaptureFiles(out, HandleObjects());
    out.clocks = CaptureClocks();
    out.valid = true;
}

RestorePlan PlanRestore(const KernelSnapshot& snap, X86Context& main_ctx, uint64_t main_depth, uint32_t transient) {
    RestorePlan p;
    char buf[200];
    auto refuse = [&](const char* fmt, auto... args) {
        if (!p.refuse.empty()) return;
        std::snprintf(buf, sizeof buf, fmt, args...);
        p.refuse = buf;
    };
    if (!snap.valid) {
        refuse("no snapshot");
        return p;
    }
    const std::vector<ThreadRef> now = Threads();
    Thread* self = Current();
    auto find = [&](uint32_t id) -> ThreadRef {
        for (const ThreadRef& t : now)
            if (t->id == id) return t;
        return nullptr;
    };
    for (size_t i = 0; i < snap.threads.size(); i++) {
        const ThreadSnap& s = snap.threads[i];
        ThreadRef c = find(s.id);
        const bool movable = transient && s.start == transient;  // a stream thread: stopped and started again from its entry
        if (s.main) {
            if (!c || c.get() != self) {
                refuse("thread %X took the snapshot but is not the one restoring", s.id);
            } else if (main_ctx.esp != s.esp || R32(main_ctx.esp) != s.ret || main_depth != s.depth) {
                refuse("the main thread is not at the snapshot's place (esp %08X, was %08X; native depth %llu, was %llu)", main_ctx.esp, s.esp,
                       (unsigned long long)main_depth, (unsigned long long)s.depth);
            } else {
                p.restore.push_back(i);
            }
            continue;
        }
        if (c) {
            const uintptr_t sp = c->host_sp.load();
            const uint64_t depth = c->host_base && sp ? c->host_base - sp : 0;
            const bool same = c->start_routine == s.start && c->k_ret.load() == s.ret && c->k_esp.load() == s.esp && depth == s.depth;
            if (same) {
                p.restore.push_back(i);
            } else if (movable && c->start_routine == transient) {
                p.stop.push_back(c);
                p.restart.push_back(i);
            } else {
                refuse("thread %X (start %08X) is not where it was (site %08X, was %08X; esp %08X, was %08X; depth %llu, was %llu)", s.id, s.start,
                       c->k_ret.load(), s.ret, c->k_esp.load(), s.esp, (unsigned long long)depth, (unsigned long long)s.depth);
            }
        } else if (movable) {
            p.restart.push_back(i);
        } else {
            refuse("thread %X (start %08X) has ended", s.id, s.start);
        }
    }
    for (const ThreadRef& c : now) {
        bool known = false;
        for (const ThreadSnap& s : snap.threads) known = known || s.id == c->id;
        if (known) continue;
        if (transient && c->start_routine == transient) p.stop.push_back(c);
        else refuse("thread %X (start %08X) is new", c->id, c->start_routine);
    }
    for (const ThreadRef& c : p.stop)
        if (c->cs_held.load() > 0 || c->locks_held.load() > 0) refuse("thread %X to stop holds a lock (%d critical sections, %d locks)", c->id, c->cs_held.load(), c->locks_held.load());
    return p;
}

RestoreResult RestoreKernel(KernelSnapshot& snap, const mem::Snapshot& memory, RestorePlan& plan, X86Context& main_ctx) {
    RestoreResult r;
    if (!plan.stop.empty()) {
        if (!StopThreads(plan.stop, 2000)) {
            r.error = "a thread to stop did not leave in 2 s";
            return r;
        }
        r.stopped = int(plan.stop.size());
    }
    const int64_t shift = SteadyNowNs() - snap.clocks.steady_ns;
    RestoreObjects(*snap.objects, shift, [&] { r.bytes = mem::RestoreSnapshot(memory); });
    RestoreFiles(*snap.files);
    r.skew_ns = RestoreClocks(snap.clocks);
    for (size_t i : plan.restore) {
        const ThreadSnap& s = snap.threads[i];
        if (s.main) {
            SetContext(main_ctx, s.ctx);
            if (Thread* self = Current()) {
                self->cs_held = s.cs;
                self->locks_held = s.locks;
            }
        }
        else RestoreThreadState(s);
        r.restored++;
    }
    for (size_t i : plan.restart) {
        RestartThread(snap.threads[i]);
        r.restarted++;
    }
    return r;
}

}  // namespace kernel
