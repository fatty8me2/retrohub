/* Game start animation: hand-built vector art (ImDrawList only) for every console, its media and the shelf.
 * Original stylised shapes: no logos, no brand names, no textures except the `label` cover art. */
#include "boot_art.h"
#include <cmath>
#include <cstring>
#include <cctype>
#include <vector>
#include <string>
#include <utility>
#include <algorithm>

namespace bootanim {
using hub::System;

namespace {
constexpr float PI = 3.14159265f;
inline float c01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline ImU32 rgb(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
ImU32 mixc(ImU32 a, ImU32 b, float t) {
    t = c01(t);
    auto ch = [&](int sh) { return (int)lerpf((float)((a >> sh) & 255), (float)((b >> sh) & 255), t); };
    return IM_COL32(ch(IM_COL32_R_SHIFT), ch(IM_COL32_G_SHIFT), ch(IM_COL32_B_SHIFT), ch(IM_COL32_A_SHIFT));
}
ImU32 withA(ImU32 c, float a) { int al = (int)(((c >> IM_COL32_A_SHIFT) & 255) * c01(a)); return (c & ~IM_COL32_A_MASK) | ((ImU32)al << IM_COL32_A_SHIFT); }
ImU32 shade(ImU32 c, float k) {   // k<1 darker, k>1 lighter (toward white)
    ImU32 tgt = k <= 1.f ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255);
    float t = k <= 1.f ? 1.f - k : k - 1.f;
    return (mixc(c, tgt, t) & 0x00FFFFFF) | (c & IM_COL32_A_MASK);
}
uint32_t hsh(uint32_t x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x; }
float hf(uint32_t x) { return (hsh(x) & 0xFFFF) / 65535.f; }

void path_rr(ImDrawList* d, ImVec2 a, ImVec2 b, float tl, float tr, float br, float bl) {
    auto arc = [&](float cx, float cy, float r, float a0, float a1) {
        if (r <= 0.5f) d->PathLineTo(ImVec2(cx, cy)); else d->PathArcTo(ImVec2(cx, cy), r, a0, a1, 8);
    };
    arc(a.x + tl, a.y + tl, tl, PI, 1.5f * PI);
    arc(b.x - tr, a.y + tr, tr, 1.5f * PI, 2.f * PI);
    arc(b.x - br, b.y - br, br, 0.f, 0.5f * PI);
    arc(a.x + bl, b.y - bl, bl, 0.5f * PI, PI);
}

/* Local-coordinate painter: everything is drawn in a console's own units, scaled by s around c. */
struct K {
    ImDrawList* d; ImVec2 c; float s;
    ImVec2 p(float x, float y) const { return ImVec2(c.x + x * s, c.y + y * s); }
    void rr4(float x0, float y0, float x1, float y1, ImU32 col, float tl, float tr, float br, float bl) {
        path_rr(d, p(x0, y0), p(x1, y1), tl * s, tr * s, br * s, bl * s); d->PathFillConvex(col);
    }
    void rr(float x0, float y0, float x1, float y1, ImU32 col, float r = 0) { rr4(x0, y0, x1, y1, col, r, r, r, r); }
    void grad4(float x0, float y0, float x1, float y1, ImU32 top, ImU32 bot, float tl, float tr, float br, float bl) {
        ImVec2 a = p(x0, y0), b = p(x1, y1);
        int n = (int)std::max(2.f, std::min(10.f, (b.y - a.y) / 5.f));
        for (int i = 0; i < n; i++) {
            float ya = lerpf(a.y, b.y, (float)i / n) - (i ? 0.5f : 2.f), yb = lerpf(a.y, b.y, (float)(i + 1) / n) + (i == n - 1 ? 2.f : 0.5f);
            d->PushClipRect(ImVec2(a.x - 3, ya), ImVec2(b.x + 3, yb), true);
            path_rr(d, a, b, tl * s, tr * s, br * s, bl * s);
            d->PathFillConvex(mixc(top, bot, (i + 0.5f) / n));
            d->PopClipRect();
        }
    }
    void grad(float x0, float y0, float x1, float y1, ImU32 top, ImU32 bot, float r = 0) { grad4(x0, y0, x1, y1, top, bot, r, r, r, r); }
    void line(float x0, float y0, float x1, float y1, ImU32 col, float th = 1.f) { d->AddLine(p(x0, y0), p(x1, y1), col, std::max(1.f, th * s)); }
    void circ(float x, float y, float r, ImU32 col) { d->AddCircleFilled(p(x, y), r * s, col, 40); }
    void ring(float x, float y, float r, ImU32 col, float th = 1.f) { d->AddCircle(p(x, y), r * s, col, 40, std::max(1.f, th * s)); }
    void ell(float x, float y, float rx, float ry, ImU32 col) { d->AddEllipseFilled(p(x, y), ImVec2(rx * s, ry * s), col, 0.f, 48); }
    void ellr(float x, float y, float rx, float ry, ImU32 col, float th = 1.f) { d->AddEllipse(p(x, y), ImVec2(rx * s, ry * s), col, 0.f, 48, std::max(1.f, th * s)); }
    void poly(std::initializer_list<ImVec2> pts, ImU32 col) {
        ImVec2 q[16]; int n = 0; for (auto& v : pts) if (n < 16) q[n++] = p(v.x, v.y);
        d->AddConvexPolyFilled(q, n, col);
    }
    void rot_rr(float cx, float cy, float w, float h, float ang, ImU32 col) {
        float ca = std::cos(ang), sa = std::sin(ang); ImVec2 q[4]; int i = 0;
        for (auto [dx, dy] : {std::pair<float, float>{-w / 2, -h / 2}, {w / 2, -h / 2}, {w / 2, h / 2}, {-w / 2, h / 2}})
            q[i++] = p(cx + dx * ca - dy * sa, cy + dx * sa + dy * ca);
        d->AddConvexPolyFilled(q, 4, col);
    }
    void shadow(float x0, float y0, float x1, float y1, float r, float spread, int alpha) {
        for (int i = 4; i >= 1; i--) { float e = spread * i / 4.f; rr(x0 - e, y0 - e * 0.4f, x1 + e, y1 + e * 1.2f, IM_COL32(0, 0, 0, alpha / 4), r + e); }
    }
    void shadow_ell(float x, float y, float rx, float ry, int alpha) {
        for (int i = 4; i >= 1; i--) ell(x, y, rx + i * 5, ry + i * 2.5f, IM_COL32(0, 0, 0, alpha / 4));
    }
    void led(float x, float y, float r, float power, ImU32 on) {
        circ(x, y, r + 1.2f, rgb(10, 10, 12));
        circ(x, y, r, mixc(rgb(40, 40, 44), mixc(on, rgb(255, 255, 255), 0.35f), power));
        if (power > 0.01f) {
            for (int i = 4; i >= 1; i--) circ(x, y, r + i * 3.5f, withA(on, 0.10f * power));
            circ(x, y, r * 0.5f, withA(rgb(255, 255, 255), 0.8f * power));
        }
    }
    void gloss(float x0, float y0, float x1, float y1, float r, int a = 38) { rr(x0 + 2, y0 + 1.5f, x1 - 2, y0 + 1.5f + std::max(2.f, (y1 - y0) * 0.12f), IM_COL32(255, 255, 255, a), r); }
};

/* ---------- consoles ---------- */

void console_n64(K& k, float lid, float power) {
    (void)lid;
    k.shadow_ell(0, 92, 135, 14, 120);
    ImU32 body = rgb(78, 80, 86), dark = rgb(34, 35, 39);
    // back ridge holding the slot
    k.poly({{-92, -96}, {92, -96}, {104, -52}, {-104, -52}}, rgb(96, 98, 105));
    k.line(-92, -96, 92, -96, rgb(130, 133, 140), 2);
    // top face
    k.poly({{-104, -52}, {104, -52}, {136, 22}, {-136, 22}}, rgb(88, 90, 97));
    k.poly({{-104, -52}, {104, -52}, {112, -34}, {-112, -34}}, rgb(100, 102, 110));
    // front face
    k.grad4(-136, 22, 136, 88, rgb(66, 68, 73), rgb(40, 41, 45), 0, 0, 8, 8);
    k.line(-136, 22, 136, 22, rgb(140, 143, 150), 1.5f);
    // cartridge slot (recessed)
    k.poly({{-66, -86}, {66, -86}, {78, -4}, {-78, -4}}, dark);
    k.poly({{-58, -80}, {58, -80}, {68, -10}, {-68, -10}}, rgb(14, 14, 17));
    k.poly({{-78, -4}, {78, -4}, {82, 6}, {-82, 6}}, rgb(60, 62, 68));
    k.line(-58, -80, -68, -10, rgb(70, 72, 80), 1.5f); k.line(58, -80, 68, -10, rgb(70, 72, 80), 1.5f);
    // buttons
    k.rr(-118, -26, -92, -12, rgb(120, 122, 130), 3);
    k.rr(-116, -24, -94, -18, rgb(150, 152, 160), 2);
    k.rr(92, -34, 122, -12, rgb(50, 51, 56), 3);
    k.rr(100, -30, 114, -16, rgb(168, 60, 52), 2);
    // front: four ports with coloured marks, status LED
    ImU32 pc[4] = {rgb(60, 110, 230), rgb(60, 190, 90), rgb(240, 200, 50), rgb(230, 70, 60)};
    for (int i = 0; i < 4; i++) {
        float x = -90.f + i * 60.f;
        k.rr(x - 22, 44, x + 22, 70, rgb(18, 18, 21), 4);
        k.rr(x - 18, 48, x + 18, 66, rgb(8, 8, 10), 3);
        k.circ(x, 36, 3.2f, pc[i]);
    }
    k.led(122, 34, 3.2f, power, rgb(255, 70, 50));
    k.gloss(-136, 22, 136, 88, 4, 22);
    (void)body;
}

void gb_handheld(K& k, bool color, float power) {
    float hw = color ? 90.f : 95.f, top = color ? -140.f : -145.f, bot = color ? 140.f : 145.f;
    k.shadow(-hw, top, hw, bot, 12, 16, 120);
    ImU32 b0 = color ? rgb(124, 84, 196) : rgb(200, 200, 190), b1 = color ? rgb(84, 52, 150) : rgb(168, 168, 158);
    // top face + slot
    k.rr4(-hw + 2, top - 12, hw - 2, top + 8, shade(b0, 1.12f), 6, 6, 0, 0);
    k.rr(-hw + 14, top - 8, hw - 14, top - 1, rgb(14, 14, 16), 2);
    k.rr(-hw + 14, top - 8, hw - 14, top - 6, rgb(70, 70, 76), 1);
    k.grad4(-hw, top, hw, bot, b0, b1, 6, 6, color ? 18 : 40, color ? 18 : 12);
    k.gloss(-hw, top, hw, bot, 5, 40);
    // bezel + screen
    ImU32 bz = color ? rgb(40, 34, 56) : rgb(104, 100, 116);
    k.rr4(-hw + 12, top + 18, hw - 12, top + 138, bz, 8, 8, color ? 24 : 30, 8);
    float sx = color ? 52.f : 48.f;
    ImU32 off = color ? rgb(62, 70, 66) : rgb(92, 100, 88), on = color ? rgb(176, 210, 190) : rgb(160, 176, 70);
    k.rr(-sx, top + 36, sx, top + 112, mixc(off, on, power), 3);
    if (power > 0.2f) {
        float a = c01(power);
        if (color) { k.rr(-sx, top + 36, sx, top + 74, withA(rgb(110, 170, 255), 0.7f * a), 3); k.rr(-sx, top + 90, sx, top + 112, withA(rgb(80, 180, 90), 0.8f * a), 3);
                     k.rr(-14, top + 62, 8, top + 90, withA(rgb(230, 90, 70), 0.9f * a), 2); }
        else { for (int i = 0; i < 6; i++) k.rr(-40 + i * 14, top + 84 - (i % 3) * 8, -30 + i * 14, top + 112, withA(rgb(60, 80, 30), 0.7f * a), 0); }
    }
    k.led(-hw + 22, top + 60, 3.4f, power, color ? rgb(120, 255, 120) : rgb(255, 60, 40));
    k.rr(-sx, top + 36, sx, top + 40, IM_COL32(0, 0, 0, 40), 2);
    // d-pad, A/B, start/select, speaker
    ImU32 pad = color ? rgb(40, 28, 70) : rgb(44, 44, 50);
    k.rr(-70, top + 190 - 10, -26, top + 190 + 10, pad, 4); k.rr(-58, top + 190 - 22, -38, top + 190 + 22, pad, 4);
    k.rr(-67, top + 188, -29, top + 191, IM_COL32(255, 255, 255, 22), 1);
    ImU32 ab = color ? rgb(68, 40, 118) : rgb(150, 36, 92);
    k.circ(48, top + 184, 15, IM_COL32(0, 0, 0, 60)); k.circ(48, top + 182, 15, ab); k.circ(46, top + 177, 6, IM_COL32(255, 255, 255, 40));
    k.circ(18, top + 202, 15, IM_COL32(0, 0, 0, 60)); k.circ(18, top + 200, 15, ab); k.circ(16, top + 195, 6, IM_COL32(255, 255, 255, 40));
    k.rot_rr(-14, top + 240, 28, 8, -0.4f, rgb(70, 70, 78)); k.rot_rr(16, top + 240, 28, 8, -0.4f, rgb(70, 70, 78));
    for (int i = 0; i < 6; i++) { float o = i * 8.f; k.line(46 + o * 0.45f, top + 222 + o * 0.15f + 6, 60 + o * 0.45f, top + 222 + o * 0.15f - 10 + 6 + 12 * 0, IM_COL32(0, 0, 0, 90), 3); }
}

void console_gba(K& k, float lid, float power) {
    (void)lid;
    k.shadow(-145, -68, 145, 68, 26, 14, 120);
    ImU32 b0 = rgb(116, 100, 206), b1 = rgb(76, 64, 150);
    // shoulder buttons + rear slot housing
    k.rr4(-136, -80, -78, -60, rgb(86, 74, 170), 8, 4, 0, 0); k.rr4(78, -80, 136, -60, rgb(86, 74, 170), 4, 8, 0, 0);
    k.rr4(-60, -84, 60, -60, rgb(62, 54, 120), 6, 6, 0, 0);
    k.rr(-46, -80, 46, -70, rgb(10, 10, 14), 2);
    k.rr(-46, -72, 46, -70, rgb(80, 74, 130), 1);
    k.grad(-145, -68, 145, 68, b0, b1, 28);
    k.gloss(-145, -68, 145, 68, 10, 46);
    k.rr(-72, -52, 72, 52, rgb(26, 24, 40), 9);
    float on = power;
    k.rr(-60, -42, 60, 42, mixc(rgb(58, 62, 64), rgb(150, 200, 235), on), 3);
    if (on > 0.2f) {
        k.rr(-60, 10, 60, 42, withA(rgb(70, 170, 80), 0.85f * on), 3);
        k.rr(-60, -42, 60, -10, withA(rgb(255, 255, 255), 0.18f * on), 3);
        k.circ(-22, -14, 11, withA(rgb(255, 220, 90), 0.9f * on)); k.rr(14, 0, 40, 10, withA(rgb(230, 100, 70), 0.9f * on), 1);
    }
    k.led(-92, -30, 3.2f, power, rgb(255, 80, 60));
    ImU32 pad = rgb(36, 32, 60);
    k.rr(-124, -8, -86, 8, pad, 4); k.rr(-113, -19, -97, 19, pad, 4);
    k.circ(108, -10, 13, IM_COL32(0, 0, 0, 70)); k.circ(108, -12, 13, rgb(52, 46, 90)); k.circ(86, 8, 13, IM_COL32(0, 0, 0, 70)); k.circ(86, 6, 13, rgb(52, 46, 90));
    k.rot_rr(-86, 40, 22, 7, -0.4f, rgb(60, 54, 100)); k.rot_rr(-60, 40, 22, 7, -0.4f, rgb(60, 54, 100));
    for (int i = 0; i < 5; i++) k.circ(108 + i * 5.f, 42 - i * 3.f, 1.6f, IM_COL32(0, 0, 0, 90));
}

void console_ds(K& k, bool is3, float lid, float power) {
    (void)lid;
    float w = is3 ? 115.f : 108.f;
    ImU32 b0 = is3 ? rgb(70, 160, 214) : rgb(214, 220, 232), b1 = is3 ? rgb(44, 112, 170) : rgb(172, 180, 198);
    ImU32 inner = is3 ? rgb(34, 38, 48) : rgb(190, 196, 210);
    k.shadow(-w, -150, w, 146, 14, 16, 110);
    // upper half
    k.grad4(-w, -150, w, -12, b0, b1, 14, 14, 3, 3);
    k.rr(-w + 8, -142, w - 8, -20, inner, 8);
    float scw = is3 ? 76.f : 62.f;
    k.rr(-scw - 4, -134, scw + 4, -26, rgb(14, 14, 18), 4);
    k.rr(-scw, -130, scw, -30, mixc(rgb(30, 32, 38), is3 ? rgb(120, 200, 240) : rgb(130, 190, 235), power), 2);
    if (power > 0.2f) { k.rr(-scw, -130, scw, -96, withA(rgb(255, 255, 255), 0.22f * power), 2); k.rr(-scw, -62, scw, -30, withA(rgb(80, 180, 90), 0.85f * power), 2); }
    for (int i = 0; i < 5; i++) { k.circ(-w + 22, -110 + i * 8.f, 1.8f, IM_COL32(0, 0, 0, 100)); k.circ(w - 22, -110 + i * 8.f, 1.8f, IM_COL32(0, 0, 0, 100)); }
    // hinge
    k.rr(-70, -14, 70, 6, is3 ? rgb(36, 90, 140) : rgb(150, 156, 172), 5);
    k.rr(-66, -12, 66, -7, IM_COL32(255, 255, 255, 60), 2);
    // lower half
    k.grad4(-w, 4, w, 142, b0, b1, 3, 3, 16, 16);
    k.rr(-w + 8, 12, w - 8, 134, inner, 8);
    float tw = is3 ? 54.f : 62.f;
    k.rr(-tw - 4, 20, tw + 4, 118, rgb(14, 14, 18), 4);
    k.rr(-tw, 24, tw, 114, mixc(rgb(34, 36, 40), rgb(200, 220, 230), power), 2);
    if (power > 0.2f) k.rr(-tw + 6, 34, tw - 6, 58, withA(rgb(90, 140, 220), 0.5f * power), 3);
    ImU32 pad = is3 ? rgb(60, 66, 80) : rgb(120, 126, 142);
    float lx = -w + 26;
    if (is3) { k.circ(lx, 44, 12, rgb(20, 22, 26)); k.circ(lx, 43, 10, pad); }
    k.rr(lx - 14, 82 - 4, lx + 14, 82 + 4, pad, 2); k.rr(lx - 4, 82 - 14, lx + 4, 82 + 14, pad, 2);
    float rx = w - 26;
    for (auto [dx, dy] : {std::pair<float, float>{0, -12}, {12, 0}, {0, 12}, {-12, 0}}) k.circ(rx + dx, 82 + dy, 6.2f, pad);
    k.led(w - 12, 124, 2.6f, power, rgb(70, 220, 255));
    // card slot on the lower bottom edge
    k.rr(30, 138, 92, 146, rgb(10, 10, 12), 2);
    k.rr(30, 138, 92, 140, rgb(90, 90, 100), 1);
}

/* Elliptical top lid shared by the two top-loaders. hinge = y of the back hinge. */
void top_lid(K& k, float cx, float cy, float rx, float ry, float hinge, float open, ImU32 face, ImU32 edge, ImU32 well, ImU32 hub) {
    k.ell(cx, cy, rx, ry, well);
    k.ell(cx, cy, rx * 0.9f, ry * 0.88f, shade(well, 0.7f));
    k.ell(cx, cy, rx * 0.2f, ry * 0.2f, hub); k.ell(cx, cy, rx * 0.07f, ry * 0.07f, rgb(20, 20, 24));
    float o = c01(open); o = o * o * (3 - 2 * o);
    float lcy = lerpf(cy, hinge - ry * 1.15f, o), lry = ry * lerpf(1.f, 1.12f, o);
    k.ell(cx, lcy + 4, rx + 1, lry, IM_COL32(0, 0, 0, 70));
    k.ell(cx, lcy, rx, lry, edge);
    k.ell(cx, lcy - 2, rx * 0.95f, lry * 0.92f, face);
    k.ellr(cx, lcy - 3, rx * 0.8f, lry * 0.74f, IM_COL32(255, 255, 255, 30), 1.5f);
    k.ell(cx - rx * 0.35f, lcy - lry * 0.45f, rx * 0.3f, lry * 0.12f, IM_COL32(255, 255, 255, 34));
    k.rr(cx - 14, lcy + lry - 8, cx + 14, lcy + lry - 2, shade(edge, 0.8f), 3);   // open-tab notch
    k.rr(cx - rx * 0.5f, hinge - 3, cx + rx * 0.5f, hinge + 2, shade(edge, 0.7f), 2);   // hinge
}

void console_gc(K& k, float lid, float power) {
    k.shadow_ell(0, 96, 120, 13, 120);
    ImU32 f0 = rgb(100, 86, 178), f1 = rgb(62, 52, 124), top = rgb(122, 108, 204);
    // carry handle
    k.rr(-46, -100, 46, -62, rgb(72, 62, 140), 14); k.rr(-34, -92, 34, -68, rgb(20, 18, 34), 10);
    // top face + front face
    k.poly({{-104, -72}, {104, -72}, {120, 12}, {-120, 12}}, top);
    k.poly({{-104, -72}, {104, -72}, {108, -56}, {-108, -56}}, rgb(138, 124, 218));
    k.grad4(-120, 12, 120, 92, f0, f1, 0, 0, 10, 10);
    k.line(-120, 12, 120, 12, rgb(176, 164, 240), 1.5f);
    top_lid(k, 0, -28, 72, 33, -66, lid, rgb(150, 138, 226), rgb(86, 74, 160), rgb(16, 15, 24), rgb(70, 66, 90));
    // front: power, open, ports
    k.circ(-92, 34, 9, rgb(30, 26, 54)); k.circ(-92, 33, 7, rgb(190, 60, 60));
    k.rr(-70, 28, -40, 40, rgb(36, 30, 64), 5);
    for (int i = 0; i < 4; i++) { float x = -50.f + i * 36.f; k.rr(x - 13, 56, x + 13, 78, rgb(18, 16, 30), 4); k.rr(x - 9, 60, x + 9, 75, rgb(6, 6, 10), 3); }
    k.rr(76, 54, 104, 62, rgb(18, 16, 30), 2); k.rr(76, 68, 104, 76, rgb(18, 16, 30), 2);
    k.led(92, 34, 3.2f, power, rgb(120, 255, 110));
    k.gloss(-120, 12, 120, 92, 4, 24);
}

void console_ps1(K& k, float lid, float power) {
    k.shadow_ell(0, 92, 142, 13, 120);
    ImU32 top = rgb(172, 174, 178), f0 = rgb(150, 152, 156), f1 = rgb(110, 112, 118);
    k.poly({{-118, -80}, {118, -80}, {142, 8}, {-142, 8}}, top);
    k.poly({{-118, -80}, {118, -80}, {122, -66}, {-122, -66}}, rgb(196, 198, 202));
    k.grad4(-142, 8, 142, 86, f0, f1, 0, 0, 8, 8);
    k.line(-142, 8, 142, 8, rgb(220, 222, 226), 1.5f);
    for (int i = 0; i < 5; i++) { k.line(-100, -62 + i * 5.f, -70 + i * 3.f, -62 + i * 5.f, IM_COL32(0, 0, 0, 40), 1.5f); k.line(100, -62 + i * 5.f, 70 - i * 3.f, -62 + i * 5.f, IM_COL32(0, 0, 0, 40), 1.5f); }
    top_lid(k, 0, -34, 66, 31, -72, lid, rgb(188, 190, 194), rgb(120, 122, 128), rgb(24, 24, 28), rgb(80, 82, 90));
    // top-front buttons
    k.rr(-112, -6, -88, 4, rgb(120, 122, 130), 3); k.rr(88, -6, 112, 4, rgb(120, 122, 130), 3);
    k.circ(0, -2, 5, rgb(120, 122, 130));
    // front ports
    for (int i = 0; i < 2; i++) { float x = -112.f + i * 54.f; k.rr(x - 20, 36, x + 20, 62, rgb(40, 40, 46), 4); k.rr(x - 15, 41, x + 15, 58, rgb(8, 8, 10), 3); }
    for (int i = 0; i < 2; i++) { float x = 62.f + i * 52.f; k.rr(x - 18, 40, x + 18, 58, rgb(40, 40, 46), 3); k.rr(x - 13, 45, x + 13, 54, rgb(8, 8, 10), 2); }
    k.led(-20, 22, 3.2f, power, rgb(110, 255, 110));
    k.gloss(-142, 8, 142, 86, 4, 30);
}

void console_ps2(K& k, float lid, float power) {
    k.shadow_ell(0, 98, 140, 12, 130);
    float tray = c01(lid); tray = tray * tray * (3 - 2 * tray);
    ImU32 top = rgb(28, 30, 38), f0 = rgb(24, 25, 31), f1 = rgb(12, 12, 16);
    k.poly({{-120, -62}, {120, -62}, {142, 14}, {-142, 14}}, top);
    k.poly({{-120, -62}, {120, -62}, {126, -48}, {-126, -48}}, rgb(52, 56, 70));
    for (int i = 0; i < 7; i++) k.line(-90 + i * 30.f, -50, -100 + i * 33.f, 10, IM_COL32(120, 150, 255, 12), 6);
    k.ell(0, -22, 60, 24, rgb(18, 19, 25)); k.ellr(0, -22, 60, 24, rgb(60, 70, 110), 1.5f);   // top emblem ring
    k.ellr(0, -22, 38, 15, withA(rgb(90, 130, 255), 0.25f + 0.3f * power), 2.f);
    k.grad4(-142, 14, 142, 80, f0, f1, 0, 0, 8, 8);
    k.line(-142, 14, 142, 14, rgb(90, 98, 124), 1.5f);
    // tray bay
    k.rr(-84, 24, 62, 58, rgb(6, 6, 9), 5);
    k.rr(-78, 40, 56, 48, rgb(0, 0, 0), 2);
    if (tray > 0.01f) {   // the tray slides out toward the viewer and widens a little
        float ext = 96.f * tray, wy = 44.f + ext, w = 70.f + 14.f * tray;
        k.poly({{-w + 8, 42}, {w - 8 - 14, 42}, {w - 14, wy}, {-w, wy}}, rgb(10, 10, 14));
        k.poly({{-w + 8, 42}, {w - 8 - 14, 42}, {w - 14, wy}, {-w, wy}}, withA(rgb(70, 76, 100), 0.35f));
        k.rr(-w - 2, wy - 5, w - 12, wy + 4, rgb(36, 38, 50), 3);
        float cy = (42.f + wy) * 0.5f; k.ell(-7, cy + 2, 48 + 6 * tray, 14 + 14 * tray, rgb(2, 2, 4)); k.ellr(-7, cy + 2, 48 + 6 * tray, 14 + 14 * tray, rgb(70, 76, 100), 1.5f);
    }
    k.rr(86, 26, 112, 42, rgb(30, 32, 42), 4);   // power
    k.led(99, 34, 3.4f, power, rgb(60, 150, 255));
    for (int i = 0; i < 2; i++) { float x = 98.f + i * 0; (void)x; }
    k.rr(70, 52, 126, 66, rgb(6, 6, 9), 3);
    k.rr(-130, 52, -92, 66, rgb(6, 6, 9), 3); k.rr(-130, 28, -92, 42, rgb(6, 6, 9), 3);
}

void console_wiiu(K& k, float lid, float power) {
    k.shadow_ell(0, 92, 138, 12, 120);
    ImU32 top = rgb(226, 228, 236), f0 = rgb(238, 240, 246), f1 = rgb(186, 190, 202);
    k.poly({{-116, -66}, {116, -66}, {138, 12}, {-138, 12}}, top);
    k.poly({{-116, -66}, {116, -66}, {138, 12}, {-138, 12}}, withA(rgb(60, 70, 110), 0.1f));
    k.poly({{-116, -66}, {116, -66}, {122, -52}, {-122, -52}}, rgb(250, 250, 255));
    k.line(-128, -26, 128, -26, IM_COL32(0, 0, 0, 22), 1.5f);   // cut line between the two top shells
    k.poly({{-128, -26}, {128, -26}, {138, 12}, {-138, 12}}, rgb(212, 215, 224));
    k.grad4(-138, 12, 138, 82, f0, f1, 0, 0, 10, 10);
    k.line(-138, 12, 138, 12, rgb(255, 255, 255), 1.5f);
    // disc slot with a light strip below it
    float o = c01(lid);
    k.rr(-4, 30, 112, 40 + 4 * o, rgb(20, 20, 26), 3);
    k.rr(0, 33, 108, 36 + 3 * o, rgb(0, 0, 0), 2);
    k.rr(2, 46, 106, 52, mixc(rgb(150, 156, 176), rgb(70, 190, 255), c01(power * 0.7f + o * 0.3f)), 3);
    if (power > 0.02f || o > 0.02f) for (int i = 3; i >= 1; i--) k.rr(2 - i * 3, 46 - i * 2, 106 + i * 3, 52 + i * 2, withA(rgb(70, 190, 255), 0.07f * std::max(power, o)), 6);
    // power button, sensor, flap
    k.circ(-100, 40, 11, rgb(170, 174, 188)); k.circ(-100, 39, 9, rgb(236, 238, 246));
    k.led(-100, 40, 2.8f, power, rgb(70, 190, 255));
    k.rr(-70, 34, -30, 46, rgb(196, 200, 212), 3);
    k.rr(-70, 62, -30, 70, rgb(186, 190, 204), 2);
    k.gloss(-138, 12, 138, 82, 4, 70);
}

} // namespace

void draw_console(System s, ImDrawList* d, ImVec2 center, float scale, float lid, float power) {
    K k{d, center, scale};
    lid = c01(lid); power = c01(power);
    switch (s) {
    case System::GB: gb_handheld(k, false, power); break;
    case System::GBC: gb_handheld(k, true, power); break;
    case System::GBA: console_gba(k, lid, power); break;
    case System::NDS: console_ds(k, false, lid, power); break;
    case System::N3DS: console_ds(k, true, lid, power); break;
    case System::GC: console_gc(k, lid, power); break;
    case System::PS1: console_ps1(k, lid, power); break;
    case System::PS2: console_ps2(k, lid, power); break;
    case System::WiiU: console_wiiu(k, lid, power); break;
    case System::N64: case System::Unknown: default: console_n64(k, lid, power); break;
    }
}

/* ---------- media ---------- */
namespace {
void sticker(K& k, float x0, float y0, float x1, float y1, ImTextureID tex, ImVec2 uv0, ImVec2 uv1, ImU32 fb, float r = 3) {
    k.rr(x0 - 5, y0 - 5, x1 + 5, y1 + 5, IM_COL32(0, 0, 0, 70), r + 3);
    k.rr(x0 - 2, y0 - 2, x1 + 2, y1 + 2, rgb(236, 236, 240), r + 1);
    if (tex) k.d->AddImageRounded(tex, k.p(x0, y0), k.p(x1, y1), uv0, uv1, IM_COL32_WHITE, r * k.s);
    else {
        k.grad(x0, y0, x1, y1, shade(fb, 1.25f), shade(fb, 0.7f), r);
        k.circ((x0 + x1) / 2, (y0 + y1) / 2, std::min(x1 - x0, y1 - y0) * 0.22f, IM_COL32(255, 255, 255, 60));
        k.rr(x0 + 6, y1 - 14, x1 - 6, y1 - 6, IM_COL32(255, 255, 255, 70), 2);
    }
    k.rr(x0, y0, x1, y0 + 3, IM_COL32(255, 255, 255, 50), r);   // sheen
}
void poly_shadow(K& k, std::initializer_list<ImVec2> pts, int alpha) {
    for (int i = 3; i >= 1; i--) {
        ImVec2 q[16]; int n = 0; for (auto& v : pts) if (n < 16) q[n++] = k.p(v.x + i * 1.2f, v.y + 4 + i * 2.2f);
        k.d->AddConvexPolyFilled(q, n, IM_COL32(0, 0, 0, alpha / 3));
    }
}

void media_n64(K& k, ImTextureID t, ImVec2 u0, ImVec2 u1) {
    k.shadow(-95, -80, 95, 66, 8, 14, 110);
    k.grad4(-95, -80, 95, 50, rgb(128, 130, 138), rgb(88, 90, 98), 8, 8, 4, 4);
    for (int i = 0; i < 12; i++) k.line(-80 + i * 14.5f, -76, -80 + i * 14.5f, -54, IM_COL32(0, 0, 0, 55), 2.f);   // grip ridges
    k.line(-90, -52, 90, -52, IM_COL32(255, 255, 255, 40), 1.5f);
    sticker(k, -72, -44, 72, 34, t, u0, u1, rgb(200, 120, 60));
    k.rr4(-70, 50, 70, 68, rgb(46, 47, 52), 0, 0, 4, 4);
    for (int i = 0; i < 16; i++) k.rr(-64 + i * 8.f, 54, -60 + i * 8.f, 66, rgb(200, 168, 80), 0);   // gold contacts
    k.rr4(-95, -80, 95, -76, IM_COL32(255, 255, 255, 50), 8, 8, 0, 0);
    k.rr(-10, 46, 10, 52, rgb(30, 30, 34), 2);
    k.line(-95, 48, -95, -70, IM_COL32(255, 255, 255, 40), 1.5f);
}

enum class Cut { None, TopRight, TopLeft };
void media_gb(K& k, System s, ImTextureID t, ImVec2 u0, ImVec2 u1) {
    bool gba = s == System::GBA, gbc = s == System::GBC;
    float hw = gba ? 56.f : 72.f, top = gba ? -88.f : -80.f, bot = gba ? 86.f : 70.f;
    ImU32 base = gbc ? rgb(54, 52, 70) : (gba ? rgb(150, 152, 164) : rgb(170, 170, 166));
    float cut = gbc ? 24.f : (gba ? 14.f : 0.f);
    bool cutRight = gbc, cutLeft = gba;
    ImVec2 pts[] = {{-hw + (cutLeft ? cut : 0), top}, {hw - (cutRight ? cut : 0), top}, {hw, top + (cutRight ? cut : 0)}, {hw, bot - 14}, {hw - 12, bot}, {-hw + 12, bot}, {-hw, bot - 14}, {-hw, top + (cutLeft ? cut : 0)}};
    poly_shadow(k, {pts[0], pts[1], pts[2], pts[3], pts[4], pts[5], pts[6], pts[7]}, 110);
    ImVec2 q[8]; for (int i = 0; i < 8; i++) q[i] = k.p(pts[i].x, pts[i].y);
    k.d->AddConvexPolyFilled(q, 8, base);
    k.rr4(-hw + 3, top + 3 + (gba ? 0 : 0), hw - 3, top + 26, IM_COL32(0, 0, 0, 40), 2, 2, 0, 0);
    for (int i = 0; i < 5; i++) k.line(-hw + 12, top + 6 + i * 4.f, hw - 12, top + 6 + i * 4.f, IM_COL32(0, 0, 0, 70), 2);   // grip ridges
    float lw = hw - 16;
    if (gba) sticker(k, -lw, top + 34, lw, top + 118, t, u0, u1, rgb(80, 120, 200));
    else sticker(k, -lw, top + 38, lw, top + 106, t, u0, u1, rgb(120, 160, 70));
    float cy0 = bot - 2;
    k.rr(-hw + 14, cy0, hw - 14, cy0 + 14, rgb(36, 36, 42), 2);
    for (int i = 0; i < (gba ? 9 : 12); i++) k.rr(-hw + 18 + i * ((2 * hw - 36) / (gba ? 9 : 12)), cy0 + 2, -hw + 21 + i * ((2 * hw - 36) / (gba ? 9 : 12)), cy0 + 12, rgb(210, 176, 84), 0);
    k.d->AddPolyline(q, 8, IM_COL32(255, 255, 255, 45), ImDrawFlags_Closed, 1.2f);
    k.rr(-hw + 4, top + 28, -hw + 7, bot - 18, IM_COL32(255, 255, 255, 40), 1);
}

void media_card(K& k, bool is3, ImTextureID t, ImVec2 u0, ImVec2 u1) {
    float hw = 54.f, top = -60.f, bot = 60.f, cut = 20.f;
    ImU32 base = is3 ? rgb(40, 42, 54) : rgb(160, 166, 182);
    ImVec2 pts[] = {{is3 ? -hw + cut : -hw, top}, {is3 ? hw : hw - cut, top}, {hw, is3 ? top : top + cut}, {hw, bot - 8}, {hw - 8, bot}, {-hw + 8, bot}, {-hw, bot - 8}, {-hw, is3 ? top : top + 0.f}};
    if (is3) pts[7] = {-hw, top + cut}; else pts[0] = {-hw, top};
    poly_shadow(k, {pts[0], pts[1], pts[2], pts[3], pts[4], pts[5], pts[6], pts[7]}, 110);
    ImVec2 q[8]; for (int i = 0; i < 8; i++) q[i] = k.p(pts[i].x, pts[i].y);
    k.d->AddConvexPolyFilled(q, 8, base);
    if (is3) k.rr4(-hw + 26, top, hw, top + 8, IM_COL32(255, 255, 255, 18), 0, 0, 0, 0);
    for (int i = 0; i < 4; i++) k.line(is3 ? 20.f : -hw + 8, top + 5 + i * 4.f, is3 ? hw - 4 : -hw + 30 + 0.f, top + 5 + i * 4.f, IM_COL32(0, 0, 0, 80), 2);
    sticker(k, -38, top + 26, 38, top + 90, t, u0, u1, is3 ? rgb(60, 180, 200) : rgb(220, 130, 60), 2);
    for (int i = 0; i < 9; i++) k.rr(-34 + i * 8.f, bot - 12, -30 + i * 8.f, bot - 2, rgb(206, 172, 84), 0);
    k.d->AddPolyline(q, 8, IM_COL32(255, 255, 255, is3 ? 30 : 60), ImDrawFlags_Closed, 1.2f);
}

void disc_annulus(ImDrawList* d, ImVec2 c, float r0, float r1, float rot, ImTextureID tex, ImVec2 uv0, ImVec2 uv1, ImU32 col, float R) {
    const int n = 96;
    ImVec2 mid((uv0.x + uv1.x) * 0.5f, (uv0.y + uv1.y) * 0.5f), half((uv1.x - uv0.x) * 0.5f, (uv1.y - uv0.y) * 0.5f);
    d->PushTextureID(tex);
    d->PrimReserve(n * 6, n * 4);
    for (int i = 0; i < n; i++) {
        float a0 = 2 * PI * i / n, a1 = 2 * PI * (i + 1) / n;
        auto P = [&](float a, float r) { return ImVec2(c.x + std::cos(a + rot) * r, c.y + std::sin(a + rot) * r); };
        auto U = [&](float a, float r) { return ImVec2(mid.x + std::cos(a) * (r / R) * half.x, mid.y + std::sin(a) * (r / R) * half.y); };
        d->PrimQuadUV(P(a0, r0), P(a0, r1), P(a1, r1), P(a1, r0), U(a0, r0), U(a0, r1), U(a1, r1), U(a1, r0), col);
    }
    d->PopTextureID();
}

void media_disc(K& k, System s, float R, float rot, ImTextureID t, ImVec2 u0, ImVec2 u1) {
    ImDrawList* d = k.d; ImVec2 c = k.c; float sR = R * k.s;
    ImU32 face, rim, tint;
    if (s == System::PS2) { face = rgb(24, 40, 96); rim = rgb(150, 170, 220); tint = rgb(40, 70, 150); }
    else if (s == System::WiiU) { face = rgb(214, 226, 240); rim = rgb(235, 245, 255); tint = rgb(130, 190, 235); }
    else if (s == System::GC) { face = rgb(40, 42, 52); rim = rgb(176, 178, 190); tint = rgb(110, 110, 150); }
    else { face = rgb(48, 50, 56); rim = rgb(190, 192, 198); tint = rgb(150, 150, 160); }
    for (int i = 4; i >= 1; i--) d->AddCircleFilled(ImVec2(c.x + 3 * k.s, c.y + (6 + i * 2.5f) * k.s), sR + i * 2.5f * k.s, IM_COL32(0, 0, 0, 24), 48);
    d->AddCircleFilled(c, sR, rim, 64);
    d->AddCircleFilled(c, sR * 0.975f, face, 64);
    float rin = sR * 0.30f, rout = sR * 0.96f;
    if (t) disc_annulus(d, c, rin, rout, rot, t, u0, u1, IM_COL32_WHITE, sR);
    else {
        d->AddCircleFilled(c, rout, tint, 64);
        d->AddCircleFilled(c, rout * 0.62f, face, 64);
        for (int i = 0; i < 6; i++) { float a = rot + i * PI / 3; d->AddLine(ImVec2(c.x + std::cos(a) * rin, c.y + std::sin(a) * rin), ImVec2(c.x + std::cos(a) * rout, c.y + std::sin(a) * rout), IM_COL32(255, 255, 255, 40), 3 * k.s); }
    }
    d->AddCircle(c, rin, IM_COL32(0, 0, 0, 90), 48, std::max(1.f, 1.5f * k.s));
    // clear hub, spindle hole (true hole: nothing drawn over the middle)
    
    d->AddCircleFilled(c, rin * 0.98f, IM_COL32(190, 196, 210, 150), 40);
    d->AddCircle(c, rin * 0.62f, IM_COL32(255, 255, 255, 90), 32, std::max(1.f, k.s));
    d->AddCircleFilled(c, rin * 0.32f, IM_COL32(30, 32, 40, 255), 24);
    d->AddCircle(c, rin * 0.32f, IM_COL32(255, 255, 255, 120), 24, std::max(1.f, k.s));
    d->AddCircle(c, sR * 0.975f, IM_COL32(255, 255, 255, 70), 64, std::max(1.f, 1.5f * k.s));
    // fixed specular wedges (light does not rotate with the disc)
    for (int w = 0; w < 2; w++) {
        float a = (w ? 0.6f : 0.6f + PI) + 0.0f, da = 0.28f;
        ImVec2 q[4] = {ImVec2(c.x + std::cos(a - da) * rin, c.y + std::sin(a - da) * rin), ImVec2(c.x + std::cos(a - da) * rout, c.y + std::sin(a - da) * rout),
                       ImVec2(c.x + std::cos(a + da) * rout, c.y + std::sin(a + da) * rout), ImVec2(c.x + std::cos(a + da) * rin, c.y + std::sin(a + da) * rin)};
        d->AddConvexPolyFilled(q, 4, IM_COL32(255, 255, 255, 30));
        ImVec2 q2[4] = {ImVec2(c.x + std::cos(a - da * 0.4f) * rin, c.y + std::sin(a - da * 0.4f) * rin), ImVec2(c.x + std::cos(a - da * 0.4f) * rout, c.y + std::sin(a - da * 0.4f) * rout),
                        ImVec2(c.x + std::cos(a + da * 0.4f) * rout, c.y + std::sin(a + da * 0.4f) * rout), ImVec2(c.x + std::cos(a + da * 0.4f) * rin, c.y + std::sin(a + da * 0.4f) * rin)};
        d->AddConvexPolyFilled(q2, 4, IM_COL32(255, 255, 255, 36));
    }
}
} // namespace

void draw_media(System s, ImDrawList* d, ImVec2 center, float scale, float rot, ImTextureID label, ImVec2 u0, ImVec2 u1) {
    K k{d, center, scale};
    switch (s) {
    case System::GB: case System::GBC: case System::GBA: media_gb(k, s, label, u0, u1); break;
    case System::NDS: media_card(k, false, label, u0, u1); break;
    case System::N3DS: media_card(k, true, label, u0, u1); break;
    case System::GC: media_disc(k, s, 80.f, rot, label, u0, u1); break;
    case System::PS1: case System::PS2: case System::WiiU: media_disc(k, s, 100.f, rot, label, u0, u1); break;
    case System::N64: case System::Unknown: default: media_n64(k, label, u0, u1); break;
    }
}

/* ---------- shelf ---------- */
namespace {
std::vector<ImTextureID> g_covers;
}
void set_shelf_covers(const ImTextureID* tex, int n) { g_covers.assign(tex, tex + (tex ? std::max(n, 0) : 0)); }

namespace {
ImU32 hsv(float h, float s, float v, int a = 255) { float r, g, b; ImGui::ColorConvertHSVtoRGB(h, s, v, r, g, b); return IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), a); }

