/* Game start animation: hand-built vector art (ImDrawList only) for every console, its media and the shelf.
 * Original stylised shapes: no logos, no brand names, no textures except the `label` cover art. */
#include "boot_art.h"
#include "imgui_internal.h"
#include <cmath>
#include <cstring>
#include <cctype>
#include <vector>
#include <string>
#include <utility>
#include <functional>
#include <algorithm>

namespace bootanim {
using hub::System;

namespace {
std::function<void(ImDrawList*)> g_inner;   // drawn inside a console between its well/tray and its lid (disc media)
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
    const float m = std::max(0.f, std::min(b.x - a.x, b.y - a.y) * 0.5f);   // radii never exceed half the short side (no folded paths)
    tl = std::min(tl, m); tr = std::min(tr, m); br = std::min(br, m); bl = std::min(bl, m);
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
    /* Quad with a colour per corner (a=top-left, b=top-right, c=bottom-right, e=bottom-left), local coords. */
    void quadg(ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 e, ImU32 ca, ImU32 cb, ImU32 cc, ImU32 ce) {
        ImVec2 P[4] = {p(a.x, a.y), p(b.x, b.y), p(c.x, c.y), p(e.x, e.y)};
        ImU32 C[4] = {ca, cb, cc, ce};
        const ImVec2 uv = d->_Data->TexUvWhitePixel;
        d->PrimReserve(6, 4);
        const ImDrawIdx i0 = (ImDrawIdx)d->_VtxCurrentIdx;
        for (int i = 0; i < 4; i++) d->PrimWriteVtx(P[i], uv, C[i]);
        d->PrimWriteIdx(i0); d->PrimWriteIdx((ImDrawIdx)(i0 + 1)); d->PrimWriteIdx((ImDrawIdx)(i0 + 2));
        d->PrimWriteIdx(i0); d->PrimWriteIdx((ImDrawIdx)(i0 + 2)); d->PrimWriteIdx((ImDrawIdx)(i0 + 3));
    }
    void vquad(ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 e, ImU32 top, ImU32 bot) { quadg(a, b, c, e, top, top, bot, bot); }
    /* recessed opening: pale lower lip, dark body, shadow band under the top edge */
    void well(float x0, float y0, float x1, float y1, float r, ImU32 deep, int lip = 60) {
        rr(x0 - 1.2f, y0 - 0.5f, x1 + 1.2f, y1 + 1.8f, IM_COL32(255, 255, 255, lip), r + 1);
        rr(x0, y0, x1, y1, deep, r);
        rr(x0, y0, x1, y0 + std::max(2.f, (y1 - y0) * 0.22f), IM_COL32(0, 0, 0, 120), r);
    }
    void screw(float x, float y, float r, ImU32 c) { circ(x, y + 0.6f, r, IM_COL32(255, 255, 255, 40)); circ(x, y, r, c); line(x - r * 0.6f, y, x + r * 0.6f, y, IM_COL32(0, 0, 0, 120), 1.f); }
    /* round-ish button with a lit top edge and a dark underside */
    void button(float x0, float y0, float x1, float y1, ImU32 c, float r) {
        rr(x0, y0 + 1.5f, x1, y1 + 1.5f, IM_COL32(0, 0, 0, 110), r);
        grad(x0, y0, x1, y1, shade(c, 1.2f), shade(c, 0.8f), r);
        rr(x0 + 1.5f, y0 + 0.8f, x1 - 1.5f, y0 + 2.4f, IM_COL32(255, 255, 255, 70), r);
    }
    void gloss(float x0, float y0, float x1, float y1, float r, int a = 38) { rr(x0 + 2, y0 + 1.5f, x1 - 2, y0 + 1.5f + std::max(2.f, (y1 - y0) * 0.12f), IM_COL32(255, 255, 255, a), r); }
};

/* ---------- consoles ---------- */

/* Round top lid hinged at its back edge, seen from in front and above. `hinge` is the y of the hinge line (back of the closed lid).
 * The lid foreshortens, goes edge-on near 30 degrees, then shows its underside as it swings past vertical. */
void hinge_lid(K& k, float cx, float hinge, float R, float open, ImU32 top, ImU32 under, ImU32 rim, float thick, ImU32 dish) {
    const float ke = 0.5f, hh = 0.866f;
    float o = c01(open); o = o * o * (3 - 2 * o);
    const float th = o * 1.85f;
    const float s = ke * std::cos(th) - hh * std::sin(th);
    const float cy = hinge + R * s, ry = std::max(1.5f, R * std::fabs(s));
    const float rimoff = thick * std::cos(th);
    k.ell(cx, cy + rimoff * 0.5f + 3, R + 2, ry + 1, IM_COL32(0, 0, 0, 55));            // contact shadow
    k.ell(cx, cy + rimoff, R, ry, shade(rim, 0.75f));                                   // thickness
    k.ell(cx, cy, R, ry, rim);
    if (s > 0) {
        k.ell(cx, cy - 0.5f, R * 0.965f, ry * 0.955f, top);
        k.ell(cx, cy - 1.f, R * 0.74f, ry * 0.72f, dish);                               // shallow dish
        k.ellr(cx, cy - 1.f, R * 0.74f, ry * 0.72f, IM_COL32(255, 255, 255, 40), 1.4f);
        k.ell(cx - R * 0.3f, cy - ry * 0.5f, R * 0.32f, ry * 0.1f, IM_COL32(255, 255, 255, 38));
    } else {
        k.ell(cx, cy, R * 0.955f, ry * 0.94f, under);
        k.ell(cx, cy, R * 0.28f, ry * 0.28f, shade(under, 0.7f));
        k.ellr(cx, cy, R * 0.62f, ry * 0.6f, IM_COL32(255, 255, 255, 22), 1.2f);
    }
}

void well_with_disc_slot(K& k, float cx, float cy, float rx, float ry, ImU32 deep, ImU32 hub) {
    k.ell(cx, cy + 1.5f, rx + 2, ry + 1.5f, IM_COL32(255, 255, 255, 40));
    k.ell(cx, cy, rx, ry, deep);
    k.ell(cx, cy - ry * 0.12f, rx * 0.93f, ry * 0.85f, shade(deep, 0.6f));
    k.ell(cx, cy, rx * 0.17f, ry * 0.17f, hub);
    k.ell(cx, cy, rx * 0.06f, ry * 0.06f, rgb(14, 14, 18));
}

void console_n64(K& k, float lid, float power) {
    (void)lid;
    k.shadow_ell(0, 86, 140, 12, 140);
    const ImU32 c0 = rgb(64, 66, 72), c1 = rgb(92, 94, 101);
    // top face (back is narrower: perspective), then the raised rear block that holds the cartridge slot
    k.vquad({-120, -70}, {120, -70}, {138, 26}, {-138, 26}, shade(c0, 0.9f), c1);
    k.quadg({-138, 26}, {-120, -70}, {-124, -70}, {-142, 26}, rgb(40, 41, 46), rgb(40, 41, 46), rgb(30, 31, 35), rgb(30, 31, 35));   // side shadow
    k.line(-120, -70, 120, -70, rgb(120, 123, 130), 1.5f);
    k.line(-120, -70, -138, 26, IM_COL32(255, 255, 255, 36), 1.2f); k.line(120, -70, 138, 26, IM_COL32(255, 255, 255, 36), 1.2f);
    // raised bezel around the slot
    k.vquad({-60, -66}, {60, -66}, {66, -4}, {-66, -4}, rgb(98, 100, 108), rgb(116, 118, 126));
    k.vquad({-66, -4}, {66, -4}, {68, 6}, {-68, 6}, rgb(46, 47, 52), rgb(28, 29, 33));     // vertical lip face
    k.line(-60, -66, 60, -66, IM_COL32(255, 255, 255, 60), 1.f);
    // the slot: black well, rails, gold edge connector at the bottom (sized for the real cart: about a quarter of the console width)
    k.quadg({-47, -60}, {47, -60}, {52, -12}, {-52, -12}, rgb(2, 2, 3), rgb(2, 2, 3), rgb(20, 21, 25), rgb(20, 21, 25));
    k.line(-47, -60, -52, -12, rgb(120, 123, 132), 1.5f); k.line(47, -60, 52, -12, rgb(120, 123, 132), 1.5f);
    k.line(-28, -58, -31, -14, rgb(44, 46, 52), 1.f); k.line(28, -58, 31, -14, rgb(44, 46, 52), 1.f);
    for (int i = 0; i < 16; i++) k.rr(-32 + i * 4.2f, -26, -30 + i * 4.2f, -14, rgb(150, 124, 56), 0);
    for (int i = 0; i < 4; i++) k.screw(i < 2 ? -56.f + i * 112.f : -62.f + (i - 2) * 124.f, i < 2 ? -62.f : -8.f, 2.4f, rgb(30, 31, 35));
    if (g_inner) g_inner(k.d);                                                           // the cartridge sits here
    // front spring flap + expansion-pak cover, in front of the cartridge
    k.rr(-52, -14, 52, -7, rgb(18, 19, 22), 2); k.rr(-52, -14, 52, -12, IM_COL32(255, 255, 255, 70), 1);
    for (int i = 0; i < 4; i++) k.rr(-46 + i * 25.f, -10, -30 + i * 25.f, -8, rgb(60, 62, 68), 1);   // flap fingers
    k.well(-60, 4, 60, 22, 4, rgb(22, 23, 27), 50);
    k.grad(-56, 7, 56, 20, rgb(74, 76, 83), rgb(58, 60, 66), 3);
    k.rr(-22, 7, 22, 12, rgb(38, 39, 44), 2);                                           // finger notch
    // power slider (left) and reset (right)
    k.well(-120, 8, -80, 20, 3, rgb(14, 14, 17), 50);
    k.button(-104, 6, -88, 22, rgb(150, 152, 160), 3);
    for (int i = 0; i < 3; i++) k.line(-100 + i * 4.f, 9, -100 + i * 4.f, 19, IM_COL32(0, 0, 0, 80), 1.f);
    k.button(86, 8, 118, 20, rgb(120, 122, 130), 4);
    k.led(-72, 14, 2.8f, power, rgb(255, 60, 40));
    // front face with the four controller ports, each under its colour mark
    k.vquad({-138, 26}, {138, 26}, {138, 80}, {-138, 80}, rgb(64, 66, 72), rgb(36, 37, 41));
    k.rr4(-138, 78, 138, 86, rgb(20, 21, 24), 0, 0, 5, 5);
    k.line(-138, 26, 138, 26, rgb(150, 153, 160), 1.5f);
    k.rr(-140, 26, -138, 80, IM_COL32(255, 255, 255, 25));
    ImU32 pc[4] = {rgb(60, 110, 230), rgb(60, 190, 90), rgb(240, 200, 50), rgb(230, 70, 60)};
    for (int i = 0; i < 4; i++) {
        float x = -90.f + i * 60.f;
        k.rr(x - 25, 41, x + 25, 67, rgb(16, 17, 20), 5);
        k.rr(x - 24, 41, x + 24, 43, IM_COL32(255, 255, 255, 40), 2);
        k.well(x - 19, 46, x + 19, 63, 3, rgb(2, 2, 3), 40);
        for (int j = 0; j < 7; j++) k.rr(x - 15 + j * 4.5f, 56, x - 13.f + j * 4.5f, 62, rgb(110, 92, 44), 0);
        k.rr(x - 6, 33, x + 6, 37, pc[i], 2);
    }
    k.gloss(-138, 26, 138, 80, 4, 20);
}

/* Game Boy family: DMG grey, or Colour in grape (translucent) */
void gb_handheld(K& k, bool color, float power) {
    const float hw = color ? 86.f : 90.f, top = color ? -142.f : -146.f, bot = color ? 142.f : 146.f;
    k.shadow(-hw, top, hw, bot, 12, 16, 130);
    ImU32 b0 = color ? rgb(132, 82, 204) : rgb(206, 205, 196), b1 = color ? rgb(88, 46, 160) : rgb(170, 169, 160);
    const float rbr = color ? 22.f : 40.f;
    // cartridge bay on top: a narrower, ridged cap
    k.rr4(-hw + 5, top - 13, hw - 5, top + 8, shade(b0, 1.1f), 6, 6, 0, 0);
    k.rr(-hw + 12, top - 8, hw - 12, top - 1, rgb(10, 10, 12), 2);
    k.rr(-hw + 12, top - 8, hw - 12, top - 6, rgb(64, 64, 72), 1);
    k.grad4(-hw, top, hw, bot, b0, b1, 8, 8, rbr, color ? 22.f : 12.f);
    if (color) {   // see-through shell: faint board shapes behind the plastic
        k.rr(-hw + 14, top + 150, hw - 14, bot - 16, IM_COL32(40, 10, 90, 55), 10);
        k.rr(-hw + 22, top + 160, -8, top + 196, IM_COL32(20, 90, 60, 60), 3);
        k.rr(10, top + 168, hw - 22, top + 214, IM_COL32(20, 70, 50, 50), 3);
        k.line(-hw + 6, top + 20, -hw + 6, bot - 30, IM_COL32(255, 255, 255, 70), 2);
    } else {
        k.line(-hw + 3, top + 12, -hw + 3, bot - 40, IM_COL32(255, 255, 255, 60), 1.5f);
        k.rr(hw - 3, top + 120, hw + 4, top + 168, rgb(120, 120, 114), 2);               // contrast wheel on the side
    }
    k.gloss(-hw, top, hw, bot, 5, 34);
    // dark bezel + screen
    ImU32 bz = color ? rgb(36, 32, 44) : rgb(98, 94, 112);
    k.rr4(-hw + 10, top + 22, hw - 10, top + 152, bz, 8, 8, 22, 8);
    if (!color) { k.rr(-hw + 14, top + 28, hw - 14, top + 31, rgb(196, 70, 70), 1); k.rr(-hw + 14, top + 34, hw - 14, top + 36, rgb(70, 90, 200), 1); }
    float sx = color ? 50.f : 48.f, sh = color ? 44.f : 43.f, sy = top + (color ? 86.f : 92.f);
    ImU32 off = color ? rgb(110, 116, 98) : rgb(118, 132, 78), on = color ? rgb(190, 214, 176) : rgb(166, 186, 74);
    k.rr(-sx - 2, sy - sh - 2, sx + 2, sy + sh + 2, rgb(20, 20, 24), 3);
    k.rr(-sx, sy - sh, sx, sy + sh, mixc(off, on, power), 2);
    if (power > 0.2f) {
        float a = c01(power);
        if (color) { k.rr(-sx, sy - sh, sx, sy - 6, withA(rgb(110, 170, 255), 0.7f * a), 2); k.rr(-sx, sy + 12, sx, sy + sh, withA(rgb(80, 180, 90), 0.8f * a), 2);
                     k.rr(-14, sy - 14, 8, sy + 12, withA(rgb(230, 90, 70), 0.9f * a), 2); }
        else { for (int i = 0; i < 6; i++) k.rr(-40 + i * 14, sy + sh - 28 - (i % 3) * 8, -30 + i * 14, sy + sh, withA(rgb(50, 70, 24), 0.75f * a), 0);
               k.rr(-30, sy - 24, 30, sy - 8, withA(rgb(40, 60, 20), 0.5f * a), 1); }
    }
    k.rr(-sx, sy - sh, sx, sy - sh + 4, IM_COL32(0, 0, 0, 50), 2);
    k.led(-hw + 17, sy - 18, 3.4f, power, rgb(255, 50, 36));
    // d-pad, A/B on the diagonal groove, start/select, speaker grille
    ImU32 pad = color ? rgb(34, 24, 62) : rgb(38, 38, 44);
    const float py = top + (color ? 204.f : 206.f);
    k.circ(-52, py, 31, IM_COL32(0, 0, 0, 38));
    k.rr(-52 - 22, py - 8, -52 + 22, py + 8, pad, 4); k.rr(-52 - 8, py - 22, -52 + 8, py + 22, pad, 4);
    k.rr(-52 - 21, py - 7, -52 + 21, py - 5, IM_COL32(255, 255, 255, 30), 1);
    k.rot_rr(40, py - 6, 84, 40, -0.42f, IM_COL32(0, 0, 0, 40));                         // diagonal recess
    ImU32 ab = color ? rgb(76, 44, 130) : rgb(142, 38, 92);
    k.circ(60, py - 12 + 2, 15, IM_COL32(0, 0, 0, 80)); k.circ(60, py - 12, 15, ab); k.circ(57, py - 17, 6, IM_COL32(255, 255, 255, 38));
    k.circ(22, py + 6 + 2, 15, IM_COL32(0, 0, 0, 80)); k.circ(22, py + 6, 15, ab); k.circ(19, py + 1, 6, IM_COL32(255, 255, 255, 38));
    k.rot_rr(-22, top + 258, 30, 9, -0.4f, IM_COL32(0, 0, 0, 70)); k.rot_rr(-22, top + 257, 28, 8, -0.4f, color ? rgb(70, 50, 110) : rgb(110, 110, 112));
    k.rot_rr(18, top + 258, 30, 9, -0.4f, IM_COL32(0, 0, 0, 70)); k.rot_rr(18, top + 257, 28, 8, -0.4f, color ? rgb(70, 50, 110) : rgb(110, 110, 112));
    for (int i = 0; i < 6; i++) { float o = i * 7.f; k.line(46 + o * 0.5f, top + 262 - o * 0.35f + 12, 62 + o * 0.5f, top + 262 - o * 0.35f - 18 + 12, IM_COL32(0, 0, 0, 100), 3); }
}

void console_gba(K& k, float lid, float power) {
    (void)lid;
    k.shadow(-148, -66, 148, 66, 26, 14, 130);
    ImU32 b0 = rgb(122, 104, 214), b1 = rgb(70, 56, 150);
    // shoulder buttons and the rear cartridge housing
    k.rr4(-140, -80, -82, -58, rgb(88, 74, 176), 9, 4, 0, 0); k.rr4(82, -80, 140, -58, rgb(88, 74, 176), 4, 9, 0, 0);
    k.rr4(-64, -83, 64, -58, rgb(58, 48, 118), 6, 6, 0, 0);
    k.rr(-50, -79, 50, -70, rgb(8, 8, 12), 2); k.rr(-50, -72, 50, -70, rgb(84, 76, 134), 1);
    k.grad(-148, -66, 148, 66, b0, b1, 30);
    k.rr(-146, -44, -143, 40, IM_COL32(255, 255, 255, 40), 1);
    k.gloss(-148, -66, 148, 66, 10, 46);
    // screen frame + screen
    k.rr(-80, -54, 80, 54, rgb(36, 34, 52), 8);
    k.rr(-72, -48, 72, 48, rgb(58, 56, 66), 5);
    k.rr(-62, -38, 62, 40, mixc(rgb(60, 66, 62), rgb(160, 206, 240), power), 2);
    if (power > 0.2f) {
        k.rr(-62, 12, 62, 40, withA(rgb(70, 170, 80), 0.85f * power), 2);
        k.rr(-62, -38, 62, -10, withA(rgb(255, 255, 255), 0.16f * power), 2);
        k.circ(-24, -14, 11, withA(rgb(255, 220, 90), 0.9f * power)); k.rr(14, 0, 42, 10, withA(rgb(230, 100, 70), 0.9f * power), 1);
    }
    k.rr(-62, -38, 62, -33, IM_COL32(0, 0, 0, 55), 2);
    k.led(-100, -44, 3.0f, power, rgb(255, 80, 60));
    // d-pad (left), A/B + start/select (right), speaker (lower left)
    ImU32 pad = rgb(40, 36, 66);
    k.circ(-110, -6, 25, IM_COL32(0, 0, 0, 30));
    k.rr(-128, -8, -92, 8, pad, 4); k.rr(-118, -19, -102, 19, pad, 4);
    k.rr(-127, -7, -93, -5, IM_COL32(255, 255, 255, 28), 1);
    k.circ(118, -14, 13, IM_COL32(0, 0, 0, 80)); k.circ(118, -16, 13, rgb(58, 50, 100)); k.circ(116, -20, 5, IM_COL32(255, 255, 255, 36));
    k.circ(92, 2, 13, IM_COL32(0, 0, 0, 80)); k.circ(92, 0, 13, rgb(58, 50, 100)); k.circ(90, -4, 5, IM_COL32(255, 255, 255, 36));
    k.rot_rr(98, 40, 22, 7, -0.45f, rgb(60, 54, 108)); k.rot_rr(124, 36, 22, 7, -0.45f, rgb(60, 54, 108));
    for (int i = 0; i < 6; i++) k.rot_rr(-120 + i * 5.f, 38 - i * 3.f, 2.5f, 10, 0.5f, IM_COL32(0, 0, 0, 100));
    k.ell(0, 62, 120, 5, IM_COL32(0, 0, 0, 40));
}

/* Clamshell DS Lite (polar white) / 3DS (aqua) opened flat: upper screen, hinge, lower screen */
void console_ds(K& k, bool is3, float lid, float power) {
    (void)lid;
    float w = is3 ? 120.f : 108.f;
    ImU32 b0 = is3 ? rgb(84, 176, 224) : rgb(238, 240, 246), b1 = is3 ? rgb(46, 118, 178) : rgb(196, 202, 216);
    ImU32 inner = is3 ? rgb(60, 140, 196) : rgb(228, 230, 238), glass = rgb(10, 11, 15);
    k.shadow(-w, -150, w, 146, 14, 16, 120);
    // upper shell
    k.grad4(-w, -152, w, -12, b0, b1, 16, 16, 3, 3);
    k.rr(-w + 6, -144, w - 6, -20, inner, 10);
    k.rr(-w + 6, -144, w - 6, -141, IM_COL32(255, 255, 255, 80), 3);
    const float scw = is3 ? 82.f : 62.f, sch = is3 ? 50.f : 46.f, scy = -82.f;   // 3DS top screen is wide; DS Lite is 4:3
    k.rr(-scw - 8, scy - sch - 8, scw + 8, scy + sch + 8, glass, 7);
    k.rr(-scw - 1, scy - sch - 1, scw + 1, scy + sch + 1, rgb(28, 30, 36), 3);
    k.rr(-scw, scy - sch, scw, scy + sch, mixc(rgb(24, 26, 30), is3 ? rgb(120, 200, 240) : rgb(128, 188, 236), power), 2);
    if (power > 0.2f) { k.rr(-scw, scy - sch, scw, scy - sch + 36, withA(rgb(255, 255, 255), 0.2f * power), 2); k.rr(-scw, scy + sch - 28, scw, scy + sch, withA(rgb(80, 180, 90), 0.85f * power), 2); }
    k.rr(-scw, scy - sch, scw, scy - sch + 3, IM_COL32(255, 255, 255, 36), 2);
    // speaker grilles either side of the upper screen (DS Lite: hole fields; 3DS: slits)
    for (int sidex = -1; sidex <= 1; sidex += 2) for (int i = 0; i < 5; i++) {
        float gx = sidex * (scw + 8 + (w - 6 - scw - 8) * 0.5f);
        if (is3) k.rr(gx - 5, scy - 20 + i * 8.f, gx + 5, scy - 17 + i * 8.f, IM_COL32(0, 0, 0, 110), 1);
        else for (int j = 0; j < 2; j++) k.circ(gx - 3 + j * 6.f, scy - 20 + i * 8.f, 1.6f, IM_COL32(0, 0, 0, 90));
    }
    if (is3) { k.circ(0, -138, 2.4f, rgb(8, 10, 16)); k.rr(w - 17, -128, w - 11, -66, rgb(30, 90, 130), 2); k.rr(w - 16, -112, w - 12, -100, rgb(180, 220, 240), 1); }   // inner camera + 3D slider
    // hinge
    k.vquad({-86, -12}, {86, -12}, {86, 6}, {-86, 6}, is3 ? rgb(40, 98, 150) : rgb(170, 176, 192), is3 ? rgb(28, 72, 118) : rgb(130, 136, 152));
    k.rr(-82, -10, 82, -6, IM_COL32(255, 255, 255, 70), 2);
    k.led(-72, -3, 1.8f, power, rgb(70, 220, 255));
    // lower shell
    k.grad4(-w, 4, w, 144, b0, b1, 3, 3, 18, 18);
    k.rr(-w + 6, 12, w - 6, 136, inner, 10);
    const float tw = is3 ? 54.f : 62.f, tt = 22.f;
    float ty0 = is3 ? 26.f : 22.f, ty1 = ty0 + (is3 ? 80.f : 92.f);
    k.rr(-tw - 6, ty0 - 6, tw + 6, ty1 + 6, glass, 6);
    k.rr(-tw, ty0, tw, ty1, mixc(rgb(34, 36, 40), rgb(206, 224, 232), power), 2);
    if (power > 0.2f) k.rr(-tw + 6, ty0 + 10, tw - 6, ty0 + 32, withA(rgb(90, 140, 220), 0.5f * power), 3);
    (void)tt;
    ImU32 pad = is3 ? rgb(52, 60, 76) : rgb(112, 118, 134);
    ImU32 pad2 = is3 ? rgb(236, 244, 250) : rgb(244, 246, 250);
    float lx = -w + 28 + (is3 ? 0.f : 2.f), rx = w - 28;
    if (is3) {   // circle pad over the d-pad, face diamond, home/start/select
        k.circ(lx, 52, 16, IM_COL32(0, 0, 0, 90)); k.circ(lx, 51, 14, rgb(28, 30, 38)); k.circ(lx, 50, 9, rgb(60, 64, 76));
        k.rr(lx - 13, 96 - 4, lx + 13, 96 + 4, pad, 2); k.rr(lx - 4, 96 - 13, lx + 4, 96 + 13, pad, 2);
        for (auto [dx, dy] : {std::pair<float, float>{0, -11}, {11, 0}, {0, 11}, {-11, 0}}) { k.circ(rx + dx, 64 + dy, 6.f, IM_COL32(0, 0, 0, 80)); k.circ(rx + dx, 63.f + dy, 5.6f, pad2); }
        k.circ(0, 124, 6, rgb(210, 220, 230)); k.rr(rx - 10, 100, rx + 10, 106, pad, 3); k.rr(rx - 10, 110, rx + 10, 116, pad, 3);
    } else {
        k.rr(lx - 14, 54 - 4, lx + 14, 54 + 4, pad, 2); k.rr(lx - 4, 54 - 14, lx + 4, 54 + 14, pad, 2);
        k.rr(lx - 15, 53 - 4, lx + 15, 53 - 2, IM_COL32(255, 255, 255, 80), 1);
        for (auto [dx, dy] : {std::pair<float, float>{0, -12}, {12, 0}, {0, 12}, {-12, 0}}) { k.circ(rx + dx, 54 + dy + 1.f, 6.2f, IM_COL32(0, 0, 0, 60)); k.circ(rx + dx, 54 + dy, 6.f, pad); }
        for (int i = 0; i < 3; i++) k.circ(lx + (i - 1) * 8.f, 100, 4.2f, pad);
        for (int i = 0; i < 3; i++) k.rr(rx - 12, 92 + i * 9.f, rx + 12, 96 + i * 9.f, pad, 2);
        k.circ(0, 124, 3.f, IM_COL32(0, 0, 0, 120));
    }
    k.led(w - 14, 126, 2.4f, power, rgb(70, 220, 255));
    // card slot in the top edge of the upper shell
    k.rr(-36, -155, 36, -146, rgb(8, 8, 10), 2);
    k.rr(-36, -155, 36, -153, rgb(96, 96, 106), 1);
}

void console_gc(K& k, float lid, float power) {
    k.shadow_ell(0, 98, 124, 13, 140);
    ImU32 f0 = rgb(104, 90, 184), f1 = rgb(58, 48, 122), top0 = rgb(116, 102, 198), top1 = rgb(132, 118, 212);
    // carry handle behind the top, with its opening
    k.rr4(-62, -112, 62, -62, rgb(78, 66, 150), 24, 24, 0, 0);
    k.rr4(-46, -101, 46, -66, rgb(14, 12, 24), 14, 14, 0, 0);
    k.rr4(-62, -112, 62, -108, IM_COL32(255, 255, 255, 50), 24, 24, 0, 0);
    // top face (cube: the back is a bit narrower), rounded back corners
    k.vquad({-98, -78}, {98, -78}, {108, 8}, {-108, 8}, top0, top1);
    k.quadg({-108, 8}, {-98, -78}, {-102, -76}, {-112, 8}, rgb(70, 60, 140), rgb(70, 60, 140), rgb(50, 42, 104), rgb(50, 42, 104));
    k.line(-98, -78, 98, -78, rgb(160, 148, 232), 1.5f);
    // front face
    k.vquad({-108, 8}, {108, 8}, {108, 96}, {-108, 96}, f0, f1);
    k.rr4(-108, 90, 108, 100, rgb(36, 30, 78), 0, 0, 10, 10);
    k.line(-108, 8, 108, 8, rgb(186, 174, 248), 1.8f);
    k.rr(-110, 8, -108, 92, IM_COL32(255, 255, 255, 30));
    // disc well + hinged round lid
    const float R = 58.f, ry = R * 0.5f, hinge = -64.f;   // the round lid only covers an 8 cm mini disc: about half the console width
    k.ell(0, hinge + ry, R + 7, ry + 4, rgb(86, 74, 160));                  // raised ring around the disc well
    k.ellr(0, hinge + ry, R + 7, ry + 4, IM_COL32(255, 255, 255, 50), 1.2f);
    well_with_disc_slot(k, 0, hinge + ry, R - 2, ry - 1, rgb(14, 13, 22), rgb(66, 62, 90));
    if (g_inner) g_inner(k.d);
    hinge_lid(k, 0, hinge, R, lid, rgb(142, 130, 222), rgb(54, 46, 100), rgb(88, 76, 164), 7.f, rgb(150, 138, 230));
    // OPEN button on the top front edge
    k.button(58, -4, 92, 4, rgb(150, 138, 226), 4);
    // front: power button (left), reset, memory card flaps (right), four controller ports
    k.circ(-84, 28, 10, rgb(24, 20, 48)); k.circ(-84, 27, 8, rgb(196, 190, 244)); k.circ(-84, 27, 4.2f, rgb(120, 112, 190));
    k.rr(-64, 22, -30, 34, rgb(40, 34, 76), 5);
    k.circ(-44, 28, 3.2f, rgb(20, 18, 36));
    k.well(44, 20, 98, 26, 2, rgb(10, 9, 18), 40); k.well(44, 30, 98, 36, 2, rgb(10, 9, 18), 40);
    for (int i = 0; i < 4; i++) {
        float x = -78.f + i * 52.f;
        k.rr(x - 21, 52, x + 21, 80, rgb(22, 18, 44), 6);
        k.well(x - 17, 56, x + 17, 77, 4, rgb(5, 5, 9), 40);
        for (int j = 0; j < 5; j++) k.rr(x - 11 + j * 5.f, 68, x - 9.f + j * 5.f, 75, rgb(100, 92, 60), 0);
    }
    k.led(-84, 41, 2.6f, power, rgb(120, 255, 110));
    k.gloss(-108, 8, 108, 96, 4, 22);
}

void console_ps1(K& k, float lid, float power) {
    k.shadow_ell(0, 92, 150, 13, 130);
    ImU32 t0 = rgb(184, 186, 190), t1 = rgb(206, 208, 212), f0 = rgb(166, 168, 172), f1 = rgb(112, 114, 120);
    k.vquad({-128, -84}, {128, -84}, {150, 6}, {-150, 6}, t0, t1);
    k.quadg({-150, 6}, {-128, -84}, {-132, -84}, {-154, 6}, rgb(120, 122, 128), rgb(120, 122, 128), rgb(96, 98, 104), rgb(96, 98, 104));
    k.line(-128, -84, 128, -84, rgb(230, 232, 236), 1.5f);
    // ribbed vent fields either side of the lid
    for (int s = -1; s <= 1; s += 2) for (int i = 0; i < 6; i++) {
        float y = -68 + i * 7.f;
        k.line(s * 98.f, y, s * (122.f + i * 2.f), y, IM_COL32(0, 0, 0, 52), 2.f);
        k.line(s * 98.f, y + 2, s * (122.f + i * 2.f), y + 2, IM_COL32(255, 255, 255, 40), 1.f);
    }
    // front face
    k.vquad({-150, 6}, {150, 6}, {150, 86}, {-150, 86}, f0, f1);
    k.rr4(-150, 80, 150, 90, rgb(70, 72, 78), 0, 0, 8, 8);
    k.line(-150, 6, 150, 6, rgb(240, 242, 246), 1.8f);
    // disc well + round lid with its hinge at the back
    const float R = 78.f, ry = R * 0.5f, hinge = -80.f;
    well_with_disc_slot(k, 0, hinge + ry, R - 2, ry - 1, rgb(22, 22, 28), rgb(84, 86, 94));
    if (g_inner) g_inner(k.d);
    hinge_lid(k, 0, hinge, R, lid, rgb(196, 198, 202), rgb(120, 122, 130), rgb(150, 152, 158), 7.f, rgb(204, 206, 210));
    // top-front strip: power (left), reset, open (right)
    k.button(-112, -2, -72, 6, rgb(138, 140, 148), 3); k.button(-8, -2, 8, 6, rgb(138, 140, 148), 3); k.button(72, -2, 112, 6, rgb(138, 140, 148), 3);
    k.led(-92, 17, 3.0f, power, rgb(110, 255, 110));
    // two controller ports (left), two memory card slots (right)
    for (int i = 0; i < 2; i++) { float x = -118.f + i * 56.f; k.rr(x - 22, 40, x + 22, 66, rgb(80, 82, 88), 4); k.well(x - 17, 44, x + 17, 62, 3, rgb(6, 6, 8), 40); }
    for (int i = 0; i < 2; i++) { float x = 62.f + i * 52.f; k.rr(x - 20, 42, x + 20, 62, rgb(80, 82, 88), 3); k.well(x - 15, 47, x + 15, 57, 2, rgb(6, 6, 8), 40); }
    k.gloss(-150, 6, 150, 86, 4, 34);
}

void console_ps2(K& k, float lid, float power) {
    k.shadow_ell(0, 98, 150, 12, 140);
    float tray = c01(lid); tray = tray * tray * (3 - 2 * tray);
    // top: glossy black with a faint blue cast and the two dark side vent fields
    k.vquad({-128, -66}, {128, -66}, {150, 14}, {-150, 14}, rgb(40, 42, 54), rgb(26, 27, 34));
    k.vquad({-128, -66}, {128, -66}, {134, -52}, {-134, -52}, rgb(66, 70, 88), rgb(46, 49, 62));
    for (int i = 0; i < 9; i++) { float x = -98 + i * 8.f; k.line(x, -56, x - 8, -14, IM_COL32(0, 0, 0, 50), 2.f); k.line(-x, -56, -x + 8, -14, IM_COL32(0, 0, 0, 50), 2.f); }
    k.ell(0, -28, 62, 24, rgb(16, 17, 22)); k.ellr(0, -28, 62, 24, rgb(70, 78, 112), 1.5f);
    k.ellr(0, -28, 40, 15, withA(rgb(90, 130, 255), 0.22f + 0.4f * power), 2.f);
    k.ell(0, -28, 14, 5.5f, withA(rgb(90, 130, 255), 0.14f + 0.3f * power));
    k.line(-120, -48, 120, -48, IM_COL32(255, 255, 255, 22), 1.f);
    // front
    k.vquad({-150, 14}, {150, 14}, {150, 84}, {-150, 84}, rgb(28, 29, 36), rgb(10, 10, 14));
    k.rr4(-150, 78, 150, 88, rgb(6, 6, 8), 0, 0, 8, 8);
    k.line(-150, 14, 150, 14, rgb(110, 118, 150), 1.5f);
    k.rr(-152, 14, -150, 80, IM_COL32(255, 255, 255, 22));
    // tray bay: the front plate slides out toward the viewer
    k.well(-86, 24, 64, 58, 4, rgb(2, 2, 4), 38);
    k.rr(-80, 40, 58, 48, rgb(0, 0, 0), 2);
    if (tray > 0.01f) {
        float ext = 96.f * tray, wy = 44.f + ext, w = 70.f + 14.f * tray;
        k.quadg({-w + 8, 42}, {w - 22, 42}, {w - 14, wy}, {-w, wy}, rgb(16, 17, 22), rgb(16, 17, 22), rgb(36, 38, 48), rgb(36, 38, 48));
        k.line(-w + 8, 42, -w, wy, IM_COL32(150, 160, 210, 70), 1.2f); k.line(w - 22, 42, w - 14, wy, IM_COL32(150, 160, 210, 70), 1.2f);
        k.rr(-w - 2, wy - 5, w - 12, wy + 5, rgb(30, 32, 42), 3); k.rr(-w - 2, wy - 5, w - 12, wy - 3, IM_COL32(255, 255, 255, 60), 2);
        float cy = (42.f + wy) * 0.5f; float erx = 50 + 6 * tray, ery = 14 + 14 * tray;
        k.ell(-7, cy + 2, erx, ery, rgb(3, 3, 5)); k.ellr(-7, cy + 2, erx, ery, rgb(82, 88, 116), 1.5f);
    } else {
        k.rr(-84, 48, 62, 56, rgb(18, 19, 25), 3); k.rr(-84, 48, 62, 50, IM_COL32(255, 255, 255, 40), 2);   // closed tray front plate
    }
    if (g_inner) g_inner(k.d);
    k.button(88, 26, 114, 42, rgb(36, 38, 50), 4);   // power
    k.led(101, 34, 3.4f, power, rgb(60, 150, 255));
    k.button(96, 50, 112, 58, rgb(36, 38, 50), 3);   // eject/reset
    for (int i = 0; i < 2; i++) { k.rr(-134, 28 + i * 24.f, -96, 44 + i * 24.f, rgb(6, 6, 9), 4); k.rr(-134, 28 + i * 24.f, -96, 30 + i * 24.f, IM_COL32(255, 255, 255, 28), 2); }
    for (int i = 0; i < 2; i++) { k.rr(70 + i * 0.f, 60 + i * 0.f, 124, 70, rgb(6, 6, 9), 3); break; }
    k.gloss(-150, 14, 150, 84, 4, 14);
}

void console_wiiu(K& k, float lid, float power) {
    k.shadow_ell(0, 92, 146, 12, 130);
    ImU32 f0 = rgb(244, 246, 250), f1 = rgb(192, 196, 208);
    // top: glossy shell, then the matte lower shell step
    k.vquad({-122, -66}, {122, -66}, {146, 12}, {-146, 12}, rgb(232, 234, 242), rgb(250, 251, 255));
    k.quadg({-146, 12}, {-122, -66}, {-126, -66}, {-150, 12}, rgb(176, 180, 194), rgb(176, 180, 194), rgb(150, 154, 170), rgb(150, 154, 170));
    k.vquad({-122, -66}, {122, -66}, {128, -50}, {-128, -50}, rgb(255, 255, 255), rgb(246, 248, 254));
    k.line(-134, -22, 134, -22, IM_COL32(0, 0, 0, 24), 1.5f);
    k.vquad({-134, -22}, {134, -22}, {146, 12}, {-146, 12}, rgb(214, 217, 226), rgb(226, 228, 236));
    k.ell(0, -40, 70, 18, IM_COL32(255, 255, 255, 40));
    // front
    k.vquad({-146, 12}, {146, 12}, {146, 80}, {-146, 80}, f0, f1);
    k.rr4(-146, 74, 146, 84, rgb(150, 154, 168), 0, 0, 12, 12);
    k.line(-146, 12, 146, 12, rgb(255, 255, 255), 1.8f);
    // disc slot, lit strip beneath
    float o = c01(lid);
    k.rr(-6, 28, 116, 40 + 4 * o, rgb(18, 18, 24), 3);
    k.rr(-2, 31, 112, 35 + 3 * o, rgb(0, 0, 0), 2);
    if (g_inner) g_inner(k.d);
    k.rr(-6, 28, 116, 30, rgb(210, 214, 226), 1);
    k.rr(2, 46, 108, 53, mixc(rgb(150, 156, 176), rgb(70, 190, 255), c01(power * 0.7f + o * 0.3f)), 3);
    if (power > 0.02f || o > 0.02f) for (int i = 3; i >= 1; i--) k.rr(2 - i * 3, 46 - i * 2, 108 + i * 3, 53 + i * 2, withA(rgb(70, 190, 255), 0.07f * std::max(power, o)), 6);
    // power button, sensor strip, SD flap
    k.circ(-110, 38, 12, rgb(176, 180, 194)); k.circ(-110, 37, 10, rgb(240, 242, 248));
    k.led(-110, 38, 2.8f, power, rgb(70, 190, 255));
    k.rr(-76, 30, -34, 42, rgb(200, 204, 216), 4);
    k.rr(-76, 62, -34, 70, rgb(188, 192, 206), 2); k.rr(-76, 62, -34, 64, rgb(150, 154, 170), 1);
    k.gloss(-146, 12, 146, 80, 4, 70);
}

void console_nes(K& k, float lid, float power) {
    k.shadow_ell(0, 92, 146, 12, 130);
    ImU32 dark = rgb(48, 48, 52), cream = rgb(184, 184, 180);   // light grey body, charcoal trim
    // top slab
    k.vquad({-130, -36}, {130, -36}, {148, 12}, {-148, 12}, rgb(166, 166, 162), cream);
    k.vquad({-130, -36}, {130, -36}, {134, -28}, {-134, -28}, rgb(206, 206, 202), rgb(190, 190, 186));
    k.quadg({-148, 12}, {-130, -36}, {-134, -36}, {-152, 12}, rgb(150, 148, 142), rgb(150, 148, 142), rgb(126, 124, 120), rgb(126, 124, 120));
    // ribbed grip lines either side of the bay
    for (int s = -1; s <= 1; s += 2) for (int i = 0; i < 6; i++) k.line(s * (102.f + i * 5.f), -22, s * (110.f + i * 5.f), 0, IM_COL32(0, 0, 0, 36), 1.4f);
    // cartridge bay: dark opening, flip-up door lip at the front
    k.quadg({-88, -28}, {88, -28}, {96, 8}, {-96, 8}, rgb(76, 76, 80), rgb(76, 76, 80), rgb(48, 48, 52), rgb(48, 48, 52));
    k.quadg({-78, -24}, {78, -24}, {86, 4}, {-86, 4}, rgb(2, 2, 3), rgb(2, 2, 3), rgb(14, 14, 16), rgb(14, 14, 16));
    for (int i = 0; i < 15; i++) k.rr(-56 + i * 8.f, -4, -53 + i * 8.f, 3, rgb(150, 124, 56), 0);
    if (g_inner) g_inner(k.d);
    k.rr(-96, 4, 96, 8, rgb(70, 70, 74), 1);                                              // door hinge bar
    // power / reset on the top left, LED
    k.well(-132, -20, -102, -4, 2, rgb(60, 60, 64), 30);
    k.button(-128, -18, -116, -6, rgb(56, 56, 60), 2); k.button(-114, -18, -102, -6, rgb(56, 56, 60), 2);
    k.led(-120, -26, 2.4f, power, rgb(255, 50, 40));
    // front face: cream upper band, black stripe, ports
    k.vquad({-148, 12}, {148, 12}, {148, 82}, {-148, 82}, rgb(186, 186, 182), rgb(150, 150, 146));
    k.line(-148, 12, 148, 12, rgb(226, 226, 222), 1.5f);
    {   // black flap door on the front, hinged along its top edge: swings up and back as the cart goes in (lid = openness), then drops shut over it
        const float dh = 28.f * (1.f - 0.86f * lid);
        k.vquad({-98, 8}, {98, 8}, {100, 8 + dh}, {-100, 8 + dh}, rgb(58, 58, 62), rgb(24, 24, 27));
        k.rr(-98, 8, 98, 10, IM_COL32(255, 255, 255, 60), 1);
        if (lid < 0.5f) for (int i = 0; i < 5; i++) k.rr(-40 + i * 20.f, 16, -30 + i * 20.f, 30, IM_COL32(255, 255, 255, 22), 1);
    }
    k.rr(-148, 36, 148, 64, dark, 0); k.rr(-148, 36, 148, 38, rgb(100, 100, 106), 0); k.rr(-148, 62, 148, 64, rgb(28, 28, 32), 0);
    for (int i = 0; i < 4; i++) k.rr(-98 + i * 8.f, 41, -95 + i * 8.f, 58, rgb(22, 22, 26), 1);    // vents
    for (int i = 0; i < 4; i++) k.rr(66 + i * 8.f, 41, 69 + i * 8.f, 58, rgb(22, 22, 26), 1);
    k.well(-60, 42, -6, 58, 3, rgb(14, 14, 16), 30); k.well(6, 42, 60, 58, 3, rgb(14, 14, 16), 30);   // controller ports
    for (int i = 0; i < 7; i++) { k.circ(-52 + i * 7.f, 50, 1.6f, rgb(60, 60, 64)); k.circ(14 + i * 7.f, 50, 1.6f, rgb(60, 60, 64)); }
    k.rr(-148, 76, 148, 84, rgb(116, 114, 110), 0);
    k.gloss(-148, 12, 148, 82, 4, 26);
}

void console_snes(K& k, float lid, float power) {
    (void)lid;
    k.shadow_ell(0, 90, 156, 12, 130);
    ImU32 top = rgb(198, 198, 204), dark = rgb(94, 88, 130), purple = rgb(118, 108, 164);
    // low base slab and the raised centre block that holds the cartridge slot
    k.vquad({-148, -24}, {148, -24}, {160, 18}, {-160, 18}, rgb(190, 190, 198), top);
    k.quadg({-160, 18}, {-148, -24}, {-152, -24}, {-164, 18}, rgb(140, 140, 150), rgb(140, 140, 150), rgb(114, 114, 126), rgb(114, 114, 126));
    k.vquad({-112, -76}, {112, -76}, {122, -22}, {-122, -22}, rgb(214, 214, 220), rgb(226, 226, 232));
    k.vquad({-112, -76}, {112, -76}, {116, -66}, {-116, -66}, rgb(240, 240, 246), rgb(224, 224, 230));
    k.quadg({-122, -22}, {-112, -76}, {-116, -76}, {-126, -22}, rgb(150, 150, 160), rgb(150, 150, 160), rgb(124, 124, 136), rgb(124, 124, 136));
    // slot
    k.quadg({-78, -66}, {78, -66}, {84, -28}, {-84, -28}, rgb(20, 18, 28), rgb(20, 18, 28), rgb(30, 28, 38), rgb(30, 28, 38));
    k.quadg({-70, -62}, {70, -62}, {76, -32}, {-76, -32}, rgb(2, 2, 4), rgb(2, 2, 4), rgb(12, 12, 16), rgb(12, 12, 16));
    for (int i = 0; i < 14; i++) k.rr(-47 + i * 7.f, -38, -44 + i * 7.f, -32, rgb(150, 124, 56), 0);
    if (g_inner) g_inner(k.d);
    k.vquad({-86, -30}, {86, -30}, {90, -20}, {-90, -20}, rgb(150, 148, 162), rgb(118, 116, 130));     // slot lip over the cart
    k.rr(-86, -30, 86, -28, IM_COL32(255, 255, 255, 70), 1);
    // top controls: power slider (left), reset + eject (right), in purple-grey
    k.well(-142, -14, -96, -2, 3, rgb(80, 76, 104), 30);
    k.button(-132, -17, -112, 0, purple, 3);
    for (int i = 0; i < 3; i++) k.line(-128 + i * 5.f, -13, -128 + i * 5.f, -4, IM_COL32(0, 0, 0, 80), 1.f);
    k.well(96, -14, 142, -2, 3, rgb(80, 76, 104), 30);
    k.button(100, -16, 118, -2, purple, 6); k.button(122, -17, 140, 1, purple, 3);
    // front face: grey panel, indigo band, controller port door
    k.vquad({-160, 18}, {160, 18}, {160, 82}, {-160, 82}, rgb(214, 214, 220), rgb(164, 164, 174));
    k.line(-160, 18, 160, 18, rgb(248, 248, 252), 1.6f);
    k.rr(-160, 44, 160, 47, rgb(150, 150, 162), 0); k.rr(-160, 47, 160, 48, IM_COL32(255, 255, 255, 60), 0); (void)dark;   // US model: plain grey with a thin seam, only the buttons are purple
    k.rr(-118, 58, -4, 78, rgb(36, 34, 48), 5); k.rr(4, 58, 118, 78, rgb(36, 34, 48), 5);          // two controller ports
    k.well(-110, 62, -12, 74, 3, rgb(6, 6, 10), 30); k.well(12, 62, 110, 74, 3, rgb(6, 6, 10), 30);
    k.led(-144, 30, 3.0f, power, rgb(255, 60, 44));
    k.gloss(-160, 18, 160, 82, 4, 26);
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
    case System::NES: console_nes(k, lid, power); break;
    case System::SNES: console_snes(k, lid, power); break;
    case System::N64: case System::Unknown: default: console_n64(k, lid, power); break;
    }
}

