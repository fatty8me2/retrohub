/* Tiny offline synth + original per-console startup sounds. All melodies/timbres are invented here;
 * nothing is sampled or transcribed from any console. See docs/BOOT_SOUNDS.md. */
#include "boot_synth.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace bootsynth {
namespace {

constexpr float kPi = 3.14159265358979f;
enum Wave { Sine, Square, Tri, Saw };

struct Rng { uint32_t s = 12345u; float next() { s = s * 1664525u + 1013904223u; return ((s >> 8) & 0xFFFF) / 32768.0f - 1.0f; } };

float osc(Wave w, float ph) {            // ph in cycles
    ph -= std::floor(ph);
    switch (w) {
    case Sine:   return std::sin(2 * kPi * ph);
    case Square: return ph < 0.5f ? 0.6f : -0.6f;
    case Tri:    return 4 * std::fabs(ph - 0.5f) - 1;
    default:     return 2 * ph - 1;
    }
}

struct Song {
    std::vector<float> L, R;
    float len;
    explicit Song(float seconds) : L(size_t(seconds * kSampleRate), 0.f), R(L), len(seconds) {}
    size_t n() const { return L.size(); }
    void add(size_t i, float v, float pan) {
        if (i >= L.size()) return;
        L[i] += v * (1 - pan) * 0.5f; R[i] += v * (1 + pan) * 0.5f;
    }
};

/* ADSR value at time t of a note of length `dur` (release after dur, lasting `rel`). */
float adsr(float t, float dur, float a, float d, float s, float rel) {
    if (t < 0) return 0;
    if (t < dur) {
        if (t < a) return t / a;
        if (t < a + d) return 1 + (s - 1) * (t - a) / d;
        return s;
    }
    float r = (t - dur) / rel;
    return r >= 1 ? 0 : s * (1 - r);
}

struct Note {
    float t0, dur, f0, f1 = 0;           // f1 > 0 = linear glide target over dur
    Wave w = Sine;
    float amp = 0.3f, pan = 0, a = 0.005f, d = 0.1f, s = 0.5f, rel = 0.2f;
    float vib = 0, tremolo = 0, lp = 0;  // lp: one-pole cutoff Hz (0 = off)
    float detune = 0;                    // second oscillator detuned by this many cents
    float partial2 = 0;                  // extra inharmonic partial amount (bell)
};

void note(Song& S, const Note& n) {
    size_t i0 = size_t(n.t0 * kSampleRate), cnt = size_t((n.dur + n.rel) * kSampleRate);
    float ph = 0, ph2 = 0, ph3 = 0, y = 0;
    float k = n.lp > 0 ? 1 - std::exp(-2 * kPi * n.lp / kSampleRate) : 1;
    float dt = std::pow(2.f, n.detune / 1200.f);
    for (size_t i = 0; i < cnt; i++) {
        float t = float(i) / kSampleRate;
        float f = n.f1 > 0 ? n.f0 + (n.f1 - n.f0) * std::min(1.f, t / n.dur) : n.f0;
        if (n.vib > 0) f *= 1 + n.vib * std::sin(2 * kPi * 5.5f * t);
        ph += f / kSampleRate; ph2 += f * dt / kSampleRate; ph3 += f * 2.76f / kSampleRate;
        float v = osc(n.w, ph);
        if (n.detune != 0) v = 0.5f * (v + osc(n.w, ph2));
        if (n.partial2 > 0) v += n.partial2 * osc(Sine, ph3) * std::exp(-t * 6);
        y += k * (v - y);
        float e = adsr(t, n.dur, n.a, n.d, n.s, n.rel);
        float tr = n.tremolo > 0 ? 1 - n.tremolo + n.tremolo * (0.5f + 0.5f * std::sin(2 * kPi * 7.f * t)) : 1;
        S.add(i0 + i, y * e * tr * n.amp, n.pan);
    }
}

/* Filtered noise burst. lp/hp one-pole corner frequencies; exp decay. */
void noise(Song& S, Rng& r, float t0, float dur, float amp, float lp, float hp, float decay, float pan = 0) {
    size_t i0 = size_t(t0 * kSampleRate), cnt = size_t(dur * kSampleRate);
    float kl = 1 - std::exp(-2 * kPi * lp / kSampleRate), kh = 1 - std::exp(-2 * kPi * hp / kSampleRate);
    float yl = 0, yh = 0;
    for (size_t i = 0; i < cnt; i++) {
        float t = float(i) / kSampleRate;
        yl += kl * (r.next() - yl);
        yh += kh * (yl - yh);
        S.add(i0 + i, (yl - yh) * amp * std::exp(-t * decay), pan);
    }
}

/* Noise whose low-pass corner sweeps lo->hi: a disc spinning up. */
void whoosh(Song& S, Rng& r, float t0, float dur, float amp, float lo, float hi) {
    size_t i0 = size_t(t0 * kSampleRate), cnt = size_t(dur * kSampleRate);
    float y = 0, y2 = 0;
    for (size_t i = 0; i < cnt; i++) {
        float u = float(i) / cnt, fc = lo + (hi - lo) * u * u;
        float k = 1 - std::exp(-2 * kPi * fc / kSampleRate);
        y += k * (r.next() - y); y2 += k * (y - y2);
        float e = std::sin(kPi * u);
        S.add(i0 + i, y2 * amp * e * 3, 0);
    }
}

void thunk(Song& S, Rng& r, float t0, float amp, float f = 90) {   // mechanical clunk: pitch-dropping thump + click
    Note n{t0, 0.01f, f * 1.8f, f, Sine, amp, 0, 0.001f, 0.08f, 0.0f, 0.12f};
    note(S, n);
    noise(S, r, t0, 0.05f, amp * 0.8f, 3000, 200, 90);
}
void click(Song& S, Rng& r, float t0, float amp) {                 // small plastic click
    noise(S, r, t0, 0.02f, amp, 6000, 800, 300);
    noise(S, r, t0 + 0.045f, 0.02f, amp * 0.6f, 5000, 600, 300);
}

/* Schroeder reverb: 4 combs + 2 allpass, wet mixed in. */
struct Comb { std::vector<float> b; size_t p = 0; float fb, damp, st = 0;
    Comb(int d, float f, float dm) : b(d, 0.f), fb(f), damp(dm) {}
    float run(float x) { float o = b[p]; st = o * (1 - damp) + st * damp; b[p] = x + st * fb; p = (p + 1) % b.size(); return o; } };
struct Allpass { std::vector<float> b; size_t p = 0;
    explicit Allpass(int d) : b(d, 0.f) {}
    float run(float x) { float bo = b[p]; float o = -x + bo; b[p] = x + bo * 0.5f; p = (p + 1) % b.size(); return o; } };

void reverb(Song& S, float wet, float room, float damp) {
    static const int dl[4] = {1557, 1617, 1491, 1422}, dr[4] = {1580, 1640, 1514, 1445};
    std::vector<float>* ch[2] = {&S.L, &S.R};
    for (int c = 0; c < 2; c++) {
        Comb cb[4] = {{c ? dr[0] : dl[0], room, damp}, {c ? dr[1] : dl[1], room, damp},
                      {c ? dr[2] : dl[2], room, damp}, {c ? dr[3] : dl[3], room, damp}};
        Allpass ap[2] = {Allpass(c ? 590 : 556), Allpass(c ? 460 : 441)};
        for (float& x : *ch[c]) {
            float w = 0;
            for (auto& k : cb) w += k.run(x * 0.25f);
            for (auto& a : ap) w = a.run(w);
            x = x + w * wet;
        }
    }
}

/* Simple feedback delay with slight L/R offset (adds stereo width). */
void echo(Song& S, float sec, float fb, float mix) {
    size_t d = size_t(sec * kSampleRate);
    for (size_t i = d; i < S.n(); i++) {
        S.L[i] += S.R[i - d] * fb * mix;      // ping-pong
        S.R[i] += S.L[i - d] * fb * mix;
    }
}

float midi(float m) { return 440.f * std::pow(2.f, (m - 69) / 12.f); }

std::vector<float> finish(Song& S) {
    const float tail = 0.25f;                           // fade the last 250 ms so there is never a click
    size_t n = S.n(), nf = size_t(tail * kSampleRate);
    float peak = 1e-6f;
    for (size_t i = 0; i < n; i++) {
        if (i + nf >= n) { float g = float(n - i) / nf; S.L[i] *= g * g; S.R[i] *= g * g; }
        peak = std::max({peak, std::fabs(S.L[i]), std::fabs(S.R[i])});
    }
    float g = 0.8f / peak;
    std::vector<float> out(n * 2);
    for (size_t i = 0; i < n; i++) { out[2 * i] = S.L[i] * g; out[2 * i + 1] = S.R[i] * g; }
    return out;
}

/* ---------------- the sounds ---------------- */

std::vector<float> n64() {          // cartridge clunk, a power hum that blooms and fades, a soft rising two-step
    Song S(1.7f); Rng r;
    thunk(S, r, 0.00f, 0.9f, 80);
    click(S, r, 0.07f, 0.25f);
    note(S, {0.18f, 1.0f, 55, 0, Tri, 0.22f, 0, 0.25f, 0.12f, 0.7f, 0.4f, 0, 0, 220});
    note(S, {0.18f, 1.0f, 110, 0, Sine, 0.12f, 0, 0.3f, 0.1f, 0.6f, 0.4f});
    note(S, {0.30f, 0.45f, 220, 440, Sine, 0.12f, -0.2f, 0.04f, 0.1f, 0.5f, 0.35f});
    note(S, {0.55f, 0.50f, 330, 660, Tri, 0.07f, 0.2f, 0.05f, 0.1f, 0.4f, 0.35f, 0, 0, 1500});
    reverb(S, 0.25f, 0.7f, 0.4f);
    return finish(S);
}
std::vector<float> gb(bool color) { // click + two bright square pings (three on Color, higher)
    Song S(color ? 1.35f : 1.2f); Rng r;
    click(S, r, 0.0f, 0.5f);
    float b = color ? 7.f : 0.f;
    note(S, {0.12f, 0.10f, midi(79 + b), 0, Square, 0.20f, 0, 0.002f, 0.05f, 0.5f, 0.18f});
    note(S, {0.30f, 0.12f, midi(86 + b), 0, Square, 0.20f, 0, 0.002f, 0.05f, 0.5f, 0.45f});
    if (color) note(S, {0.50f, 0.12f, midi(91 + b), 0, Square, 0.16f, 0, 0.002f, 0.05f, 0.5f, 0.5f});
    note(S, {0.30f, 0.12f, midi(74 + b), 0, Tri, 0.12f, 0, 0.002f, 0.05f, 0.4f, 0.5f});
    echo(S, 0.16f, 0.5f, 0.6f);
    return finish(S);
}
std::vector<float> gba() {          // click, shimmering detuned rising sweep that resolves into a bell
    Song S(2.0f); Rng r;
    click(S, r, 0.0f, 0.5f);
    for (int i = 0; i < 4; i++) {
        float pan = (i - 1.5f) * 0.4f, f = 300 + i * 70;
        note(S, {0.12f + i * 0.04f, 0.9f, f, f * 5.5f, Sine, 0.10f, pan, 0.2f, 0.2f, 0.7f, 0.4f, 0, 0.5f, 0, 14.f * (i + 1)});
    }
    note(S, {1.05f, 0.1f, midi(88), 0, Sine, 0.22f, -0.2f, 0.003f, 0.2f, 0.3f, 0.7f, 0, 0, 0, 0});
    note(S, {1.05f, 0.1f, midi(88), 0, Sine, 0.12f, 0.3f, 0.003f, 0.2f, 0.3f, 0.7f, 0, 0, 0, 8});
    note(S, {1.05f, 0.1f, midi(76), 0, Tri, 0.10f, 0, 0.003f, 0.2f, 0.3f, 0.6f});
    reverb(S, 0.35f, 0.75f, 0.3f);
    return finish(S);
}
std::vector<float> nds() {          // card click + soft round chime: three notes up, one settling
    Song S(2.0f); Rng r;
    click(S, r, 0.0f, 0.45f);
    const float m[4] = {72, 76, 79, 84};
    for (int i = 0; i < 4; i++)
        note(S, {0.18f + i * 0.17f, 0.08f, midi(m[i]), 0, Sine, 0.22f, (i - 1.5f) * 0.25f, 0.006f, 0.15f, 0.3f, 0.9f, 0, 0, 0, 0, 0});
    note(S, {0.86f, 0.1f, midi(67), 0, Tri, 0.14f, 0, 0.01f, 0.2f, 0.4f, 0.9f, 0, 0, 1800});
    reverb(S, 0.3f, 0.75f, 0.35f);
    return finish(S);
}
std::vector<float> n3ds() {         // like DS but airier: soft shutter-ish click and a glassy descending-then-up pair
    Song S(2.0f); Rng r;
    click(S, r, 0.0f, 0.3f);
    const float m[5] = {79, 74, 83, 86, 91};
    const float t[5] = {0.15f, 0.30f, 0.45f, 0.62f, 0.8f};
    for (int i = 0; i < 5; i++)
        note(S, {t[i], 0.06f, midi(m[i]), 0, Sine, 0.18f, (i % 2 ? 0.3f : -0.3f), 0.004f, 0.2f, 0.2f, 0.8f, 0, 0, 0, 5});
    reverb(S, 0.35f, 0.8f, 0.3f);
    return finish(S);
}
std::vector<float> gc() {           // lid thunk, disc spin-up, bubbly pentatonic arpeggio with a pop at the end
    Song S(2.3f); Rng r;
    thunk(S, r, 0.0f, 0.7f, 100);
    whoosh(S, r, 0.1f, 0.7f, 0.5f, 300, 2500);
    const float m[9] = {62, 69, 74, 78, 81, 86, 81, 78, 74};   // D major pentatonic-ish, invented
    for (int i = 0; i < 9; i++) {
        float t = 0.45f + i * 0.11f;
        note(S, {t, 0.04f, midi(m[i]), 0, Tri, 0.2f, std::sin(i * 1.7f) * 0.6f, 0.003f, 0.08f, 0.2f, 0.25f, 0, 0, 3500});
        note(S, {t, 0.02f, midi(m[i] + 12), 0, Sine, 0.08f, 0, 0.002f, 0.05f, 0.1f, 0.12f});
    }
    note(S, {1.5f, 0.2f, midi(62), midi(74), Sine, 0.2f, 0, 0.01f, 0.2f, 0.5f, 0.6f});
    note(S, {1.5f, 0.2f, midi(66), 0, Tri, 0.12f, -0.3f, 0.01f, 0.2f, 0.5f, 0.6f, 0, 0, 2000});
    note(S, {1.5f, 0.2f, midi(69), 0, Tri, 0.12f, 0.3f, 0.01f, 0.2f, 0.5f, 0.6f, 0, 0, 2000});
    reverb(S, 0.25f, 0.7f, 0.4f);
    return finish(S);
}
std::vector<float> ps1() {          // tray close + disc spin, deep slowly opening swell with a faint high shimmer
    Song S(2.8f); Rng r;
    thunk(S, r, 0.0f, 0.5f, 70);
    whoosh(S, r, 0.1f, 0.9f, 0.35f, 200, 1500);
    const float m[4] = {38, 45, 50, 57};
    for (int i = 0; i < 4; i++) {
        note(S, {0.35f, 1.3f, midi(m[i]), 0, Saw, 0.16f, (i - 1.5f) * 0.3f, 0.9f, 0.3f, 0.8f, 0.9f, 0.003f, 0, 500.f + i * 150.f, 9});
    }
    note(S, {0.9f, 0.9f, midi(86), 0, Sine, 0.05f, -0.4f, 0.6f, 0.2f, 0.6f, 0.9f, 0.01f, 0.3f});
    note(S, {1.0f, 0.9f, midi(93), 0, Sine, 0.04f, 0.4f, 0.6f, 0.2f, 0.6f, 0.9f, 0.01f, 0.3f});
    reverb(S, 0.35f, 0.85f, 0.4f);
    return finish(S);
}
std::vector<float> ps2() {          // airy shimmering pad, a gentle breath of noise, soft high sparkles
    Song S(2.8f); Rng r;
    thunk(S, r, 0.0f, 0.35f, 110);
    whoosh(S, r, 0.1f, 1.4f, 0.25f, 400, 6000);
    const float m[5] = {50, 57, 62, 66, 69};
    for (int i = 0; i < 5; i++)
        note(S, {0.3f, 1.2f, midi(m[i]), 0, Sine, 0.13f, (i - 2.f) * 0.35f, 0.8f, 0.3f, 0.8f, 1.0f, 0.004f, 0, 0, 12.f + 5 * i});
    for (int i = 0; i < 6; i++)
        note(S, {0.9f + i * 0.14f, 0.05f, midi(86 + (i * 5) % 12), 0, Sine, 0.07f, std::sin(i * 2.f) * 0.8f, 0.01f, 0.1f, 0.2f, 0.7f});
    reverb(S, 0.45f, 0.88f, 0.3f);
    return finish(S);
}
std::vector<float> wiiu() {         // light bell motif: four rising notes, last one rings
    Song S(2.3f); Rng r;
    click(S, r, 0.0f, 0.3f);
    whoosh(S, r, 0.05f, 0.4f, 0.25f, 400, 3000);
    const float m[5] = {76, 79, 83, 81, 88};
    const float t[5] = {0.25f, 0.43f, 0.61f, 0.82f, 1.05f};
    for (int i = 0; i < 5; i++)
        note(S, {t[i], 0.04f, midi(m[i]), 0, Sine, i == 4 ? 0.24f : 0.18f, (i - 2.f) * 0.2f, 0.002f, 0.2f, 0.15f, i == 4 ? 1.0f : 0.5f, 0, 0, 0, 0, 0.5f});
    reverb(S, 0.3f, 0.8f, 0.3f);
    return finish(S);
}

std::vector<float> nes() {          // spring-loaded two-step clack (in, then down), a quick 8-bit square arpeggio that lands on a held chord
    Song S(1.5f); Rng r;
    thunk(S, r, 0.00f, 0.55f, 120);
    click(S, r, 0.14f, 0.35f);
    thunk(S, r, 0.22f, 0.8f, 75);
    const float m[6] = {60, 64, 67, 72, 76, 79};
    for (int i = 0; i < 6; i++)
        note(S, {0.42f + i * 0.07f, 0.05f, midi(m[i]), 0, Square, 0.18f, (i % 2 ? 0.25f : -0.25f), 0.002f, 0.03f, 0.6f, 0.06f});
    note(S, {0.88f, 0.28f, midi(72), 0, Square, 0.13f, -0.2f, 0.003f, 0.05f, 0.6f, 0.35f});
    note(S, {0.88f, 0.28f, midi(76), 0, Square, 0.11f, 0.2f, 0.003f, 0.05f, 0.6f, 0.35f});
    note(S, {0.88f, 0.28f, midi(55), 0, Tri, 0.2f, 0, 0.003f, 0.05f, 0.7f, 0.35f});
    echo(S, 0.12f, 0.35f, 0.4f);
    return finish(S);
}
std::vector<float> snes() {         // soft cartridge thunk, a warm rising glide, then a shimmering chime chord with a long bright tail
    Song S(2.4f); Rng r;
    thunk(S, r, 0.00f, 0.7f, 95);
    click(S, r, 0.06f, 0.2f);
    note(S, {0.16f, 0.7f, midi(48), midi(60), Sine, 0.16f, 0, 0.2f, 0.2f, 0.7f, 0.5f});
    const float m[4] = {72, 79, 84, 91};
    for (int i = 0; i < 4; i++) {
        note(S, {0.55f + i * 0.13f, 0.06f, midi(m[i]), 0, Sine, 0.2f, (i - 1.5f) * 0.4f, 0.004f, 0.25f, 0.25f, 1.1f, 0, 0, 0, 6, 0.4f});
        note(S, {0.55f + i * 0.13f, 0.06f, midi(m[i] - 12), 0, Tri, 0.08f, 0, 0.004f, 0.25f, 0.25f, 0.9f, 0, 0, 1800});
    }
    note(S, {1.1f, 0.6f, midi(60), 0, Tri, 0.12f, -0.2f, 0.1f, 0.3f, 0.6f, 0.9f, 0.004f, 0, 1400});
    note(S, {1.1f, 0.6f, midi(67), 0, Tri, 0.1f, 0.2f, 0.1f, 0.3f, 0.6f, 0.9f, 0.004f, 0, 1400});
    reverb(S, 0.4f, 0.82f, 0.3f);
    return finish(S);
}

} // namespace

