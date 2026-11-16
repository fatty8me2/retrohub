#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui_scroll.h"

#include "imgui_internal.h"
#include "ui_kit.h"
#include "ui_layout_logic.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <unordered_map>
#include <vector>

/* UI kit scroll area (see ui_scroll.h). Native ImGui scrolling (wheel, drag, ScrollMax), with:
 *   - ImGui's own scrollbar kept for interaction but drawn transparent; a themed thumb is drawn over the same geometry;
 *   - AlwaysVerticalScrollbar so the gutter is always reserved (no reflow when overflow toggles);
 *   - focus-follow with a margin: ImGui's own nav scroll (ScrollToRectEx, edge + ItemSpacing) is overridden before the child begins;
 *   - LT / RT paging: the press is locked (LockThisFrame) so main.cpp's global paging, which reads the same press before the next
 *     NewFrame, skips it; the focus is moved with a nav request scored from the edge of the new view. */

namespace ui {
namespace {

struct Area {
    ImGuiID win_id = 0;
    int frame = -10;               // last frame drawn
    ScrollFlags flags = 0;
    ImGuiID last_nav = 0;          // pad focus seen inside the area last frame (0 = elsewhere)
    int page_follow_frame = -10;   // the focus was moved by our own page: that move keeps no margin
};
struct Frame {
    ImGuiID key = 0;
    int page_dir = 0;
    bool page_scrolled = false;
};

std::unordered_map<ImGuiID, Area> g_areas;          // key: ImGui::GetID(id) in the host window
std::unordered_map<ImGuiID, ImGuiID> g_by_window;   // child window id -> key
std::vector<Frame> g_stack;
int g_click_frame = -10;                            // last frame a mouse button was clicked (a click moves the focus too: no follow)

bool is_child(const ImGuiWindow* w) { return (w->Flags & ImGuiWindowFlags_ChildWindow) && !(w->Flags & ImGuiWindowFlags_Popup); }

/* Is `target` `from` or one of its parents, without crossing a popup / root boundary? */
bool in_chain(ImGuiWindow* from, ImGuiWindow* target) {
    for (ImGuiWindow* w = from; w; w = is_child(w) ? w->ParentWindow : nullptr)
        if (w == target) return true;
    return false;
}

/* LT / RT go to the innermost live kit area around the pad focus; failing that, to a PadOwner area whose host holds the focus. */
bool is_pager(ImGuiWindow* me, const Area& a) {
    ImGuiContext& g = *GImGui;
    ImGuiWindow* nav = g.NavWindow;
    if (!nav || g.NavLayer != ImGuiNavLayer_Main) return false;
    const int fc = ImGui::GetFrameCount();
    for (ImGuiWindow* w = nav; w; w = is_child(w) ? w->ParentWindow : nullptr) {
        auto it = g_by_window.find(w->ID);
        if (it == g_by_window.end()) continue;
        auto ar = g_areas.find(it->second);
        if (ar != g_areas.end() && ar->second.frame >= fc - 1) return w == me;
    }
    return (a.flags & ScrollFlags_PadOwner) && me->ParentWindow && in_chain(nav, me->ParentWindow);
}

void focus_follow(ImGuiWindow* w, Area& a, int fc) {
    ImGuiContext& g = *GImGui;
    ImGuiWindow* nav = g.NavWindow;
    const ImGuiID nid = g.NavId;
    if (!nid || !nav || g.NavLayer != ImGuiNavLayer_Main || !in_chain(nav, w)) { a.last_nav = 0; return; }
    const bool moved = g.NavJustMovedToId != 0 && g.NavJustMovedToId == nid;              // D-pad / keyboard move, or our page
    const bool changed = nid != a.last_nav && g.NavCursorVisible && g_click_frame < fc - 2;   // focus set in code (dropdown, dialog closed ...)
    a.last_nav = nid;
    if (!moved && !changed) return;
    const ImRect r = ImGui::WindowRectRelToAbs(nav, nav->NavRectRel[ImGuiNavLayer_Main]);   // last frame's geometry, like w->Scroll
    if (r.IsInverted()) return;
    const ImRect view = w->InnerRect;
    const float margin = a.page_follow_frame >= fc - 2 ? 0.f : hubui::kit::focus_margin(view.GetHeight(), sp(1.f));
    const float ns = hubui::kit::follow_scroll(w->Scroll.y, w->ScrollMax.y, view.Min.y, view.Max.y, r.Min.y, r.Max.y, margin);
    if (std::fabs(ns - w->Scroll.y) >= 0.5f) ImGui::SetScrollY(w, ns);   // replaces ImGui's own edge-only ScrollToRectEx target
}

/* After an LT / RT page: move the focus onto the first item of the new view (the last one when RT is already at the end). */
void page_focus(ImGuiWindow* cw, const Frame& fr, Area& a, int fc) {
    ImGuiContext& g = *GImGui;
    if (!g.NavWindow || g.NavMoveSubmitted || g.NavLayer != ImGuiNavLayer_Main) return;
    const ImRect view = cw->InnerRect;
    const ImRect old = ImGui::WindowRectRelToAbs(g.NavWindow, g.NavWindow->NavRectRel[ImGuiNavLayer_Main]);
    /* the centre of the old item's column: an edge point at a row's left end would score a row it overlaps as "to the right" */
    float x = old.IsInverted() ? (view.Min.x + view.Max.x) * 0.5f : (old.Min.x + old.Max.x) * 0.5f;
    x = hubui::kit::clampf(x, view.Min.x + 1.f, std::max(view.Min.x + 1.f, view.Max.x - 1.f));
    const bool to_last = !fr.page_scrolled && fr.page_dir > 0;
    const ImGuiDir dir = to_last ? ImGuiDir_Up : ImGuiDir_Down;
    const float y = to_last ? view.Max.y - 1.f : view.Min.y + 1.f;
    ImGui::NavMoveRequestSubmit(dir, dir, ImGuiNavMoveFlags_AllowCurrentNavId, ImGuiScrollFlags_KeepVisibleEdgeY);
    g.NavScoringRect = ImRect(x, y, x, y);   // scored from the edge of the new view, not from the old (scrolled away) item
    g.NavScoringNoClipRect = g.NavScoringRect;
    a.page_follow_frame = fc;
}

void draw_bar(ImGuiWindow* cw) {
    ImGuiContext& g = *GImGui;
    if (!cw->ScrollbarY || !hubui::kit::overflows(cw->ScrollMax.y)) return;
    const ImGuiID sid = ImGui::GetWindowScrollbarID(cw, ImGuiAxis_Y);
    const ImRect bb = ImGui::GetWindowScrollbarRect(cw, ImGuiAxis_Y);
    const hubui::kit::Track tr = hubui::kit::scrollbar_track(bb.Min.x, bb.Min.y, bb.Max.x, bb.Max.y);
    const float content = cw->ContentSize.y + cw->WindowPadding.y * 2.f, view = cw->InnerRect.Max.y - cw->InnerRect.Min.y;
    const hubui::kit::Thumb th = hubui::kit::scrollbar_thumb(tr.y0, tr.y1, view, content, cw->Scroll.y, g.Style.GrabMinSize);
    if (!th.show) return;
    const bool active = g.ActiveId == sid;
    const bool hov = g.HoveredId == sid || (bb.Contains(g.IO.MousePos) && g.HoveredWindow == cw);
    float* hv = cw->StateStorage.GetFloatRef(sid, 0.f);
    anim(*hv, (hov || active) ? 1.f : 0.f, 16.f);
    const float cx = (bb.Min.x + bb.Max.x) * 0.5f;
    const float w = std::min(bb.GetWidth() - 2.f, sp(0.5f) + sp(0.5f) * *hv);
    ImDrawList* dl = cw->DrawList;
    dl->PushClipRect(cw->Rect().Min, cw->Rect().Max, false);
    dl->AddRectFilled(ImVec2(cx - w * 0.5f, tr.y0), ImVec2(cx + w * 0.5f, tr.y1), veil(0.05f + 0.05f * *hv), w * 0.5f);
    const ImU32 col = active ? ACCENT : mix(with_alpha(TEXT2, 0.6f), TEXT2, *hv);
    dl->AddRectFilled(ImVec2(cx - w * 0.5f, th.y0), ImVec2(cx + w * 0.5f, th.y1), col, w * 0.5f);
    dl->PopClipRect();
}

}  // namespace

bool scroll_begin(const char* id, ImVec2 size, ScrollFlags flags, ImVec2 padding) {
    ImGuiContext& g = *GImGui;
    const ImGuiID key = ImGui::GetID(id);
    Area& a = g_areas[key];
    const int fc = ImGui::GetFrameCount();
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) g_click_frame = fc;
    ImGuiWindow* w = a.win_id ? ImGui::FindWindowByID(a.win_id) : nullptr;
    const bool alive = w && a.frame >= fc - 1;
    a.flags = flags;
    const ImVec2 pad(padding.x >= 0.f ? padding.x : sp(1.5f), padding.y >= 0.f ? padding.y : sp(1.f));

