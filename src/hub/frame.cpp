#include "app.h"
#include "homefun.h"
#include "ui_sound.h"

/* Startup "new version available" prompt. Shown once per run when the launch
 * check finds a newer release that the player hasn't chosen to skip (a manual
 * "Check for updates" shows it even for a skipped version). */
void draw_update_popup() {
    bool want = hubupdate::update_available() && !A.update_prompt_dismissed &&
                (A.update_check_requested || !hubupdate::latest_skipped());
    if (want && !ImGui::IsPopupOpen("Update available")) ImGui::OpenPopup("Update available");
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(sp(52.f), 0), ImVec2(sp(80.f), vp->Size.y * 0.8f));
    if (!ImGui::BeginPopupModal("Update available", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove))
        return;
    if (ui::modal_back() && !hubupdate::downloading()) {   // B = Later
        A.update_prompt_dismissed = true;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::PushFont(ui::F.h2);
    ImGui::Text("RetroHub %s is available", hubupdate::latest_version().c_str());
    ImGui::PopFont();
    ImGui::TextDisabled("You have %s. Do you want to update now?", hubupdate::current_version().c_str());
    std::string notes = hubupdate::release_notes();
    if (!notes.empty()) {
        ImGui::Dummy(ImVec2(0, 4 * S));
        ImGui::BeginChild("notes", ImVec2(sp(70.f), sp(18.f)), ImGuiChildFlags_Border);
        ImGui::TextWrapped("%s", notes.c_str());
        ImGui::EndChild();
    }
    ImGui::Dummy(ImVec2(0, 4 * S));
    if (hubupdate::downloading()) {
        float p = hubupdate::download_progress();
        ImGui::ProgressBar(p < 0 ? 0.f : p, ImVec2(-1, 0), p < 0 ? "Downloading..." : nullptr);
        ImGui::TextDisabled("%s", hubupdate::status().c_str());
        ImGui::TextDisabled("RetroHub will close, install the update and reopen by itself.");
    } else {
        if (!hubupdate::status().empty() && hubupdate::check_failed())
            ImGui::TextColored(ImVec4(1.f, 0.5f, 0.45f, 1.f), "%s", hubupdate::status().c_str());
        if (ImGui::Button("Update now", ImVec2(sp(18.f), 0))) hubupdate::start_update();
        ImGui::SameLine();
        if (ImGui::Button("Later", ImVec2(sp(12.f), 0))) {
            A.update_prompt_dismissed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Skip this version", ImVec2(sp(18.f), 0))) {
            hubupdate::skip_latest();
            A.update_prompt_dismissed = true;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndPopup();
}

/* ---------- toasts ---------- */

void draw_toasts(float bottom, float right, float top) {
    double now = ImGui::GetTime();
    A.toasts.erase(std::remove_if(A.toasts.begin(), A.toasts.end(), [&](const Toast& t) { return t.until < now; }), A.toasts.end());
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImFont* f = ui::F.body;
    float fs = f->FontSize, y_bot = bottom, y_top = top;
    for (int i = (int)A.toasts.size() - 1; i >= 0; --i) {
        const Toast& t = A.toasts[i];
        float in = (float)std::min(1.0, (now - t.t0) / 0.28), out = (float)std::min(1.0, (t.until - now) / 0.35);
        in = 1.f - (1.f - in) * (1.f - in);   // ease-out
        float a = std::max(0.f, std::min(1.f, in * out));
        float maxw = std::min(sp(58.f), ds.x * 0.62f);
        std::vector<std::string> lines = ui::wrap_lines(f, fs, t.text, maxw - sp(6.f), 4);
        float tw = 0;
        for (auto& l : lines) tw = std::max(tw, f->CalcTextSizeA(fs, FLT_MAX, 0.f, l.c_str()).x);
        float w = tw + sp(6.f), h = lines.size() * fs * 1.25f + sp(2.5f);
        float x1 = right + (1.f - in) * sp(5.f);
        float y0 = t.top ? y_top : y_bot - h;   // top toasts grow downward from the top edge, the rest upward from the bottom
        ImVec2 p1(x1, y0 + h), p0(x1 - w, y0);
        ui::shadow(dl, p0, p1, sp(1.5f), a);
        dl->AddRectFilled(p0, p1, ui::scale_alpha(ui::PANEL2, a), sp(1.5f));
        dl->AddRect(p0, p1, ui::scale_alpha(ui::HAIRLINE, a), sp(1.5f), 0, 1.f);
        dl->AddRectFilled(ImVec2(p0.x + sp(1.f), p0.y + sp(1.f)), ImVec2(p0.x + sp(1.f) + sp(0.5f), p1.y - sp(1.f)), ui::scale_alpha(t.col, a), sp(0.25f));
        for (size_t k = 0; k < lines.size(); ++k)
            dl->AddText(f, fs, ImVec2(p0.x + sp(3.f), p0.y + sp(1.25f) + k * fs * 1.25f), ui::scale_alpha(ui::TEXT, a), lines[k].c_str());
        if (t.top) y_top += h + sp(1.f);
        else y_bot -= h + sp(1.f);
    }
}

/* ---------- sidebar ---------- */

bool nav_item(Page pg, const char* label, ui::Icon ic, float w, float expand, ImU32 badge) {
    ImGui::PushID((int)pg);
    bool pressed = ImGui::InvisibleButton("nav", ImVec2(w, sp(6.f)), ImGuiButtonFlags_EnableNav);
    A.side_ids.push_back(ImGui::GetItemID());
    if (A.seed_side && A.page == pg) { A.seed_side = false; set_nav_here(); }
    bool hov = ImGui::IsItemHovered();
    bool focus = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
    bool sel = A.page == pg;
    float* an = ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID(), 0.f);
    ui::anim(*an, sel ? 1.f : ((hov || focus) ? 0.5f : 0.f), 14.f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    float rd = sp(1.5f);
    dl->AddRectFilled(p0, p1, with_alpha(sel ? ui::ACCENT : ui::HILITE, sel ? 0.20f : 0.07f * (*an * 2.f)), rd);
    if (*an > 0.6f) dl->AddRectFilled(ImVec2(p0.x, p0.y + sp(1.25f)), ImVec2(p0.x + sp(0.5f), p1.y - sp(1.25f)), ui::ACCENT, sp(0.25f));
    ImU32 col = mix(ui::TEXT2, ui::HILITE, std::min(1.f, *an * 1.4f));
    ImVec2 ic_c(p0.x + sp(3.f), (p0.y + p1.y) * 0.5f);
    ui::icon(dl, ic, ic_c, sp(3.f), sel ? mix(ui::ACCENT, IM_COL32_WHITE, 0.55f) : col);
    if (badge) dl->AddCircleFilled(ic_c + ImVec2(sp(1.5f), -sp(1.5f)), sp(0.625f), badge, 12);
    if (expand > 0.02f) {
        dl->PushClipRect(p0, p1, true);
        dl->AddText(ui::F.bold, ui::F.bold->FontSize, ImVec2(p0.x + sp(6.f), ic_c.y - ui::F.bold->FontSize * 0.5f - 1), ui::scale_alpha(col, expand), label);
        dl->PopClipRect();
    }
    if (focus) ui::focus_ring(dl, p0, p1, rd);
    if (expand < 0.5f && hov) ImGui::SetTooltip("%s", label);
    ImGui::PopID();
    return pressed;
}

/* ---------- footer: contextual controller / keyboard hints + status ---------- */


void draw_footer(float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    dl->AddLine(p, ImVec2(p.x + w, p.y), ui::HAIRLINE, 1.f);
    bool pad = A.pad_mode;
    std::vector<Hint> hints;
    const bool popup = ImGui::IsPopupOpen((const char*)nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (popup) {
        hints = {{"A", "Enter", "Select"}, {"B", "Esc", "Close"}};
    } else if (A.st.show_friends && FR.panel_focused) {
        hints = {{"A", "Enter", "Actions"}, {"X", "", "Join / invite"}, {"Y", "", "Menu"}, {"B", "Esc", "Close"}};
    } else if (A.page == Page::Library && !A.lib.empty()) {
        hints = {{"A", "Enter", "Play"}, {"X", "F", "Favorite"}, {"Y", "I", "Details"}, {"R3", "S", "Add to shelf"}};
        if (A.view == LibView::All) hints.push_back({"B", "Esc", "Back"});
    } else if (A.page == Page::Library) {
        hints = {{"A", "Enter", "Select"}};
    } else {
        hints = {{"A", "Enter", "Select"}, {"B", "Esc", "Back"}};
    }
    if (pad && !popup) {
        hints.push_back({"LB", "", "Prev page"});
        hints.push_back({"RB", "", "Next page"});
        if (A.page != Page::Library) hints.push_back({"LT", "", "Scroll up"}), hints.push_back({"RT", "", "Scroll down"});
    }
    if (!popup) hints.push_back(fr_invite_toast_visible() ? Hint{"Start", "", "Join invite"} : Hint{"Start", "F2", "Friends"});
    if (!popup && pad && fr_invite_toast_visible()) hints.push_back({"View", "", "Friends"});

    std::string status;
    ImU32 dot = ui::TEXT3;
    if (A.proc) { status = "Playing " + A.proc_title; dot = ui::ACCENT; }
    else if (A.in_room) { status = "Room " + A.code + "  -  " + A.lobby.status(); dot = A.lobby.connected() ? ui::GOOD : ui::WARN; }
    else if (A.job.running) { status = "Syncing saves..."; dot = ui::ACCENT; }
    ImFont* cf = ui::F.cap;
    float sw = 0;
    if (!status.empty()) {
        status = ui::ellipsize(cf, cf->FontSize, status, std::min(w * 0.4f, sp(48.f)));
        sw = cf->CalcTextSizeA(cf->FontSize, FLT_MAX, 0.f, status.c_str()).x + sp(2.f);
    }
    float hy = p.y + (h - (cf->FontSize + sp(0.75f))) * 0.5f, x = p.x;
    for (auto& hn : hints) {
        bool is_shoulder = hn.pad[0] == 'L' || hn.pad[0] == 'R';
        if (!pad && (is_shoulder || hn.pad[0] == 'L' || !hn.key[0])) continue;
        float need = ui::key_hint_width(pad, hn.pad, hn.key, hn.label);
        if (x + need > p.x + w - sw - sp(2.f)) break;
        x += ui::key_hint(dl, ImVec2(x, hy), pad, hn.pad, hn.key, hn.label) + sp(3.f);
    }
    if (!status.empty()) {
        float tx = p.x + w - sw + sp(2.f);
        dl->AddCircleFilled(ImVec2(tx - sp(1.f), p.y + h * 0.5f), sp(0.5f), dot, 12);
        dl->AddText(cf, cf->FontSize, ImVec2(tx, p.y + (h - cf->FontSize) * 0.5f), ui::TEXT2, status.c_str());
    }
    ImGui::Dummy(ImVec2(w, h));
}

/* ---------- global input: back, page switching, quick actions ---------- */

void global_input() {
    ImGuiIO& io = ImGui::GetIO();
    if (!A.focused || A.proc) return;
    if (attract_woke_this_frame()) return;   // the press that woke attract mode only wakes it
    if (ImGui::IsPopupOpen((const char*)nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return;
    if (io.WantTextInput) return;
    if (wizard_state() >= 0) return;   // the setup wizard owns the pad (its own B / Back / Next); Menu must not toggle the friends panel behind it
    if (A.popup_last_frame && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))) return;   // that B just closed a menu/modal: not also "back"
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false) && fr_invite_toast_visible()) fr_join_invite_toast();   // Start joins a pending invite
    else if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadBack, false) || ImGui::IsKeyPressed(ImGuiKey_F2, false)) fr_toggle_panel();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
        if (fr_handle_back()) return;
    int dir = 0;
    if (FR.panel_focused) {}
    else if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) dir = 1;
    if (!FR.panel_focused && ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) dir = -1;
    if (dir && A.page == Page::Settings) {   // Settings owns LB/RB for its tabs; past either end it carries on to the neighbouring page
        int nt, pg = hubui::settings_tab_step(A.st.settings_tab, dir, &nt);
        if (pg == 0) { A.st.settings_tab = nt; A.page_focus_req = true; mark_dirty(); }
        else A.page = static_cast<Page>((6 + (int)A.page + pg) % 6);
    } else if (dir) A.page = static_cast<Page>((6 + (int)A.page + dir) % 6);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
        if (A.page == Page::Library) {
            if (A.view == LibView::All) { A.view = LibView::Home; A.search_buf[0] = 0; A.sys_filter.clear(); A.vis_dirty = true; }
        } else {
            A.page = Page::Library;
        }
    }
    if (A.page == Page::Library && !A.lib.empty() && ImGui::IsKeyPressed(ImGuiKey_GamepadR2, false)) cycle_sort(1);
    if (A.page == Page::Library && !A.lib.empty() && A.sel_game.empty() && find_ghost(A.sel_ghost) &&
        (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false) || (ImGui::IsKeyPressed(ImGuiKey_I, false) && !io.KeyCtrl))) A.show_ghost = true;
    if (A.page == Page::Library && !A.lib.empty() && find_entry(A.sel_game)) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false) || (ImGui::IsKeyPressed(ImGuiKey_I, false) && !io.KeyCtrl)) A.show_details = true;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false) || (ImGui::IsKeyPressed(ImGuiKey_F, false) && !io.KeyCtrl)) toggle_favorite(A.sel_game);
        if (!io.WantTextInput && (ImGui::IsKeyPressed(ImGuiKey_GamepadR3, false) || (ImGui::IsKeyPressed(ImGuiKey_S, false) && !io.KeyCtrl)))
            custom_open_picker(A.sel_game, false);   // tile action: Add to shelf (R3 on a pad, S on a keyboard)
    }
}

