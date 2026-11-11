#include "app.h"
#include "winutil.h"
#include "launch_guard_logic.h"
#include "wave3_hooks.h"
#include "libview_ui.h"
#include "convenience.h"
#include "gamepack.h"
#include "wiiu_meta.h"
#include "homefun.h"
#include "hack_ui.h"
#include "hookshot_ui.h"
#include "continue_ui.h"
#include "continue_logic.h"
#include "hidden_games.h"
#include "hidden_ui.h"
#include "playtime_logic.h"
#include "lib_compact_logic.h"
#include "help_logic.h"
#include "library_scan_info.h"
#include "addgames.h"

/* help_ui.cpp: the scan result toast, the last result line and the scan caption's clock */
const std::string& scan_last_line();
double scan_elapsed_s();
std::string scan_home_dir();

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>

void go_add_rom(const GameInfo& g) {
    settings_open(hubui::ST_General);
    push_toast("Put your " + g.title + " ROM in a ROM folder, then press Rescan.", ui::ACCENT, 7.0);
}

/* The user's own library is empty: nothing scanned yet except (maybe) the bundled Smash 64 game. */
static bool own_library_empty() {
    for (const hub::GameEntry& e : A.lib) if (e.key != kNativeKey) return false;
    return true;
}

void home_shelves() {
    const bool add_lead = own_library_empty();   // the "Add your own games" tile only while there are no games of your own
    float avail = ImGui::GetContentRegionAvail().x;
    float gap = sp(2.f), bleed = sp(1.5f);
    float max_w = std::max(sp(25.f), std::min(sp(37.5f), avail * 0.2f));
    float tw = hubui::shelf_tile_w(avail, sp(24.f), max_w, gap);
    float th = tw / kTileAspect;
    /* Home order: Resume hero (above), Favorites, Recently played, Friends are playing, the other shelves, Your stats. */
    const int recent_si = hubui::friends_shelf_anchor(A.shelves);   // the Friends' shelf follows the last of Favorites / Recently played
    if (recent_si < 0) fr_home_shelf(avail, tw, th, gap, bleed);
    for (size_t si = 0; si < A.shelves.size(); ++si) {
        const hubui::Shelf& sh0 = A.shelves[si];
        /* Recently played (compose_shelves "recent") becomes the slimmer row (<= 6 games from the last 30 days, continue_ui.cpp) while the Continue hero is showing */
        std::vector<int> jump_items;
        const bool jump = sh0.id == "recent" && continue_row_items(&jump_items);
        hubui::Shelf sh_jump;
        if (jump) { sh_jump = sh0; sh_jump.title = "Recently played"; sh_jump.items = jump_items; }
        const hubui::Shelf& sh = jump ? sh_jump : sh0;
        bool is_console = sh.id.compare(0, 8, "console:") == 0 && sh.id != "console:other";   // unknown systems have no filter chip
        bool see_all = false;
        const float stw = jump ? tw * 0.8f : tw, sth = stw / kTileAspect;   // the Recently played row is slimmer than the other shelves
        bool overflow = hubui::shelf_max_scroll((int)sh.items.size() + (si == 0 && add_lead ? 1 : 0), stw, gap, bleed, avail) > 0;
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
        const int lead = si == 0 && add_lead ? 1 : 0;   // the first shelf starts with the "Add your own games" tile
        shelf_strip("shelf_" + sh.id, (int)sh.items.size() + lead, stw, sth, [&](int i, ImVec2 sz) {
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
            if (jump) spec.subtitle = hubui::cont::relative_time(unix_now(), gstate(e.key).last_played);
            TileResult r = draw_tile(spec, sz);
            if (jump) {
                continue_row_result(e, spec, r);
            } else handle_entry_result(e, spec, r);
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

static bool g_play_focused = false;   // the Play / Resume button of the last action_row has the visible nav focus (its game title tooltip follows)

float action_row(const hub::GameEntry& e, float bh, bool draw, ImVec2 at, bool compact, bool seed_nav) {
    bool idle = hubui::launch_allowed(A.proc != nullptr, session_active());   // shell-H2: any session, not just A.proc
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
    if (!idle && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", hubui::launch_busy_reason(A.proc != nullptr));
    else if (resume && ImGui::IsItemHovered()) ImGui::SetTooltip("Continue from where you left off");
    g_play_focused = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
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
        if (!idle && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", hubui::launch_busy_reason(A.proc != nullptr));
        else if (compact && ImGui::IsItemHovered()) ImGui::SetTooltip("Play online with friends");
    }
    ImGui::SameLine(0, gap);
    if (ui::icon_button("fav", gs.favorite ? ui::Icon::StarFilled : ui::Icon::Star, bh, gs.favorite, gs.favorite ? "Remove from favorites" : "Add to favorites", IM_COL32(236, 180, 40, 255)))
        toggle_favorite(e.key);
    ImGui::SameLine(0, gap);
    if (ui::icon_button("more", ui::Icon::Dots, bh, false, "Details")) A.show_details = true;
    return total;
}

/* Did wrap_lines cut the title short (it ends the last line with "...")? */
static bool title_cut(const std::vector<std::string>& lines, const std::string& title) {
    if (lines.empty()) return false;
    const std::string& l = lines.back();
    return l.size() >= 3 && l.compare(l.size() - 3, 3, "...") == 0 && !(title.size() >= 3 && title.compare(title.size() - 3, 3, "...") == 0);
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
    const std::string eyebrow_full = eyebrow;
    eyebrow = ui::ellipsize(cf, cf->FontSize, eyebrow, tw);
    shot_trace_once("hero", e.key + " " + eyebrow + (has_resume(e) ? " [resume]" : " [play]"));
    dl->AddText(cf, cf->FontSize, ImVec2(tx, ty), ui::scale_alpha(mix(c1, IM_COL32_WHITE, 0.45f), ha), eyebrow.c_str());
    if (eyebrow != eyebrow_full && ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(ImVec2(tx, ty), ImVec2(tx + tw, ty + cf->FontSize))) ImGui::SetTooltip("%s", eyebrow_full.c_str());
    ty += cf->FontSize + sp(0.75f);

    ImFont* tf = compact ? ui::F.title : ui::F.hero;
    std::vector<std::string> lines = ui::wrap_lines(tf, tf->FontSize, e.title, tw, compact ? 1 : 2);
    if (lines.size() == 1 && tf == ui::F.hero && ui::F.title != ui::F.hero &&
        tf->CalcTextSizeA(tf->FontSize, FLT_MAX, 0.f, e.title.c_str()).x > tw) {
        tf = ui::F.title;
        lines = ui::wrap_lines(tf, tf->FontSize, e.title, tw, 2);
    }
    const float title_y0 = ty;
    for (auto& ln : lines) {
        ui::text_shadowed(dl, tf, tf->FontSize, ImVec2(tx, ty), ui::scale_alpha(IM_COL32_WHITE, ha), ln.c_str());
        ty += tf->FontSize * 1.08f;
    }
    if (title_cut(lines, e.title) && ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(ImVec2(tx, title_y0), ImVec2(tx + tw, ty))) ImGui::SetTooltip("%s", e.title.c_str());   // the full title when it is cut off
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
        put(hub::console_name(e), mix(c0, IM_COL32_BLACK, 0.3f));
        if (is_hack(e)) put(hack_ribbon(e), ui::ACCENT);
        if (is_native(e)) put("Native port", IM_COL32(255, 255, 255, 30));
        if (cat && cat->year) put(std::to_string(cat->year), IM_COL32(255, 255, 255, 30));
        if (supports_online(e)) put(local_players_for(e) >= 2 ? "1-" + std::to_string(local_players_for(e)) + " players" : std::string("1 player"), IM_COL32(255, 255, 255, 30));
        if (gs.play_seconds) put(hubui::pt::hours_played(gs.play_seconds), IM_COL32(255, 255, 255, 30));
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
    if (g_play_focused && title_cut(lines, e.title)) ImGui::SetTooltip("%s", e.title.c_str());
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
    /* what the last scan found (if it ran on your folders) and which file types are read: small lines under the buttons */
    std::vector<std::string> note_lines;
    {
        const float nw = std::min(w - sp(6.f), sp(62.f));
        const std::string last = scan_last_line();
        if (!last.empty()) for (auto& ln : ui::wrap_lines(ui::F.cap, ui::F.cap->FontSize, last, nw, 2)) note_lines.push_back(ln);
        for (auto& ln : ui::wrap_lines(ui::F.cap, ui::F.cap->FontSize, hubhelp::file_types_line(hub::addgames::supported_exts()), nw, 4)) note_lines.push_back(ln);
    }
    const float note_h = note_lines.empty() ? 0.f : (float)note_lines.size() * ui::F.cap->FontSize * 1.3f + sp(1.5f);
    float h = std::min(ImGui::GetContentRegionAvail().y,
                       sp(53.f) + note_h + (suggestions.empty() ? 0.f : sp(10.f) + sp(6.f) * (float)suggestions.size()));
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
        struct Sy { const char* label; const char* con; };   // con: a console id (library.h)
        static const Sy sys[] = {{"N64", "n64"}, {"GameCube", "gc"}, {"GBA", "gba"}, {"GBC", "gbc"}, {"Game Boy", "gb"},
                                {"Nintendo DS", "nds"},
                                {"3DS", "3ds"}, {"Wii U", "wiiu"}, {"PS2", "ps2"}, {"PSP", "psp"}, {"Switch", "switch"}, {"Switch 2", "switch2"},
                                {"NES", "nes"}, {"SNES", "snes"}};
        float tot = 0;
        for (auto& s2 : sys) tot += ui::F.cap->CalcTextSizeA(ui::F.cap->FontSize, FLT_MAX, 0.f, s2.label).x + sp(2.5f) + sp(0.75f);
        float x = cx - (tot - sp(0.75f)) * 0.5f;
        for (auto& s2 : sys) {
            ImU32 a, b;
            console_colors(s2.con, &a, &b);
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
    if (!note_lines.empty()) {
        float ny = y + sp(5.5f);
        for (const std::string& ln : note_lines) { centered_text(dl, ui::F.cap, cx, ny, ui::TEXT3, ln.c_str()); ny += ui::F.cap->FontSize * 1.3f; }
        y += note_h;
    }
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

static std::string lib_sort_label = "Sort: Title", lib_friends_label = "Friends' games";   // one label per control at every width: a tight bar turns the control into an icon, never into different words
static bool g_lib_overlay = false;   // lib_menu_overlay_body is drawing the bar: the Cases menu never goes compact
static bool g_lib_compact = false;   // this frame's answer from library_top_bar (the filter chips follow it)

/* An icon-only stand-in for a chip: same height and colours, the full label in the tooltip. kind 0 = sort (three bars), 1 = friends (two people). */
static bool tb_icon_chip(const char* id, int kind, bool on, ImU32 col, const std::string& tip) {
    const float h = ImGui::GetFontSize() + sp(1.75f);
    const ImVec2 sz(h * 1.5f, h);
    const bool pressed = ImGui::InvisibleButton(id, sz, ImGuiButtonFlags_EnableNav);
    const bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
    float* hv = ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID(), 0.f);
    ui::anim(*hv, (hov || foc) ? 1.f : 0.f, 16.f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax(), c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    dl->AddRectFilled(p0, p1, on ? col : ui::mix(ui::PANEL, ui::PANEL3, *hv), h * 0.5f);
    if (!on) dl->AddRect(p0, p1, ui::HAIRLINE, h * 0.5f, 0, 1.f);
    if (foc) ui::focus_ring(dl, p0, p1, h * 0.5f);
    const ImU32 ic = on ? IM_COL32_WHITE : ui::mix(ui::TEXT2, ui::TEXT, *hv);
    const float u = h * 0.125f;
    if (kind == 0) {
        for (int i = 0; i < 3; ++i) dl->AddRectFilled(ImVec2(c.x - u * (3.0f - i), c.y + (i - 1) * u * 1.5f - u * 0.3f), ImVec2(c.x + u * (3.0f - i), c.y + (i - 1) * u * 1.5f + u * 0.3f), ic, u * 0.3f);
    } else {
        dl->AddCircleFilled(ImVec2(c.x - u * 1.5f, c.y - u * 1.1f), u * 1.2f, ic, 12);
        dl->AddCircleFilled(ImVec2(c.x + u * 1.6f, c.y - u * 0.7f), u * 1.0f, ic, 12);
        dl->AddRectFilled(ImVec2(c.x - u * 3.1f, c.y + u * 0.5f), ImVec2(c.x + u * 0.1f, c.y + u * 2.2f), ic, u * 0.8f);
        dl->AddRectFilled(ImVec2(c.x + u * 0.5f, c.y + u * 0.7f), ImVec2(c.x + u * 3.2f, c.y + u * 2.2f), ic, u * 0.7f);
    }
    if (hov || foc) ImGui::SetTooltip("%s", tip.c_str());
    return pressed;
}

void library_top_bar() {
    ImGuiIO& io = ImGui::GetIO();
    const float bar_w = ImGui::GetContentRegionAvail().x;   // the whole row, before the view tabs take their share
    ImGui::PushItemFlag(ImGuiItemFlags_NoNavDefaultFocus, true);   // D-pad focus starts on the hero / tiles, not here
    static const char* tabs[] = {"Home", "All Games"};
    int cur = A.view == LibView::Home ? 0 : 1;
    int n = ui::segmented("view", tabs, 2, cur, 1);   // All Games is the main attraction: it gets the logo-palette highlight
    if (n != cur) {
        A.view = n == 0 ? LibView::Home : LibView::All;
        if (n == 0) A.search_buf[0] = 0;
    }
    ImGui::SameLine(0, sp(2.f));

    float bh = sp(5.f);
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) A.focus_search = true;
    if (type_to_search_pending()) A.focus_search = true;   // just start typing: the characters land in the box this same frame
    if (A.focus_search) { ImGui::SetKeyboardFocusHere(); A.focus_search = false; }
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(sp(5.f), (bh - ImGui::GetFontSize()) * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, bh * 0.5f);
    float search_w = sp(10.f);
    const char* search_hint = "Search games";
    const bool compact = !g_lib_overlay && hubui::lc::compact_top(io.DisplaySize.y, bar_w, sp(1.f));   // short or narrow window: slimmer top area
    g_lib_compact = compact;
    hubui::lc::Plan plan;   // which controls are icon-only
    const bool dice = A.view == LibView::Home && !A.lib.empty();   // L7: Surprise me
    float total = bh + sp(1.f) + add_games_header_width(bh) + (dice ? bh + sp(1.f) : 0.f);
    const float sw_w = lib_switcher_width() + (A.view == LibView::All ? sp(1.f) : 0.f);
    bool util_low = false;     // wrapped AND still tight: dice / rescan buttons on the second row, right-aligned
    bool wrap_chips = false;   // sort / layout / friends chips on a second row (see below)
    {   // the search box gives way so the pinned right-hand buttons (Add games...) are never pushed past the edge (narrow window / Friends panel open)
        /* Search keeps a usable width: when the chips would squeeze it, Friends' games and then the sort chip turn into icons (the tooltip has the full label);
         * the compact bar may also turn Add games into an icon. The words never change. */
        const float min_search = ImGui::CalcTextSize("Search games").x + sp(9.f);
        const std::string sort_nm = hubui::sort_label(hubui::SortSpec(hubui::sort_from_key(A.st.library_sort), A.st.library_sort_rev, A.st.library_favs_first));
        lib_sort_label = "Sort: " + sort_nm;
        lib_friends_label = "Friends' games";
        const float chip_h = ImGui::GetFontSize() + sp(1.75f);
        const float sort_w = ImGui::CalcTextSize(lib_sort_label.c_str()).x + sp(3.25f), fr_w = ImGui::CalcTextSize(lib_friends_label.c_str()).x + sp(3.25f);
        const float icon_w = chip_h * 1.5f;
        const float avail = ImGui::GetContentRegionAvail().x;
        const float fixed = bh + sp(1.f) + (dice ? bh + sp(1.f) : 0.f) + sw_w + sp(5.f);   // rescan, dice, layout switcher, gaps
        plan = hubui::lc::plan_row(avail, fixed, sort_w, fr_w, icon_w, add_games_header_width(bh), bh, min_search, compact ? 3 : 2);
        if (!plan.fits && !compact) {   // big UI size / narrow window: even the icons would squeeze the search box, so the chips take a second row (full labels again)
            wrap_chips = true;
            plan.level = 0;
            float room1 = avail - total - sp(1.5f);
            if (room1 < sp(10.f)) { util_low = true; room1 = avail - add_games_header_width(bh) - sp(1.5f); }   // still too tight: the dice / rescan buttons join the second row
            search_w = std::max(sp(10.f), std::min(sp(46.f), room1));
        } else {
            const float room = avail - fixed - (plan.sort_icon() ? icon_w : sort_w) - (plan.friends_icon() ? icon_w : fr_w) - (plan.add_icon() ? bh : add_games_header_width(bh));
            search_w = std::max(sp(10.f), std::min(std::min(sp(46.f), avail * 0.36f), room));
        }
        ImGui::SetNextItemWidth(search_w);
    }
    if (!compact) {   // regular sizes keep the placeholder with the shortcut; it gives way to a shorter one instead of being clipped (1280x800)
        const float room_txt = search_w - sp(5.f) - sp(2.5f);
        static const char* hints[] = {"Search games   (Ctrl+F)", "Search games (Ctrl+F)", "Search games", "Search"};
        search_hint = hints[3];
        for (const char* h : hints) if (ImGui::CalcTextSize(h).x <= room_txt) { search_hint = h; break; }
    }
    osk::input("##search", search_hint, A.search_buf, sizeof A.search_buf);
    ImVec2 s0 = ImGui::GetItemRectMin(), s1 = ImGui::GetItemRectMax();
    if (compact && ImGui::IsItemHovered() && !ImGui::IsItemActive()) ImGui::SetTooltip("Search your games  (Ctrl+F)");
    ImGui::GetWindowDrawList()->AddRect(s0, s1, ImGui::IsItemActive() ? ui::ACCENT : ui::HAIRLINE, bh * 0.5f, 0, 1.f);
    ui::icon(ImGui::GetWindowDrawList(), ui::Icon::Search, ImVec2(s0.x + sp(2.5f), (s0.y + s1.y) * 0.5f), sp(2.f), ui::TEXT2);
    ImGui::PopStyleVar(2);
    if (A.search_buf[0] && A.view == LibView::Home) A.view = LibView::All;

    auto draw_chips = [&](bool first_on_row) {
    {   // sort chip: A cycles Title / Last played / Most played / Recently added (RT on the pad)
        if (!first_on_row) ImGui::SameLine(0, sp(1.5f));
        std::string sl = lib_sort_label;
        const bool sort_on = hubui::sort_from_key(A.st.library_sort) != hubui::SortMode::Title || A.st.library_sort_rev || A.st.library_favs_first;
        const bool sort_icon = plan.sort_icon() && !wrap_chips;
        const char* sort_tip = "Change the order of your games  (RT on a controller)\nRight-click: reverse the order";
        if (sort_icon ? tb_icon_chip("##sort_icon", 0, sort_on, ui::ACCENT, sl + "\n" + sort_tip) : chip(sl.c_str(), sort_on, ui::ACCENT)) cycle_sort(1);
        if (ImGui::IsItemClicked(1)) { A.st.library_sort_rev = !A.st.library_sort_rev; sort_changed(); }
        if (!sort_icon && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", sort_tip);
    }
    if (A.view == LibView::All) {   // layout switcher: grid / list / compact / showcase
        ImGui::SameLine(0, sp(1.f));
        lib_switcher();
    }
    {   // "Show friends' games": friends' titles you do not own, greyed out (metadata only)
        ImGui::SameLine(0, sp(1.f));
        const bool fr_icon = plan.friends_icon() && !wrap_chips;
        if (fr_icon ? tb_icon_chip("##friends_icon", 1, A.friends_chip, ui::ACCENT, lib_friends_label + "\nShow or hide friends' games for now") : chip(lib_friends_label.c_str(), A.friends_chip, ui::ACCENT)) { A.friends_chip = !A.friends_chip; A.vis_dirty = true; }   // session-only: never saved
        if (!fr_icon && (ImGui::IsItemHovered() || ImGui::IsItemFocused())) ImGui::SetTooltip("Show or hide friends' games for now");
    }
    };
    if (!wrap_chips) draw_chips(false);
    auto draw_utility = [&]() {
        if (dice) { surprise_ui_button(bh); ImGui::SameLine(0, sp(1.f)); }
        ImGui::BeginDisabled(A.scanning);
        if (ui::icon_button("rescan", ui::Icon::Refresh, bh, false, nullptr)) start_scan(true);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) || (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible))
            ImGui::SetTooltip("%s", A.scanning ? "Scanning..." : "Rescan library");
        ImGui::EndDisabled();
    };
    const float add_w = add_games_header_width(bh);
    ImGui::SameLine(0, 0);
    {
        const float ax = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, ax - (util_low ? add_w : total)));
    }
    if (!util_low) { draw_utility(); ImGui::SameLine(0, sp(1.f)); }
    if (plan.add_icon()) {
        if (ui::icon_button("add_games_icon", ui::Icon::Plus, bh, true, nullptr)) add_games_open();
        if (ImGui::IsItemHovered() || (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)) ImGui::SetTooltip("Add games");
    } else add_games_header_button(bh);
    if (wrap_chips) {   // second row, left-aligned under the search box
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        draw_chips(true);
        if (util_low) {
            ImGui::SameLine(0, 0);
            const float ax2 = ImGui::GetContentRegionAvail().x, uw = total - add_w;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, ax2 - uw));
            draw_utility();
        }
    }
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
            float ph = ui::reduce_motion ? 0.5f : std::fmod((float)ImGui::GetTime() * 0.8f, 1.f);
            dl->AddRectFilled(ImVec2(o.x + w * ph * 0.8f, o.y + sp(0.5f)), ImVec2(o.x + w * (ph * 0.8f + 0.2f), o.y + sp(0.5f) + lh), ui::ACCENT, lh);
        }
        /* "Scanning your games...  12 / 340. Looking in ~\Documents\Roms" (the folder is shortened in the middle to fit), and after 10 seconds a hint at the right */
        const std::string base = hubhelp::scan_caption_base(d, t2);
        const std::string hint = hubhelp::scan_browse_hint(scan_elapsed_s());
        const std::string folder = hubhelp::short_path(hub::scan_current_folder(), scan_home_dir());
        ImFont* cf = ui::F.cap;
        auto text_w = [&](const std::string& s) { return cf->CalcTextSizeA(cf->FontSize, FLT_MAX, 0.f, s.c_str()).x; };
        const float room = w - sp(0.5f);
        const float gap = sp(2.f);
        std::string left = base;
        bool show_hint = !hint.empty() && text_w(base) + gap + text_w(hint) <= room;
        const float left_room = show_hint ? room - text_w(hint) - gap : room;
        if (!folder.empty()) {
            /* the fitted text is remembered per (count text, folder, width): nothing is re-measured while those stay the same */
            static std::string c_base, c_folder, c_left;
            static int c_room = -1;
            const int room_i = (int)left_room;
            if (c_base != base || c_folder != folder || c_room != room_i) {
                c_base = base; c_folder = folder; c_room = room_i; c_left = base;
                const std::string pre = base + ". Looking in ";
                for (size_t mx = hubhelp::utf8_len(folder);; mx = mx > 6 ? mx - 2 : 0) {
                    if (mx < 10) break;   // no useful room left: the folder is left out
                    const std::string cand = pre + hubhelp::middle_trunc(folder, mx);
                    if (text_w(cand) <= left_room) { c_left = cand; break; }
                }
            }
            left = c_left;
        }
        dl->AddText(cf, cf->FontSize, ImVec2(o.x + sp(0.25f), o.y + sp(1.25f)), ui::TEXT3, left.c_str());
        if (show_hint) dl->AddText(cf, cf->FontSize, ImVec2(o.x + w - text_w(hint) - sp(0.25f), o.y + sp(1.25f)), ui::TEXT3, hint.c_str());
    }
    ImGui::Dummy(ImVec2(w, sp(A.scanning ? (compact ? 2.5f : 3.25f) : (compact ? 0.25f : 1.5f))));
    if (compact) ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y * 1.5f);   // pull the filter chips up under the bar
}

