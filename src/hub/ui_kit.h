#pragma once
/* RetroHub UI kit: palette, fonts, theme, vector icons, custom-drawn widgets, cover-art drawing.
 * Depends only on Dear ImGui (+ SDL_Texture handles); no app state, so main.cpp stays about the pages. */

#include "imgui.h"

#include <string>
#include <vector>

struct SDL_Texture;

namespace ui {

extern float S;                 // DPI scale (1.0 = 96 dpi)
inline float sp(float n) { return n * 8.f * S; }   // 8-pt spacing scale: sp(1)=8px, sp(0.5)=4px, sp(2)=16px ...

struct Fonts {
    ImFont *cap = nullptr, *body = nullptr, *bold = nullptr, *h2 = nullptr, *title = nullptr, *hero = nullptr, *big = nullptr;
};
extern Fonts F;

/* ---- palette: one accent, neutral surfaces, semantic colours ---- */
constexpr ImU32 BG = IM_COL32(16, 17, 22, 255);
constexpr ImU32 SIDEBAR = IM_COL32(12, 13, 17, 255);
constexpr ImU32 PANEL = IM_COL32(26, 28, 36, 255);
constexpr ImU32 PANEL2 = IM_COL32(37, 39, 51, 255);
constexpr ImU32 PANEL3 = IM_COL32(50, 53, 69, 255);
constexpr ImU32 HAIRLINE = IM_COL32(255, 255, 255, 20);
constexpr ImU32 TEXT = IM_COL32(238, 240, 246, 255);
constexpr ImU32 TEXT2 = IM_COL32(152, 157, 175, 255);
constexpr ImU32 TEXT3 = IM_COL32(104, 109, 127, 255);
constexpr ImU32 ACCENT = IM_COL32(108, 122, 255, 255);
constexpr ImU32 GOOD = IM_COL32(56, 200, 120, 255);
constexpr ImU32 BAD = IM_COL32(232, 95, 85, 255);
constexpr ImU32 WARN = IM_COL32(240, 176, 64, 255);

ImU32 mix(ImU32 a, ImU32 b, float t);
ImU32 with_alpha(ImU32 c, float a);        // sets alpha (0..1)
ImU32 scale_alpha(ImU32 c, float k);       // multiplies alpha
float anim(float& v, float target, float rate = 14.f);   // time-based smoothing using io.DeltaTime; returns v

void setup_fonts(float scale);
void setup_theme(float scale);

/* ---- drawing primitives ---- */
void gradient_round_rect(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 c0, ImU32 c1, float rounding);   // diagonal
/* One colour whose alpha ramps from a_top (y=p0) to a_bot (y=p1); flags = ImDrawFlags_RoundCorners*. */
void vfade_round_rect(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 col, float rounding, ImDrawFlags flags, float a_top, float a_bot);
/* Same, ramping left (p0.x) to right (p1.x). */
void hfade_round_rect(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 col, float rounding, ImDrawFlags flags, float a_left, float a_right);
void shadow(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float rounding, float strength = 1.f, float dy = 0.f);   // layered soft shadow
void glow(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float rounding, ImU32 col, float amount);                // outer coloured halo
void focus_ring(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float rounding);   // bright animated ring for keyboard/gamepad focus
void shimmer(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float rounding, ImU32 base = PANEL2);   // skeleton block with moving highlight
void text_shadowed(ImDrawList* dl, ImFont* f, float size, ImVec2 p, ImU32 col, const char* text);

/* Word-wraps to at most max_lines lines; the last line is cut with "..." when the text does not fit. */
std::vector<std::string> wrap_lines(ImFont* f, float size, const std::string& text, float max_w, int max_lines);
std::string ellipsize(ImFont* f, float size, const std::string& text, float max_w);

/* ---- icons ---- */
enum class Icon { None, Home, Globe, Cloud, Gamepad, Gear, Info, Search, Refresh, Plus, Star, StarFilled, Play, ChevronL, ChevronR,
                  Dots, Folder, Close, Check, Bolt };
void icon(ImDrawList* dl, Icon ic, ImVec2 center, float size, ImU32 col);

/* ---- widgets (custom drawn; all honour gamepad/keyboard focus) ---- */
bool button(const char* label, ImVec2 size = ImVec2(0, 0), bool primary = false, bool enabled = true, Icon ic = Icon::None);
bool icon_button(const char* id, Icon ic, float size, bool active = false, const char* tooltip = nullptr, ImU32 active_col = ACCENT);
bool chip(const char* label, bool on, ImU32 col);
/* Inside a modal: true on the frame B (pad) or Esc is pressed while this modal has the focus. ImGui never closes modals on its own. */
bool modal_back();
/* Pill-style segmented control; returns the (possibly new) selected index. */
int segmented(const char* id, const char* const* labels, int n, int cur);
float pill_at(ImDrawList* dl, ImVec2 p, const char* text, ImU32 bg, ImU32 fg = IM_COL32_WHITE, ImFont* font = nullptr);
void pill(const char* text, ImU32 bg, ImU32 fg = IM_COL32_WHITE);
/* Controller/keyboard hint ("(A) Play"). Draws at p, returns the width used. pad = show the gamepad glyph, else the key cap. */
float key_hint(ImDrawList* dl, ImVec2 p, bool pad, const char* pad_btn, const char* key, const char* label);
float key_hint_width(bool pad, const char* pad_btn, const char* key, const char* label);

/* ---- cover art ---- */
/* Draws `tex` inside [p0,p1]: cropped to fill when the aspect fits the tile, otherwise letterboxed over a
 * dimmed, magnified copy of itself so the tile never looks empty. brightness 1 = normal, <1 = dimmed. */
void draw_art(ImDrawList* dl, SDL_Texture* tex, int tw, int th, ImVec2 p0, ImVec2 p1, float rounding, float brightness = 1.f,
              float alpha = 1.f);
/* Always crops to fill (used for ambient backdrops). */
void draw_art_bg(ImDrawList* dl, SDL_Texture* tex, int tw, int th, ImVec2 p0, ImVec2 p1, float rounding, float brightness, float alpha = 1.f);

} // namespace ui

/* RetroHub brand mark (rounded app tile + gradient hexagon "hub" + play glyph + friend nodes), vector-drawn to
 * match assets/brand/retrohub-logo.svg. `pos` is the top-left, `size` the tile edge in pixels. No texture needed. */
void ui_draw_brand_logo(ImDrawList* dl, ImVec2 pos, float size);
