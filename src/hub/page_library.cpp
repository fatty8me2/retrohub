#include "app.h"
#include "wiiu_meta.h"
#include "homefun.h"

void go_add_rom(const GameInfo& g) {
    settings_open(hubui::ST_General);
    push_toast("Put your " + g.title + " ROM in a ROM folder, then press Rescan.", ui::ACCENT, 7.0);
}

void home_shelves() {
    float avail = ImGui::GetContentRegionAvail().x;
    float gap = sp(2.f), bleed = sp(1.5f);
    float max_w = std::max(sp(25.f), std::min(sp(37.5f), avail * 0.2f));
    float tw = hubui::shelf_tile_w(avail, sp(24.f), max_w, gap);
    float th = tw / kTileAspect;
    /* Home order: Resume hero (above), Continue playing, Friends are playing, the other shelves, Your stats. */
    int recent_si = -1;
    for (size_t si = 0; si < A.shelves.size(); ++si) if (A.shelves[si].id == "recent") { recent_si = (int)si; break; }
    if (recent_si < 0) fr_home_shelf(avail, tw, th, gap, bleed);
    for (size_t si = 0; si < A.shelves.size(); ++si) {
        const hubui::Shelf& sh = A.shelves[si];
        bool is_console = sh.id.compare(0, 8, "console:") == 0 && sh.id != "console:other";   // unknown systems have no filter chip
        bool see_all = false;
        bool overflow = hubui::shelf_max_scroll((int)sh.items.size() + (si == 0 ? 1 : 0), tw, gap, bleed, avail) > 0;
        shelf_header(sh.title.c_str(), is_console ? "See all" : "", bleed, (int)sh.items.size(), is_console ? &see_all : nullptr, overflow);
        if (see_all) {   // jump to All Games filtered to this console
            A.view = LibView::All;
            A.sys_filter = sh.id.substr(8);
            A.online_only = false;
            A.grid_scroll_top = true;
            A.search_buf[0] = 0;
            A.vis_dirty = true;
        }
        bool recent = sh.id == "recent";
        const int lead = si == 0 ? 1 : 0;   // the first shelf starts with the "Add your own games" tile
        shelf_strip("shelf_" + sh.id, (int)sh.items.size() + lead, tw, th, [&](int i, ImVec2 sz) {
            if (lead && i == 0) return add_games_tile(sz);
            i -= lead;
            if (sh.items[i] >= (int)A.lib.size()) {   // a friend's game you do not own
                const hubui::fg::Ghost& g = A.ghosts[sh.items[i] - A.lib.size()];
                TileSpec gs = spec_for_ghost(g, sh.id.c_str(), !is_console);
                TileResult gr = draw_tile(gs, sz);
                handle_ghost_result(g, gr);
                return gr;
            }
            const hub::GameEntry& e = A.lib[sh.items[i]];
            TileSpec spec = spec_for_entry(e, sh.id.c_str(), recent, !is_console);
            TileResult r = draw_tile(spec, sz);
            handle_entry_result(e, spec, r);
            return r;
        });
        ImGui::Dummy(ImVec2(0, sp(1.f)));
        if ((int)si == recent_si) fr_home_shelf(avail, tw, th, gap, bleed);
    }
    custom_home_shelves(avail, tw, th, gap, bleed);   // the user's own shelves, after the built-in ones (customize_ui.cpp)
    if (!A.pick_idx.empty()) {   // upsell: below the console shelves, collapsed until asked for
        const std::vector<GameInfo>& cat = hub::catalog();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + bleed);
        std::string tl = std::string(A.picks_open ? "Hide" : "Show") + " top picks you don't have yet  (" + std::to_string(A.pick_idx.size()) + ")";
        if (button(tl.c_str(), ImVec2(0, 0), false, true, A.picks_open ? ui::Icon::ChevronL : ui::Icon::ChevronR)) A.picks_open = !A.picks_open;
        ImGui::Dummy(ImVec2(0, sp(1.f)));
    }
    if (!A.pick_idx.empty() && A.picks_open) {
        const std::vector<GameInfo>& cat = hub::catalog();
        shelf_header("Top Picks You Don't Have Yet", "add the ROM to a ROM folder", bleed, (int)A.pick_idx.size(), nullptr,
                     hubui::shelf_max_scroll((int)A.pick_idx.size(), tw, gap, bleed, avail) > 0);
        shelf_strip("shelf_picks", (int)A.pick_idx.size(), tw, th, [&](int i, ImVec2 sz) {
            const GameInfo& g = cat[A.pick_idx[i]];
            TileSpec t;
            t.id = "pick|" + g.id;
            t.title = g.title;
            hub::System sy = hub::system_from_short(g.system);
            t.badge = sys_badge(sy);
            system_colors(sy, &t.c0, &t.c1);
            t.art = art_for_pick(g);
            t.owned = false;
            t.cta = "Add ROM";
            t.online = g.online;
            t.subtitle = g.year ? std::to_string(g.year) + " - not in your library" : std::string("Not in your library");
            TileResult r = draw_tile(t, sz);
            if (r.clicked || r.activated) go_add_rom(g);
            return r;
        });
        ImGui::Dummy(ImVec2(0, sp(1.f)));
    }
    achievements_home_strip(avail, bleed);   // Recent unlocks (RetroAchievements; only when signed in and something unlocked)
    stats_ui_card();   // Your stats, below the shelves
    ImGui::Dummy(ImVec2(0, sp(7.f)));   // room so the last shelf's covers clear the footer when scrolled to the end
}

/* ---------- hero ---------- */

/* Gives the item just submitted the (not yet visible) nav focus. ImGui's own first-press behaviour would otherwise
 * land on whatever is closest to the window corner (the search box). Called once per page/view change. */
void set_nav_here() {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    if (!g.NavWindow) return;
    ImVec2 off = g.NavWindow->DC.CursorStartPos;
    ImRect rel(ImGui::GetItemRectMin() - off, ImGui::GetItemRectMax() - off);
    ImGui::SetNavID(ImGui::GetItemID(), ImGuiNavLayer_Main, g.CurrentFocusScopeId, rel);
}
void seed_nav_here() {
    if (!A.nav_seed || ImGui::GetIO().WantTextInput) return;
    A.nav_seed = false;
    set_nav_here();
}

float action_row(const hub::GameEntry& e, float bh, bool draw, ImVec2 at, bool compact, bool seed_nav) {
    bool idle = A.proc == nullptr;
    const hub::GameState& gs = gstate(e.key);
    bool gc = e.system == hub::System::GC;
    bool online = supports_online(e) || gc;
    if (gc && supports_online(e)) gc = false;   // online-capable GameCube games get the normal Play Online button
    float gap = sp(1.5f), play_w = sp(compact ? 15.f : 18.f), on_w = compact ? bh : sp(21.f);
    float total = play_w;
    if (online) total += gap + on_w;
    total += (gap + bh) * 2;
    if (!draw) return total;
    ImGui::SetCursorScreenPos(at);
    const bool resume = has_resume(e);   // one press: launch and load the auto state (plain Play when there is none)
    if (button(resume ? "Resume" : "Play", ImVec2(play_w, bh), true, idle, ui::Icon::Play)) { hub::GameEntry c = e; if (resume) play_entry_anim(c, true, [](const hub::GameEntry& g) { resume_entry(g, -1); }); else play_entry_anim(c, true); }
    if (resume && ImGui::IsItemHovered()) ImGui::SetTooltip("Continue from where you left off");
    if (seed_nav) seed_nav_here();
    if (online) {
        ImGui::SameLine(0, gap);
        if (gc) {
            button(compact ? "##onsoon" : "Online soon", ImVec2(on_w, bh), false, false, ui::Icon::Globe);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Online via Dolphin's own netplay - coming soon");
        } else if (button(compact ? "##online" : "Play Online", ImVec2(on_w, bh), false, idle, ui::Icon::Globe)) {
            A.pending_room_game = room_id(e);
            enter_room(true, hubnet::new_room_code());
        }
        if (compact && ImGui::IsItemHovered()) ImGui::SetTooltip("Play online with friends");
    }
    ImGui::SameLine(0, gap);
    if (ui::icon_button("fav", gs.favorite ? ui::Icon::StarFilled : ui::Icon::Star, bh, gs.favorite, gs.favorite ? "Remove from favorites" : "Add to favorites", IM_COL32(236, 180, 40, 255)))
        toggle_favorite(e.key);
    ImGui::SameLine(0, gap);
    if (ui::icon_button("more", ui::Icon::Dots, bh, false, "Details")) A.show_details = true;
    return total;
}

