// mixer output through SDL, as xaudio2.cpp: 10 ms pieces, three queued at most. AUDIO_LOG in the environment logs the
// loudest sample and the queue each second, to tell silence from sound without listening.
#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <thread>
#include <vector>

#include "app/app.h"
#include "audio/audio.h"
#include "core/log.h"

namespace audio {

void StartOutput(std::function<void(float*, uint32_t)> render) {
    std::thread([render] {
        constexpr uint32_t kFrames = kRate / 100, kBytes = kFrames * 8;
        const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, int(kRate)};
        SDL_AudioStream* s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (s && SDL_ResumeAudioStreamDevice(s)) {
            const char* name = SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(s));
            LOGR("audio: SDL, %s, %s", SDL_GetCurrentAudioDriver(), name ? name : "default device");
        } else {
            LOGE("audio: no sound: %s", SDL_GetError());
            s = nullptr;
        }
        static const bool log = std::getenv("AUDIO_LOG") != nullptr;
        float peak = 0;
        std::vector<float> b(kFrames * 2);
        for (uint32_t n = 0;;) {
            if (s && SDL_GetAudioStreamQueued(s) >= int(3 * kBytes)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            // paused (app::Held): the mixer takes no time, so the game's sounds and music wait where they were;
            // muted, they play on unheard
            if (app::Held()) std::fill(b.begin(), b.end(), 0.0f);
            else {
                render(b.data(), kFrames);
                if (app::Muted()) std::fill(b.begin(), b.end(), 0.0f);
            }
            if (s) SDL_PutAudioStreamData(s, b.data(), int(kBytes));
            // without a device the mixer still takes its time: the game waits on its sounds ending
            else std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (log) {
                for (float x : b) peak = std::max(peak, std::fabs(x));
                if (++n % 100 == 0) {
                    LOGR("audio: %u pieces, peak %.3f, %d bytes queued", n, peak, s ? SDL_GetAudioStreamQueued(s) : 0);
                    peak = 0;
                }
            }
        }
    }).detach();
}

}  // namespace audio
