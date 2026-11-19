#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <shared_mutex>

#include "kernel/kernel.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace kernel {

namespace {

thread_local ThreadRef t_current;
std::mutex g_threads_lock;
std::vector<ThreadRef> g_threads;
std::atomic<uint32_t> g_next_id{0x10};

// top of the calling thread's native stack, 0 where there is no way to ask
uintptr_t HostStackBase() {
#if defined(_WIN32)
    return 0;
#elif defined(__APPLE__)
    return uintptr_t(pthread_get_stackaddr_np(pthread_self()));
#else
    pthread_attr_t attr;
    void* addr = nullptr;
    size_t size = 0;
    if (pthread_getattr_np(pthread_self(), &attr)) return 0;
    pthread_attr_getstack(&attr, &addr, &size);
    pthread_attr_destroy(&attr);
    return uintptr_t(addr) + size;
#endif
}

// holding the guest still (QuiesceGuestThreads)
std::shared_timed_mutex g_host_gate;
std::atomic<bool> g_gate_on{false};
std::atomic<bool> g_hold{false};
std::atomic<Thread*> g_hold_owner{nullptr};
std::mutex g_hold_lock;
std::condition_variable g_hold_cv;
thread_local int t_writer_depth = 0;

// guest structures per thread
constexpr uint32_t kKpcrSize = 0x400;
constexpr uint32_t kEthreadSize = 0x200;

void SetupGuestBlocks(Thread& t, uint32_t stack_size, uint32_t tls_size) {
    stack_size = std::max<uint32_t>(stack_size, 0x10000);
    tls_size = (tls_size + 3) & ~3u;
    t.stack_top = mem::AllocStack(stack_size + tls_size + 0x1000);
    t.stack_size = stack_size;
    t.kpcr = mem::AllocSystem(kKpcrSize, 64);
    t.ethread = mem::AllocSystem(kEthreadSize, 64);
    // TLS sits at the top of the stack: StackBase + tls_index * 4 must equal TlsData
    t.tls = t.stack_top - tls_size;
    uint32_t k = t.kpcr;
    W32(k + 0x00, 0xFFFFFFFF);                  // NtTib.ExceptionList
    W32(k + 0x04, t.stack_top);                 // NtTib.StackBase
    W32(k + 0x08, t.stack_top - stack_size);    // NtTib.StackLimit
    W32(k + 0x18, k);                           // NtTib.Self
    W32(k + 0x1C, k);                           // SelfPcr
    W32(k + 0x20, k + 0x28);                    // Prcb
    G<uint8_t>(k + 0x24) = 0;                   // Irql
    W32(k + 0x28, t.ethread);                   // PrcbData.CurrentThread
    uint32_t e = t.ethread;
    G<uint8_t>(e + 0) = 6;                      // dispatcher type ThreadObject
    G<uint8_t>(e + 2) = uint8_t(kEthreadSize / 4);  // Header.Size in dwords
    W32(e + 0x1C, t.stack_top);                 // KTHREAD.StackBase
    W32(e + 0x20, t.stack_top - stack_size);    // KTHREAD.StackLimit
    W32(e + 0x28, t.tls);                       // KTHREAD.TlsData
    W32(e + 0x12C, t.id);                       // ETHREAD.UniqueThread
}

void InitContext(Thread& t, X86Context& ctx) {
    std::memset(&ctx, 0, sizeof(ctx));
    ctx.esp = (t.tls & ~15u) - 16;
    ctx.fs_base = t.kpcr;
    ctx.fcw = 0x27F;  // x87 53 bit precision, exceptions masked
    ctx.mxcsr = 0x1F80;  // SSE exceptions masked, round to nearest
    ctx.thread_handle = 0;
    t.ctx = &ctx;
}

void Register(const ThreadRef& t) {
    std::lock_guard<std::mutex> lock(g_threads_lock);
    g_threads.push_back(t);
}

void Unregister(Thread* t) {
    std::lock_guard<std::mutex> lock(g_threads_lock);
    g_threads.erase(std::remove_if(g_threads.begin(), g_threads.end(), [t](const ThreadRef& r) { return r.get() == t; }),
                    g_threads.end());
}

void MarkExited(Thread& t, uint32_t code) {
    t.exit_code = code;
    std::lock_guard<std::mutex> lock(DispatchLock());
    SetSignal(t, 1);
    SignalChanged();
}

void NameHostThread(const char* name) {
#ifdef _WIN32
    wchar_t w[64];
    size_t i = 0;
    for (; name[i] && i < 63; i++) w[i] = wchar_t(name[i]);
    w[i] = 0;
    using Fn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static Fn set = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")));
    if (set) set(GetCurrentThread(), w);
#elif defined(__APPLE__)
    pthread_setname_np(name);
#else
    pthread_setname_np(pthread_self(), name);
#endif
}

std::atomic<uint64_t> g_affinity{0};

void ApplyAffinity() {
    uint64_t m = g_affinity.load();
    if (!m) return;
#ifdef _WIN32
    SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(m));
#elif defined(__APPLE__)
    // macOS has no way to bind a thread to a core: the scheduler decides
#else
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int i = 0; i < 64; i++)
        if (m >> i & 1) CPU_SET(i, &set);
    pthread_setaffinity_np(pthread_self(), sizeof set, &set);