void draw_hero(const hub::GameEntry& e, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + w, p0.y + h);
    float rad = sp(2.5f), ha = A.hero_t;
    const hub::GameState& gs = gstate(e.key);
    const GameInfo* cat = catalog_for(e);
    ImU32 c0, c1;
    entry_colors(e, &c0, &c1);
    ArtRef art = art_for_entry(e);
    theme_hero_art(art.tex);
    bool compact = h < sp(29.f);

    ui::shadow(dl, p0, p1, rad, 1.3f);
    gradient_round_rect(dl, p0, p1, mix(c0, ui::HERO_BASE, 0.68f), mix(c1, ui::HERO_BASE, 0.78f), rad);
    if (art.tex) ui::draw_art_bg(dl, art.tex, art.w, art.h, p0, p1, rad, 0.20f, ha);
    ui::hfade_round_rect(dl, p0, p1, ui::SCRIM, rad, 0, 0.96f, 0.66f);
    ui::vfade_round_rect(dl, ImVec2(p0.x, p0.y + h * 0.4f), p1, ui::SCRIM, rad, ImDrawFlags_RoundCornersBottom, 0.f, 0.6f);
    dl->AddRect(p0, p1, ui::HAIRLINE, rad, 0, 1.f);

    float pad = compact ? sp(2.5f) : sp(3.f), bh = compact ? sp(5.f) : sp(6.f);
    float inner_h = h - pad * 2;
    float art_h = inner_h, art_w = art_h * kTileAspect;
    if (art_w > w * 0.32f) { art_w = w * 0.32f; art_h = art_w / kTileAspect; }
    ImVec2 a0(p0.x + pad, p0.y + pad + (inner_h - art_h) * 0.5f), a1(a0.x + art_w, a0.y + art_h);
    float arad = sp(1.75f);
    ui::shadow(dl, a0, a1, arad, 1.4f);
    TileSpec ts;
    ts.title = e.title;
    ts.c0 = c0;
    ts.c1 = c1;
    if (art.tex) ui::draw_art(dl, art.tex, art.w, art.h, a0, a1, arad, 1.f, ha);
    else draw_no_art(dl, ts, a0, a1, arad, 1.f);
    if (art.loading) ui::shimmer(dl, a0, a1, arad, IM_COL32(0, 0, 0, 0));
    dl->AddRect(a0, a1, ui::HAIRLINE, arad, 0, 1.f);

    float tx = a1.x + pad, tw = p1.x - pad - tx, ty = p0.y + pad;
    ImFont* cf = ui::F.cap;
    std::string eyebrow = gs.last_played ? hubui::resume_line(e.title, unix_now(), gs.last_played, "  ·  ") : std::string("Ready to play");
    eyebrow = ui::ellipsize(cf, cf->FontSize, eyebrow, tw);
    shot_trace_once("hero", e.key + " " + eyebrow + (has_resume(e) ? " [resume]" : " [play]"));
    dl->AddText(cf, cf->FontSize, ImVec2(tx, ty), ui::scale_alpha(mix(c1, IM_COL32_WHITE, 0.45f), ha), eyebrow.c_str());
    ty += cf->FontSize + sp(0.75f);

    ImFont* tf = compact ? ui::F.title : ui::F.hero;
    std::vector<std::string> lines = ui::wrap_lines(tf, tf->FontSize, e.title, tw, compact ? 1 : 2);
    if (lines.size() == 1 && tf == ui::F.hero && ui::F.title != ui::F.hero &&
        tf->CalcTextSizeA(tf->FontSize, FLT_MAX, 0.f, e.title.c_str()).x > tw) {
        tf = ui::F.title;
        lines = ui::wrap_lines(tf, tf->FontSize, e.title, tw, 2);
    }
    for (auto& ln : lines) {
        ui::text_shadowed(dl, tf, tf->FontSize, ImVec2(tx, ty), ui::scale_alpha(IM_COL32_WHITE, ha), ln.c_str());
        ty += tf->FontSize * 1.08f;
    }
    ty += sp(0.5f);

    // meta pills
    {
        float x = tx, ph = cf->FontSize + sp(1.f);
        auto put = [&](const std::string& s, ImU32 bg) {
            float pw = cf->CalcTextSizeA(cf->FontSize, FLT_MAX, 0.f, s.c_str()).x + sp(2.5f);
            if (x + pw > tx + tw) return;
            pill_at(dl, ImVec2(x, ty), s.c_str(), ui::scale_alpha(bg, ha), ui::scale_alpha(IM_COL32_WHITE, ha));
            x += pw + sp(0.75f);
        };
        put(hub::system_name(e.system), mix(c0, IM_COL32_BLACK, 0.3f));
        if (is_native(e)) put("Native port", IM_COL32(255, 255, 255, 30));
        if (cat && cat->year) put(std::to_string(cat->year), IM_COL32(255, 255, 255, 30));
        if (supports_online(e)) put("1-" + std::to_string(local_players_for(e)) + " players", IM_COL32(255, 255, 255, 30));
        if (gs.play_seconds) put(hubui::format_play_time(gs.play_seconds) + " played", IM_COL32(255, 255, 255, 30));
        ty += ph + sp(1.f);
    }

    float by = p1.y - pad - bh;
    if (!compact && cat && !cat->blurb.empty()) {
        float room = by - ty - sp(1.5f);
        int nl = (int)(room / (ui::F.body->FontSize * 1.25f));
        if (nl > 0) {
            auto bl = ui::wrap_lines(ui::F.body, ui::F.body->FontSize, cat->blurb, tw, std::min(2, nl));
            for (auto& ln : bl) {
                dl->AddText(ui::F.body, ui::F.body->FontSize, ImVec2(tx, ty), ui::scale_alpha(IM_COL32(152, 157, 175, 255), ha), ln.c_str());
                ty += ui::F.body->FontSize * 1.25f;
            }
        }
    }

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.3f + 0.7f * ha);
    action_row(e, bh, true, ImVec2(tx, by), compact || tw < sp(70.f), true);
    ImGui::PopStyleVar();
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
    ImGui::Dummy(ImVec2(w, sp(2.f)));
}

/* ---------- skeleton / empty states ---------- */

