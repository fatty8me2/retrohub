#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui_kit.h"
#include "imgui_internal.h"   // FindRenderedTextEnd

#include <SDL.h>
#include <windows.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

#include "logic.h"
#include "ui_sound.h"

namespace ui {

float S = 1.0f;
bool reduce_motion = false;
Fonts F;

static const float kPi = 3.14159265f;

#define UI_PAL(n, slot) ImU32 n = hubtheme::preset(hubtheme::P_MIDNIGHT).c[hubtheme::slot];
UI_PAL(BG, S_BG) UI_PAL(SIDEBAR, S_SIDEBAR) UI_PAL(PANEL, S_PANEL) UI_PAL(PANEL2, S_PANEL2) UI_PAL(PANEL3, S_PANEL3)
UI_PAL(HAIRLINE, S_HAIRLINE) UI_PAL(TEXT, S_TEXT) UI_PAL(TEXT2, S_TEXT2) UI_PAL(TEXT3, S_TEXT3) UI_PAL(ACCENT, S_ACCENT)
UI_PAL(GOOD, S_GOOD) UI_PAL(BAD, S_BAD) UI_PAL(WARN, S_WARN) UI_PAL(FRAME, S_FRAME) UI_PAL(FRAME_HOV, S_FRAME_HOV)
UI_PAL(FRAME_ACT, S_FRAME_ACT) UI_PAL(HILITE, S_HILITE) UI_PAL(GHOST, S_GHOST) UI_PAL(HERO_BASE, S_HERO_BASE) UI_PAL(SCRIM, S_SCRIM)
#undef UI_PAL

static void apply_style_colors();

void apply_palette(const hubtheme::Palette& p) {
    using namespace hubtheme;
    BG = p.c[S_BG]; SIDEBAR = p.c[S_SIDEBAR]; PANEL = p.c[S_PANEL]; PANEL2 = p.c[S_PANEL2]; PANEL3 = p.c[S_PANEL3];
    HAIRLINE = p.c[S_HAIRLINE]; TEXT = p.c[S_TEXT]; TEXT2 = p.c[S_TEXT2]; TEXT3 = p.c[S_TEXT3]; ACCENT = p.c[S_ACCENT];
    GOOD = p.c[S_GOOD]; BAD = p.c[S_BAD]; WARN = p.c[S_WARN]; FRAME = p.c[S_FRAME]; FRAME_HOV = p.c[S_FRAME_HOV];
    FRAME_ACT = p.c[S_FRAME_ACT]; HILITE = p.c[S_HILITE]; GHOST = p.c[S_GHOST]; HERO_BASE = p.c[S_HERO_BASE]; SCRIM = p.c[S_SCRIM];
    if (ImGui::GetCurrentContext()) apply_style_colors();
}
ImU32 veil(float a) { return (HILITE & 0x00FFFFFFu) | ((ImU32)(std::max(0.f, std::min(1.f, a)) * 255.f + 0.5f) << 24); }

ImU32 mix(ImU32 a, ImU32 b, float t) {
    ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}
ImU32 with_alpha(ImU32 c, float a) {
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    v.w = std::max(0.f, std::min(1.f, a));
    return ImGui::ColorConvertFloat4ToU32(v);
}
ImU32 scale_alpha(ImU32 c, float k) {
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    v.w = std::max(0.f, std::min(1.f, v.w * k));
    return ImGui::ColorConvertFloat4ToU32(v);
}
float anim(float& v, float target, float rate) {
    v = reduce_motion ? target : hubui::approach(v, target, ImGui::GetIO().DeltaTime, rate);
    return v;
}

/* ============================ fonts / theme ============================ */

void setup_fonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    const char* reg = "C:\\Windows\\Fonts\\segoeui.ttf";
    const char* bold = "C:\\Windows\\Fonts\\segoeuib.ttf";
    bool have_reg = GetFileAttributesA(reg) != INVALID_FILE_ATTRIBUTES;
    bool have_bold = GetFileAttributesA(bold) != INVALID_FILE_ATTRIBUTES;
    auto load = [&](bool b, float px) -> ImFont* {
        const char* p = b ? (have_bold ? bold : reg) : reg;
        if (have_reg || (b && have_bold)) return io.Fonts->AddFontFromFileTTF(p, px * scale);
        return nullptr;
    };
    F.body = load(false, 19);
    if (!F.body) {   // fallback: built-in font, enlarged
        ImFontConfig cfg;
        cfg.SizePixels = 19 * scale;
        F.body = io.Fonts->AddFontDefault(&cfg);
        F.cap = F.bold = F.h2 = F.title = F.hero = F.big = F.body;
        return;
    }
    F.cap = load(false, 15);
    F.bold = load(true, 19);
    F.h2 = load(true, 24);
    F.title = load(true, 34);
    F.hero = load(true, 48);
    F.big = load(true, 84);
    if (!F.cap) F.cap = F.body;
    if (!F.bold) F.bold = F.body;
    if (!F.h2) F.h2 = F.body;
    if (!F.title) F.title = F.body;
    if (!F.hero) F.hero = F.title;
    if (!F.big) F.big = F.body;
}

void setup_theme(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 12;
    s.ChildRounding = 12;
    s.FrameRounding = 10;
    s.PopupRounding = 14;
    s.GrabRounding = 8;
    s.ScrollbarRounding = 8;
    s.WindowPadding = ImVec2(16, 16);
    s.FramePadding = ImVec2(14, 10);
    s.ItemSpacing = ImVec2(16, 16);
    s.ItemInnerSpacing = ImVec2(8, 8);
    s.ScrollbarSize = 10;
    s.FrameBorderSize = 0;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.PopupBorderSize = 1;
    s.ScaleAllSizes(scale);

    apply_style_colors();
}

