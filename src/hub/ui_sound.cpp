#include "ui_sound.h"
#include "boot_synth.h"
#include <SDL.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace ui_sound {
namespace {

struct Voice { const std::vector<float>* buf = nullptr; size_t pos = 0; };

std::vector<float> g_sfx[(int)bootsynth::Ui::Count];
std::vector<float> g_music;
Voice g_voices[6];
size_t g_music_pos = 0;
std::atomic<float> g_vol{0.f};          // 0..1 effective gain for sfx
std::atomic<float> g_music_gain{0.f};   // current music gain (ramped in the callback)
std::atomic<float> g_music_target{0.f};
SDL_AudioDeviceID g_dev = 0;
bool g_tried = false;
bool g_on = true, g_music_on = false, g_chime_on = true;
int g_pct = 30;
bool g_game = false, g_focus = true;
Uint32 g_last[6] = {};

bool env_flag(const char* e) { const char* v = std::getenv(e); return v && *v && std::strcmp(v, "0") != 0; }

void SDLCALL cb(void*, Uint8* out, int len) {
    float* f = reinterpret_cast<float*>(out);
    size_t n = size_t(len) / sizeof(float);
    std::memset(out, 0, len);
    const float v = g_vol.load();
    for (auto& vo : g_voices) {
        if (!vo.buf) continue;
        for (size_t i = 0; i + 1 < n; i += 2) {
            if (vo.pos + 1 >= vo.buf->size()) { vo.buf = nullptr; break; }
            f[i] += (*vo.buf)[vo.pos++] * v; f[i + 1] += (*vo.buf)[vo.pos++] * v;
        }
    }
    float mg = g_music_gain.load(); const float mt = g_music_target.load();
    if (!g_music.empty() && (mg > 0.f || mt > 0.f)) {
        for (size_t i = 0; i + 1 < n; i += 2) {
            mg += (mt - mg) * 0.00008f;    // ~0.3 s ramp
            f[i] += g_music[g_music_pos] * mg; f[i + 1] += g_music[g_music_pos + 1] * mg;
            g_music_pos += 2; if (g_music_pos + 1 >= g_music.size()) g_music_pos = 0;
        }
        g_music_gain.store(mg);
    }
}

bool open_dev() {
    if (g_dev) return true;
    if (g_tried) return false;
    g_tried = true;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;
    for (int i = 0; i < (int)bootsynth::Ui::Count; i++) g_sfx[i] = bootsynth::render_ui((bootsynth::Ui)i);
    g_music = bootsynth::render_ambient();
    SDL_AudioSpec want{}, have{};
    want.freq = bootsynth::kSampleRate; want.format = AUDIO_F32SYS; want.channels = 2; want.samples = 1024; want.callback = cb;
    g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!g_dev) return false;
    SDL_PauseAudioDevice(g_dev, 0);
    return true;
}

} // namespace

bool env_muted() {
    for (const char* e : {"SSB64_MUTE", "RETROHUB_MUTE", "RETRORUN_MUTE", "N64RUN_MUTE", "RETROHUB_QA", "HUB_SCREENSHOT"})
        if (env_flag(e)) return true;
    return false;
}

bool allowed(bool sounds_on, int volume_pct, bool game_running, bool focused, bool muted) {
    return sounds_on && volume_pct > 0 && !game_running && focused && !muted;
}

void configure(bool sounds_on, int volume_pct, bool music_on, bool notif_chime_on) {
    g_on = sounds_on; g_pct = volume_pct < 0 ? 0 : volume_pct > 100 ? 100 : volume_pct; g_music_on = music_on; g_chime_on = notif_chime_on;
    g_vol.store(g_pct / 100.f * 0.8f);
}

void tick(bool game_running, bool focused) {
    g_game = game_running; g_focus = focused;
    const bool music = g_music_on && allowed(true, g_pct, game_running, focused, env_muted());
    if (music && !g_dev && !open_dev()) return;
    if (g_dev) g_music_target.store(music ? g_pct / 100.f * 0.35f : 0.f);
}

void play(Sfx s) {
    if (s == Sfx::Notify && !g_chime_on) return;
    const bool ok = s == Sfx::Notify ? (g_pct > 0 && !g_game && g_focus && !env_muted())
                                     : allowed(g_on, g_pct, g_game, g_focus, env_muted());
    if (!ok || !open_dev()) return;
    const int idx = s == Sfx::Notify ? (int)bootsynth::Ui::Chime : (int)s;
    const Uint32 now = SDL_GetTicks();
    if (now - g_last[(int)s] < 50) return;     // debounce bursts (focus seeding, key repeat)
    g_last[(int)s] = now;
    SDL_LockAudioDevice(g_dev);
    Voice* v = &g_voices[0];
    for (auto& c : g_voices) if (!c.buf) { v = &c; break; }
    v->buf = &g_sfx[idx]; v->pos = 0;
    SDL_UnlockAudioDevice(g_dev);
}

void shutdown() {
    if (g_dev) { SDL_CloseAudioDevice(g_dev); g_dev = 0; }
}

} // namespace ui_sound
