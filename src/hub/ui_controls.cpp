#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui_controls.h"

#include "imgui_internal.h"
#include "ui_layout_logic.h"
#include "ui_sound.h"
#include "app.h"   // A.pad_mode (shoulder glyphs)

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <unordered_map>

/* UI kit sections, option rows, tab bar (see ui_controls.h). */

namespace ui {
namespace detail {
/* The last tab bar drawn: its window, frame and bottom edge. ui::sheet skips its header hairline when the header ends with a tab bar
 * (the bar draws its own baseline). */
ImGuiID tabbar_window = 0;
int tabbar_frame = -10;
float tabbar_bottom = 0.f;
}  // namespace detail
namespace {

const float kPi = 3.14159265f;

bool nav_visible() { return ImGui::GetIO().NavVisible; }
/* Rows and section headers stop at about 900 px (100 % UI size): on a wide window a label stays close to its control. */
float max_row_w() { return sp(112.f); }
float text_w(ImFont* f, const char* s, const char* e = nullptr) { return f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.f, s, e).x; }
ImU32 al(ImU32 c) { return scale_alpha(c, ImGui::GetStyle().Alpha); }   // honours BeginDisabled

bool is_child(const ImGuiWindow* w) { return (w->Flags & ImGuiWindowFlags_ChildWindow) && !(w->Flags & ImGuiWindowFlags_Popup); }
/* Steps from `from` up to `target` (0 = same window), not crossing a popup / root boundary; -1 when target is not around `from`. */
int chain_depth(ImGuiWindow* from, ImGuiWindow* target) {
    int d = 0;
    for (ImGuiWindow* w = from; w; w = is_child(w) ? w->ParentWindow : nullptr, ++d)
        if (w == target) return d;
    return -1;
}

/* A plain D-pad / arrow move submitted this frame (not tabbing, not the kit's own LT / RT page request). */
ImGuiDir pad_move_dir() {
    ImGuiContext& g = *GImGui;
    if (!g.NavMoveSubmitted || (g.NavMoveFlags & (ImGuiNavMoveFlags_IsTabbing | ImGuiNavMoveFlags_AllowCurrentNavId))) return ImGuiDir_None;
    return g.NavMoveDir;
}

/* Chevron: angle 0 points right, pi/2 down, -pi/2 up. */
void chevron(ImDrawList* dl, ImVec2 c, float s, float angle, ImU32 col) {
    const float ca = std::cos(angle), sa = std::sin(angle);
    auto P = [&](float x, float y) { return ImVec2(c.x + (x * ca - y * sa) * s, c.y + (x * sa + y * ca) * s); };
    const ImVec2 p[3] = {P(-0.16f, -0.32f), P(0.16f, 0.f), P(-0.16f, 0.32f)};
    dl->AddPolyline(p, 3, col, 0, std::max(1.5f, s * 0.12f));
}

/* ---------------- option groups ---------------- */

struct GroupState {
    std::unordered_map<ImGuiID, ImVec2> rows;   // row id -> (label + pill width, natural control width), kept while the group lives
    float label_w = 0.f, ctl_w = 0.f;
    int frame = -10;
    HelpMode help = HelpMode::Inline;           // Strip: compact rows, the help in option_help_strip()
};
std::unordered_map<ImGuiID, GroupState> g_groups;
GroupState* g_group = nullptr;
bool compact_rows() { return g_group && g_group->help == HelpMode::Strip; }

/* The help strip: what the Strip group drawn this frame says about its rows. */
struct HelpTrack {
    ImGuiID group = 0;                 // the Strip group (another one starts afresh: no last row, no texts)
    int frame = -10;                   // the frame it was drawn (option_group_end)
    OptionHelp live, first, last;
    int live_prio = 0;                 // this frame: 2 = a row has the focus (or its open list does), 1 = the mouse
    bool has_first = false, has_last = false;
    std::vector<std::pair<std::string, std::string>> texts;   // every (label, help) of the group seen so far: the strip's height
    float lines_w = -1.f;
    size_t lines_n = 0;
    int lines_style = -1, lines_max = 0;
    int lines = 1;
} g_help;

void help_track(const OptionHelp& h, int prio, bool visible = true) {
    if (!compact_rows()) return;
    if (visible && (!g_help.has_first || (g_help.first.help.empty() && !h.help.empty()))) { g_help.first = h; g_help.has_first = true; }   // the first row in view that has a help
    if (prio > g_help.live_prio) { g_help.live = h; g_help.live_prio = prio; }
    if (!h.help.empty()) {
        const std::pair<std::string, std::string> t(h.label, h.help);
        if (std::find(g_help.texts.begin(), g_help.texts.end(), t) == g_help.texts.end()) g_help.texts.push_back(t);
    }
}

/* Word wrap whose first line is first_w wide and the rest w (a run-in heading takes the start of the first line). At most max_lines; the
 * last line takes the rest, cut with "...". The first line may stay empty when not even one word fits after the heading. */
std::vector<std::string> wrap_after(ImFont* f, float fs, const std::string& s, float first_w, float w, int max_lines) {
    std::vector<std::string> words;
    for (size_t a = 0; a < s.size();) {
        size_t b = s.find(' ', a);
        if (b == std::string::npos) b = s.size();
        if (b > a) words.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    std::vector<std::string> lines(1);
    size_t k = 0;
    for (; k < words.size(); ++k) {
        std::string& cur = lines.back();
        const std::string cand = cur.empty() ? words[k] : cur + " " + words[k];
        const float lim = lines.size() == 1 ? first_w : w;
        if ((cur.empty() && lines.size() > 1) || f->CalcTextSizeA(fs, FLT_MAX, 0.f, cand.c_str()).x <= lim) { cur = cand; continue; }
        if ((int)lines.size() >= max_lines) break;
        lines.emplace_back();
        --k;   // this word starts the next line
    }
    if (k < words.size()) {   // more than max_lines
        std::string rest = lines.back();
        for (; k < words.size(); ++k) rest += (rest.empty() ? "" : " ") + words[k];
        lines.back() = ellipsize(f, fs, rest, lines.size() == 1 ? first_w : w);
    }
    return lines;
}

float run_in_label_w(const std::string& label) { return label.empty() ? 0.f : text_w(F.bold, label.c_str()) + sp(1.f); }

/* ---------------- columns of sections ---------------- */

struct FlowItem { ImGuiID id = 0; ImRect r; ImGuiID scope = 0; };
struct Flow {
    bool on = false;
    ImGuiWindow* win = nullptr;
    int cols = 1, cur = 0;
    float x0 = 0.f, y0 = 0.f, col_w = 0.f, gap = 0.f, bottom = 0.f;
    float work_max = 0.f, content_max = 0.f;   // the window's region, put back at the end
    int fcol = -1;                             // the column the focus is in (from its last frame's rect), -1 = none
    bool vertical = false;                     // an Up / Down move this frame: the other columns are left out of it
    bool nonav = false;                        // ImGuiItemFlags_NoNav pushed for the current column
    std::vector<std::vector<FlowItem>> items;  // the kit rows and section headers of each column, this frame
    /* last frame's flow, relative to the window's content start (scroll-proof) */
    ImGuiID prev_win = 0;
    int prev_frame = -10;
    ImRect prev_rel;
} g_flow;

void flow_record(ImGuiID id, ImVec2 p0, ImVec2 p1) {
    if (!g_flow.on || g_flow.win != ImGui::GetCurrentWindow() || g_flow.cur >= (int)g_flow.items.size()) return;
    g_flow.items[(size_t)g_flow.cur].push_back(FlowItem{id, ImRect(p0, p1), GImGui->CurrentFocusScopeId});
}

/* The plain D-pad / arrow move of this frame (not tabbing, not a focus request), page moves included. */
bool flow_move(ImGuiDir* dir) {
    ImGuiContext& g = *GImGui;
    if (!g.NavMoveSubmitted || (g.NavMoveFlags & (ImGuiNavMoveFlags_IsTabbing | ImGuiNavMoveFlags_FocusApi))) return false;
    *dir = g.NavMoveDir;
    return true;
}

void flow_column_start(int c) {
    ImGuiWindow* win = g_flow.win;
    const float x = g_flow.x0 + (float)c * (g_flow.col_w + g_flow.gap);
    const float r = std::min(g_flow.work_max, x + std::max(1.f, g_flow.col_w));
    win->WorkRect.Max.x = r;
    win->ContentRegionRect.Max.x = r;
    ImGui::SetCursorScreenPos(ImVec2(x, g_flow.y0));
    g_flow.cur = c;
    if (g_flow.vertical && g_flow.fcol >= 0 && c != g_flow.fcol) {   // Up / Down stay in the focus's column
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        g_flow.nonav = true;
    }
    ImGui::BeginGroup();
}

void flow_column_end() {
    ImGui::EndGroup();
    g_flow.bottom = std::max(g_flow.bottom, ImGui::GetItemRectMax().y);
    if (g_flow.nonav) { ImGui::PopItemFlag(); g_flow.nonav = false; }
}

/* ---------------- one option row ---------------- */

struct CtlSpec {
    std::string value;        // text shown in the control ("" = none)
    float chrome = 0.f;       // control width that is not value text
    float natural = 0.f;      // the control's natural width (longest value + chrome)
    float min_h = 0.f;
};
struct RowFrame {
    ImGuiID id = 0;
    bool pressed = false, mouse = false, hovered = false, focused = false;
    float hv = 0.f;
    ImVec2 p0, p1;            // row
    ImVec2 c0, c1;            // control
    std::vector<std::string> vlines;   // value text, wrapped to the control
    OptionHelp help;          // what the help strip says about this row (Strip groups)
};

void row_begin(const char* label, const RowOpts& o, const CtlSpec& cs, RowFrame& rf) {
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    ImFont* fb = F.body;
    ImFont* fc = F.cap;
    const float fs = fb->FontSize, cfs = fc->FontSize;
    const char* lend = ImGui::FindRenderedTextEnd(label);
    const std::string text(label, lend);
    rf.id = win->GetID(label);
    const bool compact = compact_rows();   // one line, tight padding; the help goes to the strip

    const float pill_gap = sp(1.f);
    const bool pill_btn = o.pill && o.pill_pressed;
    const float glyph_w = key_hint_width(true, "X", "", "") - sp(0.75f);
    const float xslot = pill_btn ? std::max(sp(2.f), glyph_w) : 0.f;   // the pill button's x (the pad's X glyph while focused)
    const float pill_w = o.pill ? text_w(fc, o.pill) + sp(2.5f) + xslot : 0.f;
    const char* pill2 = o.pill2;
    float pill2_w = pill2 ? text_w(fc, pill2) + sp(2.5f) : 0.f;
    bool pills = o.pill || pill2;
    float pills_w = pill_w + pill2_w + (o.pill && pill2 ? pill_gap : 0.f);
    const float pill_h = cfs + sp(1.f);
    const float label_tw = text_w(fb, text.c_str());
    const float label_need = label_tw + (pills ? pill_gap + pills_w : 0.f);
    float L = label_need, C = cs.natural;
    if (g_group) {
        g_group->rows[rf.id] = ImVec2(label_need, cs.natural);
        L = std::max(L, g_group->label_w);
        C = std::max(C, g_group->ctl_w);
    }
    const float padx = sp(1.25f), pady = compact ? sp(0.375f) : sp(1.f), gl = sp(0.25f);
    const float under = compact ? sp(0.5f) : sp(1.f);   // label to control when the control goes under it
    const float min_h = compact ? std::min(cs.min_h, sp(4.f)) : cs.min_h;
    const float avail = std::max(1.f, std::min(ImGui::GetContentRegionAvail().x, max_row_w()));
    hubui::kit::RowIn in;
    in.avail = std::max(0.f, avail - 2.f * padx);
    in.label_w = L;
    in.ctl_w = C;
    in.ctl_min = compact ? sp(16.f) : sp(22.f);     // compact: the row stays on one line longer (a cut label is whole in the strip)
    in.label_min = compact ? sp(14.f) : sp(20.f);
    in.gap = sp(2.f);
    const hubui::kit::RowOut lay = hubui::kit::option_columns(in);

    const float lw = std::max(1.f, lay.label_w);
    std::vector<std::string> llines;
    bool pill_inline = false, pills_stack = false;
    if (compact) {   // one line: the label, cut with "..." when it must be (the strip's heading shows it whole), then its pills
        if (pill2 && label_tw + pill_gap + pills_w > lw + 0.5f) {   // no room for the second pill: the strip says it instead
            rf.help.note = pill2;
            pill2 = nullptr;
            pill2_w = 0.f;
            pills = o.pill != nullptr;
            pills_w = pill_w;
        }
        const float room = std::max(sp(4.f), lw - (pills ? pill_gap + pills_w : 0.f));
        llines.push_back(label_tw <= room + 0.5f ? text : ellipsize(fb, fs, text, room));
        pill_inline = pills;
    } else {
        llines = wrap_lines(fb, fs, text, lw, 3);
        pill_inline = pills && llines.size() == 1 && text_w(fb, llines[0].c_str()) + pill_gap + pills_w <= lw + 0.5f;
        pills_stack = pills && !pill_inline && o.pill && pill2 && pills_w > lw + 0.5f;   // under the label, one per line
    }
    std::vector<std::string> hlines;
    if (!compact && o.help && *o.help) hlines = wrap_lines(fc, cfs, o.help, lw, 3);
    float text_h = llines.size() * fs + (llines.size() - 1) * gl;
    if (pills && !pill_inline) text_h += sp(0.5f) + pill_h + (pills_stack ? gl + pill_h : 0.f);
    if (!hlines.empty()) text_h += sp(0.5f) + hlines.size() * cfs + (hlines.size() - 1) * gl;

    const float ctl_w = lay.ctl_w;
    if (!cs.value.empty()) rf.vlines = wrap_lines(fb, fs, cs.value, std::max(1.f, ctl_w - cs.chrome), 2);
    const float ctl_h = std::max(min_h, rf.vlines.size() > 1 ? rf.vlines.size() * fs + gl * (rf.vlines.size() - 1) + (compact ? sp(1.f) : sp(2.f)) : min_h);
    const float h = lay.wrap ? pady * 2.f + text_h + under + ctl_h : pady * 2.f + std::max(text_h, ctl_h);

    ImGui::BeginDisabled(!o.enabled);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, sp(0.5f)));
    rf.pressed = ImGui::InvisibleButton(label, ImVec2(avail, h), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleVar();
    rf.mouse = rf.pressed && ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    rf.hovered = ImGui::IsItemHovered();
    rf.focused = ImGui::IsItemFocused() && nav_visible();
    item_sound(rf.focused, rf.pressed);
    float* hv = ImGui::GetStateStorage()->GetFloatRef(rf.id, 0.f);
    anim(*hv, (rf.hovered || rf.focused) ? 1.f : 0.f, 16.f);
    rf.hv = *hv;
    rf.p0 = ImGui::GetItemRectMin();
    rf.p1 = ImGui::GetItemRectMax();
    flow_record(rf.id, rf.p0, rf.p1);
    if (g_headless && rf.focused) {   // QA (HUB_SHOT_TRACE): which option row the pad / keyboard focus is on, and in which column (x)
        static ImGuiID last = 0;
        if (last != rf.id) { last = rf.id; shot_trace("kit-focus", text + " x=" + std::to_string((int)rf.p0.x) + " y=" + std::to_string((int)rf.p0.y)); }
    }
    if (compact) {
        rf.help.label = text;
        if (o.help) rf.help.help = o.help;
        help_track(rf.help, rf.focused ? 2 : rf.hovered ? 1 : 0, ImGui::IsItemVisible());
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rd = sp(1.25f);
    if (rf.hv > 0.01f) dl->AddRectFilled(rf.p0, rf.p1, al(veil(0.045f * rf.hv)), rd);
    if (rf.focused) focus_ring(dl, rf.p0 + ImVec2(sp(0.25f), sp(0.25f)), rf.p1 - ImVec2(sp(0.25f), sp(0.25f)), rd);

    /* label, pills, help */
    ImRect pill_r;   // the pill button
    auto draw_pills = [&](float x, float y, bool stack) {
        if (o.pill) {
            const ImU32 bg = al(o.pill_col ? o.pill_col : ACCENT);
            if (pill_btn) {
                pill_r = ImRect(ImVec2(x, y), ImVec2(x + pill_w, y + pill_h));
                const bool ph = rf.hovered && pill_r.Contains(ImGui::GetIO().MousePos);
                dl->AddRectFilled(pill_r.Min, pill_r.Max, ph ? mix(bg, al(IM_COL32_WHITE), 0.2f) : bg, pill_h * 0.5f);
                dl->AddText(fc, cfs, ImVec2(x + sp(1.25f), y + sp(0.5f)), al(IM_COL32_WHITE), o.pill);
                const float sx = x + sp(1.25f) + text_w(fc, o.pill) + sp(0.5f);   // the slot after the text
                if (rf.focused && A.pad_mode) key_hint(dl, ImVec2(sx + (xslot - glyph_w) * 0.5f, y + (pill_h - cfs - sp(0.75f)) * 0.5f), true, "X", "", "");
                else icon(dl, Icon::Close, ImVec2(sx + xslot * 0.5f, y + pill_h * 0.5f), sp(1.5f), al(IM_COL32_WHITE));
            } else pill_at(dl, ImVec2(x, y), o.pill, bg, al(IM_COL32_WHITE), fc);
        }
        if (pill2) {
            const ImVec2 p2 = !o.pill ? ImVec2(x, y) : stack ? ImVec2(x, y + pill_h + gl) : ImVec2(x + pill_w + pill_gap, y);
            pill_at(dl, p2, pill2, al(PANEL3), al(TEXT2), fc);
        }
    };
    const float tx = rf.p0.x + padx;
    float ty = lay.wrap ? rf.p0.y + pady : rf.p0.y + (h - text_h) * 0.5f;
    const ImU32 tcol = al(TEXT);
    for (size_t i = 0; i < llines.size(); ++i) {
        dl->AddText(fb, fs, ImVec2(tx, ty), tcol, llines[i].c_str());
        if (i == 0 && pill_inline) draw_pills(tx + text_w(fb, llines[0].c_str()) + pill_gap, ty + (fs - pill_h) * 0.5f, false);
        ty += fs + gl;
    }
    ty -= gl;
    if (pills && !pill_inline) {
        ty += sp(0.5f);
        draw_pills(tx, ty, pills_stack);
        ty += pill_h + (pills_stack ? gl + pill_h : 0.f);
    }
    if (pill_btn && o.enabled) {   // the pill button: a click on it (not the row's own action), or X / Delete while the row has the focus
        ImGuiIO& io = ImGui::GetIO();
        if (rf.mouse && pill_r.Contains(io.MouseClickedPos[0]) && pill_r.Contains(io.MousePos)) {
            *o.pill_pressed = true;
            rf.pressed = rf.mouse = false;
        } else if (rf.focused && !io.WantTextInput && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false) || ImGui::IsKeyPressed(ImGuiKey_Delete, false))) {
            ImGui::SetKeyOwner(ImGuiKey_GamepadFaceLeft, rf.id, ImGuiInputFlags_LockThisFrame);
            *o.pill_pressed = true;
            ui_sound::play(ui_sound::Sfx::Select);
        }
    }
    if (!hlines.empty()) {
        ty += sp(0.5f);
        for (const std::string& l : hlines) {
            dl->AddText(fc, cfs, ImVec2(tx, ty), al(TEXT2), l.c_str());
            ty += cfs + gl;
        }
    }