static void apply_style_colors() {
    ImVec4* c = ImGui::GetStyle().Colors;
    auto cv = [](ImU32 u) { return ImGui::ColorConvertU32ToFloat4(u); };
    c[ImGuiCol_WindowBg] = cv(BG);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = cv(PANEL);
    c[ImGuiCol_Border] = cv(veil(26/255.f));
    c[ImGuiCol_Text] = cv(TEXT);
    c[ImGuiCol_TextDisabled] = cv(TEXT2);
    c[ImGuiCol_Button] = cv(PANEL2);
    c[ImGuiCol_ButtonHovered] = cv(PANEL3);
    c[ImGuiCol_ButtonActive] = cv(mix(PANEL3, ACCENT, 0.4f));
    // checkbox / input frames: a palette-relative lift toward TEXT so an unchecked box reads on every panel and in every theme (the raw FRAME tokens sit almost on PANEL)
    c[ImGuiCol_FrameBg] = cv(mix(FRAME, TEXT, 0.16f));
    c[ImGuiCol_FrameBgHovered] = cv(mix(FRAME_HOV, TEXT, 0.22f));
    c[ImGuiCol_FrameBgActive] = cv(mix(FRAME_ACT, TEXT, 0.26f));
    c[ImGuiCol_SliderGrab] = cv(ACCENT);
    c[ImGuiCol_SliderGrabActive] = cv(mix(ACCENT, IM_COL32_WHITE, 0.25f));
    c[ImGuiCol_CheckMark] = cv(ACCENT);
    c[ImGuiCol_Header] = cv(with_alpha(ACCENT, 0.30f));
    c[ImGuiCol_HeaderHovered] = cv(with_alpha(ACCENT, 0.50f));
    c[ImGuiCol_HeaderActive] = cv(with_alpha(ACCENT, 0.70f));
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = cv(veil(40/255.f));
    c[ImGuiCol_ScrollbarGrabHovered] = cv(veil(80/255.f));
    c[ImGuiCol_ScrollbarGrabActive] = cv(veil(110/255.f));
    c[ImGuiCol_Separator] = cv(HAIRLINE);
    c[ImGuiCol_TableHeaderBg] = cv(PANEL2);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = cv(veil(6/255.f));
    c[ImGuiCol_NavCursor] = cv(mix(TEXT, ACCENT, 0.25f));
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.02f, 0.02f, 0.04f, 0.72f);
}

/* ============================ primitives ============================ */

void gradient_round_rect(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 c0, ImU32 c1, float rounding) {
    int start = dl->VtxBuffer.Size;
    dl->AddRectFilled(p0, p1, IM_COL32_WHITE, rounding);
    float w = std::max(1.f, p1.x - p0.x), h = std::max(1.f, p1.y - p0.y);
    for (int i = start; i < dl->VtxBuffer.Size; ++i) {
        ImDrawVert& v = dl->VtxBuffer[i];
        float t = ((v.pos.x - p0.x) / w + (v.pos.y - p0.y) / h) * 0.5f;
        t = std::min(1.f, std::max(0.f, t));
        ImU32 a = v.col >> 24;
        v.col = (mix(c0, c1, t) & 0x00FFFFFF) | (a << 24);
    }
}

void vfade_round_rect(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 col, float rounding, ImDrawFlags flags, float a_top, float a_bot) {
    int start = dl->VtxBuffer.Size;
    dl->AddRectFilled(p0, p1, col | 0xFF000000, rounding, flags);
    float h = std::max(1.f, p1.y - p0.y);
    for (int i = start; i < dl->VtxBuffer.Size; ++i) {
        ImDrawVert& v = dl->VtxBuffer[i];
        float t = std::min(1.f, std::max(0.f, (v.pos.y - p0.y) / h));
        float k = a_top + (a_bot - a_top) * t;
        ImU32 a = (ImU32)((v.col >> 24) * std::max(0.f, std::min(1.f, k)));
        v.col = (v.col & 0x00FFFFFF) | (a << 24);
    }
}

void hfade_round_rect(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 col, float rounding, ImDrawFlags flags, float a_left, float a_right) {
    int start = dl->VtxBuffer.Size;
    dl->AddRectFilled(p0, p1, col | 0xFF000000, rounding, flags);
    float w = std::max(1.f, p1.x - p0.x);
    for (int i = start; i < dl->VtxBuffer.Size; ++i) {
        ImDrawVert& v = dl->VtxBuffer[i];
        float t = std::min(1.f, std::max(0.f, (v.pos.x - p0.x) / w));
        float k = a_left + (a_right - a_left) * t;
        ImU32 a = (ImU32)((v.col >> 24) * std::max(0.f, std::min(1.f, k)));
        v.col = (v.col & 0x00FFFFFF) | (a << 24);
    }
}

void shadow(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float rounding, float strength, float dy) {
    for (int k = 6; k >= 1; --k) {
        float e = k * sp(0.5f);
        float a = 0.085f * strength * (1.f - k / 7.f);
        dl->AddRectFilled(ImVec2(p0.x - e, p0.y - e + sp(0.75f) + dy), ImVec2(p1.x + e, p1.y + e + sp(0.75f) + dy),
                          IM_COL32(0, 0, 0, (int)(255 * std::min(1.f, a))), rounding + e);
    }
}

void glow(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float rounding, ImU32 col, float amount) {
    if (amount <= 0.01f) return;
    for (int k = 5; k >= 1; --k) {
        float e = k * sp(0.5f);
        dl->AddRectFilled(ImVec2(p0.x - e, p0.y - e), ImVec2(p1.x + e, p1.y + e), with_alpha(col, amount * 0.11f * (1.f - k / 6.f) * 2.f),
                          rounding + e);
    }
}

void focus_ring(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float rounding) {
    float t = 0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 5.f);
    ImU32 c = mix(TEXT, ACCENT, 0.25f);
    float e = sp(0.5f);
    dl->AddRect(ImVec2(p0.x - e * 2.2f, p0.y - e * 2.2f), ImVec2(p1.x + e * 2.2f, p1.y + e * 2.2f), with_alpha(ACCENT, 0.18f + 0.18f * t),
                rounding + e * 2.2f, 0, sp(0.5f));
    dl->AddRect(ImVec2(p0.x - e, p0.y - e), ImVec2(p1.x + e, p1.y + e), c, rounding + e, 0, sp(0.4f));
}

void shimmer(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float rounding, ImU32 base) {
    dl->AddRectFilled(p0, p1, base, rounding);
    float w = p1.x - p0.x;
    float inset = std::min(rounding, w * 0.5f);
    float phase = std::fmod((float)ImGui::GetTime() * 0.55f, 1.6f) - 0.3f;   // band centre in 0..1 widths
    float cx = p0.x + w * phase, bw = std::max(sp(4), w * 0.28f);
    dl->PushClipRect(ImVec2(p0.x + inset, p0.y), ImVec2(p1.x - inset, p1.y), true);
    ImU32 hi = veil(16/255.f), lo = veil(0.f);
    dl->AddRectFilledMultiColor(ImVec2(cx - bw, p0.y), ImVec2(cx, p1.y), lo, hi, hi, lo);
    dl->AddRectFilledMultiColor(ImVec2(cx, p0.y), ImVec2(cx + bw, p1.y), hi, lo, lo, hi);
    dl->PopClipRect();
}

