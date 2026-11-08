/* Cases view drawing: see shelf_draw.h (Cases section). Every game is a 3D box (front cover, back cover, two side faces) rotated about the
 * vertical axis and projected with a real perspective camera; each face is a quad that content is placed on with bilinear (u,v)
 * coordinates, exactly like the Shelf view's boxes (shelf_prims.inl is shared). A box is convex, so back-face culling alone gives the right
 * picture; the carousel draws the cases far to near. Reflections mirror the same geometry below the floor, then a veil in the floor colour
 * fades them out. Plastic keep cases are glossy with a clear rim; cardboard boxes (cartridge systems) are matte and a little thicker. */
#include "shelf_draw.h"
#include "cases_logic.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

using namespace hubui::shelf;
using hubui::cases::CaseDims;
using hubui::cases::CaseKind;
using ui::sp;

namespace shelfdraw {

namespace {

#include "shelf_prims.inl"

ImU32 seed_color(uint32_t seed, float s, float v) { return hsv((float)(seed % 360u) / 360.f, s, v); }
uint64_t fnv(const char* s, uint64_t h = 1469598103934665603ull) { for (; *s; ++s) h = (h ^ (unsigned char)*s) * 1099511628211ull; return h; }

/* ------------------------------- text caches (no per-frame allocation once warm) ------------------------------- */

/* A tiny cache keyed by (text, size, width bucket): the width is floored to 16 px buckets and the size to whole pixels, so a case that glides
 * (its face width changes every frame) keeps hitting the same entry instead of re-wrapping. When full, only the least recently used entry is
 * dropped (never the whole cache). Returned references are valid only until the next call of the same cache function. */
template <class V>
struct TextCache {
    struct E { V v; uint64_t stamp = 0; };
    std::unordered_map<uint64_t, E> map;
    uint64_t tick = 0;
    size_t cap = 0;
    const V* find(uint64_t k) { auto it = map.find(k); if (it == map.end()) return nullptr; it->second.stamp = ++tick; return &it->second.v; }
    const V& put(uint64_t k, V&& v) {
        if (map.size() >= cap) {
            auto old = map.begin();
            for (auto it = map.begin(); it != map.end(); ++it) if (it->second.stamp < old->second.stamp) old = it;
            map.erase(old);
        }
        E& e = map[k];
        e.v = std::move(v);
        e.stamp = ++tick;
        return e.v;
    }
};
float text_bucket(float w) { return std::max(16.f, std::floor(w / 16.f) * 16.f); }

const std::vector<std::string>& wrap_cached(ImFont* f, float size, const char* text, float w, int maxl) {
    static TextCache<std::vector<std::string>> cache = [] { TextCache<std::vector<std::string>> c; c.cap = 192; return c; }();
    size = std::floor(size);
    w = text_bucket(w);
    uint64_t k = fnv(text);
    k = (k ^ (uint64_t)(int)size) * 1099511628211ull;
    k = (k ^ (uint64_t)(int)(w * (1.f / 16.f))) * 1099511628211ull;
    k = (k ^ (uint64_t)maxl) * 1099511628211ull;
    if (const auto* hit = cache.find(k)) return *hit;
    return cache.put(k, ui::wrap_lines(f, size, text, w, maxl));
}
const std::string& ellipsize_cached(ImFont* f, float size, const char* text, float w) {
    static TextCache<std::string> cache = [] { TextCache<std::string> c; c.cap = 480; return c; }();
    size = std::floor(size);
    w = text_bucket(w);
    uint64_t k = fnv(text, 7);
    k = (k ^ (uint64_t)(int)size) * 1099511628211ull;
    k = (k ^ (uint64_t)(int)(w * (1.f / 16.f))) * 1099511628211ull;
    if (const auto* hit = cache.find(k)) return *hit;
    return cache.put(k, ui::ellipsize(f, size, text, w));
}
void upper_into(char* out, size_t n, const char* s) {
    size_t i = 0;
    for (; s && s[i] && i + 1 < n; ++i) out[i] = (char)std::toupper((unsigned char)s[i]);
    out[i] = 0;
}

/* ------------------------------- 3D ------------------------------- */

struct V3 { float x = 0, y = 0, z = 0; };
struct Xf {
    float ct = 1, st = 0, cp = 1, sp_ = 0, yc = 0.5f, sc = 1, ox = 0, oy = 0, oz = 0;
    bool mirror = false;
    V3 rot(float x, float y, float z) const {   // yaw (about the vertical axis) then pitch (about the horizontal axis through the centre)
        const float xr = x * ct + z * st, zr = -x * st + z * ct;
        return {xr, y * cp - zr * sp_, y * sp_ + zr * cp};
    }
    V3 dir(float x, float y, float z) const { V3 r = rot(x, y, z); if (mirror) r.y = -r.y; return r; }
    V3 pt(float x, float y, float z) const {
        V3 r = rot(x * sc, (y - yc) * sc, z * sc);
        r.x += ox; r.y += oy + yc * sc; r.z += oz;
        if (mirror) r.y = -r.y;
        return r;
    }
};
float depth_k(const CaseScene& c, float z) { return c.dist / std::max(0.4f, c.dist + z); }
ImVec2 proj(const CaseScene& c, const V3& p) { const float s = depth_k(c, p.z); return ImVec2(c.cx + p.x * s * c.U, c.hy + (c.eye - p.y) * s * c.U); }

float face_ratio(const Face& f) {   // how wide the face looks on screen compared with face-on (1 = square on)
    const float a = std::hypot(f.q[1].x - f.q[0].x, f.q[1].y - f.q[0].y), b = std::hypot(f.q[2].x - f.q[3].x, f.q[2].y - f.q[3].y);
    return std::min(a, b) / std::max(1.f, f.W);
}
/* One face of the box: corners in local space TL TR BR BL as seen from outside, its outward normal and its size in world units. */
bool make_face(const CaseScene& sc, const Xf& xf, const float c[4][3], const float n[3], float wu, float hu, Face* out) {
    V3 w[4];
    V3 mid;
    for (int i = 0; i < 4; ++i) { w[i] = xf.pt(c[i][0], c[i][1], c[i][2]); mid.x += w[i].x * 0.25f; mid.y += w[i].y * 0.25f; mid.z += w[i].z * 0.25f; }
    const V3 nw = xf.dir(n[0], n[1], n[2]);
    const V3 to_cam{-mid.x, sc.eye - mid.y, -sc.dist - mid.z};
    if (nw.x * to_cam.x + nw.y * to_cam.y + nw.z * to_cam.z <= 0.f) return false;
    for (int i = 0; i < 4; ++i) out->q[i] = proj(sc, w[i]);
    const float k = depth_k(sc, mid.z) * sc.U * xf.sc;
    out->W = std::max(1.f, wu * k);
    out->H = std::max(1.f, hu * k);
    return true;
}

/* ------------------------------- the look of each console's retail packaging ------------------------------- */

enum class Ban { Top, Left, Bottom, Corner, Frame };   // where the console banner sits on the cover
struct Style {
    bool box = false, pale = false, nx2 = false, line = false, mixed = false;
    Media m = Media::Generic;
    Ban ban = Ban::Top;
    ImU32 body = 0, spine = 0, band = 0, band_text = 0, accent = 0, ink = 0, sink = 0, panel = 0;
};

/* NTSC-U retail packaging per media: the plastic / card colour, the spine colour, the banner (colour and wordmark colour), a thin accent
 * line, the back-cover panel. Generated colours (c0 / c1) are only used for an unknown console. */
Style style_of(const CaseDraw& d) {
    Style s;
    s.m = d.media;
    s.box = hubui::cases::is_box(d.media);
    s.nx2 = d.nx2 && d.media == Media::SwitchCase;
    auto set = [&](ImU32 body, ImU32 spine, ImU32 band, ImU32 text, ImU32 accent, ImU32 panel) { s.body = body; s.spine = spine; s.band = band; s.band_text = text; s.accent = accent; s.panel = panel; };
    switch (d.media) {
        case Media::GcCase: set(rgba(23, 23, 28), rgba(21, 21, 26), rgba(192, 196, 206), rgba(24, 26, 34), rgba(120, 124, 138), rgba(17, 17, 23)); break;
        case Media::Ps2Case: set(rgba(16, 17, 22), rgba(14, 15, 20), rgba(9, 10, 14), rgba(246, 247, 252), rgba(40, 94, 216), rgba(13, 14, 19)); s.line = true; break;
        case Media::WiiUCase: set(rgba(26, 130, 202), rgba(22, 118, 188), rgba(0, 152, 208), rgba(255, 255, 255), rgba(214, 240, 252), rgba(22, 104, 166)); s.line = true; s.mixed = true; break;
        case Media::SwitchCase:
            if (s.nx2) { set(rgba(178, 12, 26), rgba(160, 10, 24), rgba(228, 0, 18), rgba(255, 255, 255), rgba(255, 255, 255), rgba(232, 232, 236)); }   // Switch 2: one wide Nintendo-red band, white wordmark
            else set(rgba(204, 208, 218), rgba(202, 14, 28), rgba(234, 32, 48), rgba(255, 255, 255), rgba(255, 255, 255), rgba(232, 232, 236));   // Switch 1: clear plastic around a red-banded sleeve (Switch 2 is red-tinted)
            s.line = !s.nx2;   // the Switch 2 band has no white rule under it
            break;
        case Media::PspCase: set(rgba(94, 100, 114), rgba(18, 20, 28), rgba(12, 14, 20), rgba(246, 247, 252), rgba(72, 122, 232), rgba(20, 22, 30)); s.line = true; break;
        case Media::DsCase: set(rgba(24, 25, 31), rgba(24, 25, 31), rgba(228, 231, 238), rgba(34, 38, 50), rgba(58, 148, 214), rgba(236, 238, 242)); s.line = true; break;   // NTSC-U / NTSC-J DS cases are black plastic (Europe: clear)
        case Media::N3dsCase: set(rgba(242, 243, 246), rgba(247, 247, 250), rgba(252, 252, 253), rgba(210, 36, 44), rgba(210, 36, 44), rgba(250, 250, 252)); s.line = true; break;   // 3DS cases are white plastic
        case Media::Ps1Jewel: set(rgba(176, 182, 194), rgba(22, 23, 28), rgba(14, 14, 18), rgba(246, 247, 252), rgba(150, 154, 166), rgba(22, 23, 30)); s.ban = Ban::Left; break;
        case Media::NesCart: set(rgba(26, 26, 30), rgba(28, 28, 32), rgba(16, 16, 20), rgba(226, 226, 232), rgba(124, 126, 134), rgba(34, 34, 40)); s.ban = Ban::Frame; break;
        case Media::SnesCart: set(rgba(204, 204, 210), rgba(186, 186, 196), rgba(148, 150, 164), rgba(28, 30, 42), rgba(116, 92, 176), rgba(224, 224, 230)); s.ban = Ban::Bottom; break;
        case Media::N64Cart: set(rgba(44, 46, 56), rgba(38, 40, 50), rgba(22, 24, 32), rgba(250, 250, 253), rgba(214, 214, 222), rgba(40, 42, 52)); s.ban = Ban::Corner; break;
        case Media::GbCart: set(rgba(176, 178, 174), rgba(164, 166, 162), rgba(72, 76, 84), rgba(240, 240, 244), rgba(50, 54, 62), rgba(206, 208, 204)); s.ban = Ban::Left; break;
        case Media::GbcCart: set(rgba(238, 238, 242), rgba(228, 228, 234), rgba(130, 70, 190), rgba(255, 255, 255), rgba(130, 70, 190), rgba(244, 244, 247)); break;
        case Media::GbaCart: set(rgba(230, 230, 238), rgba(54, 44, 122), rgba(54, 46, 124), rgba(255, 255, 255), rgba(150, 140, 220), rgba(236, 236, 242)); break;
        default: {
            ImU32 band = lerpc(d.c0, d.c1, 0.4f);
            if (lum(band) < 0.2f) band = lerpc(band, rgba(255, 255, 255), 0.22f);
            set(rgba(52, 54, 64), rgba(52, 54, 64), band, lum(band) > 0.6f ? rgba(22, 24, 32) : rgba(250, 250, 253), band, lerpc(rgba(52, 54, 64), rgba(8, 9, 14), 0.62f));
            break;
        }
    }
    if (s.box && d.art_col) {   // cardboard is printed in the game's colours: the sides, flaps and the paper back follow the front art (never the cover itself)
        float kb = 0.3f, kp = 0.15f;
        switch (d.media) {
            case Media::N64Cart: kb = 0.62f; kp = 0.72f; break;   // N64: the whole box wears the game's colour
            case Media::GbCart: case Media::GbcCart: case Media::GbaCart: kb = 0.38f; kp = 0.14f; break;
            case Media::NesCart: kb = 0.22f; kp = 0.3f; break;
            default: break;
        }
        const bool dark_panel = lum(s.panel) < 0.5f;
        s.body = lerpc(s.body, shade(d.art_col, dark_panel ? 0.55f : 0.8f), kb);
        s.panel = lerpc(s.panel, dark_panel ? shade(d.art_col, 0.34f) : lerpc(rgba(255, 255, 255), d.art_col, 0.3f), kp);
    }
    s.pale = lum(s.body) > 0.55f;
    s.ink = lum(s.panel) > 0.55f ? rgba(28, 30, 40) : rgba(244, 245, 250);   // ink on the back-cover panel
    s.sink = lum(s.spine) > 0.55f ? rgba(28, 30, 40) : rgba(244, 245, 250);  // ink on the spine
    return s;
}

float tw_c(ImFont* f, float size, const char* s) { return f ? f->CalcTextSizeA(size, FLT_MAX, 0.f, s).x : 0.f; }   // no std::string temporary

/* The wordmark text of the banner: upper case, except for the Wii U; a Switch 2 game reads "NINTENDO SWITCH 2". */
void banner_name(char* out, size_t n, const CaseDraw& d, const Style& st) {
    if (st.nx2) { std::snprintf(out, n, "NINTENDO SWITCH 2"); return; }
    if (st.mixed) { std::snprintf(out, n, "%s", d.sys_name); return; }
    upper_into(out, n, d.sys_name);
}

/* Fits `s` into [u0,u1] of the face, centred vertically on vc; the size follows bh_px (the banner height), shrunk to fit. align 0 = left, 1 = centre. */
void label_h(Ctx& x, const Face& f, const char* s, float u0, float u1, float vc, float bh_px, ImU32 col, int align, float size_k = 0.52f, bool shadow = false) {
    ImFont* cf = ui::F.bold ? ui::F.bold : ui::F.cap;
    if (!cf || !x.detail || !s || !*s) return;
    const float track_k = 0.12f, len = (float)std::strlen(s);
    float bs = bh_px * size_k;
    const float avail = (u1 - u0) * f.W * 0.88f;
    const float w0 = tw_c(cf, bs, s) + len * bs * track_k;
    if (w0 > avail) bs *= avail / w0;
    if (bs < 5.f) return;
    const float tr = bs * track_k, w = tw_c(cf, bs, s) + len * tr;
    const float lx = align ? (u0 + u1) * 0.5f * f.W - w * 0.5f : u0 * f.W + (u1 - u0) * f.W * 0.05f + 3.f;
    const float ly = vc * f.H - bs * 0.5f;
    if (shadow) face_text(x, f, cf, bs, s, lx + 1.2f, ly + 1.2f, rgba(0, 0, 0, 110), false, tr);
    face_text(x, f, cf, bs, s, lx, ly, col, false, tr);
}
/* The same, running down a vertical strip [u0,u1] x [v0,v1] (reads top to bottom); top_aligned starts near the top end. */
void label_v(Ctx& x, const Face& f, const char* s, float u0, float u1, float v0, float v1, ImU32 col, bool top_aligned) {
    ImFont* cf = ui::F.bold ? ui::F.bold : ui::F.cap;
    if (!cf || !x.detail || !s || !*s) return;
    const float sw = (u1 - u0) * f.W, track_k = 0.14f, len = (float)std::strlen(s);
    float bs = sw * 0.5f;
    const float avail = (v1 - v0) * f.H * 0.9f;
    const float w0 = tw_c(cf, bs, s) + len * bs * track_k;
    if (w0 > avail) bs *= avail / w0;
    if (bs < 5.f) return;
    const float tr = bs * track_k, w = tw_c(cf, bs, s) + len * tr;
    const float lx = (u0 + u1) * 0.5f * f.W + bs * 0.38f;
    const float ly = top_aligned ? v0 * f.H + (v1 - v0) * f.H * 0.05f : (v0 + v1) * 0.5f * f.H - w * 0.5f;
    face_text(x, f, cf, bs, s, lx, ly, col, true, tr);
}

void ngon(Ctx& x, const Face& f, float cu, float cv, float ru, float rv, int n, ImU32 col) {
    ImVec2 p[64];
    n = std::min(n, 64);
    for (int i = 0; i < n; ++i) { const float a = 6.2831853f * (float)i / (float)n; p[i] = f.at(cu + ru * std::cos(a), cv + rv * std::sin(a)); }
    x.dl->AddConvexPolyFilled(p, n, x.c(col));
}

/* A tiny generated mark for a console (not a logo): its shape comes from the console colours. */
void console_mark(Ctx& x, const Face& f, float cu, float cv, float s_px, uint32_t h, ImU32 col) {
    const float du = s_px / f.W * 0.5f, dv = s_px / f.H * 0.5f;
    switch (h % 4u) {
        case 0: poly(x, f, {ImVec2(cu, cv - dv), ImVec2(cu + du, cv), ImVec2(cu, cv + dv), ImVec2(cu - du, cv)}, col); break;
        case 1: for (int i = -1; i <= 1; ++i) quad(x, f, cu + du * 0.8f * (float)i - du * 0.22f, cv - dv * 0.22f, cu + du * 0.8f * (float)i + du * 0.22f, cv + dv * 0.22f, col); break;
        case 2: quad(x, f, cu - du, cv - dv * 0.55f, cu + du, cv - dv * 0.2f, col); quad(x, f, cu - du * 0.6f, cv + dv * 0.2f, cu + du * 0.6f, cv + dv * 0.55f, col); break;
        default: ngon(x, f, cu, cv, du * 0.75f, dv * 0.75f, 10, col); break;
    }
}

/* ------------------------------- the generated cover (no art) ------------------------------- */

void cover_generated(Ctx& x, const Face& f, float u0, float v0, float u1, float v1, const CaseDraw& d) {
    const ImU32 top = lerpc(d.c0, seed_color(d.seed, 0.5f, 0.95f), 0.32f), bot = shade(lerpc(d.c1, seed_color(d.seed >> 5, 0.7f, 0.5f), 0.4f), 0.66f);
    vgrad(x, f, u0, v0, u1, v1, top, bot);
    const float w = u1 - u0, h = v1 - v0;
    // diagonal stripes in a console-coloured pattern
    const ImU32 stripe = alpha(lerpc(d.c0, rgba(255, 255, 255), 0.5f), 0.26f);
    for (int k = 0; k < 6; ++k) {
        const float ua = u0 + w * (-0.1f + 0.24f * (float)k), sw = w * 0.07f, sk = w * 0.5f;
        auto cl = [&](float u) { return std::max(u0, std::min(u1, u)); };
        poly(x, f, {ImVec2(cl(ua), v0), ImVec2(cl(ua + sw), v0), ImVec2(cl(ua + sw - sk), v1), ImVec2(cl(ua - sk), v1)}, stripe);
    }
    // a bold motif so covers differ at a glance
    const int shape = (int)((d.seed >> 3) % 3u);
    const ImU32 sh = alpha(seed_color(d.seed >> 9, 0.35f, 1.f), 0.5f);
    const float pw = w * f.W, ph = h * f.H, rr = std::min(pw, ph) * 0.24f;
    if (shape == 0) { ngon(x, f, u0 + w * 0.5f, v0 + h * 0.34f, rr / f.W, rr / f.H, 48, sh); ngon(x, f, u0 + w * 0.5f, v0 + h * 0.34f, rr * 0.62f / f.W, rr * 0.62f / f.H, 48, alpha(rgba(255, 255, 255), 0.22f)); }
    else if (shape == 1) { quad(x, f, u0, v0 + h * 0.26f, u1, v0 + h * 0.42f, sh); quad(x, f, u0, v0 + h * 0.46f, u1, v0 + h * 0.5f, alpha(sh, 0.6f)); }
    else poly(x, f, {ImVec2(u0 + w * 0.12f, v0 + h * 0.5f), ImVec2(u0 + w * 0.5f, v0 + h * 0.12f), ImVec2(u0 + w * 0.88f, v0 + h * 0.5f)}, sh);
    ImFont* font = ui::F.bold ? ui::F.bold : ui::F.body;
    const float size = std::max(6.f, std::min(ph * 0.092f, pw * 0.13f));
    if (x.detail && size >= 7.f && font) {
        const std::vector<std::string>& ln = wrap_cached(font, size, d.title, pw * 0.86f, 4);
        float ty = v0 * f.H + ph * 0.58f;
        for (const std::string& l : ln) {
            const float tw = text_w(font, size, l);
            face_text(x, f, font, size, l.c_str(), u0 * f.W + (pw - tw) * 0.5f + 1.5f, ty + 1.5f, rgba(0, 0, 0, 130), false);
            face_text(x, f, font, size, l.c_str(), u0 * f.W + (pw - tw) * 0.5f, ty, rgba(255, 255, 255, 248), false);
            ty += size * 1.14f;
        }
    } else {
        quad(x, f, u0 + w * 0.2f, v0 + h * 0.6f, u1 - w * 0.2f, v0 + h * 0.66f, rgba(255, 255, 255, 200));
        quad(x, f, u0 + w * 0.3f, v0 + h * 0.7f, u1 - w * 0.3f, v0 + h * 0.75f, rgba(255, 255, 255, 140));
    }
    vgrad(x, f, u0, v0, u1, v0 + h * 0.4f, rgba(255, 255, 255, 40), rgba(255, 255, 255, 0));
}

/* ------------------------------- faces ------------------------------- */

/* The top banner [u0,u1] x [v0,v1] with the wordmark (front covers with a top banner, and every back cover). */
void top_band(Ctx& x, const Face& f, const CaseDraw& d, const Style& st, float u0, float v0, float u1, float v1) {
    const float py = 1.f / f.H;
    const float bh = (v1 - v0) * f.H;
    v0 -= 1.5f * py;   // a hair over the cover's top edge, so no sliver of cover shows (a dashed light line) above the band
    switch (st.m) {
        case Media::GbcCart: {   // the colourful band
            const ImU32 c[5] = {rgba(0, 168, 176), rgba(112, 72, 196), rgba(226, 64, 146), rgba(250, 150, 40), rgba(250, 206, 50)};
            for (int i = 0; i < 4; ++i) hgrad(x, f, u0 + (u1 - u0) * 0.25f * (float)i, v0, u0 + (u1 - u0) * 0.25f * (float)(i + 1), v1, c[i], c[i + 1]);
            break;
        }
        case Media::SwitchCase:
            quad(x, f, u0, v0, u1, v1, st.band);   // (Switch 2 too: one red band, no second box)
            break;
        default: quad(x, f, u0, v0, u1, v1, st.band); break;
    }
    vgrad(x, f, u0, v0, u1, v1, rgba(255, 255, 255, st.box ? 30 : 54), rgba(0, 0, 0, st.box ? 24 : 40));
    if (st.line) quad(x, f, u0, v1, u1, v1 + std::max(1.5f, bh * 0.07f) * py, st.accent);
    else quad(x, f, u0, v1, u1, v1 + 1.5f * py, rgba(0, 0, 0, 90));
    char name[48];
    banner_name(name, sizeof name, d, st);
    const float vc = (v0 + v1) * 0.5f;
    if (st.m == Media::SwitchCase) {
        label_h(x, f, name, u0, u1, vc, bh, st.band_text, 1, st.nx2 ? 0.42f : 0.66f);   // Switch 2: the whole wordmark, centred, in one white
    } else label_h(x, f, name, u0, u1, vc, bh, st.band_text, 0, 0.58f, st.m == Media::GbcCart);
    if (st.m == Media::Generic) console_mark(x, f, u1 - (u1 - u0) * 0.07f - bh * 0.3f / f.W, vc, bh * 0.46f, hash32(d.sys_short), st.band_text);
}

/* The Switch 2 red band is a wide strip across the top of the cover (Switch 1's is slim): ~12.5% of the cover on the front, less on the back (text room). */
constexpr float kNx2BandFront = 0.125f, kNx2BandBack = 0.10f;

struct Lay { float au0, av0, au1, av1, bu0, bv0, bu1, bv1, cv1; };   // the art window, the banner rect, the bottom of the cover
Lay front_lay(const Style& st, const Face& f, float u0, float v0, float u1, float v1) {
    const float px = 1.f / f.W, py = 1.f / f.H;
    Lay l{u0, v0, u1, v1, u0, v0, u1, v0, v1};
    switch (st.ban) {
        case Ban::Top: {
            float bv = std::max(0.05f, std::min(0.085f, 27.f * py));
            if (st.m == Media::SwitchCase) bv = st.nx2 ? kNx2BandFront : std::max(0.045f, std::min(0.07f, 22.f * py));
            else if (st.m == Media::GbcCart || st.m == Media::GbaCart) bv = std::max(0.07f, std::min(0.11f, 34.f * py));
            l.bv1 = v0 + bv; l.av0 = l.bv1;
            break;
        }
        case Ban::Left: { l.bu1 = u0 + (st.m == Media::Ps1Jewel ? 0.105f : 0.14f); l.bv1 = v1; l.au0 = l.bu1 + 1.5f * px; break; }
        case Ban::Bottom: { const float bv = std::max(0.08f, std::min(0.115f, 30.f * py)); l.bv0 = v1 - bv; l.bv1 = v1; l.av1 = l.bv0; break; }
        case Ban::Corner: { l.bu1 = u0 + 0.58f; l.bv1 = v0 + std::max(0.07f, std::min(0.1f, 30.f * py)); break; }
        case Ban::Frame: {   // NES: a black frame with the wordmark in the top bar
            l.bv1 = v0 + std::max(0.055f, std::min(0.09f, 26.f * py));
            l.au0 = u0 + 0.06f; l.au1 = u1 - 0.06f; l.av0 = l.bv1 + 2.f * py; l.av1 = v1 - 0.08f;
            break;
        }
    }
    return l;
}

void banner_front(Ctx& x, const Face& f, const CaseDraw& d, const Style& st, const Lay& l) {
    const float px = 1.f / f.W, py = 1.f / f.H;
    const float bh = (l.bv1 - l.bv0) * f.H, vc = (l.bv0 + l.bv1) * 0.5f;
    char name[48];
    banner_name(name, sizeof name, d, st);
    switch (st.ban) {
        case Ban::Top: top_band(x, f, d, st, l.bu0, l.bv0, l.bu1, l.bv1); break;
        case Ban::Left: {   // a vertical strip down the left edge with the wordmark running down it
            quad(x, f, l.bu0, l.bv0, l.bu1, l.bv1, st.band);
            hgrad(x, f, l.bu0, l.bv0, l.bu1, l.bv1, rgba(255, 255, 255, 30), rgba(0, 0, 0, 44));
            quad(x, f, l.bu1 - 3.5f * px, l.bv0, l.bu1 - 1.5f * px, l.bv1, st.accent);
            quad(x, f, l.bu1, l.bv0, l.bu1 + 1.5f * px, l.bv1, rgba(0, 0, 0, 90));
            label_v(x, f, name, l.bu0, l.bu1 - 4.f * px, l.bv0, l.bv1, st.band_text, st.m == Media::Ps1Jewel);
            break;
        }
        case Ban::Bottom: {   // SNES: a grey stripe along the foot with the system name and four little buttons
            quad(x, f, l.bu0, l.bv0, l.bu1, l.bv1, st.band);
            vgrad(x, f, l.bu0, l.bv0, l.bu1, l.bv1, rgba(255, 255, 255, 46), rgba(0, 0, 0, 30));
            quad(x, f, l.bu0, l.bv0 - 2.f * py, l.bu1, l.bv0, st.accent);
            label_h(x, f, name, l.bu0, l.bu0 + (l.bu1 - l.bu0) * 0.72f, vc, bh, st.band_text, 0);
            const ImU32 bc[4] = {rgba(222, 62, 62), rgba(62, 168, 92), rgba(64, 108, 214), rgba(244, 204, 60)};
            const float r = bh * 0.15f;
            for (int i = 0; i < 4; ++i) ngon(x, f, l.bu1 - 0.05f - (3.f - (float)i) * r * 2.9f * px, vc, r * px, r * py, 12, bc[i]);
            break;
        }
        case Ban::Corner: {   // N64: a dark corner with the wordmark
            const float sl = 0.07f;
            poly(x, f, {ImVec2(l.bu0, l.bv0), ImVec2(l.bu1, l.bv0), ImVec2(l.bu1 - sl, l.bv1), ImVec2(l.bu0, l.bv1)}, st.band);
            line(x, f, l.bu0, l.bv1, l.bu1 - sl, l.bv1, st.accent, 1.5f);
            label_h(x, f, name, l.bu0, l.bu1 - sl, vc, bh, st.band_text, 0);
            break;
        }
        case Ban::Frame: {
            quad(x, f, l.bu0, l.bv0, l.bu1, l.bv1, st.band);
            label_h(x, f, name, l.bu0, l.bu0 + (l.bu1 - l.bu0) * 0.74f, vc, bh, st.band_text, 0);
            for (int i = -1; i <= 1; ++i) quad(x, f, l.bu0 + (l.bu1 - l.bu0) * 0.78f, vc + (float)i * bh * 0.2f * py - 0.8f * py, l.bu1 - 0.04f, vc + (float)i * bh * 0.2f * py + 0.8f * py, st.accent);
            outline(x, f, l.au0 - 2.f * px, l.av0 - 2.f * py, l.au1 + 2.f * px, l.av1 + 2.f * py, alpha(st.accent, 0.7f), 1.f);   // the thin grid motif
            const float gh = l.cv1 - l.av1;
            for (int i = 0; i < 3; ++i) { const float yy = l.av1 + gh * (0.34f + 0.2f * (float)i); quad(x, f, l.bu0 + 0.04f, yy - 0.8f * py, l.bu1 - 0.04f, yy + 0.8f * py, alpha(st.accent, 0.8f - 0.2f * (float)i)); }
            break;
        }
    }
}

/* ------------------------------- real art sources ------------------------------- */

/* A cover image as (texture, size of the part in pixels, uv sub-rect). A full cover scan serves three of these (back / spine / front) from one
 * texture; nothing is copied or uploaded per frame. */
struct Src {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;
    ImVec2 a{0.f, 0.f}, b{1.f, 1.f};
};
constexpr bool kFrontFromFull = true;   // with a full cover scan the front comes from it too: all three faces are then the same edition and print

hubui::cases::FullParts full_parts(const CaseDraw& d) { return hubui::cases::full_cover_parts(d.full_w, d.full_back_x1, d.full_front_x0); }
bool has_full(const CaseDraw& d) { return d.full && d.full_w > 8 && d.full_h > 8 && d.full_back_x1 > 4 && d.full_front_x0 < d.full_w - 4 && d.full_front_x0 >= d.full_back_x1; }
Src front_src(const CaseDraw& d) {
    Src s;
    if (kFrontFromFull && has_full(d)) {
        const hubui::cases::FullParts p = full_parts(d);
        s.tex = d.full; s.w = d.full_w - d.full_front_x0; s.h = d.full_h; s.a = ImVec2(p.front.u0, 0.f); s.b = ImVec2(1.f, 1.f);
    } else if (d.art) { s.tex = d.art; s.w = d.art_w; s.h = d.art_h; }
    return s;
}
Src back_src(const CaseDraw& d) {
    Src s;
    if (has_full(d)) { const hubui::cases::FullParts p = full_parts(d); s.tex = d.full; s.w = d.full_back_x1; s.h = d.full_h; s.a = ImVec2(0.f, 0.f); s.b = ImVec2(p.back.u1, 1.f); }
    return s;
}
Src spine_src(const CaseDraw& d) {
    Src s;
    if (has_full(d) && d.full_front_x0 - d.full_back_x1 >= 3) {
        const hubui::cases::FullParts p = full_parts(d);
        s.tex = d.full; s.w = d.full_front_x0 - d.full_back_x1; s.h = d.full_h; s.a = ImVec2(p.spine.u0, 0.f); s.b = ImVec2(p.spine.u1, 1.f);
    }
    return s;
}
/* Draws the sub-rect [a,b] of `tex` over the face rect exactly (no crop: callers fit the rect to the picture first). */
void image_uv(Ctx& x, const Face& f, float u0, float v0, float u1, float v1, SDL_Texture* tex, ImVec2 a, ImVec2 b, float bright = 1.f) {
    const int k = cl8(255 * bright);
    ImU32 col = IM_COL32(k, k, k, 255);
    if (x.ghost > 0.f) col = lerpc(col, IM_COL32(150, 152, 160, 255), x.ghost * 0.6f);
    if (x.a < 1.f) col = alpha(col, x.a);
    x.dl->AddImageQuad((ImTextureID)tex, f.at(u0, v0), f.at(u1, v0), f.at(u1, v1), f.at(u0, v1), a, ImVec2(b.x, a.y), b, ImVec2(a.x, b.y), col);
}
/* The paper the scan is printed on when it does not fill the window: the art's own colour, darkened (the case colour when unknown). */
ImU32 insert_col(const CaseDraw& d, const Style& st, bool letterbox) {
    const ImU32 base = d.art_col ? shade(d.art_col, 0.42f) : st.panel;
    return letterbox ? base : lerpc(st.body, base, 0.5f);
}
ImU32 on_color(ImU32 c) { return lum(c) > 0.55f ? rgba(28, 30, 40) : rgba(244, 245, 250); }

/* The 8x8 blur colours (texel centres at (i+.5)/8, flat beyond the outer centres) sampled bilinearly at (su, sv) in 0..1. */
ImU32 blur_at(const uint32_t* c, float su, float sv) {
    auto axis = [](float s, int* i0, int* i1, float* t) {
        const float g = std::max(0.f, std::min(7.f, s * 8.f - 0.5f));
        *i0 = (int)g; *i1 = std::min(7, *i0 + 1); *t = g - (float)*i0;
    };
    int x0, x1, y0, y1; float tx, ty;
    axis(su, &x0, &x1, &tx); axis(sv, &y0, &y1, &ty);
    return lerpc(lerpc(c[y0 * 8 + x0], c[y0 * 8 + x1], tx), lerpc(c[y1 * 8 + x0], c[y1 * 8 + x1], tx), ty);
}
/* Fills [u0,u1] x [v0,v1] with the blur: sample v runs sv0 -> sv1 down the rect (sv0 > sv1 mirrors it), sample u runs 0 -> 1. The surface is a mesh
 * with a vertex per texel centre and the colour interpolated per vertex (smooth in any renderer, no texture needed); alpha runs a0 -> a1 down it. */
void blur_fill(Ctx& x, const Face& f, float u0, float v0, float u1, float v1, const uint32_t* c, float sv0, float sv1, float a0, float a1) {
    float sv[10], su[10];
    int nv = 0, nu = 0;
    const float lo = std::min(sv0, sv1), hi = std::max(sv0, sv1);
    sv[nv++] = lo;
    for (int j = 0; j < 8; ++j) { const float t = ((float)j + 0.5f) / 8.f; if (t > lo + 1e-4f && t < hi - 1e-4f) sv[nv++] = t; }
    sv[nv++] = hi;
    for (int i = 0; i < 8; ++i) su[nu++] = ((float)i + 0.5f) / 8.f;
    const float rng = sv1 - sv0;
    auto vof = [&](float s) { return v0 + (v1 - v0) * ((s - sv0) / (std::fabs(rng) < 1e-6f ? 1e-6f : rng)); };
    auto col = [&](float s_u, float s_v) { const float t = (s_v - sv0) / (std::fabs(rng) < 1e-6f ? 1e-6f : rng); ImU32 k = blur_at(c, s_u, s_v); return alpha(k, a0 + (a1 - a0) * t); };
    float eu[10]; eu[0] = 0.f; for (int i = 0; i < 8; ++i) eu[i + 1] = su[i]; eu[9] = 1.f;
    for (int r = 0; r + 1 < nv; ++r) {
        const float sa = sv[r], sb = sv[r + 1];
        float va = vof(sa), vb = vof(sb);
        if (va > vb) std::swap(va, vb);
        for (int q = 0; q < 9; ++q) {
            const float ua = eu[q], ub = eu[q + 1];
            if (ub - ua < 1e-5f) continue;
            // corners: top of the rect is the sample end nearer sv0
            const float st = (std::fabs(sa - sv0) <= std::fabs(sb - sv0)) ? sa : sb, sbm = (st == sa) ? sb : sa;
            grad(x, f, u0 + (u1 - u0) * ua, std::min(va, vb), u0 + (u1 - u0) * ub, std::max(va, vb), col(ua, st), col(ub, st), col(ub, sbm), col(ua, sbm));
        }
    }
}

/* A square scan (a Switch game's icon) on a portrait case: the red console banner, the icon edge to edge directly under it, and below it a
 * smooth blur of the icon's own colours (an 8x8 average of the art, mirrored so its last row continues the icon) that darkens toward the foot. */
void front_square(Ctx& x, const Face& f, const CaseDraw& d, const Style& st, const Src& s, const hubui::cases::UvRect& win) {
    const float W = f.W, H = f.H, py = 1.f / H;
    const float bv = st.nx2 ? kNx2BandFront : std::max(0.045f, std::min(0.07f, 22.f * py));
    const float u0 = win.u0, u1 = win.u1, v0 = win.v0 + bv, v1 = win.v1;
    const float AW = (u1 - u0) * W, AH = (v1 - v0) * H;
    const float side = std::min(AW, AH), iv1 = v0 + side * py;   // icon rect: u0..u1 x v0..iv1
    const float lowh = (v1 - iv1) * H;
    const ImU32 base = d.art_col ? d.art_col : seed_color(d.seed, 0.55f, 0.6f);
    const uint32_t* bl = d.art_blur;
    if (lowh > 0.5f) {
        if (bl) {   // the mirror: the sample runs from the icon's last row (1) back up the picture
            const float k = std::min(1.f, lowh / std::max(1.f, side));
            blur_fill(x, f, u0, iv1, u1, v1, bl, 1.f, 1.f - k, 1.f, 1.f);
        } else vgrad(x, f, u0, iv1, u1, v1, shade(base, 0.9f), shade(base, 0.9f));
        vgrad(x, f, u0, iv1, u1, v1, rgba(0, 0, 0, 20), rgba(0, 0, 0, 175));   // darker toward the foot
    }
    image_uv(x, f, u0, v0, u1, iv1, s.tex, s.a, s.b);
    {   // a soft fade over the icon's bottom edge into the blur: no hard seam
        const float fh = side * 0.16f, v_a = iv1 - fh * py;
        if (bl) blur_fill(x, f, u0, v_a, u1, iv1, bl, 1.f - fh / std::max(1.f, side), 1.f, 0.f, 1.f);
        else vgrad(x, f, u0, v_a, u1, iv1, alpha(shade(base, 0.9f), 0.f), shade(base, 0.9f));
        vgrad(x, f, u0, v_a, u1, iv1, rgba(0, 0, 0, 0), rgba(0, 0, 0, 20));
    }
    vgrad(x, f, u0, v0, u1, v0 + side * 0.1f * py, rgba(0, 0, 0, 46), rgba(0, 0, 0, 0));   // the banner's shadow on the art
    ImFont* font = ui::F.bold ? ui::F.bold : ui::F.body;   // a small, quiet title in the foot (the icon already shows the logo)
    const float size = std::max(7.f, std::min(std::min(W * 0.052f, lowh * 0.2f), 17.f));
    if (x.detail && font && lowh > size * 2.6f) {
        const std::vector<std::string>& ln = wrap_cached(font, size, d.title, AW * 0.8f, 2);
        const float th = size * 1.14f * (float)ln.size();
        float ty = iv1 * H + (lowh - th) * 0.5f;
        for (const std::string& l : ln) {
            const float tw = text_w(font, size, l), lx = (u0 + u1) * 0.5f * W - tw * 0.5f;
            face_text(x, f, font, size, l.c_str(), lx + 1.f, ty + 1.f, rgba(0, 0, 0, 120), false);
            face_text(x, f, font, size, l.c_str(), lx, ty, rgba(255, 255, 255, 200), false);
            ty += size * 1.14f;
        }
    }
    quad(x, f, u0, v1 - 4.f * py, u1, v1, alpha(st.band, 0.9f));
    top_band(x, f, d, st, u0, win.v0, u1, v0);
}

/* The classic square-art front, kept as it was for every non-Switch portrait case (GameCube, PS2, Wii U, PSP): a square scan (a Switch game's icon) on a portrait case: the red console banner, the icon large over a background made of its own colours
 * (the middle of the icon blown up into a soft blur, darkened), the title underneath. */
void front_square_classic(Ctx& x, const Face& f, const CaseDraw& d, const Style& st, const Src& s, const hubui::cases::UvRect& win) {
    const float W = f.W, H = f.H, px = 1.f / W, py = 1.f / H;
    const float bv = std::max(0.045f, std::min(0.07f, 22.f * py));
    const float u0 = win.u0, u1 = win.u1, v0 = win.v0 + bv, v1 = win.v1;
    const float AW = (u1 - u0) * W, AH = (v1 - v0) * H;
    const ImU32 base = d.art_col ? d.art_col : seed_color(d.seed, 0.55f, 0.6f);
    vgrad(x, f, u0, v0, u1, v1, shade(base, 0.95f), shade(base, 0.4f));
    {   // the icon's own colours, extended: its top strip is stretched over the upper half, its bottom strip over the lower half (a soft blur of its edges)
        const float vm = (v0 + v1) * 0.5f;
        image_uv(x, f, u0, v0, u1, vm, s.tex, s.a, ImVec2(s.b.x, s.a.y + (s.b.y - s.a.y) * 0.1f), 0.8f);
        image_uv(x, f, u0, vm, u1, v1, s.tex, ImVec2(s.a.x, s.a.y + (s.b.y - s.a.y) * 0.9f), s.b, 0.8f);
        const ImU32 seam = shade(base, 0.55f);   // the two halves melt into each other
        vgrad(x, f, u0, vm - (vm - v0) * 0.4f, u1, vm, alpha(seam, 0.f), alpha(seam, 0.85f));
        vgrad(x, f, u0, vm, u1, vm + (v1 - vm) * 0.4f, alpha(seam, 0.85f), alpha(seam, 0.f));
        vgrad(x, f, u0, v0, u1, v1, rgba(0, 0, 0, 60), rgba(0, 0, 0, 150));
    }
    const float side = std::min(AW * 0.84f, AH * 0.6f);
    const float cxp = (u0 + u1) * 0.5f * W, iy = v0 * H + AH * 0.075f, ix = cxp - side * 0.5f;
    const float sh = std::max(3.f, side * 0.03f);
    quad(x, f, (ix + sh) * px, (iy + sh * 1.4f) * py, (ix + side + sh * 1.8f) * px, (iy + side + sh * 2.4f) * py, rgba(0, 0, 0, 110));
    const float fr = std::max(2.f, side * 0.012f);
    quad(x, f, (ix - fr) * px, (iy - fr) * py, (ix + side + fr) * px, (iy + side + fr) * py, rgba(250, 250, 252));
    image_uv(x, f, ix * px, iy * py, (ix + side) * px, (iy + side) * py, s.tex, s.a, s.b);
    vgrad(x, f, ix * px, iy * py, (ix + side) * px, (iy + side * 0.18f) * py, rgba(255, 255, 255, 46), rgba(255, 255, 255, 0));
    ImFont* font = ui::F.bold ? ui::F.bold : ui::F.body;
    const float size = std::max(7.f, std::min(std::min(W * 0.1f, AH * 0.085f), 34.f));
    float ty = iy + side + AH * 0.055f;
    if (x.detail && font && size >= 7.f) {
        const std::vector<std::string>& ln = wrap_cached(font, size, d.title, AW * 0.86f, 3);
        for (const std::string& l : ln) {
            if (ty + size > v1 * H - 3.f) break;
            const float tw = text_w(font, size, l), lx = cxp - tw * 0.5f;
            face_text(x, f, font, size, l.c_str(), lx + 1.4f, ty + 1.4f, rgba(0, 0, 0, 150), false);
            face_text(x, f, font, size, l.c_str(), lx, ty, rgba(255, 255, 255, 250), false);
            ty += size * 1.14f;
        }
    }
    quad(x, f, u0, v1 - 4.f * py, u1, v1, alpha(st.band, 0.9f));
    top_band(x, f, d, st, u0, win.v0, u1, v0);
}

/* The cover scan, whole: never cropped, no drawn banner or overlay. */
void front_art(Ctx& x, const Face& f, const CaseDraw& d, const Style& st, const Src& s, const hubui::cases::UvRect& win) {
    if (hubui::cases::is_square_art(s.w, s.h) && hubui::cases::is_portrait_case(d.media)) {
        if (d.media == Media::SwitchCase) front_square(x, f, d, st, s, win);   // Switch 1 and 2: the full-bleed icon over its own blur
        else front_square_classic(x, f, d, st, s, win);
        return;
    }
    const float wu = win.u1 - win.u0, wv = win.v1 - win.v0;
    const float wa = (wu * f.W) / std::max(1.f, wv * f.H);
    const hubui::cases::CaseArtFit ft = hubui::cases::fit_art(wa, (float)s.w / (float)std::max(1, s.h), hubui::cases::nominal_cover_aspect(d.media));
    const float au0 = win.u0 + wu * ft.dst.u0, au1 = win.u0 + wu * ft.dst.u1, av0 = win.v0 + wv * ft.dst.v0, av1 = win.v0 + wv * ft.dst.v1;
    if (!ft.exact) quad(x, f, win.u0, win.v0, win.u1, win.v1, insert_col(d, st, ft.letterbox));
    image_uv(x, f, au0, av0, au1, av1, s.tex, s.a, s.b);
    outline(x, f, au0, av0, au1, av1, rgba(0, 0, 0, st.box ? 90 : 70), 1.f);
}

/* ------------------------------- faces ------------------------------- */

/* The favourite marker: a small gold star badge, drawn on the case's own plastic / spine (never over the cover art). */
void fav_star(Ctx& x, const Face& f, float cu, float cv, float r_px) {
    if (r_px < 2.5f) return;
    ImVec2 p[10];
    for (int i = 0; i < 10; ++i) {
        const float a = -1.5707963f + 0.62831853f * (float)i, r = (i & 1) ? 0.42f : 1.f;
        p[i] = f.at(cu + std::cos(a) * r * r_px / f.W, cv + std::sin(a) * r * r_px / f.H);
    }
    x.dl->AddConvexPolyFilled(p, 10, x.c(rgba(255, 206, 64)));
    x.dl->AddPolyline(p, 10, x.c(rgba(120, 80, 0, 140)), ImDrawFlags_Closed, 1.f);
}

void paint_front(Ctx& x, const Face& f, const CaseDraw& d, const Style& st, CaseKind kind, float sheen_u) {
    const float W = f.W, H = f.H;
    const float px = 1.f / W, py = 1.f / H;
    const bool box = st.box;
    const float rcu = std::min(0.035f, (box ? 1.5f : 5.f) * px), rcv = rcu * W * py;
    poly(x, f, {ImVec2(rcu, 0.f), ImVec2(1.f - rcu, 0.f), ImVec2(1.f, rcv), ImVec2(1.f, 1.f - rcv), ImVec2(1.f - rcu, 1.f), ImVec2(rcu, 1.f), ImVec2(0.f, 1.f - rcv), ImVec2(0.f, rcv)}, st.body);
    vgrad(x, f, 0.f, 0.f, 1.f, 1.f, rgba(255, 255, 255, box ? 18 : 30), rgba(0, 0, 0, box ? 36 : 40));
    const float hinge = hubui::cases::hinge_frac(kind);
    const hubui::cases::UvRect win = hubui::cases::front_window(kind);
    if (!box) {
        quad(x, f, 0.f, 0.f, hinge, 1.f, shade(st.body, 0.8f));
        quad(x, f, hinge - px, 0.f, hinge + px, 1.f, rgba(0, 0, 0, 70));
        quad(x, f, hinge + px, 0.f, hinge + 2.f * px, 1.f, rgba(255, 255, 255, 36));
        if (kind == CaseKind::Jewel) {   // the jewel case's hinge tabs
            for (int i = 0; i < 2; ++i) {
                const float va = i ? 0.80f : 0.06f, vb = i ? 0.94f : 0.20f;
                quad(x, f, 0.f, va, hinge * 0.86f, vb, rgba(96, 102, 116, 220));
                quad(x, f, 0.f, va, hinge * 0.86f, va + 2.f * py, rgba(255, 255, 255, 110));
                quad(x, f, hinge * 0.86f - px, va, hinge * 0.86f, vb, rgba(0, 0, 0, 80));
            }
        }
    }
    const Src fs = front_src(d);
    if (fs.tex) front_art(x, f, d, st, fs, win);   // the real scan carries its own console banner: nothing is drawn over it
    else {   // the generated cover: the console banner is drawn
        const Lay l = front_lay(st, f, win.u0, win.v0, win.u1, win.v1);
        cover_generated(x, f, l.au0, l.av0, l.au1, l.av1, d);
        banner_front(x, f, d, st, l);
        outline(x, f, l.au0, l.av0, l.au1, l.av1, rgba(0, 0, 0, box ? 110 : 90), 1.f);
    }
    if (box) {   // matte cardboard: worn edges and a fold line, no sheen
        hgrad(x, f, 0.f, 0.f, 0.05f, 1.f, rgba(0, 0, 0, 56), rgba(0, 0, 0, 0));
        hgrad(x, f, 0.95f, 0.f, 1.f, 1.f, rgba(0, 0, 0, 0), rgba(0, 0, 0, 62));
        vgrad(x, f, 0.f, 0.94f, 1.f, 1.f, rgba(0, 0, 0, 0), rgba(0, 0, 0, 50));
        // a bevelled cardboard edge: a lighter lip on the top and left, a darker one on the bottom and right
        const float ex = std::max(0.012f, 3.f * px), ey = std::max(0.012f, 3.f * py);
        quad(x, f, 0.f, 0.f, 1.f, ey, rgba(255, 255, 255, 62)); quad(x, f, 0.f, 0.f, ex, 1.f, rgba(255, 255, 255, 40));
        quad(x, f, 0.f, 1.f - ey, 1.f, 1.f, rgba(0, 0, 0, 90)); quad(x, f, 1.f - ex, 0.f, 1.f, 1.f, rgba(0, 0, 0, 70));
        outline(x, f, ex, ey, 1.f - ex, 1.f - ey, rgba(0, 0, 0, 50), 1.f);
    } else {   // clear plastic: a bright rim, the hinge ridge and a moving glossy highlight
        const float rx = std::max(0.014f, 4.f * px), ry = std::max(0.014f, 4.f * py);
        const bool dark = lum(st.body) < 0.12f;   // black plastic (GameCube, PS2): the gloss stays subtle so the case reads black, not silver
        const ImU32 rim = st.pale ? rgba(255, 255, 255, 120) : rgba(255, 255, 255, dark ? 14 : 46);
        quad(x, f, 0.f, 0.f, 1.f, ry, rim); quad(x, f, 0.f, 1.f - ry, 1.f, 1.f, rim);
        quad(x, f, 1.f - rx, ry, 1.f, 1.f - ry, rim);
        outline(x, f, 0.f, 0.f, 1.f, 1.f, st.pale ? rgba(0, 0, 0, 80) : rgba(255, 255, 255, dark ? 44 : 100), 1.f);
        outline(x, f, rx, ry, 1.f - rx, 1.f - ry, rgba(0, 0, 0, 70), 1.f);
        line(x, f, rcu + 0.01f, 2.f * py, 1.f - rcu - 0.01f, 2.f * py, rgba(255, 255, 255, st.pale ? 160 : dark ? 60 : 120), 1.f);
        hgrad(x, f, hinge * 0.2f, 0.f, hinge, 1.f, rgba(255, 255, 255, 0), rgba(255, 255, 255, dark ? 30 : 70));   // the hinge ridge: a lit rib along the spine side
        line(x, f, hinge * 0.5f, 0.02f, hinge * 0.5f, 0.98f, rgba(255, 255, 255, 110), 1.f);
        grad(x, f, 0.f, 0.f, 1.f, 0.62f, rgba(255, 255, 255, dark ? 22 : 46), rgba(255, 255, 255, dark ? 6 : 12), rgba(255, 255, 255, 0), rgba(255, 255, 255, 0));
        const float uc = std::max(0.1f, std::min(0.9f, sheen_u));
        hgrad(x, f, std::max(0.f, uc - 0.2f), 0.f, uc, 1.f, rgba(255, 255, 255, 0), rgba(255, 255, 255, 34));
        hgrad(x, f, uc, 0.f, std::min(1.f, uc + 0.12f), 1.f, rgba(255, 255, 255, 34), rgba(255, 255, 255, 0));
    }
    if (d.fav && !box) fav_star(x, f, hinge * 0.5f, 0.06f, std::max(4.f, W * hinge * 0.42f));   // on the hinge strip of the plastic
}

/* A small generated mark for the foot of a spine: a publisher-style badge (a generic shape, never a real logo). */
void publisher_mark(Ctx& x, const Face& f, float cu, float cv, float s_px, uint32_t h, ImU32 col, ImU32 hole) {
    const float du = s_px / f.W * 0.5f, dv = s_px / f.H * 0.5f;
    switch (h % 4u) {
        case 0: ngon(x, f, cu, cv, du, dv, 20, col); ngon(x, f, cu, cv, du * 0.55f, dv * 0.55f, 16, hole); break;
        case 1: quad(x, f, cu - du, cv - dv, cu + du, cv + dv, col); quad(x, f, cu - du * 0.6f, cv - dv * 0.18f, cu + du * 0.6f, cv + dv * 0.18f, hole); break;
        case 2: poly(x, f, {ImVec2(cu, cv - dv), ImVec2(cu + du, cv + dv), ImVec2(cu - du, cv + dv)}, col); break;
        default: ngon(x, f, cu, cv, du, dv, 20, col); quad(x, f, cu - du * 0.9f, cv - dv * 0.14f, cu + du * 0.9f, cv + dv * 0.14f, hole); break;
    }
}

/* The colours of a generated spine, following each console's retail trade dress (cardboard sides take the front art's colours). */
struct SpineLook { ImU32 top = 0, bot = 0, cap = 0, cap_text = 0, accent = 0; bool vword = false; };
SpineLook spine_look(const CaseDraw& d, const Style& st) {
    SpineLook k;
    k.top = k.bot = st.spine; k.cap = st.band; k.cap_text = st.band_text; k.accent = st.accent;
    switch (d.media) {
        case Media::GcCase: k.top = rgba(30, 30, 38); k.bot = rgba(14, 14, 18); k.cap = rgba(192, 196, 206); k.cap_text = rgba(24, 26, 34); break;   // black, grey cap
        case Media::Ps2Case: k.top = rgba(26, 27, 34); k.bot = rgba(10, 11, 15); k.cap = rgba(8, 9, 13); k.cap_text = rgba(246, 247, 252); k.accent = rgba(40, 94, 216); break;
        case Media::WiiUCase: k.top = rgba(28, 142, 210); k.bot = rgba(16, 100, 166); k.cap = rgba(255, 255, 255); k.cap_text = rgba(0, 120, 190); k.accent = rgba(214, 240, 252); break;
        case Media::SwitchCase: k.top = rgba(226, 24, 36); k.bot = rgba(172, 10, 24); k.cap = d.nx2 ? rgba(150, 8, 20) : rgba(188, 12, 26); k.cap_text = rgba(255, 255, 255); k.accent = rgba(255, 255, 255); break;
        case Media::DsCase: k.top = rgba(34, 36, 44); k.bot = rgba(14, 15, 19); k.cap = rgba(228, 231, 238); k.cap_text = rgba(34, 38, 50); k.accent = rgba(58, 148, 214); break;   // black case, light logo band
        case Media::N3dsCase: k.top = rgba(252, 252, 253); k.bot = rgba(226, 228, 233); k.cap = rgba(252, 252, 253); k.cap_text = rgba(210, 36, 44); k.accent = rgba(210, 36, 44); break;   // white case, red logo
        case Media::PspCase: k.top = rgba(26, 28, 38); k.bot = rgba(8, 9, 14); k.cap = rgba(10, 12, 18); k.cap_text = rgba(246, 247, 252); k.accent = rgba(72, 122, 232); break;
        case Media::Ps1Jewel: k.top = rgba(30, 30, 36); k.bot = rgba(10, 10, 14); k.vword = true; break;
        default:
            if (st.box && d.art_col) { k.top = lerpc(d.art_col, rgba(255, 255, 255), 0.08f); k.bot = shade(d.art_col, 0.52f); }
            break;
    }
    return k;
}

/* The spine face: the real strip of a full cover scan when there is one, else a generated spine in the console's trade dress. Left of the
 * front when you look at it; the title reads top to bottom. */
void paint_spine(Ctx& x, const Face& f, const CaseDraw& d, const Style& st, CaseKind kind) {
    const float W = f.W, H = f.H;
    const float px = 1.f / W;
    const Src ss = spine_src(d);
    ImU32 ink = st.sink;
    if (ss.tex) {
        quad(x, f, 0.f, 0.f, 1.f, 1.f, st.spine);
        image_uv(x, f, 0.f, 0.f, 1.f, 1.f, ss.tex, ss.a, ss.b);
        hgrad(x, f, 0.f, 0.f, 1.f, 1.f, rgba(255, 255, 255, st.box ? 14 : 22), rgba(0, 0, 0, st.box ? 30 : 44));
    } else {
        const SpineLook k = spine_look(d, st);
        const ImU32 body_mid = lerpc(k.top, k.bot, 0.5f);
        ink = on_color(body_mid);
        vgrad(x, f, 0.f, 0.f, 1.f, 1.f, k.top, k.bot);
        hgrad(x, f, 0.f, 0.f, 1.f, 1.f, rgba(255, 255, 255, st.box ? 22 : 40), rgba(0, 0, 0, st.box ? 40 : 62));
        if (kind == CaseKind::Jewel) {   // clear rails and the black spine card
            quad(x, f, 0.f, 0.f, 0.12f, 1.f, rgba(150, 156, 170, 110));
            quad(x, f, 0.88f, 0.f, 1.f, 1.f, rgba(110, 116, 130, 110));
        }
        ImFont* cf = ui::F.bold ? ui::F.bold : ui::F.cap;
        float v_title = 0.03f;
        if (k.vword) {   // PS1: a vertical PlayStation strip down the top of the black card
            const float ve = 0.30f;
            quad(x, f, 0.16f, 0.f, 0.84f, ve, rgba(236, 238, 244));
            const ImU32 pc[4] = {rgba(222, 52, 48), rgba(246, 190, 40), rgba(54, 170, 88), rgba(48, 104, 214)};
            for (int i = 0; i < 4; ++i) quad(x, f, 0.16f + 0.17f * (float)i, ve - 0.012f, 0.16f + 0.17f * (float)(i + 1), ve, pc[i]);
            label_v(x, f, "PLAYSTATION", 0.16f, 0.84f, 0.01f, ve - 0.014f, rgba(24, 26, 34), true);
            v_title = ve + 0.03f;
        } else {
            const float bv = std::max(0.075f, std::min(0.14f, 24.f / H));
            if (st.m == Media::GbcCart) {   // the colourful band, running down the spine
                const ImU32 c[3] = {rgba(0, 168, 176), rgba(112, 72, 196), rgba(226, 64, 146)};
                vgrad(x, f, 0.f, 0.f, 1.f, bv * 0.5f, c[0], c[1]); vgrad(x, f, 0.f, bv * 0.5f, 1.f, bv, c[1], c[2]);
            } else if (st.m == Media::SwitchCase && st.nx2) {
                quad(x, f, 0.f, 0.f, 1.f, bv, st.band);
            } else quad(x, f, 0.f, 0.f, 1.f, bv, k.cap);
            vgrad(x, f, 0.f, 0.f, 1.f, bv, rgba(255, 255, 255, 54), rgba(0, 0, 0, 40));
            quad(x, f, 0.f, bv, 1.f, bv + 1.5f / H, st.line ? k.accent : rgba(0, 0, 0, 90));
            float bs = std::min(W * 0.5f, bv * H * 0.5f);
            const float tw = tw_c(cf, bs, d.sys_short);
            if (tw > W * 0.9f) bs *= W * 0.9f / tw;
            if (bs >= 5.f) face_text(x, f, cf, bs, d.sys_short, (W - tw_c(cf, bs, d.sys_short)) * 0.5f, bv * H * 0.5f - bs * 0.5f, k.cap_text, false);
            v_title = bv + 0.03f;
        }
        if (d.fav) quad(x, f, 0.f, v_title - 0.01f, 1.f, v_title - 0.01f + 4.f / H, rgba(255, 206, 64));
        // the title, reading top to bottom
        ImFont* font = ui::F.bold ? ui::F.bold : ui::F.body;
        const float size = std::max(7.f, std::min(W * 0.55f, 22.f));
        const float avail = H * (0.875f - v_title) - size * 0.2f;
        if (x.detail && W >= 9.f && avail > size * 2.f) {
            const std::string& t = ellipsize_cached(font, size, d.title, avail);
            const float lx = W * 0.5f + size * 0.38f, ly = v_title * H + 2.f;
            face_text(x, f, font, size, t.c_str(), lx + 0.8f, ly + 0.8f, rgba(0, 0, 0, lum(body_mid) > 0.55f ? 0 : 120), true);
            face_text(x, f, font, size, t.c_str(), lx, ly, ink, true);
        }
        // the foot: a rule and a publisher-style badge
        const ImU32 mk = d.art_col && st.box ? lerpc(ink, d.art_col, 0.25f) : alpha(ink, 0.85f);
        quad(x, f, 0.12f, 0.9f, 0.88f, 0.9f + 1.4f / H, alpha(ink, 0.4f));
        publisher_mark(x, f, 0.5f, 0.955f, std::min(W * 0.55f, H * 0.045f), hash32(d.title), mk, body_mid);
    }
    if (d.fav && ss.tex) fav_star(x, f, 0.5f, 0.04f, std::min(W * 0.36f, H * 0.03f));
    if (st.box) {   // the folded cardboard edges of the box
        quad(x, f, 0.f, 0.f, 0.09f, 1.f, rgba(255, 255, 255, 46)); quad(x, f, 0.91f, 0.f, 1.f, 1.f, rgba(0, 0, 0, 56));
        line(x, f, 0.09f, 0.f, 0.09f, 1.f, rgba(0, 0, 0, 50), 1.f);
    } else {   // the keep case's plastic rails and the ridge along the spine
        const bool pale = lum(lerpc(spine_look(d, st).top, spine_look(d, st).bot, 0.5f)) > 0.55f;
        line(x, f, px, 0.f, px, 1.f, rgba(255, 255, 255, pale ? 120 : 76), 1.f);
        line(x, f, 1.f - px, 0.f, 1.f - px, 1.f, rgba(255, 255, 255, pale ? 80 : 40), 1.f);
        line(x, f, 0.1f, 0.01f, 0.1f, 0.99f, rgba(0, 0, 0, 44), 1.f);
        line(x, f, 0.9f, 0.01f, 0.9f, 0.99f, rgba(0, 0, 0, 44), 1.f);
        hgrad(x, f, 0.18f, 0.f, 0.34f, 1.f, rgba(255, 255, 255, 0), rgba(255, 255, 255, 22));
        hgrad(x, f, 0.34f, 0.f, 0.5f, 1.f, rgba(255, 255, 255, 22), rgba(255, 255, 255, 0));
    }
}

/* The opposite edge of the case (the side away from the spine): plain card or clear plastic with the cover sheet's edge, no design. */
void paint_edge(Ctx& x, const Face& f, const Style& st) {
    const float px = 1.f / f.W;
    quad(x, f, 0.f, 0.f, 1.f, 1.f, st.body);
    hgrad(x, f, 0.f, 0.f, 1.f, 1.f, rgba(255, 255, 255, st.box ? 22 : 36), rgba(0, 0, 0, st.box ? 44 : 60));
    if (st.box) {
        quad(x, f, 0.f, 0.f, 0.09f, 1.f, rgba(255, 255, 255, 40)); quad(x, f, 0.91f, 0.f, 1.f, 1.f, rgba(0, 0, 0, 52));
        line(x, f, 0.91f, 0.f, 0.91f, 1.f, rgba(0, 0, 0, 50), 1.f);
    } else {
        line(x, f, px, 0.f, px, 1.f, rgba(255, 255, 255, st.pale ? 120 : 70), 1.f);
        line(x, f, 1.f - px, 0.f, 1.f - px, 1.f, rgba(255, 255, 255, st.pale ? 80 : 40), 1.f);
        quad(x, f, 0.28f, 0.01f, 0.72f, 0.99f, rgba(0, 0, 0, 22));
        quad(x, f, 0.44f, 0.02f, 0.56f, 0.98f, rgba(255, 255, 255, st.pale ? 46 : 20));   // the paper edge of the cover sheet inside the clear plastic
    }
}

/* The top (or bottom) of the case: u runs spine side -> open side, v back -> front. Plastic cases show the plastic edge in the case colour,
 * boxes the cardboard flap with its crease. */
void paint_cap(Ctx& x, const Face& f, const CaseDraw& d, const Style& st, bool top) {
    const float W = f.W, H = f.H, px = 1.f / W, py = 1.f / H;
    const float k = top ? 1.12f : 0.62f;
    ImU32 body = shade(st.body, k);
    quad(x, f, 0.f, 0.f, 1.f, 1.f, body);
    if (st.box) {
        const ImU32 edge = shade(st.spine, k);
        quad(x, f, 0.f, 0.f, 0.045f, 1.f, edge);                         // the spine side of the box
        quad(x, f, 0.04f, 0.5f, 0.96f, 1.f - 0.02f, shade(body, 1.06f)); // the tuck flap lying over the box
        quad(x, f, 0.04f, 0.5f, 0.96f, 0.5f + std::max(0.04f, 2.f * py), rgba(0, 0, 0, 70));   // the flap's shadow edge: the crease
        quad(x, f, 0.04f, 0.5f - std::max(0.02f, py), 0.96f, 0.5f, rgba(255, 255, 255, 36));
        line(x, f, 0.04f, 1.f - 0.02f, 0.96f, 1.f - 0.02f, rgba(0, 0, 0, 60), 1.f);
        line(x, f, 0.045f, 0.f, 0.045f, 1.f, rgba(0, 0, 0, 60), 1.f);
        line(x, f, 0.04f, 0.5f, 0.04f, 0.98f, rgba(0, 0, 0, 50), 1.f);
        line(x, f, 0.96f, 0.5f, 0.96f, 0.98f, rgba(0, 0, 0, 50), 1.f);
        ngon(x, f, 0.5f, 0.5f, 0.045f, 0.045f * W * py * 0.9f, 14, rgba(0, 0, 0, 56));      // the thumb notch
        const float ex = std::max(0.012f, 3.f * px);
        quad(x, f, 1.f - ex, 0.f, 1.f, 1.f, rgba(0, 0, 0, 50));
        (void)d;
    } else {
        quad(x, f, 0.f, 0.f, 1.f, std::max(0.1f, 2.5f * py), rgba(255, 255, 255, st.pale ? 90 : 26));   // the back rail of the clear case
        quad(x, f, 0.f, 0.f, 0.05f, 1.f, shade(st.spine, k));                                          // the hinge strip, in the spine colour
        quad(x, f, 0.07f, 0.3f, 0.95f, 0.7f, rgba(0, 0, 0, 30));                                        // the cover sheet seen through the plastic
        quad(x, f, 0.07f, 0.45f, 0.95f, 0.55f, rgba(255, 255, 255, st.pale ? 44 : 16));
        line(x, f, 0.f, 1.f - py, 1.f, 1.f - py, rgba(255, 255, 255, st.pale ? 120 : 70), 1.f);        // the lit front lip
        line(x, f, px, 0.f, px, 1.f, rgba(255, 255, 255, st.pale ? 100 : 50), 1.f);
        line(x, f, 1.f - px, 0.f, 1.f - px, 1.f, rgba(255, 255, 255, st.pale ? 80 : 40), 1.f);
    }
    outline(x, f, 0.f, 0.f, 1.f, 1.f, rgba(0, 0, 0, 80), 1.f);
    (void)H;
}

/* ------------------------------- the back ------------------------------- */

void paint_back_art(Ctx& x, const Face& f, const CaseDraw& d, const Style& st, CaseKind kind, const Src& bs) {
    const float px = 1.f / f.W;
    quad(x, f, 0.f, 0.f, 1.f, 1.f, st.body);
    vgrad(x, f, 0.f, 0.f, 1.f, 1.f, rgba(255, 255, 255, 24), rgba(0, 0, 0, 40));
    const float hinge = hubui::cases::hinge_frac(kind);   // the hinge is on the right of a back
    hubui::cases::UvRect win = hubui::cases::front_window(kind);
    win.u1 = st.box ? 1.f - 0.028f : 1.f - hinge - 0.008f;
    win.u0 = st.box ? 0.028f : 0.024f;
    if (!st.box) { quad(x, f, 1.f - hinge, 0.f, 1.f, 1.f, shade(st.body, 0.8f)); quad(x, f, 1.f - hinge - px, 0.f, 1.f - hinge + px, 1.f, rgba(0, 0, 0, 70)); }
    const float wu = win.u1 - win.u0, wv = win.v1 - win.v0;
    const hubui::cases::CaseArtFit ft = hubui::cases::fit_art((wu * f.W) / std::max(1.f, wv * f.H), (float)bs.w / (float)std::max(1, bs.h), hubui::cases::nominal_cover_aspect(d.media));
    if (!ft.exact) quad(x, f, win.u0, win.v0, win.u1, win.v1, insert_col(d, st, ft.letterbox));
    image_uv(x, f, win.u0 + wu * ft.dst.u0, win.v0 + wv * ft.dst.v0, win.u0 + wu * ft.dst.u1, win.v0 + wv * ft.dst.v1, bs.tex, bs.a, bs.b);
    outline(x, f, win.u0, win.v0, win.u1, win.v1, rgba(0, 0, 0, 80), 1.f);
}

/* The age-rating style box: plain text only (e.g. "ESRB E": the agency on a small line, the rating large), never a rating logo. */
void rating_box(Ctx& x, const Face& f, float lx, float y0, float bw, float bh, const char* age, ImFont* bold, ImFont* cap, ImU32 ink) {
    const float px = 1.f / f.W, py = 1.f / f.H;
    outline(x, f, lx * px, y0 * py, (lx + bw) * px, (y0 + bh) * py, ink, 1.5f);
    char a[32], b[32];
    std::snprintf(a, sizeof a, "%s", age);
    b[0] = 0;
    if (char* sp_ = std::strchr(a, ' ')) { *sp_ = 0; std::snprintf(b, sizeof b, "%s", sp_ + 1); }
    if (!b[0]) { std::snprintf(b, sizeof b, "%s", a); a[0] = 0; }
    const float cs = std::min(bh * 0.24f, bw * 0.2f);
    float bs = bh * (a[0] ? 0.5f : 0.6f);
    const float tw = tw_c(bold, bs, b);
    if (tw > bw * 0.84f) bs *= bw * 0.84f / tw;
    float ty = y0 + (bh - bs) * 0.5f + (a[0] ? cs * 0.55f : 0.f);
    if (a[0]) { const float cw = tw_c(cap, cs, a); face_text(x, f, cap, cs, a, lx + (bw - cw) * 0.5f, y0 + bh * 0.08f, ink, false, cs * 0.08f); }
    face_text(x, f, bold, bs, b, lx + (bw - tw_c(bold, bs, b)) * 0.5f, ty, ink, false);
}

void paint_back(Ctx& x, const Face& f, const CaseDraw& d, const Style& st, CaseKind kind) {
    const Src bsrc = back_src(d);
    if (bsrc.tex) {   // the real back of the cover
        paint_back_art(x, f, d, st, kind, bsrc);
        if (!st.box) {
            outline(x, f, 0.f, 0.f, 1.f, 1.f, st.pale ? rgba(0, 0, 0, 70) : rgba(255, 255, 255, 50), 1.f);
            grad(x, f, 0.f, 0.f, 1.f, 0.5f, rgba(255, 255, 255, 30), rgba(255, 255, 255, 8), rgba(255, 255, 255, 0), rgba(255, 255, 255, 0));
        }
        return;
    }
    const float W = f.W, H = f.H;
    const float px = 1.f / W, py = 1.f / H;
    const CaseBack* bk = d.back;
    quad(x, f, 0.f, 0.f, 1.f, 1.f, st.body);
    vgrad(x, f, 0.f, 0.f, 1.f, 1.f, rgba(255, 255, 255, 24), rgba(0, 0, 0, 40));
    // spine hinge: on the right when you look at the back
    const float hinge = hubui::cases::hinge_frac(kind == CaseKind::Small ? CaseKind::Keep : kind);
    if (!st.box) { quad(x, f, 1.f - hinge, 0.f, 1.f, 1.f, shade(st.body, 0.8f)); quad(x, f, 1.f - hinge - px, 0.f, 1.f - hinge + px, 1.f, rgba(0, 0, 0, 70)); }
    const float u0 = 0.03f, u1 = st.box ? 0.97f : 1.f - hinge - 0.014f, v0 = 0.025f, v1 = 0.975f;
    quad(x, f, u0, v0, u1, v1, st.panel);
    const bool pp = lum(st.panel) > 0.55f;
    const ImU32 ink = st.ink;
    const ImU32 dim = pp ? rgba(70, 74, 90) : rgba(168, 174, 192);
    const float hbv = st.nx2 ? kNx2BandBack : std::max(0.05f, std::min(0.075f, 28.f * py));
    top_band(x, f, d, st, u0, v0, u1, v0 + hbv);   // the console banner
    const float pad = W * 0.045f;
    const float lx = u0 * W + pad, pw = (u1 - u0) * W - pad * 2.f;
    float y = (v0 + hbv) * H + H * 0.03f;
    ImFont* bold = ui::F.bold ? ui::F.bold : ui::F.body;
    ImFont* body = ui::F.body;
    ImFont* cap = ui::F.cap ? ui::F.cap : body;
    const float ts = std::max(8.f, std::min(W * 0.085f, 38.f));
    if (x.detail) {
        const std::vector<std::string>& tl = wrap_cached(bold, ts, d.title, pw, 2);
        for (const std::string& l : tl) { face_text(x, f, bold, ts, l.c_str(), lx, y, ink, false); y += ts * 1.12f; }
        y += ts * 0.25f;
        // facts: Genre / Released / Players side by side
        struct Fact { const char* cap; char val[40]; };
        Fact fs[3];
        std::snprintf(fs[0].val, sizeof fs[0].val, "%s", bk && bk->genre && *bk->genre ? bk->genre : "-"); fs[0].cap = "GENRE";
        if (bk && bk->year > 0) std::snprintf(fs[1].val, sizeof fs[1].val, "%d", bk->year); else std::snprintf(fs[1].val, sizeof fs[1].val, "-");
        fs[1].cap = "RELEASED";
        if (bk && bk->players == 1) std::snprintf(fs[2].val, sizeof fs[2].val, "1 player");
        else if (bk && bk->players > 1) std::snprintf(fs[2].val, sizeof fs[2].val, "1-%d players", bk->players);
        else std::snprintf(fs[2].val, sizeof fs[2].val, "-");
        fs[2].cap = "PLAYERS";
        const float cs = std::max(6.f, std::min(W * 0.04f, 14.f)), vs = std::max(7.5f, std::min(W * 0.058f, 19.f));
        const float cwid[3] = {pw * 0.37f, pw * 0.26f, pw * 0.37f}, cx0[3] = {0.f, pw * 0.37f, pw * 0.63f};
        for (int i = 0; i < 3; ++i) {
            face_text(x, f, cap, cs, fs[i].cap, lx + cx0[i], y, dim, false, cs * 0.08f);
            const std::string& v = ellipsize_cached(bold, vs, fs[i].val, cwid[i] - pad * 0.3f);
            face_text(x, f, bold, vs, v.c_str(), lx + cx0[i], y + cs * 1.35f, ink, false);
        }
        y += cs * 1.35f + vs * 1.5f;
        {   // who made it: developer / publisher
            char who[2][160];
            int nw = 0;
            const bool dev = bk && bk->maker && *bk->maker, pub = bk && bk->publisher && *bk->publisher;
            if (dev && pub && std::strcmp(bk->maker, bk->publisher) == 0) std::snprintf(who[nw++], sizeof who[0], "Developed and published by %s", bk->maker);
            else {
                if (dev) std::snprintf(who[nw++], sizeof who[0], "Developed by %s", bk->maker);
                if (pub) std::snprintf(who[nw++], sizeof who[0], "Published by %s", bk->publisher);
            }
            for (int i = 0; i < nw; ++i) {
                const std::string& v = ellipsize_cached(body, cs * 1.25f, who[i], pw);
                face_text(x, f, body, cs * 1.25f, v.c_str(), lx, y, dim, false);
                y += cs * 1.25f * 1.4f;
            }
            if (nw) y += cs * 0.1f;
        }
        quad(x, f, u0 + 0.04f, y / H, u1 - 0.04f, y / H + 1.2f * py, alpha(ink, 0.18f));
        y += H * 0.014f;

        // ---- laid out from the bottom up: the bottom row (players box, rating box, fine print, barcode), the picture strip, then the text fills what is left ----
        const float row_h = std::max(20.f, std::min(40.f, H * 0.075f));
        const float row_y1 = (v1 - 0.022f) * H, row_y0 = row_y1 - row_h;
        const bool has_pics = bk && bk->nshots > 0;
        const int np = has_pics ? bk->nshots : 0;
        const float gap = pw * 0.03f, tw = np > 0 ? (pw - gap * (float)(np - 1)) / (float)np : 0.f;
        float pic_h = np > 0 ? std::min(tw * 0.78f, H * 0.27f) : 0.f;
        const bool label = has_pics && bk->shots_label && *bk->shots_label;
        const float bs0 = std::max(7.f, std::min(W * 0.054f, 19.f));
        float pics_y0 = row_y0 - H * 0.02f - pic_h, text_bottom = pics_y0 - (label ? cs * 1.8f : H * 0.014f);
        if (!has_pics) text_bottom = row_y0 - H * 0.016f;
        // the description; a short one is set a little larger so the page does not look empty
        const char* desc = bk && bk->desc && *bk->desc ? bk->desc : "No description for this game yet.";
        const float room = std::max(0.f, text_bottom - y);
        const float est_lines = (float)std::strlen(desc) * bs0 * 0.5f / std::max(1.f, pw);
        const float bs = est_lines * bs0 * 1.3f < room * 0.4f ? std::min(bs0 * (has_pics ? 1.22f : 1.4f), 24.f) : bs0;
        const int max_lines = (int)std::floor(room / (bs * 1.3f));
        int shown = 0;
        if (max_lines >= 1) {
            if (bk && (bk->lines_w != pw || bk->lines_size != bs || bk->lines_max != max_lines)) {   // wrap once per size; the last visible line carries its "..."
                bk->lines = ui::wrap_lines(body, bs, desc, pw, 40);
                if ((int)bk->lines.size() > max_lines) { bk->lines.resize((size_t)max_lines); bk->lines.back() = ui::ellipsize(body, bs, bk->lines.back() + " ...", pw); }
                bk->lines_w = pw; bk->lines_size = bs; bk->lines_max = max_lines;
            }
            const std::vector<std::string>& dl_lines = bk ? bk->lines : wrap_cached(body, bs, desc, pw, 40);
            for (int i = 0; i < (int)dl_lines.size() && i < max_lines; ++i, ++shown) {
                face_text(x, f, body, bs, dl_lines[(size_t)i].c_str(), lx, y, ink, false);
                y += bs * 1.3f;
            }
        }
        // feature bullets fill the room the description leaves
        {
            y += bs * 0.35f;
            const float lh = bs * 1.55f;
            char b[96];
            const bool cart = hubui::cases::is_box(d.media);
            const bool know_players = bk && bk->players > 0, know_genre = bk && bk->genre && *bk->genre;
            const int maxb = has_pics ? 3 : 5;
            int shown_b = 0;
            for (int i = 0; i < 5 && shown_b < maxb && y + lh <= text_bottom + bs * 0.2f; ++i) {
                if (i == 0) std::snprintf(b, sizeof b, "For use with %s", d.sys_name);
                else if (i == 1) std::snprintf(b, sizeof b, "%s", cart ? "Battery back-up saves your progress" : "Save your progress as you play");
                else if (i == 2) { if (!know_players) continue; if (bk->players == 1) std::snprintf(b, sizeof b, "One player"); else std::snprintf(b, sizeof b, "Up to %d players", bk->players); }
                else if (i == 3) { if (!know_genre) continue; std::snprintf(b, sizeof b, "%s", bk->genre); }
                else std::snprintf(b, sizeof b, "Instruction booklet included");
                quad(x, f, (lx + bs * 0.05f) * px, (y + bs * 0.34f) * py, (lx + bs * 0.42f) * px, (y + bs * 0.71f) * py, dim);
                const std::string& v = ellipsize_cached(body, bs * 0.92f, b, pw - bs * 0.9f);
                face_text(x, f, body, bs * 0.92f, v.c_str(), lx + bs * 0.85f, y, ink, false);
                y += lh;
                ++shown_b;
            }
        }
        // no pictures and room left over: a large faint console wordmark so the page never looks empty
        if (!has_pics && text_bottom - y > H * 0.1f) {
            char wm[48];
            upper_into(wm, sizeof wm, d.sys_name);
            float ws = std::min((text_bottom - y) * 0.45f, 40.f);
            const float w0 = tw_c(bold, ws, wm) * 1.04f;
            if (w0 > pw * 0.9f) ws *= pw * 0.9f / w0;
            if (ws >= 8.f) {
                const float cy = (y + text_bottom) * 0.5f;
                quad(x, f, (lx) * px, (cy - ws * 1.1f) * py, (lx + pw) * px, (cy - ws * 1.1f + 1.5f) * py, alpha(ink, 0.15f));
                quad(x, f, (lx) * px, (cy + ws * 1.1f) * py, (lx + pw) * px, (cy + ws * 1.1f + 1.5f) * py, alpha(ink, 0.15f));
                face_text(x, f, bold, ws, wm, lx + (pw - tw_c(bold, ws, wm)) * 0.5f, cy - ws * 0.5f, alpha(ink, 0.2f), false);
            }
        }
        // pictures: the game's real screenshots (title screen / in game), else the player's own
        if (pic_h > 0.f) {
            if (label) face_text(x, f, cap, cs, bk->shots_label, lx, pics_y0 - cs * 1.6f, dim, false, cs * 0.08f);
            for (int i = 0; i < np; ++i) {
                const float a = lx + (tw + gap) * (float)i;
                const float ua = a * px, ub = (a + tw) * px, va = pics_y0 * py, vb = (pics_y0 + pic_h) * py;
                quad(x, f, ua - px, va - py, ub + px, vb + py, rgba(0, 0, 0, 150));
                if (bk->shots[i].tex && bk->shots[i].w > 0 && bk->shots[i].h > 0) image(x, f, ua, va, ub, vb, bk->shots[i].tex, bk->shots[i].w, bk->shots[i].h, 0.5f);
                else quad(x, f, ua, va, ub, vb, rgba(60, 64, 80));
            }
        }
        // the bottom row: a players box, an age-rating box, fine print, a generated barcode
        {
            const float bw = row_h * 1.15f;
            const float bu0 = lx * px, bu1 = (lx + bw) * px, bv0 = row_y0 * py, bv1 = row_y1 * py;
            outline(x, f, bu0, bv0, bu1, bv1, dim, 1.5f);
            char pn[16];
            if (bk && bk->players == 1) std::snprintf(pn, sizeof pn, "1");
            else if (bk && bk->players > 1) std::snprintf(pn, sizeof pn, "1-%d", bk->players);
            else std::snprintf(pn, sizeof pn, "%s", d.sys_short);
            const float ps = std::min(row_h * 0.5f, bw * 0.8f / std::max(1.f, tw_c(bold, row_h * 0.5f, pn) / (row_h * 0.5f)));
            face_text(x, f, bold, ps, pn, lx + (bw - tw_c(bold, ps, pn)) * 0.5f, row_y0 + (row_h - ps) * 0.5f, ink, false);
            float fx0 = lx + bw + pw * 0.03f;
            if (bk && bk->age && *bk->age) {
                const float rw = row_h * 1.35f;
                rating_box(x, f, fx0, row_y0, rw, row_h, bk->age, bold, cap, ink);
                fx0 += rw + pw * 0.03f;
            }
            const float bx1 = u1 - 0.04f, bx0 = bx1 - std::min(0.32f, row_h * 3.4f * px);
            const float fu0 = fx0 * px, fu1 = bx0 - 0.03f;
            if (fu1 - fu0 > 0.05f) {   // fine print
                uint32_t h = d.seed * 747796405u + 2891336453u;
                for (int i = 0; i < 4; ++i) {
                    h = h * 1664525u + 1013904223u;
                    const float yy = row_y0 + row_h * (0.12f + 0.25f * (float)i), len = 0.55f + 0.45f * (float)((h >> 24) & 0xFFu) / 255.f;
                    quad(x, f, fu0, yy * py, fu0 + (fu1 - fu0) * len, (yy + std::max(1.5f, row_h * 0.1f)) * py, alpha(dim, 0.5f));
                }
            }
            quad(x, f, bx0 - 0.012f, bv0, bx1 + 0.012f, bv1, rgba(244, 244, 246));
            uint32_t h = d.seed * 2654435761u + 17u;
            float u = bx0 + 0.006f;
            while (u < bx1 - 0.012f) {
                h = h * 1664525u + 1013904223u;
                const float w = (1.f + (float)((h >> 24) % 3u)) * 1.1f * px;
                if ((h >> 8) & 1u) quad(x, f, u, bv0 + 2.f * py, std::min(bx1, u + w), bv1 - 2.f * py, rgba(18, 18, 24));
                u += w + (0.8f + (float)((h >> 16) & 1u)) * px;
            }
        }
        (void)shown;
    }
    outline(x, f, u0, v0, u1, v1, rgba(0, 0, 0, 90), 1.f);
    if (!st.box) {
        outline(x, f, 0.f, 0.f, 1.f, 1.f, st.pale ? rgba(0, 0, 0, 70) : rgba(255, 255, 255, 50), 1.f);
        grad(x, f, 0.f, 0.f, 1.f, 0.5f, rgba(255, 255, 255, 30), rgba(255, 255, 255, 8), rgba(255, 255, 255, 0), rgba(255, 255, 255, 0));
    }
}
}  // namespace

/* ======================================================================= */

void cases_bounds(const CaseScene& sc, Media m, const CasePose& ps, ImVec2* mn, ImVec2* mx) {
    const CaseDims cd = hubui::cases::case_dims(m);
    Xf xf;
    xf.ct = std::cos(ps.yaw); xf.st = std::sin(ps.yaw); xf.cp = std::cos(ps.pitch); xf.sp_ = std::sin(ps.pitch);
    xf.yc = cd.h * 0.5f; xf.sc = ps.scale; xf.ox = ps.x; xf.oy = ps.lift; xf.oz = ps.z;
    ImVec2 lo(1e9f, 1e9f), hi(-1e9f, -1e9f);
    for (int i = 0; i < 8; ++i) {
        const V3 w = xf.pt((i & 1 ? 0.5f : -0.5f) * cd.w, (i & 2) ? cd.h : 0.f, (i & 4 ? 0.5f : -0.5f) * cd.d);
        const ImVec2 p = proj(sc, w);
        lo.x = std::min(lo.x, p.x); lo.y = std::min(lo.y, p.y); hi.x = std::max(hi.x, p.x); hi.y = std::max(hi.y, p.y);
    }
    *mn = lo; *mx = hi;
}

void cases_shadow(const CaseScene& sc, const CaseDraw& d, const CasePose& ps) {
    if (ps.alpha <= 0.02f) return;
    const CaseDims cd = hubui::cases::case_dims(d.media);
    Xf xf;
    xf.ct = std::cos(ps.yaw); xf.st = std::sin(ps.yaw); xf.sc = ps.scale; xf.ox = ps.x; xf.oz = ps.z;
    const float hw = cd.w * 0.5f, dd = cd.d * 0.5f;
    const float soft = 1.f / (1.f + ps.lift * 3.f);
    for (int k = 0; k < 4; ++k) {
        const float g = 1.f + 0.16f * (float)k + ps.lift * 0.4f;   // grows and fades: a soft edge
        ImVec2 p[4];
        const float cxs[4] = {-1, 1, 1, -1}, czs[4] = {1, 1, -1, -1};
        for (int i = 0; i < 4; ++i) {
            const V3 w = xf.pt(cxs[i] * hw * g, 0.f, czs[i] * (dd * g + 0.02f * (float)k));
            V3 s = w;
            s.x += 0.05f + 0.03f * (float)k;   // the light is up and to the left: the shadow falls a little to the right
            s.z += 0.01f * (float)k;
            p[i] = proj(sc, s);
        }
        sc.dl->AddConvexPolyFilled(p, 4, IM_COL32(0, 0, 0, (int)(ps.alpha * soft * 64.f / (1.f + (float)k * 0.85f))));
    }
}

void cases_case(const CaseScene& sc, const CaseDraw& d, const CasePose& ps, bool reflection) {
    // Cases are always drawn fully opaque (see-through faces would show the cases behind them): fading toward the background is a fog quad per face
    const float a = reflection ? ps.alpha * ps.refl : 1.f;
    if (a <= 0.01f || ps.alpha <= 0.01f) return;
    const float fog = reflection ? 0.f : 1.f - ps.alpha;
    const CaseKind kind = hubui::cases::kind_for(d.media);
    const CaseDims cd = hubui::cases::case_dims(d.media);
    const Style st = style_of(d);
    Xf xf;
    xf.ct = std::cos(ps.yaw); xf.st = std::sin(ps.yaw); xf.cp = std::cos(ps.pitch); xf.sp_ = std::sin(ps.pitch);
    xf.yc = cd.h * 0.5f; xf.sc = ps.scale; xf.ox = ps.x; xf.oy = ps.lift; xf.oz = ps.z; xf.mirror = reflection;
    Ctx x{sc.dl, d.ghost ? 0.62f : 0.f};
    x.a = a;
    x.detail = !reflection || ps.refl_detail;
    const float hw = cd.w * 0.5f, H = cd.h, dd = cd.d * 0.5f;
    const float L[3] = {-0.42f, 0.55f, -0.72f};   // light direction (towards the light): up, left, in front
    auto lit = [&](const float n[3]) {
        const V3 nw = xf.dir(n[0], n[1], n[2]);
        const float len = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
        const float dp = (nw.x * L[0] + (reflection ? -nw.y : nw.y) * L[1] + nw.z * L[2]) / len;
        return 0.62f + 0.38f * std::max(0.f, dp) / 0.8f;   // 1 = full light
    };
    auto shade_face = [&](const Face& f, const float n[3]) {
        const float k = std::min(1.f, lit(n));
        if (k < 0.995f) sc.dl->AddQuadFilled(f.q[0], f.q[1], f.q[2], f.q[3], IM_COL32(4, 5, 10, (int)((1.f - k) * 230.f * a)));
        if (fog > 0.004f) sc.dl->AddQuadFilled(f.q[0], f.q[1], f.q[2], f.q[3], (ps.fog & 0x00FFFFFFu) | ((ImU32)(std::min(1.f, fog) * 255.f) << 24));
    };
    Face f;
    {   // back
        const float c[4][3] = {{hw, H, dd}, {-hw, H, dd}, {-hw, 0, dd}, {hw, 0, dd}}, n[3] = {0, 0, 1};
        if (make_face(sc, xf, c, n, cd.w, H, &f)) { Ctx xb = x; xb.detail = x.detail && face_ratio(f) > 0.5f; paint_back(xb, f, d, st, kind); shade_face(f, n); }
    }
    {   // left: the spine design; right: the plain edge
        const float c[4][3] = {{-hw, H, dd}, {-hw, H, -dd}, {-hw, 0, -dd}, {-hw, 0, dd}}, n[3] = {-1, 0, 0};
        if (make_face(sc, xf, c, n, cd.d, H, &f)) { Ctx xs = x; xs.detail = x.detail && f.W * face_ratio(f) >= 12.f; paint_spine(xs, f, d, st, kind); shade_face(f, n); }
    }
    {
        const float c[4][3] = {{hw, H, -dd}, {hw, H, dd}, {hw, 0, dd}, {hw, 0, -dd}}, n[3] = {1, 0, 0};
        if (make_face(sc, xf, c, n, cd.d, H, &f)) { Ctx xs = x; xs.detail = x.detail && f.W * face_ratio(f) >= 12.f; paint_edge(xs, f, st); shade_face(f, n); }
    }
    {   // top (seen when the case is tipped in Inspect)
        const float c[4][3] = {{-hw, H, dd}, {hw, H, dd}, {hw, H, -dd}, {-hw, H, -dd}}, n[3] = {0, 1, 0};
        if (make_face(sc, xf, c, n, cd.w, cd.d, &f)) { Ctx xt = x; xt.detail = x.detail && f.W * face_ratio(f) >= 12.f; paint_cap(xt, f, d, st, true); shade_face(f, n); }
    }
    {   // bottom (seen when the case is lifted and tipped in Inspect)
        const float c[4][3] = {{-hw, 0, -dd}, {hw, 0, -dd}, {hw, 0, dd}, {-hw, 0, dd}}, n[3] = {0, -1, 0};
        if (make_face(sc, xf, c, n, cd.w, cd.d, &f)) { Ctx xt = x; xt.detail = x.detail && f.W * face_ratio(f) >= 12.f; paint_cap(xt, f, d, st, false); shade_face(f, n); }
    }
    {   // front
        const float c[4][3] = {{-hw, H, -dd}, {hw, H, -dd}, {hw, 0, -dd}, {-hw, 0, -dd}}, n[3] = {0, 0, -1};
        if (make_face(sc, xf, c, n, cd.w, H, &f)) {
            Ctx xf2 = x;
            xf2.detail = x.detail && face_ratio(f) > 0.5f;   // a cover turned nearly side-on: no squashed text
            paint_front(xf2, f, d, st, kind, 0.5f + ps.yaw * 0.42f);
            shade_face(f, n);
        }
    }
}

/* ------------------------------- the stage ------------------------------- */
/* Wall, floor and light. The colours come from the focused game (see cases_stage.h ambient_from_art), so the whole room takes the mood of the
 * cover. Lights are a few big gradient fans / quads (one draw call each), never stacks of small shapes. */

namespace {
constexpr float kWallZ = 5.2f;
struct StageColors { ImU32 wall_top, wall_hz, floor_far, floor_near, tint, light; };
StageColors stage_colors(ImU32 c0, ImU32 c1) {
    StageColors s;
    const ImU32 dark = rgba(5, 6, 10);
    s.tint = lerpc(c0, c1, 0.5f);
    if (lum(s.tint) < 0.12f) s.tint = lerpc(s.tint, rgba(120, 130, 190), 0.4f);
    s.wall_top = lerpc(dark, s.tint, 0.20f);
    s.wall_hz = lerpc(dark, s.tint, 0.48f);
    s.floor_far = lerpc(dark, s.tint, 0.30f);
    s.floor_near = lerpc(dark, s.tint, 0.06f);
    s.light = lerpc(c1, rgba(255, 255, 255), 0.45f);
    return s;
}
ImU32 floor_at(const StageColors& s, float yw, float yb, float y) { return lerpc(s.floor_far, s.floor_near, std::max(0.f, std::min(1.f, (y - yw) / std::max(1.f, yb - yw)))); }

/* A radial gradient: one triangle fan, `col_in` at the centre fading to `col_out` on the ellipse. */
void fan(ImDrawList* dl, ImVec2 c, float rx, float ry, ImU32 col_in, ImU32 col_out, int seg = 40) {
    if (rx < 1.f || ry < 1.f) return;
    const ImVec2 uvw = dl->_Data->TexUvWhitePixel;
    dl->PrimReserve(seg * 3, seg + 1);
    const ImDrawIdx i0 = (ImDrawIdx)dl->_VtxCurrentIdx;
    dl->PrimWriteVtx(c, uvw, col_in);
    for (int k = 0; k < seg; ++k) {
        const float an = 6.2831853f * (float)k / (float)seg;
        dl->PrimWriteVtx(ImVec2(c.x + std::cos(an) * rx, c.y + std::sin(an) * ry), uvw, col_out);
    }
    ImDrawIdx* ix = dl->_IdxWritePtr;
    for (int k = 0; k < seg; ++k) {
        ix[k * 3] = i0; ix[k * 3 + 1] = (ImDrawIdx)(i0 + 1 + k); ix[k * 3 + 2] = (ImDrawIdx)(i0 + 1 + (k + 1) % seg);
    }
    dl->_IdxWritePtr += seg * 3;
}
/* A quad with a colour per corner (TL TR BR BL). */
void quad4(ImDrawList* dl, const ImVec2 p[4], const ImU32 c[4]) {
    const ImVec2 uvw = dl->_Data->TexUvWhitePixel;
    dl->PrimReserve(6, 4);
    const ImDrawIdx i = (ImDrawIdx)dl->_VtxCurrentIdx;
    for (int k = 0; k < 4; ++k) dl->PrimWriteVtx(p[k], uvw, c[k]);
    ImDrawIdx* ix = dl->_IdxWritePtr;
    ix[0] = i; ix[1] = (ImDrawIdx)(i + 1); ix[2] = (ImDrawIdx)(i + 2); ix[3] = i; ix[4] = (ImDrawIdx)(i + 2); ix[5] = (ImDrawIdx)(i + 3);
    dl->_IdxWritePtr += 6;
}
float fract01(float v) { return v - std::floor(v); }
}  // namespace

void cases_stage_back(const CaseScene& sc, ImVec2 p0, ImVec2 p1, ImU32 c0, ImU32 c1) {
    ImDrawList* dl = sc.dl;
    const StageColors s = stage_colors(c0, c1);
    const float yw = sc.floor_y(kWallZ);
    const float W = p1.x - p0.x;
    const float lit = sc.spot * (1.f - 0.45f * sc.dim);
    dl->AddRectFilledMultiColor(p0, ImVec2(p1.x, yw), s.wall_top, s.wall_top, s.wall_hz, s.wall_hz);
    dl->PushClipRect(p0, ImVec2(p1.x, yw + 1.f), true);
    // a wide soft wash of the ambient colour behind the cases, a tighter glow right behind the centre case
    fan(dl, ImVec2(sc.cx, yw - sc.U * 0.35f), W * 0.55f, sc.U * 1.55f, alpha(s.light, 0.20f), alpha(s.light, 0.f), 48);
    // the spotlight: a cone from the top of the wall to the pool on the floor, soft at both sides
    {
        const float xt[4] = {-0.95f, -0.32f, 0.32f, 0.95f}, xb[4] = {-2.9f, -1.15f, 1.15f, 2.9f};
        const float aw[4] = {0.f, 1.f, 1.f, 0.f};
        const float ytop = p0.y - sc.U * 0.2f;
        for (int k = 0; k < 3; ++k) {
            const ImVec2 q[4] = {ImVec2(sc.cx + xt[k] * sc.U, ytop), ImVec2(sc.cx + xt[k + 1] * sc.U, ytop), ImVec2(sc.cx + xb[k + 1] * sc.U, yw), ImVec2(sc.cx + xb[k] * sc.U, yw)};
            const ImU32 c[4] = {alpha(s.light, 0.f), alpha(s.light, 0.f), alpha(s.light, 0.15f * lit * aw[k + 1]), alpha(s.light, 0.15f * lit * aw[k])};
            ImU32 cc[4] = {alpha(s.light, 0.06f * lit * aw[k]), alpha(s.light, 0.06f * lit * aw[k + 1]), c[2], c[3]};
            quad4(dl, q, cc);
        }
    }
    fan(dl, ImVec2(sc.cx, sc.hy + sc.U * 0.02f), sc.U * 0.95f, sc.U * 1.1f, alpha(s.light, 0.17f * lit), alpha(s.light, 0.f), 40);   // rim / back light
    dl->PopClipRect();
    dl->AddRectFilledMultiColor(ImVec2(p0.x, yw), p1, s.floor_far, s.floor_far, s.floor_near, s.floor_near);
    // soft horizon: a glow band over the junction, a bright line that fades out to the sides, and two faint lines above it
    const ImU32 hz = lerpc(s.tint, rgba(255, 255, 255), 0.55f);
    dl->AddRectFilledMultiColor(ImVec2(p0.x, yw - sc.U * 0.07f), ImVec2(p1.x, yw), alpha(hz, 0.f), alpha(hz, 0.f), alpha(hz, 0.16f), alpha(hz, 0.16f));
    const float mid = sc.cx;
    dl->AddRectFilledMultiColor(ImVec2(p0.x, yw - 1.f), ImVec2(mid, yw + 1.f), alpha(hz, 0.f), alpha(hz, 0.60f), alpha(hz, 0.60f), alpha(hz, 0.f));
    dl->AddRectFilledMultiColor(ImVec2(mid, yw - 1.f), ImVec2(p1.x, yw + 1.f), alpha(hz, 0.60f), alpha(hz, 0.f), alpha(hz, 0.f), alpha(hz, 0.60f));
    for (int i = 1; i <= 2; ++i) {
        const float y = yw - sc.U * 0.085f * (float)i;
        dl->AddRectFilledMultiColor(ImVec2(p0.x, y), ImVec2(mid, y + 1.f), alpha(hz, 0.f), alpha(hz, 0.16f / (float)i), alpha(hz, 0.16f / (float)i), alpha(hz, 0.f));
        dl->AddRectFilledMultiColor(ImVec2(mid, y), ImVec2(p1.x, y + 1.f), alpha(hz, 0.16f / (float)i), alpha(hz, 0.f), alpha(hz, 0.f), alpha(hz, 0.16f / (float)i));
    }
    // drifting light motes (a few dozen tiny discs; none under reduced motion)
    if (sc.motion) {
        const float H = yw - p0.y;
        for (int i = 0; i < 26; ++i) {
            const float a = fract01(0.6180339f * (float)(i + 1)), b = fract01(0.7548776f * (float)(i + 1) + 0.13f);
            const float sp_ = 0.006f + 0.010f * fract01(a * 7.3f);
            const double T = sc.time;   // doubles: the phases stay exact after days of uptime
            const float x = p0.x + fract01(a + (float)std::fmod(T * (double)sp_ * 0.4, 1.0) + 0.02f * (float)std::sin(T * 0.35 + (double)i)) * W;
            const float y = p0.y + (1.f - fract01(b + (float)std::fmod(T * (double)sp_, 1.0))) * (H * 0.96f);
            const float tw = 0.5f + 0.5f * (float)std::sin(T * (0.5 + 0.4 * (double)a) + (double)i * 1.7);
            const float edge = std::min(1.f, std::min((y - p0.y) / (H * 0.2f), (yw - y) / (H * 0.2f)));
            const float al = 0.05f + 0.13f * tw;
            dl->AddCircleFilled(ImVec2(x, y), std::max(1.f, sc.U * (0.004f + 0.006f * fract01(a * 3.1f))), alpha(s.light, al * std::max(0.f, edge) * (0.4f + 0.6f * lit)), 8);
        }
    }
}

void cases_veil(const CaseScene& sc, ImVec2 p0, ImVec2 p1, ImU32 c0, ImU32 c1, float case_h) {
    ImDrawList* dl = sc.dl;
    const StageColors s = stage_colors(c0, c1);
    const float yw = sc.floor_y(kWallZ), yb = p1.y;
    const float y0 = sc.floor_y(2.2f), y1 = sc.floor_y(0.f) + case_h * 0.34f * sc.U;
    dl->PushClipRect(p0, p1, true);
    // an eased fade (the reflection stays crisp under the case and melts into the floor), in six bands
    constexpr int kBands = 6;
    for (int i = 0; i < kBands; ++i) {
        const float ta = (float)i / (float)kBands, tb = (float)(i + 1) / (float)kBands;
        const float ya = y0 + (y1 - y0) * ta, yb2 = y0 + (y1 - y0) * tb;
        const float aa = std::pow(ta, 1.35f), ab = std::pow(tb, 1.35f);
        const ImU32 ca_ = floor_at(s, yw, yb, ya), cb_ = floor_at(s, yw, yb, yb2);
        dl->AddRectFilledMultiColor(ImVec2(p0.x, ya), ImVec2(p1.x, yb2), alpha(ca_, aa), alpha(ca_, aa), alpha(cb_, ab), alpha(cb_, ab));
    }
    dl->AddRectFilledMultiColor(ImVec2(p0.x, y1), ImVec2(p1.x, std::max(y1 + 1.f, p1.y)), floor_at(s, yw, yb, y1), floor_at(s, yw, yb, y1), floor_at(s, yw, yb, yb), floor_at(s, yw, yb, yb));
    dl->PopClipRect();
}

void cases_stage_front(const CaseScene& sc, ImVec2 p0, ImVec2 p1, ImU32 c0, ImU32 c1) {
    ImDrawList* dl = sc.dl;
    const StageColors s = stage_colors(c0, c1);
    const float yw = sc.floor_y(kWallZ);
    const float lit = sc.spot * (1.f - 0.45f * sc.dim);
    dl->PushClipRect(ImVec2(p0.x, yw), p1, true);
    const ImU32 ln = lerpc(s.tint, rgba(255, 255, 255), 0.5f);
    // perspective lines running away to the horizon (fainter toward the edges of the screen)
    const float vy = sc.hy;
    const float tw = (yw - vy) / std::max(1.f, p1.y - vy);
    for (int k = -9; k <= 9; ++k) {
        const float xb = sc.cx + (float)k * (p1.x - p0.x) * 0.105f;
        const float xt = sc.cx + (xb - sc.cx) * tw;
        dl->AddLine(ImVec2(xt, yw), ImVec2(xb, p1.y), alpha(ln, 0.055f * (1.f - std::fabs((float)k) / 11.f)), 1.f);
    }
    // the pool of light under the centre case: a wide tinted spill, then a brighter core
    const float fy = sc.floor_y(0.f);
    fan(dl, ImVec2(sc.cx, fy + sc.U * 0.05f), sc.U * 2.5f, sc.U * 0.52f, alpha(s.light, 0.30f * lit), alpha(s.light, 0.f), 44);
    fan(dl, ImVec2(sc.cx, fy + sc.U * 0.03f), sc.U * 1.05f, sc.U * 0.23f, alpha(rgba(255, 255, 255), 0.46f * lit), alpha(s.light, 0.f), 36);
    // a gloss streak across the floor
    const float fy1 = fy + sc.U * 0.55f;
    dl->AddRectFilledMultiColor(ImVec2(p0.x, yw), ImVec2(p1.x, fy1), alpha(rgba(255, 255, 255), 0.05f), alpha(rgba(255, 255, 255), 0.0f), alpha(rgba(255, 255, 255), 0.0f), alpha(rgba(255, 255, 255), 0.0f));
    dl->PopClipRect();
}

void cases_vignette(const CaseScene& sc, ImVec2 p0, ImVec2 p1, float strength) {
    ImDrawList* dl = sc.dl;
    const float W = p1.x - p0.x, H = p1.y - p0.y;
    const ImU32 d = IM_COL32(2, 3, 6, (int)(150.f * std::max(0.f, std::min(1.f, strength)))), z = IM_COL32(2, 3, 6, 0);
    const float ex = W * 0.16f, ey = H * 0.18f;
    dl->AddRectFilledMultiColor(p0, ImVec2(p0.x + ex, p1.y), d, z, z, d);
    dl->AddRectFilledMultiColor(ImVec2(p1.x - ex, p0.y), p1, z, d, d, z);
    dl->AddRectFilledMultiColor(p0, ImVec2(p1.x, p0.y + ey), d, d, z, z);
    dl->AddRectFilledMultiColor(ImVec2(p0.x, p1.y - ey), p1, z, z, d, d);
}

void glow_text(ImDrawList* dl, ImFont* font, float size, ImVec2 p, ImU32 col, ImU32 glow, float strength, const char* text) {
    if (!font || !text) return;
    if (strength > 0.01f) {
        for (int r = 1; r <= 3; ++r) {
            const float o = size * 0.055f * (float)r;
            const ImU32 g = alpha(glow, strength * 0.16f / (float)r);
            for (int k = 0; k < 8; ++k) {
                const float an = 6.2831853f * (float)k / 8.f;
                dl->AddText(font, size, ImVec2(p.x + std::cos(an) * o, p.y + std::sin(an) * o), g, text);
            }
        }
    }
    dl->AddText(font, size, ImVec2(p.x + 1.5f, p.y + 2.f), IM_COL32(0, 0, 0, (int)(110 * std::min(1.f, ca(col) / 255.f))), text);
    dl->AddText(font, size, p, col, text);
}

}  // namespace shelfdraw
