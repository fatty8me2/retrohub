/* Library desk: the second library layout (Settings > General > Layout), stage 1.
 *
 *   [ icon sidebar ][ list column            ][ game pane                                   ]
 *                   [ search box             ][ cover  title / console and players / time   ]
 *                   [ Recent        - ]      ][        Play  Play Online  Options  star      ]
 *                   [   game rows            ][ about this game, screenshots                ]
 *                   [ Nintendo 64 . 19  + ]  ][                                             ]
 *                   [ Add games              ][                                             ]
 *
 * The list is ONE focusable item: the cursor row, the groups and the scrolling are ours (desk_logic.h), so the pad / keyboard moves are
 * read from ImGui's own move requests (same repeat rate as everywhere) and turned into row moves. Up on the first row and Down on the
 * last one let ImGui move the focus out (search box / Add games); Left on a closed group lets it reach the sidebar. Right on a game
 * enters the pane (its first focusable item), Left with nothing further left in the pane, or B, comes back to the same row.
 * Selection follows the cursor (A.sel_game), so every library shortcut keeps working: Y / I opens the game page popup (##details),
 * X / F favourites, R3 / S adds to a shelf. Enter / A / double-click plays through play_entry_anim, exactly like the game page's Play.
 * Colours are theme tokens only. Nothing here animates on its own once the list has stopped scrolling. */
#include "desk_view.h"
#include "desk_logic.h"
#include "console_names.h"
#include "convenience.h"
#include "launch_guard_logic.h"
#include "playtime_logic.h"
#include "ui_sound.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>

namespace {

using hubdesk::Act;
using hubdesk::Key;

struct DeskState {
    std::vector<hubdesk::Game> games;   // built from A.lib, same order
    hubdesk::Model model;
    hubdesk::OpenMap open;
    std::string open_src = "\x01";      // A.st.desk_groups the map was parsed from
    std::set<std::string> search_closed;   // groups closed during the current search (session only)
    int closed_ver = 0, built_closed_ver = -1;
    std::string query = "\x01";         // A.search_buf the model was built for
    uint32_t vis_gen = ~0u;
    size_t lib_n = ~(size_t)0;
    bool dirty = true;
    int builds = 0;
    /* cursor / selection */
    int cursor = -1;                    // the row the list cursor is on
    hubdesk::RowRef cursor_ref;
    int sel_row = -1;                   // the row that shows the selection fill (the selected game's row)
    std::string seen_sel = "\x01";      // A.sel_game as last seen: an outside change moves the cursor to it
    /* scrolling (content pixels, the list's own) */
    hubdesk::Metrics mt;
    hubdesk::Layout L;
    int layout_build = -1;
    float layout_S = 0.f;
    float scroll = 0.f, target = 0.f;
    bool reveal = true;
    float view_h = 0.f;
    float thumb_grab = 0.f;
    /* focus */
    bool focus_list = false, focus_list_visible = false, focus_pane = false;
    bool list_focused = false, pane_focused = false;
    bool list_tip = false;              // the list is hovered long enough for a tooltip
    ImGuiID list_id = 0, pane_child = 0;
} D;

/* ---------- small helpers ---------- */

int local_tz() {   // local time minus UTC in seconds (the recipe page_library.cpp and wrapped_ui.cpp use)
    static const int tz = [] {
        std::time_t t = std::time(nullptr);
        std::tm lt{}, gm{};
        localtime_s(&lt, &t);
        gmtime_s(&gm, &t);
        int z = (int)std::difftime(std::mktime(&lt), std::mktime(&gm));
        if (lt.tm_isdst > 0) z += 3600;
        return z;
    }();
    return tz;
}

bool any_popup() { return ImGui::IsPopupOpen((const char*)nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel); }

void lock_key(ImGuiKey k) { ImGui::SetKeyOwner(k, ImGuiKeyOwner_NoOwner, ImGuiInputFlags_LockThisFrame); }

/* The item just submitted takes the nav focus (libview_ui.cpp focus_item_here). */
void focus_item_here(bool visible) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiWindow* w = g.CurrentWindow;
    if (g.NavWindow != w) ImGui::FocusWindow(w);
    const ImVec2 off = w->DC.CursorStartPos;
    const ImRect rel(ImGui::GetItemRectMin() - off, ImGui::GetItemRectMax() - off);
    ImGui::SetNavID(ImGui::GetItemID(), ImGuiNavLayer_Main, g.CurrentFocusScopeId, rel);
    if (visible) g.NavCursorVisible = true;
}

/* The first focusable item submitted from here on (in the current window) takes the focus (frame.cpp page_focus_req). */
void focus_first_item_here() {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGui::FocusWindow(g.CurrentWindow);
    ImGui::SetNavID(0, ImGuiNavLayer_Main, g.CurrentWindow->NavRootFocusScopeId, ImRect());
    g.NavInitRequest = true;
    g.NavInitRequestFromMove = false;
    g.NavInitResult.ID = 0;
    g.NavAnyRequest = true;
    g.NavCursorVisible = true;
}

std::string group_title(const hubdesk::Group& grp) {
    if (grp.kind == hubdesk::GroupKind::Recent) return "Recent";
    if (grp.kind == hubdesk::GroupKind::Favorites) return "Favorites";
    if (grp.id == "other") return "Other";
    const char* n = hub::full_name_id(grp.id);
    return n && *n ? std::string(n) : grp.id;
}

/* Why a game cannot be played yet ("" = it can). Same cases the cover tiles mark as a warning. */
const char* not_ready_reason(const hub::GameEntry& e) {
    if (e.issue == "encrypted") return "This copy is encrypted, so it can't be played yet.";
    if (e.issue == "keys") return "This game needs decryption keys before it can be played.";
    return "";
}

bool launch_idle() { return hubui::launch_allowed(A.proc != nullptr, session_active()); }

/* Play: the game page's Play button, exactly (play_entry_anim checks for a running game, the lock, missing files, the cloud...).
 * Headless runs never start anything: spawn() refuses there and traces it. */
void desk_play(const hub::GameEntry& e) {
    shot_trace("desk-play", e.key);
    ui_sound::play(ui_sound::Sfx::Select);
    hub::GameEntry c = e;
    play_entry_anim(c);
}

const hub::GameEntry* row_entry(int row) {
    if (row < 0 || row >= (int)D.model.rows.size()) return nullptr;
    const hubdesk::Row& r = D.model.rows[(size_t)row];
    return r.header() ? nullptr : find_entry(D.games[(size_t)r.game].key);
}

/* ---------- model ---------- */

void sync_sel_row() {
    const int n = (int)D.model.rows.size();
    if (D.cursor >= 0 && D.cursor < n && !D.model.rows[(size_t)D.cursor].header() && D.games[(size_t)D.model.rows[(size_t)D.cursor].game].key == A.sel_game)
        D.sel_row = D.cursor;
    else D.sel_row = hubdesk::row_of_key(D.model, D.games, A.sel_game);
}

