#include "osd.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>

namespace osd {

namespace {

/* Classic 5x7 font, ASCII 32..126, 5 columns per glyph, bit0 = top row. */
const uint8_t kFont[95][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x56,0x20,0x50},{0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x2A,0x1C,0x7F,0x1C,0x2A},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},{0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x08,0x14,0x54,0x54,0x3C},
    {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},{0x7F,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},{0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},{0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7F,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},{0x08,0x08,0x2A,0x1C,0x08}};

struct Toast { std::string text; Kind kind; uint64_t born, until; };

std::mutex g_mu;
std::deque<Toast> g_toasts;
std::string g_corner;
Kind g_corner_kind = Kind::Info;
uint64_t (*g_clock)() = nullptr;
uint64_t g_gen = 0;          // bumped by every push / clear
uint64_t g_last_sig = ~0ull; // signature of the last successful render
int g_last_w = 0, g_last_h = 0;

uint64_t now() {
    if (g_clock) return g_clock();
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

constexpr int kMaxToasts = 4;
constexpr uint64_t kFadeMs = 300, kInMs = 120;

uint32_t kind_color(Kind k) {
    switch (k) {
        case Kind::Good: return 0x6CE68A;
        case Kind::Warn: return 0xFFD25A;
        case Kind::Error: return 0xFF6B6B;
        default: return 0xFFFFFF;
    }
}

int glyph_index(unsigned char c) { return (c >= 32 && c <= 126) ? c - 32 : '?' - 32; }

void fill_rect(Layer& L, int x, int y, int w, int h, uint32_t argb) {
    for (int j = std::max(0, y); j < std::min(L.h, y + h); j++)
        for (int i = std::max(0, x); i < std::min(L.w, x + w); i++) L.px[(size_t)j * L.w + i] = argb;
}

void draw_text(Layer& L, int x, int y, const std::string& s, int scale, uint32_t argb) {
    for (unsigned char c : s) {
        const uint8_t* g = kFont[glyph_index(c)];
        for (int col = 0; col < 5; col++)
            for (int row = 0; row < 7; row++)
                if (g[col] & (1 << row)) fill_rect(L, x + col * scale, y + row * scale, scale, scale, argb);
        x += 6 * scale;
    }
}

uint32_t with_alpha(uint32_t rgb, unsigned a) { return (a << 24) | (rgb & 0xFFFFFF); }

/* The box background + text of one item. alpha 0..255 scales both. */
void draw_item(Layer& L, int x, int y, const std::string& s, Kind k, int scale, unsigned alpha) {
    const int pad = 2 * scale;
    int w = text_width(s, scale) + pad * 2, h = text_height(scale) + pad * 2;
    fill_rect(L, x, y, w, h, with_alpha(0x101018, alpha * 200 / 255));
    fill_rect(L, x, y, scale, h, with_alpha(kind_color(k), alpha));   // accent bar
    draw_text(L, x + pad + scale, y + pad, s, scale, with_alpha(kind_color(k), alpha));
}

}  // namespace

int text_width(const std::string& s, int scale) { return s.empty() ? 0 : (int)s.size() * 6 * scale - scale; }
int text_height(int scale) { return 7 * scale; }
int scale_for_height(int h) { return std::max(1, h / 240); }

void set_clock_for_test(uint64_t (*fn)()) { std::lock_guard<std::mutex> lk(g_mu); g_clock = fn; }

void push(const std::string& text, Kind kind, double seconds) {
    if (text.empty()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    uint64_t t = now();
    if (seconds < 0.2) seconds = 0.2;
    // The same text as the newest toast refreshes it instead of stacking duplicates.
    if (!g_toasts.empty() && g_toasts.back().text == text) { g_toasts.back().kind = kind; g_toasts.back().until = t + (uint64_t)(seconds * 1000); }
    else g_toasts.push_back({text.substr(0, 120), kind, t, t + (uint64_t)(seconds * 1000)});
    while (g_toasts.size() > 16) g_toasts.pop_front();
    g_gen++;
}

void set_corner(const std::string& text, Kind kind) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_corner == text && g_corner_kind == kind) return;
    g_corner = text.substr(0, 60);
    g_corner_kind = kind;
    g_gen++;
}

