#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui_scroll.h"

#include "imgui_internal.h"
#include "ui_kit.h"
#include "ui_layout_logic.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

void shot_trace(const char* what, const std::string& arg);   // shot.cpp: a HUB_SHOT_TRACE line (screenshot runs only)

/* UI kit scroll area (see ui_scroll.h). Native ImGui scrolling (wheel, drag, ScrollMax), with:
 *   - ImGui's own scrollbar kept for interaction but drawn transparent; a themed thumb is drawn over the same geometry;
 *   - AlwaysVerticalScrollbar so the gutter is always reserved (no reflow when overflow toggles);
 *   - focus-follow with a margin: ImGui's own nav scroll (ScrollToRectEx, edge + ItemSpacing) is overridden before the child begins;
 *   - LT / RT paging: the press is locked (LockThisFrame) so main.cpp's global paging, which reads the same press before the next
 *     NewFrame, skips it; the focus is moved with a nav request scored from the edge of the new view.
 * Options: Eased (every scroll request, ImGui's own included, becomes the target of a spring, hubui::kit::Glide), OverlayBar (NoScrollbar
 * child; the kit submits ImGui's ScrollbarEx itself over the right edge, first in the child so it wins the hover, and draws the thumb on
 * top at the end), AutoGutter (ImGui's auto scrollbar at the style's size, only the thumb is ours). */

namespace ui {
namespace {

struct Area {
    ImGuiID win_id = 0;
    int frame = -10;               // last frame drawn
    ScrollFlags flags = 0;
    ImGuiID last_nav = 0;          // pad focus seen inside the area last frame (0 = elsewhere)
    int page_follow_frame = -10;   // the focus was moved by our own page: that move keeps no margin
    hubui::kit::Glide glide;       // ScrollFlags_Eased: where the view is and where it is heading
    float pending = 0.f;           // scroll_by() pixels for the next frame
};
struct Frame {
    ImGuiID key = 0;
    int page_dir = 0;
    bool page_scrolled = false;
    ScrollFlags flags = 0;
    bool page_req = false;     // page_focus() submitted its "first item of the new view" request this frame
    float page_off = 0.f;      // ... and the new view is this far below this frame's (an eased area still gliding there)
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

/* The scroll offset that keeps a focus that just moved in view (with the margin), or -1 when it did not move. `base` is the offset the
 * area is at (Eased: heading to); the item is measured where it will sit at that offset. */
float focus_follow(ImGuiWindow* w, Area& a, int fc, float base) {
    ImGuiContext& g = *GImGui;
    ImGuiWindow* nav = g.NavWindow;
    const ImGuiID nid = g.NavId;
    if (!nid || !nav || g.NavLayer != ImGuiNavLayer_Main || !in_chain(nav, w)) { a.last_nav = 0; return -1.f; }
    const bool moved = g.NavJustMovedToId != 0 && g.NavJustMovedToId == nid;              // D-pad / keyboard move, or our page
    const bool changed = nid != a.last_nav && g.NavCursorVisible && g_click_frame < fc - 2;   // focus set in code (dropdown, dialog closed ...)
    a.last_nav = nid;
    if (!moved && !changed) return -1.f;
    const ImRect r = ImGui::WindowRectRelToAbs(nav, nav->NavRectRel[ImGuiNavLayer_Main]);   // last frame's geometry, like w->Scroll
    if (r.IsInverted()) return -1.f;
    const ImRect view = w->InnerRect;
    const float margin = a.page_follow_frame >= fc - 2 ? 0.f : hubui::kit::focus_margin(view.GetHeight(), sp(1.f));
    const float shift = base - w->Scroll.y;   // 0 unless an eased view is still on its way
    return hubui::kit::follow_scroll(base, w->ScrollMax.y, view.Min.y, view.Max.y, r.Min.y - shift, r.Max.y - shift, margin);
}

/* After an LT / RT page: move the focus onto the first item of the new view (the last one when RT is already at the end). `off` is how
 * far the new view still is from this frame's (an eased area is only starting to glide there). */
void page_focus(ImGuiWindow* cw, Frame& fr, Area& a, int fc, float off) {
    ImGuiContext& g = *GImGui;
    if (!g.NavWindow || g.NavMoveSubmitted || g.NavLayer != ImGuiNavLayer_Main) return;
    fr.page_req = true;
    fr.page_off = off;
    const ImRect view = cw->InnerRect;
    const ImRect old = ImGui::WindowRectRelToAbs(g.NavWindow, g.NavWindow->NavRectRel[ImGuiNavLayer_Main]);
    /* the centre of the old item's column: an edge point at a row's left end would score a row it overlaps as "to the right" */
    float x = old.IsInverted() ? (view.Min.x + view.Max.x) * 0.5f : (old.Min.x + old.Max.x) * 0.5f;
    x = hubui::kit::clampf(x, view.Min.x + 1.f, std::max(view.Min.x + 1.f, view.Max.x - 1.f));
    const bool to_last = !fr.page_scrolled && fr.page_dir > 0;
    const ImGuiDir dir = to_last ? ImGuiDir_Up : ImGuiDir_Down;
    const float y = (to_last ? view.Max.y - 1.f : view.Min.y + 1.f) + off;
    ImGui::NavMoveRequestSubmit(dir, dir, ImGuiNavMoveFlags_AllowCurrentNavId, ImGuiScrollFlags_KeepVisibleEdgeY);
    g.NavScoringRect = ImRect(x, y, x, y);   // scored from the edge of the new view, not from the old (scrolled away) item
    g.NavScoringNoClipRect = g.NavScoringRect;
    a.page_follow_frame = fc;
}

/* OverlayBar: the bar's rect, the area's right edge, as wide as the regular gutter. An area that bleeds past its host (Home's shelves
 * reach sp(1.5) beyond the page, into the margin) gets it in that bleed: right beside the content, never over it. */
ImRect overlay_rect(ImGuiWindow* cw) {
    const float bw = sp(1.5f);
    return ImRect(cw->Rect().Max.x - bw, cw->InnerRect.Min.y, cw->Rect().Max.x, cw->InnerRect.Max.y);
}

/* The pointer is over the bar but ImGui says a parent window is hovered (the bar sits in the part of the area its host clips away):
 * the bar is the only thing there, so let it take the hover for its press. */
bool forward_hover(ImGuiWindow* cw, const ImRect& bb) {
    ImGuiContext& g = *GImGui;
    if (!g.HoveredWindow || g.HoveredWindow == cw || !bb.Contains(g.IO.MousePos)) return false;
    for (ImGuiWindow* w = is_child(cw) ? cw->ParentWindow : nullptr; w; w = is_child(w) ? w->ParentWindow : nullptr)
        if (w == g.HoveredWindow) return true;
    return false;
}

void push_clear_bar() {
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, 0u);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, 0u);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, 0u);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, 0u);
}