/* ---------- All Games ---------- */

/* The selected-game bar under the grid is slimmer on a short window (the grid gets the room). */
static float sel_bar_h() { return g_lib_compact && !g_lib_overlay ? sp(6.5f) : sp(8.f); }

void selected_bar(const hub::GameEntry& e) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float w = ImGui::GetContentRegionAvail().x, h = sel_bar_h();
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
    float bh = h < sp(8.f) ? sp(4.5f) : sp(5.f);
    float row_w = action_row(e, bh, false, ImVec2(0, 0), true);
    float tx = a1.x + sp(2.f);
    float text_w = std::max(sp(8.f), p1.x - sp(2.f) - row_w - tx - sp(2.f));
    const hub::GameState& gs = gstate(e.key);
    const std::string bar_title = ui::ellipsize(ui::F.h2, ui::F.h2->FontSize, e.title, text_w);
    dl->AddText(ui::F.h2, ui::F.h2->FontSize, ImVec2(tx, p0.y + sp(1.25f)), ui::TEXT, bar_title.c_str());
    std::string sub = std::string(hub::console_name(e)) + "  -  " + hubui::format_play_time(gs.play_seconds);
    const std::string bar_sub = ui::ellipsize(ui::F.cap, ui::F.cap->FontSize, sub, text_w);
    dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(tx, p0.y + sp(1.25f) + ui::F.h2->FontSize + sp(0.5f)), ui::TEXT2, bar_sub.c_str());
    if ((bar_title != e.title || bar_sub != sub) && ImGui::IsWindowHovered() &&
        ImGui::IsMouseHoveringRect(ImVec2(tx, p0.y + sp(1.25f)), ImVec2(tx + text_w, p0.y + sp(1.25f) + ui::F.h2->FontSize + sp(0.5f) + ui::F.cap->FontSize)))
        ImGui::SetTooltip("%s", bar_title != e.title ? e.title.c_str() : sub.c_str());
    action_row(e, bh, true, ImVec2(p1.x - sp(2.f) - row_w, p0.y + (h - bh) * 0.5f), true);
    if (g_play_focused && (bar_title != e.title || bar_sub != sub)) ImGui::SetTooltip("%s", bar_title != e.title ? e.title.c_str() : sub.c_str());
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
    ImGui::Dummy(ImVec2(w, 1));
}