std::vector<float> render(hub::System s) {
    switch (s) {
    case hub::System::N64:  return n64();
    case hub::System::GB:   return gb(false);
    case hub::System::GBC:  return gb(true);
    case hub::System::GBA:  return gba();
    case hub::System::NDS:  return nds();
    case hub::System::N3DS: return n3ds();
    case hub::System::GC:   return gc();
    case hub::System::PS1:  return ps1();
    case hub::System::PS2:  return ps2();
    case hub::System::WiiU: return wiiu();
    case hub::System::NES:  return nes();
    case hub::System::SNES: return snes();
    default: return {};
    }
}

bool write_wav(const char* path, const std::vector<float>& d) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    uint32_t bytes = uint32_t(d.size() * 2), rate = kSampleRate, br = rate * 4, riff = 36 + bytes, fmt = 16;
    uint16_t pcm = 1, ch = 2, ba = 4, bits = 16;
    std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
    std::fwrite(&fmt, 4, 1, f); std::fwrite(&pcm, 2, 1, f); std::fwrite(&ch, 2, 1, f);
    std::fwrite(&rate, 4, 1, f); std::fwrite(&br, 4, 1, f); std::fwrite(&ba, 2, 1, f); std::fwrite(&bits, 2, 1, f);
    std::fwrite("data", 1, 4, f); std::fwrite(&bytes, 4, 1, f);
    for (float v : d) { int16_t q = int16_t(std::max(-1.f, std::min(1.f, v)) * 32767); std::fwrite(&q, 2, 1, f); }
    std::fclose(f);
    return true;
}

} // namespace bootsynth