/* OverlayBar: ImGui's own scrollbar behaviour (drag the thumb, click the track) on the overlay rect, drawn clear (the thumb is drawn in
 * scroll_end). Submitted first in the child so it wins the hover over content under it. The new offset applies next frame: this frame's
 * items are already placed. */
void overlay_input(ImGuiWindow* cw, Area& a, bool eased) {
    if (cw->SkipItems || !hubui::kit::overflows(cw->ScrollMax.y)) return;
    const ImRect bb = overlay_rect(cw);
    const float view = cw->InnerRect.GetHeight(), content = cw->ContentSize.y + cw->WindowPadding.y * 2.f;
    if (view <= 0.f || content <= 0.f) return;
    ImS64 v = (ImS64)cw->Scroll.y;
    const ImS64 v0 = v;
    ImGuiContext& g = *GImGui;
    ImGuiWindow* const hovered = g.HoveredWindow;
    if (forward_hover(cw, bb)) g.HoveredWindow = cw;   // restored right below
    ImGui::PushClipRect(cw->Rect().Min, cw->Rect().Max, false);
    push_clear_bar();
    ImGui::ScrollbarEx(bb, ImGui::GetWindowScrollbarID(cw, ImGuiAxis_Y), ImGuiAxis_Y, &v, (ImS64)view, (ImS64)content, ImDrawFlags_RoundCornersAll);
    ImGui::PopStyleColor(4);
    ImGui::PopClipRect();
    g.HoveredWindow = hovered;
    if (v != v0) {
        if (eased) a.glide.reset((float)v);   // a dragged thumb follows the pointer, no glide
        ImGui::SetScrollY(cw, (float)v);
    }
}

