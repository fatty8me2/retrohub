#pragma once
/* RetroHub themes: pure palette data + colour math (no ImGui, no SDL), so the console test can check every preset.
 * Colours are packed like ImGui's IM_COL32: r | g<<8 | b<<16 | a<<24.
 * ui_kit.cpp copies the active Palette into the ui::BG / ui::TEXT / ... globals; theme_ui.cpp owns the choice and the picker. */
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace hubtheme {

constexpr uint32_t rgba(int r, int g, int b, int a = 255) { return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24); }
inline int cr(uint32_t c) { return (int)(c & 255); }
inline int cg(uint32_t c) { return (int)((c >> 8) & 255); }
inline int cb(uint32_t c) { return (int)((c >> 16) & 255); }
inline int ca(uint32_t c) { return (int)(c >> 24); }

enum Slot {
    S_BG, S_SIDEBAR, S_PANEL, S_PANEL2, S_PANEL3,
    S_FRAME, S_FRAME_HOV, S_FRAME_ACT,      // text inputs / sliders
    S_HAIRLINE,                             // 1px borders (carries its own alpha)
    S_TEXT, S_TEXT2, S_TEXT3,
    S_ACCENT, S_GOOD, S_BAD, S_WARN,
    S_HILITE,       // colour of subtle hover overlays and "brighten on hover" (white on dark themes, black on light ones)
    S_GHOST,        // label of flat (non-primary) buttons
    S_HERO_BASE,    // dark base the Home hero card blends its console colours into (the hero always keeps white text)
    S_SCRIM,        // the hero's left/bottom fade
    S_COUNT
};

struct Palette {
    uint32_t c[S_COUNT];
};

enum Preset { P_MIDNIGHT, P_OLED, P_LIGHT, P_ATOMIC, P_GAMECUBE, P_FAMICOM, P_COUNT };

inline const char* preset_id(int p) {
    static const char* n[] = {"midnight", "oled", "light", "atomic", "gamecube", "famicom"};
    return p >= 0 && p < P_COUNT ? n[p] : n[0];
}
inline const char* preset_name(int p) {
    static const char* n[] = {"Midnight", "OLED Black", "Light", "Atomic Purple", "GameCube Indigo", "Famicom"};
    return p >= 0 && p < P_COUNT ? n[p] : n[0];
}

