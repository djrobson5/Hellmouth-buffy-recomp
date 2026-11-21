#ifdef XBE_GAME_CODE_DLL
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <cxxabi.h>
#include <filesystem>
#include <string>
#include <typeinfo>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "core/log.h"
#include "cpu/dispatch.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

#ifdef _WIN32
#include <intrin.h>  // __debugbreak
#endif

namespace {

std::vector<X86Func*> g_table;  // one slot per code byte
std::vector<bool> g_continuation;
uint32_t g_lo = 0, g_hi = 0;
#ifdef XBE_GAME_CODE_DLL
// the recompiled code's tables, exported by the game code DLL
const X86FuncMapping* g_mappings = nullptr;
const uint32_t* g_continuations = nullptr;
uint32_t g_entry = 0;
#else
// linked into this program
const X86FuncMapping* g_mappings = X86FuncMappings;
const uint32_t* g_continuations = X86Continuations;
uint32_t g_entry = X86EntryPoint;
#endif
#ifdef XBE_GAME_CODE_DLL
// The resume that x86_icall last threw on this thread, for x86_try_resume: the game code DLL's catch cannot name the type
thread_local X86Resume t_resume{};
#endif

#if defined(XBE_GAME_CODE_DLL) && defined(__APPLE__)
// the X86_EXTERN_FUNC slots of this executable (cpu/x86_ops.h), which register at start up; dispatch::Load fills them
std::vector<std::pair<const char*, X86Func**>>& Externs() {
    static std::vector<std::pair<const char*, X86Func**>> externs;
    return externs;
}
void* g_dylib = nullptr;
std::string g_dylib_stamp;
#endif

struct Export {
    const char* name = nullptr;
    kernel::KernelFn fn = nullptr;
    uint32_t data = 0;
};
Export g_exports[400];

}  // namespace

// synthetic guest addresses for kernel functions reached through a pointer
constexpr uint32_t kKernelThunkBase = 0xFE000000;

