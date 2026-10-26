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
    float fm = 0, fmdecay = 5, fmratio = 2;   // 2-op FM: index (decays), decay rate, modulator ratio (SNES-ish pluck/bell)
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
        float v = n.fm > 0 ? std::sin(2 * kPi * (ph + n.fm * std::exp(-t * n.fmdecay) * std::sin(2 * kPi * ph * n.fmratio))) : osc(n.w, ph);
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

/* Mix-down: cosine fade over the last `tail` seconds (ends at exactly 0), loudness normalisation to a fixed RMS
 * (~ -19 dBFS, roughly -16 LUFS for this kind of material), then a soft-knee limiter so peaks stay below 0.95. */
std::vector<float> finish(Song& S, float tail = 0.8f) {
    size_t n = S.n(), nf = std::min(n, size_t(tail * kSampleRate));
    for (size_t i = 0; i < n; i++) {
        if (i + nf >= n) {
            float u = float(n - 1 - i) / float(nf);                 // 1 -> 0
            float g = 0.5f - 0.5f * std::cos(kPi * std::min(1.f, u));
            S.L[i] *= g; S.R[i] *= g;
        }
    }
    double sq = 0;
    for (size_t i = 0; i < n; i++) sq += double(S.L[i]) * S.L[i] + double(S.R[i]) * S.R[i];
    float rms = float(std::sqrt(sq / (2.0 * n)) + 1e-9);
    float g = 0.11f / rms;
    auto lim = [](float x) {
        float a = std::fabs(x);
        if (a <= 0.55f) return x;
        float y = 0.55f + 0.38f * std::tanh((a - 0.55f) / 0.38f);
        return x < 0 ? -y : y;
    };
    std::vector<float> out(n * 2);
    for (size_t i = 0; i < n; i++) { out[2 * i] = lim(S.L[i] * g); out[2 * i + 1] = lim(S.R[i] * g); }
    return out;
}

/* ---------------- building blocks for the longer pieces ---------------- */

void pad(Song& S, float t0, float dur, std::initializer_list<float> ms, float amp, Wave w, float lp, float att, float rel, float det = 9) {
    int n = int(ms.size()), i = 0;
    for (float m : ms) {
        float pan = n > 1 ? (i - (n - 1) * 0.5f) / (n - 1) * 1.2f : 0.f;
        Note x{t0, dur, midi(m), 0, w, amp, pan, att, 0.3f, 0.85f, rel, 0.002f, 0, lp, det + 3.f * i};
        note(S, x); i++;
    }
}
void bell(Song& S, float t, float m, float amp, float pan, float ring, float det = 5) {
    note(S, {t, 0.05f, midi(m), 0, Sine, amp, pan, 0.003f, 0.3f, 0.2f, ring, 0, 0, 0, det, 0.4f});
}
void pluck(Song& S, float t, float m, float amp, float pan, float idx = 3, float ratio = 2, float rel = 0.5f) {   // FM, sample-like
    Note x{t, 0.04f, midi(m), 0, Sine, amp, pan, 0.002f, 0.1f, 0.3f, rel};
    x.fm = idx; x.fmdecay = 7; x.fmratio = ratio;
    note(S, x);
}
void chip(Song& S, float t, float dur, float m, float amp, float pan, Wave w = Square) {
    note(S, {t, dur, midi(m), 0, w, amp, pan, 0.002f, 0.03f, 0.7f, 0.04f});
}
void hat(Song& S, Rng& r, float t, float amp) { noise(S, r, t, 0.05f, amp, 9000, 3000, 70); }
void snare(Song& S, Rng& r, float t, float amp) { noise(S, r, t, 0.12f, amp, 7000, 500, 30); }
void kick(Song& S, float t, float amp) { note(S, {t, 0.08f, 150, 45, Sine, amp, 0, 0.001f, 0.05f, 0.5f, 0.1f}); }
/* Plays `m[i]` (0 = rest) every `step` seconds as short chiptune notes. */
void line(Song& S, float t0, float step, const std::vector<int>& m, float off, float amp, float pan, Wave w = Square) {
    for (size_t i = 0; i < m.size(); i++)
        if (m[i]) chip(S, t0 + i * step, step * 0.85f, m[i] + off, amp, pan, w);
}