void select_row(int row) {
    if (row < 0 || row >= (int)D.model.rows.size() || D.model.rows[(size_t)row].header()) return;
    A.sel_game = D.games[(size_t)D.model.rows[(size_t)row].game].key;
    A.sel_ghost.clear();
    D.seen_sel = A.sel_game;
}

void trace_cursor() {
    if (D.cursor < 0 || D.cursor >= (int)D.model.rows.size()) return;
    const hubdesk::Row& r = D.model.rows[(size_t)D.cursor];
    shot_trace("desk-cursor", "row=" + std::to_string(D.cursor) + "/" + std::to_string(D.model.rows.size()) + " group=" + D.model.groups[(size_t)r.group].id +
               " key=" + (r.header() ? std::string("-") : D.games[(size_t)r.game].key) + " sel=" + A.sel_game);
}

void set_cursor(int row, bool sound) {
    if (row < 0 || row >= (int)D.model.rows.size()) return;
    const bool moved = row != D.cursor;
    D.cursor = row;
    D.cursor_ref = hubdesk::ref_of(D.model, D.games, row);
    select_row(row);   // selection follows the cursor onto games; on a header the last game stays selected
    sync_sel_row();
    D.reveal = true;
    if (moved && sound) ui_sound::play(ui_sound::Sfx::Move);
    if (moved) trace_cursor();
}

void rebuild() {
    const std::string q = A.search_buf;
    if (A.st.desk_groups != D.open_src) { D.open = hubdesk::parse_open(A.st.desk_groups); D.open_src = A.st.desk_groups; D.dirty = true; }
    const bool query_changed = q != D.query;
    if (query_changed) { D.search_closed.clear(); ++D.closed_ver; }
    if (!D.dirty && !query_changed && D.vis_gen == A.vis_gen && D.lib_n == A.lib.size() && D.built_closed_ver == D.closed_ver) return;
    D.dirty = false;
    D.query = q;
    D.vis_gen = A.vis_gen;
    D.lib_n = A.lib.size();
    D.built_closed_ver = D.closed_ver;
    ++D.builds;
    D.games.clear();
    D.games.reserve(A.lib.size());
    for (const hub::GameEntry& e : A.lib) {   // A.lib never holds hidden games (apply_scan takes them out)
        const hub::GameState& gs = gstate(e.key);
        hubdesk::Game g;
        g.key = e.key;
        g.title = e.title;
        g.console = hub::console_id(e);
        g.last_played = gs.last_played;
        g.favorite = gs.favorite;
        g.pinned = is_native(e);
        D.games.push_back(std::move(g));
    }
    D.model = hubdesk::build(D.games, q, D.open, D.search_closed);
    const int n = (int)D.model.rows.size();
    const int old_cursor = D.cursor;
    int c = hubdesk::find_row(D.model, D.games, D.cursor_ref);
    if (c < 0) c = hubdesk::row_of_key(D.model, D.games, A.sel_game);
    if (c < 0 && n > 0) c = std::max(0, std::min(n - 1, D.cursor));
    if (query_changed && D.model.searching && !hubdesk::has_game(D.model, D.games, A.sel_game)) {   // the search hides the selected game: the first match takes over
        const int f = hubdesk::first_game_row(D.model);
        if (f >= 0) { c = f; select_row(f); }
    }
    D.cursor = n > 0 ? c : -1;
    D.cursor_ref = hubdesk::ref_of(D.model, D.games, D.cursor);
    D.seen_sel = A.sel_game;
    sync_sel_row();
    if (D.cursor != old_cursor || D.builds == 1) D.reveal = true;   // a rebuild that leaves the cursor where it was keeps the scroll too
    shot_trace("desk-model", "games=" + std::to_string(D.model.total) + " matches=" + std::to_string(D.model.matches) + " groups=" + std::to_string(D.model.groups.size()) +
               " rows=" + std::to_string(n) + " cursor=" + std::to_string(D.cursor));
}

/* Something else chose a game (the palette, the start animation, the details popup...): the cursor follows when the game is in view. */
void follow_outside_selection() {
    if (A.sel_game == D.seen_sel) return;
    D.seen_sel = A.sel_game;
    const hub::GameEntry* cur = row_entry(D.cursor);
    if (!cur || cur->key != A.sel_game) {
        const int r = hubdesk::row_of_key(D.model, D.games, A.sel_game);
        if (r >= 0) { D.cursor = r; D.cursor_ref = hubdesk::ref_of(D.model, D.games, r); D.reveal = true; }
    }
    sync_sel_row();
}

void set_group_open(int gi, bool open) {
    if (gi < 0 || gi >= (int)D.model.groups.size()) return;
    const std::string id = D.model.groups[(size_t)gi].id;
    if (D.model.searching) {   // a search opens every group with a match; closing one lasts until the search text changes
        if (open) D.search_closed.erase(id); else D.search_closed.insert(id);
        ++D.closed_ver;
    } else {
        D.open[id] = open;
        A.st.desk_groups = hubdesk::format_open(D.open);
        D.open_src = A.st.desk_groups;
        D.dirty = true;
        mark_dirty();
    }
    D.cursor_ref = hubdesk::RowRef{id, ""};   // the cursor stays on the header
    rebuild();
    D.cursor = hubdesk::header_row(D.model, id);
    D.cursor_ref = hubdesk::ref_of(D.model, D.games, D.cursor);
    sync_sel_row();
    ui_sound::play(ui_sound::Sfx::Select);
    shot_trace("desk-group", id + (open ? " open" : " closed"));
}

/* One step of the list cursor; true = let ImGui's own move go on (the focus leaves the list). */
bool apply_step(const hubdesk::Step& s) {
    switch (s.act) {
    case Act::None: return false;
    case Act::Move: set_cursor(s.row, true); return false;
    case Act::Open: set_group_open(s.group, true); return false;
    case Act::Close: set_group_open(s.group, false); return false;
    case Act::EnterPane: D.focus_pane = true; ui_sound::play(ui_sound::Sfx::Move); return false;
    case Act::Play: if (const hub::GameEntry* e = row_entry(s.row)) desk_play(*e); return false;
    case Act::Edge: case Act::Leave: return true;
    }
    return false;
}

/* ---------- the list column ---------- */

