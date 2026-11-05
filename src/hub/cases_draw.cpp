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

const std::vector<std::string>& wrap_cached(ImFont* f, float size, const char* text, float w, int maxl) {
    static std::unordered_map<uint64_t, std::vector<std::string>> cache;
    size = std::floor(size);                 // quantised so a case that is gliding keeps hitting the same entry
    w = std::floor(w / 8.f) * 8.f;
    uint64_t k = fnv(text);
    k = (k ^ (uint64_t)(int)size) * 1099511628211ull;
    k = (k ^ (uint64_t)(int)(w * 0.125f)) * 1099511628211ull;
    k = (k ^ (uint64_t)maxl) * 1099511628211ull;
    auto it = cache.find(k);
    if (it != cache.end()) return it->second;
    if (cache.size() > 160) cache.clear();
    return cache.emplace(k, ui::wrap_lines(f, size, text, w, maxl)).first->second;
}
const std::string& ellipsize_cached(ImFont* f, float size, const char* text, float w) {
    static std::unordered_map<uint64_t, std::string> cache;
    size = std::floor(size);
    w = std::floor(w / 6.f) * 6.f;
    uint64_t k = fnv(text, 7);
    k = (k ^ (uint64_t)(int)size) * 1099511628211ull;
    k = (k ^ (uint64_t)(int)(w * (1.f / 6.f))) * 1099511628211ull;
    auto it = cache.find(k);
    if (it != cache.end()) return it->second;
    if (cache.size() > 400) cache.clear();
    return cache.emplace(k, ui::ellipsize(f, size, text, w)).first->second;
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

/* ------------------------------- colours per console ------------------------------- */

struct Style {
    bool box = false;
    ImU32 body = 0, band = 0, band_text = 0, ink = 0, panel = 0;
    bool pale = false;
};
ImU32 body_of(Media m) {
    switch (m) {
        case Media::Ps1Jewel: return rgba(30, 31, 38);
        case Media::Ps2Case: return rgba(20, 25, 42);
        case Media::GcCase: return rgba(84, 64, 156);
        case Media::WiiUCase: return rgba(34, 128, 196);
        case Media::PspCase: return rgba(34, 38, 52);
        case Media::DsCase: return rgba(214, 217, 224);
        case Media::N3dsCase: return rgba(238, 240, 245);
        case Media::NesCart: return rgba(150, 147, 138);
        case Media::SnesCart: return rgba(164, 162, 180);
        case Media::N64Cart: return rgba(66, 69, 82);
        case Media::GbCart: return rgba(168, 170, 164);
        case Media::GbcCart: return rgba(112, 92, 168);
        case Media::GbaCart: return rgba(96, 88, 144);
        default: return rgba(52, 54, 64);
    }
}
Style style_of(const CaseDraw& d) {
    Style s;
    s.box = hubui::cases::is_box(d.media);
    s.body = body_of(d.media);
    ImU32 band = lerpc(d.c0, d.c1, 0.4f);
    if (lum(band) < 0.2f) band = lerpc(band, rgba(255, 255, 255), 0.22f);
    s.band = band;
    s.band_text = lum(band) > 0.6f ? rgba(22, 24, 32) : rgba(250, 250, 253);
    s.pale = lum(s.body) > 0.55f;
    s.ink = s.pale ? rgba(30, 32, 42) : rgba(244, 245, 250);
    s.panel = s.pale ? lerpc(s.body, rgba(255, 255, 255), 0.55f) : lerpc(s.body, rgba(8, 9, 14), 0.62f);
    return s;
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

void header_band(Ctx& x, const Face& f, const CaseDraw& d, const Style& st, float u0, float v0, float u1, float bv) {
    quad(x, f, u0, v0, u1, v0 + bv, st.band);
    vgrad(x, f, u0, v0, u1, v0 + bv, rgba(255, 255, 255, 54), rgba(0, 0, 0, 40));
    quad(x, f, u0, v0 + bv, u1, v0 + bv + 1.5f / f.H, rgba(0, 0, 0, 90));
    ImFont* cf = ui::F.bold ? ui::F.bold : ui::F.cap;
    char name[48];
    upper_into(name, sizeof name, d.sys_name);
    float bs = bv * f.H * 0.52f;
    const float avail = (u1 - u0) * f.W * 0.74f;
    const float tw0 = text_w(cf, bs, name) + (float)std::strlen(name) * bs * 0.12f;
    if (tw0 > avail) bs *= avail / tw0;
    if (bs >= 5.f) face_text(x, f, cf, bs, name, u0 * f.W + (u1 - u0) * f.W * 0.05f, (v0 + bv * 0.5f) * f.H - bs * 0.5f, st.band_text, false, bs * 0.12f);
    console_mark(x, f, u1 - (u1 - u0) * 0.07f - bv * f.H * 0.3f / f.W, v0 + bv * 0.5f, bv * f.H * 0.46f, hash32(d.sys_short), st.band_text);
}

void paint_front(Ctx& x, const Face& f, const CaseDraw& d, const Style& st, CaseKind kind, float sheen_u) {
    const float W = f.W, H = f.H;
    const float px = 1.f / W, py = 1.f / H;
    const bool box = st.box;
    const float rcu = std::min(0.035f, (box ? 2.5f : 5.f) * px), rcv = rcu * W * py;
    poly(x, f, {ImVec2(rcu, 0.f), ImVec2(1.f - rcu, 0.f), ImVec2(1.f, rcv), ImVec2(1.f, 1.f - rcv), ImVec2(1.f - rcu, 1.f), ImVec2(rcu, 1.f), ImVec2(0.f, 1.f - rcv), ImVec2(0.f, rcv)}, st.body);
    vgrad(x, f, 0.f, 0.f, 1.f, 1.f, rgba(255, 255, 255, box ? 18 : 30), rgba(0, 0, 0, box ? 36 : 40));
    float cu0, cv0, cu1, cv1;
    if (box) {
        cu0 = 0.028f; cv0 = 0.028f; cu1 = 1.f - 0.028f; cv1 = 1.f - 0.03f;
    } else {
        const float hinge = kind == CaseKind::Jewel ? 0.066f : kind == CaseKind::Small ? 0.05f : 0.045f;
        quad(x, f, 0.f, 0.f, hinge, 1.f, shade(st.body, 0.8f));
        quad(x, f, hinge - px, 0.f, hinge + px, 1.f, rgba(0, 0, 0, 70));
        quad(x, f, hinge + px, 0.f, hinge + 2.f * px, 1.f, rgba(255, 255, 255, 36));
        cu0 = hinge + 2.f * px; cv0 = 0.02f; cu1 = 1.f - 0.024f; cv1 = 1.f - 0.02f;
    }
    if (d.art) image(x, f, cu0, cv0, cu1, cv1, d.art, d.art_w, d.art_h, 0.f);
    else cover_generated(x, f, cu0, cv0, cu1, cv1, d);
    header_band(x, f, d, st, cu0, cv0, cu1, std::max(0.05f, std::min(0.085f, 30.f * py)));
    outline(x, f, cu0, cv0, cu1, cv1, rgba(0, 0, 0, box ? 110 : 90), 1.f);
    if (box) {   // matte cardboard: worn edges and a fold line, no sheen
        hgrad(x, f, 0.f, 0.f, 0.05f, 1.f, rgba(0, 0, 0, 56), rgba(0, 0, 0, 0));
        hgrad(x, f, 0.95f, 0.f, 1.f, 1.f, rgba(0, 0, 0, 0), rgba(0, 0, 0, 62));
        vgrad(x, f, 0.f, 0.94f, 1.f, 1.f, rgba(0, 0, 0, 0), rgba(0, 0, 0, 50));
        line(x, f, 0.f, 0.006f, 1.f, 0.006f, rgba(255, 255, 255, 46), 1.f);
    } else {   // clear plastic: a bright rim and a moving glossy highlight
        outline(x, f, 0.f, 0.f, 1.f, 1.f, st.pale ? rgba(0, 0, 0, 70) : rgba(255, 255, 255, 64), 1.f);
        line(x, f, rcu + 0.01f, 2.f * py, 1.f - rcu - 0.01f, 2.f * py, rgba(255, 255, 255, st.pale ? 140 : 90), 1.f);
        grad(x, f, 0.f, 0.f, 1.f, 0.62f, rgba(255, 255, 255, 50), rgba(255, 255, 255, 14), rgba(255, 255, 255, 0), rgba(255, 255, 255, 0));
        const float uc = std::max(0.1f, std::min(0.9f, sheen_u));
        hgrad(x, f, std::max(0.f, uc - 0.2f), 0.f, uc, 1.f, rgba(255, 255, 255, 0), rgba(255, 255, 255, 36));
        hgrad(x, f, uc, 0.f, std::min(1.f, uc + 0.12f), 1.f, rgba(255, 255, 255, 36), rgba(255, 255, 255, 0));
    }
    if (d.fav) x.dl->AddCircleFilled(f.at(0.88f, 0.14f), std::max(5.f, W * 0.05f), x.c(rgba(255, 206, 64)), 16);
}

void paint_spine(Ctx& x, const Face& f, const CaseDraw& d, const Style& st, CaseKind kind) {
    const float W = f.W, H = f.H;
    const float px = 1.f / W;
    quad(x, f, 0.f, 0.f, 1.f, 1.f, st.body);
    hgrad(x, f, 0.f, 0.f, 1.f, 1.f, rgba(255, 255, 255, st.box ? 22 : 40), rgba(0, 0, 0, st.box ? 40 : 62));
    if (kind == CaseKind::Jewel) {   // clear rails and the black spine card
        quad(x, f, 0.f, 0.f, 0.12f, 1.f, rgba(150, 156, 170, 110));
        quad(x, f, 0.88f, 0.f, 1.f, 1.f, rgba(110, 116, 130, 110));
    }
    const float bv = std::max(0.075f, std::min(0.14f, 24.f / H));
    quad(x, f, 0.f, 0.f, 1.f, bv, st.band);
    vgrad(x, f, 0.f, 0.f, 1.f, bv, rgba(255, 255, 255, 54), rgba(0, 0, 0, 40));
    quad(x, f, 0.f, bv, 1.f, bv + 1.5f / H, rgba(0, 0, 0, 90));
    ImFont* cf = ui::F.bold ? ui::F.bold : ui::F.cap;
    float bs = std::min(W * 0.5f, bv * H * 0.5f);
    const float tw = text_w(cf, bs, d.sys_short);
    if (tw > W * 0.9f) bs *= W * 0.9f / tw;
    if (bs >= 5.f) face_text(x, f, cf, bs, d.sys_short, (W - text_w(cf, bs, d.sys_short)) * 0.5f, bv * H * 0.5f - bs * 0.5f, st.band_text, false);
    if (d.fav) quad(x, f, 0.f, bv + 2.f / H, 1.f, bv + 6.f / H, rgba(255, 206, 64));
    // the title, reading top to bottom
    ImFont* font = ui::F.bold ? ui::F.bold : ui::F.body;
    const float size = std::max(7.f, std::min(W * 0.55f, 22.f));
    const float avail = H * (0.9f - bv) - size * 0.5f;
    if (x.detail && W >= 9.f && avail > size * 2.f) {
        const std::string& t = ellipsize_cached(font, size, d.title, avail);
        const float lx = W * 0.5f + size * 0.38f, ly = bv * H + H * 0.03f;
        face_text(x, f, font, size, t.c_str(), lx + 0.8f, ly + 0.8f, rgba(0, 0, 0, st.pale ? 0 : 120), true);
        face_text(x, f, font, size, t.c_str(), lx, ly, st.ink, true);
    }
    quad(x, f, 0.f, 0.915f, 1.f, 0.925f, alpha(st.band, 0.9f));
    console_mark(x, f, 0.5f, 0.962f, std::min(W * 0.55f, H * 0.04f), hash32(d.sys_short), st.ink);
    if (!st.box) {
        line(x, f, px, 0.f, px, 1.f, rgba(255, 255, 255, st.pale ? 120 : 76), 1.f);
        line(x, f, 1.f - px, 0.f, 1.f - px, 1.f, rgba(255, 255, 255, st.pale ? 80 : 40), 1.f);
        hgrad(x, f, 0.18f, 0.f, 0.34f, 1.f, rgba(255, 255, 255, 0), rgba(255, 255, 255, 22));
        hgrad(x, f, 0.34f, 0.f, 0.5f, 1.f, rgba(255, 255, 255, 22), rgba(255, 255, 255, 0));
    }
}

void paint_back(Ctx& x, const Face& f, const CaseDraw& d, const Style& st, CaseKind kind) {
    const float W = f.W, H = f.H;
    const float px = 1.f / W, py = 1.f / H;
    const CaseBack* bk = d.back;
    quad(x, f, 0.f, 0.f, 1.f, 1.f, st.body);
    vgrad(x, f, 0.f, 0.f, 1.f, 1.f, rgba(255, 255, 255, 24), rgba(0, 0, 0, 40));
    // spine hinge: on the right when you look at the back
    const float hinge = kind == CaseKind::Jewel ? 0.066f : 0.045f;
    if (!st.box) { quad(x, f, 1.f - hinge, 0.f, 1.f, 1.f, shade(st.body, 0.8f)); quad(x, f, 1.f - hinge - px, 0.f, 1.f - hinge + px, 1.f, rgba(0, 0, 0, 70)); }
    const float u0 = st.box ? 0.03f : 0.03f, u1 = st.box ? 0.97f : 1.f - hinge - 0.014f, v0 = 0.025f, v1 = 0.975f;
    quad(x, f, u0, v0, u1, v1, st.panel);
    const ImU32 ink = st.pale ? rgba(26, 28, 38) : rgba(244, 245, 250);
    const ImU32 dim = st.pale ? rgba(70, 74, 90) : rgba(168, 174, 192);
    header_band(x, f, d, st, u0, v0, u1, std::max(0.05f, std::min(0.075f, 28.f * py)));
    const float pad = W * 0.045f;
    const float lx = u0 * W + pad, pw = (u1 - u0) * W - pad * 2.f;
    float y = (v0 + std::max(0.05f, std::min(0.075f, 28.f * py))) * H + H * 0.03f;
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
        const float colw = pw / 3.f;
        for (int i = 0; i < 3; ++i) {
            face_text(x, f, cap, cs, fs[i].cap, lx + colw * (float)i, y, dim, false, cs * 0.08f);
            const std::string& v = ellipsize_cached(bold, vs, fs[i].val, colw - pad * 0.5f);
            face_text(x, f, bold, vs, v.c_str(), lx + colw * (float)i, y + cs * 1.35f, ink, false);
        }
        y += cs * 1.35f + vs * 1.5f;
        if (bk && bk->maker && *bk->maker) {
            const std::string& v = ellipsize_cached(body, cs * 1.25f, bk->maker, pw);
            face_text(x, f, body, cs * 1.25f, v.c_str(), lx, y, dim, false);
            y += cs * 1.25f * 1.5f;
        }
        quad(x, f, u0 + 0.04f, y / H, u1 - 0.04f, y / H + 1.2f * py, alpha(ink, 0.18f));
        y += H * 0.014f;
        // the description fills the room above the pictures
        const float bs = std::max(7.f, std::min(W * 0.054f, 19.f));
        const bool has_pics = bk && bk->nshots > 0;
        const float pic_h = has_pics ? std::min(H * 0.17f, pw * 0.3f) : 0.f;
        const float bottom = (v1 - 0.03f) * H - (has_pics ? pic_h + cs * 2.4f : H * 0.11f);
        const char* desc = bk && bk->desc && *bk->desc ? bk->desc : "No description for this game yet.";
        int max_lines = (int)std::max(1.f, std::floor((bottom - y) / (bs * 1.3f)));
        if (bk && (bk->lines_w != pw || bk->lines_size != bs)) { bk->lines = ui::wrap_lines(body, bs, desc, pw, 40); bk->lines_w = pw; bk->lines_size = bs; }
        const std::vector<std::string>& dl_lines = bk ? bk->lines : wrap_cached(body, bs, desc, pw, 40);
        for (int i = 0; i < (int)dl_lines.size() && i < max_lines; ++i) {
            if (i == max_lines - 1 && (int)dl_lines.size() > max_lines) {
                const std::string t = ui::ellipsize(body, bs, dl_lines[(size_t)i] + " ...", pw);
                face_text(x, f, body, bs, t.c_str(), lx, y, ink, false);
            } else face_text(x, f, body, bs, dl_lines[(size_t)i].c_str(), lx, y, ink, false);
            y += bs * 1.3f;
        }
        // pictures: the player's own screenshots or save-state thumbnails
        if (has_pics) {
            const float gap = pw * 0.03f;
            const int n = bk->nshots;
            const float tw = (pw - gap * (float)(n - 1)) / (float)n;
            const float ty = (v1 - 0.03f) * H - pic_h;
            face_text(x, f, cap, cs, bk->shots_label, lx, ty - cs * 1.5f, dim, false, cs * 0.08f);
            for (int i = 0; i < n; ++i) {
                const float a = lx + (tw + gap) * (float)i;
                const float ua = a * px, ub = (a + tw) * px, va = ty * py, vb = (ty + pic_h) * py;
                quad(x, f, ua - px, va - py, ub + px, vb + py, rgba(0, 0, 0, 150));
                if (bk->shots[i].tex) image(x, f, ua, va, ub, vb, bk->shots[i].tex, bk->shots[i].w, bk->shots[i].h, 0.5f);
                else quad(x, f, ua, va, ub, vb, rgba(60, 64, 80));
            }
        } else {   // a generated barcode
            const float bx0 = u1 - 0.34f, by0 = v1 - 0.095f, bx1 = u1 - 0.04f, by1 = v1 - 0.03f;
            quad(x, f, bx0 - 0.012f, by0 - 0.01f, bx1 + 0.012f, by1 + 0.01f, rgba(244, 244, 246));
            uint32_t h = d.seed * 2654435761u + 17u;
            float u = bx0 + 0.006f;
            while (u < bx1 - 0.012f) {
                h = h * 1664525u + 1013904223u;
                const float w = (1.f + (float)((h >> 24) % 3u)) * 1.1f * px;
                if ((h >> 8) & 1u) quad(x, f, u, by0, std::min(bx1, u + w), by1, rgba(18, 18, 24));
                u += w + (0.8f + (float)((h >> 16) & 1u)) * px;
            }
        }
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
    float a = ps.alpha * (reflection ? ps.refl : 1.f);
    if (a <= 0.01f) return;
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
    };
    Face f;
    {   // back
        const float c[4][3] = {{hw, H, dd}, {-hw, H, dd}, {-hw, 0, dd}, {hw, 0, dd}}, n[3] = {0, 0, 1};
        if (make_face(sc, xf, c, n, cd.w, H, &f)) { Ctx xb = x; xb.detail = x.detail && face_ratio(f) > 0.5f; paint_back(xb, f, d, st, kind); shade_face(f, n); }
    }
    {   // left, right: both drawn as the spine design
        const float c[4][3] = {{-hw, H, dd}, {-hw, H, -dd}, {-hw, 0, -dd}, {-hw, 0, dd}}, n[3] = {-1, 0, 0};
        if (make_face(sc, xf, c, n, cd.d, H, &f)) { paint_spine(x, f, d, st, kind); shade_face(f, n); }
    }
    {
        const float c[4][3] = {{hw, H, -dd}, {hw, H, dd}, {hw, 0, dd}, {hw, 0, -dd}}, n[3] = {1, 0, 0};
        if (make_face(sc, xf, c, n, cd.d, H, &f)) { paint_spine(x, f, d, st, kind); shade_face(f, n); }
    }
    {   // top (seen when the case is tipped in Inspect)
        const float c[4][3] = {{-hw, H, dd}, {hw, H, dd}, {hw, H, -dd}, {-hw, H, -dd}}, n[3] = {0, 1, 0};
        if (make_face(sc, xf, c, n, cd.w, cd.d, &f)) { quad(x, f, 0.f, 0.f, 1.f, 1.f, shade(st.body, 1.15f)); quad(x, f, 0.f, 0.f, 1.f, 0.2f, rgba(255, 255, 255, 40)); shade_face(f, n); }
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

namespace {
constexpr float kWallZ = 5.2f;
struct StageColors { ImU32 wall_top, wall_hz, floor_far, floor_near, tint; };
StageColors stage_colors(ImU32 c0, ImU32 c1) {
    StageColors s;
    const ImU32 dark = rgba(6, 7, 11);
    s.tint = lerpc(c0, c1, 0.5f);
    if (lum(s.tint) < 0.12f) s.tint = lerpc(s.tint, rgba(120, 130, 190), 0.4f);
    s.wall_top = lerpc(dark, s.tint, 0.22f);
    s.wall_hz = lerpc(dark, s.tint, 0.50f);
    s.floor_far = lerpc(dark, s.tint, 0.30f);
    s.floor_near = lerpc(dark, s.tint, 0.07f);
    return s;
}
ImU32 floor_at(const StageColors& s, float yw, float yb, float y) { return lerpc(s.floor_far, s.floor_near, std::max(0.f, std::min(1.f, (y - yw) / std::max(1.f, yb - yw)))); }
void ellipse_layers(ImDrawList* dl, ImVec2 c, float rx, float ry, ImU32 col, int layers, int a_each) {
    for (int i = 0; i < layers; ++i) {
        const float t = 1.f - (float)i / (float)layers;
        ImVec2 p[28];
        for (int k = 0; k < 28; ++k) { const float an = 6.2831853f * (float)k / 28.f; p[k] = ImVec2(c.x + std::cos(an) * rx * t, c.y + std::sin(an) * ry * t); }
        dl->AddConvexPolyFilled(p, 28, alpha(col, (float)a_each / 255.f));
    }
}
}  // namespace

void cases_stage_back(const CaseScene& sc, ImVec2 p0, ImVec2 p1, ImU32 c0, ImU32 c1) {
    ImDrawList* dl = sc.dl;
    const StageColors s = stage_colors(c0, c1);
    const float yw = sc.floor_y(kWallZ);
    dl->AddRectFilledMultiColor(p0, ImVec2(p1.x, yw), s.wall_top, s.wall_top, s.wall_hz, s.wall_hz);
    dl->PushClipRect(p0, ImVec2(p1.x, yw + 1.f), true);
    const ImU32 glow = lerpc(s.tint, rgba(255, 255, 255), 0.35f);
    ellipse_layers(dl, ImVec2(sc.cx, yw - sc.U * 0.30f), (p1.x - p0.x) * 0.42f, sc.U * 1.25f, glow, 18, 5);
    dl->PopClipRect();
    dl->AddRectFilledMultiColor(ImVec2(p0.x, yw), p1, s.floor_far, s.floor_far, s.floor_near, s.floor_near);
    // soft horizon: a glow band over the junction, a bright line that fades out to the sides, and two faint lines above it
    const ImU32 hz = lerpc(s.tint, rgba(255, 255, 255), 0.55f);
    dl->AddRectFilledMultiColor(ImVec2(p0.x, yw - sc.U * 0.07f), ImVec2(p1.x, yw), alpha(hz, 0.f), alpha(hz, 0.f), alpha(hz, 0.16f), alpha(hz, 0.16f));
    const float mid = sc.cx;
    dl->AddRectFilledMultiColor(ImVec2(p0.x, yw - 1.f), ImVec2(mid, yw + 1.f), alpha(hz, 0.f), alpha(hz, 0.55f), alpha(hz, 0.55f), alpha(hz, 0.f));
    dl->AddRectFilledMultiColor(ImVec2(mid, yw - 1.f), ImVec2(p1.x, yw + 1.f), alpha(hz, 0.55f), alpha(hz, 0.f), alpha(hz, 0.f), alpha(hz, 0.55f));
    for (int i = 1; i <= 2; ++i) {
        const float y = yw - sc.U * 0.085f * (float)i;
        dl->AddRectFilledMultiColor(ImVec2(p0.x, y), ImVec2(mid, y + 1.f), alpha(hz, 0.f), alpha(hz, 0.16f / (float)i), alpha(hz, 0.16f / (float)i), alpha(hz, 0.f));
        dl->AddRectFilledMultiColor(ImVec2(mid, y), ImVec2(p1.x, y + 1.f), alpha(hz, 0.16f / (float)i), alpha(hz, 0.f), alpha(hz, 0.f), alpha(hz, 0.16f / (float)i));
    }
}

void cases_veil(const CaseScene& sc, ImVec2 p0, ImVec2 p1, ImU32 c0, ImU32 c1, float case_h) {
    ImDrawList* dl = sc.dl;
    const StageColors s = stage_colors(c0, c1);
    const float yw = sc.floor_y(kWallZ), yb = p1.y;
    const float y0 = sc.floor_y(2.2f), y1 = sc.floor_y(0.f) + case_h * 0.60f * sc.U;
    const float al[4] = {0.f, 0.22f, 0.62f, 1.f};
    const float ys[4] = {y0, y0 + (y1 - y0) * 0.32f, y0 + (y1 - y0) * 0.68f, y1};
    dl->PushClipRect(p0, p1, true);
    for (int i = 0; i < 3; ++i) {
        const ImU32 ca_ = floor_at(s, yw, yb, ys[i]), cb_ = floor_at(s, yw, yb, ys[i + 1]);
        dl->AddRectFilledMultiColor(ImVec2(p0.x, ys[i]), ImVec2(p1.x, ys[i + 1]), alpha(ca_, al[i]), alpha(ca_, al[i]), alpha(cb_, al[i + 1]), alpha(cb_, al[i + 1]));
    }
    dl->AddRectFilledMultiColor(ImVec2(p0.x, y1), ImVec2(p1.x, std::max(y1 + 1.f, p1.y)), floor_at(s, yw, yb, y1), floor_at(s, yw, yb, y1), floor_at(s, yw, yb, yb), floor_at(s, yw, yb, yb));
    dl->PopClipRect();
}

void cases_stage_front(const CaseScene& sc, ImVec2 p0, ImVec2 p1, ImU32 c0, ImU32 c1) {
    ImDrawList* dl = sc.dl;
    const StageColors s = stage_colors(c0, c1);
    const float yw = sc.floor_y(kWallZ);
    dl->PushClipRect(ImVec2(p0.x, yw), p1, true);
    const ImU32 ln = lerpc(s.tint, rgba(255, 255, 255), 0.5f);
    // perspective lines running away to the horizon
    const float vy = sc.hy;
    const float tw = (yw - vy) / std::max(1.f, p1.y - vy);
    for (int k = -9; k <= 9; ++k) {
        const float xb = sc.cx + (float)k * (p1.x - p0.x) * 0.105f;
        const float xt = sc.cx + (xb - sc.cx) * tw;
        dl->AddLine(ImVec2(xt, yw), ImVec2(xb, p1.y), alpha(ln, 0.05f), 1.f);
    }
    // the pool of light under the centre case and a gloss streak across the floor
    ellipse_layers(dl, ImVec2(sc.cx, sc.floor_y(0.f) + sc.U * 0.02f), sc.U * 0.95f, sc.U * 0.20f, lerpc(s.tint, rgba(255, 255, 255), 0.55f), 12, 6);
    const float fy1 = sc.floor_y(0.f) + sc.U * 0.55f;
    dl->AddRectFilledMultiColor(ImVec2(p0.x, yw), ImVec2(p1.x, fy1), alpha(rgba(255, 255, 255), 0.05f), alpha(rgba(255, 255, 255), 0.0f), alpha(rgba(255, 255, 255), 0.0f), alpha(rgba(255, 255, 255), 0.0f));
    dl->PopClipRect();
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