void text_shadowed(ImDrawList* dl, ImFont* f, float size, ImVec2 p, ImU32 col, const char* text) {
    dl->AddText(f, size, p + ImVec2(0, std::max(1.f, S)), IM_COL32(0, 0, 0, 150), text);
    dl->AddText(f, size, p, col, text);
}

/* ---- text fitting ---- */

static float text_w(ImFont* f, float size, const std::string& s) { return f->CalcTextSizeA(size, FLT_MAX, 0.f, s.c_str()).x; }
static size_t utf8_prev(const std::string& s, size_t i) {
    if (i == 0) return 0;
    --i;
    while (i > 0 && (s[i] & 0xC0) == 0x80) --i;
    return i;
}

std::string ellipsize(ImFont* f, float size, const std::string& text, float max_w) {
    if (text_w(f, size, text) <= max_w) return text;
    std::string t = text;
    while (!t.empty() && text_w(f, size, t + "...") > max_w) t.erase(utf8_prev(t, t.size()));
    while (!t.empty() && t.back() == ' ') t.pop_back();
    return t + "...";
}

std::vector<std::string> wrap_lines(ImFont* f, float size, const std::string& text, float max_w, int max_lines) {
    std::vector<std::string> lines;
    std::string cur, word;
    auto flush_word = [&]() {
        if (word.empty()) return;
        std::string cand = cur.empty() ? word : cur + " " + word;
        if (text_w(f, size, cand) <= max_w) cur = cand;
        else {
            if (!cur.empty()) lines.push_back(cur);
            cur = word;
            while (text_w(f, size, cur) > max_w && cur.size() > 1) {   // a single over-long word: hard break
                size_t cut = cur.size();
                while (cut > 1 && text_w(f, size, cur.substr(0, cut)) > max_w) cut = utf8_prev(cur, cut);
                if (cut == 0) break;
                lines.push_back(cur.substr(0, cut));
                cur = cur.substr(cut);
            }
        }
        word.clear();
    };
    for (char ch : text) {
        if (ch == ' ') flush_word();
        else word += ch;
    }
    flush_word();
    if (!cur.empty() || lines.empty()) lines.push_back(cur);
    if ((int)lines.size() > max_lines && max_lines > 0) {
        std::string rest = lines[max_lines - 1];
        for (size_t i = max_lines; i < lines.size(); ++i) rest += " " + lines[i];
        lines.resize(max_lines);
        lines.back() = ellipsize(f, size, rest, max_w);
    }
    return lines;
}

/* ============================ icons ============================ */