void skeleton_home() {
    float w = ImGui::GetContentRegionAvail().x, h = std::max(sp(24.f), std::min(sp(38.f), ImGui::GetContentRegionAvail().y * 0.4f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ui::shimmer(dl, p0, ImVec2(p0.x + w, p0.y + h), sp(2.5f), ui::PANEL);
    float aw = std::min((h - sp(6.f)) * kTileAspect, w * 0.3f);
    ui::shimmer(dl, ImVec2(p0.x + sp(3.f), p0.y + sp(3.f)), ImVec2(p0.x + sp(3.f) + aw, p0.y + h - sp(3.f)), sp(1.75f), ui::PANEL2);
    float tx = p0.x + sp(6.f) + aw;
    ui::shimmer(dl, ImVec2(tx, p0.y + sp(3.f)), ImVec2(tx + sp(12.f), p0.y + sp(4.f)), sp(0.5f), ui::PANEL2);
    ui::shimmer(dl, ImVec2(tx, p0.y + sp(5.f)), ImVec2(tx + sp(34.f), p0.y + sp(8.f)), sp(1.f), ui::PANEL2);
    ui::shimmer(dl, ImVec2(tx, p0.y + h - sp(9.f)), ImVec2(tx + sp(18.f), p0.y + h - sp(3.f)), sp(1.5f), ui::PANEL3);
    ImGui::Dummy(ImVec2(w, h + sp(2.f)));
    float tw = hubui::shelf_tile_w(w, sp(24.f), std::max(sp(25.f), std::min(sp(37.5f), w * 0.2f)), sp(2.f)), th = tw / kTileAspect;
    for (int row = 0; row < 2; ++row) {
        ImVec2 o = ImGui::GetCursorScreenPos();
        ui::shimmer(dl, o + ImVec2(sp(1.5f), 0), o + ImVec2(sp(1.5f) + sp(18.f), sp(2.5f)), sp(0.75f), ui::PANEL);
        float x = o.x + sp(1.5f), y = o.y + sp(4.f);
        for (int i = 0; x < o.x + w && i < 12; ++i, x += tw + sp(2.f))
            ui::shimmer(dl, ImVec2(x, y), ImVec2(x + tw, y + th), sp(1.75f), ui::PANEL);
        ImGui::Dummy(ImVec2(w, th + sp(7.f)));
    }
}

void centered_text(ImDrawList* dl, ImFont* f, float cx, float y, ImU32 col, const char* s) {
    ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.f, s);
    dl->AddText(f, f->FontSize, ImVec2(cx - ts.x * 0.5f, y), col, s);
}

void empty_library_state() {
    static const std::vector<std::string> suggestions = hub::suggest_rom_folders(3);   // scanned once, on first empty view
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float w = ImGui::GetContentRegionAvail().x;
    float h = std::min(ImGui::GetContentRegionAvail().y,
                       sp(53.f) + (suggestions.empty() ? 0.f : sp(10.f) + sp(6.f) * (float)suggestions.size()));
    ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + w, p0.y + h);
    dl->AddRectFilled(p0, p1, ui::PANEL, sp(2.5f));
    dl->AddRect(p0, p1, ui::HAIRLINE, sp(2.5f), 0, 1.f);
    float cx = (p0.x + p1.x) * 0.5f, y = p0.y + sp(3.f);
    float ir = sp(5.f);
    dl->AddCircleFilled(ImVec2(cx, y + ir), ir + sp(1.5f), ui::with_alpha(ui::ACCENT, 0.10f), 48);
    dl->AddCircleFilled(ImVec2(cx, y + ir), ir, ui::with_alpha(ui::ACCENT, 0.22f), 48);
    ui::icon(dl, ui::Icon::Gamepad, ImVec2(cx, y + ir), ir * 1.2f, ui::mix(ui::ACCENT, IM_COL32_WHITE, 0.5f));
    y += ir * 2 + sp(3.f);
    centered_text(dl, ui::F.title, cx, y, ui::TEXT, "Let's find your games");
    y += ui::F.title->FontSize + sp(1.f);
    float tw = std::min(w - sp(6.f), sp(62.f));
    auto lines = ui::wrap_lines(ui::F.body, ui::F.body->FontSize,
                                "RetroHub doesn't include any games. Point it at the folder(s) that hold your own game files and "
                                "everything is recognised by its contents, not its name.",
                                tw, 3);
    for (auto& ln : lines) {
        centered_text(dl, ui::F.body, cx, y, ui::TEXT2, ln.c_str());
        y += ui::F.body->FontSize * 1.3f;
    }
    y += sp(1.5f);
    // supported systems
    {
        struct Sy { const char* label; hub::System s; };
        static const Sy sys[] = {{"N64", hub::System::N64}, {"GameCube", hub::System::GC}, {"GBA", hub::System::GBA}, {"GBC", hub::System::GBC}, {"Game Boy", hub::System::GB},
                                {"Nintendo DS", hub::System::NDS},
                                {"3DS", hub::System::N3DS}, {"Wii U", hub::System::WiiU}, {"PS2", hub::System::PS2},
                                {"NES", hub::System::NES}, {"SNES", hub::System::SNES}};
        float tot = 0;
        for (auto& s2 : sys) tot += ui::F.cap->CalcTextSizeA(ui::F.cap->FontSize, FLT_MAX, 0.f, s2.label).x + sp(2.5f) + sp(0.75f);
        float x = cx - (tot - sp(0.75f)) * 0.5f;
        for (auto& s2 : sys) {
            ImU32 a, b;
            system_colors(s2.s, &a, &b);
            x += pill_at(dl, ImVec2(x, y), s2.label, mix(a, IM_COL32_BLACK, 0.2f)) + sp(0.75f);
        }
        y += ui::F.cap->FontSize + sp(4.f);
    }
    float bw = sp(24.f);
    ImGui::SetCursorScreenPos(ImVec2(cx - bw * 0.5f, y));
    add_rom_folder(button("Add ROM folder", ImVec2(bw, sp(6.f)), true, true, ui::Icon::Folder));
    y += sp(7.f);
    ImGui::SetCursorScreenPos(ImVec2(cx - bw * 0.5f, y));
    if (button("Find my games...", ImVec2(bw, sp(4.5f)), false, !A.scanning, ui::Icon::Search)) find_games_open();
    if (!suggestions.empty()) {
        y += sp(6.f) + sp(3.f) - sp(1.5f);
        centered_text(dl, ui::F.cap, cx, y, ui::TEXT2, "Found game files in these folders:");
        y += ui::F.cap->FontSize + sp(1.5f);
        for (size_t i = 0; i < suggestions.size(); ++i) {
            const std::string& s = suggestions[i];
            // show the last two path parts, e.g. "Desktop\Roms"
            size_t cut = s.find_last_of('\\');
            size_t cut2 = cut == std::string::npos || cut == 0 ? std::string::npos : s.find_last_of('\\', cut - 1);
            std::string label = "Add  " + (cut2 == std::string::npos ? s : s.substr(cut2 + 1));
            ImGui::PushID((int)i);
            ImGui::SetCursorScreenPos(ImVec2(cx - bw * 0.5f, y));
            if (button(label.c_str(), ImVec2(bw, sp(4.5f)), false, true, ui::Icon::Plus)) add_rom_folder_path(s);
            ImGui::PopID();
            y += sp(6.f);
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
    ImGui::Dummy(ImVec2(w, sp(1.f)));
}

/* ---------- top bar ---------- */

void library_top_bar() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::PushItemFlag(ImGuiItemFlags_NoNavDefaultFocus, true);   // D-pad focus starts on the hero / tiles, not here
    static const char* tabs[] = {"Home", "All Games"};
    int cur = A.view == LibView::Home ? 0 : 1;
    int n = ui::segmented("view", tabs, 2, cur);
    if (n != cur) {
        A.view = n == 0 ? LibView::Home : LibView::All;
        if (n == 0) A.search_buf[0] = 0;
    }
    ImGui::SameLine(0, sp(2.f));

    float bh = sp(5.f);
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) A.focus_search = true;
    if (A.focus_search) { ImGui::SetKeyboardFocusHere(); A.focus_search = false; }
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(sp(5.f), (bh - ImGui::GetFontSize()) * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, bh * 0.5f);
    const bool dice = A.view == LibView::Home && !A.lib.empty();   // L7: Surprise me
    float total = bh + sp(1.f) + add_games_header_width(bh) + (dice ? bh + sp(1.f) : 0.f);
    {   // the search box gives way so the pinned right-hand buttons (Add games...) are never pushed past the edge (narrow window / Friends panel open)
        const float chips = ImGui::CalcTextSize("Sort: Recently added").x + ImGui::CalcTextSize("Friends' games").x + sp(14.f) + sp(4.f);
        const float room = ImGui::GetContentRegionAvail().x - total - chips;
        ImGui::SetNextItemWidth(std::max(sp(10.f), std::min(std::min(sp(46.f), ImGui::GetContentRegionAvail().x * 0.36f), room)));
    }
    osk::input("##search", "Search games   (Ctrl+F)", A.search_buf, sizeof A.search_buf);
    ImVec2 s0 = ImGui::GetItemRectMin(), s1 = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRect(s0, s1, ImGui::IsItemActive() ? ui::ACCENT : ui::HAIRLINE, bh * 0.5f, 0, 1.f);
    ui::icon(ImGui::GetWindowDrawList(), ui::Icon::Search, ImVec2(s0.x + sp(2.5f), (s0.y + s1.y) * 0.5f), sp(2.f), ui::TEXT2);
    ImGui::PopStyleVar(2);
    if (A.search_buf[0] && A.view == LibView::Home) A.view = LibView::All;

    {   // sort chip: A cycles Title / Last played / Most played / Recently added (RT on the pad)
        ImGui::SameLine(0, sp(1.5f));
        std::string sl = std::string("Sort: ") + hubui::sort_name(hubui::sort_from_key(A.st.library_sort));
        if (chip(sl.c_str(), hubui::sort_from_key(A.st.library_sort) != hubui::SortMode::Title, ui::ACCENT)) cycle_sort(1);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Change the order of your games  (RT on a controller)");
    }
    {   // "Show friends' games": friends' titles you do not own, greyed out (metadata only)
        ImGui::SameLine(0, sp(1.f));
        if (chip("Friends' games", A.st.show_friend_games, ui::ACCENT)) { A.st.show_friend_games = !A.st.show_friend_games; mark_dirty(); A.vis_dirty = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Games your friends have that you don't, shown greyed out (%d now)", (int)A.ghosts.size());
    }
    ImGui::SameLine(0, 0);
    float ax = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, ax - total));
    if (dice) { surprise_ui_button(bh); ImGui::SameLine(0, sp(1.f)); }
    ImGui::BeginDisabled(A.scanning);
    if (ui::icon_button("rescan", ui::Icon::Refresh, bh, false, A.scanning ? "Scanning..." : "Rescan ROM folders")) start_scan();
    ImGui::EndDisabled();
    ImGui::SameLine(0, sp(1.f));
    add_games_header_button(bh);
    ImGui::PopItemFlag();

    // scan progress: thin animated line under the bar
    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 o = ImGui::GetCursorScreenPos();
    float lh = sp(0.25f);
    if (A.scanning) {
        int d = A.scan_done, t2 = A.scan_total;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(o.x, o.y + sp(0.5f)), ImVec2(o.x + w, o.y + sp(0.5f) + lh), ui::PANEL2, lh);
        if (t2 > 0) dl->AddRectFilled(ImVec2(o.x, o.y + sp(0.5f)), ImVec2(o.x + w * d / (float)t2, o.y + sp(0.5f) + lh), ui::ACCENT, lh);
        else {
            float ph = std::fmod((float)ImGui::GetTime() * 0.8f, 1.f);
            dl->AddRectFilled(ImVec2(o.x + w * ph * 0.8f, o.y + sp(0.5f)), ImVec2(o.x + w * (ph * 0.8f + 0.2f), o.y + sp(0.5f) + lh), ui::ACCENT, lh);
        }
        char buf[64];
        std::snprintf(buf, sizeof buf, t2 > 0 ? "Scanning your games...  %d / %d" : "Scanning your games...", d, t2);
        dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(o.x + sp(0.25f), o.y + sp(1.25f)), ui::TEXT3, buf);
    }
    ImGui::Dummy(ImVec2(w, sp(A.scanning ? 3.25f : 1.5f)));
}

