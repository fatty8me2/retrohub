#pragma once
/* Cases library view (a console-style 3D carousel of game cases): the pure parts. No ImGui here, unit-tested in test_cases.cpp.
 * Slot pose maths (where each case stands for its distance from the centre), the case shapes per console, stepping with wrap / clamp,
 * hold-to-accelerate, the console tab list and its per-console selection memory, the flip / inspect mode machine, and the shuffle pick.
 * The drawing is shelf_draw.cpp (cases section), the integration with the All Games state is libview_ui.cpp. */

#include "libview_logic.h"
#include "shelf_logic.h"
#include "surprise_logic.h"

#include "state_slots.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace hubui::cases {

constexpr int kWindow = 12;          // cases drawn on each side of the centre
constexpr float kFadeFrom = 8.6f;    // distance where the edge fade starts
constexpr float kFadeTo = 12.2f;     // distance where a case is fully faded out (never popping in or out)

/* ---- slot pose: where a case stands for its (signed, fractional) distance d from the centre ---- */
struct SlotPose {
    float x = 0;        // world offset from the centre, in world units (case height of a tall keep case = 1)
    float yaw = 0;      // turn about the vertical axis, radians: 0 = face-on, +ve = right of centre (front turned toward the centre)
    float z = 0;        // depth behind the centre case, world units (0 = the centre plane)
    float scale = 1;
    float alpha = 1;
};
inline float smoothstep01(float t) { t = std::max(0.f, std::min(1.f, t)); return t * t * (3.f - 2.f * t); }
inline SlotPose slot_pose(float d) {
    SlotPose p;
    const float a = std::fabs(d);
    const float sgn = d < 0.f ? -1.f : 1.f;
    // x: a Hermite curve to the first neighbour (0 -> 0.66 for a keep case; wider boxes add neighbour_x() on top, so it stays clear of the centre case's face, ends with the shelf pitch), then a tight pitch
    const float x1 = 0.66f, m0 = 1.0f, m1 = 0.16f, pitch = 0.16f;
    float x;
    if (a < 1.f) {
        const float t = a, t2 = t * t, t3 = t2 * t;
        x = (2 * t3 - 3 * t2 + 1) * 0.f + (t3 - 2 * t2 + t) * m0 + (-2 * t3 + 3 * t2) * x1 + (t3 - t2) * m1;
    } else x = x1 + (a - 1.f) * pitch;
    p.x = sgn * x;
    // yaw: nearly flat at the centre, ~41 degrees for the first neighbour, close to side-on (75-78 degrees) further out
    const float ymax = 1.37f, k = 0.75f;
    p.yaw = sgn * ymax * (1.f - std::exp(-k * a));
    p.z = 0.55f * std::min(a, 1.f) * (2.f - std::min(a, 1.f)) + 0.11f * std::max(0.f, a - 1.f);
    p.scale = 1.f - 0.18f * std::min(a, 1.f) - 0.015f * std::min(std::max(a - 1.f, 0.f), 6.f);   // the neighbour is about 75% of the centre
    p.alpha = a <= kFadeFrom ? 1.f : 1.f - smoothstep01((a - kFadeFrom) / (kFadeTo - kFadeFrom));
    return p;
}

/* ---- case shapes ---- */
enum class CaseKind { Keep, Small, Jewel, Box };
inline CaseKind kind_for(shelf::Media m) {
    using shelf::Media;
    switch (m) {
        case Media::NesCart: case Media::SnesCart: case Media::N64Cart: case Media::GbCart: case Media::GbcCart: case Media::GbaCart: return CaseKind::Box;
        case Media::DsCase: case Media::N3dsCase: case Media::PspCase: return CaseKind::Small;
        case Media::Ps1Jewel: return CaseKind::Jewel;
        default: return CaseKind::Keep;   // GC / Wii U / PS2 / Switch / generic: the tall DVD-style keep case
    }
}
inline bool is_box(shelf::Media m) { return kind_for(m) == CaseKind::Box; }
inline bool has_sheen(shelf::Media m) { return !is_box(m); }   // clear plastic is glossy, cardboard is matte
struct CaseDims { float w = 0.71f, h = 1.f, d = 0.075f; };   // world units; a 190 mm tall keep case is 1.0 high
inline CaseDims case_dims(shelf::Media m) {
    using shelf::Media;
    switch (m) {
        case Media::GcCase: return {0.71f, 1.f, 0.068f};        // GameCube keep case (about 135 x 191 x 13 mm)
        case Media::Ps2Case: return {0.71f, 1.f, 0.075f};       // DVD keep case (135 x 190 x 14)
        case Media::WiiUCase: return {0.67f, 0.90f, 0.058f};    // Blu-ray keep case (the cover scans measure 0.71 w/h)
        case Media::SwitchCase: return {0.553f, 0.895f, 0.058f};// Switch keep case, slim (105 x 170 x 11 mm)
        case Media::Ps1Jewel: return {0.86f, 0.757f, 0.058f};   // jewel case (142 x 125 mm, wider than tall; drawn a little large)
        case Media::DsCase: return {0.76f, 0.65f, 0.07f};       // DS case: the cover scans are landscape (1.12 w/h)
        case Media::N3dsCase: return {0.76f, 0.65f, 0.072f};
        case Media::PspCase: return {0.54f, 0.90f, 0.068f};     // UMD case: tall and narrow (104 x 171 x 14)
        case Media::NesCart: return {0.67f, 0.93f, 0.16f};      // cardboard boxes: thicker than the plastic cases
        case Media::SnesCart: return {0.97f, 0.70f, 0.18f};     // landscape (184 x 132 x 35)
        case Media::N64Cart: return {0.94f, 0.68f, 0.19f};      // landscape (178 x 128 x 38)
        case Media::GbCart: case Media::GbcCart: return {0.66f, 0.66f, 0.15f};   // small square boxes (the cover scans are 1:1)
        case Media::GbaCart: return {0.66f, 0.66f, 0.15f};
        default: return {0.71f, 1.f, 0.075f};
    }
}