void icon(ImDrawList* dl, Icon ic, ImVec2 c, float s, ImU32 col) {
    float t = std::max(1.5f, s * 0.085f);
    auto P = [&](float x, float y) { return ImVec2(c.x + x * s, c.y + y * s); };
    switch (ic) {
        case Icon::Home: {
            ImVec2 roof[3] = {P(-0.47f, 0.0f), P(0.f, -0.44f), P(0.47f, 0.0f)};
            dl->AddPolyline(roof, 3, col, 0, t);
            ImVec2 body[4] = {P(-0.34f, -0.14f), P(-0.34f, 0.42f), P(0.34f, 0.42f), P(0.34f, -0.14f)};
            dl->AddPolyline(body, 4, col, 0, t);
            ImVec2 door[4] = {P(-0.1f, 0.42f), P(-0.1f, 0.14f), P(0.1f, 0.14f), P(0.1f, 0.42f)};
            dl->AddPolyline(door, 4, col, 0, t);
            break;
        }
        case Icon::Globe: {
            float r = 0.44f * s;
            dl->AddCircle(c, r, col, 32, t);
            ImVec2 mer[25];
            for (int i = 0; i < 25; ++i) {
                float a = i * 2 * kPi / 24;
                mer[i] = ImVec2(c.x + std::cos(a) * 0.2f * s, c.y + std::sin(a) * r);
            }
            dl->AddPolyline(mer, 25, col, 0, t * 0.85f);
            dl->AddLine(P(-0.44f, 0), P(0.44f, 0), col, t * 0.85f);
            for (float y : {-0.22f, 0.22f}) {
                float hw = std::sqrt(std::max(0.f, 0.44f * 0.44f - y * y));
                dl->AddLine(P(-hw, y), P(hw, y), col, t * 0.85f);
            }
            break;
        }
        case Icon::Cloud: {
            struct Circ { float x, y, r; };
            const Circ cs[3] = {{-0.2f, 0.06f, 0.19f}, {0.02f, -0.12f, 0.27f}, {0.26f, 0.08f, 0.17f}};
            ImVec2 pts[64];
            const float ox = 0.f, oy = 0.08f;
            for (int i = 0; i < 64; ++i) {
                float a = i * 2 * kPi / 64, dx = std::cos(a), dy = std::sin(a), best = 0;
                for (auto& q : cs) {   // far intersection of the ray with each circle
                    float fx = ox - q.x, fy = oy - q.y;
                    float b = fx * dx + fy * dy, cc = fx * fx + fy * fy - q.r * q.r, disc = b * b - cc;
                    if (disc >= 0) best = std::max(best, -b + std::sqrt(disc));
                }
                // base slab: x in [-0.38,0.43], y <= 0.25
                float tb = FLT_MAX;
                if (dx > 1e-4f) tb = std::min(tb, (0.43f - ox) / dx); else if (dx < -1e-4f) tb = std::min(tb, (-0.38f - ox) / dx);
                if (dy > 1e-4f) tb = std::min(tb, (0.25f - oy) / dy); else if (dy < -1e-4f) tb = std::min(tb, (-0.1f - oy) / dy);
                best = std::max(best, tb);
                pts[i] = P(ox + dx * best, oy + dy * best);
            }
            dl->AddPolyline(pts, 64, col, ImDrawFlags_Closed, t);
            break;
        }
        case Icon::Gamepad: {
            dl->AddRect(P(-0.47f, -0.25f), P(0.47f, 0.27f), col, 0.22f * s, 0, t);
            dl->AddLine(P(-0.28f, 0.0f), P(-0.08f, 0.0f), col, t);
            dl->AddLine(P(-0.18f, -0.1f), P(-0.18f, 0.1f), col, t);
            dl->AddCircleFilled(P(0.14f, -0.04f), 0.055f * s, col, 12);
            dl->AddCircleFilled(P(0.29f, 0.08f), 0.055f * s, col, 12);
            break;
        }
        case Icon::Gear: {
            ImVec2 pts[32];
            int n = 0;
            for (int k = 0; k < 8; ++k) {
                float a = k * kPi / 4;
                const float da[4] = {-0.22f, -0.13f, 0.13f, 0.22f}, rr[4] = {0.34f, 0.47f, 0.47f, 0.34f};
                for (int j = 0; j < 4; ++j) pts[n++] = ImVec2(c.x + std::cos(a + da[j]) * rr[j] * s, c.y + std::sin(a + da[j]) * rr[j] * s);
            }
            dl->AddPolyline(pts, n, col, ImDrawFlags_Closed, t);
            dl->AddCircle(c, 0.15f * s, col, 20, t);
            break;
        }
        case Icon::Info: {
            dl->AddCircle(c, 0.44f * s, col, 32, t);
            dl->AddCircleFilled(P(0, -0.18f), t * 0.75f, col, 12);
            dl->AddLine(P(0, -0.04f), P(0, 0.22f), col, t);
            break;
        }
        case Icon::Search: {
            dl->AddCircle(P(-0.08f, -0.08f), 0.28f * s, col, 24, t);
            dl->AddLine(P(0.13f, 0.13f), P(0.4f, 0.4f), col, t);
            break;
        }
        case Icon::Refresh: {
            float r = 0.34f * s;
            dl->PathArcTo(c, r, -0.9f, 4.35f, 24);
            dl->PathStroke(col, 0, t);
            ImVec2 e(c.x + std::cos(4.35f) * r, c.y + std::sin(4.35f) * r);
            ImVec2 tri[3] = {e + ImVec2(-0.02f * s, -0.16f * s), e + ImVec2(0.17f * s, 0.02f * s), e + ImVec2(-0.14f * s, 0.12f * s)};
            dl->AddTriangleFilled(tri[0], tri[1], tri[2], col);
            break;
        }
        case Icon::Plus: {
            dl->AddLine(P(-0.34f, 0), P(0.34f, 0), col, t);
            dl->AddLine(P(0, -0.34f), P(0, 0.34f), col, t);
            break;
        }
        case Icon::Star:
        case Icon::StarFilled: {
            ImVec2 pts[10];
            for (int i = 0; i < 10; ++i) {
                float a = -kPi / 2 + i * kPi / 5, rr = ((i & 1) ? 0.2f : 0.48f) * s;
                pts[i] = ImVec2(c.x + std::cos(a) * rr, c.y + 0.03f * s + std::sin(a) * rr);
            }
            if (ic == Icon::StarFilled) dl->AddConvexPolyFilled(pts, 10, col);
            else dl->AddPolyline(pts, 10, col, ImDrawFlags_Closed, t);
            break;
        }
        case Icon::Play: {
            dl->AddTriangleFilled(P(-0.2f, -0.34f), P(-0.2f, 0.34f), P(0.36f, 0.f), col);
            break;
        }
        case Icon::ChevronL: {
            ImVec2 p[3] = {P(0.14f, -0.3f), P(-0.16f, 0.f), P(0.14f, 0.3f)};
            dl->AddPolyline(p, 3, col, 0, t);
            break;
        }
        case Icon::ChevronR: {
            ImVec2 p[3] = {P(-0.14f, -0.3f), P(0.16f, 0.f), P(-0.14f, 0.3f)};
            dl->AddPolyline(p, 3, col, 0, t);
            break;
        }
        case Icon::Dots: {
            for (float x : {-0.26f, 0.f, 0.26f}) dl->AddCircleFilled(P(x, 0), t * 0.9f, col, 10);
            break;
        }
        case Icon::Folder: {
            ImVec2 p[7] = {P(-0.44f, 0.3f), P(-0.44f, -0.28f), P(-0.12f, -0.28f), P(-0.02f, -0.16f), P(0.44f, -0.16f), P(0.44f, 0.3f), P(-0.44f, 0.3f)};
            dl->AddPolyline(p, 7, col, 0, t);
            break;
        }
        case Icon::Close: {
            dl->AddLine(P(-0.28f, -0.28f), P(0.28f, 0.28f), col, t);
            dl->AddLine(P(0.28f, -0.28f), P(-0.28f, 0.28f), col, t);
            break;
        }
        case Icon::Check: {
            ImVec2 p[3] = {P(-0.32f, 0.02f), P(-0.08f, 0.26f), P(0.34f, -0.24f)};
            dl->AddPolyline(p, 3, col, 0, t * 1.2f);
            break;
        }
        case Icon::Bolt: {
            ImVec2 p[6] = {P(0.1f, -0.46f), P(-0.26f, 0.06f), P(-0.02f, 0.06f), P(-0.1f, 0.46f), P(0.26f, -0.1f), P(0.02f, -0.1f)};
            dl->AddConvexPolyFilled(p, 6, col);
            break;
        }
        default: break;
    }
}

/* ============================ widgets ============================ */

static bool nav_focus_visible() { return ImGui::GetIO().NavVisible; }
/* Menu sounds: one call per interactive widget. Focus moving onto a new item = Move, activation = Select. */
static void sfx_item(bool focus, bool pressed) {
    static ImGuiID last = 0;
    if (focus) { ImGuiID id = ImGui::GetItemID(); if (id != last) { last = id; ui_sound::play(ui_sound::Sfx::Move); } }
    if (pressed) ui_sound::play(ui_sound::Sfx::Select);
}