void shelf_row(ImDrawList* d, ImVec2 mn, ImVec2 mx, float board_y, float u, uint32_t seed, float t) {
    float thick = 20 * u;
    // back shadow under the row, then the objects standing on the board
    float x = mn.x + 14 * u; int idx = 0;
    while (x < mx.x - 30 * u) {
        uint32_t h = hsh(seed * 977 + idx * 131);
        int kind = h % 10;   // 0-5 box fronts, 6-8 spines, 9 cartridge
        float hue = hf(h + 7), sat = 0.45f + 0.35f * hf(h + 3), val = 0.42f + 0.33f * hf(h + 5);
        ImU32 c = hsv(hue, sat, val);
        float w, hgt;
        if (kind <= 5) { w = (46 + 22 * hf(h + 1)) * u; hgt = w * (1.28f + 0.1f * hf(h + 2)); }
        else if (kind <= 8) { w = (15 + 7 * hf(h + 1)) * u; hgt = (92 + 30 * hf(h + 2)) * u; }
        else { w = 40 * u; hgt = 46 * u; }
        float y1 = board_y, y0 = y1 - hgt, x1 = x + w;
        for (int i = 3; i >= 1; i--) d->AddRectFilled(ImVec2(x - i * u, y0 + i * u), ImVec2(x1 + i * 3 * u, y1), IM_COL32(0, 0, 0, 18), 2 * u);
        if (kind == 9) {   // loose cartridge
            d->AddRectFilled(ImVec2(x, y0), ImVec2(x1, y1), rgb(112, 114, 122), 3 * u);
            d->AddRectFilled(ImVec2(x + 5 * u, y0 + 12 * u), ImVec2(x1 - 5 * u, y1 - 6 * u), c, 2 * u);
            d->AddRectFilled(ImVec2(x + 3 * u, y0 + 2 * u), ImVec2(x1 - 3 * u, y0 + 8 * u), IM_COL32(0, 0, 0, 60), 1);
        } else {
            ImU32 lt = shade(c, 1.25f), dk = shade(c, 0.62f);
            d->AddRectFilledMultiColor(ImVec2(x, y0), ImVec2(x1, y1), lt, lt, dk, dk);
            d->AddRect(ImVec2(x, y0), ImVec2(x1, y1), IM_COL32(0, 0, 0, 90), 1.5f * u);
            bool useCover = !g_covers.empty() && kind <= 5 && (idx % 3) == 0;
            if (useCover) d->AddImage(g_covers[(idx / 3) % g_covers.size()], ImVec2(x + 2 * u, y0 + 2 * u), ImVec2(x1 - 2 * u, y1 - 2 * u));
            else if (kind <= 5) {
                d->AddRectFilled(ImVec2(x, y0), ImVec2(x1, y0 + hgt * 0.13f), IM_COL32(10, 10, 14, 150));
                for (int i = 0; i < 3; i++) d->AddRectFilled(ImVec2(x + w * 0.12f, y0 + hgt * 0.04f + i * 2.4f * u), ImVec2(x + w * (0.4f + 0.14f * i), y0 + hgt * 0.04f + i * 2.4f * u + 1.2f * u), IM_COL32(255, 255, 255, 140));
                ImVec2 cc(x + w * 0.5f, y0 + hgt * 0.5f); float rr = w * 0.26f; ImU32 em = hsv(fmodf(hue + 0.12f + 0.3f * hf(h + 9), 1.f), 0.5f, 0.95f);
                int shape = (h >> 8) % 3;
                if (shape == 0) d->AddCircleFilled(cc, rr, em, 24);
                else if (shape == 1) d->AddQuadFilled(ImVec2(cc.x, cc.y - rr * 1.2f), ImVec2(cc.x + rr, cc.y), ImVec2(cc.x, cc.y + rr * 1.2f), ImVec2(cc.x - rr, cc.y), em);
                else d->AddTriangleFilled(ImVec2(cc.x, cc.y - rr), ImVec2(cc.x + rr * 1.1f, cc.y + rr), ImVec2(cc.x - rr * 1.1f, cc.y + rr), em);
                d->AddRectFilled(ImVec2(x, y1 - hgt * 0.1f), ImVec2(x1, y1), IM_COL32(0, 0, 0, 70));
            } else {   // spine: stripes and a label strip
                d->AddRectFilled(ImVec2(x, y0 + hgt * 0.06f), ImVec2(x1, y0 + hgt * 0.16f), IM_COL32(255, 255, 255, 170));
                d->AddRectFilled(ImVec2(x + w * 0.25f, y0 + hgt * 0.28f), ImVec2(x1 - w * 0.25f, y1 - hgt * 0.12f), IM_COL32(0, 0, 0, 80), 1);
                d->AddRectFilled(ImVec2(x, y1 - hgt * 0.08f), ImVec2(x1, y1), IM_COL32(0, 0, 0, 80));
            }
            d->AddRectFilled(ImVec2(x, y0), ImVec2(x + 2.5f * u, y1), IM_COL32(255, 255, 255, 50));
            d->AddRectFilled(ImVec2(x1 - 3 * u, y0), ImVec2(x1, y1), IM_COL32(0, 0, 0, 60));
        }
        x = x1 + (3 + 5 * hf(h + 11)) * u;
        if (kind <= 5 && hf(h + 13) < 0.12f) x += 24 * u;   // gaps
        idx++;
    }
    // board
    float bx0 = mn.x - 4 * u, bx1 = mx.x + 4 * u;
    d->AddRectFilledMultiColor(ImVec2(bx0, board_y), ImVec2(bx1, board_y + thick), rgb(132, 92, 58), rgb(132, 92, 58), rgb(84, 56, 36), rgb(84, 56, 36));
    d->AddRectFilled(ImVec2(bx0, board_y), ImVec2(bx1, board_y + 3 * u), IM_COL32(255, 220, 170, 90));
    for (int i = 0; i < 26; i++) { float gx = bx0 + (bx1 - bx0) * hf(seed * 31 + i), gy = board_y + 5 * u + 12 * u * hf(seed * 17 + i); d->AddLine(ImVec2(gx, gy), ImVec2(gx + (30 + 80 * hf(i + seed)) * u, gy + (hf(i * 3 + seed) - 0.5f) * 2 * u), IM_COL32(40, 22, 10, 55), u); }
    d->AddRectFilled(ImVec2(bx0, board_y + thick), ImVec2(bx1, board_y + thick + 22 * u), IM_COL32(0, 0, 0, 70));
    d->AddRectFilledMultiColor(ImVec2(bx0, board_y + thick), ImVec2(bx1, board_y + thick + 40 * u), IM_COL32(0, 0, 0, 90), IM_COL32(0, 0, 0, 90), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
    (void)t;
}
} // namespace

