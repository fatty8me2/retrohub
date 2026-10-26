#pragma once
/* Pure (no ImGui, no SDL) part of the game start animation: media mapping, options, timeline and the state machine.
 * Header-only so the console test (test_boot_anim.cpp) links nothing but hublogic. */

#include <algorithm>
#include <cmath>
#include "library.h"

namespace bootanim {

enum class Media { Cartridge, SmallCart /*GB/GBC/GBA*/, Card /*DS/3DS*/, MiniDisc /*GC*/, Disc /*PS1/PS2/WiiU*/, None };

inline Media media_for(hub::System s) {
    switch (s) {
        case hub::System::N64: return Media::Cartridge;
        case hub::System::GB: case hub::System::GBC: case hub::System::GBA: return Media::SmallCart;
        case hub::System::NDS: case hub::System::N3DS: return Media::Card;
        case hub::System::GC: return Media::MiniDisc;
        case hub::System::PS1: case hub::System::PS2: case hub::System::WiiU: return Media::Disc;
        default: return Media::None;
    }
}

struct Options {
    bool enabled = true;          // master switch: "Play the start animation"
    bool sounds = true;           // "Startup sounds"
    float volume = 0.7f;          // 0..1; boot_sound.cpp scales its output by this (read bootanim::options().volume)
    bool short_version = false;   // "Short version" (~1 s)
    bool skip_online = true;      // "Skip in online games"
};

/* Phase boundaries in seconds. */
enum class Phase { Idle, Slide, Fly, Insert, Power, Fade, Done };
struct Timeline {
    float slide, fly, insert, power, fade;
    float total() const { return slide + fly + insert + power + fade; }
};
inline Timeline timeline_for(bool short_version) {
    return short_version ? Timeline{0.15f, 0.25f, 0.25f, 0.20f, 0.15f}    // 1.0 s
                         : Timeline{0.60f, 0.70f, 0.75f, 0.65f, 0.40f};   // 3.1 s
}
inline Phase phase_at(const Timeline& tl, float t) {
    if (t < 0.f) return Phase::Idle;
    float e = tl.slide;
    if (t < e) return Phase::Slide;
    if (t < (e += tl.fly)) return Phase::Fly;
    if (t < (e += tl.insert)) return Phase::Insert;
    if (t < (e += tl.power)) return Phase::Power;
    if (t < (e += tl.fade)) return Phase::Fade;
    return Phase::Done;
}
inline float phase_start(const Timeline& tl, Phase p) {
    switch (p) {
        case Phase::Slide: return 0.f;
        case Phase::Fly: return tl.slide;
        case Phase::Insert: return tl.slide + tl.fly;
        case Phase::Power: return tl.slide + tl.fly + tl.insert;
        case Phase::Fade: return tl.slide + tl.fly + tl.insert + tl.power;
        case Phase::Done: return tl.total();
        default: return 0.f;
    }
}
inline float phase_len(const Timeline& tl, Phase p) {
    switch (p) {
        case Phase::Slide: return tl.slide;
        case Phase::Fly: return tl.fly;
        case Phase::Insert: return tl.insert;
        case Phase::Power: return tl.power;
        case Phase::Fade: return tl.fade;
        default: return 0.f;
    }
}
inline float phase_progress(const Timeline& tl, float t) {   // 0..1 within the phase containing t
    Phase p = phase_at(tl, t);
    float len = phase_len(tl, p);
    if (p == Phase::Done) return 1.f;
    if (p == Phase::Idle || len <= 0.f) return 0.f;
    return std::min(1.f, std::max(0.f, (t - phase_start(tl, p)) / len));
}

enum class Mode { Skip, Short, Full };
/* online_launch: a room launch (or the player is in a room). quick_path: resume / quick-launch. Online never gets the full version:
 * with "Skip in online games" it is skipped, otherwise at most the short version, so online sync never waits on it. */
inline Mode choose_mode(const Options& o, bool online_launch, bool quick_path) {
    if (!o.enabled) return Mode::Skip;
    if (online_launch) return o.skip_online ? Mode::Skip : Mode::Short;
    if (quick_path) return Mode::Short;
    return o.short_version ? Mode::Short : Mode::Full;
}

inline float clamp01(float x) { return x < 0.f ? 0.f : x > 1.f ? 1.f : x; }
inline float ease_in_out(float x) { x = clamp01(x); return x * x * (3.f - 2.f * x); }
inline float ease_out(float x) { x = clamp01(x); return 1.f - (1.f - x) * (1.f - x) * (1.f - x); }
inline float ease_out_back(float x) {
    x = clamp01(x);
    const float c1 = 1.70158f, c3 = c1 + 1.f;
    float u = x - 1.f;
    return 1.f + c3 * u * u * u + c1 * u * u;
}

/* The state machine: begin() -> tick(dt) each frame -> finished() once. skip() jumps straight to finished (launch now).
 * A skip is ignored on the frame the animation began (the click/A press that started it must not skip it). */
struct Runner {
    Timeline tl{};
    float t = -1.f;
    bool running = false, finished_ = false, skipped = false, sound_started = false, armed = false;

    void begin(Mode m) {
        *this = Runner();
        if (m == Mode::Skip) { finished_ = true; return; }
        tl = timeline_for(m == Mode::Short);
        t = 0.f;
        running = true;
    }
    /* Advances the clock; returns true exactly once, on the frame the animation completes (or was skipped). */
    bool tick(float dt) {
        if (!running) return false;
        if (!armed) { armed = true; return false; }   // first frame: no time passes, input ignored
        t += std::max(0.f, std::min(dt, 0.1f));       // a hitch never eats the whole animation
        if (t >= tl.total()) return finish();
        return false;
    }
    bool skip() {
        if (!running || !armed) return false;
        skipped = true;
        return finish();
    }
    bool finish() { running = false; finished_ = true; return true; }
    /* True once, when the Power phase is first entered: start the startup sound here. */
    bool want_sound() {
        if (!running || sound_started || t < phase_start(tl, Phase::Power)) return false;
        sound_started = true;
        return true;
    }
    Phase phase() const { return running ? phase_at(tl, t) : (finished_ ? Phase::Done : Phase::Idle); }
    float progress() const { return running ? phase_progress(tl, t) : 0.f; }
};

} // namespace bootanim