bool button(const char* label, ImVec2 size, bool primary, bool enabled, Icon ic) {
    const char* end = ImGui::FindRenderedTextEnd(label);
    ImVec2 ts = ImGui::CalcTextSize(label, end);
    bool has_text = end > label;
    float isz = sp(2.25f), gap = (ic != Icon::None && has_text) ? sp(1) : 0, pad = sp(2.5f);
    float w = size.x > 0 ? size.x : ts.x + (ic != Icon::None ? isz : 0) + gap + pad * 2;
    if (size.x < 0) w = std::max(1.f, ImGui::GetContentRegionAvail().x + size.x);
    float h = size.y > 0 ? size.y : sp(5.5f);
    ImGui::BeginDisabled(!enabled);
    bool pressed = ImGui::InvisibleButton(label, ImVec2(std::max(1.f, w), std::max(1.f, h)), ImGuiButtonFlags_EnableNav);
    bool hov = ImGui::IsItemHovered();
    bool held = ImGui::IsItemActive();
    bool focus = ImGui::IsItemFocused() && nav_focus_visible(); sfx_item(focus, pressed);
    float* hv = ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID(), 0.f);
    anim(*hv, (hov || focus) ? 1.f : 0.f, 16.f);
    float al = ImGui::GetStyle().Alpha;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    if (held) { p0 += ImVec2(1, 1) * S; p1 -= ImVec2(1, 1) * S; }
    float rd = std::min(sp(1.5f), (p1.y - p0.y) * 0.5f);
    ImU32 bg, fg;
    if (primary) {
        bg = mix(ACCENT, IM_COL32_WHITE, 0.2f * *hv);
        fg = IM_COL32_WHITE;
        glow(dl, p0, p1, rd, ACCENT, *hv * al * 0.8f);
    } else {
        bg = mix(PANEL2, PANEL3, *hv);
        fg = mix(GHOST, HILITE, *hv);
    }
    dl->AddRectFilled(p0, p1, scale_alpha(bg, al), rd);
    if (!primary) dl->AddRect(p0, p1, scale_alpha(HAIRLINE, al), rd, 0, 1.f);
    float cw = ts.x + (ic != Icon::None ? isz : 0) + gap;
    float x = p0.x + ((p1.x - p0.x) - cw) * 0.5f, cy = (p0.y + p1.y) * 0.5f;
    if (ic != Icon::None) {
        icon(dl, ic, ImVec2(x + isz * 0.5f, cy), isz, scale_alpha(fg, al));
        x += isz + gap;
    }
    if (has_text) dl->AddText(ImVec2(x, cy - ts.y * 0.5f), scale_alpha(fg, al), label, end);
    if (focus) focus_ring(dl, p0, p1, rd);
    ImGui::EndDisabled();
    return pressed && enabled;
}

bool icon_button(const char* id, Icon ic, float size, bool active, const char* tooltip, ImU32 active_col) {
    ImGui::PushID(id);
    bool pressed = ImGui::InvisibleButton("##ib", ImVec2(size, size), ImGuiButtonFlags_EnableNav);
    bool hov = ImGui::IsItemHovered();
    bool focus = ImGui::IsItemFocused() && nav_focus_visible(); sfx_item(focus, pressed);
    float* hv = ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID(), 0.f);
    anim(*hv, (hov || focus) ? 1.f : 0.f, 16.f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax(), c = (p0 + p1) * 0.5f;
    ImU32 bg = active ? mix(active_col, IM_COL32_WHITE, 0.15f * *hv) : mix(PANEL2, PANEL3, *hv);
    if (active) glow(dl, p0, p1, size * 0.5f, active_col, *hv * 0.6f);
    dl->AddCircleFilled(c, size * 0.5f, bg, 32);
    if (!active) dl->AddCircle(c, size * 0.5f, HAIRLINE, 32, 1.f);
    icon(dl, ic, c, size * 0.5f, active ? IM_COL32_WHITE : mix(TEXT2, HILITE, *hv));
    if (focus) focus_ring(dl, p0, p1, size * 0.5f);
    if (tooltip && hov) ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    return pressed;
}

bool modal_back() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return false;
    const bool back = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (back) ui_sound::play(ui_sound::Sfx::Back);
    return back;
}

bool chip(const char* label, bool on, ImU32 col) {
    ImVec2 ts = ImGui::CalcTextSize(label);
    ImVec2 sz(ts.x + sp(3.25f), ts.y + sp(1.75f));
    bool pressed = ImGui::InvisibleButton(label, sz, ImGuiButtonFlags_EnableNav);
    bool hov = ImGui::IsItemHovered();
    bool focus = ImGui::IsItemFocused() && nav_focus_visible(); sfx_item(focus, pressed);
    float* hv = ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID(), 0.f);
    anim(*hv, (hov || focus) ? 1.f : 0.f, 16.f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    ImU32 bg = on ? col : mix(PANEL, PANEL3, *hv);
    dl->AddRectFilled(p0, p1, bg, sz.y * 0.5f);
    if (!on) dl->AddRect(p0, p1, HAIRLINE, sz.y * 0.5f, 0, 1.f);
    if (focus) focus_ring(dl, p0, p1, sz.y * 0.5f);
    dl->AddText(p0 + ImVec2(sp(1.625f), sp(0.875f)), on ? IM_COL32_WHITE : mix(TEXT2, TEXT, *hv), label);
    return pressed;
}

int segmented(const char* id, const char* const* labels, int n, int cur) {
    ImGui::PushID(id);
    float pad = sp(2.f), h = sp(5.f), inner = sp(0.5f);
    std::vector<float> ws(n);
    float total = inner * 2;
    for (int i = 0; i < n; ++i) { ws[i] = ImGui::CalcTextSize(labels[i]).x + pad * 2; total += ws[i]; }
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, o + ImVec2(total, h), PANEL, h * 0.5f);
    dl->AddRect(o, o + ImVec2(total, h), HAIRLINE, h * 0.5f, 0, 1.f);
    int result = cur;
    float x = o.x + inner;
    for (int i = 0; i < n; ++i) {
        ImGui::SetCursorScreenPos(ImVec2(x, o.y + inner));
        ImGui::PushID(i);
        bool pr = ImGui::InvisibleButton("seg", ImVec2(ws[i], h - inner * 2), ImGuiButtonFlags_EnableNav);
        bool hov = ImGui::IsItemHovered();
        bool focus = ImGui::IsItemFocused() && nav_focus_visible(); sfx_item(focus, pr);
        float* hv = ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID(), 0.f);
        anim(*hv, (hov || focus) ? 1.f : 0.f, 16.f);
        ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
        float r = (p1.y - p0.y) * 0.5f;
        if (i == cur) dl->AddRectFilled(p0, p1, mix(ACCENT, IM_COL32_WHITE, 0.1f * *hv), r);
        else if (*hv > 0.01f) dl->AddRectFilled(p0, p1, with_alpha(HILITE, 0.07f * *hv), r);
        ImVec2 ts = ImGui::CalcTextSize(labels[i]);
        dl->AddText(F.bold ? F.bold : nullptr, F.bold ? F.bold->FontSize : 0, ImVec2((p0.x + p1.x - ts.x) * 0.5f, (p0.y + p1.y - ts.y) * 0.5f),
                    i == cur ? IM_COL32_WHITE : mix(TEXT2, TEXT, *hv), labels[i]);
        if (focus) focus_ring(dl, p0, p1, r);
        if (pr) result = i;
        ImGui::PopID();
        x += ws[i];
    }
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(total, h));
    ImGui::PopID();
    return result;
}