void draw_header_row(ImDrawList* dl, const ImRect& rr, const hubdesk::Group& grp, bool hov, bool ring) {
    const float rad = sp(0.75f);
    if (hov) dl->AddRectFilled(rr.Min, rr.Max, ui::veil(0.04f), rad);
    ImFont* bf = ui::F.bold;
    ImFont* cf = ui::F.cap;
    const float fs = cf->FontSize;
    const float y = rr.Min.y + (rr.GetHeight() - fs) * 0.62f;   // a little low: the label sits with the games below it
    const ImVec2 mc(rr.Max.x - sp(1.75f), y + fs * 0.5f);
    const std::string count = "\xC2\xB7 " + std::to_string(grp.items.size());   // "· 19"
    const float cw = cf->CalcTextSizeA(fs, FLT_MAX, 0.f, count.c_str()).x;
    const float x = rr.Min.x + sp(1.f);
    const float room = std::max(sp(3.f), mc.x - sp(1.75f) - x - cw - sp(0.75f));
    const std::string full = group_title(grp);
    const std::string title = ui::ellipsize(bf, fs, full, room);
    const float tw = bf->CalcTextSizeA(fs, FLT_MAX, 0.f, title.c_str()).x;
    dl->AddText(bf, fs, ImVec2(x, y), hov ? ui::TEXT : ui::TEXT2, title.c_str());
    dl->AddText(cf, fs, ImVec2(x + tw + sp(0.75f), y), ui::TEXT3, count.c_str());
    const float s = sp(0.6f), th = std::max(1.f, sp(0.2f));   // open: minus, closed: plus
    const ImU32 mcol = hov ? ui::TEXT : ui::TEXT2;
    dl->AddLine(ImVec2(mc.x - s, mc.y), ImVec2(mc.x + s, mc.y), mcol, th);
    if (!grp.open) dl->AddLine(ImVec2(mc.x, mc.y - s), ImVec2(mc.x, mc.y + s), mcol, th);
    if (ring) ui::focus_ring(dl, rr.Min, rr.Max, rad);
    if (hov && D.list_tip && title != full) ImGui::SetTooltip("%s", full.c_str());
}

void draw_game_row(ImDrawList* dl, const ImRect& rr, int row, const hubdesk::Group& grp, const hubdesk::Game& gm, bool hov, bool ring) {
    const hub::GameEntry* e = find_entry(gm.key);
    const float rad = sp(0.75f);
    if (row == D.sel_row) {
        dl->AddRectFilled(rr.Min, rr.Max, ui::PANEL2, rad);
        dl->AddRectFilled(ImVec2(rr.Min.x, rr.Min.y + sp(0.75f)), ImVec2(rr.Min.x + sp(0.375f), rr.Max.y - sp(0.75f)), ui::ACCENT, sp(0.2f));
    } else if (hov) dl->AddRectFilled(rr.Min, rr.Max, ui::veil(0.04f), rad);
    const char* why = e ? not_ready_reason(*e) : "";
    const bool ready = !*why;
    const float th = rr.GetHeight() - sp(1.25f);
    const ImVec2 a0(rr.Min.x + sp(1.f), rr.Min.y + (rr.GetHeight() - th) * 0.5f), a1(a0.x + th, a0.y + th);
    const float trad = sp(0.5f);
    ImU32 c0 = ui::PANEL3, c1 = ui::PANEL2;
    if (e) entry_colors(*e, &c0, &c1);
    const ArtRef art = e ? art_for_entry(*e) : ArtRef();   // only rows on screen ask for art
    if (art.tex) ui::draw_art(dl, art.tex, art.w, art.h, a0, a1, trad, ready ? 1.f : 0.45f);
    else gradient_round_rect(dl, a0, a1, ready ? c0 : mix(ui::PANEL2, c0, 0.3f), ready ? c1 : mix(ui::PANEL2, c1, 0.3f), trad);
    if (art.loading) ui::shimmer(dl, a0, a1, trad, ui::with_alpha(ui::PANEL, 0.f));
    dl->AddRect(a0, a1, ui::HAIRLINE, trad, 0, 1.f);
    ImFont* bf = ui::F.body;
    ImFont* cf = ui::F.cap;
    const float cy = (rr.Min.y + rr.Max.y) * 0.5f;
    float rx = rr.Max.x - sp(1.f);
    if (gm.favorite) {   // favourite: a small star
        ui::icon(dl, ui::Icon::StarFilled, ImVec2(rx - sp(0.8f), cy), sp(1.6f), ui::WARN);
        rx -= sp(2.25f);
    }
    if (grp.kind != hubdesk::GroupKind::Console) {   // Recent / Favorites mix consoles: say which
        const char* sn = hub::short_name_id(gm.console);
        if (sn && *sn) {
            const float w = cf->CalcTextSizeA(cf->FontSize, FLT_MAX, 0.f, sn).x;
            dl->AddText(cf, cf->FontSize, ImVec2(rx - w, cy - cf->FontSize * 0.5f), ui::TEXT3, sn);
            rx -= w + sp(1.f);
        }
    }
    const float tx = a1.x + sp(1.25f);
    const std::string t = ui::ellipsize(bf, bf->FontSize, gm.title, std::max(sp(2.f), rx - tx));
    dl->AddText(bf, bf->FontSize, ImVec2(tx, cy - bf->FontSize * 0.5f), ready ? ui::TEXT : ui::TEXT3, t.c_str());
    if (ring) ui::focus_ring(dl, rr.Min, rr.Max, rad);
    if (hov && D.list_tip) {
        if (!ready) ImGui::SetTooltip("%s\n%s", gm.title.c_str(), why);
        else if (t != gm.title) ImGui::SetTooltip("%s", gm.title.c_str());
    }
}

void search_box(const ImRect& r) {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float bh = r.GetHeight();
    const bool has_text = A.search_buf[0] != 0;
    if (has_text) {   // the clear button sits inside the box; submitted first so it gets the mouse before the text field
        ImGui::SetCursorScreenPos(ImVec2(r.Max.x - bh, r.Min.y));
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        const bool clr = ImGui::InvisibleButton("##desk_clear", ImVec2(bh, bh));
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopItemFlag();
        if (hov) ImGui::SetTooltip("Clear search");
        if (clr) { A.search_buf[0] = 0; shot_trace("desk-search", "cleared"); }
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) A.focus_search = true;   // Ctrl+F, as in the other layout
    if (type_to_search_pending()) A.focus_search = true;                              // just start typing
    ImGui::SetCursorScreenPos(r.Min);
    if (A.focus_search) { ImGui::SetKeyboardFocusHere(); A.focus_search = false; }
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(sp(4.5f), (bh - ImGui::GetFontSize()) * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, bh * 0.5f);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNavDefaultFocus, true);
    ImGui::SetNextItemWidth(r.GetWidth());
    osk::input("##desk_search", "Search games", A.search_buf, sizeof A.search_buf);
    const bool active = ImGui::IsItemActive();
    {   // Down from the (not typing) search box: into the list, on its cursor row (the pane beside it is not "below")
        ImGuiContext& g = *ImGui::GetCurrentContext();
        if (ImGui::IsItemFocused() && !active && g.NavMoveSubmitted && g.NavMoveDir == ImGuiDir_Down && !D.model.rows.empty() &&
            !(g.NavMoveFlags & (ImGuiNavMoveFlags_IsTabbing | ImGuiNavMoveFlags_FocusApi | ImGuiNavMoveFlags_IsPageMove | ImGuiNavMoveFlags_ScrollToEdgeY))) {
            ImGui::NavMoveRequestCancel();
            D.focus_list = D.focus_list_visible = true;
        }
    }
    if (ImGui::IsItemDeactivated() && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) {   // Enter: on to the list
        D.focus_list = true;
        D.focus_list_visible = true;
    }
    if (ImGui::IsItemHovered() && !active) ImGui::SetTooltip("Search your games  (Ctrl+F)");
    ImGui::PopItemFlag();
    ImGui::PopStyleVar(2);
    dl->AddRect(r.Min, r.Max, active ? ui::ACCENT : ui::HAIRLINE, bh * 0.5f, 0, 1.f);
    ui::icon(dl, ui::Icon::Search, ImVec2(r.Min.x + sp(2.25f), (r.Min.y + r.Max.y) * 0.5f), sp(2.f), ui::TEXT2);
    if (has_text && A.search_buf[0]) {
        const bool hov = ImGui::IsMouseHoveringRect(ImVec2(r.Max.x - bh, r.Min.y), r.Max);
        ui::icon(dl, ui::Icon::Close, ImVec2(r.Max.x - bh * 0.5f, (r.Min.y + r.Max.y) * 0.5f), sp(1.75f), hov ? ui::TEXT : ui::TEXT2);
    }
}