    rf.c0.x = rf.p0.x + padx + lay.ctl_x;
    rf.c1.x = rf.c0.x + ctl_w;
    rf.c0.y = lay.wrap ? rf.p0.y + pady + text_h + under : rf.p0.y + (h - ctl_h) * 0.5f;
    rf.c1.y = rf.c0.y + ctl_h;
    if (o.tooltip && rf.hovered && !rf.focused) ImGui::SetTooltip("%s", o.tooltip);
}

void row_end() { ImGui::EndDisabled(); }

/* Value text inside a control, centred vertically; align 0 = left, 0.5 = centre. */
void draw_value(ImDrawList* dl, const RowFrame& rf, float x0, float x1, float align, ImU32 col) {
    ImFont* fb = F.body;
    const float fs = fb->FontSize, gl = sp(0.25f);
    const float th = rf.vlines.size() * fs + (rf.vlines.empty() ? 0.f : (rf.vlines.size() - 1) * gl);
    float y = (rf.c0.y + rf.c1.y - th) * 0.5f;
    for (const std::string& l : rf.vlines) {
        const float w = text_w(fb, l.c_str());
        dl->AddText(fb, fs, ImVec2(x0 + std::max(0.f, (x1 - x0 - w) * align), y), col, l.c_str());
        y += fs + gl;
    }
}