namespace dispatch {

#if defined(XBE_GAME_CODE_DLL) && defined(_WIN32)
// the game code DLL: its tables come by name, as the delay loaded imports carry only functions. The DLL's imports name the
// executable (Buffy.exe), so Windows can only bind them when the running executable has that name: LoadLibraryW then fails with
// "the specified module could not be found", which tells the player nothing, so the name is checked first.
LoadResult Load(const std::filesystem::path& dll, const std::wstring& exe_name, const char* stamp) {
    wchar_t self[32768];
    DWORD n = GetModuleFileNameW(nullptr, self, DWORD(sizeof(self) / sizeof(self[0])));
    std::wstring running = std::filesystem::path(std::wstring(self, n)).filename().wstring();
    if (_wcsicmp(running.c_str(), exe_name.c_str()) != 0) return {LoadStatus::WrongExeName, 0, ""};
    HMODULE m = LoadLibraryW(dll.c_str());
    if (!m) {
        DWORD error = GetLastError();
        // the loader binds the DLL's imports by name before anything of the DLL runs: one this executable lacks is a DLL for
        // another version of it
        return {error == ERROR_PROC_NOT_FOUND ? LoadStatus::Stale : LoadStatus::NotLoaded, error, ""};
    }
    g_mappings = reinterpret_cast<const X86FuncMapping*>(GetProcAddress(m, "X86FuncMappings"));
    g_continuations = reinterpret_cast<const uint32_t*>(GetProcAddress(m, "X86Continuations"));
    auto* lo = reinterpret_cast<const uint32_t*>(GetProcAddress(m, "X86CodeBase"));
    auto* hi = reinterpret_cast<const uint32_t*>(GetProcAddress(m, "X86CodeEnd"));
    auto* entry = reinterpret_cast<const uint32_t*>(GetProcAddress(m, "X86EntryPoint"));
    if (!g_mappings || !g_continuations || !lo || !hi || !entry) return {LoadStatus::NoTables, 0, ""};
    // the DLL binds to this executable by names, so one built for an older executable (or another copy of the game) loads
    // happily and then calls the wrong code: it says what it was built for, and it must say what this executable is
    const char* dll_stamp = reinterpret_cast<const char*>(GetProcAddress(m, "X86GameCodeStamp"));
    if (!dll_stamp || std::strcmp(dll_stamp, stamp) != 0) return {LoadStatus::Stale, 0, dll_stamp ? dll_stamp : ""};
    g_lo = *lo;
    g_hi = *hi;
    g_entry = *entry;
    return {LoadStatus::Ok, 0, ""};
}
#endif

#if defined(XBE_GAME_CODE_DLL) && defined(__APPLE__)
LoadResult Load(const std::filesystem::path& dylib, const char* stamp) {
    void* h = dlopen(dylib.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* why = dlerror();
        g_dylib_stamp = why ? why : "";
        // the loader binds the dylib's undefined names to this executable before anything of the dylib runs: one the executable does
        // not export is a dylib for another version of it ("symbol not found in flat namespace '_sub_00012345'")
        const bool unbound = g_dylib_stamp.find("symbol not found") != std::string::npos;
        return {unbound ? LoadStatus::Stale : LoadStatus::NotLoaded, 0, "", g_dylib_stamp};
    }
    g_mappings = reinterpret_cast<const X86FuncMapping*>(dlsym(h, "X86FuncMappings"));
    g_continuations = reinterpret_cast<const uint32_t*>(dlsym(h, "X86Continuations"));
    auto* lo = reinterpret_cast<const uint32_t*>(dlsym(h, "X86CodeBase"));
    auto* hi = reinterpret_cast<const uint32_t*>(dlsym(h, "X86CodeEnd"));
    auto* entry = reinterpret_cast<const uint32_t*>(dlsym(h, "X86EntryPoint"));
    if (!g_mappings || !g_continuations || !lo || !hi || !entry) return {LoadStatus::NoTables, 0, "", "the tables are missing"};
    // as on Windows: a dylib made for another build of this executable binds all the same, and then calls the wrong code
    const char* dylib_stamp = reinterpret_cast<const char*>(dlsym(h, "X86GameCodeStamp"));
    if (!dylib_stamp || std::strcmp(dylib_stamp, stamp) != 0) {
        g_dylib_stamp = dylib_stamp ? dylib_stamp : "";
        return {LoadStatus::Stale, 0, g_dylib_stamp.c_str()};
    }
    // what the executable calls in the dylib (its overrides call the recompiled body they replace): each by name
    for (auto& [name, slot] : Externs()) {
        *slot = reinterpret_cast<X86Func*>(dlsym(h, name));
        if (!*slot) return {LoadStatus::NoTables, 0, "", std::string("the dylib has no ") + name};
    }
    g_dylib = h;
    g_lo = *lo;
    g_hi = *hi;
    g_entry = *entry;
    return {LoadStatus::Ok, 0, ""};
}

Tables Describe() {
    Tables t;
    for (const X86FuncMapping* m = g_mappings; m && m->host; m++) t.functions++;
    for (const uint32_t* c = g_continuations; c && *c; c++) t.continuations++;
    t.externs = Externs().size();
    for (auto& [name, slot] : Externs()) t.externs_filled += *slot != nullptr;
    t.code_base = g_lo;
    t.code_end = g_hi;
    t.entry = g_entry;
    return t;
}
#endif

uint32_t EntryPoint() { return g_entry; }
bool InCode(uint32_t va) { return va >= g_lo && va < g_hi; }

void Init() {
#ifndef XBE_GAME_CODE_DLL
    g_lo = X86CodeBase;
    g_hi = X86CodeEnd;
#endif
    g_table.assign(g_hi - g_lo, nullptr);
    size_t n = 0;
    for (const X86FuncMapping* m = g_mappings; m->host; m++, n++)
        if (m->guest >= g_lo && m->guest < g_hi) g_table[m->guest - g_lo] = m->host;
    g_continuation.assign(g_hi - g_lo, false);
    for (const uint32_t* c = g_continuations; *c; c++)
        if (*c >= g_lo && *c < g_hi) g_continuation[*c - g_lo] = true;
    LOGI("dispatch: %zu functions over %08X..%08X", n, g_lo, g_hi);
}

X86Func* Find(uint32_t guest) {
    if (guest >= g_lo && guest < g_hi) return g_table[guest - g_lo];
    return nullptr;
}

// host code the guest may call through a pointer gets an address in this range
constexpr uint32_t kHostThunkBase = 0xFD000000;
static std::vector<X86Func*> g_host_thunks;
static std::mutex g_host_lock;

uint32_t HostThunk(X86Func* fn) {
    std::lock_guard<std::mutex> lock(g_host_lock);
    for (size_t i = 0; i < g_host_thunks.size(); i++)
        if (g_host_thunks[i] == fn) return kHostThunkBase + uint32_t(i) * 4;
    g_host_thunks.push_back(fn);
    return kHostThunkBase + uint32_t(g_host_thunks.size() - 1) * 4;
}

uint32_t KernelThunkAddress(uint32_t ordinal) { return kKernelThunkBase + ordinal * 4; }

}  // namespace dispatch

