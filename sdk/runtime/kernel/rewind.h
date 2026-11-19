#pragma once
// Rewinding the guest inside its process (a game's mid-level restore): a snapshot of what the kernel holds outside guest
// memory, and the restore of it. Guest memory itself is mem::Snapshot. A restore changes nothing until PlanRestore has said
// it can be done; the caller holds the guest still (StopHostWriters, QuiesceGuestThreads) from the capture to the end.
#include <array>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "kernel/kernel.h"

namespace kernel {

// The guest's clocks: guest nanoseconds (GuestNs), the time stamp counter's, and the steady clock they were read at
struct Clocks {
    int64_t steady_ns = 0, guest_ns = 0, tsc_ns = 0;
};
Clocks CaptureClocks();
// guest time and the counter go on from the snapshot's reading, whatever has passed since; returns the nanoseconds the
// guest clock was set back
int64_t RestoreClocks(const Clocks& c);

struct ThreadSnap {
    ThreadRef t;  // kept alive: a thread that has since left can start again
    uint32_t id = 0, start = 0, system_routine = 0, start_context = 0;
    uint32_t esp = 0, ret = 0;  // where it blocks (the main thread: its context)
    uint64_t depth = 0;         // native stack in use
    int suspend = 0, cs = 0, locks = 0;
    bool main = false;
    X86Context ctx;
    std::deque<std::array<uint32_t, 4>> apcs;
};

struct KernelSnapshot {
    KernelSnapshot();
    ~KernelSnapshot();
    KernelSnapshot(const KernelSnapshot&) = delete;
    KernelSnapshot& operator=(const KernelSnapshot&) = delete;
    struct Objects;  // objects.cpp: handles, pointers, object fields, timers, DPCs
    struct Files;    // files.cpp: positions and directory listings
    std::shared_ptr<Objects> objects;  // shared, not unique: made and ended where the type is complete
    std::shared_ptr<Files> files;
    std::vector<ThreadSnap> threads;
    Clocks clocks;
    bool valid = false;
};

// The calling thread is the main one: its context and the native stack it has used (from the frame of the caller that will
// also restore) are given.
void CaptureKernel(KernelSnapshot& out, X86Context& main_ctx, uint64_t main_depth);

struct RestorePlan {
    std::vector<size_t> restore;  // threads (in KernelSnapshot::threads) whose context is put back
    std::vector<size_t> restart;  // threads that start again from their entry
    std::vector<ThreadRef> stop;  // threads there now that the snapshot did not have, or that start again
    std::string refuse;           // set: nothing can be restored, and why
};
// Compares the threads now with the snapshot's. 'transient' is a start routine whose threads hold nothing of their own (the
// state is in guest memory they poll), so they may be stopped and started again from their entry; any other difference
// refuses. Changes nothing.
RestorePlan PlanRestore(const KernelSnapshot& snap, X86Context& main_ctx, uint64_t main_depth, uint32_t transient);

struct RestoreResult {
    uint64_t bytes = 0;
    int restored = 0, restarted = 0, stopped = 0;
    int64_t skew_ns = 0;
    std::string error;  // set: the restore failed part way (the plan's stops could not finish)
};
// Stops what the plan says, puts memory, objects, files, timers, clocks and thread contexts back, starts threads again.
// The main thread's context is set last, in main_ctx. Needs the memory snapshot taken together with 'snap'.
RestoreResult RestoreKernel(KernelSnapshot& snap, const mem::Snapshot& memory, RestorePlan& plan, X86Context& main_ctx);

// threads.cpp, used by RestoreKernel
void CaptureThreads(std::vector<ThreadSnap>& out, X86Context& main_ctx, uint64_t main_depth);
bool StopThreads(const std::vector<ThreadRef>& threads, int timeout_ms);
void RestoreThreadState(const ThreadSnap& s);
void RestartThread(const ThreadSnap& s);

// objects.cpp
std::vector<ObjectRef> HandleObjects();
void CaptureObjects(KernelSnapshot& out);
// 'locked' runs with the dispatcher and the tables locked, so no waiter sees the memory and the objects out of step
void RestoreObjects(const KernelSnapshot::Objects& in, int64_t steady_shift_ns, const std::function<void()>& locked);
// files.cpp
void CaptureFiles(KernelSnapshot& out, const std::vector<ObjectRef>& handles);
void RestoreFiles(const KernelSnapshot::Files& in);

}  // namespace kernel