float pill_at(ImDrawList* dl, ImVec2 p, const char* text, ImU32 bg, ImU32 fg, ImFont* font) {
    ImFont* f = font ? font : (F.cap ? F.cap : ImGui::GetFont());
    float fs = f->FontSize;
    ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0.f, text);
    float px = sp(1.25f), py = sp(0.5f);
    ImVec2 sz(ts.x + 2 * px, ts.y + 2 * py);
    dl->AddRectFilled(p, p + sz, bg, sz.y * 0.5f);
    dl->AddText(f, fs, p + ImVec2(px, py), fg, text);
    return sz.x;
}
void pill(const char* text, ImU32 bg, ImU32 fg) {
    ImFont* f = F.cap ? F.cap : ImGui::GetFont();
    ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.f, text);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = pill_at(ImGui::GetWindowDrawList(), p, text, bg, fg);
    ImGui::Dummy(ImVec2(w, ts.y + sp(1)));
}

/* ---- controller / keyboard hints ---- */
hubglyph::Kind pad_kind = hubglyph::Kind::Xbox;

static ImU32 pad_color(const char* b) {
    if (!b) return PANEL3;
    if (b[0] == 'A' && !b[1]) return IM_COL32(76, 185, 102, 255);
    if (b[0] == 'B' && !b[1]) return IM_COL32(226, 82, 74, 255);
    if (b[0] == 'X' && !b[1]) return IM_COL32(70, 142, 236, 255);
    if (b[0] == 'Y' && !b[1]) return IM_COL32(238, 190, 60, 255);
    return IM_COL32(86, 90, 108, 255);
}
static ImU32 ps_color(const char* shape) {
    if (!std::strcmp(shape, "Cross")) return IM_COL32(120, 160, 230, 255);
    if (!std::strcmp(shape, "Circle")) return IM_COL32(232, 100, 100, 255);
    if (!std::strcmp(shape, "Square")) return IM_COL32(220, 130, 190, 255);
    return IM_COL32(90, 200, 150, 255);   // Triangle
}
/* The text on the button for a pad hint: what this player's pad prints (Xbox names stay as written). */
static const char* pad_text(const char* pad_btn) { return hubglyph::label(pad_kind, pad_btn); }
static float glyph_w(bool pad, const char* pad_btn, const char* key) {
    ImFont* f = F.cap;
    float fs = f->FontSize;
    const char* t = pad ? pad_text(pad_btn) : key;
    float tw = f->CalcTextSizeA(fs, FLT_MAX, 0.f, t).x;
    float h = fs + sp(0.75f);
    if (pad && (hubglyph::is_ps_shape(t) || (!pad_btn[1] && std::strlen(pad_btn) == 1))) return h;   // round face button
    return std::max(h, tw + sp(1.5f));
}
float key_hint_width(bool pad, const char* pad_btn, const char* key, const char* label) {
    ImFont* f = F.cap;
    return glyph_w(pad, pad_btn, key) + sp(0.75f) + f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.f, label).x;
}
/* PlayStation face symbol inside a button disc of radius r. */
static void ps_shape(ImDrawList* dl, const char* s, ImVec2 c, float r, ImU32 col) {
    const float k = r * 0.5f, t = std::max(1.2f, r * 0.17f);
    if (!std::strcmp(s, "Cross")) { dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), col, t); dl->AddLine(ImVec2(c.x + k, c.y - k), ImVec2(c.x - k, c.y + k), col, t); }
    else if (!std::strcmp(s, "Circle")) dl->AddCircle(c, k * 1.05f, col, 20, t);
    else if (!std::strcmp(s, "Square")) dl->AddRect(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), col, 0.f, 0, t);
    else dl->AddTriangle(ImVec2(c.x, c.y - k * 1.1f), ImVec2(c.x + k * 1.1f, c.y + k * 0.8f), ImVec2(c.x - k * 1.1f, c.y + k * 0.8f), col, t);
}
float key_hint(ImDrawList* dl, ImVec2 p, bool pad, const char* pad_btn, const char* key, const char* label) {
    ImFont* f = F.cap;
    float fs = f->FontSize, h = fs + sp(0.75f);
    float gw = glyph_w(pad, pad_btn, key);
    const char* t = pad ? pad_text(pad_btn) : key;
    ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0.f, t);
    if (pad && hubglyph::is_ps_shape(t)) {
        ImVec2 c(p.x + gw * 0.5f, p.y + h * 0.5f);
        dl->AddCircleFilled(c, h * 0.5f, IM_COL32(40, 44, 60, 255), 24);
        ps_shape(dl, t, c, h * 0.5f, ps_color(t));
    } else if (pad && std::strlen(pad_btn) == 1) {
        ImVec2 c(p.x + gw * 0.5f, p.y + h * 0.5f);
        dl->AddCircleFilled(c, h * 0.5f, pad_color(pad_btn), 24);
        dl->AddText(f, fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32(18, 18, 24, 255), t);
    } else {
        ImVec2 q1(p.x + gw, p.y + h);
        if (pad) dl->AddRectFilled(p, q1, pad_color(pad_btn), h * 0.4f);
        else {
            dl->AddRectFilled(p, q1, PANEL3, sp(0.6f));
            dl->AddRect(p, q1, veil(40/255.f), sp(0.6f), 0, 1.f);
        }
        dl->AddText(f, fs, ImVec2(p.x + (gw - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), pad ? IM_COL32_WHITE : TEXT, t);
    }
    float lx = p.x + gw + sp(0.75f);
    ImVec2 ls = f->CalcTextSizeA(fs, FLT_MAX, 0.f, label);
    dl->AddText(f, fs, ImVec2(lx, p.y + (h - ls.y) * 0.5f), TEXT2, label);
    return gw + sp(0.75f) + ls.x;
}

/* ============================ cover art ============================ */