/* ---- case faces: where the cover art goes (pure; shared by cases_draw.cpp and the tests) ---- */
struct UvRect { float u0 = 0, v0 = 0, u1 = 1, v1 = 1; };
/* Share of the face width taken by the hinge strip of a plastic case (the spine side of the front, and of the back). */
inline float hinge_frac(CaseKind k) { return k == CaseKind::Jewel ? 0.066f : k == CaseKind::Small ? 0.05f : 0.045f; }
/* The cover window of the front face in face uv: the area the cover scan fills (cardboard boxes: nearly the whole face; plastic cases: inside the
 * hinge strip and the rim). Pixel-free so the nominal cover aspect below can be computed without a face. */
inline UvRect front_window(CaseKind k) {
    UvRect r;
    if (k == CaseKind::Box) { r.u0 = 0.028f; r.v0 = 0.028f; r.u1 = 1.f - 0.028f; r.v1 = 1.f - 0.03f; }
    else { r.u0 = hinge_frac(k) + 0.008f; r.v0 = 0.02f; r.u1 = 1.f - 0.024f; r.v1 = 1.f - 0.02f; }
    return r;
}
/* Width / height of the cover window of a console's case (what a scan of that console's cover is expected to measure). */
inline float nominal_cover_aspect(shelf::Media m) {
    const CaseDims d = case_dims(m);
    const UvRect r = front_window(kind_for(m));
    return (d.w * (r.u1 - r.u0)) / (d.h * (r.v1 - r.v0));
}
/* Where a whole cover scan of aspect `art_aspect` (w/h) goes inside a window of aspect `win_aspect` (both pixel aspects), as a rect in window
 * fractions (0..1). The scan is never cropped. A scan within ~15% of the console's nominal cover aspect is the case's own insert: it takes the
 * window as is when it is within 7% of its shape (a hair of stretch), else the window shrinks to the scan's shape (the plastic shows around it);
 * anything else is letterboxed (`letterbox` = true: the margin is painted with the insert colour, not the case colour). */
struct CaseArtFit { UvRect dst; bool letterbox = false; bool exact = false; };
inline CaseArtFit fit_art(float win_aspect, float art_aspect, float nominal) {
    CaseArtFit f;
    if (!(win_aspect > 0.f) || !(art_aspect > 0.f)) return f;
    const float rel = art_aspect / (nominal > 0.f ? nominal : win_aspect);
    f.letterbox = !(rel >= 0.85f && rel <= 1.15f);
    const float k = art_aspect / win_aspect;   // > 1: the scan is wider than the window
    if (k > 0.93f && k < 1.07f) { f.exact = true; return f; }
    if (k > 1.f) { const float h = 1.f / k; f.dst.v0 = (1.f - h) * 0.5f; f.dst.v1 = f.dst.v0 + h; }
    else { f.dst.u0 = (1.f - k) * 0.5f; f.dst.u1 = f.dst.u0 + k; }
    return f;
}
/* A square-ish scan (a Switch game's own icon) on a portrait case: the front is composed (banner, large icon, title) instead of a scan. */
inline bool is_square_art(int w, int h) { return w > 0 && h > 0 && (float)w / (float)h >= 0.9f && (float)w / (float)h <= 1.1f; }
inline bool is_portrait_case(shelf::Media m) { const CaseDims d = case_dims(m); return d.h >= d.w * 1.15f; }
/* uv sub-rects of a full cover scan (BACK | SPINE | FRONT left to right) from the pixel split fullcover_split() returns. */
struct FullParts { UvRect back, spine, front; };
inline FullParts full_cover_parts(int w, int back_x1, int front_x0) {
    FullParts p;
    const float iw = w > 0 ? 1.f / (float)w : 0.f;
    p.back = {0.f, 0.f, (float)back_x1 * iw, 1.f};
    p.spine = {(float)back_x1 * iw, 0.f, (float)front_x0 * iw, 1.f};
    p.front = {(float)front_x0 * iw, 0.f, 1.f, 1.f};
    return p;
}

/* Real heights differ a lot (a DS case is 0.76 of a keep case, a box 0.72): cases are drawn this much bigger so the centre case is always about
 * as tall as the stage allows, but only part-way (the smaller systems still look smaller). */
inline float display_scale(shelf::Media m) { const CaseDims d = case_dims(m); return std::min(std::pow(1.f / d.h, 0.6f), 1.0f / d.w); }   // wide boxes stay narrow enough for neighbours to fit
/* World x of the nearest neighbour (turned ~41 degrees, 0.82 size, 0.92 perspective at its depth) so that its inner edge clears the centre
 * case's face by a small gap. cs / ns = the display scales of the two cases. */
inline float neighbour_x(const CaseDims& c, float cs, const CaseDims& n, float ns) {
    const float persp = 0.922f, yaw1 = 0.72f, scale1 = 0.82f, gap = 0.04f;
    const float half_n = (0.5f * n.w * std::cos(yaw1) + 0.5f * n.d * std::sin(yaw1)) * scale1 * ns;
    return (0.5f * c.w * cs + gap) / persp + half_n;
}
constexpr float kNeighbourBase = 0.66f;   // slot_pose(1).x: enough for two keep cases