/* ---------------- the sounds (all original) ---------------- */

std::vector<float> n64() {          // 6 s: cartridge clunk, power hum blooms, rising motif, bell development, open D add9 resolution
    Song S(6.0f); Rng r;
    thunk(S, r, 0.00f, 0.9f, 80);
    click(S, r, 0.07f, 0.25f);
    note(S, {0.15f, 4.4f, midi(38), 0, Tri, 0.22f, 0, 0.6f, 0.2f, 0.8f, 1.2f, 0, 0, 260});
    note(S, {0.15f, 4.4f, midi(50), 0, Sine, 0.12f, 0, 0.8f, 0.2f, 0.7f, 1.2f});
    const float a[4] = {62, 66, 69, 74}, ta[4] = {0.5f, 0.9f, 1.3f, 1.7f};
    for (int i = 0; i < 4; i++) note(S, {ta[i], 0.25f, midi(a[i]), 0, Tri, 0.14f, (i - 1.5f) * 0.2f, 0.02f, 0.1f, 0.5f, 0.6f, 0.004f, 0, 2200});
    pad(S, 1.9f, 1.8f, {47, 59, 66, 69, 74}, 0.05f, Saw, 900, 0.5f, 0.8f);
    const float b[5] = {76, 81, 78, 71, 83}, tb[5] = {2.4f, 2.75f, 3.1f, 3.45f, 3.8f};
    for (int i = 0; i < 5; i++) bell(S, tb[i], b[i], 0.13f, (i - 2.f) * 0.3f, 0.9f);
    pad(S, 3.8f, 1.2f, {50, 62, 66, 69, 76}, 0.06f, Saw, 1100, 0.3f, 1.4f);
    bell(S, 3.9f, 86, 0.16f, 0.1f, 1.8f); bell(S, 4.0f, 81, 0.1f, -0.2f, 1.8f);
    reverb(S, 0.3f, 0.78f, 0.4f);
    return finish(S, 1.2f);
}
std::vector<float> gb(bool color) { // 5 s (Color 5.7 s): click + pings, bouncy square tune in three phrases, bass and hats, held chord
    float step = color ? 0.13f : 0.14f, off = color ? 2.f : 0.f;
    Song S(color ? 5.7f : 5.0f); Rng r;
    click(S, r, 0.0f, 0.5f);
    chip(S, 0.12f, 0.1f, 79 + off + (color ? 7 : 0), 0.18f, 0);
    chip(S, 0.30f, 0.12f, 86 + off + (color ? 7 : 0), 0.18f, 0);
    std::vector<int> mel = {72, 76, 79, 76, 74, 77, 81, 77,   72, 76, 79, 84, 83, 79, 76, 79,   77, 76, 74, 72, 74, 76, 79, 0};
    if (color) {
        const int more[8] = {84, 83, 79, 83, 86, 84, 79, 76};
        mel.insert(mel.end(), more, more + 8);
    }
    float t0 = 0.6f;
    line(S, t0, step, mel, off, 0.17f, -0.2f);
    std::vector<int> harm(mel.size(), 0);                          // echo-ish second voice in the second phrase onward
    for (size_t i = 8; i < mel.size(); i++) if (mel[i] && i % 2 == 0) harm[i] = mel[i] - 12 + (color ? 4 : 0);
    line(S, t0, step, harm, off, 0.09f, 0.3f, Tri);
    const int bass[8] = {48, 53, 55, 48, 48, 53, 55, 55};
    size_t groups = mel.size() / 4;
    for (size_t g = 0; g < groups; g++) {
        int bnote = bass[g % 8];
        for (int k = 0; k < 4; k += 2) chip(S, t0 + (g * 4 + k) * step, step * 1.7f, bnote + off - 12, 0.2f, 0, Tri);
        hat(S, r, t0 + (g * 4 + 1) * step, 0.12f); hat(S, r, t0 + (g * 4 + 3) * step, 0.12f);
    }
    float te = t0 + mel.size() * step;
    for (int m : {72, 76, 79, 84}) note(S, {te, 0.45f, midi(m + off), 0, Square, 0.09f, (m - 78) * 0.03f, 0.003f, 0.05f, 0.7f, 0.55f});
    chip(S, te, 0.45f, 48 + off, 0.2f, 0, Tri);
    echo(S, 0.16f, 0.5f, 0.55f);
    return finish(S, 0.9f);
}
std::vector<float> gba() {          // 6 s: click, shimmering detuned sweep, glassy arpeggios over a drifting pad progression, bell resolution
    Song S(6.0f); Rng r;
    click(S, r, 0.0f, 0.5f);
    for (int i = 0; i < 4; i++) {
        float pan = (i - 1.5f) * 0.4f, f = 300 + i * 70;
        note(S, {0.12f + i * 0.04f, 0.9f, f, f * 5.5f, Sine, 0.10f, pan, 0.2f, 0.2f, 0.7f, 0.4f, 0, 0.5f, 0, 14.f * (i + 1)});
    }
    const float ch[4][4] = {{50, 57, 60, 64}, {46, 53, 57, 62}, {43, 50, 55, 58}, {45, 52, 57, 60}};   // Dm9-ish, Bbmaj7, Gm7, A7sus-ish
    for (int c = 0; c < 4; c++) {
        float t = 1.1f + c, bp = c == 3 ? 0.7f : 1.f;
        pad(S, t, 1.0f * bp, {ch[c][0], ch[c][1], ch[c][2], ch[c][3]}, 0.06f, Saw, 1300, 0.15f, 0.5f, 11);
        for (int k = 0; k < 8; k++) {
            int idx = (k * 3) % 4;
            bell(S, t + k * 0.125f, ch[c][idx] + 24 + (k >= 4 ? 12 : 0), 0.06f + 0.002f * k, std::sin(k * 1.3f) * 0.7f, 0.5f, 3);
        }
    }
    pad(S, 4.2f, 0.9f, {50, 57, 62, 66, 69}, 0.07f, Saw, 1600, 0.1f, 1.2f, 12);
    bell(S, 4.2f, 88, 0.22f, -0.2f, 1.6f, 0); bell(S, 4.2f, 88, 0.12f, 0.3f, 1.6f, 8); bell(S, 4.35f, 74, 0.12f, 0.1f, 1.5f);
    reverb(S, 0.35f, 0.78f, 0.3f);
    return finish(S, 1.2f);
}
std::vector<float> nds() {          // 6 s: card click, marimba-like motif stated and answered, soft pad, round chime resolution
    Song S(6.0f); Rng r;
    click(S, r, 0.0f, 0.45f);
    const float m1[6] = {72, 76, 79, 84, 79, 76};
    for (int i = 0; i < 6; i++) pluck(S, 0.2f + i * 0.17f, m1[i], 0.2f, (i - 2.5f) * 0.25f, 1.6f, 4.f, 0.5f);
    pad(S, 0.8f, 1.5f, {48, 55, 60, 64}, 0.05f, Tri, 1400, 0.4f, 0.6f);
    const float m2[7] = {74, 77, 81, 86, 84, 81, 77}, t2[7] = {2.0f, 2.17f, 2.34f, 2.58f, 2.82f, 3.0f, 3.2f};
    for (int i = 0; i < 7; i++) pluck(S, t2[i], m2[i], 0.2f, (i - 3.f) * 0.2f, 1.6f, 4.f, 0.5f);
    pad(S, 2.0f, 1.8f, {50, 57, 62, 65}, 0.05f, Tri, 1400, 0.3f, 0.6f);
    pad(S, 3.8f, 1.2f, {48, 55, 64, 67, 72}, 0.06f, Tri, 1800, 0.25f, 1.3f);
    const float m3[4] = {79, 84, 88, 91};
    for (int i = 0; i < 4; i++) bell(S, 3.8f + i * 0.09f, m3[i], 0.18f, (i - 1.5f) * 0.35f, 1.3f, 3);
    reverb(S, 0.3f, 0.78f, 0.35f);
    return finish(S, 1.1f);
}
std::vector<float> n3ds() {         // 6.5 s: shutter-ish clicks, glass sparkles climbing a lydian scale, airy pad and breath, high shimmer finish
    Song S(6.5f); Rng r;
    click(S, r, 0.0f, 0.3f); click(S, r, 0.09f, 0.18f);
    whoosh(S, r, 0.2f, 1.6f, 0.12f, 500, 5000);
    const int sc[8] = {0, 2, 4, 6, 7, 9, 11, 12};                 // lydian
    for (int i = 0; i < 16; i++) {
        int deg = (i * 3 + (i / 4)) % 8;
        float m = 74 + sc[deg] + (i % 5 == 4 ? 12 : 0);
        bell(S, 0.3f + i * 0.17f, m, 0.10f + (i > 8 ? 0.02f : 0), std::sin(i * 2.1f) * 0.8f, 0.7f, 6);
    }
    pad(S, 0.5f, 2.6f, {53, 60, 64, 69, 72}, 0.045f, Sine, 0, 0.9f, 1.0f, 14);
    pad(S, 3.0f, 2.0f, {55, 62, 66, 71, 74}, 0.05f, Sine, 0, 0.6f, 1.5f, 14);
    const float f[5] = {86, 91, 90, 95, 98}, tf[5] = {3.6f, 3.85f, 4.1f, 4.35f, 4.6f};
    for (int i = 0; i < 5; i++) bell(S, tf[i], f[i], 0.12f, (i - 2.f) * 0.35f, 1.5f, 4);
    note(S, {4.5f, 0.5f, midi(79), midi(91), Sine, 0.08f, 0, 0.1f, 0.2f, 0.6f, 1.0f, 0.003f});
    reverb(S, 0.38f, 0.82f, 0.3f);
    return finish(S, 1.3f);
}
std::vector<float> gc() {           // 8 s: lid thunk, disc spin-up, bubbly pentatonic arpeggios over three chords with bass pulses, pop and long bloom
    Song S(8.0f); Rng r;
    thunk(S, r, 0.0f, 0.7f, 100);
    whoosh(S, r, 0.1f, 1.1f, 0.5f, 300, 2800);
    const float pent[3][5] = {{62, 66, 69, 74, 78}, {59, 62, 66, 71, 74}, {55, 59, 62, 67, 71}};    // D, Bm, G pentatonic sets
    const float root[3] = {38, 35, 31};
    for (int c = 0; c < 3; c++) {
        float t0 = 1.2f + c * 1.6f;
        for (int i = 0; i < 16; i++) {
            float m = pent[c][(i * 2) % 5] + ((i / 5) % 2 ? 12 : 0) + (c == 2 ? 12 * (i > 11) : 0);
            float t = t0 + i * 0.1f;
            note(S, {t, 0.04f, midi(m), 0, Tri, 0.17f, std::sin(i * 1.7f) * 0.6f, 0.003f, 0.08f, 0.2f, 0.25f, 0, 0, 3500});
            note(S, {t, 0.02f, midi(m + 12), 0, Sine, 0.07f, 0, 0.002f, 0.05f, 0.1f, 0.12f});
        }
        for (int k = 0; k < 4; k++) note(S, {t0 + k * 0.4f, 0.12f, midi(root[c] + 12), midi(root[c]), Sine, 0.3f, 0, 0.004f, 0.08f, 0.5f, 0.2f});
        pad(S, t0, 1.5f, {root[c] + 24, root[c] + 31, root[c] + 36}, 0.035f, Saw, 1200, 0.4f, 0.4f);
    }
    note(S, {6.0f, 0.5f, midi(62), midi(74), Sine, 0.2f, 0, 0.01f, 0.2f, 0.5f, 0.8f});
    pad(S, 6.0f, 0.7f, {50, 62, 66, 69, 74, 78}, 0.07f, Tri, 2500, 0.02f, 1.6f);
    pluck(S, 6.0f, 86, 0.2f, 0.2f, 4.f, 3.f, 1.3f);
    reverb(S, 0.28f, 0.75f, 0.4f);
    return finish(S, 1.5f);
}
std::vector<float> ps1() {          // 9.5 s: tray close, disc spin, then a slow ambient swell: sub drone, saw pads opening, sparse shimmer, slow fade
    Song S(9.5f); Rng r;
    thunk(S, r, 0.0f, 0.5f, 70);
    whoosh(S, r, 0.1f, 1.2f, 0.35f, 200, 1500);
    note(S, {0.3f, 6.5f, midi(26), 0, Sine, 0.25f, 0, 2.0f, 0.5f, 0.9f, 2.5f});
    note(S, {0.6f, 6.0f, midi(38), 0, Tri, 0.12f, 0, 2.2f, 0.5f, 0.85f, 2.5f, 0, 0, 350});
    pad(S, 0.5f, 3.9f, {38, 45, 50, 57}, 0.10f, Saw, 450, 1.6f, 1.4f, 7);
    pad(S, 1.8f, 2.6f, {62, 65, 69}, 0.05f, Saw, 800, 1.5f, 1.4f, 9);
    pad(S, 4.2f, 2.7f, {34, 46, 50, 53, 57}, 0.09f, Saw, 750, 1.0f, 1.4f, 7);
    pad(S, 6.4f, 2.3f, {38, 45, 50, 57, 60}, 0.09f, Saw, 1100, 1.0f, 2.2f, 8);
    const float sh[6] = {86, 93, 89, 96, 91, 98}, tsh[6] = {3.0f, 4.3f, 5.2f, 6.2f, 6.9f, 7.6f};
    for (int i = 0; i < 6; i++) note(S, {tsh[i], 0.6f, midi(sh[i]), 0, Sine, 0.045f, (i % 2 ? 0.5f : -0.5f), 0.5f, 0.2f, 0.6f, 1.6f, 0.01f, 0.3f});
    note(S, {4.0f, 0.2f, midi(33), 0, Sine, 0.25f, 0, 0.005f, 0.3f, 0.2f, 1.2f});   // soft sub hit marks the second chord
    reverb(S, 0.38f, 0.87f, 0.4f);
    return finish(S, 2.8f);
}
std::vector<float> ps2() {          // 9.5 s: airy evolving pad over three chords, breathy noise sweeps, a sub, sparkles cascading and thinning out
    Song S(9.5f); Rng r;
    thunk(S, r, 0.0f, 0.35f, 110);
    whoosh(S, r, 0.1f, 2.4f, 0.22f, 400, 6000);
    whoosh(S, r, 4.2f, 2.4f, 0.12f, 800, 7000);
    pad(S, 0.4f, 3.2f, {50, 57, 62, 66, 69, 73}, 0.11f, Sine, 0, 1.4f, 1.0f, 12);
    pad(S, 3.4f, 3.1f, {47, 54, 59, 62, 66, 71}, 0.11f, Sine, 0, 0.9f, 1.0f, 12);
    pad(S, 6.3f, 2.4f, {43, 55, 62, 66, 69, 74}, 0.11f, Sine, 0, 0.9f, 2.0f, 12);
    pad(S, 0.8f, 7.5f, {74, 78, 81, 85}, 0.015f, Saw, 3000, 3.0f, 2.0f, 6);
    note(S, {0.5f, 6.0f, midi(38), 0, Sine, 0.2f, 0, 1.5f, 0.3f, 0.9f, 2.0f});
    const int sc[7] = {0, 2, 4, 7, 9, 12, 14};
    for (int i = 0; i < 18; i++) {
        float t = 1.6f + i * 0.33f - (i > 8 ? (i - 8) * 0.07f : 0.f);
        float amp = 0.05f * (i < 12 ? 1.f : 1.f - (i - 12) * 0.13f);
        bell(S, t, 79 + sc[(i * 4) % 7] + (i % 3) * 5 - (i % 3 == 2 ? 12 : 0), amp, std::sin(i * 2.3f) * 0.85f, 1.1f, 7);
    }
    reverb(S, 0.5f, 0.89f, 0.3f);
    return finish(S, 2.6f);
}
std::vector<float> wiiu() {         // 7 s: click and breath, bell motif, harmonised marimba reply over a soft pad, ringing bell chord
    Song S(7.0f); Rng r;
    click(S, r, 0.0f, 0.3f);
    whoosh(S, r, 0.05f, 0.5f, 0.22f, 400, 3000);
    const float m[5] = {76, 79, 83, 81, 88}, t[5] = {0.3f, 0.5f, 0.7f, 0.95f, 1.2f};
    for (int i = 0; i < 5; i++) bell(S, t[i], m[i], i == 4 ? 0.22f : 0.17f, (i - 2.f) * 0.2f, i == 4 ? 1.4f : 0.7f, 4);
    pad(S, 0.9f, 2.0f, {52, 59, 64, 67}, 0.05f, Tri, 1500, 0.5f, 0.8f);
    const float h[8] = {79, 83, 86, 84, 83, 88, 86, 91}, th[8] = {2.2f, 2.4f, 2.6f, 2.85f, 3.2f, 3.4f, 3.6f, 3.85f};
    for (int i = 0; i < 8; i++) {
        pluck(S, th[i], h[i], 0.17f, (i - 3.5f) * 0.2f, 1.5f, 4.f, 0.6f);
        if (i % 2 == 0) pluck(S, th[i], h[i] - 9, 0.08f, 0, 1.5f, 4.f, 0.6f);
    }
    pad(S, 2.2f, 2.0f, {55, 62, 67, 71}, 0.05f, Tri, 1500, 0.4f, 0.8f);
    for (int k = 0; k < 8; k++) hat(S, r, 2.2f + k * 0.2f, 0.05f);
    pad(S, 4.3f, 1.4f, {52, 59, 64, 71, 76}, 0.07f, Tri, 2200, 0.1f, 1.8f);
    bell(S, 4.3f, 88, 0.2f, -0.2f, 2.4f); bell(S, 4.35f, 83, 0.12f, 0.3f, 2.4f); bell(S, 4.4f, 95, 0.08f, 0.1f, 2.0f);
    reverb(S, 0.32f, 0.8f, 0.3f);
    return finish(S, 1.4f);
}
std::vector<float> nes() {          // 5 s: spring-loaded two-step clack, square arpeggios over C-G-Am, a lead melody with bass and drums, held chord
    Song S(5.0f); Rng r;
    thunk(S, r, 0.00f, 0.55f, 120);
    click(S, r, 0.14f, 0.35f);
    thunk(S, r, 0.22f, 0.8f, 75);
    const int ch[3][4] = {{60, 64, 67, 72}, {59, 62, 67, 71}, {57, 60, 64, 69}};
    const int bs[3] = {36, 43, 45};
    for (int c = 0; c < 3; c++)
        for (int k = 0; k < 8; k++) {
            float t = 0.45f + (c * 8 + k) * 0.07f;
            chip(S, t, 0.06f, ch[c][k % 4] + (k >= 4 ? 0 : 0), 0.15f, (k % 2 ? 0.25f : -0.25f));
            if (k % 4 == 0) chip(S, t, 0.2f, bs[c], 0.22f, 0, Tri);
        }
    const std::vector<int> lead = {76, 79, 81, 79, 77, 76, 74, 72};
    line(S, 2.2f, 0.2f, lead, 0, 0.14f, -0.15f);
    const int bass[8] = {36, 36, 41, 41, 43, 43, 36, 36};
    for (int i = 0; i < 8; i++) {
        chip(S, 2.2f + i * 0.2f, 0.18f, bass[i], 0.22f, 0, Tri);
        if (i % 2 == 0) { kick(S, 2.2f + i * 0.2f, 0.4f); } else snare(S, r, 2.2f + i * 0.2f, 0.25f);
        hat(S, r, 2.3f + i * 0.2f, 0.1f);
    }
    for (int m : {72, 76, 79}) note(S, {3.85f, 0.45f, midi(m), 0, Square, 0.1f, (m - 76) * 0.04f, 0.003f, 0.05f, 0.7f, 0.65f});
    chip(S, 3.85f, 0.45f, 36, 0.25f, 0, Tri);
    echo(S, 0.12f, 0.35f, 0.4f);
    return finish(S, 0.9f);
}
std::vector<float> snes() {         // 7.2 s: soft thunk, warm glide, FM pluck arpeggios over warm string-ish pads (maj7 / m9 / maj7), echo, bright final chord
    Song S(7.2f); Rng r;
    thunk(S, r, 0.00f, 0.7f, 95);
    click(S, r, 0.06f, 0.2f);
    note(S, {0.16f, 0.5f, midi(48), midi(60), Sine, 0.16f, 0, 0.2f, 0.2f, 0.7f, 0.4f});
    const int ch[3][5] = {{48, 55, 59, 64, 67}, {45, 52, 59, 64, 67}, {41, 53, 60, 64, 69}};   // Cmaj9, Am9, Fmaj9
    for (int c = 0; c < 3; c++) {
        float t0 = 0.6f + c * 1.5f;
        pad(S, t0, 1.45f, {float(ch[c][0]), float(ch[c][1]), float(ch[c][2]), float(ch[c][3])}, 0.055f, Saw, 1000, 0.35f, 0.5f, 10);
        for (int k = 0; k < 8; k++) pluck(S, t0 + k * 0.1875f, ch[c][(k * 2) % 5] + 24 - (k % 4 == 3 ? 12 : 0), 0.13f, (k - 3.5f) * 0.2f, 2.5f, 3.f, 0.7f);
        note(S, {t0, 0.5f, midi(ch[c][0] - 12), 0, Sine, 0.2f, 0, 0.01f, 0.1f, 0.8f, 0.3f});
    }
    const float ld[6] = {88, 86, 84, 81, 84, 91}, tl[6] = {3.6f, 3.95f, 4.2f, 4.55f, 4.8f, 5.1f};
    for (int i = 0; i < 6; i++) pluck(S, tl[i], ld[i], 0.16f, 0.3f, 3.5f, 1.f, 0.9f);
    pad(S, 5.1f, 0.9f, {48, 55, 59, 64, 71}, 0.07f, Saw, 1500, 0.08f, 1.4f, 10);
    for (int i = 0; i < 5; i++) pluck(S, 5.1f + i * 0.09f, 72 + (i * 7) % 24 + 12 * (i > 2), 0.17f, (i - 2) * 0.35f, 3.f, 3.f, 1.3f);
    echo(S, 0.19f, 0.45f, 0.5f);
    reverb(S, 0.3f, 0.82f, 0.3f);
    return finish(S, 1.4f);
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