void draw_shelf(ImDrawList* d, ImVec2 mn, ImVec2 mx, float t) {
    float h = mx.y - mn.y, u = std::max(0.3f, h / 800.f);
    d->PushClipRect(mn, mx, true);
    d->AddRectFilledMultiColor(mn, mx, rgb(18, 19, 26), rgb(18, 19, 26), rgb(30, 25, 28), rgb(30, 25, 28));
    for (int i = 6; i >= 1; i--) d->AddCircleFilled(ImVec2((mn.x + mx.x) * 0.5f, mn.y - 40 * u), (mx.x - mn.x) * (0.1f + 0.09f * i), IM_COL32(255, 220, 170, 5), 48);   // warm light from above
    shelf_row(d, mn, mx, mn.y + h * 0.43f, u, 1, t);
    shelf_row(d, mn, mx, mn.y + h * 0.86f, u, 2, t);
    // slow sweeping sheen
    float sw = std::fmod(t * 0.07f, 1.4f) - 0.2f, sx = mn.x + (mx.x - mn.x) * sw;
    d->AddRectFilledMultiColor(ImVec2(sx - 80 * u, mn.y), ImVec2(sx, mx.y), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 9), IM_COL32(255, 255, 255, 9), IM_COL32(255, 255, 255, 0));
    d->AddRectFilledMultiColor(ImVec2(sx, mn.y), ImVec2(sx + 80 * u, mx.y), IM_COL32(255, 255, 255, 9), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 9));
    d->AddRectFilledMultiColor(mn, ImVec2(mx.x, mn.y + h * 0.15f), IM_COL32(0, 0, 0, 120), IM_COL32(0, 0, 0, 120), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
    d->PopClipRect();
}