    Frame fr;
    fr.key = key;
    float set_scroll = -1.f;
    if (alive && !(flags & ScrollFlags_NoPadPaging) && !g.IO.WantTextInput) {
        int dir = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL2, true)) dir = -1;
        else if (ImGui::IsKeyPressed(ImGuiKey_GamepadR2, true)) dir = 1;
        if (dir && is_pager(w, a)) {
            ImGui::SetKeyOwner(dir < 0 ? ImGuiKey_GamepadL2 : ImGuiKey_GamepadR2, key, ImGuiInputFlags_LockThisFrame);   // main.cpp's LT / RT paging reads the same press: it is ours
            fr.page_dir = dir;
            if (hubui::kit::can_page(w->Scroll.y, w->ScrollMax.y, dir)) {
                set_scroll = hubui::kit::page_target(w->Scroll.y, w->ScrollMax.y, w->InnerRect.GetHeight(), dir);
                fr.page_scrolled = true;
            }
        }
    }
    if (alive && !fr.page_dir && !(flags & ScrollFlags_NoFocusFollow)) focus_follow(w, a, fc);
    if (set_scroll >= 0.f) ImGui::SetNextWindowScroll(ImVec2(-1.f, set_scroll));

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float h = size.y > 0.f ? size.y : std::max(1.f, avail.y + size.y);
    const bool autoh = (flags & ScrollFlags_AutoHeight) != 0;
    if (autoh) ImGui::SetNextWindowSizeConstraints(ImVec2(0.f, 0.f), ImVec2(FLT_MAX, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, pad);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, sp(1.5f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, 0u);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, 0u);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, 0u);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, 0u);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, 0u);
    const ImGuiChildFlags cf = ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened | (autoh ? ImGuiChildFlags_AutoResizeY : 0);
    const bool vis = ImGui::BeginChild(id, ImVec2(size.x, autoh ? 0.f : h), cf, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(2);

    ImGuiWindow* cw = g.CurrentWindow;
    a.win_id = cw->ID;
    a.frame = fc;
    g_by_window[cw->ID] = key;
    if (fr.page_dir) page_focus(cw, fr, a, fc);
    g_stack.push_back(fr);
    return vis;
}

ScrollInfo scroll_end() {
    ImGuiContext& g = *GImGui;
    ScrollInfo si;
    IM_ASSERT(!g_stack.empty() && "ui::scroll_end() without ui::scroll_begin()");
    if (!g_stack.empty()) g_stack.pop_back();
    ImGuiWindow* cw = g.CurrentWindow;
    si.window_id = cw->ID;
    si.visible = !cw->SkipItems;
    si.scroll = cw->Scroll.y;
    si.scroll_max = cw->ScrollMax.y;
    si.view_h = cw->InnerRect.GetHeight();
    si.content_h = cw->ContentSize.y + cw->WindowPadding.y * 2.f;
    si.overflows = hubui::kit::overflows(cw->ScrollMax.y);
    draw_bar(cw);
    ImGui::EndChild();
    return si;
}

void scroll_set(ImGuiID window_id, float y) {
    if (ImGuiWindow* w = window_id ? ImGui::FindWindowByID(window_id) : nullptr) ImGui::SetScrollY(w, std::max(0.f, y));
}

}  // namespace ui