/* ---------- All Games ---------- */

void selected_bar(const hub::GameEntry& e) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float w = ImGui::GetContentRegionAvail().x, h = sp(8.f);
    ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + w, p0.y + h);
    dl->AddRectFilled(p0, p1, ui::PANEL, sp(2.f));
    dl->AddRect(p0, p1, ui::HAIRLINE, sp(2.f), 0, 1.f);
    ImU32 c0, c1;
    entry_colors(e, &c0, &c1);
    ArtRef art = art_for_entry(e);
    float th = h - sp(2.f), tw = th * kTileAspect;
    ImVec2 a0(p0.x + sp(1.f), p0.y + sp(1.f)), a1(a0.x + tw, a0.y + th);
    TileSpec ts;
    ts.title = e.title;
    ts.c0 = c0;
    ts.c1 = c1;
    if (art.tex) ui::draw_art(dl, art.tex, art.w, art.h, a0, a1, sp(1.25f));
    else draw_no_art(dl, ts, a0, a1, sp(1.25f), 1.f);
    float bh = sp(5.f);
    float row_w = action_row(e, bh, false, ImVec2(0, 0), true);
    float tx = a1.x + sp(2.f);
    float text_w = std::max(sp(8.f), p1.x - sp(2.f) - row_w - tx - sp(2.f));
    const hub::GameState& gs = gstate(e.key);
    dl->AddText(ui::F.h2, ui::F.h2->FontSize, ImVec2(tx, p0.y + sp(1.25f)), ui::TEXT, ui::ellipsize(ui::F.h2, ui::F.h2->FontSize, e.title, text_w).c_str());
    std::string sub = std::string(hub::system_name(e.system)) + "  -  " + hubui::format_play_time(gs.play_seconds);
    dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(tx, p0.y + sp(1.25f) + ui::F.h2->FontSize + sp(0.5f)), ui::TEXT2,
                ui::ellipsize(ui::F.cap, ui::F.cap->FontSize, sub, text_w).c_str());
    action_row(e, bh, true, ImVec2(p1.x - sp(2.f) - row_w, p0.y + (h - bh) * 0.5f), true);
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
    ImGui::Dummy(ImVec2(w, 1));
}