#if defined(XBE_GAME_CODE_DLL) && defined(__APPLE__)
X86Func* x86_extern_register(const char* name, X86Func** slot) {
    Externs().push_back({name, slot});
    return nullptr;
}
#endif

void x86_icall(X86Context& ctx, uint8_t* base, uint32_t target) {
    ctx.last_icall = target;
    if (target >= g_lo && target < g_hi && g_continuation[target - g_lo]) {
#ifdef XBE_GAME_CODE_DLL
        t_resume = {target, ctx.esp};
#endif
        throw X86Resume{target, ctx.esp};
    }
    if (X86Func* f = dispatch::Find(target)) {
        f(ctx, base);
        return;
    }
    if (target >= kKernelThunkBase && target < kKernelThunkBase + 400 * 4) {
        x86_kernel(ctx, base, (target - kKernelThunkBase) / 4);
        return;
    }
    if (target >= dispatch::kHostThunkBase) {
        size_t i = (target - dispatch::kHostThunkBase) / 4;
        X86Func* f = nullptr;
        {
            std::lock_guard<std::mutex> lock(dispatch::g_host_lock);
            if (i < dispatch::g_host_thunks.size()) f = dispatch::g_host_thunks[i];
        }
        if (f) {
            f(ctx, base);
            return;
        }
    }
    uint32_t ret = kernel::R32(ctx.esp);
    LOGE("indirect call to %08X has no recompiled function (return address %08X, esp %08X)", target, ret, ctx.esp);
    LOGE("add 0x%X to [functions] in the image's config/*.toml and rebuild", target);
    x86_trap(ctx, ret, "indirect call to unknown code");
}

void x86_resume(X86Context& ctx, uint8_t* base, const X86Resume& r) {
    ctx.esp = r.esp;
    dispatch::Find(r.target)(ctx, base);
}

#ifdef XBE_GAME_CODE_DLL
// For the game code DLL's catch of everything: when the exception is a resume (not another one passing through) that this
// frame may take, make it. The copy is kept as the continuation may throw another resume.
bool x86_try_resume(X86Context& ctx, uint8_t* base, uint32_t frame) {
    const std::type_info* type = abi::__cxa_current_exception_type();
    if (!type || *type != typeid(X86Resume) || t_resume.esp >= frame) return false;
    X86Resume r = t_resume;
    x86_resume(ctx, base, r);
    return true;
}
#endif

void x86_kernel(X86Context& ctx, uint8_t* base, uint32_t ordinal) {
    if (ordinal < 400 && g_exports[ordinal].fn) {
        g_exports[ordinal].fn(ctx, base);
        return;
    }
    LOGE("kernel export %u (%s) is not implemented, return address %08X", ordinal, kernel::ExportName(ordinal),
         kernel::R32(ctx.esp));
    x86_trap(ctx, kernel::R32(ctx.esp), "unimplemented kernel export");
}

namespace kernel {

void RegisterExport(uint32_t ordinal, const char* name, KernelFn fn) {
    g_exports[ordinal].name = name;
    g_exports[ordinal].fn = fn;
}

static uint32_t g_kdata_next = mem::kKernelDataBase + 0x1000;

uint32_t RegisterData(uint32_t ordinal, const char* name, uint32_t size) {
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    if (g_exports[ordinal].data) return g_exports[ordinal].data;
    uint32_t a = (g_kdata_next + 15) & ~15u;
    g_kdata_next = a + ((size + 15) & ~15u);
    g_exports[ordinal].name = name;
    g_exports[ordinal].data = a;
    return a;
}

uint32_t DataAddress(uint32_t ordinal) { return ordinal < 400 ? g_exports[ordinal].data : 0; }
const char* ExportName(uint32_t ordinal) { return ordinal < 400 && g_exports[ordinal].name ? g_exports[ordinal].name : OrdinalName(ordinal); }
bool HasExport(uint32_t ordinal) { return ordinal < 400 && (g_exports[ordinal].fn || g_exports[ordinal].data); }

uint32_t CallGuest(X86Context& ctx, uint32_t fn, const uint32_t* args, size_t count) {
    // the D3D and DirectSound hooks call this several times a draw: no heap, only the registers put back are kept
    struct {
        uint32_t eax, ecx, edx, ebx, esi, edi, ebp;
    } saved{ctx.eax, ctx.ecx, ctx.edx, ctx.ebx, ctx.esi, ctx.edi, ctx.ebp};
    uint32_t esp = ctx.esp;
    for (const uint32_t* p = args + count; p != args;) {
        ctx.esp -= 4;
        W32(ctx.esp, *--p);
    }
    ctx.esp -= 4;
    W32(ctx.esp, 0xFFFFFFF0u);  // never used as a target, calls return through the host stack
    x86_icall(ctx, g_base, fn);
    uint32_t result = ctx.eax;
    ctx.eax = saved.eax;
    ctx.ecx = saved.ecx;
    ctx.edx = saved.edx;
    ctx.ebx = saved.ebx;
    ctx.esi = saved.esi;
    ctx.edi = saved.edi;
    ctx.ebp = saved.ebp;
    ctx.esp = esp;
    return result;
}

}  // namespace kernel