/* The filter chip rows of All Games (console chips, Online, Favorites, Genre, Group). In the Cases layout the console chips and Group are left
 * out (it has its own console tabs); the Cases menu overlay shows this row. */
void library_filter_chips() {
    struct Chip { const char* label; const char* sys; };
    static const Chip chips[] = {{"All", ""}, {"N64", "n64"}, {"GameCube", "gc"}, {"PlayStation", "ps1"}, {"Wii U", "wiiu"}, {"3DS", "3ds"},
                                {"Nintendo DS", "nds"}, {"PS2", "ps2"}, {"PSP", "psp"}, {"Switch", "switch"}, {"Switch 2", "switch2"}, {"NES", "nes"}, {"SNES", "snes"}, {"GBA", "gba"}, {"GBC", "gbc"}, {"Game Boy", "gb"}};
    std::map<std::string, int>& counts = A.chip_counts;
    /* At big UI sizes the wrapped chip rows alone could fill the page and push the grid out of view: the rows live in a child that is as tall as
     * its rows, up to about a quarter of the page, and scrolls past that. (Padding-free, so at normal sizes it looks exactly like before.) */
    const bool compact = g_lib_compact && !g_lib_overlay && lib_layout() != hubui::LibLayout::Cases;   // short window: ONE row that scrolls sideways, the health chip rides at its end
    static bool compact_overflow = false;   // last frame's answer: reserve room for the thin scrollbar only while the row overflows
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (compact) {
        const float ch = ImGui::GetFontSize() + sp(1.75f);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, sp(0.6f));
        ImGui::BeginChild("##lib_chips", ImVec2(0, hubui::lc::chips_row_h(ch + sp(0.5f), sp(0.6f), compact_overflow)), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PopStyleVar();
    } else {
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, std::max(sp(7.f), ImGui::GetContentRegionAvail().y * 0.25f)));
        ImGui::BeginChild("##lib_chips", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);
    }
    ImGui::PopStyleVar();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(sp(1.f), sp(1.f)));
    /* The chip rows wrap onto a new line instead of running off the right edge (1280x800 / 1100x700). */
    const float chips_right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    bool first = true;
    auto chip_same_line = [&](const char* label, float spacing) {
        if (first) return;   // nothing before it on this row yet (Cases hides the console chips)
        if (spacing < 0.f) spacing = ImGui::GetStyle().ItemSpacing.x;
        if (compact) { ImGui::SameLine(0, spacing); return; }   // never wrap: the row scrolls instead
        if (ImGui::GetItemRectMax().x + spacing + ImGui::CalcTextSize(label).x + sp(3.25f) <= chips_right) ImGui::SameLine(0, spacing);
    };
    ImGui::PushItemFlag(ImGuiItemFlags_NoNavDefaultFocus, true);
    if (compact) {   // the health chip leads the row (only drawn when there is something to say): one small chip that opens the same panel as a popup (verify_ui.cpp)
        ImGui::PushID("health");
        first = !verify_library_card(true);
        ImGui::PopID();
        if (!first) ImGui::SameLine(0, sp(1.f));
        first = true;   // the console chips below start the line themselves; the SameLine above joined them
    }
    const bool cases_view = lib_layout() == hubui::LibLayout::Cases;   // Cases has its own console tabs along the top
    for (auto& c : chips) {
        if (cases_view) break;
        int n = c.sys[0] ? counts[c.sys] : A.chip_total;
        if (c.sys[0] && n == 0 && A.sys_filter != c.sys) continue;
        char lab[48];
        std::snprintf(lab, sizeof lab, "%s  %d", c.label, n);
        if (!first) chip_same_line(lab, -1.f);
        first = false;
        ImU32 a = COL_ACCENT, b2;
        if (c.sys[0]) console_colors(c.sys, &a, &b2);
        if (chip(lab, A.sys_filter == c.sys, a)) { A.sys_filter = c.sys; A.vis_dirty = true; A.grid_scroll_top = true; }
    }
    {   // Online filter chip
        int n_online = 0;
        for (auto& e : A.lib) if (entry_online(e)) ++n_online;   // memoized: no catalog scan per frame
        char lab[48];
        std::snprintf(lab, sizeof lab, "Online  %d", n_online);
        chip_same_line(lab, sp(2.f));
        if (chip(lab, A.online_only, ui::GOOD)) { A.online_only = !A.online_only; A.vis_dirty = true; }
        first = false;
    }
    {   // Favorites chip, Genre picker and Group by Genre (saved like library_sort)
        int n_fav = 0;
        for (auto& e : A.lib) if (gstate(e.key).favorite) ++n_fav;
        char lab[48];
        std::snprintf(lab, sizeof lab, "Favorites  %d", n_fav);
        chip_same_line(lab, sp(2.f));
        if (chip(lab, A.st.lib_favs_only, COL_ACCENT)) { A.st.lib_favs_only = !A.st.lib_favs_only; mark_dirty(); A.vis_dirty = true; A.grid_scroll_top = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Only your favorite games");
        std::string gl = A.st.lib_genre.empty() ? std::string("Genre: All") : "Genre: " + A.st.lib_genre;
        chip_same_line(gl.c_str(), -1.f);
        if (chip(gl.c_str(), !A.st.lib_genre.empty(), COL_ACCENT)) ImGui::OpenPopup("##genre_pick");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show one genre only");
        if (cases_view) {   // Cases: Coverflow (default) or the Classic shelf of turned cases
            const char* sl = A.st.lib_cases_classic ? "Style: Classic shelf" : "Style: Coverflow";
            chip_same_line(sl, -1.f);
            if (chip(sl, !A.st.lib_cases_classic, COL_ACCENT)) { A.st.lib_cases_classic = !A.st.lib_cases_classic; mark_dirty(); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Carousel style: Coverflow (straight row, disc slides out) or the Classic shelf");
        }
        {   // Show hidden (library-12): off by default, remembered on this PC. Only drawn when something is hidden (or it is on).
            const std::string hl = hub::hidden::chip_label(show_hidden_on(), hidden_stash_count());
            if (!hl.empty()) {
                chip_same_line(hl.c_str(), -1.f);
                if (chip(hl.c_str(), show_hidden_on(), COL_ACCENT)) show_hidden_toggle();
                if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) ImGui::SetTooltip("Show the games you hid, dimmed, with an Unhide button");
            }
        }
        if (!cases_view) {
            chip_same_line(A.st.lib_group_genre ? "Group: Genre" : "Group: Console", -1.f);
            if (chip(A.st.lib_group_genre ? "Group: Genre" : "Group: Console", A.st.lib_group_genre, COL_ACCENT)) {
                A.st.lib_group_genre = !A.st.lib_group_genre; mark_dirty(); A.vis_dirty = true; A.grid_scroll_top = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Group the library by console or by genre");
        }
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
    if (compact) {
        compact_overflow = ImGui::GetScrollMaxX() > 0.f;
        const ImGuiIO& cio = ImGui::GetIO();   // the wheel scrolls the row sideways
        if (compact_overflow && ImGui::IsWindowHovered() && cio.MouseWheel != 0.f) ImGui::SetScrollX(ImGui::GetScrollX() - cio.MouseWheel * sp(6.f));
    }
    ImGui::EndChild();
    if (compact) ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y * 0.75f);   // and the grid up under the chips
    hidden_strip_draw();   // "Show hidden" on: the hidden games, dimmed, each with Unhide (libcopies_ui.cpp)
    ImGui::Dummy(ImVec2(0, compact ? sp(0.f) : sp(0.5f)));
}

/* The Cases menu overlay (libview_ui.cpp slides it down over the carousel): the toolbar (Home / All Games, search, sort, layout switcher to
 * leave Cases, friends' games, rescan, add games), the filters, and a button for every page, so nothing is out of reach. */
void lib_menu_overlay_body(bool* close) {
    g_lib_overlay = true;   // the Cases menu keeps the regular (non-compact) bar and chips
    library_top_bar();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    library_filter_chips();
    g_lib_overlay = false;
    static const struct { Page p; const char* label; } pages[] = {{Page::Discover, "Discover"}, {Page::Online, "Play Online"}, {Page::Saves, "Saves"},
                                                                  {Page::Controllers, "Controllers"}, {Page::Settings, "Settings"}, {Page::About, "Updates"}};
    ImGui::PushItemFlag(ImGuiItemFlags_NoNavDefaultFocus, false);
    if (ui::button("Back to the carousel", ImVec2(0, sp(4.f)), true, true)) *close = true;
    for (const auto& pg : pages) {
        ImGui::SameLine(0, sp(1.f));
        if (ui::button(pg.label, ImVec2(0, sp(4.f)), false, true)) { A.page = pg.p; *close = true; }
    }
    ImGui::PopItemFlag();
}

void all_games_view() {
    if (added_stamps_changed() || A.vis_dirty || A.vis_filter != A.sys_filter || A.vis_query != A.search_buf) rebuild_vis();
    const bool cases_view = lib_layout() == hubui::LibLayout::Cases;   // immersive: the filters live in its menu overlay
    if (!cases_view) library_filter_chips();
    if (!cases_view && !g_lib_compact) verify_library_card();   // Wave 3 L3: only when there is something to say (Cases is a full-screen view: it stays out)
    if (added_stamps_changed() || A.vis_dirty || A.vis_filter != A.sys_filter || A.vis_query != A.search_buf) rebuild_vis();

    const hubui::fg::Ghost* selg = (lib_layout_has_bar() && !A.sel_ghost.empty()) ? find_ghost(A.sel_ghost) : nullptr;   // a friend's game is selected
    const hub::GameEntry* sel = (lib_layout_has_bar() && !selg) ? find_entry(A.sel_game) : nullptr;
    float bar_h = (sel || selg) ? (selg ? sp(8.f) : sel_bar_h()) + ImGui::GetStyle().ItemSpacing.y + 2.f : 0;   // selected bar + the gap above it
    if (A.vis.empty() && !cases_view) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(1.5f), sp(1.5f)));
        if (A.grid_scroll_top) { ImGui::SetNextWindowScroll(ImVec2(0.f, 0.f)); A.grid_scroll_top = false; }
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() - sp(1.5f));
        ImGui::BeginChild("grid", ImVec2(ImGui::GetContentRegionAvail().x + sp(1.5f), -bar_h), ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();
        A.page_scrollable = true;
        add_games_banner();
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
        ImGui::EndChild();
    } else {
        lib_body(bar_h);   // libview_ui.cpp: grid / list / compact / showcase + the jump strip
    }
    if (sel) selected_bar(*sel);
    else if (selg) selected_bar_ghost(*selg);
}