/* ---------- media ---------- */
namespace {
bool g_back = false;   // disc media: show the data side (rainbow sheen) instead of the printed label

void sticker(K& k, float x0, float y0, float x1, float y1, ImTextureID tex, ImVec2 uv0, ImVec2 uv1, ImU32 fb, float r = 3) {
    k.rr(x0 - 6, y0 - 5, x1 + 6, y1 + 6, IM_COL32(0, 0, 0, 80), r + 3);                    // recessed pocket
    k.rr(x0 - 3, y0 - 3, x1 + 3, y1 + 3, rgb(226, 226, 230), r + 1);
    if (tex) k.d->AddImageRounded(tex, k.p(x0, y0), k.p(x1, y1), uv0, uv1, IM_COL32_WHITE, r * k.s);
    else {
        k.grad(x0, y0, x1, y1, shade(fb, 1.25f), shade(fb, 0.7f), r);
        k.circ((x0 + x1) / 2, (y0 + y1) / 2, std::min(x1 - x0, y1 - y0) * 0.22f, IM_COL32(255, 255, 255, 60));
        k.rr(x0 + 6, y1 - 14, x1 - 6, y1 - 6, IM_COL32(255, 255, 255, 70), 2);
    }
    k.rr(x0, y0, x1, y0 + 3, IM_COL32(255, 255, 255, 46), r);                              // glossy sheen
    k.rr(x0, y1 - 2, x1, y1, IM_COL32(0, 0, 0, 40), r);
}
void poly_shadow(K& k, std::initializer_list<ImVec2> pts, int alpha) {
    for (int i = 3; i >= 1; i--) {
        ImVec2 q[16]; int n = 0; for (auto& v : pts) if (n < 16) q[n++] = k.p(v.x + i * 1.2f, v.y + 4 + i * 2.2f);
        k.d->AddConvexPolyFilled(q, n, IM_COL32(0, 0, 0, alpha / 3));
    }
}
void pins(K& k, float x0, float x1, float y0, float y1, int n) {
    k.rr(x0 - 3, y0 - 2, x1 + 3, y1 + 2, rgb(30, 30, 34), 2);
    float step = (x1 - x0) / n;
    for (int i = 0; i < n; i++) k.rr(x0 + i * step + step * 0.18f, y0, x0 + (i + 1) * step - step * 0.18f, y1, rgb(214, 178, 84), 0);
}
/* plastic shell: lit top-left edge, shaded bottom-right, 1px seam */
void shell(K& k, float x0, float y0, float x1, float y1, ImU32 c, float tl, float tr, float br, float bl) {
    k.grad4(x0, y0, x1, y1, shade(c, 1.12f), shade(c, 0.86f), tl, tr, br, bl);
    k.rr(x0 + 1.5f, y0 + 1.2f, x1 - 1.5f, y0 + 3.2f, IM_COL32(255, 255, 255, 60), tl);
    k.rr(x0, y0 + tl, x0 + 2.f, y1 - bl, IM_COL32(255, 255, 255, 34), 1);
    k.rr(x1 - 2.5f, y0 + tr, x1, y1 - br, IM_COL32(0, 0, 0, 44), 1);
}

void media_n64(K& k, ImTextureID t, ImVec2 u0, ImVec2 u1) {
    k.shadow(-75, -65, 75, 62, 8, 14, 120);
    ImU32 g = rgb(134, 136, 144);
    shell(k, -60, 34, 60, 62, shade(g, 0.8f), 0, 0, 6, 6);                                  // narrower neck that carries the connector
    shell(k, -75, -65, 75, 40, g, 7, 7, 3, 3);
    k.rr(-75, 36, -60, 40, IM_COL32(0, 0, 0, 70), 0); k.rr(60, 36, 75, 40, IM_COL32(0, 0, 0, 70), 0);
    k.rr(-64, -61, 64, -40, shade(g, 0.78f), 3);                                            // recessed grip band
    for (int i = 0; i < 17; i++) k.rr(-60 + i * 7.5f, -58, -57.f + i * 7.5f, -43, shade(g, 1.2f), 1);
    k.rr(-64, -61, 64, -59, IM_COL32(0, 0, 0, 60), 2);
    sticker(k, -56, -32, 56, 26, t, u0, u1, rgb(200, 120, 60), 3);
    pins(k, -44, 44, 48, 60, 16);
    k.rr(-8, 36, 8, 42, rgb(30, 30, 34), 2);                                                // keying notch
    for (int i = 0; i < 2; i++) k.line(i ? 70.f : -70.f, -30, i ? 70.f : -70.f, 30, IM_COL32(0, 0, 0, 40), 1.5f);   // side seam
}

void media_nes(K& k, ImTextureID t, ImVec2 u0, ImVec2 u1) {
    k.shadow(-70, -66, 70, 66, 8, 14, 120);
    ImU32 g = rgb(142, 140, 144);
    // lower neck with the 72-pin edge, stepped body above it
    shell(k, -62, 40, 62, 66, shade(g, 0.72f), 0, 0, 5, 5);
    shell(k, -70, -66, 70, 46, g, 6, 6, 2, 2);
    k.rr4(-70, -66, 70, -48, shade(g, 0.8f), 6, 6, 0, 0);                                    // darker top band
    for (int i = 0; i < 3; i++) k.line(-62, -62 + i * 5.f, 62, -62 + i * 5.f, IM_COL32(0, 0, 0, 62), 1.6f);
    for (int s = -1; s <= 1; s += 2) for (int i = 0; i < 3; i++) k.line(s * 65.f, 14 + i * 7.f, s * 65.f, 20 + i * 7.f, IM_COL32(0, 0, 0, 60), 2.f);   // side grooves
    sticker(k, -54, -40, 54, 24, t, u0, u1, rgb(200, 90, 60), 2);
    k.rr(-46, 30, 46, 38, shade(g, 0.8f), 2);
    pins(k, -50, 50, 52, 63, 18);
    for (int s = -1; s <= 1; s += 2) k.rr(s * 62.f - 3, 40, s * 62.f + 3, 46, IM_COL32(0, 0, 0, 60), 0);
}

void media_snes(K& k, ImTextureID t, ImVec2 u0, ImVec2 u1) {
    k.shadow(-80, -60, 80, 60, 10, 14, 120);
    ImU32 g = rgb(176, 176, 184);
    shell(k, -62, 30, 62, 60, shade(g, 0.78f), 0, 0, 5, 5);
    shell(k, -80, -60, 80, 36, g, 12, 12, 4, 4);
    k.rr4(-80, -60, 80, -42, shade(g, 0.88f), 12, 12, 0, 0);
    for (int i = 0; i < 4; i++) k.line(-64, -56 + i * 4.f, 64, -56 + i * 4.f, IM_COL32(0, 0, 0, 52), 1.5f);
    for (int s = -1; s <= 1; s += 2) { k.rr(s * 80.f - (s < 0 ? 0.f : 6.f), -20, s * 80.f + (s < 0 ? 6.f : 0.f), 24, shade(g, 0.8f), 2); }   // side grip recesses
    sticker(k, -62, -36, 62, 20, t, u0, u1, rgb(120, 90, 180), 3);
    pins(k, -52, 52, 46, 58, 17);
}

/* Game Boy family cartridges. GB / GBC: small, square-shouldered, top-right corner cut, a recessed label well and a narrower
 * connector foot (DMG grey, Colour dual-mode charcoal). GBA: short and WIDE with rounded top corners and a wide recessed label. */
void media_gb(K& k, System s, ImTextureID t, ImVec2 u0, ImVec2 u1) {
    const bool gba = s == System::GBA, gbc = s == System::GBC;
    const float hw = gba ? 72.f : 62.f, top = gba ? -44.f : -78.f, bot = gba ? 44.f : 74.f;
    const ImU32 base = gbc ? rgb(70, 68, 80) : (gba ? rgb(158, 160, 170) : rgb(168, 168, 164));
    const float body_b = bot - (gba ? 12.f : 16.f);
    k.shadow(-hw, top, hw, bot, 6, 12, 120);
    const float neck = hw - (gba ? 14.f : 12.f);
    shell(k, -neck, body_b - 4, neck, bot + 2, shade(base, 0.74f), 0, 0, 3, 3);          // connector foot
    if (gba) {
        shell(k, -hw, top, hw, body_b, base, 16, 16, 2, 2);                                  // rounded top
        k.rr(-hw, body_b - 5, -hw + 5, body_b, shade(base, 0.7f), 0); k.rr(hw - 5, body_b - 5, hw, body_b, shade(base, 0.7f), 0);   // shoulder steps
    } else {
        const float cut = gbc ? 18.f : 14.f;                                                 // top-right corner cut (the DMG/Colour notch)
        ImVec2 pts[] = {{-hw, top}, {hw - cut, top}, {hw, top + cut}, {hw, body_b}, {-hw, body_b}};
        ImVec2 q[5]; for (int i = 0; i < 5; i++) q[i] = k.p(pts[i].x, pts[i].y);
        k.d->AddConvexPolyFilled(q, 5, base);
        k.d->AddPolyline(q, 5, IM_COL32(255, 255, 255, 48), ImDrawFlags_Closed, std::max(1.f, 1.2f * k.s));
        k.rr(-hw + 1, top + 2, hw - cut - 3, top + 4, IM_COL32(255, 255, 255, 60), 1);
        k.rr(-hw, top + 64, -hw + 4, top + 74, shade(base, 0.6f), 1); k.rr(hw - 4, top + 64, hw, top + 74, shade(base, 0.6f), 1);   // side notches
    }
    k.rr(-hw + 1, top + 10, -hw + 3, body_b - 4, IM_COL32(255, 255, 255, 36), 1);
    k.rr(hw - 3, top + 10, hw - 1, body_b - 4, IM_COL32(0, 0, 0, 44), 1);
    const int ridges = gba ? 3 : 4;
    for (int i = 0; i < ridges; i++) {                                                       // grip ridges along the top shoulder
        const float yy = top + 6 + i * 4.f;
        k.line(-hw + (gba ? 22.f : 10.f), yy, hw - (gba ? 22.f : 24.f), yy, IM_COL32(0, 0, 0, 74), 2.f);
    }
    if (gba) {
        const float ly0 = top + 17, ly1 = body_b - 6;
        k.rr(-hw + 8, ly0 - 2, hw - 8, ly1 + 2, shade(base, 0.66f), 4);                      // label recess
        sticker(k, -hw + 15, ly0 + 3, hw - 15, ly1 - 2, t, u0, u1, rgb(80, 120, 200), 2);
    } else {
        const float ly0 = top + 24, ly1 = top + 106;
        k.rr(-hw + 8, ly0 - 4, hw - 8, ly1 + 4, shade(base, 0.66f), 5);                      // label recess
        k.rr(-hw + 8, ly0 - 4, hw - 8, ly0 - 2, IM_COL32(0, 0, 0, 60), 2);
        sticker(k, -hw + 17, ly0 + 2, hw - 17, ly1 - 2, t, u0, u1, gbc ? rgb(120, 90, 200) : rgb(120, 160, 70), 2);
    }
    pins(k, -neck + 6, neck - 6, bot - 5, bot + 1, gba ? 14 : 12);
}

void media_card(K& k, bool is3, ImTextureID t, ImVec2 u0, ImVec2 u1) {
    float hw = 54.f, top = -60.f, bot = 62.f, cut = 20.f;
    ImU32 base = is3 ? rgb(46, 48, 62) : rgb(178, 184, 198);
    k.shadow(-hw, top, hw, bot, 6, 10, 120);
    // DS card: top-right corner cut; 3DS card: top-left corner cut plus a step in the right edge
    ImVec2 pts[8];
    if (is3) { pts[0] = {-hw + cut, top}; pts[1] = {hw, top}; pts[2] = {hw, bot - 6}; pts[3] = {hw - 6, bot}; pts[4] = {-hw + 6, bot}; pts[5] = {-hw, bot - 6}; pts[6] = {-hw, top + cut}; }
    else     { pts[0] = {-hw, top}; pts[1] = {hw - cut, top}; pts[2] = {hw, top + cut}; pts[3] = {hw, bot - 6}; pts[4] = {hw - 6, bot}; pts[5] = {-hw + 6, bot}; pts[6] = {-hw, bot - 6}; }
    int n = is3 ? 7 : 7;
    ImVec2 q[8]; for (int i = 0; i < n; i++) q[i] = k.p(pts[i].x, pts[i].y);
    k.d->AddConvexPolyFilled(q, n, base);
    k.d->AddPolyline(q, n, IM_COL32(255, 255, 255, is3 ? 40 : 90), ImDrawFlags_Closed, std::max(1.f, 1.2f * k.s));
    k.rr(is3 ? -hw + cut + 4 : -hw + 3, top + 2, is3 ? hw - 3 : hw - cut - 4, top + 6, IM_COL32(255, 255, 255, is3 ? 20 : 70), 1);
    for (int i = 0; i < 3; i++) k.line(is3 ? 16.f : -hw + 8, top + 10 + i * 4.f, is3 ? hw - 6 : -hw + 34, top + 10 + i * 4.f, IM_COL32(0, 0, 0, 70), 2);
    k.rr(-hw, top + 40, -hw + 4, top + 54, IM_COL32(0, 0, 0, 80), 1);                         // side notch
    sticker(k, -38, top + 26, 38, top + 88, t, u0, u1, is3 ? rgb(60, 180, 200) : rgb(220, 130, 60), 2);
    pins(k, -34, 34, bot - 12, bot - 3, 11);
}

/* Conic rainbow sheen over the data side of a disc: two fixed (light does not spin) lobes of spectrum, brightest across the middle rings. */
void disc_rainbow(ImDrawList* d, ImVec2 c, float r0, float r1, float strength, float rot_light) {
    const int n = 72, bands = 4;
    for (int b = 0; b < bands; b++) {
        float ra = lerpf(r0, r1, (float)b / bands), rb = lerpf(r0, r1, (float)(b + 1) / bands);
        float hue = 0.82f * (1.f - (b + 0.5f) / bands);
        for (int i = 0; i < n; i++) {
            float a0 = 2 * PI * i / n, a1 = 2 * PI * (i + 1) / n, am = 0.5f * (a0 + a1);
            float lobe = std::pow(std::max(0.f, std::cos(2.f * (am - rot_light))), 3.f);
            if (lobe < 0.03f) continue;
            float cr, cg, cb; ImGui::ColorConvertHSVtoRGB(std::fmod(hue + 0.25f * (1.f - lobe), 1.f), 0.85f, 1.f, cr, cg, cb);
            ImVec2 q[4] = {ImVec2(c.x + std::cos(a0) * ra, c.y + std::sin(a0) * ra), ImVec2(c.x + std::cos(a0) * rb, c.y + std::sin(a0) * rb),
                           ImVec2(c.x + std::cos(a1) * rb, c.y + std::sin(a1) * rb), ImVec2(c.x + std::cos(a1) * ra, c.y + std::sin(a1) * ra)};
            d->AddConvexPolyFilled(q, 4, IM_COL32((int)(cr * 255), (int)(cg * 255), (int)(cb * 255), (int)(170 * strength * lobe)));
        }
    }
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
    if (s == System::PS2) { face = rgb(30, 44, 100); rim = rgb(168, 178, 214); tint = rgb(40, 70, 150); }
    else if (s == System::WiiU) { face = rgb(214, 226, 240); rim = rgb(232, 242, 255); tint = rgb(130, 190, 235); }
    else if (s == System::GC) { face = rgb(44, 46, 58); rim = rgb(176, 178, 190); tint = rgb(110, 110, 150); }
    else { face = rgb(50, 52, 58); rim = rgb(190, 192, 198); tint = rgb(150, 150, 160); }
    const bool gc = s == System::GC;
    const float rin = sR * (gc ? 0.34f : 0.28f), rout = sR * 0.97f;
    for (int i = 4; i >= 1; i--) d->AddCircleFilled(ImVec2(c.x + 3 * k.s, c.y + (6 + i * 2.5f) * k.s), sR + i * 2.5f * k.s, IM_COL32(0, 0, 0, 24), 48);
    d->AddCircleFilled(ImVec2(c.x, c.y + 2.2f * k.s), sR, shade(rim, 0.55f), 64);              // thickness
    d->AddCircleFilled(c, sR, rim, 64);
    if (g_back) {   // data side: mirror finish, rainbow diffraction, clear hub ring
        d->AddCircleFilled(c, sR * 0.985f, rgb(196, 200, 210), 64);
        d->AddCircleFilled(c, sR * 0.80f, rgb(150, 156, 170), 64);
        d->AddCircleFilled(c, sR * 0.55f, rgb(184, 188, 200), 64);
        disc_rainbow(d, c, rin, sR * 0.97f, 1.f, 0.55f);
        d->AddCircle(c, sR * 0.97f, IM_COL32(255, 255, 255, 90), 64, std::max(1.f, k.s));
    } else {
        d->AddCircleFilled(c, sR * 0.975f, face, 64);
        if (t) disc_annulus(d, c, rin, rout, rot, t, u0, u1, IM_COL32_WHITE, sR);
        else {
            d->AddCircleFilled(c, rout, tint, 64);
            d->AddCircleFilled(c, rout * 0.62f, face, 64);
            for (int i = 0; i < 6; i++) { float a = rot + i * PI / 3; d->AddLine(ImVec2(c.x + std::cos(a) * rin, c.y + std::sin(a) * rin), ImVec2(c.x + std::cos(a) * rout, c.y + std::sin(a) * rout), IM_COL32(255, 255, 255, 40), 3 * k.s); }
        }
        d->AddCircle(c, rout, IM_COL32(255, 255, 255, 40), 64, std::max(1.f, 1.2f * k.s));
        disc_rainbow(d, c, sR * 0.90f, sR * 0.985f, 0.55f, 0.55f);                                // clear outer rim catches the light
        disc_rainbow(d, c, rin, rin * 1.18f, 0.5f, 0.55f);
    }
    d->AddCircle(c, rin, IM_COL32(0, 0, 0, 90), 48, std::max(1.f, 1.5f * k.s));
    d->AddCircleFilled(c, rin * 0.98f, g_back ? IM_COL32(210, 216, 228, 190) : IM_COL32(190, 196, 210, 140), 40);
    d->AddCircle(c, rin * 0.62f, IM_COL32(255, 255, 255, 90), 32, std::max(1.f, k.s));
    d->AddCircleFilled(c, rin * 0.32f, IM_COL32(30, 32, 40, 255), 24);
    d->AddCircle(c, rin * 0.32f, IM_COL32(255, 255, 255, 120), 24, std::max(1.f, k.s));
    d->AddCircle(c, sR * 0.99f, IM_COL32(255, 255, 255, 70), 64, std::max(1.f, 1.5f * k.s));
    for (int w = 0; w < 2; w++) {   // fixed specular wedges
        float a = 0.6f + (w ? PI : 0.f), da = 0.22f;
        ImVec2 q[4] = {ImVec2(c.x + std::cos(a - da) * rin, c.y + std::sin(a - da) * rin), ImVec2(c.x + std::cos(a - da) * rout, c.y + std::sin(a - da) * rout),
                       ImVec2(c.x + std::cos(a + da) * rout, c.y + std::sin(a + da) * rout), ImVec2(c.x + std::cos(a + da) * rin, c.y + std::sin(a + da) * rin)};
        d->AddConvexPolyFilled(q, 4, IM_COL32(255, 255, 255, g_back ? 46 : 26));
    }
}
} // namespace

