#include "start_hint.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace starthint {

float alpha_at(long long t) {
    if (t < 0 || t >= kTotalMs) return 0.f;
    float a = 1.f;
    if (t < kFadeInMs) a = (float)t / (float)kFadeInMs;
    else if (t >= kFadeInMs + kHoldMs) a = (float)(kTotalMs - t) / (float)kFadeOutMs;
    return a * kMaxAlpha;
}

std::vector<Row> rows(Mode m) {
    if (m == Mode::Keyboard)
        return {{{TokKind::Key, "Esc"}, {TokKind::Label, "back to RetroHub"}},
                {{TokKind::Key, "F12"}, {TokKind::Label, "screenshot"}}};
    return {{{TokKind::Text, "Hold"}, {TokKind::Pad, "View"}, {TokKind::Plus, "+"}, {TokKind::Pad, "Y"}, {TokKind::Label, "game menu"}},
            {{TokKind::Text, "Hold"}, {TokKind::Pad, "View"}, {TokKind::Plus, "+"}, {TokKind::Pad, "Menu"}, {TokKind::Label, "back to RetroHub"}}};
}

std::string plain(const Row& r) {
    std::string s;
    for (const Tok& t : r) {
        if (!s.empty()) s += t.kind == TokKind::Label ? " - " : " ";
        s += t.text;
    }
    return s;
}

Mode pick_mode(bool kb, bool runner) { return (kb && runner) ? Mode::Keyboard : Mode::Pad; }

float scale_for_height(int h) { return std::max(1.f, std::min(3.f, (float)h / 540.f)); }

void placement(int aw, int ah, const Bitmap& b, int* x, int* y) {
    int m = std::max(8, ah / 40);
    *x = m;
    *y = std::max(0, ah - b.h - m);
    (void)aw;
}

void composite(uint32_t* dst, int w, int h, int pitch, const Bitmap& b, float alpha) {
    if (!dst || b.px.empty() || alpha <= 0.f) return;
    int ox, oy;
    placement(w, h, b, &ox, &oy);
    for (int y = 0; y < b.h; y++) {
        int dy = oy + y;
        if (dy < 0 || dy >= h) continue;
        for (int x = 0; x < b.w; x++) {
            int dx = ox + x;
            if (dx < 0 || dx >= w) continue;
            uint32_t s = b.px[(size_t)y * b.w + x];
            float sa = ((s >> 24) / 255.f) * alpha;
            if (sa <= 0.f) continue;
            uint32_t& d = dst[(size_t)dy * pitch + dx];
            auto mix = [&](int sh) { float sc = (float)((s >> sh) & 255), dc = (float)((d >> sh) & 255); return (uint32_t)std::lround(sc * sa + dc * (1.f - sa)) & 255u; };
            d = (d & 0xFF000000u) | (mix(16) << 16) | (mix(8) << 8) | mix(0);
        }
    }
}

#ifdef _WIN32
namespace {

struct Canvas {
    int w, h;
    std::vector<uint32_t>* px;
    void blend(int x, int y, int r, int g, int b, float a) {
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0.f) return;
        a = std::min(1.f, a);
        uint32_t& d = (*px)[(size_t)y * w + x];
        float da = (d >> 24) / 255.f, oa = a + da * (1.f - a);
        auto ch = [&](int sc, int sh) { float dc = (float)((d >> sh) & 255); return (uint32_t)std::lround((sc * a + dc * da * (1.f - a)) / oa) & 255u; };
        d = ((uint32_t)std::lround(oa * 255.f) << 24) | (ch(r, 16) << 16) | (ch(g, 8) << 8) | ch(b, 0);
    }
    /* Rounded rect (center cx,cy, half-size hx,hy, radius rad), anti-aliased; ring>0 draws only an outline of that width. */
    void rrect(float cx, float cy, float hx, float hy, float rad, int r, int g, int b, float alpha, float ring = 0.f) {
        int x0 = (int)std::floor(cx - hx - 1), x1 = (int)std::ceil(cx + hx + 1), y0 = (int)std::floor(cy - hy - 1), y1 = (int)std::ceil(cy + hy + 1);
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                float px_ = std::fabs(x + 0.5f - cx) - (hx - rad), py = std::fabs(y + 0.5f - cy) - (hy - rad);
                float d = std::hypot(std::max(px_, 0.f), std::max(py, 0.f)) + std::min(std::max(px_, py), 0.f) - rad;
                float cov = ring > 0.f ? std::min(std::max(0.5f - d, 0.f), 1.f) * std::min(std::max(0.5f + d + ring, 0.f), 1.f)
                                       : std::min(std::max(0.5f - d, 0.f), 1.f);
                blend(x, y, r, g, b, cov * alpha);
            }
    }
};

