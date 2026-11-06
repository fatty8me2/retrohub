#pragma once
/* Cases view, stage side (car-stage): the pure parts. No ImGui, unit-tested in test_cases.cpp.
 *  - ambient colour: the focused cover's dominant colour turned into the stage tint pair;
 *  - flip / inspect clearance: how far the neighbours must stand out so nothing intersects the enlarged centre case;
 *  - small helpers for the motion (idle float, settle) and the text under the centre case. */

#include "cases_logic.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace hubui::cases {

/* ---- ambient colour from the cover art ---- */
struct Rgb8 { int r = 0, g = 0, b = 0; };
inline void rgb_to_hsv(Rgb8 c, float* h, float* s, float* v) {
    const float r = (float)c.r / 255.f, g = (float)c.g / 255.f, b = (float)c.b / 255.f;
    const float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b)), d = mx - mn;
    *v = mx;
    *s = mx > 0.f ? d / mx : 0.f;
    float hh = 0.f;
    if (d > 1e-6f) {
        if (mx == r) hh = std::fmod((g - b) / d, 6.f);
        else if (mx == g) hh = (b - r) / d + 2.f;
        else hh = (r - g) / d + 4.f;
        hh /= 6.f;
        if (hh < 0.f) hh += 1.f;
    }
    *h = hh;
}
inline Rgb8 hsv_to_rgb(float h, float s, float v) {
    h = h - std::floor(h);
    const float k = h * 6.f;
    const int i = (int)k;
    const float f = k - (float)i, p = v * (1.f - s), q = v * (1.f - f * s), t = v * (1.f - (1.f - f) * s);
    float r = v, g = t, b = p;
    switch (i % 6) {
        case 0: r = v; g = t; b = p; break; case 1: r = q; g = v; b = p; break; case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break; case 4: r = t; g = p; b = v; break; default: r = v; g = p; b = q; break;
    }
    auto q8 = [](float x) { return std::max(0, std::min(255, (int)(x * 255.f + 0.5f))); };
    return {q8(r), q8(g), q8(b)};
}
/* The stage tint pair (c0 = deeper, c1 = brighter) for a cover whose dominant colour is `art`: the hue is kept, saturation and
 * brightness are clamped to a range that always makes a tasteful, never garish, wall (a grey / black cover returns false: use the console colours). */
inline bool ambient_from_art(Rgb8 art, Rgb8* c0, Rgb8* c1) {
    float h, s, v;
    rgb_to_hsv(art, &h, &s, &v);
    if (s < 0.14f || v < 0.10f) return false;
    const float ss = std::max(0.42f, std::min(0.82f, s));
    *c0 = hsv_to_rgb(h, ss, 0.58f);
    *c1 = hsv_to_rgb(h + 0.025f, std::max(0.30f, ss * 0.85f), 0.92f);
    return true;
}

/* ---- flip / inspect: nothing may intersect the centre case ---- */
/* What the flip and inspect presentation does to the centre case on top of its rest pose (so the view and the tests agree). */
struct CentreAdj { float z = 0, scale_mul = 1; };
inline CentreAdj centre_adj(float flip, float insp, float settle = 0.f) {
    const float pi = 3.14159265f;
    const float f = std::max(0.f, std::min(flip, pi)), sf = std::sin(f);
    const float ins = std::max(0.f, std::min(1.f, insp));
    CentreAdj a;
    a.z = -(0.5f * sf) - 0.4f * smoothstep01(f / pi) - 0.6f * ins;
    a.scale_mul = (1.f + 0.05f * sf + 0.10f * smoothstep01(f / pi)) * (1.f + 0.05f * ins) * (1.f + 0.03f * std::max(-1.5f, std::min(1.5f, settle)));   // settle: the bounce of the case coming to rest
    return a;
}
/* Half the screen width (in world units at the centre plane, i.e. screen px / U) the centre case may cover, whatever its yaw: it is turned
 * freely while inspected, so the bound is the half diagonal of its footprint seen from its nearest corner. x = its world offset. */
inline float centre_right_edge(const CaseDims& c, float scale, float x, float z, float pitch_abs, float dist) {
    const float half = 0.5f * std::hypot(c.w, c.d) * scale;
    const float zn = z - half - scale * 0.5f * c.h * std::sin(pitch_abs);   // the nearest a corner can come, whatever the yaw
    return (x + half) * dist / std::max(0.4f, dist + zn);
}
/* The least world x of a neighbour on the RIGHT (yaw/scale/z of its resting pose) so its left-most corner stands `gap` right of `target`
 * (screen units / U). The left neighbour is the mirror image (negate the result, pass the positive yaw). yaw > 0 = turned away like a right neighbour. */
inline float neighbour_min_x(const CaseDims& n, float yaw, float scale, float z, float dist, float target) {
    const float ct = std::cos(yaw), st = std::sin(yaw);
    float need = -1e9f;
    for (int i = 0; i < 4; ++i) {
        const float lx = (i & 1 ? 0.5f : -0.5f) * n.w * scale, lz = (i & 2 ? 0.5f : -0.5f) * n.d * scale;
        const float xr = lx * ct + lz * st, zr = -lx * st + lz * ct;
        const float k = dist / std::max(0.4f, dist + z + zr);
        need = std::max(need, target / k - xr);
    }
    return need;
}
/* How far (world units, >= 0) the first neighbour on one side must move outward from `base_x` (its resting x, positive) so it clears the centre case. */
inline float flip_push(const CaseDims& c, float c_scale, float c_x, float c_z, float pitch_abs, const CaseDims& n, float n_yaw, float n_scale, float n_z,
                       float base_x, float dist, float gap = 0.045f) {
    const float edge = centre_right_edge(c, c_scale, c_x, c_z, pitch_abs, dist) + gap;
    return std::max(0.f, neighbour_min_x(n, n_yaw, n_scale, n_z, dist, edge) - base_x);
}

/* ---- motion ---- */
/* Idle float of the centre case: a slow bob (world units) and sway (radians). */
/* t is a double (the clock runs for days: a float stops resolving frames after ~12 days). */
inline float idle_bob(double t) { return (float)(0.011 * std::sin(t * 1.45) + 0.004 * std::sin(t * 2.7 + 1.1)); }
inline float idle_sway(double t) { return (float)(0.014 * std::sin(t * 0.83 + 0.6)); }
/* How far a slot at distance |d| from the centre moves outward: the nearest neighbours' extra room plus the flip / inspect push, fading in over the first slot.
 * Every slot from the first neighbour outward gets the same shift, so the spacing between the neighbours (the shelf pitch) never changes while a case is flipped. */
inline float neighbour_shift(float ad, float extra, float push) { return (extra + push) * smoothstep01(std::min(ad, 1.f)); }

/* ---- the text under the centre case ---- */
/* "Switch 2" for Switch 2 games, else the console's name. */
inline std::string console_chip(const std::string& console, bool nx2) { return nx2 ? std::string("Switch 2") : console; }
/* "3 days ago" style playtime line pieces are built with continue_logic.h's last_played_line (view side). */
/* Colour for the chip text on a console colour: dark ink on a light chip, white on a dark one (luma in 0..1). */
inline bool chip_wants_dark_ink(float luma) { return luma > 0.62f; }

}  // namespace hubui::cases