inline Palette preset(int p) {
    Palette q;
    auto set = [&](Slot s, uint32_t v) { q.c[s] = v; };
    switch (p) {
    default:
    case P_MIDNIGHT:   // today's colours, exactly
        set(S_BG, rgba(16, 17, 22)); set(S_SIDEBAR, rgba(12, 13, 17)); set(S_PANEL, rgba(26, 28, 36)); set(S_PANEL2, rgba(37, 39, 51));
        set(S_PANEL3, rgba(50, 53, 69)); set(S_FRAME, rgba(30, 32, 42)); set(S_FRAME_HOV, rgba(40, 43, 56)); set(S_FRAME_ACT, rgba(46, 49, 64));
        set(S_HAIRLINE, rgba(255, 255, 255, 20)); set(S_TEXT, rgba(238, 240, 246)); set(S_TEXT2, rgba(152, 157, 175)); set(S_TEXT3, rgba(104, 109, 127));
        set(S_ACCENT, rgba(108, 122, 255)); set(S_GOOD, rgba(56, 200, 120)); set(S_BAD, rgba(232, 95, 85)); set(S_WARN, rgba(240, 176, 64));
        set(S_HILITE, rgba(255, 255, 255)); set(S_GHOST, rgba(214, 217, 228)); set(S_HERO_BASE, rgba(16, 17, 22)); set(S_SCRIM, rgba(14, 15, 20));
        break;
    case P_OLED:
        set(S_BG, rgba(0, 0, 0)); set(S_SIDEBAR, rgba(0, 0, 0)); set(S_PANEL, rgba(14, 14, 17)); set(S_PANEL2, rgba(26, 26, 31));
        set(S_PANEL3, rgba(40, 40, 48)); set(S_FRAME, rgba(18, 18, 22)); set(S_FRAME_HOV, rgba(27, 27, 33)); set(S_FRAME_ACT, rgba(34, 34, 42));
        set(S_HAIRLINE, rgba(255, 255, 255, 22)); set(S_TEXT, rgba(240, 241, 246)); set(S_TEXT2, rgba(160, 163, 178)); set(S_TEXT3, rgba(112, 115, 130));
        set(S_ACCENT, rgba(108, 122, 255)); set(S_GOOD, rgba(56, 200, 120)); set(S_BAD, rgba(232, 95, 85)); set(S_WARN, rgba(240, 176, 64));
        set(S_HILITE, rgba(255, 255, 255)); set(S_GHOST, rgba(214, 217, 228)); set(S_HERO_BASE, rgba(0, 0, 0)); set(S_SCRIM, rgba(0, 0, 0));
        break;
    case P_LIGHT:
        set(S_BG, rgba(238, 240, 246)); set(S_SIDEBAR, rgba(226, 229, 238)); set(S_PANEL, rgba(255, 255, 255)); set(S_PANEL2, rgba(232, 235, 244));
        set(S_PANEL3, rgba(214, 219, 233)); set(S_FRAME, rgba(247, 248, 252)); set(S_FRAME_HOV, rgba(238, 240, 248)); set(S_FRAME_ACT, rgba(228, 232, 244));
        set(S_HAIRLINE, rgba(0, 0, 0, 28)); set(S_TEXT, rgba(20, 22, 32)); set(S_TEXT2, rgba(78, 84, 104)); set(S_TEXT3, rgba(108, 114, 134));
        set(S_ACCENT, rgba(62, 76, 214)); set(S_GOOD, rgba(20, 128, 72)); set(S_BAD, rgba(196, 52, 44)); set(S_WARN, rgba(164, 100, 0));
        set(S_HILITE, rgba(0, 0, 0)); set(S_GHOST, rgba(52, 57, 76)); set(S_HERO_BASE, rgba(16, 17, 22)); set(S_SCRIM, rgba(14, 15, 20));
        break;
    case P_ATOMIC:   // translucent N64 purple
        set(S_BG, rgba(22, 10, 38)); set(S_SIDEBAR, rgba(15, 6, 28)); set(S_PANEL, rgba(36, 19, 60)); set(S_PANEL2, rgba(52, 29, 84));
        set(S_PANEL3, rgba(70, 41, 110)); set(S_FRAME, rgba(42, 23, 70)); set(S_FRAME_HOV, rgba(54, 31, 88)); set(S_FRAME_ACT, rgba(64, 37, 104));
        set(S_HAIRLINE, rgba(235, 215, 255, 26)); set(S_TEXT, rgba(246, 240, 255)); set(S_TEXT2, rgba(190, 172, 222)); set(S_TEXT3, rgba(144, 124, 178));
        set(S_ACCENT, rgba(168, 86, 238)); set(S_GOOD, rgba(70, 214, 140)); set(S_BAD, rgba(244, 100, 112)); set(S_WARN, rgba(244, 184, 70));
        set(S_HILITE, rgba(255, 255, 255)); set(S_GHOST, rgba(220, 206, 242)); set(S_HERO_BASE, rgba(22, 10, 38)); set(S_SCRIM, rgba(17, 8, 30));
        break;
    case P_GAMECUBE:   // indigo cube
        set(S_BG, rgba(18, 16, 42)); set(S_SIDEBAR, rgba(12, 11, 32)); set(S_PANEL, rgba(29, 26, 64)); set(S_PANEL2, rgba(42, 38, 92));
        set(S_PANEL3, rgba(57, 52, 120)); set(S_FRAME, rgba(33, 30, 72)); set(S_FRAME_HOV, rgba(43, 39, 94)); set(S_FRAME_ACT, rgba(52, 47, 110));
        set(S_HAIRLINE, rgba(225, 222, 255, 24)); set(S_TEXT, rgba(240, 239, 253)); set(S_TEXT2, rgba(170, 168, 214)); set(S_TEXT3, rgba(128, 126, 176));
        set(S_ACCENT, rgba(104, 90, 232)); set(S_GOOD, rgba(60, 206, 128)); set(S_BAD, rgba(238, 98, 94)); set(S_WARN, rgba(242, 180, 66));
        set(S_HILITE, rgba(255, 255, 255)); set(S_GHOST, rgba(216, 214, 240)); set(S_HERO_BASE, rgba(18, 16, 42)); set(S_SCRIM, rgba(14, 12, 34));
        break;
    case P_FAMICOM:   // cream body, red accent
        set(S_BG, rgba(244, 235, 214)); set(S_SIDEBAR, rgba(232, 220, 194)); set(S_PANEL, rgba(253, 247, 232)); set(S_PANEL2, rgba(238, 226, 200));
        set(S_PANEL3, rgba(222, 207, 175)); set(S_FRAME, rgba(250, 243, 226)); set(S_FRAME_HOV, rgba(242, 232, 208)); set(S_FRAME_ACT, rgba(234, 222, 194));
        set(S_HAIRLINE, rgba(60, 30, 20, 34)); set(S_TEXT, rgba(46, 26, 24)); set(S_TEXT2, rgba(108, 76, 68)); set(S_TEXT3, rgba(132, 100, 88));
        set(S_ACCENT, rgba(190, 30, 40)); set(S_GOOD, rgba(26, 128, 74)); set(S_BAD, rgba(176, 28, 38)); set(S_WARN, rgba(160, 98, 0));
        set(S_HILITE, rgba(0, 0, 0)); set(S_GHOST, rgba(86, 58, 54)); set(S_HERO_BASE, rgba(30, 14, 14)); set(S_SCRIM, rgba(24, 11, 11));
        break;
    }
    return q;
}