void all_games_view() {
    // system chips
    struct Chip { const char* label; const char* sys; };
    static const Chip chips[] = {{"All", ""}, {"N64", "n64"}, {"GameCube", "gc"}, {"PlayStation", "ps1"}, {"Wii U", "wiiu"}, {"3DS", "3ds"},
                                {"Nintendo DS", "nds"}, {"PS2", "ps2"}, {"NES", "nes"}, {"SNES", "snes"}, {"GBA", "gba"}, {"GBC", "gbc"}, {"Game Boy", "gb"}};
    if (A.vis_dirty || A.vis_filter != A.sys_filter || A.vis_query != A.search_buf) rebuild_vis();
    std::map<std::string, int>& counts = A.chip_counts;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(sp(1.f), sp(1.f)));
    ImGui::PushItemFlag(ImGuiItemFlags_NoNavDefaultFocus, true);
    bool first = true;
    for (auto& c : chips) {
        int n = c.sys[0] ? counts[c.sys] : A.chip_total;
        if (c.sys[0] && n == 0 && A.sys_filter != c.sys) continue;
        if (!first) ImGui::SameLine();
        first = false;
        char lab[48];
        std::snprintf(lab, sizeof lab, "%s  %d", c.label, n);
        ImU32 a = COL_ACCENT, b2;
        if (c.sys[0]) system_colors(hub::system_from_short(c.sys), &a, &b2);
        if (chip(lab, A.sys_filter == c.sys, a)) { A.sys_filter = c.sys; A.vis_dirty = true; A.grid_scroll_top = true; }
    }
    {   // Online filter chip
        int n_online = 0;
        for (auto& e : A.lib) {
            const GameInfo* c = catalog_for(e);
            (void)c; if (is_native(e) || supports_online(e)) ++n_online;
        }
        char lab[48];
        std::snprintf(lab, sizeof lab, "Online  %d", n_online);
        ImGui::SameLine(0, sp(2.f));
        if (chip(lab, A.online_only, ui::GOOD)) { A.online_only = !A.online_only; A.vis_dirty = true; }
    }
    {   // Favorites chip, Genre picker and Group by Genre (saved like library_sort)
        int n_fav = 0;
        for (auto& e : A.lib) if (gstate(e.key).favorite) ++n_fav;
        char lab[48];
        std::snprintf(lab, sizeof lab, "Favorites  %d", n_fav);
        ImGui::SameLine(0, sp(2.f));
        if (chip(lab, A.st.lib_favs_only, COL_ACCENT)) { A.st.lib_favs_only = !A.st.lib_favs_only; mark_dirty(); A.vis_dirty = true; A.grid_scroll_top = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Only your favorite games");
        ImGui::SameLine();
        std::string gl = A.st.lib_genre.empty() ? std::string("Genre: All") : "Genre: " + A.st.lib_genre;
        if (chip(gl.c_str(), !A.st.lib_genre.empty(), COL_ACCENT)) ImGui::OpenPopup("##genre_pick");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show one genre only");
        ImGui::SameLine();
        if (chip(A.st.lib_group_genre ? "Group: Genre" : "Group: Console", A.st.lib_group_genre, COL_ACCENT)) {
            A.st.lib_group_genre = !A.st.lib_group_genre; mark_dirty(); A.vis_dirty = true; A.grid_scroll_top = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Group the library by console or by genre");
        if (ImGui::BeginPopup("##genre_pick")) {
            if (ImGui::Selectable("All genres", A.st.lib_genre.empty())) { A.st.lib_genre.clear(); mark_dirty(); A.vis_dirty = true; A.grid_scroll_top = true; }
            for (auto& kv : A.genre_counts) {
                if (kv.first == "Other") continue;
                char row[96];
                std::snprintf(row, sizeof row, "%s  (%d)", kv.first.c_str(), kv.second);
                if (ImGui::Selectable(row, A.st.lib_genre == kv.first)) { A.st.lib_genre = kv.first; mark_dirty(); A.vis_dirty = true; A.grid_scroll_top = true; }
            }
            auto oi = A.genre_counts.find("Other");
            if (oi != A.genre_counts.end()) {
                char row[96];
                std::snprintf(row, sizeof row, "Other  (%d)", oi->second);
                if (ImGui::Selectable(row, A.st.lib_genre == "Other")) { A.st.lib_genre = "Other"; mark_dirty(); A.vis_dirty = true; A.grid_scroll_top = true; }
            }
            ImGui::EndPopup();
        }
    }
    ImGui::PopItemFlag();
    ImGui::PopStyleVar();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    if (A.vis_dirty || A.vis_filter != A.sys_filter || A.vis_query != A.search_buf) rebuild_vis();

    const hub::GameEntry* sel = find_entry(A.sel_game);
    const hubui::fg::Ghost* selg = sel ? nullptr : find_ghost(A.sel_ghost);
    float bar_h = (sel || selg) ? sp(8.f) + ImGui::GetStyle().ItemSpacing.y + 2.f : 0;   // selected bar + the gap above it
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(1.5f), sp(1.5f)));
    if (A.grid_scroll_top) { ImGui::SetNextWindowScroll(ImVec2(0.f, 0.f)); A.grid_scroll_top = false; }
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - sp(1.5f));   // the grid's padding is the cover inset: first cover lines up with the chips
    ImGui::BeginChild("grid", ImVec2(ImGui::GetContentRegionAvail().x + sp(1.5f), -bar_h), ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    A.page_scrollable = true;
    add_games_banner();
    if (A.vis.empty()) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 o = ImGui::GetCursorScreenPos();
        float cx = o.x + ImGui::GetContentRegionAvail().x * 0.5f;
        ui::icon(dl, ui::Icon::Search, ImVec2(cx, o.y + sp(6.f)), sp(5.f), ui::TEXT3);
        std::string msg = A.search_buf[0] ? std::string("No games match \"") + A.search_buf + "\"" : std::string("No games in this system yet");
        centered_text(dl, ui::F.h2, cx, o.y + sp(10.f), ui::TEXT, msg.c_str());
        centered_text(dl, ui::F.body, cx, o.y + sp(10.f) + ui::F.h2->FontSize + sp(1.f), ui::TEXT2, "Try a different spelling, or pick another system.");
        ImGui::Dummy(ImVec2(0, sp(17.f)));
        if (A.search_buf[0]) {
            float bw = sp(18.f);
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - bw) * 0.5f);
            if (button("Clear search", ImVec2(bw, sp(5.f)), false, true, ui::Icon::Close)) A.search_buf[0] = 0;
        }
    } else {
        // one section per console (fixed order), A-Z inside; A.vis already holds the search/console/online filters
        const std::vector<hubui::ConsoleSection>& secs = A.secs;
        float gap = sp(2.f);
        float avail = ImGui::GetContentRegionAvail().x;
        hubui::GridLayout gl = hubui::grid_layout(avail, sp(26.f), sp(37.5f), gap);
        ImVec2 sz(gl.tile_w, gl.tile_w / kTileAspect);
        for (auto& sec : secs) {
            if (sec.items.empty()) continue;
            shelf_header(sec.title.c_str(), "", 0.f, (int)sec.items.size());
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
            for (size_t k = 0; k < sec.items.size(); ++k) {
                if (k % gl.cols) ImGui::SameLine();
                if (sec.items[k] >= (int)A.lib.size()) {   // a friend's game you do not own
                    const hubui::fg::Ghost& g = A.ghosts[sec.items[k] - A.lib.size()];
                    TileSpec gs = spec_for_ghost(g, "grid", sec.system == "other");
                    TileResult gr = draw_tile(gs, sz);
                    handle_ghost_result(g, gr);
                    continue;
                }
                const hub::GameEntry& e = A.lib[sec.items[k]];
                TileSpec spec = spec_for_entry(e, "grid", false, sec.system == "other");
                TileResult r = draw_tile(spec, sz);
                handle_entry_result(e, spec, r);
            }
            ImGui::PopStyleVar();
            ImGui::Dummy(ImVec2(0, sp(1.5f)));
        }
        ImGui::Dummy(ImVec2(0, sp(2.f)));
    }
    ImGui::EndChild();
    if (sel) selected_bar(*sel);
    else if (selg) selected_bar_ghost(*selg);
}

/* ---------- Wii U details: updates / DLC, Cemu game profile, save accounts, key help ---------- */