namespace kernel {
// return addresses found on the guest stack, the cheapest way to place a guest event
void LogGuestStack(X86Context& ctx, int words) {
    LOGE("  eax %08X ecx %08X edx %08X ebx %08X esp %08X ebp %08X esi %08X edi %08X", ctx.eax, ctx.ecx, ctx.edx, ctx.ebx,
         ctx.esp, ctx.ebp, ctx.esi, ctx.edi);
    for (int i = 0; i < words; i++) {
        uint32_t a = ctx.esp + uint32_t(i) * 4;
        if (!mem::IsCommitted(a)) break;
        uint32_t v = R32(a);
        if (dispatch::InCode(v)) LOGE("  [esp+%03X] %08X", i * 4, v);
    }
}
}  // namespace kernel

void x86_trap(X86Context& ctx, uint32_t va, const char* what) {
    LOGE("guest trap at %08X: %s", va, what);
    kernel::LogGuestStack(ctx, 64);
    logx::Flush();
#ifdef _WIN32
    __debugbreak();
#endif
    std::abort();
}

uint32_t x86_trace(X86Context& ctx, uint32_t va, uint32_t ret) {
    using kernel::R32;
    if (!ret) {
        ret = R32(ctx.esp);
        LOGI("trace %08X from %08X (%08X, %08X, %08X, %08X) ecx %08X", va, ret, R32(ctx.esp + 4), R32(ctx.esp + 8),
             R32(ctx.esp + 12), R32(ctx.esp + 16), ctx.ecx);
    } else {
        LOGI("trace %08X returned %08X to %08X", va, ctx.eax, ret);
    }
    return ret;
}

void x86_priv(X86Context& ctx, uint32_t va, const char* what) {
    static std::mutex m;
    static std::unordered_set<uint32_t> seen;
    std::lock_guard<std::mutex> lock(m);
    if (seen.insert(va).second) LOGW("privileged instruction %s at %08X ignored", what, va);
    (void)ctx;
}

// the guest measured time against a 733.33 MHz time stamp counter. The skew is the rewind's: after a restore the counter goes on
// from the snapshot's reading
static std::atomic<int64_t> g_tsc_skew_ns{0};
static int64_t RawTscNs() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return duration_cast<nanoseconds>(steady_clock::now() - start).count();
}
int64_t x86_tsc_ns() { return RawTscNs() - g_tsc_skew_ns.load(); }
void x86_set_tsc_ns(int64_t ns) { g_tsc_skew_ns = RawTscNs() - ns; }
uint64_t x86_rdtsc() {
    int64_t ns = x86_tsc_ns();
    uint64_t sec = uint64_t(ns) / 1000000000ull, rem = uint64_t(ns) % 1000000000ull;
    return sec * 733333333ull + rem * 733333333ull / 1000000000ull;
}

void x86_cpuid(X86Context& ctx) {
    switch (ctx.eax) {
        case 0:
            ctx.eax = 2;
            ctx.ebx = 0x756E6547;  // GenuineIntel
            ctx.edx = 0x49656E69;
            ctx.ecx = 0x6C65746E;
            break;
        case 1:
            ctx.eax = 0x0000068A;  // family 6 model 8, Pentium III Coppermine
            ctx.ebx = 0;
            ctx.ecx = 0;
            ctx.edx = 0x0383F9FF;  // fpu tsc cx8 cmov mmx fxsr sse
            break;
        default:
            ctx.eax = ctx.ebx = ctx.ecx = ctx.edx = 0;
            break;
    }
}
