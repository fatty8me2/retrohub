/* Shelf view drawing: see shelf_draw.h. Every game is a small box (front face, one side face, a thin top) projected with a mild
 * perspective; each face is a quad that content is placed on with bilinear (u,v) coordinates, so the same code draws a cartridge label
 * face-on and a case spine swinging toward the viewer. Spine titles are drawn glyph by glyph (the font atlas quads are rotated 90
 * degrees and mapped onto the spine quad), so text foreshortens correctly as the case turns. */
#include "shelf_draw.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>

using namespace hubui::shelf;
using ui::sp;

namespace shelfdraw {

namespace {

#include "shelf_prims.inl"

/* ------------------------------- the box ------------------------------- */

struct Box {
    Face front, side, top;
    bool has_front = false, has_side = false, side_left = true;
    ImVec2 mn, mx;
    ImVec2 foot[4];   // footprint on the plank
};

Box project(const Dims& d, const SlidePose& p, float B, ImVec2 base) {
    Box b;
    const float sc = B * p.scale;
    const float hw = d.w * 0.5f * sc, dd = d.d * 0.5f * sc, H = d.h * sc;
    const float th = p.theta, ct = std::cos(th), st = std::sin(th);
    const float zc = sc * (7.0f - 8.0f * p.persp);
    ImVec2 bs(base.x, base.y + p.lift * B);
    auto pr = [&](float x, float y, float z) {
        const float xr = x * ct + z * st, zr = -x * st + z * ct;
        const float k = zc / std::max(zc * 0.25f, zc + zr);
        return ImVec2(bs.x + xr * k, bs.y - y * k - zr * 0.10f * k);
    };
    b.front.q[0] = pr(-hw, H, -dd); b.front.q[1] = pr(hw, H, -dd); b.front.q[2] = pr(hw, 0, -dd); b.front.q[3] = pr(-hw, 0, -dd);
    b.front.W = hw * 2.f; b.front.H = H;
    b.has_front = ct > 0.02f;
    if (st < -0.015f) {   // left face: back edge at the left, front edge at the right
        b.side_left = true; b.has_side = true;
        b.side.q[0] = pr(-hw, H, dd); b.side.q[1] = pr(-hw, H, -dd); b.side.q[2] = pr(-hw, 0, -dd); b.side.q[3] = pr(-hw, 0, dd);
    } else if (st > 0.015f) {
        b.side_left = false; b.has_side = true;
        b.side.q[0] = pr(hw, H, -dd); b.side.q[1] = pr(hw, H, dd); b.side.q[2] = pr(hw, 0, dd); b.side.q[3] = pr(hw, 0, -dd);
    }
    b.side.W = dd * 2.f; b.side.H = H;
    b.top.q[0] = pr(-hw, H, dd); b.top.q[1] = pr(hw, H, dd); b.top.q[2] = pr(hw, H, -dd); b.top.q[3] = pr(-hw, H, -dd);
    b.top.W = hw * 2.f; b.top.H = dd * 2.f;
    b.foot[0] = pr(-hw, 0, dd); b.foot[1] = pr(hw, 0, dd); b.foot[2] = pr(hw, 0, -dd); b.foot[3] = pr(-hw, 0, -dd);
    b.mn = ImVec2(1e9f, 1e9f); b.mx = ImVec2(-1e9f, -1e9f);
    auto acc = [&](const ImVec2& v) { b.mn.x = std::min(b.mn.x, v.x); b.mn.y = std::min(b.mn.y, v.y); b.mx.x = std::max(b.mx.x, v.x); b.mx.y = std::max(b.mx.y, v.y); };
    for (int i = 0; i < 4; ++i) { acc(b.front.q[i]); acc(b.top.q[i]); if (b.has_side) acc(b.side.q[i]); }
    return b;
}

/* ------------------------------- art / labels ------------------------------- */

struct Look {   // colours of a case family
    ImU32 body, band, band_text, title, rail;
};
Look case_look(Media m) {
    switch (m) {
        case Media::Ps1Jewel: return {rgba(24, 25, 30), rgba(150, 154, 164), rgba(24, 25, 30), rgba(240, 240, 244), rgba(150, 156, 170, 150)};
        case Media::Ps2Case: return {rgba(17, 19, 27), rgba(38, 84, 214), rgba(240, 244, 255), rgba(240, 242, 250), rgba(0, 0, 0, 0)};
        case Media::GcCase: return {rgba(78, 60, 148), rgba(196, 199, 210), rgba(52, 40, 100), rgba(244, 242, 252), rgba(0, 0, 0, 0)};
        case Media::WiiUCase: return {rgba(30, 140, 205), rgba(236, 242, 248), rgba(24, 112, 170), rgba(248, 252, 255), rgba(0, 0, 0, 0)};
        case Media::PspCase: return {rgba(32, 36, 48), rgba(176, 182, 194), rgba(28, 32, 42), rgba(236, 238, 244), rgba(0, 0, 0, 0)};
        case Media::DsCase: return {rgba(216, 218, 224), rgba(54, 144, 210), rgba(255, 255, 255), rgba(40, 42, 52), rgba(255, 255, 255, 120)};
        case Media::N3dsCase: return {rgba(242, 242, 246), rgba(214, 56, 60), rgba(255, 255, 255), rgba(40, 42, 52), rgba(255, 255, 255, 120)};
        default: return {rgba(52, 54, 64), rgba(128, 132, 144), rgba(248, 248, 250), rgba(240, 240, 244), rgba(0, 0, 0, 0)};
    }
}

ImU32 seed_color(uint32_t seed, float s, float v) { return hsv((float)(seed % 360u) / 360.f, s, v); }

/* A label / cover: the cached art when there is some, else a printed placeholder with the title. */
void art_or_print(Ctx& x, const Face& f, const Item& it, float u0, float v0, float u1, float v1, bool big_title, float anchor_y) {
    if (it.art) {
        image(x, f, u0, v0, u1, v1, it.art, it.art_w, it.art_h, anchor_y);
        return;
    }
    const ImU32 top = lerpc(it.c0, seed_color(it.seed, 0.5f, 0.95f), 0.45f), bot = shade(lerpc(it.c1, seed_color(it.seed >> 5, 0.7f, 0.5f), 0.5f), 0.75f);
    vgrad(x, f, u0, v0, u1, v1, top, bot);
    const float w = (u1 - u0) * f.W, h = (v1 - v0) * f.H;
    // a bold shape so the labels differ at a glance
    const int shape = (int)((it.seed >> 3) % 3u);
    const ImU32 sh = alpha(seed_color(it.seed >> 9, 0.35f, 1.f), 0.55f);
    if (shape == 0) x.dl->AddCircleFilled(f.at(u0 + (u1 - u0) * 0.5f, v0 + (v1 - v0) * 0.38f), std::min(w, h) * 0.26f, x.c(sh), 20);
    else if (shape == 1) quad(x, f, u0, v0 + (v1 - v0) * 0.55f, u1, v0 + (v1 - v0) * 0.75f, sh);
    else poly(x, f, {ImVec2(u0, v1), ImVec2(u0 + (u1 - u0) * 0.5f, v0 + (v1 - v0) * 0.2f), ImVec2(u1, v1)}, sh);
    ImFont* font = ui::F.bold ? ui::F.bold : ui::F.body;
    float size = big_title ? std::min(h * 0.12f, w * 0.14f) : std::min(h * 0.17f, w * 0.15f);
    if (size >= 6.f && font) {
        std::vector<std::string> ln = ui::wrap_lines(font, size, it.title, w - size, big_title ? 4 : 3);
        float ty = v0 * f.H + (big_title ? h * 0.58f : h * 0.30f);
        for (const std::string& l : ln) {
            const float tw = text_w(font, size, l);
            face_text(x, f, font, size, l.c_str(), u0 * f.W + (w - tw) * 0.5f + 1.f, ty + 1.f, rgba(0, 0, 0, 120), false);
            face_text(x, f, font, size, l.c_str(), u0 * f.W + (w - tw) * 0.5f, ty, rgba(255, 255, 255, 245), false);
            ty += size * 1.12f;
        }
    } else {   // too small to read: two lines of "text"
        quad(x, f, u0 + (u1 - u0) * 0.2f, v0 + (v1 - v0) * 0.36f, u1 - (u1 - u0) * 0.2f, v0 + (v1 - v0) * 0.46f, rgba(255, 255, 255, 190));
        quad(x, f, u0 + (u1 - u0) * 0.3f, v0 + (v1 - v0) * 0.54f, u1 - (u1 - u0) * 0.3f, v0 + (v1 - v0) * 0.62f, rgba(255, 255, 255, 140));
    }
    vgrad(x, f, u0, v0, u1, v0 + (v1 - v0) * 0.4f, rgba(255, 255, 255, 46), rgba(255, 255, 255, 0));   // gloss
}

/* ------------------------------- cartridges ------------------------------- */

void paint_cart(Ctx& x, const Face& f, const Item& it) {
    const Media m = it.media;
    const float W = f.W, H = f.H;
    const float px = 1.f / std::max(1.f, W), py = 1.f / std::max(1.f, H);   // one pixel in u / v
    ImU32 body = rgba(150, 150, 146);
    float cb_ = 0.05f, ctl = 0.f, ctr = 0.f;   // bottom / top-left / top-right corner cuts (fraction of width)
    float lu0 = 0.11f, lv0 = 0.10f, lu1 = 0.89f, lv1 = 0.60f;   // label
    ImU32 frame = rgba(60, 60, 58);
    switch (m) {
        case Media::NesCart: body = rgba(140, 140, 135); ctl = ctr = 0.05f; lu0 = 0.13f; lu1 = 0.87f; lv0 = 0.09f; lv1 = 0.50f; break;
        case Media::SnesCart: body = rgba(176, 176, 171); ctl = ctr = 0.09f; lv1 = 0.62f; break;
        case Media::N64Cart: body = rgba(72, 72, 80); lu0 = 0.10f; lu1 = 0.90f; lv0 = 0.14f; lv1 = 0.62f; frame = rgba(214, 214, 220); break;
        case Media::GbCart: body = rgba(168, 170, 174); ctr = 0.13f; lu0 = 0.12f; lu1 = 0.88f; lv0 = 0.09f; lv1 = 0.52f; break;
        case Media::GbcCart: {
            static const ImU32 pal[] = {rgba(46, 46, 56), rgba(34, 156, 156), rgba(118, 76, 178), rgba(196, 56, 70), rgba(206, 214, 224), rgba(232, 188, 44)};
            body = pal[(it.seed >> 4) % 6u]; ctl = 0.07f; lu0 = 0.12f; lu1 = 0.88f; lv0 = 0.09f; lv1 = 0.52f; break;
        }
        default: body = rgba(100, 102, 114); cb_ = 0.09f; lu0 = 0.12f; lu1 = 0.88f; lv0 = 0.10f; lv1 = 0.58f; break;   // GBA
    }
    // silhouette
    poly(x, f, {ImVec2(ctl, 0.f), ImVec2(1.f - ctr, 0.f), ImVec2(1.f, ctr * W * py), ImVec2(1.f, 1.f - cb_ * W * py), ImVec2(1.f - cb_, 1.f), ImVec2(cb_, 1.f), ImVec2(0.f, 1.f - cb_ * W * py), ImVec2(0.f, ctl * W * py)}, body);
    // shading: lit from the top left
    vgrad(x, f, 0.f, 0.f, 1.f, 0.5f, rgba(255, 255, 255, 52), rgba(255, 255, 255, 0));
    vgrad(x, f, 0.f, 0.5f, 1.f, 1.f - cb_ * W * py, rgba(0, 0, 0, 0), rgba(0, 0, 0, 58));
    hgrad(x, f, 0.f, ctl * W * py, 0.05f, 1.f - cb_ * W * py, rgba(255, 255, 255, 40), rgba(255, 255, 255, 0));
    hgrad(x, f, 0.93f, ctr * W * py, 1.f, 1.f - cb_ * W * py, rgba(0, 0, 0, 0), rgba(0, 0, 0, 46));
    // shape details per console
    if (m == Media::NesCart) {
        quad(x, f, 0.f, 0.f, 1.f, 0.045f, rgba(0, 0, 0, 40));
        quad(x, f, 0.12f, 0.60f, 0.88f, 0.93f, rgba(0, 0, 0, 52));
        for (int i = 0; i < 6; ++i) { const float v = 0.63f + 0.05f * (float)i; quad(x, f, 0.14f, v, 0.86f, v + 2.f * py, rgba(0, 0, 0, 80)); quad(x, f, 0.14f, v + 2.f * py, 0.86f, v + 3.5f * py, rgba(255, 255, 255, 44)); }
        quad(x, f, 0.f, 0.94f, 1.f, 1.f, rgba(0, 0, 0, 70));
        quad(x, f, 0.035f, 0.1f, 0.075f, 0.6f, rgba(0, 0, 0, 36)); quad(x, f, 0.925f, 0.1f, 0.965f, 0.6f, rgba(0, 0, 0, 36));
    } else if (m == Media::SnesCart) {
        quad(x, f, 0.2f, 0.69f, 0.8f, 0.99f, rgba(0, 0, 0, 78));
        for (int i = 0; i < 9; ++i) { const float u = 0.24f + 0.065f * (float)i; quad(x, f, u, 0.72f, u + 2.f * px, 0.96f, rgba(255, 255, 255, 36)); }
        for (int i = 0; i < 3; ++i) { const float u = 0.03f + 0.03f * (float)i; quad(x, f, u, 0.16f, u + 2.f * px, 0.62f, rgba(0, 0, 0, 56)); quad(x, f, 0.97f - u - 0.01f, 0.16f, 0.97f - u - 0.01f + 2.f * px, 0.62f, rgba(0, 0, 0, 56)); }
    } else if (m == Media::N64Cart) {
        quad(x, f, 0.38f, 0.f, 0.62f, 0.09f, rgba(0, 0, 0, 120));   // the top notch
        quad(x, f, 0.f, 0.f, 1.f, 0.035f, rgba(255, 255, 255, 26));
        for (int i = 0; i < 16; ++i) { const float u = 0.12f + 0.049f * (float)i; quad(x, f, u, 0.70f, u + 2.f * px, 0.95f, rgba(255, 255, 255, 38)); }
        quad(x, f, 0.1f, 0.68f, 0.9f, 0.70f, rgba(0, 0, 0, 80));
        quad(x, f, 0.f, 0.97f, 1.f, 1.f, rgba(0, 0, 0, 80));
    } else if (m == Media::GbCart || m == Media::GbcCart) {
        for (int i = 0; i < 4; ++i) { const float v = 0.58f + 0.055f * (float)i; quad(x, f, 0.12f, v, 0.88f, v + 2.f * py, rgba(0, 0, 0, 60)); quad(x, f, 0.12f, v + 2.f * py, 0.88f, v + 3.f * py, rgba(255, 255, 255, 36)); }
        quad(x, f, 0.18f, 0.84f, 0.82f, 1.f, rgba(0, 0, 0, 70));
        if (m == Media::GbcCart) quad(x, f, 0.f, 0.f, 0.08f, 0.1f, rgba(0, 0, 0, 40));
    } else {   // GBA
        for (int i = 0; i < 4; ++i) { const float v = 0.66f + 0.07f * (float)i; quad(x, f, 0.12f, v, 0.88f, v + 2.f * py, rgba(0, 0, 0, 70)); }
        quad(x, f, 0.4f, 0.86f, 0.6f, 1.f, rgba(0, 0, 0, 90));
    }
    // label
    if (m != Media::N64Cart) frame = shade(body, 0.45f);
    frame = x.c(frame);
    quad(x, f, lu0 - 4.f * px, lv0 - 4.f * py, lu1 + 4.f * px, lv1 + 4.f * py, frame);
    art_or_print(x, f, it, lu0, lv0, lu1, lv1, false, 0.15f);
    outline(x, f, lu0, lv0, lu1, lv1, rgba(0, 0, 0, 90), 1.f);
    // the console colour stripe under the label (the "logo band" in colour only)
    quad(x, f, lu0, lv1 + 6.f * py, lu1, lv1 + 6.f * py + std::max(2.f * py, 0.025f), alpha(it.c1, 0.9f));
}

/* ------------------------------- cases ------------------------------- */

void paint_spine(Ctx& x, const Face& f, const Item& it) {
    const Look lk = case_look(it.media);
    const float W = f.W, H = f.H;
    const float px = 1.f / std::max(1.f, W);
    const bool jewel = it.media == Media::Ps1Jewel;
    const bool pale = lum(lk.body) > 0.6f;
    quad(x, f, 0.f, 0.f, 1.f, 1.f, lk.body);
    hgrad(x, f, 0.f, 0.f, 1.f, 1.f, rgba(255, 255, 255, pale ? 40 : 34), rgba(0, 0, 0, pale ? 38 : 70));   // a rounded, left-lit edge
    if (jewel) {   // the clear plastic rails and the black spine card
        quad(x, f, 0.f, 0.f, 0.14f, 1.f, lk.rail);
        quad(x, f, 0.86f, 0.f, 1.f, 1.f, shade(lk.rail, 0.7f));
        vgrad(x, f, 0.14f, 0.f, 0.86f, 1.f, rgba(255, 255, 255, 22), rgba(0, 0, 0, 0));
    } else if (it.media == Media::DsCase || it.media == Media::N3dsCase) {
        quad(x, f, 0.08f, 0.f, 0.92f, 0.012f, rgba(255, 255, 255, 120));   // clear plastic rim
    }
    // colour band with the console name
    const float bh = std::min(0.17f, std::max(0.1f, 30.f / H));
    quad(x, f, 0.f, 0.f, 1.f, bh, lk.band);
    vgrad(x, f, 0.f, 0.f, 1.f, bh, rgba(255, 255, 255, 50), rgba(0, 0, 0, 36));
    quad(x, f, 0.f, bh, 1.f, bh + 1.5f / H, rgba(0, 0, 0, 90));
    ImFont* cf = ui::F.bold ? ui::F.bold : ui::F.cap;
    float bs = std::min(W * 0.46f, bh * H * 0.5f);
    const float tw = text_w(cf, bs, it.sys_name);
    if (tw > W * 0.92f) bs *= (W * 0.92f) / tw;
    if (bs >= 5.f) face_text(x, f, cf, bs, it.sys_name.c_str(), (W - text_w(cf, bs, it.sys_name)) * 0.5f, bh * H * 0.5f - bs * 0.5f, lk.band_text, false);
    // the title, reading top to bottom
    ImFont* font = ui::F.bold ? ui::F.bold : ui::F.body;
    float size = std::max(7.f, std::min(W * 0.58f, 15.f));
    const float avail = H * (0.90f - bh) - size * 0.4f;
    std::string t = ui::ellipsize(font, size, it.title, avail);
    ImU32 tc = lk.title;
    if (size >= 6.f) {
        face_text(x, f, font, size, t.c_str(), W * 0.5f + size * 0.36f + 0.6f, bh * H + H * 0.03f + 0.6f, rgba(0, 0, 0, pale ? 0 : 110), true);
        face_text(x, f, font, size, t.c_str(), W * 0.5f + size * 0.36f, bh * H + H * 0.03f, tc, true);
    }
    // the publisher stripe at the foot (gold for favourites)
    const ImU32 st = it.fav ? rgba(255, 206, 64) : seed_color(it.seed >> 3, 0.55f, 0.85f);
    quad(x, f, 0.f, 0.925f, 1.f, 0.965f, st);
    quad(x, f, 0.f, 0.965f, 1.f, 1.f, rgba(0, 0, 0, 60));
    line(x, f, 0.f + px, 0.f, 0.f + px, 1.f, rgba(255, 255, 255, pale ? 120 : 70), 1.f);
}

void paint_cover(Ctx& x, const Face& f, const Item& it) {
    const Look lk = case_look(it.media);
    const float W = f.W, H = f.H;
    const float px = 1.f / std::max(1.f, W), py = 1.f / std::max(1.f, H);
    const bool jewel = it.media == Media::Ps1Jewel;
    const bool pale = lum(lk.body) > 0.6f;
    // the plastic: body colour, a hinge strip along the spine edge (left) and a darker lip
    quad(x, f, 0.f, 0.f, 1.f, 1.f, jewel ? rgba(188, 194, 206) : lk.body);
    vgrad(x, f, 0.f, 0.f, 1.f, 1.f, rgba(255, 255, 255, 38), rgba(0, 0, 0, 40));
    const float b = std::max(0.035f, 6.f * px);   // side border
    const float bv = std::max(0.03f, 6.f * py);
    const float hinge = jewel ? 0.085f : 0.055f;
    quad(x, f, 0.f, 0.f, hinge, 1.f, shade(jewel ? rgba(210, 214, 224) : lk.body, 0.78f));
    for (int i = 0; i < 3; ++i) { const float v = 0.12f + 0.3f * (float)i; quad(x, f, 0.f, v, hinge * 0.9f, v + 0.14f, rgba(0, 0, 0, 34)); }
    const float cu0 = hinge + (jewel ? 0.01f : 0.0f), cv0 = bv * (jewel ? 1.1f : 0.8f), cu1 = 1.f - b * 0.8f, cv1 = 1.f - bv * 0.8f;
    // the cover art
    const bool wide = it.media == Media::Ps1Jewel;   // jewel cases are close to square
    (void)wide;
    art_or_print(x, f, it, cu0, cv0, cu1, cv1, true, 0.0f);
    // the console colour band along the top of the cover (the "logo band" in colour only)
    quad(x, f, cu0, cv0, cu1, cv0 + std::max(0.045f, 9.f * py), alpha(lk.band, 0.95f));
    ImFont* cf = ui::F.bold ? ui::F.bold : ui::F.cap;
    const float bs = std::min(H * 0.036f, 13.f);
    if (bs >= 5.f) face_text(x, f, cf, bs, it.sys_name.c_str(), cu0 * W + 5.f, cv0 * H + (std::max(0.045f, 9.f * py) * H - bs) * 0.5f, lk.band_text, false);
    outline(x, f, cu0, cv0, cu1, cv1, rgba(0, 0, 0, 100), 1.f);
    // glass / plastic sheen
    grad(x, f, 0.f, 0.f, 1.f, 0.55f, rgba(255, 255, 255, 54), rgba(255, 255, 255, 12), rgba(255, 255, 255, 0), rgba(255, 255, 255, 0));
    outline(x, f, 0.f, 0.f, 1.f, 1.f, pale ? rgba(0, 0, 0, 80) : rgba(255, 255, 255, 40), 1.f);
    if (it.fav) x.dl->AddCircleFilled(f.at(0.88f, 0.07f), std::max(5.f, W * 0.07f), x.c(rgba(255, 206, 64)), 16);
}

void paint_top(Ctx& x, const Face& f, const Item& it, float k) {
    ImU32 c = is_cart(it.media) ? rgba(190, 190, 186) : case_look(it.media).body;
    if (is_cart(it.media) && it.media == Media::N64Cart) c = rgba(104, 104, 114);
    if (is_cart(it.media) && it.media == Media::GbcCart) c = rgba(70, 70, 78);
    quad(x, f, 0.f, 0.f, 1.f, 1.f, lerpc(shade(c, 1.12f), rgba(255, 255, 255), 0.18f * k));
}

}  // namespace

/* ======================================================================= */

RowGeo row_geo(float B, float cap_font_px) {
    RowGeo g;
    g.B = B;
    g.head = B * 0.30f;
    g.depth = std::max(B * 0.20f, sp(1.8f));
    g.edge = std::max(B * 0.17f, cap_font_px + sp(1.1f));
    g.side = std::max(sp(1.1f), B * 0.075f);
    g.pitch = g.head + B + g.depth + g.edge;
    g.base_dy = g.head + B + g.depth * 0.55f;
    return g;
}

namespace {
struct Wood { ImU32 back_top, back_bot, plank0, plank1, edge0, edge1, side0, side1, hi; };
Wood wood() {
    const ImU32 base = lerpc(rgba(128, 84, 50), ui::ACCENT, 0.10f);
    const float l = lum(ui::BG);
    const float bright = l < 0.35f ? 0.88f : l < 0.6f ? 1.0f : 1.18f;
    auto w = [&](float k) { return shade(base, k * bright); };
    Wood o;
    o.back_top = w(0.26f); o.back_bot = w(0.50f);
    o.plank0 = w(1.0f); o.plank1 = w(1.22f);
    o.edge0 = w(0.92f); o.edge1 = w(0.62f);
    o.side0 = w(0.78f); o.side1 = w(0.52f);
    o.hi = w(1.55f);
    return o;
}
}  // namespace

void bay(ImDrawList* dl, const RowGeo& g, float x0, float x1, float y, const std::string& label, const std::string& count_text, ImU32 chip, bool section_start) {
    const Wood w = wood();
    const float side = g.side;
    const float py = y + g.head + g.B;          // back edge of the plank top
    const float fy = py + g.depth;              // front edge top
    const float ey = fy + g.edge;               // bottom of the row
    // back panel: dark at the top (shaded by the plank above), lighter toward the plank
    dl->AddRectFilledMultiColor(ImVec2(x0 + side, y), ImVec2(x1 - side, py), w.back_top, w.back_top, w.back_bot, w.back_bot);
    // vertical boards and grain
    const float board = std::max(sp(7.f), g.B * 0.9f);
    for (float bx = x0 + side + board; bx < x1 - side - 1.f; bx += board) {
        dl->AddLine(ImVec2(bx, y), ImVec2(bx, py), IM_COL32(0, 0, 0, 70), 1.f);
        dl->AddLine(ImVec2(bx + 1.f, y), ImVec2(bx + 1.f, py), IM_COL32(255, 255, 255, 10), 1.f);
    }
    for (float gx = x0 + side + 7.f; gx < x1 - side - 4.f; gx += 11.f) {
        const uint32_t h = hubui::shelf::hash32(std::to_string((int)(gx / 11.f)));
        if (h & 3u) continue;
        const float gy0 = y + (float)(h % 29u) / 29.f * g.head * 0.5f;
        dl->AddLine(ImVec2(gx, gy0), ImVec2(gx + 0.5f, py - (float)((h >> 8) % 17u)), IM_COL32(0, 0, 0, 22), 1.f);
    }
    // ambient shadow from the plank above and from the uprights
    dl->AddRectFilledMultiColor(ImVec2(x0 + side, y), ImVec2(x1 - side, y + g.head * 0.9f), IM_COL32(0, 0, 0, 120), IM_COL32(0, 0, 0, 120), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
    dl->AddRectFilledMultiColor(ImVec2(x0 + side, y), ImVec2(x0 + side + g.B * 0.35f, py), IM_COL32(0, 0, 0, 80), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 80));
    dl->AddRectFilledMultiColor(ImVec2(x1 - side - g.B * 0.35f, y), ImVec2(x1 - side, y), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
    dl->AddRectFilledMultiColor(ImVec2(x1 - side - g.B * 0.35f, y), ImVec2(x1 - side, py), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 70), IM_COL32(0, 0, 0, 70), IM_COL32(0, 0, 0, 0));
    // uprights
    dl->AddRectFilledMultiColor(ImVec2(x0, y), ImVec2(x0 + side, ey), w.side0, w.side1, w.side1, w.side0);
    dl->AddRectFilledMultiColor(ImVec2(x1 - side, y), ImVec2(x1, ey), w.side1, w.side0, w.side0, w.side1);
    dl->AddLine(ImVec2(x0 + side, y), ImVec2(x0 + side, ey), IM_COL32(0, 0, 0, 110), 1.f);
    dl->AddLine(ImVec2(x1 - side, y), ImVec2(x1 - side, ey), IM_COL32(0, 0, 0, 110), 1.f);
    // plank top (the surface the games stand on): darker at the back, lighter at the front
    dl->AddRectFilledMultiColor(ImVec2(x0 + side, py), ImVec2(x1 - side, fy), w.plank0, w.plank0, w.plank1, w.plank1);
    dl->AddRectFilledMultiColor(ImVec2(x0 + side, py), ImVec2(x1 - side, py + g.depth * 0.35f), IM_COL32(0, 0, 0, 90), IM_COL32(0, 0, 0, 90), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
    for (int i = 1; i < 4; ++i) {   // long grain lines running along the plank
        const float gy = py + g.depth * (float)i / 4.f;
        dl->AddLine(ImVec2(x0 + side, gy), ImVec2(x1 - side, gy), IM_COL32(0, 0, 0, 16), 1.f);
    }
    // front edge: a bright top bevel, then the face fading darker
    dl->AddRectFilledMultiColor(ImVec2(x0, fy), ImVec2(x1, ey), w.edge0, w.edge0, w.edge1, w.edge1);
    dl->AddLine(ImVec2(x0, fy), ImVec2(x1, fy), w.hi, 1.5f);
    dl->AddLine(ImVec2(x0, fy + 1.5f), ImVec2(x1, fy + 1.5f), IM_COL32(255, 255, 255, 28), 1.f);
    dl->AddLine(ImVec2(x0, ey - 0.5f), ImVec2(x1, ey - 0.5f), IM_COL32(0, 0, 0, 150), 1.5f);
    // plaque
    if (!label.empty()) {
        ImFont* f = ui::F.bold ? ui::F.bold : ui::F.cap;
        const float fs = ui::F.cap ? ui::F.cap->FontSize : 12.f;
        const float ph = g.edge - sp(0.6f);
        const float pw = text_w(f, fs, label) + (count_text.empty() ? 0.f : text_w(ui::F.cap, fs, count_text) + sp(0.8f)) + sp(2.6f);
        const ImVec2 p0(x0 + side + sp(1.2f), fy + (g.edge - ph) * 0.5f), p1(p0.x + pw, p0.y + ph);
        const ImU32 brass0 = lerpc(rgba(222, 186, 108), ui::ACCENT, 0.08f), brass1 = shade(brass0, 0.74f);
        dl->AddRectFilledMultiColor(p0, p1, brass0, brass0, brass1, brass1);
        dl->AddRect(p0, p1, IM_COL32(60, 40, 12, 200), 2.f, 0, 1.f);
        dl->AddLine(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p0.y + 1), IM_COL32(255, 255, 255, 110), 1.f);
        dl->AddRectFilled(ImVec2(p0.x + sp(0.45f), p0.y + ph * 0.5f - sp(0.3f)), ImVec2(p0.x + sp(0.45f) + sp(0.6f), p0.y + ph * 0.5f + sp(0.3f)), chip, 2.f);
        float tx = p0.x + sp(1.5f);
        dl->AddText(f, fs, ImVec2(tx, p0.y + (ph - fs) * 0.5f), IM_COL32(52, 34, 10, 255), label.c_str());
        if (!count_text.empty()) dl->AddText(ui::F.cap, fs, ImVec2(tx + text_w(f, fs, label) + sp(0.8f), p0.y + (ph - fs) * 0.5f), IM_COL32(52, 34, 10, section_start ? 190 : 120), count_text.c_str());
    }
}

void item(ImDrawList* dl, const Item& it, float B, ImVec2 base, const SlidePose& pose, bool ring, ImVec2* bmin, ImVec2* bmax) {
    Ctx x{dl, it.ghost ? 0.62f : 0.f};
    const Box b = project(it.dims, pose, B, base);
    const float s = pose.shadow;
    const bool cart = is_cart(it.media);
    // contact shadow on the plank (stays on the shelf while the game is pulled forward)
    {
        const float grow = 1.f + 0.20f * s;
        const ImVec2 c((b.foot[0].x + b.foot[2].x) * 0.5f, base.y + 0.01f * B);
        ImVec2 p[4];
        for (int i = 0; i < 4; ++i) p[i] = ImVec2(c.x + (b.foot[i].x - c.x) * grow + B * 0.02f * (1.f + s), base.y + (b.foot[i].y - (b.foot[0].y + b.foot[2].y) * 0.5f) * grow + B * 0.035f);
        for (int k = 0; k < 3; ++k) {
            const float e = (float)k * B * 0.018f;
            const ImVec2 q[4] = {ImVec2(p[0].x - e, p[0].y - e * 0.5f), ImVec2(p[1].x + e * 1.5f, p[1].y - e * 0.5f), ImVec2(p[2].x + e * 1.5f, p[2].y + e), ImVec2(p[3].x - e, p[3].y + e)};
            dl->AddConvexPolyFilled(q, 4, IM_COL32(0, 0, 0, (int)((60 + 34 * s) / (1.f + (float)k * 0.7f))));
        }
    }
    // soft drop shadow on the back panel, cast to the right
    if (b.has_front) {
        const float off = B * (0.035f + 0.07f * s);
        for (int k = 0; k < 2; ++k) {
            const float e = (float)k * B * 0.02f;
            const ImVec2 q[4] = {ImVec2(b.front.q[0].x + off - e, b.front.q[0].y - e), ImVec2(b.front.q[1].x + off + e, b.front.q[1].y - e), ImVec2(b.front.q[2].x + off + e, b.front.q[2].y), ImVec2(b.front.q[3].x + off - e, b.front.q[3].y)};
            dl->AddConvexPolyFilled(q, 4, IM_COL32(0, 0, 0, (int)((50 + 30 * s) / (1.f + (float)k))));
        }
    }
    if (s > 0.3f) ui::glow(dl, b.mn, b.mx, sp(0.6f), it.c1, 0.5f * std::min(1.f, s));
    // the game card peeking out of the top of a DS / 3DS case
    if (pose.card > 0.01f && b.has_front) {
        const Face& f = b.front;
        Face c = f;
        const float lift = 0.17f * pose.card;
        for (int i = 0; i < 4; ++i) c.q[i] = f.at(i == 0 || i == 3 ? 0.2f : 0.8f, i < 2 ? -lift : 0.2f);
        c.W = f.W * 0.6f; c.H = f.H * (0.2f + lift);
        poly(x, c, {ImVec2(0.f, 0.f), ImVec2(1.f, 0.f), ImVec2(1.f, 1.f), ImVec2(0.f, 1.f)}, rgba(88, 90, 98));
        quad(x, c, 0.08f, 0.1f, 0.92f, 0.62f, rgba(122, 126, 136));
        quad(x, c, 0.f, 0.f, 0.14f, 0.28f, rgba(30, 30, 36, 160));
        quad(x, c, 0.18f, 0.8f, 0.82f, 0.96f, rgba(210, 190, 90));   // contacts
    }
    if (b.has_side) {
        Face sd = b.side;
        const bool left = b.side_left;
        if (cart) {
            // a plain plastic edge: the left one lit, the right one in shade
            const ImU32 body = it.media == Media::N64Cart ? rgba(80, 80, 88) : it.media == Media::GbcCart ? rgba(60, 60, 68) : rgba(150, 150, 146);
            quad(x, sd, 0.f, 0.f, 1.f, 1.f, shade(body, left ? 0.92f : 0.58f));
            quad(x, sd, left ? 0.f : 0.82f, 0.f, left ? 0.18f : 1.f, 1.f, rgba(0, 0, 0, 44));
        } else if (left) {
            paint_spine(x, sd, it);
        } else {
            quad(x, sd, 0.f, 0.f, 1.f, 1.f, shade(case_look(it.media).body, 0.55f));
        }
    }
    if (b.top.q[0].y != b.top.q[3].y) paint_top(x, b.top, it, 0.f);
    if (b.has_front) {
        if (cart) paint_cart(x, b.front, it);
        else paint_cover(x, b.front, it);
        if (cart && it.fav) {
            const ImVec2 sp0 = b.front.at(0.86f, 0.08f);
            dl->AddCircleFilled(sp0, B * 0.055f, IM_COL32(0, 0, 0, 140), 16);
            ui::icon(dl, ui::Icon::StarFilled, sp0, B * 0.09f, IM_COL32(255, 214, 70, 255));
        }
        // darken the face slightly while it turns away
        const float th = std::fabs(pose.theta);
        if (th > 0.2f) {
            dl->AddQuadFilled(b.front.q[0], b.front.q[1], b.front.q[2], b.front.q[3], IM_COL32(0, 0, 0, (int)std::min(70.f, th * 38.f)));
        }
    }
    if (ring) {
        ImVec2 mn(b.mn.x - 2.f, b.mn.y - 2.f), mx(b.mx.x + 2.f, b.mx.y + 2.f);
        ui::focus_ring(dl, mn, mx, sp(0.5f));
    }
    if (bmin) *bmin = b.mn;
    if (bmax) *bmax = b.mx;
}

void caption(ImDrawList* dl, ImVec2 anchor, const std::string& title, const std::string& sub, float x0, float x1, float a) {
    if (a <= 0.01f) return;
    ImFont* f = ui::F.bold ? ui::F.bold : ui::F.body;
    ImFont* cf = ui::F.cap;
    const float maxw = std::min(sp(30.f), x1 - x0 - sp(2.f));
    std::string t = ui::ellipsize(f, f->FontSize, title, maxw - sp(2.f));
    const float tw = std::max(text_w(f, f->FontSize, t), cf ? text_w(cf, cf->FontSize, sub) : 0.f);
    const float w = tw + sp(2.f), h = f->FontSize + (cf && !sub.empty() ? cf->FontSize + sp(0.2f) : 0.f) + sp(1.f);
    float px = anchor.x - w * 0.5f;
    px = std::max(x0 + sp(0.5f), std::min(px, x1 - w - sp(0.5f)));
    const ImVec2 p0(px, anchor.y), p1(px + w, anchor.y + h);
    dl->AddRectFilled(ImVec2(p0.x + 2.f, p0.y + 3.f), ImVec2(p1.x + 2.f, p1.y + 3.f), IM_COL32(0, 0, 0, (int)(90 * a)), sp(0.7f));
    dl->AddRectFilled(p0, p1, ui::with_alpha(ui::PANEL3, 0.96f * a), sp(0.7f));
    dl->AddRect(p0, p1, ui::scale_alpha(ui::HAIRLINE, a), sp(0.7f), 0, 1.f);
    dl->AddText(f, f->FontSize, ImVec2(p0.x + (w - text_w(f, f->FontSize, t)) * 0.5f, p0.y + sp(0.5f)), ui::scale_alpha(ui::TEXT, a), t.c_str());
    if (cf && !sub.empty())
        dl->AddText(cf, cf->FontSize, ImVec2(p0.x + (w - text_w(cf, cf->FontSize, sub)) * 0.5f, p0.y + sp(0.5f) + f->FontSize + sp(0.2f)), ui::scale_alpha(ui::TEXT2, a), sub.c_str());
}

}  // namespace shelfdraw
