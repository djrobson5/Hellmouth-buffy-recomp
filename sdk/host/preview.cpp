// The picture when Vulkan isn't there or [graphics] renderer = 0: frames drawn on the CPU by the reference renderer,
// shown through SDL's 2D renderer. REF=N in the environment draws every Nth frame (6 by default: drawing one takes about
// 100 ms). SDL_Renderer is why this build needs an SDL with its renderer in, which the SDK's CMake leaves out.
#include <cstdlib>
#include <mutex>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "gpu/state.h"
#include "harness.h"
#include "host.h"
#include "reference_renderer.h"

namespace host {

namespace {

SDL_Renderer* g_renderer = nullptr;
SDL_Texture* g_texture = nullptr;
uint32_t g_tex_w = 0, g_tex_h = 0;

std::mutex g_lock;
std::vector<uint8_t> g_pixels;
uint32_t g_w = 0, g_h = 0;
bool g_new = false;

// the title's thread
void Show(const gpu::Surface& front) {
    std::lock_guard lock(g_lock);
    g_new = reference::ReadBgra(front, g_pixels, g_w, g_h) || g_new;
}

}  // namespace

void PreviewStart(SDL_Window* window) {
    g_renderer = SDL_CreateRenderer(window, nullptr);
    if (!g_renderer) {
        LOGE("preview: no renderer: %s", SDL_GetError());
        return;
    }
    SDL_SetRenderVSync(g_renderer, 1);
    LOGR("preview: %s", SDL_GetRendererName(g_renderer));
    setenv("REF", "6", 0);
    harness::g_show = Show;
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    SDL_RenderPresent(g_renderer);
}

void PreviewDraw(bool again) {
    if (!g_renderer) return;
    {
        std::lock_guard lock(g_lock);
        if (!g_new && !again) return;
        if (g_new) {
            if (!g_texture || g_tex_w != g_w || g_tex_h != g_h) {
                if (g_texture) SDL_DestroyTexture(g_texture);
                g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_BGRA32, SDL_TEXTUREACCESS_STREAMING, int(g_w), int(g_h));
                g_tex_w = g_w, g_tex_h = g_h;
            }
            if (g_texture) SDL_UpdateTexture(g_texture, nullptr, g_pixels.data(), int(g_w * 4));
            g_new = false;
        }
    }
    int w = 0, h = 0;
    SDL_GetCurrentRenderOutputSize(g_renderer, &w, &h);
    // the frame at its shape on the screen, with bars
    float aspect = app::FrameAspect();
    SDL_FRect dst{0, 0, float(w), float(h)};
    if (float(w) > float(h) * aspect) dst.w = float(h) * aspect;
    else dst.h = float(w) / aspect;
    dst.x = (float(w) - dst.w) / 2;
    dst.y = (float(h) - dst.h) / 2;
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    if (g_texture) SDL_RenderTexture(g_renderer, g_texture, nullptr, &dst);
    SDL_RenderPresent(g_renderer);
}

}  // namespace host
