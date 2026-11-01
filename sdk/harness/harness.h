#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace gpu {
struct Surface;
}

// state the headless host's parts share
namespace harness {

inline std::atomic<uint64_t> g_frames{0};  // frames presented
// a host with a window (sdk/host) sets it: each frame the reference renderer draws comes here, not to a file
inline void (*g_show)(const gpu::Surface& front) = nullptr;
// and this when the backend starts, to get ready for them
inline void (*g_show_start)() = nullptr;

// an environment number, fallback when unset
inline long Env(const char* name, long fallback = 0) {
    const char* v = std::getenv(name);
    return v && *v ? std::atol(v) : fallback;
}

}  // namespace harness