/* ---------- Wii U details: updates / DLC, Cemu game profile, save accounts, key help ---------- */

/* Disk-heavy (Cemu install probe, WUA directory, mlc01 save accounts): runs on a worker thread. */
static std::vector<std::string> wiiu_info_compute(const hub::GameEntry& e, const std::string& cemu_exe) {
    std::vector<std::string> s_lines;
    hubemu::Install inst = hubemu::find(hubemu::Kind::Cemu, cemu_exe, e.path);
    std::string game = launch_path_for(e);
    if (e.issue == "extract" && game == e.path) game.clear();   // still packed: nothing to read yet
    uint64_t tid = hubemu::title_id_from_key(e.key);
    if (!tid && !game.empty()) tid = hubemu::wiiu_title_id_of_file(game);
    hub::FsProbe fp = hubemu::wiiu_probe();
    std::string user_data, mlc;
    if (!inst.exe.empty()) {
        if (inst.managed) { user_data = inst.dir + "\\portable"; mlc = user_data + "\\mlc01"; }
        else {
            const std::string ad_s = winutil::getenv_utf8("APPDATA"); const char* ad = ad_s.empty() ? nullptr : ad_s.c_str();
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

/* UI thread: returns the last finished result for this game (empty until the first scan lands) and
   re-scans in the background at most every 4 s. */
static std::vector<std::string> wiiu_info_lines(const hub::GameEntry& e) {
    if (g_headless) return wiiu_info_compute(e, A.st.cemu_exe);   // screenshots stay deterministic
    struct Bg { std::mutex mu; bool running = false; std::string key; std::vector<std::string> lines; };
    static Bg* bg = new Bg();
    static std::string s_key;
    static double s_at = -100;
    static std::vector<std::string> s_lines;
    {
        std::lock_guard<std::mutex> l(bg->mu);
        if (bg->key == e.key) s_lines = bg->lines;   // only ever show a result computed for THIS game
        else s_lines.clear();
        if (s_key == e.key && bg->key == e.key && ImGui::GetTime() - s_at < 4.0) return s_lines;
        if (bg->running) return s_lines;   // A's scan still running: B's starts on the first frame after it lands
        bg->running = true;
    }
    s_key = e.key;
    s_at = ImGui::GetTime();
    std::thread([e, cemu = A.st.cemu_exe] {
        if (g_io_shutdown) return;
        std::vector<std::string> r = wiiu_info_compute(e, cemu);
        if (g_io_shutdown) return;
        std::lock_guard<std::mutex> l(bg->mu);
        bg->key = e.key;
        bg->lines = std::move(r);
        bg->running = false;
    }).detach();
    return s_lines;
}

/* ---------- game page play time (Steam-style header line + stats strip) ---------- */

/* local time minus UTC in seconds (same recipe as wrapped_ui.cpp) */
static int pt_local_tz() {
    std::time_t t = std::time(nullptr);
    std::tm lt{}, gm{};
    localtime_s(&lt, &t);
    gmtime_s(&gm, &t);
    int tz = (int)std::difftime(std::mktime(&lt), std::mktime(&gm));
    if (lt.tm_isdst > 0) tz += 3600;
    return tz;
}

/* Screenshot aid (HUB_SHOT_PLAYTIME=1, headless): deterministic sessions instead of the vault. This PC's sessions add up to exactly
 * the seeded total (lib_state.cpp), the newest starts at the seeded last-played time. */
static std::vector<hub::saves::Session> pt_fake_sessions(const std::string& key, const hub::GameState& gs) {
    std::vector<hub::saves::Session> v;
    if (!gs.play_seconds || !gs.last_played) return v;
    const int n = key == "fake:1" ? 30 : key == "fake:0" ? 8 : 3;
    const int64_t T = (int64_t)gs.play_seconds, last = (int64_t)gs.last_played;
    const int64_t gap = key == "fake:1" ? (int64_t)(5.5 * 86400) : key == "fake:0" ? 129600 : 86400;
    int64_t left = T;
    for (int j = 0; j < n; ++j) {
        hub::saves::Session s;
        s.system = "fake";
        s.key = key;
        s.start = last - (int64_t)j * gap;
        s.secs = (j == n - 1) ? left : T / n;
        left -= s.secs;
        v.push_back(s);
    }
    if (key == "fake:0") {   // a second PC, about 3.1 hours
        hub::saves::Session a, b;
        a.system = b.system = "fake";
        a.key = b.key = key;
        a.device = b.device = "LAPTOP";
        a.start = (int64_t)gs.last_played - 20 * 86400; a.secs = 5400;
        b.start = (int64_t)gs.last_played - 30 * 86400; b.secs = 5760;
        v.push_back(a);
        v.push_back(b);
    }
    return v;
}

/* The shown game's numbers, cached: recomputed when the game changes, when library.json's numbers for it changed (a session just
 * ended), when the modal was closed in between, and otherwise every 30 s. Never reads files every frame. */
static const hubui::pt::GameStats& pt_stats_for(const hub::GameEntry& e, const hub::GameState& gs, int64_t now) {
    static struct {
        std::string key;
        unsigned long long ps = 0, lp = 0;
        double at = -1e9;
        int frame = -100;
        bool ok = false;
        hubui::pt::GameStats st;
    } C;
    const double t = ImGui::GetTime();
    const int fr = ImGui::GetFrameCount();
    if (!C.ok || C.key != e.key || C.ps != gs.play_seconds || C.lp != gs.last_played || t - C.at > 30.0 || fr - C.frame > 2) {
        static const bool fake_on = g_headless && std::getenv("HUB_SHOT_PLAYTIME") != nullptr;
        const bool fake = fake_on && e.key.compare(0, 5, "fake:") == 0;   // only the made-up games get made-up sessions
        std::vector<hub::saves::Session> sessions = fake ? pt_fake_sessions(e.key, gs) : hub::saves::playlog_read_game(sys_short(e), e.key);
        C.st = hubui::pt::game_stats(gs.play_seconds, gs.last_played, sessions, hub::saves::playlog_device(), now);
        C.key = e.key;
        C.ps = gs.play_seconds;
        C.lp = gs.last_played;
        C.at = t;
        C.ok = true;
    }
    C.frame = fr;
    return C.st;
}

/* shorten text with "..." until it is at most w wide in the current font (last resort only; UTF-8 safe) */
static std::string pt_fit(const std::string& s, float w) { return ui::ellipsize(ImGui::GetFont(), ImGui::GetFontSize(), s, w); }

/* The stats strip: one rounded panel, cells = small caption over a bold value, hairline separators; cells wrap greedily into rows
 * when they do not fit max_w. Draw-only (one Dummy reserves the space), no focusable items. */
static void draw_playtime_strip(const std::vector<hubui::pt::Cell>& cells, float max_w) {
    if (cells.empty()) return;
    const float padx = sp(1.5f), pady = sp(0.9f), gap_y = sp(0.25f), rad = sp(1.25f);
    float cap_h = 0.f, val_h = 0.f;
    ImGui::PushFont(ui::F.cap); cap_h = ImGui::GetTextLineHeight(); ImGui::PopFont();
    ImGui::PushFont(ui::F.bold); val_h = ImGui::GetTextLineHeight(); ImGui::PopFont();
    struct Item { std::string label, cap, val; float w = 0.f; };
    std::vector<Item> items;
    bool has_other = false;
    for (const hubui::pt::Cell& c : cells) {
        Item it;
        it.label = c.label;
        it.cap = c.label;
        for (char& ch : it.cap) ch = (char)std::toupper((unsigned char)ch);
        it.val = c.value;
        const float inner = std::max(sp(4.f), max_w - 2.f * padx);
        ImGui::PushFont(ui::F.cap); it.cap = pt_fit(it.cap, inner); const float cw = ImGui::CalcTextSize(it.cap.c_str()).x; ImGui::PopFont();
        ImGui::PushFont(ui::F.bold); it.val = pt_fit(it.val, inner); const float vw = ImGui::CalcTextSize(it.val.c_str()).x; ImGui::PopFont();
        it.w = std::max(cw, vw) + 2.f * padx;
        if (c.label.compare(0, 8, "Other PC") == 0) has_other = true;
        items.push_back(it);
    }
    /* greedy rows */
    std::vector<std::vector<size_t>> rows(1);
    float used = 0.f, panel_w = 0.f;
    for (size_t i = 0; i < items.size(); ++i) {
        if (!rows.back().empty() && used + items[i].w > max_w) { rows.emplace_back(); used = 0.f; }
        rows.back().push_back(i);
        used += items[i].w;
        panel_w = std::max(panel_w, used);
    }
    const float row_h = 2.f * pady + cap_h + gap_y + val_h;
    const float panel_h = row_h * (float)rows.size();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + panel_w, p0.y + panel_h);
    ImGui::Dummy(ImVec2(panel_w, panel_h));
    dl->AddRectFilled(p0, p1, ui::PANEL2, rad);
    dl->AddRect(p0, p1, ui::HAIRLINE, rad, 0, 1.f);
    const bool hov_ok = ImGui::IsWindowHovered();
    for (size_t r = 0; r < rows.size(); ++r) {
        const float y = p0.y + row_h * (float)r;
        if (r > 0) dl->AddLine(ImVec2(p0.x + 1.f, y), ImVec2(p1.x - 1.f, y), ui::HAIRLINE, 1.f);
        float x = p0.x;
        for (size_t k = 0; k < rows[r].size(); ++k) {
            const Item& it = items[rows[r][k]];
            if (k > 0) dl->AddLine(ImVec2(x, y + pady * 0.7f), ImVec2(x, y + row_h - pady * 0.7f), ui::HAIRLINE, 1.f);
            ImGui::PushFont(ui::F.cap);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x + padx, y + pady), ui::TEXT3, it.cap.c_str());
            ImGui::PopFont();
            ImGui::PushFont(ui::F.bold);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x + padx, y + pady + cap_h + gap_y), ui::TEXT, it.val.c_str());
            ImGui::PopFont();
            if (has_other && hov_ok && it.label == "Play time" && ImGui::IsMouseHoveringRect(ImVec2(x, y), ImVec2(x + it.w, y + row_h)))
                ImGui::SetTooltip("Every PC together: this PC plus the time your other PCs logged (their play logs sync with your saves).");
            x += it.w;
        }
    }
}

