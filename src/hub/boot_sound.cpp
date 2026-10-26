/* Playback of the procedural startup sounds (boot_synth.cpp) on its OWN SDL audio device, so it never
 * touches, ducks or interrupts the game's audio. Non-blocking: rendering a ~2 s sound takes a few ms. */
#include "boot_anim.h"
#include "boot_synth.h"
#include <SDL.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace bootanim {
namespace {

SDL_AudioDeviceID g_dev = 0;
std::vector<float> g_buf;        // guarded by the device lock
size_t g_pos = 0;                // in floats
std::atomic<float> g_vol{0.7f};
bool g_tried = false;

bool muted() {
    for (const char* e : {"SSB64_MUTE", "RETRORUN_MUTE", "N64RUN_MUTE", "RETROHUB_QA"}) {
        const char* v = std::getenv(e);
        if (v && *v && std::strcmp(v, "0") != 0) return true;
    }
    return false;
}

void SDLCALL cb(void*, Uint8* out, int len) {
    float* f = reinterpret_cast<float*>(out);
    size_t n = size_t(len) / sizeof(float), i = 0;
    float v = g_vol.load();
    for (; i < n && g_pos < g_buf.size(); i++) f[i] = g_buf[g_pos++] * v;
    for (; i < n; i++) f[i] = 0.f;
}

bool open_dev() {
    if (g_dev) return true;
    if (g_tried) return false;
    g_tried = true;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;
    SDL_AudioSpec want{}, have{};
    want.freq = bootsynth::kSampleRate; want.format = AUDIO_F32SYS; want.channels = 2; want.samples = 1024; want.callback = cb;
    g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);   // no format changes allowed: we render exactly this
    if (!g_dev) return false;
    SDL_PauseAudioDevice(g_dev, 1);
    return true;
}

} // namespace

bool sound_available(hub::System s) {
    switch (s) {
    case hub::System::N64: case hub::System::GB: case hub::System::GBC: case hub::System::GBA: case hub::System::NDS:
    case hub::System::N3DS: case hub::System::GC: case hub::System::PS1: case hub::System::PS2: case hub::System::WiiU:
        return true;
    default: return false;
    }
}

void sound_set_volume(float v) { g_vol.store(v < 0 ? 0 : v > 1 ? 1 : v); }

void sound_stop() {
    if (!g_dev) return;
    SDL_LockAudioDevice(g_dev);
    g_buf.clear(); g_pos = 0;
    SDL_UnlockAudioDevice(g_dev);
    SDL_PauseAudioDevice(g_dev, 1);
}

void sound_play(hub::System s) {
    if (!sound_available(s) || muted() || g_vol.load() <= 0.f) return;
    if (!open_dev()) return;
    std::vector<float> b = bootsynth::render(s);
    SDL_LockAudioDevice(g_dev);
    g_buf = std::move(b); g_pos = 0;
    SDL_UnlockAudioDevice(g_dev);
    SDL_PauseAudioDevice(g_dev, 0);
}

} // namespace bootanim