void control_box(ImDrawList* dl, const RowFrame& rf, bool open) {
    const float rd = std::min(sp(1.25f), (rf.c1.y - rf.c0.y) * 0.5f);
    dl->AddRectFilled(rf.c0, rf.c1, al(mix(PANEL2, PANEL3, open ? 1.f : rf.hv)), rd);
    dl->AddRect(rf.c0, rf.c1, al(open ? with_alpha(ACCENT, 0.7f) : HAIRLINE), rd, 0, 1.f);
}

/* ---------------- dropdown ---------------- */

struct DdState {
    int frame = -10;
    int open_frame = -10;
    int list_focus_frame = -10;
    bool focus_sel = false;   // put the focus on the chosen option when the list is drawn
    bool refocus = false;     // put the focus back on the row next frame
};
std::unordered_map<ImGuiID, DdState> g_dd;
ImGuiID g_dd_open = 0;        // the one open list

bool dropdown_impl(const char* label, int* idx, int n, const std::function<const char*(int)>& item, const RowOpts& o) {
    ImGuiContext& g = *GImGui;
    const int fc = ImGui::GetFrameCount();
    const int cur = n > 0 ? std::max(-1, std::min(*idx, n - 1)) : -1;
    ImFont* fb = F.body;
    float maxw = 0.f;
    for (int i = 0; i < n; ++i) maxw = std::max(maxw, text_w(fb, item(i)));
    if (o.value_text) maxw = std::max(maxw, text_w(fb, o.value_text));
    CtlSpec cs;
    cs.chrome = sp(2.f) + sp(4.5f);   // left padding + chevron area
    cs.natural = maxw + cs.chrome;
    cs.value = o.value_text ? o.value_text : (cur >= 0 ? item(cur) : "");
    cs.min_h = sp(4.5f);
    RowFrame rf;
    row_begin(label, o, cs, rf);
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    DdState& st = g_dd[rf.id];
    bool open = g_dd_open == rf.id && st.frame >= fc - 1;
    if (!open && g_dd_open == rf.id) g_dd_open = 0;
    st.frame = fc;
    if (st.refocus) { st.refocus = false; ImGui::SetFocusID(rf.id, win); ImGui::ScrollToRect(win, ImRect(rf.p0, rf.p1)); }   // (scrolls only when not in view)
    bool changed = false;
    const bool in_list = st.list_focus_frame >= fc - 1;
    if (open && in_list) help_track(rf.help, 2);   // the open list has the focus: the strip keeps this row's help
    const bool back =ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (open && back && (g.NavId == rf.id || in_list)) {   // B / Esc closes only the list
        ImGui::SetKeyOwner(ImGuiKey_GamepadFaceRight, rf.id, ImGuiInputFlags_LockThisFrame);   // the sheet / dialog / page "back" reads the same press
        ImGui::SetKeyOwner(ImGuiKey_Escape, rf.id, ImGuiInputFlags_LockThisFrame);
        ui_sound::play(ui_sound::Sfx::Back);
        open = false;
        g_dd_open = 0;
        ImGui::SetFocusID(rf.id, win);
        ImGui::ScrollToRect(win, ImRect(rf.p0, rf.p1));
    } else if (open && g.NavId && g.NavId != rf.id && !in_list && st.open_frame < fc - 1) {
        open = false;   // the focus went somewhere else (LT / RT page, a click elsewhere)
        g_dd_open = 0;
    }
    if (rf.pressed && o.enabled) {
        if (open) { open = false; g_dd_open = 0; }
        else { open = true; g_dd_open = rf.id; st.focus_sel = true; st.open_frame = fc; }
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    control_box(dl, rf, open);
    const float cx = rf.c1.x - sp(2.25f), cy = (rf.c0.y + rf.c1.y) * 0.5f;
    chevron(dl, ImVec2(cx, cy), sp(2.f), open ? -kPi * 0.5f : kPi * 0.5f, al(mix(TEXT2, TEXT, rf.hv)));
    draw_value(dl, rf, rf.c0.x + sp(2.f), rf.c1.x - sp(4.5f), 0.f, al(TEXT));
    row_end();

    if (open) {
        const float lx = rf.c0.x, lw = std::max(1.f, rf.c1.x - rf.c0.x);
        const float ipad = sp(0.5f), fs = fb->FontSize, gl = sp(0.25f), check = sp(3.5f);
        std::vector<std::vector<std::string>> lines(n);
        std::vector<float> hs(n);
        float total = ipad * 2.f;
        for (int i = 0; i < n; ++i) {
            lines[i] = wrap_lines(fb, fs, item(i), std::max(1.f, lw - ipad * 2.f - check - sp(1.5f)), 2);
            hs[i] = std::max(sp(4.5f), lines[i].size() * fs + (lines[i].size() - 1) * gl + sp(2.f));
            total += hs[i];
        }
        const ImVec2 l0(lx, ImGui::GetCursorScreenPos().y), l1(lx + lw, l0.y + total);
        if (st.open_frame == fc && n > 0) {   // just opened: bring the row and its list into view (or what fits of it, down to the chosen
            const int s = std::max(0, cur);   // option), so a host that does not follow the focus never leaves the D-pad on an option it cannot see
            float sy = l0.y + ipad;
            for (int i = 0; i < s; ++i) sy += hs[i];
            const float view = win->InnerRect.GetHeight() - sp(2.f);
            ImGui::ScrollToRect(win, l1.y - rf.p0.y <= view ? ImRect(rf.p0, l1) : total <= view ? ImRect(l0, l1) : ImRect(ImVec2(lx, sy), ImVec2(lx + lw, sy + hs[s])));
        }
        const float rd = sp(1.25f);
        shadow(dl, l0, l1, rd, 0.6f);
        dl->AddRectFilled(l0, l1, PANEL2, rd);
        dl->AddRect(l0, l1, HAIRLINE, rd, 0, 1.f);
        ImGui::PushOverrideID(rf.id);
        float y = l0.y + ipad;
        int picked = -1;
        for (int i = 0; i < n; ++i) {
            ImGui::SetCursorScreenPos(ImVec2(lx + ipad, y));
            ImGui::PushID(i);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.f));
            const bool pr = ImGui::InvisibleButton("##opt", ImVec2(lw - ipad * 2.f, hs[i]), ImGuiButtonFlags_EnableNav);
            ImGui::PopStyleVar();
            const ImGuiID oid = ImGui::GetItemID();
            const bool oh = ImGui::IsItemHovered();
            if (st.focus_sel && i == std::max(0, cur)) { ImGui::SetFocusID(oid, win); st.focus_sel = false; }
            const bool ofocus = ImGui::IsItemFocused();
            const bool of = ofocus && nav_visible();
            item_sound(of, pr);
            if (ofocus) {
                st.list_focus_frame = fc;
                const ImGuiDir md = pad_move_dir();   // keep the D-pad inside the open list
                if (md == ImGuiDir_Left || md == ImGuiDir_Right ||
                    (md == ImGuiDir_Up && hubui::kit::list_move_blocked(i, n, -1)) || (md == ImGuiDir_Down && hubui::kit::list_move_blocked(i, n, +1)))
                    ImGui::NavMoveRequestCancel();
            }
            const ImVec2 o0 = ImGui::GetItemRectMin(), o1 = ImGui::GetItemRectMax();
            if (oh || of) dl->AddRectFilled(o0, o1, veil(of ? 0.09f : 0.06f), sp(1.f));
            if (i == cur) icon(dl, Icon::Check, ImVec2(o0.x + check * 0.5f + sp(0.5f), (o0.y + o1.y) * 0.5f), sp(2.f), ACCENT);
            const float th = lines[i].size() * fs + (lines[i].size() - 1) * gl;
            float ty = (o0.y + o1.y - th) * 0.5f;
            for (const std::string& l : lines[i]) {
                dl->AddText(fb, fs, ImVec2(o0.x + check + sp(0.75f), ty), i == cur ? TEXT : mix(TEXT2, TEXT, 0.6f), l.c_str());   // same font as measured
                ty += fs + gl;
            }
            if (of) focus_ring(dl, o0 + ImVec2(sp(0.25f), sp(0.25f)), o1 - ImVec2(sp(0.25f), sp(0.25f)), sp(1.f));
            if (pr) picked = i;
            ImGui::PopID();
            y += hs[i];
        }
        ImGui::PopID();
        ImGui::SetCursorScreenPos(ImVec2(rf.p0.x, l1.y));
        ImGui::Dummy(ImVec2(1.f, sp(0.5f)));
        if (picked >= 0) {
            changed = picked != *idx;
            *idx = picked;
            g_dd_open = 0;
            st.refocus = true;
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !rf.hovered && !ImRect(l0, l1).Contains(ImGui::GetIO().MousePos)) {
            g_dd_open = 0;   // a click outside closes it
        }
    }
    return changed;
}