static std::vector<std::string> wiiu_info_lines(const hub::GameEntry& e) {
    static std::string s_key;
    static double s_at = -100;
    static std::vector<std::string> s_lines;
    if (s_key == e.key && ImGui::GetTime() - s_at < 4.0) return s_lines;
    s_key = e.key;
    s_at = ImGui::GetTime();
    s_lines.clear();
    hubemu::Install inst = hubemu::find(hubemu::Kind::Cemu, A.st.cemu_exe, e.path);
    std::string game = launch_path_for(e);
    if (e.issue == "extract" && game == e.path) game.clear();   // still packed: nothing to read yet
    uint64_t tid = hubemu::title_id_from_key(e.key);
    if (!tid && !game.empty()) tid = hubemu::wiiu_title_id_of_file(game);
    hub::FsProbe fp = hubemu::wiiu_probe();
    std::string user_data, mlc;
    if (!inst.exe.empty()) {
        if (inst.managed) { user_data = inst.dir + "\\portable"; mlc = user_data + "\\mlc01"; }
        else {
            const char* ad = std::getenv("APPDATA");
            hub::CemuDirs d = hub::cemu_dirs_for(inst.dir, ad ? ad : "", fp);
            user_data = d.user_data;
            mlc = d.mlc;
        }
    }
    if (tid) {
        hub::WiiuAddons a = hub::wiiu_installed_addons(tid, mlc, fp);
        if (!game.empty() && game.size() > 4) {
            std::ifstream f(std::filesystem::u8path(game), std::ios::binary);
            std::error_code ec;
            uint64_t sz = std::filesystem::file_size(std::filesystem::u8path(game), ec);
            if (f && !ec) {
                hub::ReadAtFn rd = [&](uint64_t off, void* buf, size_t n) { f.clear(); f.seekg((std::streamoff)off); f.read((char*)buf, (std::streamsize)n); return f.gcount() == (std::streamsize)n; };
                hub::wiiu_addons_from_wua(hub::wua_titles(rd, sz), &a);
            }
        }
        s_lines.push_back("Updates and DLC: " + hub::wiiu_addons_text(a));
        if (!mlc.empty()) {
            auto rels = hub::wiiu_save_rels(tid);
            std::vector<std::string> acc = hub::wiiu_save_accounts(mlc + "/" + rels[0]);
            if (!acc.empty()) { std::string t = "Cemu save accounts:"; for (auto& x : acc) t += " " + x; s_lines.push_back(t); }
        }
        std::string prof = hubemu::cemu_profile_summary(inst.dir, user_data, tid);
        if (!prof.empty()) s_lines.push_back("Cemu profile: " + prof);
    }
    return s_lines;
}

/* ---------- details modal (Y / right-click / "...") ---------- */