/* The list area when there is nothing to list: a plain line (and Clear search for a search with no match). */
void list_empty(const ImRect& vp) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cx = (vp.Min.x + vp.Max.x) * 0.5f, w = vp.GetWidth() - sp(3.f);
    float y = vp.Min.y + sp(4.f);
    std::string msg;
    if (A.lib.empty()) msg = A.scanning ? "Looking for your games..." : "Your games will show up here.";
    else msg = std::string("No games match \"") + A.search_buf + "\".";
    for (const std::string& ln : ui::wrap_lines(ui::F.body, ui::F.body->FontSize, msg, w, 3)) {
        centered_text(dl, ui::F.body, cx, y, ui::TEXT2, ln.c_str());
        y += ui::F.body->FontSize * 1.3f;
    }
    if (!A.lib.empty() && A.search_buf[0]) {
        /* the focus comes here when it was on the list (now gone), was asked for (Enter in the search box) or the page just opened */
        const bool want = (A.nav_seed || D.focus_list || (D.list_id != 0 && ImGui::GetCurrentContext()->NavId == D.list_id)) && !ImGui::GetIO().WantTextInput;
        const bool visible = D.focus_list_visible || D.list_focused;
        const float bw = std::min(w, sp(18.f)), bh = sp(4.5f);
        ImGui::SetCursorScreenPos(ImVec2(cx - bw * 0.5f, y + sp(1.5f)));
        if (ui::button("Clear search", ImVec2(bw, bh), false, true, ui::Icon::Close)) { A.search_buf[0] = 0; D.focus_list = D.focus_list_visible = true; }   // the list again, focused
        else if (want) { A.nav_seed = false; D.focus_list = D.focus_list_visible = false; focus_item_here(visible); }
    }
    D.list_focused = false;
    A.lib_pg = false;
}

