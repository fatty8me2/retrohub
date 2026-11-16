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
};
std::unordered_map<ImGuiID, GroupState> g_groups;
GroupState* g_group = nullptr;

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
};

void row_begin(const char* label, const RowOpts& o, const CtlSpec& cs, RowFrame& rf) {
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    ImFont* fb = F.body;
    ImFont* fc = F.cap;
    const float fs = fb->FontSize, cfs = fc->FontSize;
    const char* lend = ImGui::FindRenderedTextEnd(label);
    const std::string text(label, lend);
    rf.id = win->GetID(label);

    const float pill_gap = sp(1.f);
    const float pill_w = o.pill ? text_w(fc, o.pill) + sp(2.5f) : 0.f;
    const float pill_h = cfs + sp(1.f);
    const float label_tw = text_w(fb, text.c_str());
    const float label_need = label_tw + (o.pill ? pill_gap + pill_w : 0.f);
    float L = label_need, C = cs.natural;
    if (g_group) {
        g_group->rows[rf.id] = ImVec2(label_need, cs.natural);
        L = std::max(L, g_group->label_w);
        C = std::max(C, g_group->ctl_w);
    }
    const float padx = sp(1.25f), pady = sp(1.f), gl = sp(0.25f);
    const float avail = std::max(1.f, std::min(ImGui::GetContentRegionAvail().x, max_row_w()));
    hubui::kit::RowIn in;
    in.avail = std::max(0.f, avail - 2.f * padx);
    in.label_w = L;
    in.ctl_w = C;
    in.ctl_min = sp(22.f);
    in.label_min = sp(20.f);
    in.gap = sp(2.f);
    const hubui::kit::RowOut lay = hubui::kit::option_columns(in);

    const float lw = std::max(1.f, lay.label_w);
    const std::vector<std::string> llines = wrap_lines(fb, fs, text, lw, 3);
    const bool pill_inline = o.pill && llines.size() == 1 && text_w(fb, llines[0].c_str()) + pill_gap + pill_w <= lw + 0.5f;
    std::vector<std::string> hlines;
    if (o.help && *o.help) hlines = wrap_lines(fc, cfs, o.help, lw, 3);
    float text_h = llines.size() * fs + (llines.size() - 1) * gl;
    if (o.pill && !pill_inline) text_h += sp(0.5f) + pill_h;
    if (!hlines.empty()) text_h += sp(0.5f) + hlines.size() * cfs + (hlines.size() - 1) * gl;

    const float ctl_w = lay.ctl_w;
    if (!cs.value.empty()) rf.vlines = wrap_lines(fb, fs, cs.value, std::max(1.f, ctl_w - cs.chrome), 2);
    const float ctl_h = std::max(cs.min_h, rf.vlines.size() > 1 ? rf.vlines.size() * fs + gl * (rf.vlines.size() - 1) + sp(2.f) : cs.min_h);
    const float h = lay.wrap ? pady * 2.f + text_h + sp(1.f) + ctl_h : pady * 2.f + std::max(text_h, ctl_h);

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

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rd = sp(1.25f);
    if (rf.hv > 0.01f) dl->AddRectFilled(rf.p0, rf.p1, al(veil(0.045f * rf.hv)), rd);
    if (rf.focused) focus_ring(dl, rf.p0 + ImVec2(sp(0.25f), sp(0.25f)), rf.p1 - ImVec2(sp(0.25f), sp(0.25f)), rd);

    /* label, pill, help */
    const float tx = rf.p0.x + padx;
    float ty = lay.wrap ? rf.p0.y + pady : rf.p0.y + (h - text_h) * 0.5f;
    const ImU32 tcol = al(TEXT);
    for (size_t i = 0; i < llines.size(); ++i) {
        dl->AddText(fb, fs, ImVec2(tx, ty), tcol, llines[i].c_str());
        if (i == 0 && pill_inline) {
            const float px = tx + text_w(fb, llines[0].c_str()) + pill_gap;
            pill_at(dl, ImVec2(px, ty + (fs - pill_h) * 0.5f), o.pill, al(o.pill_col ? o.pill_col : ACCENT), al(IM_COL32_WHITE), fc);
        }
        ty += fs + gl;
    }
    ty -= gl;
    if (o.pill && !pill_inline) {
        ty += sp(0.5f);
        pill_at(dl, ImVec2(tx, ty), o.pill, al(o.pill_col ? o.pill_col : ACCENT), al(IM_COL32_WHITE), fc);
        ty += pill_h;
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
    rf.c0.y = lay.wrap ? rf.p0.y + pady + text_h + sp(1.f) : rf.p0.y + (h - ctl_h) * 0.5f;
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
    CtlSpec cs;
    cs.chrome = sp(2.f) + sp(4.5f);   // left padding + chevron area
    cs.natural = maxw + cs.chrome;
    cs.value = cur >= 0 ? item(cur) : "";
    cs.min_h = sp(4.5f);
    RowFrame rf;
    row_begin(label, o, cs, rf);
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    DdState& st = g_dd[rf.id];
    bool open = g_dd_open == rf.id && st.frame >= fc - 1;
    if (!open && g_dd_open == rf.id) g_dd_open = 0;
    st.frame = fc;
    if (st.refocus) { st.refocus = false; ImGui::SetFocusID(rf.id, win); }
    bool changed = false;
    const bool in_list = st.list_focus_frame >= fc - 1;
    const bool back = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (open && back && (g.NavId == rf.id || in_list)) {   // B / Esc closes only the list
        ImGui::SetKeyOwner(ImGuiKey_GamepadFaceRight, rf.id, ImGuiInputFlags_LockThisFrame);   // the sheet / dialog / page "back" reads the same press
        ImGui::SetKeyOwner(ImGuiKey_Escape, rf.id, ImGuiInputFlags_LockThisFrame);
        ui_sound::play(ui_sound::Sfx::Back);
        open = false;
        g_dd_open = 0;
        ImGui::SetFocusID(rf.id, win);
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

bool stepper_impl(const char* label, int* idx, int n, const std::function<std::string(int)>& item, const RowOpts& o) {
    ImGuiContext& g = *GImGui;
    if (n <= 0) return false;
    const int cur = std::max(0, std::min(*idx, n - 1));
    ImFont* fb = F.body;
    float maxw = 0.f;
    if (n <= 64) for (int i = 0; i < n; ++i) maxw = std::max(maxw, text_w(fb, item(i).c_str()));
    else for (int i : {0, n / 2, n - 1}) maxw = std::max(maxw, text_w(fb, item(i).c_str()));
    const float zone = sp(4.f);
    CtlSpec cs;
    cs.chrome = zone * 2.f + sp(1.f);
    cs.natural = maxw + cs.chrome;
    cs.value = item(cur);
    cs.min_h = sp(4.5f);
    RowFrame rf;
    row_begin(label, o, cs, rf);
    int nv = cur;
    if (o.enabled && g.NavId == rf.id) {
        const ImGuiDir md = pad_move_dir();
        if (md == ImGuiDir_Left || md == ImGuiDir_Right) {   // Left / Right change the value instead of moving the focus
            ImGui::NavMoveRequestCancel();
            nv = hubui::kit::step_value(cur, 0, n - 1, 1, md == ImGuiDir_Left ? -1 : +1, false);
        }
    }
    if (rf.pressed && o.enabled) {
        const float mx = ImGui::GetIO().MousePos.x;
        if (rf.mouse && mx < rf.c0.x + zone && mx >= rf.c0.x - sp(1.f)) nv = hubui::kit::step_value(cur, 0, n - 1, 1, -1, false);
        else if (rf.mouse && mx > rf.c1.x - zone) nv = hubui::kit::step_value(cur, 0, n - 1, 1, +1, false);
        else nv = hubui::kit::step_value(cur, 0, n - 1, 1, +1, true);   // A / click on the value: next, wrapping
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    control_box(dl, rf, false);
    const float cy = (rf.c0.y + rf.c1.y) * 0.5f;
    const ImU32 on = al(mix(TEXT2, TEXT, rf.hv)), off = al(with_alpha(TEXT3, 0.45f));
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

struct SecState { bool init = false, open = true; };
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

void option_group_begin(const char* id) {
    const int fc = ImGui::GetFrameCount();
    if ((fc & 255) == 0)   // forget groups unused for about a minute (pages that are gone)
        for (auto it = g_groups.begin(); it != g_groups.end();) it = it->second.frame < fc - 3600 ? g_groups.erase(it) : std::next(it);
    GroupState& gs = g_groups[ImGui::GetID(id)];
    gs.frame = fc;
    g_group = &gs;
}

void option_group_end() {
    if (!g_group) return;
    float l = 0.f, c = 0.f;
    for (auto& kv : g_group->rows) { l = std::max(l, kv.second.x); c = std::max(c, kv.second.y); }
    g_group->label_w = l;
    g_group->ctl_w = c;
    g_group = nullptr;
}

bool dropdown(const char* label, int* idx, const char* const* items, int n, const RowOpts& opts) {
    return dropdown_impl(label, idx, n, [&](int i) { return items[i] ? items[i] : ""; }, opts);
}
bool dropdown(const char* label, int* idx, const std::vector<std::string>& items, const RowOpts& opts) {
    return dropdown_impl(label, idx, (int)items.size(), [&](int i) { return items[i].c_str(); }, opts);
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
        if (o.persist_key && g_store.load) {
            const int s = g_store.load(o.persist_key);
            if (s >= 0) st.open = s == 1;
        }
    }
    if (win->DC.CursorPos.y > win->DC.CursorStartPos.y + 1.f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + sp(1.f));   // air above every section but the first
    const char* lend = ImGui::FindRenderedTextEnd(label);
    const float h = sp(5.5f);
    const float avail = std::max(1.f, std::min(ImGui::GetContentRegionAvail().x, max_row_w()));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, sp(0.75f)));
    const bool pressed = ImGui::InvisibleButton(label, ImVec2(avail, h), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleVar();
    const bool hov = ImGui::IsItemHovered();
    const bool focus = ImGui::IsItemFocused() && nav_visible();
    item_sound(focus, pressed);
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
        const int dir = ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) ? -1 : (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) ? +1 : 0);
        if (dir && owns_shoulders(bid)) {
            ImGui::SetKeyOwner(dir < 0 ? ImGuiKey_GamepadL1 : ImGuiKey_GamepadR1, bid, ImGuiInputFlags_LockThisFrame);   // the hub's global LB / RB page switch reads the same press
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
    const float fs = fb->FontSize, h = sp(5.f);
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