/* ---------------- stepper ---------------- */

/* In two-column sections (option_columns_begin with 2+ columns: a wide game Settings tab) Left / Right move between the columns, so a
 * stepper row there is changed in an "adjusting" state: A (or Enter) starts it, the control lights up, Left / Right step the value, and
 * A or B (Enter or Esc) ends it; moving the focus away ends it too. In one column Left / Right step the value as before. The mouse
 * arrows work the same everywhere. */
struct StepAdjust { ImGuiID id = 0; int frame = -10; };
StepAdjust g_adj;
int g_step_hint = 0, g_step_hint_frame = -10;   // 1 = a two-column stepper row has the focus, 2 = it is being adjusted (hint bars)

bool stepper_impl(const char* label, int* idx, int n, const std::function<std::string(int)>& item, const RowOpts& o) {
    ImGuiContext& g = *GImGui;
    if (n <= 0) return false;
    const int fc = ImGui::GetFrameCount();
    const int cur = std::max(0, std::min(*idx, n - 1));
    ImFont* fb = F.body;
    float maxw = 0.f;
    if (n <= 64) for (int i = 0; i < n; ++i) maxw = std::max(maxw, text_w(fb, item(i).c_str()));
    else for (int i : {0, n / 2, n - 1}) maxw = std::max(maxw, text_w(fb, item(i).c_str()));
    if (o.value_text) maxw = std::max(maxw, text_w(fb, o.value_text));
    const float zone = sp(4.f);
    CtlSpec cs;
    cs.chrome = zone * 2.f + sp(1.f);
    cs.natural = maxw + cs.chrome;
    cs.value = o.value_text ? std::string(o.value_text) : item(cur);
    cs.min_h = sp(4.5f);
    RowFrame rf;
    row_begin(label, o, cs, rf);
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    const std::string lname(label, ImGui::FindRenderedTextEnd(label));
    const bool cols = g_flow.on && g_flow.cols > 1 && g_flow.win == win;   // two columns: Left / Right belong to the columns
    bool adjusting = cols && o.enabled && g_adj.id == rf.id && g_adj.frame >= fc - 1;
    if (adjusting && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
        /* B / Esc ends the adjusting only: the page around (sheet, desk pane, dialog) must not also take it as "back" */
        ImGui::SetKeyOwner(ImGuiKey_GamepadFaceRight, rf.id, ImGuiInputFlags_LockThisFrame);
        ImGui::SetKeyOwner(ImGuiKey_Escape, rf.id, ImGuiInputFlags_LockThisFrame);
        ui_sound::play(ui_sound::Sfx::Back);
        adjusting = false;
        ImGui::SetFocusID(rf.id, win);   // (ImGui's own "back" moved the focus off the row this frame: put it back)
        shot_trace("stepper-adjust", "off-b " + lname);
    } else if (adjusting && g.NavId != rf.id) {   // the focus moved on (Up / Down, LT / RT, a click elsewhere)
        adjusting = false;
        shot_trace("stepper-adjust", "off-moved " + lname);
    }
    int nv = cur;
    if (o.enabled && g.NavId == rf.id) {
        const ImGuiDir md = pad_move_dir();
        if ((md == ImGuiDir_Left || md == ImGuiDir_Right) && (!cols || adjusting)) {   // Left / Right change the value instead of moving the focus
            ImGui::NavMoveRequestCancel();
            nv = hubui::kit::step_value(cur, 0, n - 1, 1, md == ImGuiDir_Left ? -1 : +1, false);
        }
    }
    if (rf.pressed && o.enabled) {
        const float mx = ImGui::GetIO().MousePos.x;
        if (rf.mouse && mx < rf.c0.x + zone && mx >= rf.c0.x - sp(1.f)) nv = hubui::kit::step_value(cur, 0, n - 1, 1, -1, false);
        else if (rf.mouse && mx > rf.c1.x - zone) nv = hubui::kit::step_value(cur, 0, n - 1, 1, +1, false);
        else if (cols && !rf.mouse) {   // A / Enter in two columns: start or end adjusting
            adjusting = !adjusting;
            shot_trace("stepper-adjust", std::string(adjusting ? "on " : "off-a ") + lname);
        } else nv = hubui::kit::step_value(cur, 0, n - 1, 1, +1, true);   // A / click on the value: next, wrapping
    }
    if (adjusting) g_adj = StepAdjust{rf.id, fc};
    else if (g_adj.id == rf.id) g_adj = StepAdjust();
    if (cols && rf.focused) { g_step_hint = adjusting ? 2 : 1; g_step_hint_frame = fc; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    control_box(dl, rf, adjusting);   // adjusting: lit like an open dropdown
    const float cy = (rf.c0.y + rf.c1.y) * 0.5f;
    const ImU32 on = adjusting ? al(ACCENT) : al(mix(TEXT2, TEXT, rf.hv)), off = al(with_alpha(TEXT3, 0.45f));
    icon(dl, Icon::ChevronL, ImVec2(rf.c0.x + zone * 0.5f, cy), sp(2.5f), cur > 0 ? on : off);
    icon(dl, Icon::ChevronR, ImVec2(rf.c1.x - zone * 0.5f, cy), sp(2.5f), cur < n - 1 ? on : off);
    if (nv != cur) {
        RowFrame shown = rf;   // draw the new value at once
        shown.vlines = wrap_lines(fb, fb->FontSize, item(nv), std::max(1.f, (rf.c1.x - rf.c0.x) - cs.chrome), 2);
        draw_value(dl, shown, rf.c0.x + zone, rf.c1.x - zone, 0.5f, al(TEXT));
    } else draw_value(dl, rf, rf.c0.x + zone, rf.c1.x - zone, 0.5f, al(TEXT));
    row_end();
    if (nv != cur) {
        if (!rf.pressed) ui_sound::play(ui_sound::Sfx::Select);
        *idx = nv;
        return true;
    }
    return false;
}

/* ---------------- sections ---------------- */

struct SecState { bool init = false, open = true, touched = false; };
std::unordered_map<ImGuiID, SecState> g_sec;
SectionStore g_store;

/* ---------------- tab bars ---------------- */

struct BarReg {
    ImGuiWindow* win = nullptr;
    int frame = -10;
    bool anywhere = false;
    float ux = -1.f, uw = 0.f;   // animated underline (relative to the bar's left edge)
};
std::unordered_map<ImGuiID, BarReg> g_bars;

/* May this bar take LB / RB? The innermost bar around the pad focus wins; an "anywhere" bar only when the focus is outside every bar
 * and no modal popup sits above it. */
int bar_rank(const BarReg& b) {
    ImGuiContext& g = *GImGui;
    if (g.NavWindow) {
        const int d = chain_depth(g.NavWindow, b.win);
        if (d >= 0) return d;
    }
    if (!b.anywhere) return -1;
    ImGuiWindow* top = ImGui::GetTopMostPopupModal();
    if (top && top != b.win->RootWindow) return -1;
    return 1000;
}
bool owns_shoulders(ImGuiID bid) {
    const int fc = ImGui::GetFrameCount();
    const int mine = bar_rank(g_bars[bid]);
    if (mine < 0) return false;
    for (auto& kv : g_bars) {
        if (kv.first == bid || kv.second.frame < fc - 1 || !kv.second.win) continue;
        const int r = bar_rank(kv.second);
        if (r >= 0 && (r < mine || (r == mine && kv.first < bid))) return false;
    }
    return true;
}

}  // namespace