void draw_list(const ImRect& vp) {
    ImGuiIO& io = ImGui::GetIO();
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float lp = sp(0.75f);   // room above the first row and below the last (the focus ring is never clipped)
    if (D.layout_build != D.builds || D.layout_S != ui::S) {
        D.mt = hubdesk::Metrics{sp(4.f), sp(4.75f), sp(1.f)};
        D.L = hubdesk::layout_rows(D.model.rows, D.mt);
        for (float& t : D.L.top) t += lp;
        D.L.total += lp * 2.f;
        D.layout_build = D.builds;
        D.layout_S = ui::S;
    }
    const std::vector<hubdesk::Row>& rows = D.model.rows;
    const int n = (int)rows.size();
    const float view_h = vp.GetHeight();
    D.view_h = view_h;
    const bool sb = D.L.total > view_h + 0.5f;
    const float sb_hit = sp(1.75f);
    const float list_w = std::max(1.f, vp.GetWidth() - (sb ? sb_hit : 0.f));

    ImGui::SetCursorScreenPos(vp.Min);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ui::with_alpha(ui::TEXT, 0.f));   // the cursor row draws its own focus ring, not the whole list
    ImGui::InvisibleButton("##desk_list", ImVec2(list_w, std::max(1.f, view_h)), ImGuiButtonFlags_EnableNav | ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    ImGui::PopStyleColor();
    const ImGuiID lid = ImGui::GetItemID();
    D.list_id = lid;
    const bool hovered = ImGui::IsItemHovered();
    D.list_tip = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
    if ((A.nav_seed && !io.WantTextInput) || D.focus_list) {   // arriving on the page, back from the pane, Enter in the search box
        focus_item_here(D.focus_list && D.focus_list_visible);
        if (D.focus_list) shot_trace("desk-focus", "list");
        A.nav_seed = false;
        D.focus_list = D.focus_list_visible = false;
    }
    const bool focused = g.NavId == lid;
    D.list_focused = focused;
    A.lib_pg = focused;   // LT / RT page this list (main.cpp then sends no PageUp / PageDown)

    /* mouse: the rows as the player saw them last frame */
    int hover_row = -1;
    if (hovered) {
        hover_row = hubdesk::row_at(D.L, rows, D.mt, io.MousePos.y - vp.Min.y + D.scroll);
        if (hover_row >= 0) {
            const bool head = rows[(size_t)hover_row].header();
            if (ImGui::IsMouseClicked(1) && !head) { set_cursor(hover_row, false); A.show_details = true; }   // right-click: the game page popup
            if (ImGui::IsMouseClicked(0)) {
                const bool twice = ImGui::IsMouseDoubleClicked(0);   // the second press of a double-click is a click too
                if (head) { if (!twice) set_group_open(rows[(size_t)hover_row].group, !D.model.groups[(size_t)rows[(size_t)hover_row].group].open); }
                else {
                    set_cursor(hover_row, false);
                    if (twice) if (const hub::GameEntry* e = row_entry(hover_row)) desk_play(*e);
                }
            }
            if (hover_row >= (int)D.model.rows.size()) hover_row = -1;   // a group just opened or closed under the pointer
        }
    }
    if (io.MouseWheel != 0.f && ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(vp.Min, vp.Max)) {   // the wheel, over the rows or the scrollbar
        D.target -= io.MouseWheel * D.mt.row_h * 3.f;
        D.reveal = false;
    }

    /* pad / keyboard while the list has the focus */
    const bool keys_ok = focused && A.focused && !io.WantTextInput && !any_popup() && !osk::is_open();
    if (keys_ok && n > 0) {
        const int page = hubdesk::page_rows(view_h, D.mt.row_h);
        if (g.NavMoveSubmitted && !(g.NavMoveFlags & (ImGuiNavMoveFlags_IsTabbing | ImGuiNavMoveFlags_FocusApi))) {
            if (g.NavMoveFlags & (ImGuiNavMoveFlags_IsPageMove | ImGuiNavMoveFlags_ScrollToEdgeY)) ImGui::NavMoveRequestCancel();   // PageUp / PageDown / Home / End: below
            else {
                Key k = Key::Down;
                bool known = true;
                switch (g.NavMoveDir) {
                case ImGuiDir_Up: k = Key::Up; break;
                case ImGuiDir_Down: k = Key::Down; break;
                case ImGuiDir_Left: k = Key::Left; break;
                case ImGuiDir_Right: k = Key::Right; break;
                default: known = false; break;
                }
                if (known && !apply_step(hubdesk::step(D.model, D.cursor, k, page))) ImGui::NavMoveRequestCancel();
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp, true) || (A.pad_mode && ImGui::IsKeyPressed(ImGuiKey_GamepadL2, true))) apply_step(hubdesk::step(D.model, D.cursor, Key::PageUp, page));
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown, true) || (A.pad_mode && ImGui::IsKeyPressed(ImGuiKey_GamepadR2, true))) apply_step(hubdesk::step(D.model, D.cursor, Key::PageDown, page));
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) apply_step(hubdesk::step(D.model, D.cursor, Key::Home, page));
        if (ImGui::IsKeyPressed(ImGuiKey_End, false)) apply_step(hubdesk::step(D.model, D.cursor, Key::End, page));
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false))
            apply_step(hubdesk::step(D.model, D.cursor, Key::Activate, page));
    } else if (keys_ok && g.NavMoveSubmitted && (g.NavMoveFlags & (ImGuiNavMoveFlags_IsPageMove | ImGuiNavMoveFlags_ScrollToEdgeY))) ImGui::NavMoveRequestCancel();
    if (keys_ok && A.search_buf[0] && !A.popup_last_frame && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))) {
        A.search_buf[0] = 0;   // B / Esc on the list: clear the search
        lock_key(ImGuiKey_Escape);
        lock_key(ImGuiKey_GamepadFaceRight);
        shot_trace("desk-search", "cleared");
    }

    /* the layout may have changed (a group opened, a search typed) */
    if (D.layout_build != D.builds || D.layout_S != ui::S) {
        D.L = hubdesk::layout_rows(D.model.rows, D.mt);
        for (float& t : D.L.top) t += lp;
        D.L.total += lp * 2.f;
        D.layout_build = D.builds;
        D.layout_S = ui::S;
    }
    const float total = D.L.total;

    /* scrollbar (drawn thin, grabbed wide) */
    const bool sb_now = total > view_h + 0.5f;
    const ImRect track(ImVec2(vp.Max.x - sb_hit, vp.Min.y + sp(0.5f)), ImVec2(vp.Max.x, vp.Max.y - sp(0.5f)));
    bool sb_hov = false, sb_act = false;
    if (sb_now) {
        ImGui::SetCursorScreenPos(track.Min);
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        ImGui::InvisibleButton("##desk_sb", ImVec2(track.GetWidth(), std::max(1.f, track.GetHeight())));
        ImGui::PopItemFlag();
        sb_hov = ImGui::IsItemHovered();
        sb_act = ImGui::IsItemActive();
        const hubdesk::Thumb t0 = hubdesk::thumb(D.scroll, total, view_h, track.GetHeight(), sp(3.f));
        if (ImGui::IsItemActivated()) {
            const float my = io.MousePos.y - track.Min.y;
            D.thumb_grab = (my >= t0.y0 && my <= t0.y1) ? my - t0.y0 : (t0.y1 - t0.y0) * 0.5f;   // a click beside the thumb jumps there
        }
        if (sb_act) {
            D.target = D.scroll = hubdesk::scroll_for_thumb(io.MousePos.y - track.Min.y - D.thumb_grab, t0.y1 - t0.y0, total, view_h, track.GetHeight());
            D.reveal = false;
        }
    }

    /* scroll: keep the cursor in view, ease toward the target */
    if (D.reveal && D.cursor >= 0) D.target = hubdesk::reveal(D.L, rows, D.mt, D.cursor, D.target, view_h, sp(1.75f));   // room for the whole focus ring
    D.reveal = false;
    D.target = hubdesk::clamp_scroll(D.target, total, view_h);
    D.scroll = hubdesk::clamp_scroll(D.scroll, total, view_h);
    if (std::fabs(D.target - D.scroll) < 0.5f || ui::reduce_motion) D.scroll = D.target;
    else ui::anim(D.scroll, D.target, 20.f);

    /* rows */
    int first = -1, last = -1;
    hubdesk::visible_rows(D.L, rows, D.mt, D.scroll, view_h, &first, &last);
    dl->PushClipRect(ImVec2(vp.Min.x - sp(0.6f), vp.Min.y), ImVec2(vp.Max.x + sp(0.6f), vp.Max.y), true);
    const float rx0 = vp.Min.x + sp(0.5f), rx1 = vp.Max.x - (sb_now ? sb_hit : sp(0.5f));
    const bool ring_on = focused && g.NavCursorVisible;
    for (int i = first; i >= 0 && i <= last && i < (int)rows.size(); ++i) {   // rows, not n: a group may have opened above
        const hubdesk::Row& r = rows[(size_t)i];
        const float y = vp.Min.y + D.L.top[(size_t)i] - D.scroll;
        const ImRect rr(ImVec2(rx0, y), ImVec2(rx1, y + hubdesk::row_height(r, D.mt)));
        const hubdesk::Group& grp = D.model.groups[(size_t)r.group];
        const bool hov = hovered && hover_row == i && !sb_act;
        const bool ring = ring_on && i == D.cursor;
        if (r.header()) draw_header_row(dl, rr, grp, hov, ring);
        else draw_game_row(dl, rr, i, grp, D.games[(size_t)r.game], hov, ring);
    }
    dl->PopClipRect();
    if (sb_now) {
        const hubdesk::Thumb t = hubdesk::thumb(D.scroll, total, view_h, track.GetHeight(), sp(3.f));
        const float bw = (sb_hov || sb_act) ? sp(0.75f) : sp(0.5f), bx = track.Min.x + (track.GetWidth() - bw) * 0.5f;
        dl->AddRectFilled(ImVec2(bx, track.Min.y), ImVec2(bx + bw, track.Max.y), ui::HAIRLINE, bw * 0.5f);
        dl->AddRectFilled(ImVec2(bx, track.Min.y + t.y0), ImVec2(bx + bw, track.Min.y + t.y1), (sb_hov || sb_act) ? ui::TEXT2 : ui::TEXT3, bw * 0.5f);
    }
}