/* ================= Coverflow (the default carousel style): a straight coverflow, layout numbers adapted from PyroCade (MIT, Pyrodrifter) =================
 * World units as above (a keep case is 1.0 high; 1.0 = 323 px of the reference). z here is the engine's z: positive = away from the camera. */
namespace cf {
constexpr float kDeg = 0.017453293f;
constexpr float kDist = 4.64f;       // camera distance in case heights (their perspective 1500 px / 323 px)
constexpr float kEye = 0.68f;        // camera height above the floor (their vanishing point sits a little above the case centre)
constexpr float kPivot = 0.5f;       // every case is centred on this height (they pivot about their centre; normalised sizes keep the bottoms close)
constexpr int kReach = 6;            // cases shown each side before the fade
constexpr float kFadeSpan = 1.2f;    // ...and the distance over which one more fades out
constexpr float kHeroX = -0.17f, kHeroZ = -0.40f, kHeroLift = 0.025f, kHeroYaw = -22.f * kDeg, kHeroPitch = -4.f * kDeg;
constexpr float kNbLift = -0.031f, kNbScale = 0.92f, kNbYaw = 50.f * kDeg;
constexpr float kMoveSecs = 0.7f, kMediaSecs = 0.85f, kMediaDwell = 0.42f, kIdleSecs = 6.f;

struct Pose { float x = 0, lift = 0, z = 0, yaw = 0, pitch = 0, scale = 1, alpha = 1; };
inline float nb_x(int a) { return 1.006f + (float)(a - 1) * 0.483f; }
inline float nb_z(int a) { return 0.356f * (float)a + 0.263f; }
/* The resting pose of slot k (k >= 0; 0 = the selected case) on the side `sgn` (+1 right, -1 left). */
inline Pose slot_int(int k, float sgn) {
    Pose p;
    if (k <= 0) { p.x = kHeroX; p.lift = kHeroLift; p.z = kHeroZ; p.yaw = kHeroYaw; p.pitch = kHeroPitch; p.scale = 1.f; return p; }
    p.x = sgn * nb_x(k); p.lift = kNbLift; p.z = nb_z(k); p.yaw = sgn * kNbYaw; p.scale = kNbScale;
    return p;
}
/* The pose for a signed, fractional distance d from the selection: straight lines between the slots, so a case in transit keeps a steady look. */
inline Pose slot(float d) {
    const float a = std::fabs(d), sgn = d < 0.f ? -1.f : 1.f;
    const int k = (int)std::floor(a);
    const float f = a - (float)k;
    const Pose p0 = slot_int(k, sgn), p1 = slot_int(k + 1, sgn);
    Pose p;
    auto L = [&](float u, float v) { return u + (v - u) * f; };
    p.x = L(p0.x, p1.x); p.lift = L(p0.lift, p1.lift); p.z = L(p0.z, p1.z); p.yaw = L(p0.yaw, p1.yaw); p.pitch = L(p0.pitch, p1.pitch); p.scale = L(p0.scale, p1.scale);
    p.alpha = a <= (float)kReach ? 1.f : 1.f - smoothstep01((a - (float)kReach) / kFadeSpan);
    return p;
}
/* Every case is shown about as large as the stage allows whatever its real size: their ds = min(1.5, 323/H, 300/W). */
inline float display_scale(shelf::Media m) { const CaseDims d = case_dims(m); return std::min(1.5f, std::min(1.f / d.h, (300.f / 323.f) / d.w)); }
/* The case's centre stands on one height, so its bottom is at: */
inline float base_lift(const CaseDims& c, float scale) { return kPivot - 0.5f * c.h * scale; }

/* The selected case's idle loop (6 s ease-in-out, starting at rest): bob up to 0.015, yaw 22 -> 26 degrees, pitch -4 -> -3. t in seconds since it settled. */
struct Idle { float lift = 0, yaw = 0, pitch = 0; };
inline Idle idle(double t) {
    const float u = 0.5f - 0.5f * (float)std::cos(6.283185307 * std::fmod(std::max(0.0, t), (double)kIdleSecs) / (double)kIdleSecs);
    return {0.015f * u, -4.f * kDeg * u, 1.f * kDeg * u};
}

/* ---- the motion: cubic-bezier(.22, 1, .36, 1) over 0.7 s, restarted from wherever it is when the target moves (no overshoot) ---- */
inline float ease(float t) {
    if (!(t > 0.f)) return 0.f;
    if (t >= 1.f) return 1.f;
    float lo = 0.f, hi = 1.f, s = t;
    for (int i = 0; i < 22; ++i) {   // x(s) = t, x(s) = 3(.22)s(1-s)^2 + 3(.36)s^2(1-s) + s^3
        const float u = 1.f - s, x = 0.66f * s * u * u + 1.08f * s * s * u + s * s * s;
        if (x < t) lo = s; else hi = s;
        s = 0.5f * (lo + hi);
    }
    return 3.f * s * (1.f - s) + s * s * s;
}
constexpr float kEaseSlope0 = 4.5454545f;   // ease'(0) = 3 / (3 * 0.22): how much faster than average the curve starts
/* The slope of ease() at t: dy/dx at the bezier parameter s with x(s) = t. */
inline float ease_slope(float t) {
    t = std::max(0.f, std::min(1.f, t));
    float lo = 0.f, hi = 1.f, s = t;
    for (int i = 0; i < 22; ++i) {
        const float u = 1.f - s, x = 0.66f * s * u * u + 1.08f * s * s * u + s * s * s;
        if (x < t) lo = s; else hi = s;
        s = 0.5f * (lo + hi);
    }
    const float u = 1.f - s;
    const float dx = 0.66f * (u * u - 2.f * s * u) + 1.08f * (2.f * s * u - s * s) + 3.f * s * s;
    return 3.f * u * u / std::max(1e-4f, dx);
}
struct EaseState { float from = 0, to = 0, t = 1e9f, out = 0, vel = 0, v0 = 0; };
/* Moves x toward target; true while moving. A retarget in mid-glide starts the new segment at the speed the carousel already has (a correction term that
 * dies out with zero slope at the end), so held keys glide instead of kicking at every press; from rest the curve's own fast start is kept. A change of x
 * from outside (a jump) restarts from the new place. reduced snaps. */
inline bool ease_step(float& x, EaseState& e, float target, float dt, bool reduced, float dur = kMoveSecs) {
    if (reduced || !(dur > 0.f)) { x = target; e.from = e.to = e.out = target; e.t = 1e9f; e.vel = 0.f; return false; }
    const bool was_moving = e.t < dur;
    if (x != e.out) { e.from = x; e.to = target; e.t = 0.f; e.v0 = (target - x) * kEaseSlope0 / dur; e.vel = 0.f; }
    else if (target != e.to) {
        e.from = x; e.to = target; e.t = 0.f;
        e.v0 = was_moving ? e.vel : (target - x) * kEaseSlope0 / dur;
        if (was_moving && e.v0 * (target - x) < 0.f) e.v0 = 0.f;   // turning round: no momentum backwards (the media never dips on its way back out)
    }
    if (e.t >= dur) { x = e.out = e.to; e.vel = 0.f; return false; }
    if (!(dt > 0.f)) return true;
    const float h = std::min(dt, 0.1f);
    e.t += h;
    if (e.t >= dur) { x = e.out = e.to; e.vel = 0.f; return false; }
    const float D = e.to - e.from, tt = e.t / dur, k = 1.f - tt;
    float xn = e.from + D * ease(tt) + (e.v0 - D * kEaseSlope0 / dur) * e.t * k * k;
    const float C = e.v0 - D * kEaseSlope0 / dur;
    float vel = D * ease_slope(tt) / dur + C * (k * k - 2.f * e.t * k / dur);   // the derivative of the curve at its end, not the average over the step
    if (D > 0.f && xn >= e.to) { xn = e.to; vel = 0.f; } else if (D < 0.f && xn <= e.to) { xn = e.to; vel = 0.f; }   // never past the target
    e.vel = vel;
    x = e.out = xn;
    return true;
}

/* ---- wide boxes: the neighbours may not run into the selected case ---- */
/* Screen x extent (units of U at depth 0) of a case at x, depth z, turned by yaw, scaled: l / r. */
inline void screen_extent(const CaseDims& c, float scale, float yaw, float x, float z, float* l, float* r) {
    const float ct = std::cos(yaw), st = std::sin(yaw);
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < 4; ++i) {
        const float lx = (i & 1 ? 0.5f : -0.5f) * c.w * scale, lz = (i & 2 ? 0.5f : -0.5f) * c.d * scale;
        const float k = kDist / std::max(0.4f, kDist + z - lx * st + lz * ct);
        const float px = (x + lx * ct + lz * st) * k;
        lo = std::min(lo, px); hi = std::max(hi, px);
    }
    *l = lo; *r = hi;
}
/* How far (>= 0) the first neighbour on one side must stand beyond its slot so its inner edge clears the selected case (c, drawn at scale cs) by a gap. */
inline float neighbour_extra(const CaseDims& c, float cs, const CaseDims& n, float ns, bool right, float gap = 0.03f) {
    float hl, hr;
    screen_extent(c, cs, kHeroYaw, kHeroX, kHeroZ, &hl, &hr);
    const float edge = right ? hr + gap : -hl + gap;   // the left side is the mirror image (positive yaw / x below)
    const float ct = std::cos(kNbYaw), st = std::sin(kNbYaw), sc = kNbScale * ns;
    float need = -1e9f;
    for (int i = 0; i < 4; ++i) {
        const float lx = (i & 1 ? 0.5f : -0.5f) * n.w * sc, lz = (i & 2 ? 0.5f : -0.5f) * n.d * sc;
        const float xr = lx * ct + lz * st, zr = -lx * st + lz * ct;
        const float k = kDist / std::max(0.4f, kDist + nb_z(1) + zr);
        need = std::max(need, edge / k - xr);
    }
    return std::max(0.f, need - nb_x(1));
}
/* ---- the disc / cartridge that slides out from behind the selected case ---- */
enum class MediaKind { None, Disc, Card };
struct MediaSpec {
    MediaKind kind = MediaKind::None;
    float w = 0, h = 0, thick = 0;                  // a card: world units for a 190 mm tall case = 1.0 (a disc: see disc_dia)
    float slide = 0, rise = 0, yaw = 0, roll = 0;   // fractions of the case width / height, radians (yaw: face turned toward the centre; roll: clockwise on screen)
    bool mini = false;
};
inline MediaSpec media_spec(shelf::Media m) {
    using shelf::Media;
    MediaSpec s;
    auto card = [&](float wmm, float hmm, float tmm) { s.kind = MediaKind::Card; s.w = wmm / 190.f; s.h = hmm / 190.f; s.thick = tmm / 190.f; s.slide = 0.71f; s.rise = 0.28f; s.yaw = 17.f * kDeg; s.roll = 9.f * kDeg; };
    auto disc = [&](bool mini) { s.kind = MediaKind::Disc; s.mini = mini; s.thick = 1.2f / 190.f; s.slide = 0.65f; s.rise = 0.04f; s.yaw = 15.f * kDeg; s.roll = 14.f * kDeg; };
    switch (m) {
        case Media::SwitchCase: card(21.f, 31.f, 3.f); break;
        case Media::DsCase: case Media::N3dsCase: card(33.f, 35.f, 3.8f); break;
        case Media::N64Cart: card(116.f, 76.6f, 18.5f); break;
        case Media::SnesCart: card(135.85f, 87.7f, 19.7f); break;
        case Media::NesCart: card(120.f, 134.f, 17.f); break;
        case Media::GbCart: case Media::GbcCart: card(57.f, 65.f, 7.8f); break;
        case Media::GbaCart: card(60.f, 35.f, 8.f); break;
        case Media::PspCase: card(65.f, 64.f, 4.2f); break;   // the UMD
        case Media::GcCase: disc(true); break;
        case Media::Ps1Jewel: case Media::Ps2Case: case Media::WiiUCase: case Media::Generic: disc(false); break;
    }
    return s;
}
/* Disc diameter in world units for a case (before the display scale): 0.94 min(W, H), a GameCube mini-disc 8/12 of that. */
inline float disc_dia(const MediaSpec& s, const CaseDims& c) { return 0.94f * std::min(c.w, c.h) * (s.mini ? 8.f / 12.f : 1.f); }
/* How far (world units, at amount 1) the media sits right of / above the case centre. `scale` = the case's whole scale, mw / mh = the media's size (scaled),
 * `far_limit` = the world x (at the case's depth) its far edge must stay left of (the next neighbour). */