/* ================= public ================= */

void option_group_begin(const char* id) { option_group_begin(id, GroupOpts()); }

void option_group_begin(const char* id, const GroupOpts& opts) {
    const int fc = ImGui::GetFrameCount();
    if ((fc & 255) == 0)   // forget groups unused for about a minute (pages that are gone)
        for (auto it = g_groups.begin(); it != g_groups.end();) it = it->second.frame < fc - 3600 ? g_groups.erase(it) : std::next(it);
    const ImGuiID gid = ImGui::GetID(id);
    GroupState& gs = g_groups[gid];
    gs.frame = fc;
    gs.help = opts.help;
    g_group = &gs;
    if (opts.help == HelpMode::Strip) {
        if (g_help.group != gid) {   // another page (another game): nothing carries over
            g_help = HelpTrack();
            g_help.group = gid;
        }
        g_help.live_prio = 0;
        g_help.has_first = false;
    }
}

void option_group_end() {
    if (!g_group) return;
    float l = 0.f, c = 0.f;
    for (auto& kv : g_group->rows) { l = std::max(l, kv.second.x); c = std::max(c, kv.second.y); }
    g_group->label_w = l;
    g_group->ctl_w = c;
    if (g_group->help == HelpMode::Strip) {
        g_help.frame = ImGui::GetFrameCount();
        if (g_help.live_prio > 0) { g_help.last = g_help.live; g_help.has_last = true; }
    }
    g_group = nullptr;
}

bool option_help(OptionHelp* out) {
    if (g_help.frame != ImGui::GetFrameCount()) return false;
    if (g_help.live_prio > 0) { *out = g_help.live; out->live = true; }
    else if (g_help.has_last) { *out = g_help.last; out->live = false; }
    else if (g_help.has_first) { *out = g_help.first; out->live = false; }
    else *out = OptionHelp();
    return true;
}

int option_help_lines(float w, StripStyle style, int max_lines) {
    const int mx = std::max(1, std::min(3, max_lines));
    if (std::fabs(w - g_help.lines_w) > 0.5f || g_help.lines_n != g_help.texts.size() || g_help.lines_style != (int)style || g_help.lines_max != mx) {
        int n = 1;
        const float ww = std::max(1.f, w);
        for (const auto& t : g_help.texts) {
            const int k = style == StripStyle::RunIn ? (int)wrap_after(F.cap, F.cap->FontSize, t.second, ww - run_in_label_w(t.first), ww, 3).size()
                                                     : (int)wrap_lines(F.cap, F.cap->FontSize, t.second, ww, 3).size();
            n = std::max(n, k);
        }
        g_help.lines = std::min(mx, n);
        g_help.lines_w = w;
        g_help.lines_n = g_help.texts.size();
        g_help.lines_style = (int)style;
        g_help.lines_max = mx;
    }
    return g_help.lines;
}

namespace {
float strip_head_h() { return std::max(F.bold->FontSize, F.cap->FontSize + sp(0.75f)); }   // the heading line also holds key hints
float run_in_pitch() { return F.cap->FontSize + sp(0.375f); }
}  // namespace

float option_help_strip_h(int lines, StripStyle style) {
    const int n = std::max(1, lines);
    if (style == StripStyle::RunIn) return F.bold->FontSize + (float)(n - 1) * run_in_pitch();
    return strip_head_h() + sp(0.5f) + (float)n * F.cap->FontSize + (float)(n - 1) * sp(0.25f);
}