#endif
}

// recompiled code nests host frames as deep as the guest's calls, give it room
void Spawn(std::function<void()> fn) {
    auto* f = new std::function<void()>(std::move(fn));
#ifdef _WIN32
    auto entry = [](void* p) -> DWORD {
        ApplyAffinity();
        std::unique_ptr<std::function<void()>> g(static_cast<std::function<void()>*>(p));
        (*g)();
        return 0;
    };
    HANDLE h = ::CreateThread(nullptr, 64u << 20, entry, f, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (h) ::CloseHandle(h);
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u << 20);
    pthread_t th;
    pthread_create(&th, &attr, [](void* p) -> void* {
        ApplyAffinity();
        std::unique_ptr<std::function<void()>> g(static_cast<std::function<void()>*>(p));
        (*g)();
        return nullptr;
    }, f);
    pthread_detach(th);
    pthread_attr_destroy(&attr);
#endif
}

}  // namespace

void SetGuestAffinity(uint64_t mask) { g_affinity.store(mask); }

Thread* Current() { return t_current.get(); }
ThreadRef CurrentRef() { return t_current; }
X86Context& CurrentContext() { return *t_current->ctx; }

std::vector<ThreadRef> Threads() {
    std::lock_guard<std::mutex> lock(g_threads_lock);
    return g_threads;
}

struct StartInfo {
    uint32_t system_routine, start_routine, start_context;
};
static std::unordered_map<Thread*, StartInfo> g_start;
static std::mutex g_start_lock;

ThreadRef CreateThread(uint32_t stack_size, uint32_t tls_size, uint32_t system_routine, uint32_t start_routine,
                       uint32_t start_context, bool suspended) {
    auto t = std::make_shared<Thread>();
    t->id = g_next_id.fetch_add(4);
    SetupGuestBlocks(*t, stack_size, tls_size);
    Bind(t, t->ethread);
    if (suspended) t->suspend = 1;
    std::lock_guard<std::mutex> lock(g_start_lock);
    g_start[t.get()] = {system_routine, start_routine, start_context};
    return t;
}

void StartThread(ThreadRef t) {
    Register(t);
    Spawn([t] {
        t_current = t;
        char name[32];
        std::snprintf(name, sizeof(name), "guest %X", t->id);
        NameHostThread(name);
        t->host_base = HostStackBase();
        X86Context ctx;
        InitContext(*t, ctx);
        {
            std::unique_lock<std::mutex> lock(t->apc_lock);
            t->resume_cv.wait(lock, [&] { return t->suspend.load() <= 0; });
        }
        StartInfo si;
        {
            std::lock_guard<std::mutex> lock(g_start_lock);
            si = g_start[t.get()];
            g_start.erase(t.get());
        }
        t->start_routine = si.start_routine;
        t->system_routine = si.system_routine;
        t->kstate = Thread::kRunning;
        SuspendPoint();  // a thread that starts while the guest is held waits for it to be let go
        LOGI("thread %X starts at %08X (%08X, %08X)", t->id, si.system_routine ? si.system_routine : si.start_routine,
             si.start_routine, si.start_context);
        uint32_t code;
        if (si.system_routine)
            code = CallGuest(ctx, si.system_routine, {si.start_routine, si.start_context});
        else
            code = CallGuest(ctx, si.start_routine, {si.start_context});
        LOGI("thread %X returned %08X", t->id, code);
        MarkExited(*t, code);
        Unregister(t.get());
        t_current.reset();
    });
}

void StartHostThread(const char* name, std::function<void(X86Context&)> fn) {
    auto t = std::make_shared<Thread>();
    t->id = g_next_id.fetch_add(4);
    SetupGuestBlocks(*t, 0x20000, 0x100);
    Bind(t, t->ethread);
    std::string n = name;
    Spawn([t, fn, n] {
        t_current = t;
        NameHostThread(n.c_str());
        t->host_base = HostStackBase();
        t->kstate = Thread::kRunning;
        X86Context ctx;
        InitContext(*t, ctx);
        fn(ctx);
        t_current.reset();
    });
}