/* ---------- details modal (Y / right-click / "...") ---------- */

std::string g_details_shown;

void draw_details_modal() {
    draw_ghost_modal();   // a friend's game you do not own (friend_games_ui.cpp)
    g_details_shown.clear();
    if (g_headless && A.show_details && !A.lib.empty() && !A.env_select.empty() && find_entry(A.env_select)) { A.sel_game = A.env_select; A.env_select.clear(); }   // HUB_SHOT_DETAILS: select the named game before opening
    if (A.show_details && !(g_headless && A.lib.empty())) {   // headless aids (HUB_SHOT_DETAILS) set this before the library exists: keep it pending until then
        A.show_details = false;
        if (find_entry(A.sel_game)) ImGui::OpenPopup("##details");
    }
    const hub::GameEntry* ep = find_entry(A.sel_game);
    ImGuiIO& io = ImGui::GetIO();
    float w = std::min(sp(104.f), io.DisplaySize.x * 0.92f);
    ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    const float win_max_h = io.DisplaySize.y * 0.92f;
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(w, win_max_h));   // never taller than the window: the tab body scrolls above a fixed footer
    const bool short_win = io.DisplaySize.y < hubui::lc::kShortWindowPx;   // short window: tighter padding and gaps so the body keeps some height
    const float win_pad = short_win ? sp(2.f) : sp(3.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(win_pad, win_pad));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.5f));
    bool open = ImGui::BeginPopupModal("##details", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    if (!open) return;
    if (!ep || ui::modal_back()) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    if (short_win) ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, sp(0.75f)));
    const hub::GameEntry& e = *ep;
    g_details_shown = e.key;
    const hub::GameState& gs = gstate(e.key);
    const GameInfo* cat = catalog_for(e);
    poll_save_info();
    if (A.info_key != e.key || ImGui::GetTime() - A.info_at > 4.0) refresh_save_info_bg(e);
    bool idle = hubui::launch_allowed(A.proc != nullptr, session_active()), native = is_native(e);
    ImU32 c0, c1;
    entry_colors(e, &c0, &c1);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    float avail = ImGui::GetContentRegionAvail().x;
    bool with_art = avail > sp(62.f);
    float aw = with_art ? (io.DisplaySize.y < hubui::lc::kShortWindowPx ? sp(14.f) : sp(20.f)) : 0, ah = aw / kTileAspect;   // slimmer header on a short window
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
        std::string facts = hub::console_name(e);
        if (native) facts += std::string(dot) + "Native port";
        if (local_players_for(e) >= 2) facts += std::string(dot) + "1-" + std::to_string(local_players_for(e)) + " players";
        if (supports_online(e)) facts += std::string(dot) + "Online";
        if (cat && cat->year) facts += std::string(dot) + std::to_string(cat->year);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tw);
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
        ImGui::TextWrapped("%s", facts.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        /* Steam-style play time: prominent header line, then (when played) the stats strip */
        const int64_t pt_now = (int64_t)std::time(nullptr);
        static int pt_tz = 0;
        static double pt_tz_at = -1e9;
        if (ImGui::GetTime() - pt_tz_at > 30.0) { pt_tz = pt_local_tz(); pt_tz_at = ImGui::GetTime(); }   // mktime twice: not every frame
        const hubui::pt::GameStats& pst = pt_stats_for(e, gs, pt_now);
        if (!pst.played()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
            ImGui::TextUnformatted("Not played yet");
            ImGui::PopStyleColor();
        } else {
            /* The stats strip below says it all (Play time, Last played, ...): no extra bold hours line above it. */
            const std::vector<hubui::pt::Cell> cells = hubui::pt::strip_cells(pst, pt_now, pt_tz);
            if (!cells.empty()) {
                ImGui::Dummy(ImVec2(0, sp(0.75f)));
                draw_playtime_strip(cells, tw);
            }
        }
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
    if (e.issue == "extract" && !hub::extracted_3ds_path(e).empty()) {
        text_wrapped_disabled("Unpacked and ready: Play starts at once.");
    } else if (e.issue == "extract") {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN);
        ImGui::TextWrapped("Archived: %s cannot open %s files. Press Play to unpack it (about %.1f GB) into RetroHub's data folder - your archive stays untouched.",
                           e.system == hub::System::PS2 ? "PCSX2" : e.system == hub::System::PSP ? "PPSSPP" : e.system == hub::System::GC ? "Dolphin" : "Azahar", e.system == hub::System::GC || e.system == hub::System::PS2 || e.system == hub::System::PSP ? "archive" : ".zip", unpacked_size(e) / 1073741824.0);
        ImGui::PopStyleColor();
    } else if (e.issue == "encrypted") {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN);
        ImGui::TextWrapped("This 3DS image is encrypted. Azahar needs your own AES keys (aes_keys.txt in its user\\sysdata folder) to run it.");
        ImGui::PopStyleColor();
    } else if (e.issue == "keys") {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN);
        std::string ud;
        hubemu::Install ki = hubemu::find(hubemu::Kind::Cemu, A.st.cemu_exe, e.path);
        if (!ki.exe.empty()) { const std::string ad_s = winutil::getenv_utf8("APPDATA"); const char* ad = ad_s.empty() ? nullptr : ad_s.c_str(); ud = ki.managed ? ki.dir + "\\portable" : hub::cemu_dirs_for(ki.dir, ad ? ad : "", hubemu::wiiu_probe()).user_data; }
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
    if (!idle && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", hubui::launch_busy_reason(A.proc != nullptr));
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
            ImGui::SetTooltip("%s", e.system == hub::System::PSP ? "PSP games play offline only: PPSSPP has no online rooms in RetroHub" : e.system == hub::System::Switch ? "Switch games play offline only for now (Lichii is very experimental)"
                              : !online_block_reason(e).empty() ? online_block_reason(e).c_str() : "Online play isn't available for this game");
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
        if (!idle && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", hubui::launch_busy_reason(A.proc != nullptr));
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
    int tab_shown = tab;   // what is drawn; `tab` keeps the request so a gallery that is seeded a frame later still lands on its tab
    {
        bool ok = false;
        for (auto& t : tabs) ok |= t.id == tab;
        if (!ok) {
            if (g_headless && std::getenv("HUB_SHOT_TAB")) {   // the screenshot aid asked for a tab this game does not have: clamp to the nearest one that exists
                tab_shown = tabs.back().id;
                for (auto& t : tabs) if (t.id >= tab) { tab_shown = t.id; break; }
            } else tab = tab_shown = TabAbout;
        }
    }
    for (size_t i = 0; i < tabs.size(); ++i) {
        float w = label_w(tabs[i].label.c_str(), false) - sp(1.5f);
        if (i) ImGui::SameLine(0, sp(1.f));
        ImGui::PushID((int)i);
        if (button(tabs[i].label.c_str(), ImVec2(w, sp(4.f)), tab_shown == tabs[i].id)) tab = tab_shown = tabs[i].id;
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    /* the content area keeps one height for every tab so the page does not jump; it scrolls inside if needed */
    /* The footer (secondary actions, wraps to several rows) keeps its measured height from the last frame; the body takes what is left of the
     * window's maximum height and scrolls inside it, so nothing is ever hidden behind the buttons. */
    static float footer_h = 0.f;
    const float footer_est = footer_h > 0.f ? footer_h : bh2 * 2.f + sp(1.5f);
    float room = hubui::lc::body_room(win_max_h, ImGui::GetCursorPosY(), footer_est + sp(1.f), win_pad, sp(6.f));
    float tabh = std::min(sp(36.f), room);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, std::min(tabh, sp(15.f))), ImVec2(FLT_MAX, tabh));
    ImGui::BeginChild("##tabbody", ImVec2(0, 0), ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AutoResizeY);
    if (tab_shown == TabAbout) {
        dumpwarn_details_section(e);   // first: a copy that looks incomplete must be seen without scrolling
        nuprepair_details_section(e);   // a damaged .nup copy RetroHub fills in at launch
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
        hack_details_section(e);   // Versions row (Original + hacks) and Remove hack
        progress_details_section(e);   // Wave 3: completion + notes
        transferpak_details_section(e);
        gba_link_details_section(e);
        palpark_details_section(e);
        verify_details_section(e);
        hookshot_details_section(e);   // Ocarina of Time (Switch 2) only: build the native version
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        if (gamepack_owns(e)) {   // installed from a shared pack: its folder (full path on hover) + Uninstall; the archive file name inside is not interesting
            const std::string ft = hubgp::installed_folder_title(e.path, hubgp::pack_root());
            const std::string full = hubgp::install_dir(sys_short(e), ft);
            ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
            ImGui::TextUnformatted(ui::ellipsize(ui::F.cap, ImGui::GetFontSize(), "Installed in " + sys_short(e) + "\\" + ft + "  (RetroHub games folder)", ImGui::GetContentRegionAvail().x).c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", full.c_str());
            ImGui::Dummy(ImVec2(0, sp(0.5f)));
            if (gamepack_uninstall_ui(e, sp(5.f))) ImGui::CloseCurrentPopup();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextWrapped("%s", e.path.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
    } else if (tab_shown == TabSaves) {
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
        resume_details_section(e);   // Wave 3 R1
        draw_state_strip(e);
    } else if (tab_shown == TabShots) {
        shots_gallery(e);
    } else if (tab_shown == TabSettings) {
        gs_draw_game_section(e);
    }
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, sp(1.f)));

    /* secondary actions */
    const float footer_y0 = ImGui::GetCursorPosY();
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
            ImGui::SetTooltip("Cheats aren't available for %s yet", hub::console_name(e));
    }
    if (cheats_supported(e)) {
        int cn = cheats_enabled_count(e);
        std::string cl = cn ? "Cheats (" + std::to_string(cn) + ")" : std::string("Cheats");
        place(label_w(cl.c_str(), true));
        if (button(cl.c_str(), ImVec2(0, bh2), cn > 0, true, ui::Icon::Bolt) || forced("cheats")) { ImGui::CloseCurrentPopup(); hub::GameEntry c = e; cheats_open(c); shot_trace("cheats", "opened " + e.key); }
    }
    tr("controls", hubctl::console_key(e).empty() ? "hidden" : "enabled:" + hubctl::console_key(e));
    tr("settings", hub::gs::options_for(e.system).empty() ? "none" : "options=" + std::to_string(hub::gs::options_for(e.system).size()));
    if (!hubctl::console_key(e).empty()) {
        place(label_w("Controls", true));
        if (button("Controls", ImVec2(0, bh2), false, true, ui::Icon::Gamepad) || forced("controls")) { ImGui::CloseCurrentPopup(); open_controls_for(e); shot_trace("controls", "tab-for " + hubctl::console_key(e) + " game=" + CTL_game_for_trace()); }
    }
    tr("desktop", "enabled");
    place(label_w("Add to desktop", true));
    if (button("Add to desktop", ImVec2(0, bh2), false, true, ui::Icon::Star) || forced("desktop")) { add_game_shortcut(e); shot_trace("desktop", e.key); }
    place(label_w("Customize", true));   // rename / cover / reset (customize_ui.cpp)
    if (button("Customize", ImVec2(0, bh2), false, true, ui::Icon::Gear)) custom_open_customize(e.key);
    place(label_w("Add to shelf...", true));
    if (button("Add to shelf...", ImVec2(0, bh2), false, true, ui::Icon::Plus)) custom_open_picker(e.key, true);
    if (hack_can_add(e)) {   // ROM hacks and translations (hack_ui.cpp): pick a patch file, RetroHub finds the base game and adds a new entry
        place(label_w("Add hack or translation", true));
        if (button("Add hack or translation", ImVec2(0, bh2), false, true, ui::Icon::Plus)) { ImGui::CloseCurrentPopup(); hack_add_open(e); }
    }
    if (!native) {   // Hide game (library-12): one press, Undo card after; synced like the Hidden games list
        place(label_w("Hide game", true));
        if (button("Hide game", ImVec2(0, bh2), false, true, ui::Icon::Close)) {
            const std::string hk = e.key;
            hide_game_now(hk);   // e is gone from the library after this: nothing below may use it
            A.show_details = false;
            ImGui::CloseCurrentPopup();
            if (short_win) ImGui::PopStyleVar();
            ImGui::EndPopup();
            return;   // nothing after this touches the (now removed) entry
        }
    }
    footer_h = ImGui::GetCursorPosY() - footer_y0 + ImGui::GetStyle().ItemSpacing.y;
    custom_draw(1);   // Customize / Add to shelf modals nest inside this one
    if (short_win) ImGui::PopStyleVar();
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
    if (added_stamps_changed() || A.vis_dirty || A.vis_filter != A.sys_filter || A.vis_query != A.search_buf) rebuild_vis();
    ensure_selection();
    if (!lib_cases_active()) library_top_bar();   // Cases hides it (its menu overlay shows it)
    A.page_scrollable = false;

    if (A.lib.empty() && A.ghosts.empty()) {   // nothing of your own and no friends' games either
        if (A.scanning && !A.scanned_once) skeleton_home();
        else if (A.scanning) {}   // rescan of an empty library: top bar shows the progress line
        else empty_library_state();
        return;
    }
    if (A.view == LibView::All) {
        all_games_view();
        return;
    }
    if (own_library_empty() && !A.scanning) {   // only the bundled game: ask for game folders
        begin_panel("add_folders_card");
        ImGui::PushFont(ui::F.bold);
        ImGui::TextUnformatted("Add your game folders");
        ImGui::PopFont();
        text_wrapped_disabled("Only the bundled game is here. Point RetroHub at the folder(s) with your N64, GBA, GB, DS, 3DS and Wii U games and they all show up.");
        if (!scan_last_line().empty()) text_wrapped_disabled(scan_last_line());
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

    finish_setup_card();   // Plan 4 W2 C: while first-run steps are open

    const hub::GameEntry* sel = find_entry(A.sel_game);
    if (sel) {
        if (A.hero_key != sel->key) { A.hero_key = sel->key; A.hero_t = 0.f; }
        ui::anim(A.hero_t, 1.f, 9.f);
        float avail_h = ImGui::GetContentRegionAvail().y;
        float hero_h = std::min(sp(40.f), std::max(sp(21.f), std::min(avail_h * 0.36f, ImGui::GetIO().DisplaySize.y * 0.25f)));   // <= a quarter of the window
        hero_h = tvmode::hero_h(hero_h, avail_h);
        if (attract_update()) attract_draw_hero(hero_h);   // L6: idle slideshow in place of the hero
        else if (continue_hero_ok(*sel)) draw_continue_hero(*sel, continue_hero_min_h(hero_h));   // played in the last 30 days: Continue hero
        else draw_hero(*sel, hero_h);
    }
    /* The shelf strips inset their covers by `bleed` (room for the hover grow); pull the child out by the same amount so
     * headers and the first cover start on the same left edge as the hero and the top bar. */
    const float lib_bleed = sp(1.5f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - lib_bleed);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    home_note_page();
    home_scroll_pre();
    ImGui::BeginChild("shelves", ImVec2(ImGui::GetContentRegionAvail().x + lib_bleed, 0), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
    home_scroll_post();
    ImGui::PopStyleVar();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(sp(2.f), 0));
    A.page_scrollable = ImGui::GetScrollMaxY() > 0;
    home_shelves();
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