void draw_column(const ImRect& col) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rad = sp(2.f), pad = sp(1.25f);
    dl->AddRectFilled(col.Min, col.Max, ui::PANEL, rad);
    dl->AddRect(col.Min, col.Max, ui::HAIRLINE, rad, 0, 1.f);
    const float sh = sp(4.5f), ah = sp(4.5f);
    const ImRect sr(ImVec2(col.Min.x + pad, col.Min.y + pad), ImVec2(col.Max.x - pad, col.Min.y + pad + sh));
    const ImRect ar(ImVec2(col.Min.x + pad, col.Max.y - pad - ah), ImVec2(col.Max.x - pad, col.Max.y - pad));
    const ImRect vp(ImVec2(col.Min.x + sp(0.75f), sr.Max.y + sp(1.f)), ImVec2(col.Max.x - sp(0.75f), ar.Min.y - sp(1.f)));
    search_box(sr);
    if (D.model.rows.empty()) list_empty(vp);
    else draw_list(vp);
    dl->AddLine(ImVec2(col.Min.x + pad, ar.Min.y - sp(0.5f)), ImVec2(col.Max.x - pad, ar.Min.y - sp(0.5f)), ui::HAIRLINE, 1.f);
    ImGui::SetCursorScreenPos(ar.Min);
    ImGui::PushID("desk_add");
    ImGui::PushItemFlag(ImGuiItemFlags_NoNavDefaultFocus, true);
    if (ui::button("Add games", ar.GetSize(), A.lib.empty(), true, ui::Icon::Plus)) { shot_trace("desk-add-games", ""); add_games_open(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy your own game files into RetroHub, or add a folder");
    ImGui::PopItemFlag();
    if (A.lib.empty() && A.scanned_once && !A.scanning && A.nav_seed && !ImGui::GetIO().WantTextInput) { A.nav_seed = false; focus_item_here(false); }   // nothing to list: the pad starts here (while loading the seed waits for the list)
    ImGui::PopID();
}

/* ---------- the pane ---------- */

/* A plain message in the pane (no library yet, nothing selected) with the Add games button. */
void pane_message(const ImRect& r, const char* title, const char* body) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rad = sp(2.f);
    dl->AddRectFilled(r.Min, r.Max, ui::PANEL, rad);
    dl->AddRect(r.Min, r.Max, ui::HAIRLINE, rad, 0, 1.f);
    const float cx = (r.Min.x + r.Max.x) * 0.5f, w = std::min(r.GetWidth() - sp(6.f), sp(56.f));
    const float ir = sp(4.f);
    float y = r.Min.y + std::max(sp(4.f), r.GetHeight() * 0.22f);
    dl->AddCircleFilled(ImVec2(cx, y + ir), ir + sp(1.25f), ui::with_alpha(ui::ACCENT, 0.10f), 48);
    dl->AddCircleFilled(ImVec2(cx, y + ir), ir, ui::with_alpha(ui::ACCENT, 0.22f), 48);
    ui::icon(dl, ui::Icon::Gamepad, ImVec2(cx, y + ir), ir * 1.2f, ui::mix(ui::ACCENT, ui::TEXT, 0.5f));
    y += ir * 2.f + sp(3.f);
    centered_text(dl, ui::F.title, cx, y, ui::TEXT, title);
    y += ui::F.title->FontSize + sp(1.f);
    for (const std::string& ln : ui::wrap_lines(ui::F.body, ui::F.body->FontSize, body, w, 4)) {
        centered_text(dl, ui::F.body, cx, y, ui::TEXT2, ln.c_str());
        y += ui::F.body->FontSize * 1.3f;
    }
    const float bw = sp(22.f);
    ImGui::SetCursorScreenPos(ImVec2(cx - bw * 0.5f, y + sp(2.f)));
    if (ui::button("Add games", ImVec2(bw, sp(5.5f)), true, true, ui::Icon::Plus)) { shot_trace("desk-add-games", "pane"); add_games_open(); }
}

/* The description shown in the pane: the cached / fetched one (gameinfo), else the catalog's blurb. Refreshed when a lookup lands. */
struct PaneInfo { std::string key, desc, facts; uint64_t gen = ~0ull; };
const PaneInfo& pane_info(const hub::GameEntry& e) {
    static PaneInfo P;
    const uint64_t gen = gameinfo_generation();
    if (P.key == e.key && P.gen == gen) return P;
    P = PaneInfo();
    P.key = e.key;
    P.gen = gen;
    const GameInfo* cat = catalog_for(e);
    CaseInfo ci;
    gameinfo_case_info(e, cat ? cat->title : std::string(), true, &ci);   // never blocks; queues the usual background lookup (none headless)
    P.desc = !ci.desc.empty() ? ci.desc : (cat ? cat->blurb : std::string());
    std::vector<std::string> f;
    if (!ci.genre.empty()) f.push_back(ci.genre);
    if (ci.year > 0) f.push_back(std::to_string(ci.year));
    else if (cat && cat->year) f.push_back(std::to_string(cat->year));
    if (!ci.maker.empty()) f.push_back(ci.maker);
    for (size_t i = 0; i < f.size(); ++i) P.facts += (i ? "  \xC2\xB7  " : "") + f[i];
    return P;
}

const hub::GameEntry* shown_game() {
    if (A.lib.empty()) return nullptr;
    const hub::GameEntry* e = find_entry(A.sel_game);
    if (!e || !hubdesk::has_game(D.model, D.games, e->key)) return nullptr;   // a search that hides it: nothing selected
    return e;
}

void pane_host(const ImRect& r) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGui::SetCursorScreenPos(r.Min);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##desk_pane", r.GetSize(), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    D.pane_child = ImGui::GetCurrentWindow()->ChildId;
    if (D.focus_pane) { D.focus_pane = false; focus_first_item_here(); shot_trace("desk-focus", "pane"); }
    if (const hub::GameEntry* e = shown_game()) desk_draw_game_pane(*e, r);
    else if (A.lib.empty()) pane_message(r, A.scanning ? "Looking for your games" : "No games yet",
                                         A.scanning ? "RetroHub is looking through your folders. Your games show up in the list as they are found."
                                                    : "RetroHub doesn't include any games. Add your own game files and they show up in the list.");
    else pane_message(r, "No game selected", "Pick a game from the list.");
    D.pane_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
    ImGui::EndChild();
    /* Left with nothing further left inside the pane: back to the list, on the same row (instead of the sidebar) */
    if (D.pane_focused && g.NavMoveSubmitted && g.NavMoveDir == ImGuiDir_Left &&
        !(g.NavMoveFlags & (ImGuiNavMoveFlags_IsTabbing | ImGuiNavMoveFlags_FocusApi | ImGuiNavMoveFlags_IsPageMove | ImGuiNavMoveFlags_ScrollToEdgeY)) &&
        g.NavMoveResultLocal.ID == 0 && g.NavMoveResultLocalVisible.ID == 0 && g.NavMoveResultOther.ID == 0) {
        ImGui::NavMoveRequestCancel();
        D.focus_list = D.focus_list_visible = true;
        ui_sound::play(ui_sound::Sfx::Move);
    }
}

}  // namespace