void ExitThread(uint32_t code) {
    Thread* t = Current();
    if (t) {
        LOGI("thread %X exits with %08X", t->id, code);
        MarkExited(*t, code);
        Unregister(t);
    }
    t_current.reset();
#ifdef _WIN32
    ::ExitThread(code);
#else
    pthread_exit(nullptr);
#endif
}

void Resume(Thread& t) {
    std::lock_guard<std::mutex> lock(t.apc_lock);
    if (t.suspend > 0) t.suspend--;
    t.resume_cv.notify_all();
}

uint32_t Suspend(Thread& t) {
    uint32_t prev = uint32_t(t.suspend++);
    if (&t == Current()) SuspendPoint();
    return prev;
}

void (*g_blocking)(bool done) = nullptr;

void EnterBlocked(uint8_t state, void* frame) {
    Thread* t = Current();
    if (!t) return;
    if (t->ctx) {
        t->k_esp = t->ctx->esp;
        t->k_ret = R32(t->ctx->esp);
    }
    t->host_sp = uintptr_t(frame);
    t->kstate = state;
}

void LeaveBlocked() {
    if (Thread* t = Current()) t->kstate = Thread::kRunning;
}

void SuspendPoint() {
    Thread* t = Current();
    if (!t) return;
    if (g_hold.load(std::memory_order_acquire) && g_hold_owner.load() != t) {
        EnterBlocked(Thread::kParked, __builtin_frame_address(1));  // the kernel call's frame, as where it blocks
        {
            std::unique_lock<std::mutex> lock(g_hold_lock);
            g_hold_cv.wait(lock, [] { return !g_hold.load(); });
        }
        t->kstate = Thread::kRunning;
    }
    if (t->suspend.load() <= 0) return;
    EnterBlocked(Thread::kParked, __builtin_frame_address(1));
    {
        std::unique_lock<std::mutex> lock(t->apc_lock);
        t->resume_cv.wait(lock, [&] { return t->suspend.load() <= 0; });
    }
    t->kstate = Thread::kRunning;
}

void EnableHostWriterGate() { g_gate_on = true; }

HostWriter::HostWriter() {
    if (!g_gate_on.load(std::memory_order_relaxed)) return;
    counted = true;
    if (t_writer_depth++ == 0) {
        g_host_gate.lock_shared();
        owns = true;
    }
}

HostWriter::~HostWriter() {
    if (!counted) return;
    t_writer_depth--;
    if (owns) g_host_gate.unlock_shared();
}

bool StopHostWriters(int timeout_ms) {
    return g_host_gate.try_lock_for(std::chrono::milliseconds(timeout_ms));
}

void StartHostWriters() { g_host_gate.unlock(); }

std::vector<ThreadRef> QuiesceGuestThreads(int timeout_ms) {
    g_hold_owner = Current();
    g_hold.store(true, std::memory_order_release);  // before the states are read: a thread read as blocked stops when it leaves
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::vector<ThreadRef> loose;
    for (;;) {
        loose.clear();
        for (const ThreadRef& t : Threads())
            if (t.get() != Current() && t->kstate.load() == Thread::kRunning) loose.push_back(t);
        if (loose.empty() || std::chrono::steady_clock::now() >= until) break;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return loose;
}

void ResumeGuestThreads() {
    {
        std::lock_guard<std::mutex> lock(g_hold_lock);
        g_hold.store(false);
    }
    g_hold_cv.notify_all();
    g_hold_owner = nullptr;
}

void QueueApc(Thread& t, uint32_t routine, uint32_t a1, uint32_t a2, uint32_t a3) {
    std::lock_guard<std::mutex> lock(t.apc_lock);
    t.apcs.push_back({routine, a1, a2, a3});
    SignalChanged();
}

bool RunApcs(Thread& t) {
    bool ran = false;
    // the routines are guest code: the thread is running them, whatever call it was blocked in
    struct Back {
        Thread& t;
        uint8_t was;
        ~Back() { if (t.kstate.load() == Thread::kRunning) t.kstate = was; }
    } back{t, t.kstate.exchange(Thread::kRunning)};
    for (;;) {
        std::array<uint32_t, 4> a;
        {
            std::lock_guard<std::mutex> lock(t.apc_lock);
            if (t.apcs.empty()) break;
            a = t.apcs.front();
            t.apcs.pop_front();
        }
        CallGuest(*t.ctx, a[0], {a[1], a[2], a[3]});
        ran = true;
    }
    return ran;
}

}  // namespace kernel