struct MediaOffset { float out = 0, up = 0; };
inline MediaOffset media_offset(const MediaSpec& s, const CaseDims& c, float scale, float mw, float mh, float case_x, float far_limit) {
    MediaOffset o;
    const float W = c.w * scale, H = c.h * scale;
    const float reach = far_limit - case_x - 0.5f * mw * std::cos(s.yaw);   // the most the media may sit right of the case centre
    o.out = std::min(s.slide * W, reach);
    o.out = std::max(o.out, std::min(0.25f * W, reach));   // some of it always shows, but never into the next case
    o.out = std::max(o.out, 0.f);
    o.up = s.kind == MediaKind::Disc ? std::max(s.rise * H, 0.5f * (mh - H)) : s.rise * H;
    return o;
}
/* Who has their media out: the selected case after a 420 ms dwell (cancelled when the selection moves on), slid back in over 0.85 s on leaving. */
struct MediaSlot { int idx = -1; float dwell = 0, x = 0; EaseState es; };
struct MediaReveal {
    static constexpr int kN = 6;
    MediaSlot s[kN];   // s[0]: the selected case; the others: cases whose media is still sliding back in
    void reset() { for (MediaSlot& m : s) m = MediaSlot(); }
    void step(int focus, bool show, float dt, bool reduced) {
        if (reduced) {
            reset();
            s[0].idx = focus; s[0].x = (show && focus >= 0) ? 1.f : 0.f;
            s[0].es.from = s[0].es.to = s[0].es.out = s[0].x;
            return;
        }
        if (s[0].idx != focus) {
            MediaSlot back;
            bool had = false;
            for (int k = 1; k < kN; ++k) if (s[k].idx == focus && focus >= 0) { back = s[k]; had = true; s[k] = MediaSlot(); break; }
            if (s[0].idx >= 0 && s[0].x > 0.002f) {   // the old selection's media slides back: it takes a free slot, or evicts the one least out (never one that is further out)
                int worst = 1;
                for (int k = 1; k < kN; ++k) { if (s[k].idx < 0) { worst = k; break; } if (s[k].x < s[worst].x) worst = k; }
                if (s[worst].idx < 0 || s[worst].x <= s[0].x) s[worst] = s[0];
            }
            s[0] = had ? back : MediaSlot();
            s[0].idx = focus; s[0].dwell = (had && back.x > 0.001f) ? kMediaDwell : 0.f;   // still (partly) out: no dwell, carry on from where it is
        }
        if (show && focus >= 0) s[0].dwell += std::max(0.f, dt); else s[0].dwell = 0.f;
        ease_step(s[0].x, s[0].es, (show && focus >= 0 && s[0].dwell >= kMediaDwell) ? 1.f : 0.f, dt, false, kMediaSecs);
        for (int k = 1; k < kN; ++k) {
            if (s[k].idx < 0) continue;
            ease_step(s[k].x, s[k].es, 0.f, dt, false, kMediaSecs);
            if (s[k].x <= 0.001f && s[k].es.t >= kMediaSecs) s[k] = MediaSlot();
        }
    }
    float amount(int idx) const { for (const MediaSlot& m : s) if (m.idx == idx && idx >= 0) return m.x; return 0.f; }
    bool any() const { for (const MediaSlot& m : s) if (m.idx >= 0 && m.x > 0.001f) return true; return false; }
};
/* What the info panel was built for: the text is rebuilt when any of it changes (the game actually shown, not only the carousel's focus). */
struct InfoKey {
    int focus = -2, idx = -2, tick = -1; uint64_t sig = ~0ull, gen = ~0ull; float w = 0, fs = 0; bool tight = false;
    bool same(const InfoKey& o) const { return focus == o.focus && idx == o.idx && tick == o.tick && sig == o.sig && gen == o.gen && std::fabs(w - o.w) <= 2.f && fs == o.fs && tight == o.tight; }
};
/* The selected-case text block: fades in with a 12 px rise over 0.45 s on every new selection (t = seconds since the selection). */
inline float info_alpha(float t) { return std::max(0.f, std::min(1.f, t / 0.45f)); }
inline float info_rise(float t) { const float k = 1.f - info_alpha(t); return 12.f * k * k; }
}  // namespace cf

