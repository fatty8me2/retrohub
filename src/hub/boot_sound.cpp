/* Playback of the procedural startup sounds (boot_synth.cpp) on its OWN SDL audio device, so it never
 * touches, ducks or interrupts the game's audio. Non-blocking: rendering a 5-10 s sound takes a few ms. It plays to its natural end even after the game launches. */
#include "boot_anim.h"
#include "boot_synth.h"
#include "games.h"
#include "real_boot.h"
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
int g_fade = -1;                 // samples of fade-out left (-1 = none); total kFade
constexpr int kFade = 6615;      // 150 ms

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
    for (; i + 1 < n && g_pos + 1 < g_buf.size(); i += 2) {
        float g = v;
        if (g_fade >= 0) { g *= float(g_fade) / kFade; if (--g_fade < 0) { g_buf.clear(); g_pos = 0; g = 0; } }
        if (g_buf.empty()) break;
        f[i] = g_buf[g_pos++] * g; f[i + 1] = g_buf[g_pos++] * g;
    }
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

std::string custom_dir() { return hub::data_dir() + "boot_sounds"; }

bool sound_available(hub::System s) {
    if (!hub::realboot::custom_sound_path(custom_dir(), s).empty()) return true;
    switch (s) {
    case hub::System::N64: case hub::System::GB: case hub::System::GBC: case hub::System::GBA: case hub::System::NDS:
    case hub::System::N3DS: case hub::System::GC: case hub::System::PS1: case hub::System::PS2: case hub::System::WiiU:
    case hub::System::NES: case hub::System::SNES:
        return true;
    default: return false;
    }
}

void sound_stop() {
    if (!g_dev) return;
    SDL_LockAudioDevice(g_dev);
    if (g_pos < g_buf.size()) g_fade = kFade;     // fade out then silence (callback clears the buffer)
    SDL_UnlockAudioDevice(g_dev);
}

void sound_play(hub::System s, bool short_cut) {
    float vol = options().volume < 0 ? 0 : options().volume > 1 ? 1 : options().volume;
    if (!sound_available(s) || muted() || vol <= 0.f) return;
    if (!open_dev()) return;
    std::vector<float> b;   // the player's own file (userdata\boot_sounds\<system>.wav|ogg|mp3, max 15 s) replaces the synthesized sound
    const std::string custom = hub::realboot::custom_sound_path(custom_dir(), s);
    if (custom.empty() || !hub::realboot::decode_sound(custom, bootsynth::kSampleRate, &b)) b = bootsynth::render(s);
    if (short_cut) {   // ~2 s cut, quick 250 ms fade
        size_t keep = size_t(2.0 * bootsynth::kSampleRate) * 2, fade = size_t(0.25 * bootsynth::kSampleRate) * 2;
        if (b.size() > keep) {
            b.resize(keep);
            for (size_t i = 0; i < fade; i += 2) { float g = float(fade - i) / fade; b[keep - fade + i] *= g; b[keep - fade + i + 1] *= g; }
        }
    }
    SDL_LockAudioDevice(g_dev);
    g_buf = std::move(b); g_pos = 0; g_fade = -1; g_vol.store(vol);
    SDL_UnlockAudioDevice(g_dev);
    SDL_PauseAudioDevice(g_dev, 0);
}

} // namespace bootanim