void draw_bar(ImGuiWindow* cw, bool overlay) {
    ImGuiContext& g = *GImGui;
    if (!hubui::kit::overflows(cw->ScrollMax.y) || (!overlay && !cw->ScrollbarY)) return;
    const ImGuiID sid = ImGui::GetWindowScrollbarID(cw, ImGuiAxis_Y);
    const ImRect bb = overlay ? overlay_rect(cw) : ImGui::GetWindowScrollbarRect(cw, ImGuiAxis_Y);
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
    const bool eased = (flags & ScrollFlags_Eased) != 0;
    const bool overlay = (flags & ScrollFlags_OverlayBar) != 0;
    const bool auto_gutter = !overlay && (flags & ScrollFlags_AutoGutter) != 0;
    const ImVec2 pad(padding.x >= 0.f ? padding.x : sp(1.5f), padding.y >= 0.f ? padding.y : sp(1.f));
    if (eased && !alive) a.glide.reset(w ? w->Scroll.y : 0.f);   // first frame, or the area was away: start where the window is
    const float base = !alive ? 0.f : (eased ? a.glide.target : w->Scroll.y);   // pages and focus moves count from here

    Frame fr;
    fr.key = key;
    fr.flags = flags;
    float set_scroll = -1.f;
    if (alive && !(flags & ScrollFlags_NoPadPaging) && !g.IO.WantTextInput) {
        int dir = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL2, true)) dir = -1;
        else if (ImGui::IsKeyPressed(ImGuiKey_GamepadR2, true)) dir = 1;
        if (dir && is_pager(w, a)) {
            ImGui::SetKeyOwner(dir < 0 ? ImGuiKey_GamepadL2 : ImGuiKey_GamepadR2, key, ImGuiInputFlags_LockThisFrame);   // main.cpp's LT / RT paging reads the same press: it is ours
            fr.page_dir = dir;
            if (hubui::kit::can_page(base, w->ScrollMax.y, dir)) {
                set_scroll = hubui::kit::page_target(base, w->ScrollMax.y, w->InnerRect.GetHeight(), dir);
                fr.page_scrolled = true;
            }
        }
    }
    const float follow = alive && !fr.page_dir && !(flags & ScrollFlags_NoFocusFollow) ? focus_follow(w, a, fc, base) : -1.f;
    float page_off = 0.f;
    if (eased && alive) {
        const float mx = std::max(0.f, w->ScrollMax.y);
        if (w->ScrollTarget.y < FLT_MAX) {   // ImGui wants to move it (wheel, keys, its own nav scroll, SetScrollY ...): glide there instead
            if (g.WheelingWindow == w && g.WheelingWindowScrolledFrame == g.FrameCount) {
                a.glide.by(w->ScrollTarget.y - w->Scroll.y, mx);   // a wheel notch adds up with the ones still under way
            } else {
                const float deco = w->DecoOuterSizeY1 + w->DecoInnerSizeY1 + w->DecoOuterSizeY2;
                a.glide.to(hubui::kit::resolve_scroll_target(w->ScrollTarget.y, w->ScrollTargetCenterRatio.y, w->SizeFull.y - deco,
                                                             w->ScrollTargetEdgeSnapDist.y, mx), mx);
            }
            w->ScrollTarget.y = FLT_MAX;
            w->ScrollTargetEdgeSnapDist.y = 0.f;
        }
        if (follow >= 0.f) a.glide.to(follow, mx);            // the focus moved: its place wins over ImGui's own edge-only scroll
        if (set_scroll >= 0.f) a.glide.to(set_scroll, mx);    // LT / RT
        if (a.pending != 0.f) a.glide.by(a.pending, mx);      // scroll_by()
        a.glide.step(g.IO.DeltaTime, mx, reduce_motion);
        set_scroll = a.glide.pos;
        page_off = a.glide.target - a.glide.pos;
    } else if (alive) {
        if (follow >= 0.f && std::fabs(follow - w->Scroll.y) >= 0.5f) ImGui::SetScrollY(w, follow);   // replaces ImGui's own edge-only ScrollToRectEx target
        if (a.pending != 0.f && set_scroll < 0.f) set_scroll = hubui::kit::clampf(w->Scroll.y + a.pending, 0.f, std::max(0.f, w->ScrollMax.y));
    }
    a.pending = 0.f;
    if (set_scroll >= 0.f) ImGui::SetNextWindowScroll(ImVec2(-1.f, set_scroll));

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float h = size.y > 0.f ? size.y : std::max(1.f, avail.y + size.y);
    const bool autoh = (flags & ScrollFlags_AutoHeight) != 0;
    if (autoh) ImGui::SetNextWindowSizeConstraints(ImVec2(0.f, 0.f), ImVec2(FLT_MAX, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, pad);
    if (!auto_gutter) ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, sp(1.5f));   // AutoGutter keeps the style's size: a plain child's exact layout
    push_clear_bar();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, 0u);
    const ImGuiChildFlags cf = ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened | (autoh ? ImGuiChildFlags_AutoResizeY : 0);
    const ImGuiWindowFlags wf = overlay ? ImGuiWindowFlags_NoScrollbar : (auto_gutter ? ImGuiWindowFlags_None : ImGuiWindowFlags_AlwaysVerticalScrollbar);
    const bool vis = ImGui::BeginChild(id, ImVec2(size.x, autoh ? 0.f : h), cf, wf);
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(auto_gutter ? 1 : 2);

    ImGuiWindow* cw = g.CurrentWindow;
    a.win_id = cw->ID;
    a.frame = fc;
    g_by_window[cw->ID] = key;
    if (overlay) overlay_input(cw, a, eased);
    if (fr.page_dir) page_focus(cw, fr, a, fc, page_off);
    g_stack.push_back(fr);
    return vis;
}