/* ---- the spring that moves the carousel ---- */
/* One frame of a soft, slightly under-damped spring toward `target` (the stiffer the further away it is, so a long run keeps up).
 * reduced_motion snaps. Returns true while still moving. */
inline bool pos_step(shelf::Spring& s, float target, float dt, bool reduced_motion = false) {
    if (reduced_motion || !(dt > 0.f)) {
        if (reduced_motion) { s.x = target; s.v = 0; }
        return s.x != target || s.v != 0.f;
    }
    dt = std::min(dt, 0.1f);
    const float err0 = std::fabs(target - s.x);
    const float k = 150.f * (1.f + 0.25f * std::min(err0, 12.f)), c = 2.f * 0.82f * std::sqrt(k);   // zeta ~ 0.82: a hint of overshoot
    const int steps = std::max(1, (int)std::ceil(dt / (1.f / 180.f)));
    const float h = dt / (float)steps;
    for (int i = 0; i < steps; ++i) {
        s.v += (k * (target - s.x) - c * s.v) * h;
        s.x += s.v * h;
    }
    if (std::fabs(target - s.x) < 0.0008f && std::fabs(s.v) < 0.012f) { s.x = target; s.v = 0; return false; }
    return true;
}

/* ---- stepping ---- */
/* One step of `dir` (+1 / -1) from `cur` among n cases. A fresh press at an end wraps around when `wrap`; otherwise it clamps. */
inline int step_index(int cur, int dir, int n, bool wrap) {
    if (n <= 0) return 0;
    cur = std::max(0, std::min(n - 1, cur));
    const int t = cur + (dir < 0 ? -1 : dir > 0 ? 1 : 0);
    if (t < 0) return wrap ? n - 1 : 0;
    if (t >= n) return wrap ? 0 : n - 1;
    return t;
}
/* Cases moved per repeat while a direction is held for `held` seconds: 1, then 2, 3, 5 as the hold goes on. */
inline int hold_boost(float held) { return held < 0.9f ? 1 : held < 1.8f ? 2 : held < 3.0f ? 3 : 5; }
/* Target index after moving `steps` cases (clamped, never wraps while held). */
inline int clamp_index(int i, int n) { return n <= 0 ? 0 : std::max(0, std::min(n - 1, i)); }
/* The cases to draw for a carousel position: [first, last] clamped to [0, n-1], first > last when n == 0. */
inline void window(float pos, int n, int* first, int* last) {
    *first = std::max(0, (int)std::floor(pos) - kWindow);
    *last = std::min(n - 1, (int)std::ceil(pos) + kWindow);
}
/* How far a jump may animate: a longer way snaps the carousel to within this distance first, then glides in. */
inline float jump_start(float pos, int target, float max_glide) {
    const float d = (float)target - pos;
    if (std::fabs(d) <= max_glide) return pos;
    return (float)target - (d < 0.f ? -max_glide : max_glide);
}