/* ======================= public ======================= */

bool desk_layout_on() { return hubdesk::is_desk(A.st.ui_layout); }

void desk_set_layout(bool desk) {
    const std::string v = desk ? hubdesk::kDesk : hubdesk::kTraditional;
    if (A.st.ui_layout == v) return;
    A.st.ui_layout = v;
    mark_dirty();
    D.reveal = true;
    D.dirty = true;
    shot_trace("desk-layout", v);
}

void desk_pre_frame() {
    if (!desk_layout_on()) return;
    if (A.view != LibView::Home) A.view = LibView::Home;   // All Games (and its Cases layout) belongs to Traditional: the desk is one view
    if (A.page != Page::Library) A.lib_pg = false;         // the list pages itself only while it is showing
}

void desk_page() {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    A.page_scrollable = false;
    rebuild();
    follow_outside_selection();
    /* B / Esc inside the pane: ImGui leaves the pane window (the focus lands on the pane itself, as an item of the page). Back to the list.
     * The same item reached by a move from outside (Right from the search box): into the pane instead. */
    if (g.NavId != 0 && g.NavId == D.pane_child) {
        if (D.pane_focused) {
            D.focus_list = D.focus_list_visible = true;
            ui_sound::play(ui_sound::Sfx::Back);
            lock_key(ImGuiKey_Escape);
            lock_key(ImGuiKey_GamepadFaceRight);
        } else D.focus_pane = true;
    }
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const ImVec2 avail(ImGui::GetContentRegionAvail().x, std::max(1.f, ImGui::GetContentRegionAvail().y - sp(1.f)));
    const float gap = sp(2.f);
    const float col_w = hubdesk::column_width(avail.x, sp(1.f));
    const ImRect col(o, ImVec2(o.x + col_w, o.y + avail.y));
    const ImRect pane(ImVec2(col.Max.x + gap, o.y), ImVec2(o.x + avail.x, o.y + avail.y));
    draw_column(col);
    pane_host(pane);
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR2, false)) lock_key(ImGuiKey_GamepadR2);   // RT pages the list here: it must not also change the sort order
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(avail);
}

bool desk_footer_hints(std::vector<Hint>* out) {
    if (!desk_layout_on() || A.page != Page::Library) return false;
    if (A.lib.empty() || D.model.rows.empty()) { *out = {{"A", "Enter", "Select"}}; return true; }   // nothing listed (no games, or a search with no match)
    if (D.pane_focused) { *out = {{"A", "Enter", "Select"}, {"B", "Esc", "Back"}, {"Y", "I", "Options"}, {"X", "F", "Favorite"}}; return true; }
    const bool header = D.cursor >= 0 && D.cursor < (int)D.model.rows.size() && D.model.rows[(size_t)D.cursor].header();
    if (header && D.list_focused) {
        const bool open = D.model.groups[(size_t)D.model.rows[(size_t)D.cursor].group].open;
        *out = {{"A", "Enter", open ? "Close" : "Open"}, {"Y", "I", "Options"}, {"X", "F", "Favorite"}};
        return true;
    }
    *out = {{"A", "Enter", "Play"}, {"Y", "I", "Options"}, {"X", "F", "Favorite"}, {"R3", "S", "Add to shelf"}};
    return true;
}

void desk_shot_seed() {
    const char* v = g_headless ? std::getenv("HUB_SHOT_LAYOUT") : nullptr;
    if (!v) return;
    const std::string s = v;
    if (s == hubdesk::kDesk || s == hubdesk::kTraditional) A.st.ui_layout = s;
    else {
        std::fprintf(stderr, "HUB_SHOT_LAYOUT: unknown layout '%s' (desk or traditional); no screenshot taken\n", v);
        SH.fail = true;
    }
}

/* ---------- the selected game's pane (stage 1: a summary; the next stage puts the real game page here) ---------- */