void draw_art(ImDrawList* dl, SDL_Texture* tex, int tw, int th, ImVec2 p0, ImVec2 p1, float rounding, float brightness, float alpha) {
    if (!tex) return;
    ImTextureID id = (ImTextureID)tex;
    float bw = p1.x - p0.x, bh = p1.y - p0.y;
    hubui::ArtFit f = hubui::fit_art((float)tw, (float)th, bw, bh);
    int b = (int)(255 * std::max(0.f, std::min(1.f, brightness)));
    int a = (int)(255 * std::max(0.f, std::min(1.f, alpha)));
    ImU32 tint = IM_COL32(b, b, b, a);
    if (f.cover) {
        dl->AddImageRounded(id, p0, p1, ImVec2(f.u0, f.v0), ImVec2(f.u1, f.v1), tint, rounding);
        return;
    }
    // Letterboxed: magnified, darkened copy fills the tile, the real art sits centred on top.
    hubui::ArtFit bg = hubui::fit_art((float)tw, (float)th, bw, bh, 0.f, 1e9f);
    int bb = (int)(b * 0.38f);
    dl->AddImageRounded(id, p0, p1, ImVec2(bg.u0, bg.v0), ImVec2(bg.u1, bg.v1), IM_COL32(bb, bb, bb, a), rounding);
    ImVec2 q0(p0.x + f.x, p0.y + f.y), q1(q0.x + f.w, q0.y + f.h);
    float inner_r = std::min(rounding * 0.5f, sp(0.75f));
    if (alpha > 0.5f) shadow(dl, q0, q1, inner_r, 0.9f);
    dl->AddImageRounded(id, q0, q1, ImVec2(0, 0), ImVec2(1, 1), tint, inner_r);
}

void draw_art_bg(ImDrawList* dl, SDL_Texture* tex, int tw, int th, ImVec2 p0, ImVec2 p1, float rounding, float brightness, float alpha) {
    if (!tex) return;
    hubui::ArtFit f = hubui::fit_art((float)tw, (float)th, p1.x - p0.x, p1.y - p0.y, 0.f, 1e9f);
    int b = (int)(255 * std::max(0.f, std::min(1.f, brightness)));
    dl->AddImageRounded((ImTextureID)tex, p0, p1, ImVec2(f.u0, f.v0), ImVec2(f.u1, f.v1),
                        IM_COL32(b, b, b, (int)(255 * std::max(0.f, std::min(1.f, alpha)))), rounding);
}

} // namespace ui

/* ---- RetroHub brand logo: the "Candy D-pad" (mirrors assets/brand/build_brand.py geometry in a 256 box) ---- */
void ui_draw_brand_logo(ImDrawList* dl, ImVec2 pos, float size) {
    const float k = size / 256.f;
    const bool tiny = size < 26.f, mid = size < 50.f;
    auto P = [&](float x, float y) { return ImVec2(pos.x + x * k, pos.y + y * k); };
    const ImU32 navy = IM_COL32(29, 20, 70, 255);
    const float th = tiny ? 66.f : mid ? 60.f : 56.f;      // arm thickness
    const float rr = tiny ? 30.f : mid ? 27.f : 26.f;      // arm rounding
    const float br = tiny ? 32.f : mid ? 28.f : 27.f;      // button radius
    const float olu = tiny ? 12.f : mid ? 12.f : 8.f;      // outline width (units, centred on the edge)
    const float ol = std::max(olu * 0.5f * k, size < 64.f ? 0.9f : 0.f);   // px the outline grows outwards (never a hairline at 40 px)
    const float span = 180.f, c = 128.f, bo = 66.f;
    const float shx = tiny ? 0.f : (mid ? 5.f : 6.f) * k, shy = tiny ? 0.f : (mid ? 7.f : 9.f) * k;
    const float lo = c - span * 0.5f, hi = c + span * 0.5f;

    auto arms = [&](ImU32 col, float grow, float dx, float dy) {
        ImVec2 h0 = P(lo, c - th * 0.5f), h1 = P(hi, c + th * 0.5f), v0 = P(c - th * 0.5f, lo), v1 = P(c + th * 0.5f, hi);
        const float r = rr * k + grow;
        dl->AddRectFilled(ImVec2(h0.x - grow + dx, h0.y - grow + dy), ImVec2(h1.x + grow + dx, h1.y + grow + dy), col, r);
        dl->AddRectFilled(ImVec2(v0.x - grow + dx, v0.y - grow + dy), ImVec2(v1.x + grow + dx, v1.y + grow + dy), col, r);
    };
    struct Btn { float x, y; ImU32 col; };
    const Btn btns[4] = {{c - bo, c - bo, IM_COL32(47, 224, 255, 255)}, {c + bo, c - bo, IM_COL32(255, 210, 63, 255)},
                         {c + bo, c + bo, IM_COL32(255, 111, 168, 255)}, {c - bo, c + bo, IM_COL32(139, 224, 78, 255)}};
    // hard shadow, outline, body (lighter top half = the SVG's violet gradient)
    if (!tiny) arms(navy, ol * 1.9f, shx, shy);
    arms(navy, ol, 0.f, 0.f);
    arms(IM_COL32(106, 69, 245, 255), 0.f, 0.f, 0.f);
    dl->PushClipRect(P(0, 0), ImVec2(P(256, 0).x, P(0, 128 - 6).y), true);
    arms(IM_COL32(150, 122, 255, 255), 0.f, 0.f, 0.f);
    dl->PopClipRect();
    // D-pad arrow notches
    if (!tiny) {
        const float t = mid ? 14.f : 12.f, e = 22.f, d = 12.f;
        const ImU32 nc = IM_COL32(29, 20, 70, 72);
        dl->AddTriangleFilled(P(c - t, lo + e), P(c, lo + d), P(c + t, lo + e), nc);
        dl->AddTriangleFilled(P(c - t, hi - e), P(c, hi - d), P(c + t, hi - e), nc);
        dl->AddTriangleFilled(P(lo + e, c - t), P(lo + d, c), P(lo + e, c + t), nc);
        dl->AddTriangleFilled(P(hi - e, c - t), P(hi - d, c), P(hi - e, c + t), nc);
    }
    // play glyph (rounded by a stroke of the same colour)
    {
        ImVec2 tri[3] = {P(c - 10.f, c - 16.f), P(c + 18.f, c), P(c - 10.f, c + 16.f)};
        dl->AddConvexPolyFilled(tri, 3, IM_COL32_WHITE);
        dl->AddPolyline(tri, 3, IM_COL32_WHITE, ImDrawFlags_Closed | ImDrawFlags_RoundCornersAll, std::max(1.f, (tiny ? 12.f : 8.f) * k));
    }
    // candy buttons: shadow, outline, fill, glint
    const int seg = size > 60.f ? 40 : 28;
    for (const Btn& b : btns) {
        if (!tiny) dl->AddCircleFilled(ImVec2(P(b.x, b.y).x + shx * 0.8f, P(b.x, b.y).y + shy * 0.8f), br * k + ol, navy, seg);
        dl->AddCircleFilled(P(b.x, b.y), br * k + ol, navy, seg);
        dl->AddCircleFilled(P(b.x, b.y), br * k, b.col, seg);
        if (!tiny) dl->AddCircleFilled(P(b.x - 9.f, b.y - 9.f), 6.f * k, IM_COL32(255, 255, 255, 190), 16);
    }
}