void clear() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_toasts.clear();
    g_corner.clear();
    g_gen++;
}

std::string latest_text() {
    std::lock_guard<std::mutex> lk(g_mu);
    uint64_t t = now();
    for (auto it = g_toasts.rbegin(); it != g_toasts.rend(); ++it) if (it->until > t) return it->text;
    return std::string();
}

bool render(Layer* out, int w, int h, bool* changed) {
    if (changed) *changed = false;
    if (w <= 0 || h <= 0 || !out) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    uint64_t t = now();
    while (!g_toasts.empty() && g_toasts.front().until <= t) { g_toasts.pop_front(); g_gen++; }
    for (auto it = g_toasts.begin(); it != g_toasts.end();) { if (it->until <= t) it = g_toasts.erase(it); else ++it; }
    if (g_toasts.empty() && g_corner.empty()) { g_last_sig = ~0ull; return false; }

    const int scale = scale_for_height(h);
    const int margin = 3 * scale, step = text_height(scale) + 4 * scale + 2 * scale;
    // Visible = the newest kMaxToasts; each has an alpha from its fade in/out, quantized to 1/8 for the change signature.
    std::vector<std::pair<const Toast*, unsigned>> vis;
    size_t first = g_toasts.size() > (size_t)kMaxToasts ? g_toasts.size() - kMaxToasts : 0;
    uint64_t sig = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { sig = (sig ^ v) * 1099511628211ull; };
    for (size_t i = first; i < g_toasts.size(); i++) {
        const Toast& to = g_toasts[i];
        double a = 1.0;
        if (t - to.born < kInMs) a = std::min(a, (double)(t - to.born) / kInMs);
        if (to.until - t < kFadeMs) a = std::min(a, (double)(to.until - t) / kFadeMs);
        unsigned q = (unsigned)(a * 8.0 + 0.5);
        vis.push_back({&to, q * 255 / 8});
        mix(i); mix(q);
    }
    mix(g_gen); mix((uint64_t)w << 20 | (uint64_t)h);
    bool same_size = out->w == w && out->h == h && (int)out->px.size() == w * h;
    if (same_size && sig == g_last_sig && g_last_w == w && g_last_h == h) return true;   // pixels unchanged

    out->w = w; out->h = h;
    out->px.assign((size_t)w * h, 0u);
    int y = h - margin;
    for (size_t i = vis.size(); i-- > 0;) {
        y -= step;
        draw_item(*out, margin, y, vis[i].first->text, vis[i].first->kind, scale, vis[i].second);
    }
    if (!g_corner.empty()) {
        int bw = text_width(g_corner, scale) + 4 * scale + scale;
        draw_item(*out, std::max(0, w - margin - bw), margin, g_corner, g_corner_kind, scale, 255);
    }
    g_last_sig = sig; g_last_w = w; g_last_h = h;
    if (changed) *changed = true;
    return true;
}

void blend(uint32_t* dst, int dw, int dh, int stride, const Layer& L) {
    if (!dst || L.px.empty()) return;
    int w = std::min(dw, L.w), h = std::min(dh, L.h);
    for (int y = 0; y < h; y++) {
        uint32_t* d = dst + (size_t)y * stride;
        const uint32_t* s = L.px.data() + (size_t)y * L.w;
        for (int x = 0; x < w; x++) {
            uint32_t sp = s[x];
            unsigned a = sp >> 24;
            if (!a) continue;
            uint32_t dp = d[x];
            unsigned r = (((sp >> 16) & 255) * a + ((dp >> 16) & 255) * (255 - a)) / 255;
            unsigned g = (((sp >> 8) & 255) * a + ((dp >> 8) & 255) * (255 - a)) / 255;
            unsigned b = ((sp & 255) * a + (dp & 255) * (255 - a)) / 255;
            d[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

}  // namespace osd