ScrollInfo scroll_end() {
    ImGuiContext& g = *GImGui;
    ScrollInfo si;
    IM_ASSERT(!g_stack.empty() && "ui::scroll_end() without ui::scroll_begin()");
    const Frame fr = g_stack.empty() ? Frame() : g_stack.back();
    const ScrollFlags flags = fr.flags;
    if (!g_stack.empty()) g_stack.pop_back();
    ImGuiWindow* cw = g.CurrentWindow;
    /* After an LT / RT page: the "first item of the new view" must be IN the new view. With nothing focusable there (a long text, a tall
     * panel) ImGui's best guess is an item outside it, and taking it would scroll the view straight back: keep the page, leave the focus
     * where it was (the next D-pad press starts from the view, as ImGui does for a focus scrolled away). */
    if (fr.page_req && g.NavMoveSubmitted && g.NavMoveScoringItems) {
        /* the result ImGui would take so far (NavMoveRequestApplyResult's choice; the area's items are all scored by now) */
        const ImGuiNavItemData* res = g.NavMoveResultLocal.ID ? &g.NavMoveResultLocal : g.NavMoveResultOther.ID ? &g.NavMoveResultOther : nullptr;
        const ImGuiNavItemData& oth = g.NavMoveResultOther;
        if (res && res != &oth && oth.ID && oth.Window && oth.Window->ParentWindow == g.NavWindow &&
            (oth.DistBox < res->DistBox || (oth.DistBox == res->DistBox && oth.DistCenter < res->DistCenter)))
            res = &oth;
        ImRect view = cw->InnerRect;
        view.TranslateY(fr.page_off);
        const bool in_view = res && res->Window && (res->Window == cw || ImGui::IsWindowChildOf(res->Window, cw, false)) &&
                             ImGui::WindowRectRelToAbs(res->Window, res->RectRel).Overlaps(view);
        if (!in_view) {
            ImGui::NavMoveRequestCancel();
            shot_trace("page-keep-focus", "");   // QA (HUB_SHOT_TRACE): nothing focusable in the new view
        }
    }
    si.window_id = cw->ID;
    si.visible = !cw->SkipItems;
    si.scroll = cw->Scroll.y;
    si.scroll_max = cw->ScrollMax.y;
    si.view_h = cw->InnerRect.GetHeight();
    si.content_h = cw->ContentSize.y + cw->WindowPadding.y * 2.f;
    si.overflows = hubui::kit::overflows(cw->ScrollMax.y);
    draw_bar(cw, (flags & ScrollFlags_OverlayBar) != 0);
    ImGui::EndChild();
    return si;
}

static Area* area_of(ImGuiID window_id) {
    auto it = g_by_window.find(window_id);
    if (it == g_by_window.end()) return nullptr;
    auto ar = g_areas.find(it->second);
    return ar == g_areas.end() ? nullptr : &ar->second;
}

void scroll_set(ImGuiID window_id, float y) {
    if (ImGuiWindow* w = window_id ? ImGui::FindWindowByID(window_id) : nullptr) {
        ImGui::SetScrollY(w, std::max(0.f, y));
        if (Area* a = area_of(window_id); a && (a->flags & ScrollFlags_Eased)) a->glide.reset(y);   // a jump, not a glide
    }
}

void scroll_by(ImGuiID window_id, float dy) {
    if (Area* a = window_id ? area_of(window_id) : nullptr) a->pending += dy;
}

void scrollbar_theme_push() { push_clear_bar(); }
void scrollbar_theme_pop() { ImGui::PopStyleColor(4); }
void scrollbar_theme_draw() { draw_bar(GImGui->CurrentWindow, false); }

}  // namespace ui
