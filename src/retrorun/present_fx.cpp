#include "present_fx.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace fx {

/* ------------------------------------------------------------------ names */

static std::string lower_s(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

bool parse_filter(const std::string& name, Filter* out) {
    std::string n = lower_s(name);
    Filter f;
    if (n == "off" || n == "none" || n.empty()) f = Filter::Off;
    else if (n == "crt") f = Filter::Crt;
    else if (n == "lcd") f = Filter::Lcd;
    else if (n == "dmg") f = Filter::Dmg;
    else if (n == "sharp") f = Filter::Sharp;
    else return false;
    if (out) *out = f;
    return true;
}
const char* filter_name(Filter f) {
    switch (f) { case Filter::Crt: return "crt"; case Filter::Lcd: return "lcd"; case Filter::Dmg: return "dmg"; case Filter::Sharp: return "sharp"; default: return "off"; }
}
const char* filter_label(Filter f) {
    switch (f) { case Filter::Crt: return "CRT"; case Filter::Lcd: return "LCD grid"; case Filter::Dmg: return "DMG green"; case Filter::Sharp: return "Sharp bilinear"; default: return "Off"; }
}
Filter next_filter(Filter f) {
    switch (f) { case Filter::Off: return Filter::Crt; case Filter::Crt: return Filter::Lcd; case Filter::Lcd: return Filter::Dmg; case Filter::Dmg: return Filter::Sharp; default: return Filter::Off; }
}

/* ------------------------------------------------------------------ pixel helpers (SWAR, 8-bit lanes, weights 0..256) */

static inline uint32_t lerp_px(uint32_t a, uint32_t b, uint32_t w) {
    uint32_t iw = 256 - w;
    uint32_t rb = (((a & 0xFF00FFu) * iw + (b & 0xFF00FFu) * w) >> 8) & 0xFF00FFu;
    uint32_t g = (((a & 0xFF00u) * iw + (b & 0xFF00u) * w) >> 8) & 0xFF00u;
    return rb | g;
}
static inline uint32_t scale_px(uint32_t p, uint32_t g) {
    return ((((p & 0xFF00FFu) * g) >> 8) & 0xFF00FFu) | ((((p & 0xFF00u) * g) >> 8) & 0xFF00u);
}

/* ------------------------------------------------------------------ plan */

static const int kLevels = 32;

struct Engine::Plan {
    Filter f = Filter::Off;
    int sw = 0, sh = 0, dw = 0, dh = 0;
    bool crt = false;
    struct Cols { std::vector<int32_t> j; std::vector<uint16_t> w; };
    std::vector<Cols> cols;
    struct Row { int j0, j1; uint16_t wy, gain; uint8_t level; };
    std::vector<Row> rows;
    std::vector<uint16_t> mask;   // dw * 3 (r, g, b gains 0..256); empty = none
};

static double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

static std::shared_ptr<Engine::Plan> build_plan(Filter f, int sw, int sh, int dw, int dh) {
    auto P = std::make_shared<Engine::Plan>();
    P->f = f; P->sw = sw; P->sh = sh; P->dw = dw; P->dh = dh;
    P->crt = f == Filter::Crt;
    const double sx = (double)dw / sw, sy = (double)dh / sh;
    const int levels = P->crt ? kLevels : 1;
    const double K = 0.05;

    P->cols.resize(levels);
    for (int l = 0; l < levels; l++) {
        double c = 1.0 + K * (l / (double)(kLevels - 1));
        auto& C = P->cols[l];
        C.j.resize(dw); C.w.resize(dw);
        for (int x = 0; x < dw; x++) {
            double q = (x + 0.5) / sx;
            double cx = sw * 0.5;
            if (P->crt) q = cx + (q - cx) * c;
            double u = q - 0.5, fj = std::floor(u), t = u - fj;
            double s = P->crt ? sx / c : sx;
            double tp = clampd((t - 0.5) * s + 0.5, 0.0, 1.0);
            if (fj < -1 || fj > sw - 1) { C.j[x] = 0; C.w[x] = 0; continue; }   // outside the bowed image: black pad pixel
            C.j[x] = (int32_t)fj + 1;
            C.w[x] = (uint16_t)std::lround(tp * 256);
        }
    }
    P->rows.resize(dh);
    for (int y = 0; y < dh; y++) {
        double q = (y + 0.5) / sy, u = q - 0.5, fj = std::floor(u), t = u - fj;
        double tp = clampd((t - 0.5) * sy + 0.5, 0.0, 1.0);
        auto& R = P->rows[y];
        R.j0 = std::clamp((int)fj, 0, sh - 1); R.j1 = std::clamp((int)fj + 1, 0, sh - 1);
        R.wy = (uint16_t)std::lround(tp * 256);
        if (R.wy == 0) R.j1 = R.j0;
        else if (R.wy >= 256) { R.j0 = R.j1; R.wy = 0; }
        double yn = (y + 0.5) / dh * 2 - 1;
        R.level = P->crt ? (uint8_t)std::lround(yn * yn * (kLevels - 1)) : 0;
        R.gain = 256;
    }

    // grid / scanline / mask strengths (fade out when the scale is too small to show a pattern)
    auto fade = [](double s) { return clampd((s - 1.5) / 2.0, 0.0, 1.0); };
    if (f == Filter::Lcd || f == Filter::Dmg) {
        double gs = (f == Filter::Lcd ? 0.30 : 0.20);
        double gx = gs * fade(sx), gy = gs * fade(sy);
        P->mask.resize((size_t)dw * 3);
        for (int x = 0; x < dw; x++) {
            double p = (x + 0.5) / sx; p -= std::floor(p);
            double d = std::fabs(p - 0.5) * 2; d = d * d * d * d;
            uint16_t g = (uint16_t)std::lround(256 * (1.0 - gx * d));
            P->mask[x * 3] = P->mask[x * 3 + 1] = P->mask[x * 3 + 2] = g;
        }
        for (int y = 0; y < dh; y++) {
            double p = (y + 0.5) / sy; p -= std::floor(p);
            double d = std::fabs(p - 0.5) * 2; d = d * d * d * d;
            P->rows[y].gain = (uint16_t)std::lround(256 * (1.0 - gy * d));
        }
    } else if (f == Filter::Crt) {
        double ss = 0.50 * fade(sy), ms = 0.30 * fade(sx);
        P->mask.resize((size_t)dw * 3);
        for (int x = 0; x < dw; x++) {
            double xn = (x + 0.5) / dw * 2 - 1;
            double vig = 1.0 - 0.14 * xn * xn;
            for (int ch = 0; ch < 3; ch++) {
                double base = (x % 3 == ch) ? 1.0 : 1.0 - ms;
                if (ms == 0) base = 1.0;
                P->mask[x * 3 + ch] = (uint16_t)std::lround(256 * base * vig);
            }
        }
        for (int y = 0; y < dh; y++) {
            double p = (y + 0.5) / sy; p -= std::floor(p);
            double d = std::fabs(p - 0.5) * 2;
            double yn = (y + 0.5) / dh * 2 - 1;
            double g = (1.0 - ss * d * d) * (1.0 - 0.14 * yn * yn);
            P->rows[y].gain = (uint16_t)std::lround(256 * g);
        }
    }
    return P;
}

/* ------------------------------------------------------------------ filter render */

static void render_band(const Engine::Plan& P, const Src& s, uint32_t* out, int y0, int y1) {
    const int sw = P.sw, dw = P.dw;
    std::vector<uint32_t> vrow(sw + 2), base(dw);
    int kj0 = -1, kj1 = -1, kw = -1, kl = -1;
    const bool black = P.crt;
    const bool mask = !P.mask.empty();
    for (int y = y0; y < y1; y++) {
        const auto& R = P.rows[y];
        if (R.j0 != kj0 || R.j1 != kj1 || R.wy != kw || R.level != kl) {
            kj0 = R.j0; kj1 = R.j1; kw = R.wy; kl = R.level;
            const uint32_t* a = s.px + (size_t)R.j0 * s.stride;
            if (R.wy == 0) for (int x = 0; x < sw; x++) vrow[x + 1] = a[x];
            else { const uint32_t* b = s.px + (size_t)R.j1 * s.stride; for (int x = 0; x < sw; x++) vrow[x + 1] = lerp_px(a[x], b[x], R.wy); }
            vrow[0] = black ? 0 : vrow[1];
            vrow[sw + 1] = black ? 0 : vrow[sw];
            const auto& C = P.cols[R.level];
            const int32_t* cj = C.j.data(); const uint16_t* cw = C.w.data();
            if (!mask) {
                for (int x = 0; x < dw; x++) { uint32_t p = vrow[cj[x]]; if (cw[x]) p = lerp_px(p, vrow[cj[x] + 1], cw[x]); base[x] = p | 0xFF000000u; }
            } else {
                const uint16_t* m = P.mask.data();
                for (int x = 0; x < dw; x++) {
                    uint32_t p = vrow[cj[x]]; if (cw[x]) p = lerp_px(p, vrow[cj[x] + 1], cw[x]);
                    uint32_t r = (((p >> 16) & 255) * m[x * 3]) >> 8, g = (((p >> 8) & 255) * m[x * 3 + 1]) >> 8, b = ((p & 255) * m[x * 3 + 2]) >> 8;
                    base[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
                }
            }
        }
        uint32_t* o = out + (size_t)y * dw;
        if (R.gain >= 256) std::memcpy(o, base.data(), (size_t)dw * 4);
        else { const uint32_t g = R.gain; for (int x = 0; x < dw; x++) o[x] = scale_px(base[x], g) | 0xFF000000u; }
    }
}

static uint32_t dmg_lut_color(int luma) {
    static const int stops[4][3] = {{15, 56, 15}, {48, 98, 48}, {139, 172, 15}, {155, 188, 15}};
    double t = luma / 255.0 * 3.0; int i = std::min(2, (int)t); double f = t - i;
    int c[3];
    for (int k = 0; k < 3; k++) c[k] = (int)std::lround(stops[i][k] * (1 - f) + stops[i + 1][k] * f);
    return 0xFF000000u | (c[0] << 16) | (c[1] << 8) | c[2];
}

void Engine::render(Filter f, const Src& src0, int dw, int dh, std::vector<uint32_t>* out) {
    if (f == Filter::Off || !src0.px || src0.w <= 0 || src0.h <= 0 || dw <= 0 || dh <= 0 || !out) return;
    Src src = src0;
    if (f == Filter::Dmg) {   // tint the (small) source once, then scale as usual
        static uint32_t lut[256]; static bool init = false;
        if (!init) { for (int i = 0; i < 256; i++) lut[i] = dmg_lut_color(i); init = true; }
        tint_.resize((size_t)src.w * src.h);
        for (int y = 0; y < src.h; y++) {
            const uint32_t* a = src.px + (size_t)y * src.stride; uint32_t* o = tint_.data() + (size_t)y * src.w;
            for (int x = 0; x < src.w; x++) { uint32_t p = a[x]; o[x] = lut[(((p >> 16) & 255) * 77 + ((p >> 8) & 255) * 150 + (p & 255) * 29) >> 8]; }
        }
        src.px = tint_.data(); src.stride = src.w;
    }
    if (!plan_ || plan_->f != f || plan_->sw != src.w || plan_->sh != src.h || plan_->dw != dw || plan_->dh != dh)
        plan_ = build_plan(f, src.w, src.h, dw, dh);
    out->resize((size_t)dw * dh);
    int nt = threads_ > 0 ? threads_ : (int)std::min(4u, std::max(1u, std::thread::hardware_concurrency()));
    nt = std::max(1, std::min(nt, dh / 128));
    if (nt <= 1) { render_band(*plan_, src, out->data(), 0, dh); return; }
    std::vector<std::thread> th;
    for (int t = 1; t < nt; t++) th.emplace_back([&, t] { render_band(*plan_, src, out->data(), dh * t / nt, dh * (t + 1) / nt); });
    render_band(*plan_, src, out->data(), 0, dh / nt);
    for (auto& t : th) t.join();
}

/* ------------------------------------------------------------------ bezels */

namespace {

struct Canvas {
    int W, H; uint32_t* p;
    void put(int x, int y, uint32_t rgb, float a) {
        if (x < 0 || y < 0 || x >= W || y >= H || a <= 0.f) return;
        uint32_t d = p[(size_t)y * W + x];
        if (a >= 1.f) { p[(size_t)y * W + x] = 0xFF000000u | rgb; return; }
        auto mix = [&](int sh) { int s = (rgb >> sh) & 255, t = (d >> sh) & 255; return (uint32_t)(t + (s - t) * a + 0.5f) << sh; };
        p[(size_t)y * W + x] = 0xFF000000u | mix(16) | mix(8) | mix(0);
    }
    /* Rounded rect (signed distance, 1 px anti-aliasing). */
    void rr(float x0, float y0, float x1, float y1, float r, uint32_t rgb, float alpha = 1.f) {
        if (x1 <= x0 || y1 <= y0) return;
        r = std::min(r, std::min(x1 - x0, y1 - y0) * 0.5f);
        float hx = (x1 - x0) * 0.5f, hy = (y1 - y0) * 0.5f, cx = x0 + hx, cy = y0 + hy;
        int ix0 = std::max(0, (int)std::floor(x0) - 1), ix1 = std::min(W - 1, (int)std::ceil(x1) + 1);
        int iy0 = std::max(0, (int)std::floor(y0) - 1), iy1 = std::min(H - 1, (int)std::ceil(y1) + 1);
        for (int y = iy0; y <= iy1; y++) for (int x = ix0; x <= ix1; x++) {
            float qx = std::fabs(x + 0.5f - cx) - hx + r, qy = std::fabs(y + 0.5f - cy) - hy + r;
            float d = std::hypot(std::max(qx, 0.f), std::max(qy, 0.f)) + std::min(std::max(qx, qy), 0.f) - r;
            float cov = std::min(1.f, std::max(0.f, 0.5f - d));
            if (cov > 0) put(x, y, rgb, cov * alpha);
        }
    }
    void circ(float cx, float cy, float r, uint32_t rgb, float alpha = 1.f) { rr(cx - r, cy - r, cx + r, cy + r, r, rgb, alpha); }
    void vgrad(int y0, int y1, uint32_t top, uint32_t bot) {
        for (int y = std::max(0, y0); y < std::min(H, y1); y++) {
            float t = (float)(y - y0) / std::max(1, y1 - y0 - 1);
            auto ch = [&](int sh) { return (uint32_t)(((top >> sh) & 255) * (1 - t) + ((bot >> sh) & 255) * t + 0.5f) << sh; };
            uint32_t c = 0xFF000000u | ch(16) | ch(8) | ch(0);
            std::fill(p + (size_t)y * W, p + (size_t)y * W + W, c);
        }
    }
};

struct Style {
    uint32_t top, bot, panel, accent1, accent2, dpad;
    uint32_t btn[4];
    int face;        // 0 none, 1 A/B diagonal pair, 2 diamond of 4, 3 horizontal pair (B A)
    bool pills, speaker, led;
};

const Style* style_for(const std::string& sys) {
    static const Style gb   {0xCFCDC4, 0xB4B2A8, 0x555766, 0x7B2F6E, 0x35456F, 0x2C2C32, {0x9E2057, 0x9E2057, 0, 0}, 1, true, true, true};
    static const Style gbc  {0x7A58C8, 0x4B3592, 0x2A2A38, 0xB9A4F0, 0x8A70D8, 0x1E1E26, {0x2B2B34, 0x2B2B34, 0, 0}, 1, true, true, true};
    static const Style gba  {0x6A5DB8, 0x453B8C, 0x1E1E2A, 0xA79BE8, 0x8378D0, 0x1C1C24, {0xB3B0CC, 0xB3B0CC, 0, 0}, 1, true, true, false};
    static const Style nes  {0xB8B7B2, 0x9A9994, 0x1F1F22, 0xB8222C, 0x1F1F22, 0x161618, {0xC02A33, 0xC02A33, 0, 0}, 3, false, false, true};
    static const Style snes {0xCBC9C6, 0xAAA8A6, 0x6E6A86, 0x8D86B4, 0x5A5478, 0x35353C, {0xC8344A, 0xE8B72C, 0x3A9A55, 0x3C6CC4}, 2, true, false, false};
    static const Style nds  {0xC3C8D0, 0x959BA6, 0x1D1E25, 0x8B93A2, 0x6C7484, 0x2B2D34, {0x6F7480, 0x6F7480, 0x6F7480, 0x6F7480}, 2, true, true, true};
    static const Style ps1  {0xB4B4B8, 0x8E8E94, 0x25252B, 0x777782, 0x5C5C66, 0x3A3A42, {0x3CAA5C, 0xD64A4A, 0x4A70D0, 0xD070B0}, 2, false, false, false};
    static const Style gen  {0x20232C, 0x0E0F14, 0x07070A, 0x4A5BD8, 0x2C3680, 0x2A2D38, {0, 0, 0, 0}, 0, false, false, false};
    if (sys == "gb") return &gb;
    if (sys == "gbc") return &gbc;
    if (sys == "gba") return &gba;
    if (sys == "nes") return &nes;
    if (sys == "snes") return &snes;
    if (sys == "nds") return &nds;
    if (sys == "ps1") return &ps1;
    return &gen;
}

void button(Canvas& c, float x, float y, float r, uint32_t col) {
    c.circ(x, y + r * 0.18f, r * 1.04f, 0x000000, 0.38f);                  // soft contact shadow (boot-art "lip")
    c.circ(x, y, r, col);
    c.circ(x, y - r * 0.28f, r * 0.62f, 0xFFFFFF, 0.20f);                   // top gloss
}

void dpad(Canvas& c, float cx, float cy, float R, uint32_t col) {
    float a = R * 0.34f;
    c.rr(cx - R, cy - a + R * 0.08f, cx + R, cy + a + R * 0.08f, a * 0.4f, 0x000000, 0.35f);
    c.rr(cx - a + 0, cy - R + R * 0.08f, cx + a, cy + R + R * 0.08f, a * 0.4f, 0x000000, 0.35f);
    c.rr(cx - R, cy - a, cx + R, cy + a, a * 0.4f, col);
    c.rr(cx - a, cy - R, cx + a, cy + R, a * 0.4f, col);
    c.rr(cx - R + 2, cy - a + 1, cx + R - 2, cy - a + 1 + std::max(1.5f, a * 0.25f), 1.f, 0xFFFFFF, 0.14f);
    c.circ(cx, cy, a * 0.55f, 0x000000, 0.25f);
}

}  // namespace

static void draw_bezel(Canvas& c, const std::string& sys, const Rect& sc) {
    const Style& st = *style_for(sys);
    const int W = c.W, H = c.H;
    c.vgrad(0, H, st.top, st.bot);
    c.rr(0, 0, (float)W, 3.f, 0, 0xFFFFFF, 0.25f);                          // top highlight
    const float pm = std::clamp(H / 40.f, 6.f, 36.f);
    const float px0 = (float)sc.x - pm, py0 = (float)sc.y - pm, px1 = (float)(sc.x + sc.w) + pm, py1 = (float)(sc.y + sc.h) + pm;

    // accent stripes above the panel
    if (sys != "nes" && py0 > 18) {
        float t = std::max(2.f, pm * 0.28f);
        c.rr(px0, py0 - 3 * t, px1, py0 - 2 * t, t * 0.5f, st.accent1);
        c.rr(px0, py0 - 5.2f * t, px1, py0 - 4.2f * t, t * 0.5f, st.accent2);
    }
    // screen panel: shadow lip, panel, inner rim
    c.rr(px0 - 2, py0 - 1, px1 + 2, py1 + 4, pm * 1.3f, 0x000000, 0.35f);
    c.rr(px0, py0, px1, py1, pm * 1.2f, st.panel);
    c.rr(px0 + 1, py0 + 1, px1 - 1, py0 + 1 + std::max(1.5f, pm * 0.12f), 1.f, 0xFFFFFF, 0.14f);
    c.rr((float)sc.x - 2, (float)sc.y - 2, (float)(sc.x + sc.w) + 2, (float)(sc.y + sc.h) + 2, 2.f, 0x000000);
    if (st.led) {
        float ly = (float)sc.y + sc.h * 0.5f, lx = px0 + pm * 0.5f;
        if (pm >= 10) { c.circ(lx, ly, pm * 0.14f, 0xD83030); c.circ(lx, ly, pm * 0.30f, 0xD83030, 0.18f); }
    }

    // side clusters
    const float lbar = px0, rbar = (float)W - px1;
    const float cy = H * 0.52f;
    auto cluster_r = [&](float bar) { return std::min(bar * 0.36f, H * 0.12f); };
    float Rl = cluster_r(lbar), Rr = cluster_r(rbar);
    if (Rl >= 14) dpad(c, lbar * 0.5f, cy, Rl, st.dpad);
    if (Rr >= 14) {
        float cx = px1 + rbar * 0.5f, R = Rr;
        if (st.face == 1) { button(c, cx - R * 0.5f, cy + R * 0.22f, R * 0.40f, st.btn[0]); button(c, cx + R * 0.5f, cy - R * 0.22f, R * 0.40f, st.btn[1]); }
        else if (st.face == 3) { c.rr(cx - R * 1.0f, cy - R * 0.6f, cx + R * 1.0f, cy + R * 0.6f, R * 0.3f, 0x101012, 0.9f); button(c, cx - R * 0.45f, cy, R * 0.34f, st.btn[0]); button(c, cx + R * 0.45f, cy, R * 0.34f, st.btn[1]); }
        else if (st.face == 2) {
            float d = R * 0.62f, r = R * 0.31f;
            button(c, cx, cy - d, r, st.btn[0]); button(c, cx - d, cy, r, st.btn[1]); button(c, cx + d, cy, r, st.btn[2]); button(c, cx, cy + d, r, st.btn[3]);
        }
    }
    // start / select pills + speaker grooves in the bottom bar
    float bh = (float)H - py1;
    if (bh >= 40) {
        float by = py1 + bh * 0.5f, ph = std::clamp(bh * 0.14f, 4.f, 10.f), pw = ph * 3.6f;
        if (st.pills) for (int i = -1; i <= 1; i += 2) {
            float x = W * 0.5f + i * pw * 0.9f;
            c.rr(x - pw * 0.5f, by - ph * 0.5f + 1.5f, x + pw * 0.5f, by + ph * 0.5f + 1.5f, ph * 0.5f, 0x000000, 0.3f);
            c.rr(x - pw * 0.5f, by - ph * 0.5f, x + pw * 0.5f, by + ph * 0.5f, ph * 0.5f, st.dpad);
        }
        if (st.speaker) for (int i = 0; i < 5; i++) {
            float x = px1 - pm * 3 - i * ph * 1.6f;
            c.rr(x - ph * 0.25f, by - bh * 0.22f, x + ph * 0.25f, by + bh * 0.22f, ph * 0.25f, 0x000000, 0.32f);
        }
    }
    // generic frame: thin glow line around the panel
    if (st.face == 0) c.rr(px0 - 3, py0 - 3, px1 + 3, py1 + 3, pm * 1.3f, st.accent1, 0.22f), c.rr(px0, py0, px1, py1, pm * 1.2f, st.panel),
        c.rr((float)sc.x - 2, (float)sc.y - 2, (float)(sc.x + sc.w) + 2, (float)(sc.y + sc.h) + 2, 2.f, 0x000000);
}

const std::vector<uint32_t>& Engine::bezel(const std::string& system, int ow, int oh, const Rect& screen) {
    std::string sys = lower_s(system);
    if (!bez_px_.empty() && sys == bez_sys_ && ow == bez_ow_ && oh == bez_oh_ && screen.x == bez_rect_.x && screen.y == bez_rect_.y && screen.w == bez_rect_.w && screen.h == bez_rect_.h)
        return bez_px_;
    bez_px_.assign((size_t)std::max(0, ow) * std::max(0, oh), 0xFF000000u);
    if (ow > 0 && oh > 0) { Canvas c{ow, oh, bez_px_.data()}; draw_bezel(c, sys, screen); }
    bez_sys_ = sys; bez_ow_ = ow; bez_oh_ = oh; bez_rect_ = screen;
    return bez_px_;
}

}  // namespace fx