/* ---- console tabs ---- */
struct Tab { std::string sys; std::string label; int count = 0; };   // sys "" = All
inline const char* tab_label(const std::string& sys) {
    static const char* m[][2] = {{"n64", "N64"}, {"gc", "GameCube"}, {"ps1", "PS1"}, {"wiiu", "Wii U"}, {"3ds", "3DS"}, {"nds", "DS"}, {"ps2", "PS2"},
                                 {"psp", "PSP"}, {"switch", "Switch"}, {"switch2", "Switch 2"}, {"nes", "NES"}, {"snes", "SNES"}, {"gba", "GBA"}, {"gbc", "GBC"}, {"gb", "Game Boy"}};
    for (auto& e : m) if (sys == e[0]) return e[1];
    return nullptr;
}
inline int console_rank(const std::string& sys) {
    static const char* o[] = {"n64", "gc", "ps1", "wiiu", "3ds", "nds", "ps2", "psp", "switch", "switch2", "nes", "snes", "gba", "gbc", "gb"};
    for (int i = 0; i < (int)(sizeof(o) / sizeof(o[0])); ++i) if (sys == o[i]) return i;
    return 100;
}
/* "All" first, then every console that has games (count > 0, or `keep` = the active one) in the library's console order. */
inline std::vector<Tab> build_tabs(const std::map<std::string, int>& counts, int total, const std::string& keep) {
    std::vector<Tab> t;
    t.push_back({"", "All", total});
    std::vector<Tab> rest;
    for (const auto& kv : counts) {
        if (kv.first.empty() || (kv.second <= 0 && kv.first != keep)) continue;
        std::string lab = tab_label(kv.first) ? tab_label(kv.first) : kv.first;
        if (!tab_label(kv.first)) for (char& c : lab) c = (char)std::toupper((unsigned char)c);
        rest.push_back({kv.first, lab, kv.second});
    }
    std::stable_sort(rest.begin(), rest.end(), [](const Tab& a, const Tab& b) {
        const int ra = console_rank(a.sys), rb = console_rank(b.sys);
        return ra != rb ? ra < rb : a.sys < b.sys;
    });
    for (auto& r : rest) t.push_back(r);
    return t;
}
inline int tab_index(const std::vector<Tab>& tabs, const std::string& sys) {
    for (size_t i = 0; i < tabs.size(); ++i) if (tabs[i].sys == sys) return (int)i;
    return 0;
}
/* LB / RB: the neighbouring tab, wrapping. */
inline int tab_step(int cur, int dir, int n) { return n <= 0 ? 0 : ((cur + (dir < 0 ? -1 : 1)) % n + n) % n; }