/* ---------- frame ---------- */

void draw_frame() {
    ImGuiIO& io = ImGui::GetIO();
    theme_tick();
    fr_anim_panel();
    const bool fr_docked = io.DisplaySize.x >= 1100.f * S;
    const float fr_pw = fr_panel_w(io.DisplaySize.x);
    const float dock_w = fr_docked ? fr_pw * FR.panel_t : 0.f;
    const float view_w = io.DisplaySize.x - dock_w;
    ui_sound::configure(A.st.menu_sounds, A.st.menu_volume, A.st.menu_music, A.st.notif_sound);
    ui_sound::tick(A.proc != nullptr, g_headless ? false : (SDL_GetWindowFlags(A.win) & SDL_WINDOW_INPUT_FOCUS) != 0);
    A.focused = g_headless || (SDL_GetWindowFlags(A.win) & SDL_WINDOW_INPUT_FOCUS) != 0;
    if (A.page != A.last_page || A.view != A.last_view) { if (A.page != A.last_page) A.page_focus_req = true; A.last_page = A.page; A.last_view = A.view; A.nav_seed = true; }
    {   // a modal/menu just closed and the pad lost its focus: land on the page's default item again
        static bool was_popup = false;
        bool pop = ImGui::IsPopupOpen((const char*)nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        if (was_popup && !pop && A.pad_mode && !ImGui::GetCurrentContext()->NavId && !FR.panel_focused) { A.nav_seed = true; A.page_focus_req = true; }
        if (pop && !was_popup) ui_sound::play(ui_sound::Sfx::Popup);
        was_popup = pop;
    }
    /* D-pad zones: the sidebar and the content are separate focus zones, otherwise ImGui's scoring happily jumps diagonally
     * from a hero button to a sidebar entry. Left with nothing further left enters the sidebar, Right leaves it. */
    bool in_side = false;
    {
        ImGuiContext& g = *ImGui::GetCurrentContext();
        for (ImGuiID id : A.side_ids_prev) if (id == g.NavId && id != 0) in_side = true;
        if (A.zone_req == 1) { in_side = true; A.seed_side = true; }
        if (A.zone_req == 2) { in_side = false; A.nav_seed = true; }
        A.zone_req = 0;
        if (A.nav_seed) in_side = false;
        A.side_ids_prev.swap(A.side_ids);
        A.side_ids.clear();
    }
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(view_w, io.DisplaySize.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar(2);

    /* ---- sidebar (icons + labels; shrinks to icons only when narrow) ---- */
    bool collapsed = tvmode::force_icon_sidebar() || hubui::sidebar_collapsed(A.st.sidebar_mode, view_w, 1040.f * S);
    float full_w = sp(29.f), mini_w = sp(9.f), target = collapsed ? mini_w : full_w;
    if (A.side_w <= 0) A.side_w = target;
    ui::anim(A.side_w, target, 14.f);
    float sw = A.side_w, expand = std::max(0.f, std::min(1.f, (sw - mini_w) / (full_w - mini_w)));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::SIDEBAR);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(1.5f), sp(3.f)));
    ImGui::BeginChild("side", ImVec2(sw, 0), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* sdl = ImGui::GetWindowDrawList();
    ImGui::PushItemFlag(ImGuiItemFlags_NoNavDefaultFocus, true);   // the first D-pad press should land in the content, not the sidebar
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, !in_side);
    float iw = sw - sp(3.f);
    {   // logo mark + wordmark
        ImVec2 p = ImGui::GetCursorScreenPos();
        float ms = sp(5.f);
        ImVec2 m0(p.x + sp(0.5f), p.y), m1(m0.x + ms, m0.y + ms);
        ui_draw_brand_logo(sdl, m0, ms);
        if (expand > 0.02f) {
            sdl->PushClipRect(ImVec2(m1.x, p.y), ImVec2(p.x + iw, p.y + ms), true);
            sdl->AddText(ui::F.h2, ui::F.h2->FontSize, ImVec2(m1.x + sp(1.25f), p.y + (ms - ui::F.h2->FontSize) * 0.5f), ui::scale_alpha(ui::TEXT, expand), "RetroHub");
            sdl->PopClipRect();
        }
        ImGui::Dummy(ImVec2(iw, ms + sp(3.f)));
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, sp(0.5f)));
    ImU32 room_badge = A.in_room ? (A.lobby.connected() ? ui::GOOD : ui::WARN) : 0;
    if (nav_item(Page::Library, "Library", ui::Icon::Home, iw, expand, 0)) A.page = Page::Library;
    if (nav_item(Page::Online, A.in_room ? "Play Online  (in room)" : "Play Online", ui::Icon::Globe, iw, expand, room_badge)) A.page = Page::Online;
    const int cloud_conflicts = cloud_ui_conflict_count();
    {
        std::string saves_label = cloud_conflicts ? "Saves  (" + std::to_string(cloud_conflicts) + " to resolve)" : A.job.running ? "Saves  (syncing...)" : "Saves";
        if (nav_item(Page::Saves, saves_label.c_str(), ui::Icon::Cloud, iw, expand, cloud_conflicts ? ui::BAD : A.job.running ? ui::ACCENT : 0)) A.page = Page::Saves;
    }
    if (nav_item(Page::Controllers, "Controllers", ui::Icon::Gamepad, iw, expand, 0)) A.page = Page::Controllers;
    if (nav_item(Page::Settings, "Settings", ui::Icon::Gear, iw, expand, 0)) A.page = Page::Settings;
    if (nav_item(Page::Changelog, "What's new", ui::Icon::Star, iw, expand, 0)) A.page = Page::Changelog;
    if (nav_item(Page::About, "About / Updates", ui::Icon::Info, iw, expand, 0)) A.page = Page::About;
    ImGui::PopStyleVar();

    // bottom: collapse toggle + player card
    float card_h = sp(7.f), bs = sp(4.f);
    ImVec2 wp = ImGui::GetWindowPos();
    float bottom = wp.y + ImGui::GetWindowHeight() - sp(3.f);
    float left = wp.x + sp(1.5f);
    float icx = left + sp(3.f);
    {
        float xl = icx - bs * 0.5f, xr = left + iw - bs;
        ImGui::SetCursorScreenPos(ImVec2(xl + (xr - xl) * expand, bottom - card_h - sp(1.f) - bs));
        if (ui::icon_button("side_toggle", collapsed ? ui::Icon::ChevronR : ui::Icon::ChevronL, bs, false, collapsed ? "Expand sidebar" : "Collapse sidebar")) {
            A.st.sidebar_mode = collapsed ? 1 : 2;
            mark_dirty();
        }
    }
    {
        ImVec2 p(left, bottom - card_h);
        sdl->AddRectFilled(p, p + ImVec2(iw, card_h), ui::scale_alpha(ui::PANEL, expand), sp(1.5f));
        sdl->AddRect(p, p + ImVec2(iw, card_h), ui::scale_alpha(ui::HAIRLINE, expand), sp(1.5f), 0, 1.f);
        ImVec2 ac(left + sp(3.f), p.y + card_h * 0.5f);
        sdl->AddCircleFilled(ac, sp(2.25f), ui::ACCENT, 24);
        std::string nm = A.st.name.empty() ? "?" : A.st.name;
        char init[8] = {(char)std::toupper((unsigned char)nm[0]), 0};
        ImVec2 its = ui::F.bold->CalcTextSizeA(ui::F.bold->FontSize, FLT_MAX, 0.f, init);
        sdl->AddText(ui::F.bold, ui::F.bold->FontSize, ac - its * 0.5f, IM_COL32_WHITE, init);
        const char* stt = A.proc ? "In game" : (A.in_room ? (A.lobby.connected() ? "In a room" : "Connecting") : "Offline");
        ImU32 sc = A.proc ? ui::ACCENT : (A.in_room ? (A.lobby.connected() ? ui::GOOD : ui::WARN) : ui::TEXT3);
        sdl->AddCircleFilled(ac + ImVec2(sp(1.6f), sp(1.6f)), sp(0.6f), sc, 12);
        sdl->AddCircle(ac + ImVec2(sp(1.6f), sp(1.6f)), sp(0.6f), ui::SIDEBAR, 12, 2.f);
        if (expand > 0.05f) {
            sdl->PushClipRect(p, p + ImVec2(iw, card_h), true);
            float tx = left + sp(6.f), mw = iw - sp(6.f) - sp(1.f);
            sdl->AddText(ui::F.bold, ui::F.bold->FontSize, ImVec2(tx, p.y + sp(1.25f)), ui::scale_alpha(ui::TEXT, expand),
                         ui::ellipsize(ui::F.bold, ui::F.bold->FontSize, nm, mw).c_str());
            sdl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(tx, p.y + sp(1.25f) + ui::F.bold->FontSize + sp(0.25f)), ui::scale_alpha(ui::TEXT2, expand), stt);
            sdl->PopClipRect();
        }
    }
    ImGui::PopItemFlag();
    ImGui::PopItemFlag();
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    ImGui::SameLine(0, 0);

    /* ---- content ---- */
    float hpad = (view_w - sw > 1200.f * S) ? sp(5.f) : sp(4.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(hpad, sp(3.f)));
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, in_side);
    ImGui::BeginChild("content", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    fr_top_bar();
    if (A.proc) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x, h = sp(5.f);
        dl->AddRectFilled(p, p + ImVec2(w, h), mix(ui::PANEL, ui::ACCENT, 0.22f), sp(1.5f));
        dl->AddRect(p, p + ImVec2(w, h), with_alpha(ui::ACCENT, 0.5f), sp(1.5f), 0, 1.f);
        float pulse = 0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 4.f);
        dl->AddCircleFilled(p + ImVec2(sp(2.f), h * 0.5f), sp(0.6f) + pulse * sp(0.25f), ui::ACCENT, 16);
        std::string m = A.proc_title + " is running - the hub is ready again when it closes.";
        dl->AddText(ui::F.body, ui::F.body->FontSize, p + ImVec2(sp(3.5f), (h - ui::F.body->FontSize) * 0.5f),
                    ui::TEXT, ui::ellipsize(ui::F.body, ui::F.body->FontSize, m, w - sp(5.f)).c_str());
        ImGui::Dummy(ImVec2(w, h + sp(2.f)));
    }

    float footer_h = sp(5.5f);
    ImGuiWindowFlags pf = A.page == Page::Library ? (ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse) : ImGuiWindowFlags_None;
    ImGui::BeginChild("page", ImVec2(0, -footer_h), ImGuiChildFlags_NavFlattened, pf);
    if (A.page_focus_req) {   // arrived on a page (LB/RB, sidebar, B): put the focus on its first item unless the user is in the sidebar
        A.page_focus_req = false;
        if (!in_side && !io.WantTextInput) {
            ImGuiContext& g = *ImGui::GetCurrentContext();
            if (g.NavWindow != g.CurrentWindow) ImGui::FocusWindow(g.CurrentWindow);   // the previous page's panels are gone: without this ImGui falls back to the top bar
            if (A.page != Page::Library) {   // the library seeds its own focus (hero Play); elsewhere the first item submitted from here on takes it
                ImGui::SetNavID(0, ImGuiNavLayer_Main, g.CurrentWindow->NavRootFocusScopeId, ImRect());
                g.NavInitRequest = true;
                g.NavInitRequestFromMove = false;
                g.NavInitResult.ID = 0;
                g.NavAnyRequest = true;
            }
        }
    }
    switch (A.page) {
        case Page::Library: page_library(); break;
        case Page::Online: page_online(); break;
        case Page::Saves: page_saves(); break;
        case Page::Controllers: page_controllers(); break;
        case Page::Settings: page_settings(); break;
        case Page::About: page_about(); break;
        case Page::Changelog: page_changelog(); break;
    }
    ImGui::EndChild();
    draw_footer(footer_h);
    ImGui::EndChild();
    ImGui::PopItemFlag();

    {   // a Left/Right move that found no neighbour hands the focus to the other zone
        ImGuiContext& g = *ImGui::GetCurrentContext();
        bool miss = g.NavMoveSubmitted && g.NavMoveResultLocal.ID == 0 && g.NavMoveResultOther.ID == 0;
        if (miss && !g.IO.WantTextInput && !ImGui::IsPopupOpen((const char*)nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
            if (g.NavMoveDir == ImGuiDir_Left && !in_side) A.zone_req = 1;
            else if (g.NavMoveDir == ImGuiDir_Right && in_side) A.zone_req = 2;
        }
    }

    tvmode::draw_corner(view_w - hpad - sp(10.5f), sp(3.f), sp(4.5f));
    draw_overlays();
    global_input();
    A.popup_last_frame = ImGui::IsPopupOpen((const char*)nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    draw_toasts(io.DisplaySize.y - footer_h - sp(1.5f), view_w - sp(3.f), sp(7.5f));
    ImGui::End();
    fr_draw_panel(io.DisplaySize.x, io.DisplaySize.y);
    fr_draw_toasts(io.DisplaySize.x - (fr_docked ? fr_pw * FR.panel_t : (A.st.show_friends ? fr_pw * FR.panel_t : 0.f)) - sp(2.f),
                   io.DisplaySize.y - footer_h - sp(1.5f) - (A.toasts.empty() ? 0.f : sp(9.f)));
}

