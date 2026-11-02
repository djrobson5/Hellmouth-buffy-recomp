// [graphics] widescreen: more of the 3D scene at the sides of a level, with its 2D kept in proportion. The front end and
// movies stay 4:3 with bars: the title screen's 3D set shows what's built past its frame, and movies are 4:3 pictures.
#include <atomic>
#include <bit>
#include <cmath>

#include "app/app.h"
#include "core/log.h"
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

// The HUD's item (IWeaponStatDisplay::RenderViewItem) is a small 3D view: TGORender::CustomViewport_Render first draws a 2D
// quad over its rectangle at z 1 with ZFUNC ALWAYS and z writes on (Context_ClearDepthRect), so the world can't hide it,
// then sets a viewport on the same rectangle and draws the item. The quad is 2D, so it's pulled towards the middle with
// the rest of the HUD, and the viewport has to follow or the item is drawn beside its ring and behind the world.
bool g_custom_viewport = false;

X86_EXTERN_FUNC(__imp__sub_00098020);
X86_FUNC(sub_00098020) {
    g_custom_viewport = true;
    __imp__sub_00098020(ctx, base);
    g_custom_viewport = false;
}

// CD3DMatrixState::SetViewport(D3DVIEWPORT8*), which View_InternalSetup calls once the projection is built: within a
// custom view, narrowed as gpu::SqueezeScreenX narrows 2D (nothing about as wide as the frame), and kept inside the quad
// where it lands between pixels. Only the device's viewport changes, so the item keeps the console's projection.
X86_EXTERN_FUNC(__imp__sub_002A3830);
X86_FUNC(sub_002A3830) {
    const uint32_t vp = kernel::R32(ctx.esp + 4);
    const float f = kConsoleAspect / app::FrameAspect();
    const uint32_t x = vp ? kernel::R32(vp) : 0, w = vp ? kernel::R32(vp + 8) : 0;
    if (!g_custom_viewport || !g_in_level || !vp || f >= 1.0f || w >= 0.9f * kGuestW) {
        __imp__sub_002A3830(ctx, base);
        return;
    }
    const float b = kGuestW / 2.0f * (1 - f);
    const uint32_t left = uint32_t(std::ceil(f * float(x) + b)), right = uint32_t(std::floor(f * float(x + w) + b));
    kernel::W32(vp, left);
    kernel::W32(vp + 8, right > left ? right - left : 1);
    LOG_FIRST(1, "widescreen: the HUD item's view at x %u, %u wide, is drawn at x %u, %u wide", x, w, left,
              right > left ? right - left : 1);
    __imp__sub_002A3830(ctx, base);
    kernel::W32(vp, x);
    kernel::W32(vp + 8, w);
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
