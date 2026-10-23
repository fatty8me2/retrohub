#include "app.h"

/* ---------- shelves ---------- */

/* Arrow buttons that scroll an overflowing shelf live in the header row (right edge), never over the covers. */
float shelf_arrow_size() { return sp(3.25f); }
float shelf_arrows_w() { return shelf_arrow_size() * 2 + sp(0.5f); }

void shelf_header(const char* title, const std::string& right, float bleed, int count, bool* link_clicked, bool arrows) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* f = ui::F.h2;
    dl->AddText(f, f->FontSize, ImVec2(p.x + bleed, p.y), ui::TEXT, title);
    if (count >= 0) {
        float tw = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.f, title).x;
        char c[16];
        std::snprintf(c, sizeof c, "%d", count);
        ImFont* cf = ui::F.cap;
        dl->AddText(cf, cf->FontSize, ImVec2(p.x + bleed + tw + sp(1.25f), p.y + f->FontSize - cf->FontSize - sp(0.25f)), ui::TEXT2, c);   // ~7:1 on BG
    }
    if (!right.empty()) {
        ImFont* rf = ui::F.body;
        ImVec2 ts = rf->CalcTextSizeA(rf->FontSize, FLT_MAX, 0.f, right.c_str());
        ImVec2 tp(p.x + w - bleed - ts.x - (arrows ? shelf_arrows_w() + sp(1.5f) : 0.f), p.y + f->FontSize - ts.y - sp(0.125f));
        ImU32 col = ui::TEXT2;
        if (link_clicked) {   // the right label is a link ("See all")
            ImGui::SetCursorScreenPos(ImVec2(tp.x - sp(1.f), p.y));
            ImGui::PushID(title);
            *link_clicked = ImGui::InvisibleButton("##see_all", ImVec2(ts.x + sp(2.f), f->FontSize + sp(0.5f)), ImGuiButtonFlags_EnableNav);
            ImGui::PopID();
            if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) col = ui::mix(ui::ACCENT, IM_COL32_WHITE, 0.35f);
            ImGui::SetCursorScreenPos(p);
        }
        dl->AddText(rf, rf->FontSize, tp, col, right.c_str());
    }
    ImGui::Dummy(ImVec2(w, f->FontSize + sp(1.25f)));
}

/* Mouse wheel over a shelf row that overflows scrolls THAT row horizontally (called from the event loop before
 * ImGui sees the event, so the page does not scroll too). Returns false when the pointer is not over such a row. */
bool route_wheel(float px, float py, float wx, float wy) {
    std::vector<hubui::FRect> rects;
    std::vector<ShelfState*> rows;
    int fc = ImGui::GetFrameCount();
    for (auto& kv : A.shelf_state)
        if (kv.second.frame >= fc - 2 && kv.second.max_scroll > 0) {
            rects.push_back(kv.second.rect);
            rows.push_back(&kv.second);
        }
    int i = hubui::rect_hit(rects, px, py);
    if (i < 0) return false;
    ShelfState& s = *rows[i];
    s.target = hubui::shelf_clamp(s.target + hubui::shelf_wheel_delta(wx, wy, s.tw, s.gap), s.max_scroll);
    return true;
}

/* A horizontally scrolling row of tiles. Scrolling is managed here (smooth, time based) instead of by a child
 * window, so every tile stays a real ImGui item and the D-pad can walk onto tiles that are off-screen.
 * Wheel (see route_wheel), arrow buttons, click-and-drag and keyboard/gamepad focus all move the same target. */