/* ---------- names + gallery ---------- */
const char* art_name(System s) {
    switch (s) {
    case System::N64: return "n64"; case System::GB: return "gb"; case System::GBC: return "gbc"; case System::GBA: return "gba";
    case System::NDS: return "ds"; case System::N3DS: return "3ds"; case System::GC: return "gc"; case System::PS1: return "ps1";
    case System::PS2: return "ps2"; case System::WiiU: return "wiiu"; default: return "n64";
    }
}
bool art_dedicated(System s) { return s != System::Unknown; }
System art_system_from_name(const char* n) {
    if (!n) return System::Unknown;
    std::string l; for (const char* p = n; *p; p++) l += (char)std::tolower((unsigned char)*p);
    for (System s : {System::N64, System::GB, System::GBC, System::GBA, System::NDS, System::N3DS, System::GC, System::PS1, System::PS2, System::WiiU})
        if (l == art_name(s)) return s;
    return System::Unknown;
}

void draw_gallery(ImDrawList* d, ImVec2 mn, ImVec2 mx, const char* which, ImTextureID cover) {
    float W = mx.x - mn.x, H = mx.y - mn.y;
    d->AddRectFilled(mn, mx, rgb(16, 17, 22));
    std::string w = which ? which : "all";
    for (auto& c : w) c = (char)std::tolower((unsigned char)c);
    if (w == "shelf") { draw_shelf(d, mn, mx, 1.f); return; }
    const System all[] = {System::N64, System::GB, System::GBC, System::GBA, System::NDS, System::N3DS, System::GC, System::PS1, System::PS2, System::WiiU};
    System one = art_system_from_name(w.c_str());
    ImVec2 uv0(0, 0), uv1(1, 1);
    if (one == System::Unknown) {   // grid of everything
        int cols = 5, rows = 2; float cw = W / cols, ch = H / rows;
        for (int i = 0; i < 10; i++) {
            System s = all[i]; ImVec2 c0(mn.x + (i % cols) * cw, mn.y + (i / cols) * ch);
            float sc = std::min(cw / 330.f, ch / 480.f);
            d->AddRect(ImVec2(c0.x + 4, c0.y + 4), ImVec2(c0.x + cw - 4, c0.y + ch - 4), IM_COL32(255, 255, 255, 18), 8);
            bool tall = s == System::GB || s == System::GBC || s == System::NDS || s == System::N3DS;
            float cs = sc * (tall ? 0.78f : 1.0f);
            draw_console(s, d, ImVec2(c0.x + cw * 0.5f, c0.y + ch * (tall ? 0.36f : 0.28f)), cs, 0.8f, 1.f);
            draw_media(s, d, ImVec2(c0.x + cw * 0.5f, c0.y + ch * 0.82f), sc * 0.8f, 0.4f, cover, uv0, uv1);
        }
        return;
    }
    // single system: consoles at three states on top, media (two rotations) + shelf strip below
    bool tall = one == System::GB || one == System::GBC || one == System::NDS || one == System::N3DS;
    float sc = std::min(W / 3.f / 330.f, H * 0.5f / (tall ? 330.f : 200.f));
    float st[3][2] = {{0.f, 0.f}, {0.5f, 0.5f}, {1.f, 1.f}};
    for (int i = 0; i < 3; i++) draw_console(one, d, ImVec2(mn.x + W * (0.17f + 0.33f * i), mn.y + H * 0.30f), sc, st[i][0], st[i][1]);
    draw_media(one, d, ImVec2(mn.x + W * 0.17f, mn.y + H * 0.76f), sc * 0.9f, 0.f, cover, uv0, uv1);
    draw_media(one, d, ImVec2(mn.x + W * 0.50f, mn.y + H * 0.76f), sc * 0.9f, 0.7f, (ImTextureID)0, uv0, uv1);
    draw_shelf(d, ImVec2(mn.x + W * 0.68f, mn.y + H * 0.58f), ImVec2(mx.x - 10, mx.y - 10), 3.f);
}

} // namespace bootanim