void draw_details_modal() {
    draw_ghost_modal();   // a friend's game you do not own (friend_games_ui.cpp)
    if (A.show_details && !(g_headless && A.lib.empty())) {   // headless aids (HUB_SHOT_DETAILS) set this before the library exists: keep it pending until then
        A.show_details = false;
        if (find_entry(A.sel_game)) ImGui::OpenPopup("##details");
    }
    const hub::GameEntry* ep = find_entry(A.sel_game);
    ImGuiIO& io = ImGui::GetIO();
    float w = std::min(sp(104.f), io.DisplaySize.x * 0.92f);
    ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(3.f), sp(3.f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.5f));
    bool open = ImGui::BeginPopupModal("##details", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    if (!open) return;
    if (!ep || ui::modal_back()) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    const hub::GameEntry& e = *ep;
    const hub::GameState& gs = gstate(e.key);
    const GameInfo* cat = catalog_for(e);
    if (A.info_key != e.key || ImGui::GetTime() - A.info_at > 4.0) refresh_save_info(e);
    bool idle = A.proc == nullptr, native = is_native(e);
    ImU32 c0, c1;
    entry_colors(e, &c0, &c1);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    float avail = ImGui::GetContentRegionAvail().x;
    bool with_art = avail > sp(62.f);
    float aw = with_art ? sp(20.f) : 0, ah = aw / kTileAspect;
    if (with_art) {
        ArtRef art = art_for_entry(e);
        TileSpec ts;
        ts.title = e.title;
        ts.c0 = c0;
        ts.c1 = c1;
        ImVec2 a0 = o, a1(o.x + aw, o.y + ah);
        ui::shadow(dl, a0, a1, sp(1.75f), 1.2f);
        if (art.tex) ui::draw_art(dl, art.tex, art.w, art.h, a0, a1, sp(1.75f));
        else draw_no_art(dl, ts, a0, a1, sp(1.75f), 1.f);
        dl->AddRect(a0, a1, ui::HAIRLINE, sp(1.75f), 0, 1.f);
    }
    float tx = with_art ? aw + sp(3.f) : 0;
    ImGui::SetCursorScreenPos(ImVec2(o.x + tx, o.y));
    ImGui::BeginGroup();
    float tw = avail - tx;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tw);
    ImGui::PushFont(ui::F.title);
    ImGui::TextWrapped("%s", e.title.c_str());
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
    {   /* one short line of key facts */
        const char* dot = "  \xC2\xB7  ";
        std::string facts = hub::system_name(e.system);
        if (native) facts += std::string(dot) + "Native port";
        if (local_players_for(e) >= 2) facts += std::string(dot) + "1-" + std::to_string(local_players_for(e)) + " players";
        if (supports_online(e)) facts += std::string(dot) + "Online";
        if (cat && cat->year) facts += std::string(dot) + std::to_string(cat->year);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tw);
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
        ImGui::TextWrapped("%s", facts.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        std::string played = gs.last_played ? hubui::format_play_time(gs.play_seconds) + " played" + dot + "last played " + fmt_time(gs.last_played)
                                            : std::string("Not played yet");
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tw);
        ImGui::TextWrapped("%s", played.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::EndGroup();
    if (with_art) ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + std::max(ah, ImGui::GetItemRectSize().y) + sp(1.f)));
    {   /* short description (clamped, More expands); falls back to the built-in blurb when nothing loaded */
        bool drew = gameinfo_ui_section(e, cat ? cat->title : std::string(), 1);
        if (!drew && cat && !cat->blurb.empty()) {
            ImGui::PushTextWrapPos(0.f);
            ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
            ImGui::TextWrapped("%s", cat->blurb.c_str());
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
        }
    }
    if (e.issue == "extract") {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN);
        ImGui::TextWrapped("Archived: %s cannot open %s files. Press Play to unpack it (about %.1f GB) into RetroHub's data folder - your archive stays untouched.",
                           e.system == hub::System::PS2 ? "PCSX2" : e.system == hub::System::GC ? "Dolphin" : "Azahar", e.system == hub::System::GC || e.system == hub::System::PS2 ? "archive" : ".zip", unpacked_size(e) / 1073741824.0);
        ImGui::PopStyleColor();
    } else if (e.issue == "encrypted") {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN);
        ImGui::TextWrapped("This 3DS image is encrypted. Azahar needs your own AES keys (aes_keys.txt in its user\\sysdata folder) to run it.");
        ImGui::PopStyleColor();
    } else if (e.issue == "keys") {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN);
        std::string ud;
        hubemu::Install ki = hubemu::find(hubemu::Kind::Cemu, A.st.cemu_exe, e.path);
        if (!ki.exe.empty()) { const char* ad = std::getenv("APPDATA"); ud = ki.managed ? ki.dir + "\\portable" : hub::cemu_dirs_for(ki.dir, ad ? ad : "", hubemu::wiiu_probe()).user_data; }
        std::string why = hub::wiiu_keys_message(hub::wiiu_keys_check(ud, ki.dir, hubemu::wiiu_probe()), "wud");
        ImGui::TextWrapped("%s", why.empty() ? "Disc images (.wud/.wux) are decrypted with the disc keys in Cemu's keys.txt (found). A .wua archive needs no keys." : why.c_str());
        ImGui::PopStyleColor();
    }
    if (is_kanto(e)) {
        text_wrapped_disabled("A friend's two-player co-op version of Pokemon FireRed. It is played online: press Play to set it up (once), then host or join a room and pick it. Needs your own Pokemon FireRed (USA) 1.0 in the library.");
        if (button("How to play", ImVec2(sp(14.f), sp(4.f)))) kt_open_howto();
    }
    ImGui::Dummy(ImVec2(0, sp(1.f)));

    const float bh = sp(5.5f);
    const float bh2 = sp(4.5f);
    /* Action rows: each button joins the row only if it fits the modal width, otherwise it wraps to the next line. */
    const float row_gap = sp(1.5f), row_avail = ImGui::GetContentRegionAvail().x;
    float row_used = 0.f;
    bool row_started = false;
    auto place = [&](float w) {   // call before each button in the row
        if (!row_started) { row_started = true; row_used = w; return; }
        if (row_used + row_gap + w <= row_avail) { ImGui::SameLine(0, row_gap); row_used += row_gap + w; }
        else row_used = w;   // no SameLine: this button starts a new row
    };
    auto label_w = [&](const char* label, bool icon) {   // matches button(): text + icon + gap + 2.5sp padding each side
        return ImGui::CalcTextSize(label).x + (icon ? sp(2.25f) + sp(1) : 0.f) + sp(5);
    };
    /* Reachability QA (qa/reach_matrix.ps1): every action reports whether it is offered; HUB_SHOT_ACT=<name> presses it once. */
    static const char* act = std::getenv("HUB_SHOT_ACT");
    static std::string act_done;
    auto tr = [&](const char* name, const std::string& state) { shot_trace_once("act", e.key + " " + name + " " + state); };
    auto forced = [&](const char* name) {
        if (!act || std::strcmp(act, name) != 0 || act_done == e.key + name) return false;
        act_done = e.key + name;
        return true;
    };
    /* primary: Play (Resume when a resume point exists) + Play Online */
    const bool resume = !native && A.info_auto_state && e.issue.empty();
    const char* play_label = resume ? "Resume" : "Play";
    place(sp(17.f));
    tr("play", idle ? "enabled" : "disabled");
    if (button(play_label, ImVec2(sp(17.f), bh), true, idle, ui::Icon::Play) || forced("play")) { ImGui::CloseCurrentPopup(); hub::GameEntry c = e; play_entry_anim(c); }
    ImGui::SetItemDefaultFocus();
    if (!supports_online(e) && e.system == hub::System::GC) {
        place(label_w("Online soon", true));
        button("Online soon", ImVec2(0, bh), false, false, ui::Icon::Globe);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Only GameCube games with a verified online setup can be played online");
        tr("online", "explained:online-soon");
    } else if (!supports_online(e)) {
        place(label_w("Offline only", true));
        button("Offline only", ImVec2(0, bh), false, false, ui::Icon::Globe);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Online play isn't available for this game");
        tr("online", "explained:offline-only");
    } else {
        place(label_w("Play Online", true));
        tr("online", idle ? "enabled" : "disabled");
        if (button("Play Online", ImVec2(0, bh), false, idle, ui::Icon::Globe) || forced("online")) {
            ImGui::CloseCurrentPopup();
            A.pending_room_game = room_id(e);
            shot_trace("play-online", "pending_room_game=" + A.pending_room_game);
            if (!g_headless) enter_room(true, hubnet::new_room_code());
        }
    }
    place(label_w("Close", true));
    if (button("Close", ImVec2(0, bh), false, true, ui::Icon::Close)) ImGui::CloseCurrentPopup();
    ImGui::Dummy(ImVec2(0, sp(1.f)));

    /* tabs: only the ones that have something to show */
    enum { TabAbout, TabSaves, TabShots, TabSettings };
    static int tab = TabAbout;
    static std::string tab_key;
    if (tab_key != e.key || ImGui::IsWindowAppearing()) { tab_key = e.key; tab = TabAbout; if (const char* st = g_headless ? std::getenv("HUB_SHOT_TAB") : nullptr) tab = std::atoi(st); }   // HUB_SHOT_TAB=0..3 for screenshots
    qm_seed_gallery(e);   // HUB_SHOT_GALLERY screenshot aid
    const int nshots = shots_count(e);
    const bool has_settings = gs_has_game_section(e);
    struct TabDef { int id; std::string label; };
    std::vector<TabDef> tabs;
    tabs.push_back({TabAbout, "About"});
    if (!native) tabs.push_back({TabSaves, "Saves"});
    if (nshots > 0) tabs.push_back({TabShots, "Screenshots (" + std::to_string(nshots) + ")"});
    if (has_settings) tabs.push_back({TabSettings, "Settings"});
    {
        bool ok = false;
        for (auto& t : tabs) ok |= t.id == tab;
        if (!ok && !(g_headless && std::getenv("HUB_SHOT_TAB"))) tab = TabAbout;
    }
    for (size_t i = 0; i < tabs.size(); ++i) {
        float w = label_w(tabs[i].label.c_str(), false) - sp(1.5f);
        if (i) ImGui::SameLine(0, sp(1.f));
        ImGui::PushID((int)i);
        if (button(tabs[i].label.c_str(), ImVec2(w, sp(4.f)), tab == tabs[i].id)) tab = tabs[i].id;
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    /* the content area keeps one height for every tab so the page does not jump; it scrolls inside if needed */
    float room = io.DisplaySize.y * 0.92f - ImGui::GetCursorPosY() - (bh2 + sp(1.5f) + sp(3.f) + sp(1.f));
    float tabh = std::max(sp(12.f), std::min(sp(36.f), room));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, std::min(tabh, sp(15.f))), ImVec2(FLT_MAX, tabh));
    ImGui::BeginChild("##tabbody", ImVec2(0, 0), ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AutoResizeY);
    if (tab == TabAbout) {
        gameinfo_ui_section(e, cat ? cat->title : std::string(), 2);
        achievements_ui_section(e);   // RetroAchievements progress (only when signed in)
        if (e.system == hub::System::WiiU) {
            for (const std::string& l : wiiu_info_lines(e)) ImGui::TextDisabled("%s", l.c_str());
            ImGui::TextDisabled("GamePad screen: hold View + RB, or Game menu > Swap GamePad / TV screen (Cemu: Ctrl+Tab).");
        }
        { std::string fl = fr_playtime_line(e.key); if (!fl.empty()) ImGui::TextDisabled("Friends: %s", fl.c_str()); }
        if (native) ImGui::TextDisabled("Smash Classic keeps its own saves.");
        if (local_players_for(e) >= 2) {   // offline local multiplayer: who holds which port before Play
            ImGui::Dummy(ImVec2(0, sp(0.5f)));
            if (draw_players_strip(local_players_for(e))) { ImGui::CloseCurrentPopup(); open_port_assign(); }
        }
        dupes_details_section(e);
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextWrapped("%s", e.path.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    } else if (tab == TabSaves) {
        std::string sv;
        if (A.info_battery_files > 0) sv = "Save data: " + std::to_string(A.info_battery_files) + " file(s), " + size_text(A.info_battery_bytes);
        if (A.info_state_files > 0 || A.info_auto_state) {
            if (!sv.empty()) sv += "   ";
            sv += std::to_string(A.info_state_files) + " save state(s)" + (A.info_auto_state ? " + resume point" : "");
        }
        if (sv.empty()) sv = "No save data yet.";
        ImGui::TextUnformatted(sv.c_str());
        cloud_ui_game_line(e);
        history_ui_details_section(e);
        draw_state_strip(e);
    } else if (tab == TabShots) {
        shots_gallery(e);
    } else if (tab == TabSettings) {
        gs_draw_game_section(e);
    }
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, sp(1.f)));

    /* secondary actions */
    row_started = false;
    const char* fav_label = gs.favorite ? "Unfavorite" : "Favorite";
    place(label_w(fav_label, true));
    tr("favorite", "enabled");
    if (button(fav_label, ImVec2(0, bh2), false, true, gs.favorite ? ui::Icon::StarFilled : ui::Icon::Star) || forced("favorite")) {
        const bool was_fav = gs.favorite;
        toggle_favorite(e.key);
        shot_trace("favorite", e.key + " was=" + (was_fav ? "1" : "0") + " now=" + (gstate(e.key).favorite ? "1" : "0"));
    }
    if (!native) {
        place(label_w("Save folder", true));
        tr("savefolder", "enabled");
        if (button("Save folder", ImVec2(0, bh2), false, true, ui::Icon::Folder) || forced("savefolder")) {
            std::string sd = hub::saves::game_dir(sys_short(e), e.key);
            shot_trace("savefolder", sd);
            if (!g_headless) open_in_explorer(sd);
        }
    } else {
        tr("savefolder", "hidden:native-keeps-own-saves");
        place(label_w("Locate BattleShip.exe", false));
        locate_smash(button("Locate BattleShip.exe", ImVec2(0, bh2)));
    }
    const bool cheats_na = !cheats_supported(e) && !is_native(e) && !is_kanto(e);   // GC / 3DS / Wii U / PS2: shown disabled, with the reason
    tr("cheats", cheats_supported(e) ? "enabled" : cheats_na ? "explained:unavailable" : "hidden");
    if (cheats_na) {
        place(label_w("Cheats", true));
        button("Cheats", ImVec2(0, bh2), false, false, ui::Icon::Bolt);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Cheats aren't available for %s yet", hub::system_name(e.system));
    }
    if (cheats_supported(e)) {
        int cn = cheats_enabled_count(e);
        std::string cl = cn ? "Cheats (" + std::to_string(cn) + ")" : std::string("Cheats");
        place(label_w(cl.c_str(), true));
        if (button(cl.c_str(), ImVec2(0, bh2), cn > 0, true, ui::Icon::Bolt) || forced("cheats")) { ImGui::CloseCurrentPopup(); hub::GameEntry c = e; cheats_open(c); shot_trace("cheats", "opened " + e.key); }
    }
    tr("controls", hubctl::system_key(e.system).empty() ? "hidden" : "enabled:" + hubctl::system_key(e.system));
    tr("settings", hub::gs::options_for(e.system).empty() ? "none" : "options=" + std::to_string(hub::gs::options_for(e.system).size()));
    if (!hubctl::system_key(e.system).empty()) {
        place(label_w("Controls", true));
        if (button("Controls", ImVec2(0, bh2), false, true, ui::Icon::Gamepad) || forced("controls")) { ImGui::CloseCurrentPopup(); open_controls_for(e); shot_trace("controls", "tab-for " + hubctl::system_key(e.system) + " game=" + CTL_game_for_trace()); }
    }
    tr("desktop", "enabled");
    place(label_w("Add to desktop", true));
    if (button("Add to desktop", ImVec2(0, bh2), false, true, ui::Icon::Star) || forced("desktop")) { add_game_shortcut(e); shot_trace("desktop", e.key); }
    place(label_w("Customize", true));   // rename / cover / reset (customize_ui.cpp)
    if (button("Customize", ImVec2(0, bh2), false, true, ui::Icon::Gear)) custom_open_customize(e.key);
    place(label_w("Add to shelf...", true));
    if (button("Add to shelf...", ImVec2(0, bh2), false, true, ui::Icon::Plus)) custom_open_picker(e.key, true);
    custom_draw(1);   // Customize / Add to shelf modals nest inside this one
    ImGui::EndPopup();
}

