// Hooks for Default.xbe. A strong definition replaces the recompiled function at that address.
#include <atomic>
#include <chrono>
#include <thread>

#include "app/app.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "xdk/hle.h"

// CLocker, the engine's lock: a count at +8 that Lock and Unlock change with a plain read, add and write. That holds on
// the Xbox's one processor, but on several cores two threads lose each other's change and the count sticks (-1 was seen
// on the DiskManager lock during the first level load), after which every Lock waits until ShellAbort. Both are
// replaced with atomic versions, which also give the ordering that the plain writes inside the lock need on ARM. The
// count still means what it did: 1 held, 0 free.
static std::atomic_ref<uint32_t> LockerCount(X86Context& ctx, uint8_t* base) {
    return std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(base + ctx.ecx + 8));
}

// CLocker::Lock (thiscall)
X86_FUNC(sub_002AA870) {
    std::atomic_ref<uint32_t> count = LockerCount(ctx, base);
    const auto start = std::chrono::steady_clock::now();
    bool warned = false;
    for (uint32_t tries = 0;; tries++) {
        uint32_t expected = 0;
        if (count.compare_exchange_weak(expected, 1, std::memory_order_acquire, std::memory_order_relaxed)) break;
        if (tries < 2000) {
            std::this_thread::yield();
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!warned && std::chrono::steady_clock::now() - start > std::chrono::seconds(10)) {
            warned = true;
            LOGW("buffy: lock %s still held after 10 s (count %d)", kernel::ReadCString(kernel::R32(ctx.ecx + 4)).c_str(),
                 int32_t(count.load()));
            logx::Flush();
        }
    }
    ctx.esp += 4;
}

// CLocker::Unlock (thiscall)
X86_FUNC(sub_002AA950) {
    uint32_t expected = 1;
    if (!LockerCount(ctx, base).compare_exchange_strong(expected, 0, std::memory_order_release, std::memory_order_relaxed))
        LOGW("buffy: unlock of %s with count %d", kernel::ReadCString(kernel::R32(ctx.ecx + 4)).c_str(), int32_t(expected));
    ctx.esp += 4;
}

// ShellAbort(const char*): the engine's fatal error. It builds its message on the stack and spins forever, so the
// message never leaves the game unless it's logged here.
X86_EXTERN_FUNC(__imp__sub_00064550);
X86_FUNC(sub_00064550) {
    LOGE("buffy: ShellAbort: %s", kernel::ReadCString(kernel::R32(ctx.esp + 4)).c_str());
    kernel::LogGuestStack(ctx, 64);
    logx::Flush();
    __imp__sub_00064550(ctx, base);
}

// XapiVerifyMediaInDrive@0: sends the Xbox drive's own SCSI authentication (0x4D014), which only that drive can answer.
// When it fails, XapiInitProcess skips the drive letters and goes to the dashboard, and the game shows its bad disc
// screen. Here the disc is the player's own image.
X86_FUNC(sub_0057C9F2) {
    LOGI("buffy: no Xbox drive, media check passed");
    ctx.eax = 0;
    ctx.esp += 4;
}

// TGame::EndFrame spins until 1000 / the limit at [[this+0x20]+0x7C] ms have passed, 60 frames a second as shipped, and 0
// skips it. At 120 the host paces the frames instead, without the spinning. The rest of the game takes the measured time,
// so its speed holds: a walk covered 292.6 units at 60 and 288.5 at 120. Characters still step at 60 a second, and the
// camera and everything else on every frame.
X86_EXTERN_FUNC(__imp__sub_00086190);
X86_FUNC(sub_00086190) {
    static uint32_t shipped = 0;
    const uint32_t limit = kernel::R32(ctx.ecx + 0x20) + 0x7C, now = kernel::R32(limit);
    if (now) shipped = now;
    const uint32_t want = app::g_high_frame_rate ? 0 : shipped;
    if (now != want) kernel::W32(limit, want);
    __imp__sub_00086190(ctx, base);
}