void shelf_strip(const std::string& id, int count, float tw, float th, const std::function<TileResult(int, ImVec2)>& draw_one) {
    ImGuiIO& io = ImGui::GetIO();
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    ShelfState& ss = A.shelf_state[id];
    float gap = sp(2.f), bleed = sp(1.5f);
    float view_w = ImGui::GetContentRegionAvail().x;
    float strip_h = th + sp(4.5f);
    float max_scroll = hubui::shelf_max_scroll(count, tw, gap, bleed, view_w);
    ImVec2 org = ImGui::GetCursorScreenPos();
    ImVec2 v0 = org, v1(org.x + view_w, org.y + strip_h);

    ImVec2 c0(std::max(v0.x, win->InnerClipRect.Min.x), std::max(v0.y, win->InnerClipRect.Min.y));
    ImVec2 c1(std::min(v1.x, win->InnerClipRect.Max.x), std::min(v1.y, win->InnerClipRect.Max.y));
    ss.rect = {c0.x, c0.y, c1.x, c1.y};
    ss.max_scroll = max_scroll;
    ss.tw = tw;
    ss.gap = gap;
    ss.frame = ImGui::GetFrameCount();

    // arrow buttons: in the header row (right edge) while the row overflows, so they never cover a cover
    float bs = shelf_arrow_size();
    float hdr_h = ui::F.h2->FontSize + sp(1.25f);
    float by = org.y - hdr_h + (ui::F.h2->FontSize - bs) * 0.5f + sp(0.3f);
    ImVec2 la(v1.x - bleed - shelf_arrows_w(), by), ra(v1.x - bleed - bs, by);
    bool over_arrow = false;   // arrows are outside the strip, so a press on a tile is never an arrow press

    bool hov = ImGui::IsMouseHoveringRect(c0, c1) && ImGui::IsWindowHovered();
    // click-and-drag panning (a press that moves past the threshold becomes a drag; a plain click is untouched)
    if (max_scroll > 0) {
        if (hov && !over_arrow && ImGui::IsMouseClicked(0)) {
            ss.pressed = true;
            ss.dragging = false;
            ss.press_x = io.MousePos.x;
            ss.press_y = io.MousePos.y;
            ss.press_scroll = ss.scroll;
        }
        if (ss.pressed && ImGui::IsMouseDown(0)) {
            if (!ss.dragging && hubui::drag_exceeds(io.MousePos.x - ss.press_x, io.MousePos.y - ss.press_y, sp(0.75f))) ss.dragging = true;
            if (ss.dragging) {
                ss.target = hubui::shelf_clamp(ss.press_scroll - (io.MousePos.x - ss.press_x), max_scroll);
                ss.scroll = ss.target;
            }
        }
    }
    ss.target = hubui::shelf_clamp(ss.target, max_scroll);
    if (!ss.dragging) ss.scroll = hubui::approach(ss.scroll, ss.target, io.DeltaTime, 16.f);
    if (std::fabs(ss.scroll - ss.target) < 0.4f) ss.scroll = ss.target;
    g_suppress_click = ss.dragging;

    ImGui::PushClipRect(v0, v1, true);
    bool any_focus = false;
    for (int i = 0; i < count; ++i) {
        float x = org.x + bleed + i * (tw + gap) - ss.scroll;
        ImGui::SetCursorScreenPos(ImVec2(x, org.y + sp(1.5f)));
        TileResult r = draw_one(i, ImVec2(tw, th));
        if (r.focused) {
            any_focus = true;
            if (ss.focus_idx != i) {   // gamepad/keyboard moved onto this tile: scroll it into view
                ss.focus_idx = i;
                float left = bleed + i * (tw + gap);
                ss.target = hubui::shelf_reveal(left, left + tw, ss.target, view_w, sp(5.f), max_scroll);
            }
        }
    }
    if (!any_focus) ss.focus_idx = -1;
    ImGui::PopClipRect();
    g_suppress_click = false;
    if (!io.MouseDown[0]) { ss.pressed = false; ss.dragging = false; }

    // edge fades hint that the row scrolls
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float fw = sp(5.f);
    ImU32 solid = ui::BG, clear = ui::with_alpha(ui::BG, 0.f);
    dl->PushClipRect(v0, v1, true);
    if (ss.scroll > 1.f) dl->AddRectFilledMultiColor(v0, ImVec2(v0.x + fw, v1.y), solid, clear, clear, solid);
    if (ss.scroll < max_scroll - 1.f) dl->AddRectFilledMultiColor(ImVec2(v1.x - fw, v0.y), v1, clear, solid, solid, clear);
    dl->PopClipRect();
    if (max_scroll > 0) {
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        float page = hubui::shelf_page_step(view_w, tw, gap, bleed);
        bool can_l = ss.target > 1.f, can_r = ss.target < max_scroll - 1.f;
        ImGui::SetCursorScreenPos(la);
        ImGui::BeginDisabled(!can_l);
        if (ui::icon_button((id + "<").c_str(), ui::Icon::ChevronL, bs, false, "Scroll left")) ss.target = hubui::shelf_clamp(ss.target - page, max_scroll);
        ImGui::EndDisabled();
        ImGui::SetCursorScreenPos(ra);
        ImGui::BeginDisabled(!can_r);
        if (ui::icon_button((id + ">").c_str(), ui::Icon::ChevronR, bs, false, "Scroll right")) ss.target = hubui::shelf_clamp(ss.target + page, max_scroll);
        ImGui::EndDisabled();
        ImGui::PopItemFlag();
    }

    ImGui::SetCursorScreenPos(org);
    ImGui::Dummy(ImVec2(view_w, strip_h));
    win->DC.CursorMaxPos.x = std::min(win->DC.CursorMaxPos.x, win->WorkRect.Max.x);   // off-screen tiles must not widen the page
}

