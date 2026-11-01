// [graphics] widescreen: more of the 3D scene at the sides of a level, with its 2D kept in proportion. The front end and
// movies stay 4:3 with bars: the title screen's 3D set shows what's built past its frame, and movies are 4:3 pictures.
#include <atomic>
#include <bit>
#include <cmath>

#include "app/app.h"
#include "gpu/backend.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"
#include "xdk/hle.h"

namespace {

constexpr uint32_t kHalfFovX = 0x007F4248;  // UTClipState::clipState+0x90, half the horizontal field of view
constexpr uint32_t kViewW = 0x00836A2C, kViewH = 0x00836A30;  // UTRendState::rendState+8, +0xC
constexpr uint32_t kGuestW = 640, kGuestH = 480;
constexpr float kConsoleAspect = 4.0f / 3.0f;

constexpr uint32_t kMovieThreadUp = 0x007715E0, kMovieCurrent = 0x007F30E8;  // what Movie_Playing reads
std::atomic<bool> g_in_level{false};

// the game's 2D (HUD, menus, text): pretransformed vertices on the back buffer
bool Is2D(const gpu::State& s) {
    if ((s.vs_handle & 1) || (s.vs_handle & 0xE) != 0x4) return false;  // not pretransformed
    uint32_t w = 0, h = 0;
    gpu::SurfaceSize(s.rt, w, h);
    return w == kGuestW && h == kGuestH;
}

// Asked for every draw, which also keeps the frame's shape up to date and says when a movie is on.
// 2D drawn on the back buffer: pulled towards the middle, except what's about as wide as the frame (gpu::SqueezeScreenX)
float ScreenSpace(const gpu::State& s) {
    const bool movie = kernel::R32(kMovieThreadUp) && kernel::R32(kMovieCurrent);
    gpu::g_movie = movie;
    app::g_frame_43 = !g_in_level || movie;
    return Is2D(s) ? kConsoleAspect / app::FrameAspect() : 1.0f;
}

}  // namespace

// TIF_Render_Implement::View_InternalSetup builds the projection and the culling planes from the half fields of view, so
// widening the horizontal one here shows more and culls less alike. The vertical one and the stored value stay, as
// View_SetViewport reads the stored value back and would widen it again.
X86_EXTERN_FUNC(__imp__sub_002A4870);
X86_FUNC(sub_002A4870) {
    const uint32_t stored = kernel::R32(kHalfFovX);
    if (!g_in_level || !app::Widescreen() || kernel::R32(kViewW) != kGuestW || kernel::R32(kViewH) != kGuestH) {
        __imp__sub_002A4870(ctx, base);
        return;
    }
    const float x = std::atan(std::tan(std::bit_cast<float>(stored)) * (app::WideAspect() / kConsoleAspect));
    kernel::W32(kHalfFovX, std::bit_cast<uint32_t>(x));
    __imp__sub_002A4870(ctx, base);
    kernel::W32(kHalfFovX, stored);
}

// TGameManagerImplementation::MapSet: only a level has a map. Each level, and the front end, is its own process.
X86_EXTERN_FUNC(__imp__sub_00018800);
X86_FUNC(sub_00018800) {
    g_in_level = true;
    __imp__sub_00018800(ctx, base);
}

// With widescreen off these change nothing, as the frame's shape is 4:3; they stay in place for the menu to turn it on.
void WidescreenInit() {
    app::g_frame_43 = true;
    gpu::g_wide_targets = true;
    gpu::g_screen_space_hook = ScreenSpace;
    gpu::g_screen_space_test = Is2D;
}