/* The last selected game of each console tab ("" = the All tab), so switching tabs lands where the player left off. */
struct TabMemory {
    std::map<std::string, std::string> last;
    void remember(const std::string& tab_sys, const std::string& game_sys, const std::string& key) {
        if (key.empty()) return;
        last[tab_sys] = key;
        last[game_sys] = key;   // the same game is also the last one seen on its own console's tab
        last[""] = key;
    }
    std::string recall(const std::string& tab_sys) const {
        auto it = last.find(tab_sys);
        return it == last.end() ? std::string() : it->second;
    }
};
/* Where to land after a tab change: the remembered game when it is in `keys` (the new tab's games, display order), else 0. */
inline int land_index(const std::vector<std::string>& keys, const std::string& remembered) {
    for (size_t i = 0; i < keys.size(); ++i) if (!remembered.empty() && keys[i] == remembered) return (int)i;
    return 0;
}

/* ---- browse / flip / inspect ---- */
enum class Mode { Browse, Flip, Inspect };
struct ModeState {
    Mode mode = Mode::Browse;
    float flip_target() const { return mode == Mode::Flip ? 3.14159265f : 0.f; }
};
/* X: tap flips the centre case to its back and back again (from Inspect, a tap goes back to browsing). */
inline Mode on_flip_tap(Mode m) { return m == Mode::Browse ? Mode::Flip : Mode::Browse; }
/* R3 / R / dragging the centre case: inspect; again (or B) returns. A flipped case cannot be inspected until it is flipped back. */
inline Mode on_inspect(Mode m) { return m == Mode::Inspect ? Mode::Browse : m == Mode::Browse ? Mode::Inspect : m; }
/* B: leaves flip / inspect and reports whether it consumed the press (otherwise B goes on to "back"). */
inline bool on_back(Mode* m) { if (*m == Mode::Browse) return false; *m = Mode::Browse; return true; }
/* X held for this long toggles the favourite instead of flipping. */
constexpr float kFavHold = 0.55f;
/* Moving to another case or console leaves flip / inspect. */
inline Mode on_move(Mode) { return Mode::Browse; }

/* Free rotation while inspecting: the stick / drag deltas turn the case (yaw unlimited, pitch limited). */
struct Spin { float yaw = 0, pitch = 0; };
inline Spin spin_apply(Spin s, float dx, float dy) {
    s.yaw += dx;
    s.pitch = std::max(-0.55f, std::min(0.55f, s.pitch + dy));
    return s;
}
/* The rest yaw to settle on when leaving inspect (nearest whole turn): the case swings back to face the viewer. */
inline float spin_home(float yaw) { const float tau = 6.28318531f; return std::round(yaw / tau) * tau; }

/* ---- shuffle ---- */
/* Index of a random game for L3 / Z: the Surprise-me weighting (games not played for a while, favourites a little likelier) over the
 * shown games, never the current one when there is a choice. u in [0,1). -1 for an empty list. */
struct ShuffleCand { std::string key; int64_t last_played = 0; bool favorite = false; };
inline int pick_shuffle(const std::vector<ShuffleCand>& c, int cur, int64_t now, double u) {
    if (c.empty()) return -1;
    if (c.size() == 1) return 0;
    std::vector<surprise::Cand> sc;
    sc.reserve(c.size());
    for (const ShuffleCand& x : c) sc.push_back({x.key, x.last_played, x.favorite});
    const std::string avoid = cur >= 0 && cur < (int)c.size() ? c[(size_t)cur].key : std::string();
    const surprise::Pick p = surprise::pick(sc, now, u, avoid);
    return p.index < 0 ? 0 : p.index;
}

/* ---- the strings under the centre case ---- */
/* "Nintendo 64  -  1999  -  1-4 players": console, then year and players when known. */
inline std::string subtitle_text(const std::string& console, int year, int players) {
    std::string s = console;
    if (year > 0) s += "  -  " + std::to_string(year);
    if (players == 1) s += "  -  1 player";
    else if (players > 1) s += "  -  1-" + std::to_string(players) + " players";
    return s;
}

/* ---- nav: sort sections, scrubber, quick jump ---- */
/* What the scrubber's marks name for each sort mode, and the pure pieces behind them: labels per game, runs of equal labels ("sections"),
 * previous / next section, index <-> scrubber fraction, typed-prefix lookup, the sort picker rows and the faster hold / wheel travel. */
enum class NavKind { Letters, Recency, Plays, Added, Years, Consoles };
inline NavKind nav_kind_for(SortMode m) {
    switch (m) {
        case SortMode::LastPlayed: return NavKind::Recency;
        case SortMode::MostPlayed: return NavKind::Plays;
        case SortMode::RecentlyAdded: return NavKind::Added;
        case SortMode::ReleaseYear: return NavKind::Years;
        case SortMode::Console: return NavKind::Consoles;
        default: return NavKind::Letters;
    }
}
/* Title: "A".."Z", everything else "#" (digits, symbols, non-ASCII). */
inline std::string nav_label_letter(const std::string& title) { std::string l = letter_of(title); return l.empty() ? std::string("#") : l; }
inline std::string nav_label_year(int year) { return year > 0 ? std::to_string(year) : std::string("?"); }
/* Age buckets for Last played / Recently added: never (t == 0), then Today (< 1 day), This week, This month, Earlier. */
inline std::string nav_label_age(int64_t now, int64_t t, bool never_label = true) {
    if (t <= 0) return never_label ? "Never" : "Unknown";
    const int64_t age = now - t;
    return age < 86400 ? "Today" : age < 7 * 86400 ? "This week" : age < 30 * 86400 ? "This month" : "Earlier";
}
inline std::string nav_label_plays(int64_t seconds) {
    return seconds <= 0 ? "Never" : seconds >= 36000 ? "10h+" : seconds >= 3600 ? "1h+" : "Under 1h";
}
constexpr const char* kNavFavorites = "Favorites";