void option_help_strip(const OptionHelp& h, float w, int lines, StripStyle style, float heading_w) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImFont* fb = F.bold;
    ImFont* fc = F.cap;
    const int n = std::max(1, lines);
    const float total = option_help_strip_h(n, style);
    w = std::max(1.f, w);
    std::vector<std::string> ls;
    float help_y0 = p.y;
    if (style == StripStyle::RunIn) {   // "Label  help that wraps on, ..." in one slim block
        float x = p.x;
        if (!h.label.empty()) {
            const std::string t = ellipsize(fb, fb->FontSize, h.label, w * 0.6f);
            dl->AddText(fb, fb->FontSize, p, TEXT, t.c_str());
            x += text_w(fb, t.c_str()) + sp(1.f);
            if (!h.note.empty() && x + text_w(fc, h.note.c_str()) + sp(2.5f) <= p.x + w * 0.8f) {   // the second pill the row had no room for
                x += pill_at(dl, ImVec2(x, p.y + (fb->FontSize - fc->FontSize - sp(1.f)) * 0.5f), h.note.c_str(), PANEL3, TEXT2, fc) + sp(1.f);
            }
        }
        if (!h.help.empty()) {
            ls = wrap_after(fc, fc->FontSize, h.help, p.x + w - x, w, n);
            const float base = (fb->FontSize - fc->FontSize) * 0.8f;   // the first line sits on the label's baseline
            for (size_t i = 0; i < ls.size(); ++i) {
                const float y = i == 0 ? p.y + base : p.y + fb->FontSize + sp(0.375f) + (float)(i - 1) * run_in_pitch();
                dl->AddText(fc, fc->FontSize, ImVec2(i == 0 ? x : p.x, y), TEXT2, ls[i].c_str());
            }
        }
    } else {
        const float head = strip_head_h(), hw = heading_w > 0.f ? std::min(w, heading_w) : w;
        if (!h.label.empty()) {
            const std::string t = ellipsize(fb, fb->FontSize, h.label, hw);
            dl->AddText(fb, fb->FontSize, ImVec2(p.x, p.y + (head - fb->FontSize) * 0.5f), TEXT, t.c_str());
            const float x = p.x + text_w(fb, t.c_str()) + sp(1.f);
            if (!h.note.empty() && x + text_w(fc, h.note.c_str()) + sp(2.5f) <= p.x + hw)   // the second pill the compact row had no room for
                pill_at(dl, ImVec2(x, p.y + (head - fc->FontSize - sp(1.f)) * 0.5f), h.note.c_str(), PANEL3, TEXT2, fc);
        }
        help_y0 = p.y + head;
        if (!h.help.empty()) {
            ls = wrap_lines(fc, fc->FontSize, h.help, w, n);
            float y = p.y + head + sp(0.5f);
            for (const std::string& l : ls) {
                dl->AddText(fc, fc->FontSize, ImVec2(p.x, y), TEXT2, l.c_str());
                y += fc->FontSize + sp(0.25f);
            }
        }
    }
    const bool cut = !ls.empty() && ls.back().size() >= 3 && ls.back().compare(ls.back().size() - 3, 3, "...") == 0 &&
                     !(h.help.size() >= 3 && h.help.compare(h.help.size() - 3, 3, "...") == 0);
    if (cut && ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(ImVec2(p.x, help_y0), ImVec2(p.x + w, p.y + total))) ImGui::SetTooltip("%s", h.help.c_str());
    ImGui::Dummy(ImVec2(w, total));
}

/* ---------------- columns of sections ---------------- */

void option_columns_begin(int cols, float col_w, float gap) {
    ImGuiContext& g = *GImGui;
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    Flow& f = g_flow;
    if (f.on) option_columns_end();   // (a missing end)
    const int fc = ImGui::GetFrameCount();
    f.on = true;
    f.win = win;
    f.cols = std::max(1, cols);
    f.cur = 0;
    f.col_w = std::max(1.f, col_w);
    f.gap = std::max(0.f, gap);
    const ImVec2 o = ImGui::GetCursorScreenPos();
    f.x0 = o.x;
    f.y0 = o.y;
    f.bottom = o.y;
    f.work_max = win->WorkRect.Max.x;
    f.content_max = win->ContentRegionRect.Max.x;
    f.items.assign((size_t)f.cols, std::vector<FlowItem>());
    f.nonav = false;
    /* the column the focus is in: its rect (last frame's, scroll-proof) inside last frame's columns */
    f.fcol = -1;
    if (g.NavWindow == win && g.NavId && g.NavLayer == ImGuiNavLayer_Main && f.prev_win == win->ID && f.prev_frame >= fc - 1) {
        const ImRect nr = win->NavRectRel[ImGuiNavLayer_Main];
        const ImVec2 base = win->DC.CursorStartPos;
        const float cx = (nr.Min.x + nr.Max.x) * 0.5f, cy = (nr.Min.y + nr.Max.y) * 0.5f;
        if (!nr.IsInverted() && cy >= f.prev_rel.Min.y && cy <= f.prev_rel.Max.y) {
            const float rx = cx + base.x - f.x0;   // from the left edge of the first column, this frame
            if (rx >= -1.f && rx <= f.cols * f.col_w + (f.cols - 1) * f.gap + 1.f)
                f.fcol = std::max(0, std::min(f.cols - 1, (int)std::floor(rx / (f.col_w + f.gap))));
        }
    }
    ImGuiDir dir = ImGuiDir_None;
    f.vertical = f.cols > 1 && f.fcol >= 0 && flow_move(&dir) && (dir == ImGuiDir_Up || dir == ImGuiDir_Down);
    flow_column_start(0);
}

void option_columns_next() {
    Flow& f = g_flow;
    if (!f.on || f.win != ImGui::GetCurrentWindow()) return;
    flow_column_end();
    if (f.cur + 1 < f.cols) flow_column_start(f.cur + 1);
    else flow_column_start(f.cur);   // (more columns than asked for: the last one goes on)
}

void option_columns_end() {
    ImGuiContext& g = *GImGui;
    Flow& f = g_flow;
    if (!f.on) return;
    ImGuiWindow* win = f.win;
    flow_column_end();
    win->WorkRect.Max.x = f.work_max;
    win->ContentRegionRect.Max.x = f.content_max;
    f.on = false;
    /* Left / Right: the row beside in the next column when ImGui found nothing there (the columns' rows do not line up) */
    ImGuiDir dir = ImGuiDir_None;
    if (f.cols > 1 && f.fcol >= 0 && g.NavWindow == win && flow_move(&dir) && !(g.NavMoveFlags & ImGuiNavMoveFlags_IsPageMove) &&
        (dir == ImGuiDir_Left || dir == ImGuiDir_Right)) {
        const int tc = f.fcol + (dir == ImGuiDir_Right ? 1 : -1);
        if (tc >= 0 && tc < f.cols && !f.items[(size_t)tc].empty()) {
            const float tx0 = f.x0 + (float)tc * (f.col_w + f.gap), tx1 = tx0 + f.col_w;
            auto in_target = [&](const ImGuiNavItemData& r) {
                if (!r.ID || r.Window != win) return false;
                const ImRect a = ImGui::WindowRectRelToAbs(win, r.RectRel);
                const float cx = (a.Min.x + a.Max.x) * 0.5f;
                return cx >= tx0 && cx <= tx1;
            };
            if (!in_target(g.NavMoveResultLocal) && !in_target(g.NavMoveResultLocalVisible)) {
                const ImRect nr = ImGui::WindowRectRelToAbs(win, win->NavRectRel[ImGuiNavLayer_Main]);
                const float cy = (nr.Min.y + nr.Max.y) * 0.5f;
                const FlowItem* best = nullptr;
                float bd = FLT_MAX;
                for (const FlowItem& it : f.items[(size_t)tc]) {
                    const float d = std::fabs((it.r.Min.y + it.r.Max.y) * 0.5f - cy);
                    if (d < bd) { bd = d; best = &it; }
                }
                if (best) {
                    ImGui::NavMoveRequestCancel();
                    ImGui::SetNavID(best->id, ImGuiNavLayer_Main, best->scope, ImGui::WindowRectAbsToRel(win, best->r));
                    g.NavCursorVisible = true;
                    g.NavHighlightItemUnderNav = true;
                }
            }
        }
    }
    /* where the next frame finds the columns (relative to the content start: scrolling does not move it) */
    const ImVec2 base = win->DC.CursorStartPos;
    f.prev_win = win->ID;
    f.prev_frame = ImGui::GetFrameCount();
    const float x1 = f.x0 + f.cols * f.col_w + (f.cols - 1) * f.gap;
    f.prev_rel = ImRect(ImVec2(f.x0 - base.x, f.y0 - base.y), ImVec2(x1 - base.x, f.bottom - base.y));
    ImGui::SetCursorScreenPos(ImVec2(f.x0, f.bottom));
    ImGui::Dummy(ImVec2(0.f, 0.f));   // a submitted item: the scroll area counts the taller column
}