void set_media_back(bool back) { g_back = back; }

void draw_media(System s, ImDrawList* d, ImVec2 center, float scale, float rot, ImTextureID label, ImVec2 u0, ImVec2 u1) {
    K k{d, center, scale};
    switch (s) {
    case System::GB: case System::GBC: case System::GBA: media_gb(k, s, label, u0, u1); break;
    case System::NDS: media_card(k, false, label, u0, u1); break;
    case System::N3DS: media_card(k, true, label, u0, u1); break;
    case System::GC: media_disc(k, s, 80.f, rot, label, u0, u1); break;
    case System::PS1: case System::PS2: case System::WiiU: media_disc(k, s, 100.f, rot, label, u0, u1); break;
    case System::NES: media_nes(k, label, u0, u1); break;
    case System::SNES: media_snes(k, label, u0, u1); break;
    case System::N64: case System::Unknown: default: media_n64(k, label, u0, u1); break;
    }
}

/* ---------- shelf ---------- */
namespace {
std::vector<ImTextureID> g_covers;
}
void set_console_inner(std::function<void(ImDrawList*)> fn) { g_inner = std::move(fn); }
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
    case System::PS2: return "ps2"; case System::WiiU: return "wiiu"; case System::NES: return "nes"; case System::SNES: return "snes"; default: return "n64";
    }
}
bool art_dedicated(System s) { return s != System::Unknown; }
System art_system_from_name(const char* n) {
    if (!n) return System::Unknown;
    std::string l; for (const char* p = n; *p; p++) l += (char)std::tolower((unsigned char)*p);
    for (System s : {System::N64, System::GB, System::GBC, System::GBA, System::NDS, System::N3DS, System::GC, System::PS1, System::PS2, System::WiiU, System::NES, System::SNES})
        if (l == art_name(s)) return s;
    return System::Unknown;
}

void draw_gallery(ImDrawList* d, ImVec2 mn, ImVec2 mx, const char* which, ImTextureID cover) {
    float W = mx.x - mn.x, H = mx.y - mn.y;
    d->AddRectFilled(mn, mx, rgb(16, 17, 22));
    std::string w = which ? which : "all";
    for (auto& c : w) c = (char)std::tolower((unsigned char)c);
    if (w == "shelf") { draw_shelf(d, mn, mx, 1.f); return; }
    const System all[] = {System::N64, System::GB, System::GBC, System::GBA, System::NDS, System::N3DS, System::GC, System::PS1, System::PS2, System::WiiU, System::NES, System::SNES};
    System one = art_system_from_name(w.c_str());
    ImVec2 uv0(0, 0), uv1(1, 1);
    if (one == System::Unknown) {   // grid of everything
        int cols = 6, rows = 2; float cw = W / cols, ch = H / rows;
        for (int i = 0; i < 12; i++) {
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