struct Font {
    HFONT f = nullptr;
    HDC dc = nullptr;
    Font(int px, int weight) {
        dc = CreateCompatibleDC(nullptr);
        f = CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        SelectObject(dc, f);
    }
    ~Font() { if (f) DeleteObject(f); if (dc) DeleteDC(dc); }
    std::wstring wide(const std::string& s) const {
        if (s.empty()) return L"";
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring w(n, 0);
        MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
        return w;
    }
    SIZE measure(const std::string& s) const {
        std::wstring w = wide(s);
        SIZE z{0, 0};
        GetTextExtentPoint32W(dc, w.c_str(), (int)w.size(), &z);
        return z;
    }
    /* Draws `s` white-on-black into a DIB and blends it onto the canvas (centered vertically on cy). */
    void draw(Canvas& c, const std::string& s, float x, float cy, int r, int g, int b, float alpha) const {
        SIZE z = measure(s);
        if (z.cx <= 0 || z.cy <= 0) return;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = z.cx;
        bi.bmiHeader.biHeight = -z.cy;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bm || !bits) return;
        HGDIOBJ old = SelectObject(dc, bm);
        std::memset(bits, 0, (size_t)z.cx * z.cy * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        std::wstring w = wide(s);
        TextOutW(dc, 0, 0, w.c_str(), (int)w.size());
        GdiFlush();
        const uint32_t* p = (const uint32_t*)bits;
        int ox = (int)std::lround(x), oy = (int)std::lround(cy - z.cy * 0.5f);
        for (int yy = 0; yy < z.cy; yy++)
            for (int xx = 0; xx < z.cx; xx++) {
                uint32_t v = p[(size_t)yy * z.cx + xx];
                float cov = (float)(((v >> 8) & 255)) / 255.f;
                c.blend(ox + xx, oy + yy, r, g, b, cov * alpha);
            }
        SelectObject(dc, old);
        DeleteObject(bm);
    }
};

void pad_color(const std::string& n, int* r, int* g, int* b) {
    if (n == "Y") { *r = 244; *g = 190; *b = 36; }
    else { *r = 74; *g = 78; *b = 96; }   // View / Menu: neutral grey like the hub's footer glyphs
}

struct Metrics {
    float s, gl, gap, row_h, pad;
    explicit Metrics(float sc) : s(sc), gl(22.f * sc), gap(7.f * sc), row_h(28.f * sc), pad(13.f * sc) {}
};

float tok_width(const Tok& t, const Font& f, const Font& fb, const Metrics& m) {
    switch (t.kind) {
        case TokKind::Text: return (float)f.measure(t.text).cx;
        case TokKind::Label: return (float)fb.measure(t.text).cx;
        case TokKind::Plus: return (float)f.measure("+").cx;
        case TokKind::Pad: return t.text.size() == 1 ? m.gl : m.gl;   // all glyphs are circles
        case TokKind::Key: return std::max(m.gl, (float)f.measure(t.text).cx + 12.f * m.s);
    }
    return 0.f;
}

}  // namespace