void desk_draw_game_pane(const hub::GameEntry& e, const ImRect& r) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rad = sp(2.f);
    const hub::GameState& gs = gstate(e.key);
    ImU32 c0, c1;
    entry_colors(e, &c0, &c1);
    dl->AddRectFilled(r.Min, r.Max, ui::PANEL, rad);

    /* header band: a soft wash of the game's colours behind the cover, the title and the actions */
    const bool short_pane = r.GetHeight() < sp(56.f);
    const float pad = short_pane ? sp(2.f) : sp(2.5f);
    const float bh = short_pane ? sp(4.5f) : sp(5.f);
    const float cover_h = std::max(sp(13.f), std::min(sp(24.f), r.GetHeight() * 0.3f));
    const float cover_w = std::floor(cover_h * 0.75f);
    const float tx = r.Min.x + pad + cover_w + pad, tw = std::max(sp(10.f), r.Max.x - pad - tx);
    ImFont* tf = ui::F.title;
    const std::vector<std::string> lines = ui::wrap_lines(tf, tf->FontSize, e.title, tw, short_pane ? 1 : 2);
    const float text_h = (float)lines.size() * tf->FontSize * 1.1f + sp(0.5f) + ui::F.body->FontSize + sp(0.5f) + ui::F.cap->FontSize;
    const float band_h = std::max(cover_h, text_h + sp(1.5f) + bh) + pad * 2.f;   // tall enough for the text AND the buttons at any UI size
    const ImVec2 b0 = r.Min, b1(r.Max.x, r.Min.y + band_h);
    dl->AddRectFilled(b0, b1, mix(ui::PANEL, c0, 0.16f), rad, ImDrawFlags_RoundCornersTop);
    ui::hfade_round_rect(dl, b0, b1, mix(ui::PANEL, c1, 0.28f), rad, ImDrawFlags_RoundCornersTop, 0.f, 1.f);
    dl->AddLine(ImVec2(b0.x, b1.y), ImVec2(b1.x, b1.y), ui::HAIRLINE, 1.f);
    dl->AddRect(r.Min, r.Max, ui::HAIRLINE, rad, 0, 1.f);

    const ImVec2 a0(r.Min.x + pad, r.Min.y + pad), a1(a0.x + cover_w, a0.y + cover_h);
    const float arad = sp(1.25f);
    const ArtRef art = art_for_entry(e);
    ui::shadow(dl, a0, a1, arad, 1.f);
    if (art.tex) ui::draw_art(dl, art.tex, art.w, art.h, a0, a1, arad);
    else { TileSpec ts; ts.title = e.title; ts.c0 = c0; ts.c1 = c1; draw_no_art(dl, ts, a0, a1, arad, 1.f); }
    if (art.loading) ui::shimmer(dl, a0, a1, arad, ui::with_alpha(ui::PANEL, 0.f));
    dl->AddRect(a0, a1, ui::HAIRLINE, arad, 0, 1.f);

    float ty = a0.y;
    const float title_y0 = ty;
    for (const std::string& ln : lines) { dl->AddText(tf, tf->FontSize, ImVec2(tx, ty), ui::TEXT, ln.c_str()); ty += tf->FontSize * 1.1f; }
    const bool cut = !lines.empty() && lines.back().size() >= 3 && lines.back().compare(lines.back().size() - 3, 3, "...") == 0 &&
                     !(e.title.size() >= 3 && e.title.compare(e.title.size() - 3, 3, "...") == 0);   // wrap_lines cut the title short
    if (cut && ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(ImVec2(tx, title_y0), ImVec2(tx + tw, ty))) ImGui::SetTooltip("%s", e.title.c_str());
    ty += sp(0.5f);
    {   // "Nintendo 64  ·  1-4 players"
        const int pl = local_players_for(e);
        std::string meta = hub::console_name(e);
        meta += "  \xC2\xB7  ";
        meta += pl >= 2 ? "1-" + std::to_string(pl) + " players" : std::string("1 player");
        dl->AddText(ui::F.body, ui::F.body->FontSize, ImVec2(tx, ty), ui::TEXT2, ui::ellipsize(ui::F.body, ui::F.body->FontSize, meta, tw).c_str());
        ty += ui::F.body->FontSize + sp(0.5f);
    }
    {   // the game page's play-time line: "4.5 hours played  ·  Last played: Today" (this PC's numbers)
        const std::string pt = hubui::pt::header_line(gs.play_seconds, gs.last_played, (int64_t)unix_now(), local_tz(), "  \xC2\xB7  ");
        dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(tx, ty), ui::TEXT2, ui::ellipsize(ui::F.cap, ui::F.cap->FontSize, pt, tw).c_str());
    }

    /* actions along the bottom of the band: Play, Play Online (when the game has it), Options, favourite */
    const bool idle = launch_idle();
    const bool online = supports_online(e);
    const float gap = sp(1.25f);
    const char* play_label = has_resume(e) ? "Resume" : "Play";
    auto text_w = [&](const char* s, bool icon) { return ImGui::CalcTextSize(s).x + (icon ? sp(2.25f) + sp(1.f) : 0.f) + sp(5.f); };   // ui::button's own sizing
    const float play_w = std::max(sp(14.f), text_w(play_label, true));
    const float online_w = text_w("Play Online", true), opt_w = text_w("Options", true);
    const float full = play_w + (online ? gap + online_w : 0.f) + gap + opt_w + gap + bh;
    const bool icons = full > tw;   // a narrow pane: Play Online and Options become round icon buttons (their names in the tooltip)
    const float by = b1.y - pad - bh;
    ImGui::SetCursorScreenPos(ImVec2(tx, by));
    if (ui::button(play_label, ImVec2(play_w, bh), true, idle, ui::Icon::Play)) desk_play(e);
    if (!idle && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", hubui::launch_busy_reason(A.proc != nullptr));
    if (online) {
        ImGui::SameLine(0, gap);
        bool go = false;
        if (icons) { ImGui::BeginDisabled(!idle); go = ui::icon_button("desk_online", ui::Icon::Globe, bh, false, idle ? "Play Online" : nullptr); ImGui::EndDisabled(); }
        else go = ui::button("Play Online", ImVec2(online_w, bh), false, idle, ui::Icon::Globe);
        if (!idle && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", hubui::launch_busy_reason(A.proc != nullptr));
        if (go && idle) {   // the game page's Play Online: host a new room with this game picked
            A.pending_room_game = room_id(e);
            shot_trace("play-online", "pending_room_game=" + A.pending_room_game);
            if (!g_headless) enter_room(true, hubnet::new_room_code());
        }
    }
    ImGui::SameLine(0, gap);
    const bool opt = icons ? ui::icon_button("desk_options", ui::Icon::Dots, bh, false, "Options") : ui::button("Options", ImVec2(opt_w, bh), false, true, ui::Icon::Dots);
    if (!icons && ImGui::IsItemHovered()) ImGui::SetTooltip("Saves, settings and everything else for this game");
    if (opt) { A.sel_game = e.key; A.show_details = true; shot_trace("desk-options", e.key); }
    ImGui::SameLine(0, gap);
    if (ui::icon_button("desk_fav", gs.favorite ? ui::Icon::StarFilled : ui::Icon::Star, bh, gs.favorite, gs.favorite ? "Remove from favorites" : "Add to favorites", ui::WARN))
        toggle_favorite(e.key);

    /* below the band: what is cheap and already here. It scrolls on its own; the band stays. */
    const float body_top = b1.y + sp(0.5f);
    if (r.Max.y - body_top < sp(4.f)) return;
    ImGui::SetCursorScreenPos(ImVec2(r.Min.x, body_top));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, short_pane ? sp(1.25f) : sp(2.f)));
    ImGui::BeginChild("##desk_body", ImVec2(r.GetWidth(), r.Max.y - body_top - sp(0.5f)), ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, sp(0.5f)));
    const float w = ImGui::GetContentRegionAvail().x;
    const PaneInfo& info = pane_info(e);
    auto heading = [&](const char* s) {
        ImGui::PushFont(ui::F.bold);
        ImGui::TextUnformatted(s);
        ImGui::PopFont();
    };
    if (!info.desc.empty() || !info.facts.empty()) {
        heading("About this game");
        if (!info.facts.empty()) {
            ImGui::PushFont(ui::F.cap);
            ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
            ImGui::TextUnformatted(ui::ellipsize(ui::F.cap, ui::F.cap->FontSize, info.facts, w).c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }
        if (!info.desc.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
            for (const std::string& ln : ui::wrap_lines(ImGui::GetFont(), ImGui::GetFontSize(), info.desc, w, 8)) ImGui::TextUnformatted(ln.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0, sp(1.5f)));
    }
    qm_seed_gallery(e);   // HUB_SHOT_GALLERY screenshot aid (headless only)
    if (const int n = shots_count(e); n > 0) {
        const std::string h = "Screenshots (" + std::to_string(n) + ")";
        heading(h.c_str());
        shots_gallery(e);   // the game page popup's own strip
        ImGui::Dummy(ImVec2(0, sp(1.f)));
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
    for (const std::string& ln : ui::wrap_lines(ImGui::GetFont(), ImGui::GetFontSize(), "Saves, settings and everything else for this game are under Options.", w, 3))
        ImGui::TextUnformatted(ln.c_str());
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::EndChild();
}
