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
        case hub::System::N64: case hub::System::NES: case hub::System::SNES: return Media::Cartridge;
        case hub::System::GB: case hub::System::GBC: case hub::System::GBA: return Media::SmallCart;
        case hub::System::NDS: case hub::System::N3DS: return Media::Card;
        case hub::System::GC: return Media::MiniDisc;
        case hub::System::PS1: case hub::System::PS2: case hub::System::WiiU: return Media::Disc;
        default: return Media::None;
    }
}


/* How the media goes into the console, and where. All positions are in CONSOLE-LOCAL art units (the units of boot_art.cpp's console
 * drawings; screen px = units * console scale). Retune here when the art changes (the HUB_SHOT_BOOTANIM frames show the result).
 *   Behind: the media sinks behind the console's top edge (rear slot: GB/GBC/GBA/DS/3DS); console is drawn over it.
 *   Slot:   the media is drawn over the console and clipped at the slot lip `clip` (N64/SNES top slot, NES front loader).
 *   Lid:    the lid opens, the disc is squashed into the well (drawn between well and lid) and the lid closes (GC, PS1).
 *   Tray:   the tray slides out, the disc lands on it and rides it back in (PS2).
 *   Front:  the disc slides edge-on into a front slot, clipped at `clip` (Wii U). */
enum class Insert { Behind, Slot, Lid, Tray, Front };
struct Geo {
    Insert kind;
    float ms;        // media scale relative to the console scale
    float ex, ey;    // media centre when inserted
    float hover;     // how far above (ex, ey) the media hovers before it goes in
    float clip;      // console-local y of the slot lip (Slot, Front)
    float ys_end;    // disc squash (vertical scale) when it sits in the well / on the tray / at the slot
    float push;      // Slot: extra final push down after the first insertion step (NES front loader)
    float cs;        // console scale multiplier (tall handhelds are drawn smaller)
    float cy;        // console centre as a fraction of the screen height
};
inline Geo geo_for(hub::System s) {
    using S = hub::System;
    switch (s) {
        case S::NES:   return {Insert::Slot, 0.85f, 0, -18, 120, 6, 1.f, 24, 1.0f, 0.66f};
        case S::SNES:  return {Insert::Slot, 0.70f, 0, -48, 110, -30, 1.f, 0, 1.0f, 0.66f};
        case S::GB: case S::GBC: return {Insert::Behind, 0.85f, 0, -149, 100, 0, 1.f, 0, 0.76f, 0.62f};
        case S::GBA:   return {Insert::Behind, 0.60f, 0, -86, 110, 0, 1.f, 0, 1.0f, 0.66f};
        case S::NDS: case S::N3DS: return {Insert::Behind, 0.70f, 0, -148, 100, 0, 1.f, 0, 0.76f, 0.62f};
        case S::GC:    return {Insert::Lid, 0.85f, 0, -28, 150, 0, 0.46f, 0, 1.0f, 0.66f};
        case S::PS1:   return {Insert::Lid, 0.62f, 0, -34, 150, 0, 0.47f, 0, 1.0f, 0.66f};
        case S::PS2:   return {Insert::Tray, 0.50f, -7, 93, 150, 0, 0.52f, 0, 1.0f, 0.66f};
        case S::WiiU:  return {Insert::Front, 0.55f, 54, 46, 120, 37, 0.14f, 0, 1.0f, 0.66f};
        case S::N64: default: return {Insert::Slot, 0.78f, 0, -38, 120, -8, 1.f, 0, 1.0f, 0.68f};
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
/* Full version: 5.2 s base (unhurried; the insertion and power-on get room to breathe). Disc consoles (lid / tray / front slot) need
 * more room for the lid or tray to open, the disc to settle and the lid to close, so their Insert phase is 1.6x longer.
 * `sound_s` (startup sound length, 0 = none): the Power phase is held so the sound plays out before the game window appears
 * (N64 keeps its tuned 5.2 s). `real`: the console's own startup follows, so the animation still plays in full but without our sound. */
inline Timeline timeline_for(bool short_version, hub::System sys = hub::System::N64, float sound_s = 0.f, bool real = false) {
    if (short_version) return Timeline{0.15f, 0.25f, 0.25f, 0.20f, 0.15f};   // 1.0 s
    Timeline tl{0.80f, 1.10f, 1.60f, 1.20f, 0.50f};
    if (sys == hub::System::N64) return tl;
    const Geo g = geo_for(sys);
    if (g.kind == Insert::Lid || g.kind == Insert::Tray || g.kind == Insert::Front) tl.insert = 2.6f;
    if (real) tl.power = 2.0f;
    else if (sound_s > 0.f) tl.power = std::min(6.0f, std::max(tl.power, sound_s - 1.0f));
    return tl;
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
/* online_launch: a room launch (or the player is in a room). quick_path: resume / quick-launch (same as a normal launch now). Online never gets the full version:
 * with "Skip in online games" it is skipped, otherwise at most the short version, so online sync never waits on it. */
inline Mode choose_mode(const Options& o, bool online_launch, bool quick_path) {
    if (!o.enabled) return Mode::Skip;
    if (online_launch) return o.skip_online ? Mode::Skip : Mode::Short;
    (void)quick_path;   // resume / quick-launch used to force the 1 s version, which made every non-details launch feel rushed
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
    bool real = false;   // the console's own startup follows: no synthesized/custom sound, and the animation still plays in full, then the console's own startup follows

    float end_t() const { return tl.total(); }   // real boot plays the whole animation too, then hands off
    void begin(Mode m, bool real_boot = false, hub::System sys = hub::System::N64, float sound_s = 0.f) {
        *this = Runner();
        real = real_boot;
        if (m == Mode::Skip) { finished_ = true; return; }
        tl = timeline_for(m == Mode::Short, sys, sound_s, real_boot);
        t = 0.f;
        running = true;
    }
    /* Advances the clock; returns true exactly once, on the frame the animation completes (or was skipped). */
    bool tick(float dt) {
        if (!running) return false;
        if (!armed) { armed = true; return false; }   // first frame: no time passes, input ignored
        t += std::max(0.f, std::min(dt, 0.1f));       // a hitch never eats the whole animation
        if (t >= end_t()) return finish();
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
        if (!running || real || sound_started || t < phase_start(tl, Phase::Power)) return false;
        sound_started = true;
        return true;
    }
    Phase phase() const { return running ? phase_at(tl, t) : (finished_ ? Phase::Done : Phase::Idle); }
    float progress() const { return running ? phase_progress(tl, t) : 0.f; }
};

} // namespace bootanim