struct NavSection { std::string label; int first = 0; int count = 0; };
/* Runs of equal labels in list order. An empty label (e.g. the pinned native game) joins the section before it, or the first one after it. */
inline std::vector<NavSection> build_sections(const std::vector<std::string>& labels) {
    std::vector<NavSection> out;
    int lead = 0;   // empty labels at the very start, folded into the first real section
    for (int i = 0; i < (int)labels.size(); ++i) {
        const std::string& l = labels[(size_t)i];
        if (l.empty()) { if (out.empty()) ++lead; else ++out.back().count; continue; }
        if (!out.empty() && out.back().label == l) { ++out.back().count; continue; }
        NavSection s;
        s.label = l; s.first = out.empty() ? 0 : i; s.count = out.empty() ? lead + 1 : 1;
        out.push_back(s);
    }
    return out;
}
inline int section_index(const std::vector<NavSection>& secs, int idx) {
    for (int k = (int)secs.size() - 1; k >= 0; --k) if (idx >= secs[(size_t)k].first) return k;
    return secs.empty() ? -1 : 0;
}
/* LT / RT / PageUp / PageDown: dir > 0 goes to the start of the next section (the last game from the last section); dir < 0 goes to the start of
 * this section, or of the previous one when already there. */
inline int section_step(const std::vector<NavSection>& secs, int cur, int dir, int n) {
    if (n <= 0) return 0;
    cur = clamp_index(cur, n);
    if (secs.empty()) return clamp_index(cur + (dir < 0 ? -8 : 8), n);
    const int k = section_index(secs, cur);
    if (dir > 0) return k + 1 < (int)secs.size() ? secs[(size_t)k + 1].first : n - 1;
    if (cur > secs[(size_t)k].first) return secs[(size_t)k].first;
    return k > 0 ? secs[(size_t)k - 1].first : 0;
}
/* First section with this label (quick-jump by name); -1 when none. */
inline int section_by_label(const std::vector<NavSection>& secs, const std::string& label) {
    for (size_t k = 0; k < secs.size(); ++k) if (secs[k].label == label) return (int)k;
    return -1;
}
/* The scrubber maps the whole list onto its track: fraction 0..1 <-> index 0..n-1. */
inline float scrub_frac(int idx, int n) { return n <= 1 ? 0.f : std::max(0.f, std::min(1.f, (float)idx / (float)(n - 1))); }
inline int scrub_index(float frac, int n) { return n <= 0 ? 0 : clamp_index((int)std::lround(std::max(0.f, std::min(1.f, frac)) * (float)(n - 1)), n); }
/* x pixel of the section's mark on a track [x0, x1]: where its first game sits. */
inline float scrub_x(int idx, int n, float x0, float x1) { return x0 + scrub_frac(idx, n) * (x1 - x0); }
/* "123 / 2,400" */
inline std::string count_text(int pos0, int n) {
    auto group = [](int v) { std::string d = std::to_string(v), o; for (size_t i = 0; i < d.size(); ++i) { if (i && (d.size() - i) % 3 == 0) o += ','; o += d[i]; } return o; };
    return group(n <= 0 ? 0 : pos0 + 1) + " / " + group(n);
}
/* Quick-jump typing (Title sort): the first game whose title starts with `prefix` (case-insensitive); -1 when none. `title_at(i)` returns a
 * const std::string& / const char*. */
template <class F> inline int prefix_index(int n, F&& title_at, const std::string& prefix) {
    if (prefix.empty()) return -1;
    for (int i = 0; i < n; ++i) {
        const std::string t = title_at(i);
        if (t.size() < prefix.size()) continue;
        bool ok = true;
        for (size_t k = 0; k < prefix.size() && ok; ++k) ok = std::tolower((unsigned char)t[k]) == std::tolower((unsigned char)prefix[k]);
        if (ok) return i;
    }
    return -1;
}
/* Held direction: cases per repeat. Slower to start than hold_boost, and it keeps climbing so a 2,000-game library is crossed in about ten seconds. */
inline int nav_hold_boost(float held) { return held < 0.8f ? 1 : held < 1.6f ? 2 : held < 2.6f ? 4 : held < 3.8f ? 8 : held < 5.f ? 14 : 24; }
/* Wheel: notches per second -> cases per notch (1 for a slow scroll, up to 8 for a fast spin). */
inline float nav_wheel_gain(float notches_per_sec) { return 1.f + 7.f * std::max(0.f, std::min(1.f, (notches_per_sec - 5.f) / 15.f)); }
/* True when travel is fast enough that the section bubble should show. */
inline bool nav_is_fast(int hold_boost_now, float wheel_gain_now) { return hold_boost_now >= 2 || wheel_gain_now >= 1.5f; }

/* The sort picker: one row per mode, then the two toggles. */
constexpr int kSortRows = kSortModes + 2;
inline SortSpec sort_apply_row(SortSpec cur, int row) {
    if (row >= 0 && row < kSortModes) { if (cur.mode != (SortMode)row) { cur.mode = (SortMode)row; cur.reverse = false; } }   // a new mode starts in its natural direction; the active one again changes nothing
    else if (row == kSortModes) cur.reverse = !cur.reverse;
    else if (row == kSortModes + 1) cur.favs_first = !cur.favs_first;
    return cur;
}
inline bool sort_row_on(const SortSpec& s, int row) {
    return row < kSortModes ? (int)s.mode == row : row == kSortModes ? s.reverse : s.favs_first;
}

}  // namespace hubui::cases