/* ---------- the page ---------- */

void ensure_selection() {
    if (A.lib.empty()) { A.sel_game.clear(); return; }
    if (!A.env_select.empty() && find_entry(A.env_select)) { A.sel_game = A.env_select; A.env_select.clear(); }
    if (find_entry(A.sel_game)) return;
    int i = hubui::pick_hero(A.sg, "");
    A.sel_game = i >= 0 ? A.lib[i].key : std::string();
}

/* Home card: shown until a sync folder is set or the player dismisses it ("Not now", saved in hub.json). */
void sync_card() {
    if (!A.st.sync_folder.empty() || A.st.sync_card_dismissed) return;
    const auto& sugg = sync_suggestions();
    bool busy = A.job.running;

    begin_panel("sync_card");
    ImGui::PushFont(ui::F.bold);
    ImGui::TextUnformatted("Keep your saves and friends on all your PCs");
    ImGui::PopFont();
    text_wrapped_disabled("Sync your save vault through a folder both PCs can see. RetroHub never deletes a save.");
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    if (!sugg.empty() && button(("Use " + sugg[0].label).c_str(), ImVec2(0, 0), true, !busy)) {
        make_dirs(sugg[0].path);
        A.st.sync_folder = sugg[0].path;
        A.st.sync_auto = true;
        mark_dirty();
        start_sync(true);
    }
    if (!sugg.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", sugg[0].path.c_str());
    ImGui::SameLine(0, sp(1.f));
    {
        std::string p = osk::folder("lib_sync", button("Choose folder...", ImVec2(0, 0), false, !busy));
        if (!p.empty()) {
            A.st.sync_folder = p;
            A.st.sync_auto = true;
            mark_dirty();
            start_sync(true);
        }
    }
    ImGui::SameLine(0, sp(1.f));
    if (button("Not now", ImVec2(0, 0))) {
        A.st.sync_card_dismissed = true;
        mark_dirty();
    }
    end_panel();
    ImGui::Dummy(ImVec2(0, sp(1.f)));
}

void page_library() {
    if (A.vis_dirty || A.vis_filter != A.sys_filter || A.vis_query != A.search_buf) rebuild_vis();
    ensure_selection();
    library_top_bar();
    A.page_scrollable = false;

    if (A.lib.empty()) {
        if (A.scanning && !A.scanned_once) skeleton_home();
        else if (A.scanning) {}   // rescan of an empty library: top bar shows the progress line
        else empty_library_state();
        return;
    }
    if (A.view == LibView::All) {
        all_games_view();
        return;
    }
    if (A.st.rom_folders.empty() && !A.scanning) {   // only the bundled game: ask for game folders
        begin_panel("add_folders_card");
        ImGui::PushFont(ui::F.bold);
        ImGui::TextUnformatted("Add your game folders");
        ImGui::PopFont();
        text_wrapped_disabled("Only the bundled game is here. Point RetroHub at the folder(s) with your N64, GBA, GB, DS, 3DS and Wii U games and they all show up.");
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        add_rom_folder(button("Add ROM folder", ImVec2(0, 0), true, true, ui::Icon::Folder));
        static const std::vector<std::string> sugg2 = hub::suggest_rom_folders(2);
        for (const std::string& s : sugg2) {
            ImGui::SameLine(0, sp(1.f));
            size_t cut = s.find_last_of('\\');
            size_t cut2 = cut == std::string::npos || cut == 0 ? std::string::npos : s.find_last_of('\\', cut - 1);
            if (button(("Add  " + (cut2 == std::string::npos ? s : s.substr(cut2 + 1))).c_str(), ImVec2(0, 0))) add_rom_folder_path(s);
        }
        end_panel();
        ImGui::Dummy(ImVec2(0, sp(1.f)));
    }
    // The old "sync through a folder" Home card is gone: cloud saves are automatic (v1.25). Folder sync stays
    // an optional extra on the Saves page.

    const hub::GameEntry* sel = find_entry(A.sel_game);
    if (sel) {
        if (A.hero_key != sel->key) { A.hero_key = sel->key; A.hero_t = 0.f; }
        ui::anim(A.hero_t, 1.f, 9.f);
        float avail_h = ImGui::GetContentRegionAvail().y;
        float hero_h = std::min(sp(40.f), std::max(sp(21.f), std::min(avail_h * 0.36f, ImGui::GetIO().DisplaySize.y * 0.25f)));   // <= a quarter of the window
        hero_h = tvmode::hero_h(hero_h, avail_h);
        if (attract_update()) attract_draw_hero(hero_h);   // L6: idle slideshow in place of the hero
        else draw_hero(*sel, hero_h);
    }
    /* The shelf strips inset their covers by `bleed` (room for the hover grow); pull the child out by the same amount so
     * headers and the first cover start on the same left edge as the hero and the top bar. */
    const float lib_bleed = sp(1.5f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - lib_bleed);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("shelves", ImVec2(ImGui::GetContentRegionAvail().x + lib_bleed, 0), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(sp(2.f), 0));
    A.page_scrollable = ImGui::GetScrollMaxY() > 0;
    home_shelves();
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