/* ---- colour math ---- */
inline double lin(int v) { double x = v / 255.0; return x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4); }
inline double luminance(uint32_t c) { return 0.2126 * lin(cr(c)) + 0.7152 * lin(cg(c)) + 0.0722 * lin(cb(c)); }
/* WCAG contrast ratio (1..21); alpha is ignored. */
inline double contrast(uint32_t a, uint32_t b) {
    double la = luminance(a), lb = luminance(b);
    if (la < lb) std::swap(la, lb);
    return (la + 0.05) / (lb + 0.05);
}
inline uint32_t mix_rgb(uint32_t a, uint32_t b, double t) {
    auto m = [&](int x, int y) { return (int)(x + (y - x) * t + 0.5); };
    return rgba(m(cr(a), cr(b)), m(cg(a), cg(b)), m(cb(a), cb(b)), ca(a));
}

/* Minimum text contrast every preset must reach (see test_theme.cpp). */
constexpr double kMinText = 7.0, kMinText2 = 4.5, kMinText3 = 3.0, kMinAccent = 3.0;

/* ---- "Accent from cover" ---- */

/* Average of the saturated, mid-bright pixels of an RGBA8 image (greys, near-black and near-white are ignored, each pixel is
 * weighted by its saturation). `step` samples every step-th pixel. Returns 0 when nothing colourful was found. */
inline uint32_t average_saturated(const unsigned char* rgba8, int w, int h, int step = 3) {
    if (!rgba8 || w <= 0 || h <= 0) return 0;
    if (step < 1) step = 1;
    double sr = 0, sg = 0, sb = 0, sw = 0;
    for (int y = 0; y < h; y += step)
        for (int x = 0; x < w; x += step) {
            const unsigned char* p = rgba8 + ((size_t)y * w + x) * 4;
            if (p[3] < 128) continue;
            int mx = std::max(p[0], std::max(p[1], p[2])), mn = std::min(p[0], std::min(p[1], p[2]));
            if (mx < 50 || mn > 215) continue;
            double sat = (mx - mn) / (double)mx;
            if (sat < 0.25) continue;
            double wgt = sat * sat;
            sr += p[0] * wgt; sg += p[1] * wgt; sb += p[2] * wgt; sw += wgt;
        }
    if (sw <= 0) return 0;
    return rgba((int)(sr / sw + 0.5), (int)(sg / sw + 0.5), (int)(sb / sw + 0.5));
}

/* Adjusts `want` so it can be used as THE accent on `bg`: >= kMinAccent against the background (as a button/ring) and
 * >= kMinAccent against white (the labels on primary buttons are white). Hue is kept; only brightness moves. */
inline uint32_t readable_accent(uint32_t want, uint32_t bg) {
    const uint32_t white = rgba(255, 255, 255), black = rgba(0, 0, 0);
    auto ok = [&](uint32_t c) { return contrast(c, bg) >= kMinAccent && contrast(c, white) >= kMinAccent; };
    if (ok(want)) return want | 0xFF000000u;
    /* walk from `want` toward black and toward white and take the nearest step that satisfies both limits */
    for (int i = 1; i <= 40; ++i) {
        double t = i / 40.0;
        uint32_t d = mix_rgb(want, black, t), l = mix_rgb(want, white, t);
        bool bgDark = luminance(bg) < 0.4;
        uint32_t first = bgDark ? l : d, second = bgDark ? d : l;   // on a dark theme lightening is the natural first move
        if (ok(first)) return first | 0xFF000000u;
        if (ok(second)) return second | 0xFF000000u;
    }
    return want | 0xFF000000u;
}

/* Theme choice as stored in the synced prefs `theme` field: "<preset>" or "<preset>+cover". Unknown text means Midnight. */
struct Choice {
    int preset = P_MIDNIGHT;
    bool cover = false;
};
inline Choice parse_choice(const std::string& s) {
    Choice c;
    std::string id = s;
    size_t plus = s.find('+');
    if (plus != std::string::npos) { id = s.substr(0, plus); c.cover = s.compare(plus, std::string::npos, "+cover") == 0; }
    for (int i = 0; i < P_COUNT; ++i) if (id == preset_id(i)) c.preset = i;
    return c;
}
inline std::string format_choice(const Choice& c) { return std::string(preset_id(c.preset)) + (c.cover ? "+cover" : ""); }

}  // namespace hubtheme