bool dropdown(const char* label, int* idx, const char* const* items, int n, const RowOpts& opts) {
    return dropdown_impl(label, idx, n, [&](int i) { return items[i] ? items[i] : ""; }, opts);
}
bool dropdown(const char* label, int* idx, const std::vector<std::string>& items, const RowOpts& opts) {
    return dropdown_impl(label, idx, (int)items.size(), [&](int i) { return items[i].c_str(); }, opts);
}

bool stepper_adjusting() { return g_adj.id != 0 && g_adj.frame >= ImGui::GetFrameCount() - 1; }
int stepper_pad_state() { return g_step_hint_frame >= ImGui::GetFrameCount() - 1 ? g_step_hint : 0; }

bool dropdown_list_open() {
    if (!g_dd_open) return false;
    auto it = g_dd.find(g_dd_open);
    return it != g_dd.end() && it->second.frame >= ImGui::GetFrameCount() - 1;
}

bool toggle(const char* label, bool* v, const RowOpts& opts) {
    ImFont* fb = F.body;
    const float tw = sp(5.5f), th = sp(3.f);
    CtlSpec cs;
    cs.natural = std::max(text_w(fb, "On"), text_w(fb, "Off")) + sp(1.25f) + tw;
    cs.min_h = sp(4.5f);
    RowFrame rf;
    row_begin(label, opts, cs, rf);
    bool changed = false;
    if (rf.pressed && opts.enabled) { *v = !*v; changed = true; }
    float* kt = ImGui::GetStateStorage()->GetFloatRef(rf.id ^ 0x7E57u, *v ? 1.f : 0.f);
    anim(*kt, *v ? 1.f : 0.f, 18.f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cy = (rf.c0.y + rf.c1.y) * 0.5f;
    const ImVec2 t0(rf.c1.x - tw, cy - th * 0.5f), t1(rf.c1.x, cy + th * 0.5f);
    const ImU32 track = mix(PANEL3, mix(ACCENT, IM_COL32_WHITE, 0.12f * rf.hv), *kt);
    dl->AddRectFilled(t0, t1, al(track), th * 0.5f);
    if (*kt < 0.99f) dl->AddRect(t0, t1, al(scale_alpha(HAIRLINE, 1.f - *kt)), th * 0.5f, 0, 1.f);
    const float kr = th * 0.5f - sp(0.375f);
    const ImVec2 kc(t0.x + th * 0.5f + (tw - th) * *kt, cy);
    dl->AddCircleFilled(kc, kr, al(mix(TEXT2, IM_COL32_WHITE, *kt)), 20);
    const char* st = *v ? "On" : "Off";
    dl->AddText(fb, fb->FontSize, ImVec2(t0.x - sp(1.25f) - text_w(fb, st), cy - fb->FontSize * 0.5f), al(*v ? TEXT : TEXT2), st);
    row_end();
    return changed;
}

bool stepper(const char* label, int* idx, const char* const* items, int n, const RowOpts& opts) {
    return stepper_impl(label, idx, n, [&](int i) { return std::string(items[i] ? items[i] : ""); }, opts);
}

bool stepper_int(const char* label, int* v, int lo, int hi, int step, const char* fmt, const RowOpts& opts) {
    if (hi < lo) std::swap(lo, hi);
    if (step <= 0) step = 1;
    const int n = (hi - lo) / step + 1;
    int idx = (std::max(lo, std::min(*v, hi)) - lo) / step;
    auto item = [&](int i) {
        char b[64];
        std::snprintf(b, sizeof b, fmt ? fmt : "%d", lo + i * step);
        return std::string(b);
    };
    if (!stepper_impl(label, &idx, n, item, opts)) return false;
    *v = lo + idx * step;
    return true;
}

bool section(const char* label, const SectionOpts& o) {
    ImGuiContext& g = *GImGui;
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    const ImGuiID id = win->GetID(label);
    SecState& st = g_sec[id];
    if (!st.init) {
        st.init = true;
        st.open = o.default_open;
    }
    if (o.persist_key && g_store.load) {   // a remembered section follows the store every frame: one key looks the same on every page
        const int s = g_store.load(o.persist_key);
        if (s >= 0) st.open = s == 1;
        else if (!st.touched) st.open = o.default_open;
    }
    const bool compact = compact_rows();   // inside a Strip group: a lower header, like its rows
    const bool first_in_column = g_flow.on && g_flow.win == win && std::fabs(win->DC.CursorPos.y - g_flow.y0) < 1.f;
    if (win->DC.CursorPos.y > win->DC.CursorStartPos.y + 1.f && !first_in_column)
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (compact ? sp(0.5f) : sp(1.f)));   // air above every section but the first
    const char* lend = ImGui::FindRenderedTextEnd(label);
    const float h = compact ? sp(4.5f) : sp(5.5f);
    const float avail = std::max(1.f, std::min(ImGui::GetContentRegionAvail().x, max_row_w()));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, compact ? sp(0.5f) : sp(0.75f)));
    const bool pressed = ImGui::InvisibleButton(label, ImVec2(avail, h), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleVar();
    flow_record(id, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    const bool hov = ImGui::IsItemHovered();
    const bool focus = ImGui::IsItemFocused() && nav_visible();
    item_sound(focus, pressed);
    if (compact && (focus || hov)) {   // the help strip names the header that has the focus (it has no help of its own)
        OptionHelp sh;
        sh.label.assign(label, lend);
        help_track(sh, focus ? 2 : 1);
    }
    bool toggle_now = pressed;
    if (g.NavId == id) {   // Left closes an open section, Right opens a closed one (otherwise the D-pad moves on as usual)
        const ImGuiDir md = pad_move_dir();
        if ((md == ImGuiDir_Left && st.open) || (md == ImGuiDir_Right && !st.open)) {
            ImGui::NavMoveRequestCancel();
            toggle_now = true;
            ui_sound::play(ui_sound::Sfx::Select);
        }
    }
    if (toggle_now) {
        st.open = !st.open;
        st.touched = true;
        if (o.persist_key && g_store.save) g_store.save(o.persist_key, st.open);
    }
    float* hv = ImGui::GetStateStorage()->GetFloatRef(id, 0.f);
    anim(*hv, (hov || focus) ? 1.f : 0.f, 16.f);
    float* rot = ImGui::GetStateStorage()->GetFloatRef(id ^ 0x5EC7u, st.open ? 1.f : 0.f);
    anim(*rot, st.open ? 1.f : 0.f, 18.f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    const float rd = sp(1.25f);
    if (*hv > 0.01f) dl->AddRectFilled(p0, p1, veil(0.045f * *hv), rd);
    dl->AddRectFilled(ImVec2(p0.x, std::floor(p1.y) - 1.f), ImVec2(p1.x, std::floor(p1.y)), HAIRLINE);
    const float cy = (p0.y + p1.y) * 0.5f;
    chevron(dl, ImVec2(p0.x + sp(2.5f), cy), sp(2.f), *rot * kPi * 0.5f, mix(TEXT2, TEXT, *hv));
    ImFont* fbold = F.bold;
    dl->AddText(fbold, fbold->FontSize, ImVec2(p0.x + sp(5.f), cy - fbold->FontSize * 0.5f), TEXT, label, lend);
    if (o.summary && *o.summary) {
        ImFont* fc = F.cap;
        const float sw = text_w(fc, o.summary);
        const float lx = p0.x + sp(5.f) + text_w(fbold, label, lend) + sp(2.f);
        const float sx = std::max(lx, p1.x - sp(1.5f) - sw);
        dl->PushClipRect(ImVec2(lx, p0.y), p1, true);
        dl->AddText(fc, fc->FontSize, ImVec2(sx, cy - fc->FontSize * 0.5f), TEXT2, o.summary);
        dl->PopClipRect();
    }
    if (focus) focus_ring(dl, p0 + ImVec2(sp(0.25f), sp(0.25f)), p1 - ImVec2(sp(0.25f), sp(0.25f)), rd);
    return st.open;
}

void section_set_store(const SectionStore& store) { g_store = store; }

bool tab_bar(const char* id, const char* const* labels, int n, int* cur, const TabBarOpts& o) {
    ImGuiContext& g = *GImGui;
    if (n <= 0) return false;
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    ImGui::PushID(id);
    const ImGuiID bid = win->GetID("##bar");
    const int fc = ImGui::GetFrameCount();
    BarReg& reg = g_bars[bid];
    reg.win = win;
    reg.frame = fc;
    reg.anywhere = o.shoulders_anywhere;
    int sel = std::max(0, std::min(*cur, n - 1));
    bool changed = false, focus_sel = false;
    if (n > 1 && !g.IO.WantTextInput) {
        ImGuiKey used = ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) ? ImGuiKey_GamepadL1 : ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) ? ImGuiKey_GamepadR1 : ImGuiKey_None;
        if (used == ImGuiKey_None && !g.IO.KeyCtrl && !g.IO.KeyAlt)   // [ and ] on a keyboard, as LB / RB (the Cases view's keys)
            used = ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false) ? ImGuiKey_LeftBracket : ImGui::IsKeyPressed(ImGuiKey_RightBracket, false) ? ImGuiKey_RightBracket : ImGuiKey_None;
        const int dir = (used == ImGuiKey_GamepadL1 || used == ImGuiKey_LeftBracket) ? -1 : used != ImGuiKey_None ? +1 : 0;
        if (dir && owns_shoulders(bid)) {
            ImGui::SetKeyOwner(used, bid, ImGuiInputFlags_LockThisFrame);   // the hub's global LB / RB page switch reads the same press
            const int t = hubui::kit::tab_step(sel, n, dir, o.wrap);
            if (t >= 0) {
                sel = t;
                changed = true;
                focus_sel = true;
                ui_sound::play(ui_sound::Sfx::Move);
            }
        }
    }

    ImFont* fb = F.bold;
    const float fs = fb->FontSize, h = o.height > 0.f ? std::max(fs + sp(1.f), o.height) : sp(5.f);
    const bool hints = o.show_hints && A.pad_mode && n > 1;
    const float hint_w = hints ? key_hint_width(true, "LB", "", "") : 0.f;
    const float avail = std::max(1.f, ImGui::GetContentRegionAvail().x);
    float padx = sp(2.f);
    std::vector<float> tw(n);
    auto total = [&]() {
        float t = hints ? 2.f * (hint_w + sp(1.f)) : 0.f;
        for (int i = 0; i < n; ++i) t += tw[i] + padx * 2.f;
        return t;
    };
    for (int i = 0; i < n; ++i) tw[i] = text_w(fb, labels[i], ImGui::FindRenderedTextEnd(labels[i]));
    if (total() > avail) padx = sp(1.f);
    std::vector<std::string> shown(n);
    for (int i = 0; i < n; ++i) shown[i].assign(labels[i], ImGui::FindRenderedTextEnd(labels[i]));
    if (total() > avail) {   // still too wide: every tab gets an equal share and long labels are shortened
        const float share = std::max(sp(4.f), (avail - (hints ? 2.f * (hint_w + sp(1.f)) : 0.f)) / n - padx * 2.f);
        for (int i = 0; i < n; ++i)
            if (tw[i] > share) { shown[i] = ellipsize(fb, fs, shown[i], share); tw[i] = text_w(fb, shown[i].c_str()); }
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o0 = ImGui::GetCursorScreenPos();
    float x = o0.x;
    const float gh = F.cap->FontSize + sp(0.75f);
    if (hints) {
        key_hint(dl, ImVec2(x, o0.y + (h - gh) * 0.5f), true, "LB", "", "");
        x += hint_w + sp(1.f);
    }
    float sel_x = 0.f, sel_w = 0.f;
    for (int i = 0; i < n; ++i) {
        const float w = tw[i] + padx * 2.f;
        ImGui::SetCursorScreenPos(ImVec2(x, o0.y));
        ImGui::PushID(i);
        const bool pr = ImGui::InvisibleButton("##tab", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
        if (focus_sel && i == sel) ImGui::SetFocusID(ImGui::GetItemID(), win);
        const bool hov = ImGui::IsItemHovered();
        const bool focus = ImGui::IsItemFocused() && nav_visible();
        item_sound(focus, pr);
        if (pr && i != sel) { sel = i; changed = true; }
        float* hv = ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID(), 0.f);
        anim(*hv, (hov || focus) ? 1.f : 0.f, 16.f);
        const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
        if (*hv > 0.01f) dl->AddRectFilled(p0, ImVec2(p1.x, p1.y - sp(0.5f)), veil(0.05f * *hv), sp(1.f));
        dl->AddText(fb, fs, ImVec2(p0.x + padx, (p0.y + p1.y - fs) * 0.5f - sp(0.25f)), i == sel ? TEXT : mix(TEXT2, TEXT, *hv), shown[i].c_str());
        if (focus) focus_ring(dl, p0 + ImVec2(sp(0.25f), sp(0.25f)), p1 - ImVec2(sp(0.25f), sp(0.75f)), sp(1.f));
        if (i == sel) { sel_x = p0.x - o0.x; sel_w = w; }
        ImGui::PopID();
        x += w;
    }
    if (hints) key_hint(dl, ImVec2(x + sp(1.f), o0.y + (h - gh) * 0.5f), true, "RB", "", "");
    /* baseline + the selected tab's underline (slides to the new tab; snaps under Reduce motion) */
    const float by = std::floor(o0.y + h) - 1.f;   // a 1 px rect on a whole pixel row: crisp on every renderer
    dl->AddRectFilled(ImVec2(o0.x, by), ImVec2(o0.x + avail, by + 1.f), HAIRLINE);
    detail::tabbar_window = win->ID;
    detail::tabbar_frame = fc;
    detail::tabbar_bottom = o0.y + h + sp(0.25f);
    if (reg.ux < 0.f) { reg.ux = sel_x; reg.uw = sel_w; }
    anim(reg.ux, sel_x, 18.f);
    anim(reg.uw, sel_w, 18.f);
    const float inset = std::min(sp(1.5f), reg.uw * 0.2f);
    dl->AddRectFilled(ImVec2(o0.x + reg.ux + inset, o0.y + h - sp(0.375f)), ImVec2(o0.x + reg.ux + reg.uw - inset, o0.y + h + sp(0.125f)), ACCENT, sp(0.25f));
    ImGui::SetCursorScreenPos(o0);
    ImGui::Dummy(ImVec2(avail, h + sp(0.25f)));
    ImGui::PopID();
    *cur = sel;
    return changed;
}

float button_width(const char* label, Icon ic) {
    const char* end = ImGui::FindRenderedTextEnd(label);
    const float tx = ImGui::CalcTextSize(label, end).x;
    const bool has_text = end > label;
    return tx + (ic != Icon::None ? sp(2.25f) : 0.f) + ((ic != Icon::None && has_text) ? sp(1.f) : 0.f) + sp(2.5f) * 2.f;
}

FlowRow::FlowRow(float gap) : avail_(ImGui::GetContentRegionAvail().x), gap_(gap >= 0.f ? gap : sp(1.5f)) {}

void FlowRow::next(float w) {
    if (!started_) { started_ = true; used_ = w; return; }
    if (used_ + gap_ + w <= avail_) { ImGui::SameLine(0.f, gap_); used_ += gap_ + w; }
    else used_ = w;   // no SameLine: this item starts a new line
}

}  // namespace ui