/* The "RetroHub" wordmark: stroked letters from build_brand.py (cap 0..98) drawn as round-capped polylines, violet with a pink hard
 * shadow and a yellow "o". Built from shapes, so it is as sharp as the pixel grid allows at any DPI scale. `pos` is the top-left of the
 * cap box and `cap_h` the cap height in pixels; returns the width drawn. */
float ui_draw_brand_wordmark(ImDrawList* dl, ImVec2 pos, float cap_h, float alpha, bool light_bg) {
    static const char* text = "RetroHub";
    const float s = cap_h / 98.f;
    const float sw = std::max(2.2f, 16.f * s);
    const float gap = 22.f;
    auto A = [&](ImU32 c) { return ui::scale_alpha(c, alpha); };
    const ImU32 violet = A(light_bg ? IM_COL32(106, 69, 245, 255) : IM_COL32(154, 123, 255, 255));
    const ImU32 yellow = A(light_bg ? IM_COL32(244, 176, 0, 255) : IM_COL32(255, 210, 63, 255));
    const ImU32 pink = A(IM_COL32(255, 95, 168, 255));
    const float off = std::max(1.f, std::floor(cap_h * 0.085f + 0.5f));
    static const float W[8] = {56, 62, 44, 36, 62, 54, 56, 64};

    // one letter -> list of polylines in glyph units
    using Pts = std::vector<ImVec2>;
    auto arc = [](Pts& p, float cx, float cy, float r, float a0, float a1) {
        int n = std::max(8, (int)(std::fabs(a1 - a0) / 9.f));
        for (int i = 0; i <= n; ++i) {
            float a = (a0 + (a1 - a0) * i / n) * 3.14159265f / 180.f;
            p.push_back(ImVec2(cx + r * std::cos(a), cy + r * std::sin(a)));
        }
    };
    auto quad = [](Pts& p, ImVec2 a, ImVec2 c, ImVec2 b) {
        for (int i = 1; i <= 10; ++i) {
            float t = i / 10.f, u = 1.f - t;
            p.push_back(ImVec2(u * u * a.x + 2 * u * t * c.x + t * t * b.x, u * u * a.y + 2 * u * t * c.y + t * t * b.y));
        }
    };
    auto glyph = [&](char ch, std::vector<Pts>& out) {
        out.clear();
        Pts p;
        switch (ch) {
            case 'R':
                p = {{0, 98}, {0, 0}, {28, 0}}; arc(p, 28, 25, 25, -90, 90); p.push_back({0, 50}); out.push_back(p);
                out.push_back({{26, 50}, {52, 98}}); break;
            case 'e':
                out.push_back({{0, 67}, {62, 67}});
                p.clear(); arc(p, 31, 67, 31, 0, -320); out.push_back(p); break;
            case 't':
                p = {{20, 10}, {20, 80}}; quad(p, {20, 80}, {20, 98}, {42, 98}); out.push_back(p);
                out.push_back({{3, 36}, {41, 36}}); break;
            case 'r':
                out.push_back({{2, 36}, {2, 98}});
                p = {{2, 66}}; quad(p, {2, 66}, {2, 36}, {36, 37}); out.push_back(p); break;
            case 'o': arc(p, 31, 67, 31, 0, 360); out.push_back(p); break;
            case 'H': out.push_back({{0, 0}, {0, 98}}); out.push_back({{54, 0}, {54, 98}}); out.push_back({{0, 49}, {54, 49}}); break;
            case 'u':
                p = {{2, 36}, {2, 71}}; arc(p, 29, 71, 27, 180, 0); out.push_back(p);
                out.push_back({{56, 36}, {56, 98}}); break;
            case 'b':
                out.push_back({{2, 0}, {2, 98}}); arc(p, 33, 67, 31, 0, 360); out.push_back(p); break;
        }
    };
    float x = 0.f;
    std::vector<Pts> g;
    // three passes (shadow, then letters) keep the shadow behind every letter
    for (int pass = 0; pass < 2; ++pass) {
        x = 0.f;
        for (int i = 0; i < 8; ++i) {
            glyph(text[i], g);
            ImU32 col = pass == 0 ? pink : (i == 4 ? yellow : violet);
            float ox = pos.x + (pass == 0 ? off : 0.f), oy = pos.y + (pass == 0 ? off : 0.f);
            for (const Pts& pl : g) {
                std::vector<ImVec2> q;
                q.reserve(pl.size());
                for (const ImVec2& v : pl) q.push_back(ImVec2(ox + (x + v.x) * s, oy + v.y * s));
                const bool closed = q.size() > 8 && std::fabs(pl.front().x - pl.back().x) < 0.01f && std::fabs(pl.front().y - pl.back().y) < 0.01f;
                if (closed) q.pop_back();
                dl->AddPolyline(q.data(), (int)q.size(), col, closed ? ImDrawFlags_Closed | ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersAll, sw);
                for (size_t vi = 1; vi + 1 < q.size(); ++vi) {   // round joins at the sharp corners (the polyline joins are mitred)
                    ImVec2 d0(q[vi].x - q[vi - 1].x, q[vi].y - q[vi - 1].y), d1(q[vi + 1].x - q[vi].x, q[vi + 1].y - q[vi].y);
                    float l0 = std::sqrt(d0.x * d0.x + d0.y * d0.y), l1 = std::sqrt(d1.x * d1.x + d1.y * d1.y);
                    if (l0 > 0.f && l1 > 0.f && (d0.x * d1.x + d0.y * d1.y) / (l0 * l1) < 0.9f) dl->AddCircleFilled(q[vi], sw * 0.5f, col, 12);
                }
                if (!closed) {   // round caps
                    dl->AddCircleFilled(q.front(), sw * 0.5f, col, 12);
                    dl->AddCircleFilled(q.back(), sw * 0.5f, col, 12);
                }
            }
            x += W[i] + gap;
        }
    }
    return (x - gap) * s + sw;
}