Bitmap render(Mode mode, float scale) {
    Metrics m(scale);
    Font f((int)std::lround(14.f * scale), FW_SEMIBOLD), fl((int)std::lround(14.f * scale), FW_NORMAL);
    std::vector<Row> rs = rows(mode);
    float maxw = 0.f;
    std::vector<float> widths;
    for (const Row& r : rs) {
        float w = 0.f;
        for (size_t i = 0; i < r.size(); i++) {
            w += tok_width(r[i], f, fl, m);
            if (i + 1 < r.size()) w += (r[i + 1].kind == TokKind::Label ? m.gap * 1.6f : m.gap);
        }
        widths.push_back(w);
        maxw = std::max(maxw, w);
    }
    Bitmap bm;
    bm.w = (int)std::ceil(maxw + 2 * m.pad);
    bm.h = (int)std::ceil(m.row_h * rs.size() + 2 * m.pad - 4.f * m.s);
    bm.px.assign((size_t)bm.w * bm.h, 0u);
    Canvas c{bm.w, bm.h, &bm.px};
    float rad = 12.f * scale;
    c.rrect(bm.w * 0.5f, bm.h * 0.5f, bm.w * 0.5f, bm.h * 0.5f, rad, 16, 17, 24, 0.80f);
    c.rrect(bm.w * 0.5f, bm.h * 0.5f, bm.w * 0.5f, bm.h * 0.5f, rad, 255, 255, 255, 0.16f, std::max(1.f, scale));
    for (size_t ri = 0; ri < rs.size(); ri++) {
        float cy = m.pad - 2.f * m.s + m.row_h * (ri + 0.5f), x = m.pad;
        const Row& r = rs[ri];
        for (size_t i = 0; i < r.size(); i++) {
            const Tok& t = r[i];
            float w = tok_width(t, f, fl, m);
            switch (t.kind) {
                case TokKind::Text: f.draw(c, t.text, x, cy, 230, 232, 240, 1.f); break;
                case TokKind::Plus: f.draw(c, "+", x, cy, 150, 154, 170, 1.f); break;
                case TokKind::Label: fl.draw(c, t.text, x, cy, 190, 194, 208, 1.f); break;
                case TokKind::Key:
                    c.rrect(x + w * 0.5f, cy, w * 0.5f, m.gl * 0.5f, 5.f * scale, 44, 46, 60, 1.f);
                    c.rrect(x + w * 0.5f, cy, w * 0.5f, m.gl * 0.5f, 5.f * scale, 255, 255, 255, 0.25f, 1.f);
                    f.draw(c, t.text, x + (w - f.measure(t.text).cx) * 0.5f, cy, 235, 236, 244, 1.f);
                    break;
                case TokKind::Pad: {
                    int pr, pg, pb;
                    pad_color(t.text, &pr, &pg, &pb);
                    float cx = x + w * 0.5f, rr = m.gl * 0.5f;
                    c.rrect(cx, cy, rr, rr, rr, pr, pg, pb, 1.f);
                    if (t.text == "Y") {
                        Font fy((int)std::lround(13.f * scale), FW_BOLD);
                        fy.draw(c, "Y", cx - fy.measure("Y").cx * 0.5f, cy, 24, 24, 30, 1.f);
                    } else if (t.text == "Menu") {   // three bars
                        for (int k = -1; k <= 1; k++) c.rrect(cx, cy + k * 4.2f * scale, 5.2f * scale, 0.75f * scale, 0.75f * scale, 240, 242, 250, 1.f);
                    } else {                          // View: two overlapping squares
                        c.rrect(cx - 1.6f * scale, cy - 1.6f * scale, 3.6f * scale, 3.2f * scale, 0.8f * scale, 240, 242, 250, 1.f, std::max(1.f, 1.2f * scale));
                        c.rrect(cx + 1.8f * scale, cy + 1.8f * scale, 3.6f * scale, 3.2f * scale, 0.8f * scale, 240, 242, 250, 1.f, std::max(1.f, 1.2f * scale));
                    }
                    break;
                }
            }
            x += w + (i + 1 < r.size() ? (r[i + 1].kind == TokKind::Label ? m.gap * 1.6f : m.gap) : 0.f);
        }
    }
    return bm;
}
#else
Bitmap render(Mode, float) { return Bitmap(); }
#endif

}  // namespace starthint
