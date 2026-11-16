/* Library desk: the second library layout (Settings > General > Layout).
 *
 *   [ icon sidebar ][ list column              ]|[ (setup banner while it applies: pane_banner)  ]
 *                   [ search box               ]|[ game page (game_page.h, SheetHost::Pane)     ]
 *                   [ Sort: Title A-Z  [F][?][..]]|[ cover  title / facts / play time   Play More  ]
 *                   [ health chip / filters line ]|[ About  Saves  Screenshots  Settings           ]
 *                   [ Recent             5  -  ]|[ the tab, scrolling                           ]
 *                   [   game rows              ]|[ (Settings tab: the help strip of the focused row) ]
 *                   [ Add games  | Coverflow   ]|
 *                                               ^ the divider: drag it to change the list width (hub.json desk_list_w)
 *
 * The list is ONE focusable item: the cursor row, the groups and the scrolling are ours (desk_logic.h), so the pad / keyboard moves are
 * read from ImGui's own move requests (same repeat rate as everywhere) and turned into row moves. Up on the first row and Down on the
 * last one let ImGui move the focus out (toolbar / Add games); Left on a closed group lets it reach the sidebar. Right on a game
 * enters the pane (Play gets the focus), Left with nothing further left in the pane, or B, comes back to the same row. Letters and digits
 * typed while the list has the focus jump to a title (type to jump); F / I / S stay the library shortcuts.
 * The rows follow the library's own Sort order and filters: rebuild_vis (A.vis, the list Traditional's All Games shows) gives every game
 * its rank and says which ones the filters keep, so Sort, Favorites only, Genre, Online games and Friends' games work the same in both
 * layouts. Friends' games you do not own are greyed rows in their console group; A opens the same friend-game view as Traditional.
 * Selection follows the cursor (A.sel_game), so every library shortcut keeps working: Y / I and right-click move the focus into the
 * pane, X / F favourites, R3 / S adds to a shelf. Enter / A / double-click plays through play_entry_anim, exactly like the game page's
 * Play. The Coverflow button shows Traditional's Cases carousel (All Games, Cases layout) full-window; B there comes back here with the
 * carousel's game selected. Colours are theme tokens only. Nothing here animates on its own once the list has stopped scrolling. */
#include "desk_view.h"
#include "desk_logic.h"
#include "game_page.h"
#include "console_names.h"
#include "convenience.h"
#include "ui_sound.h"
#include "libview_ui.h"
#include "padspots_logic.h"
#include "homefun.h"
#include "wave3_hooks.h"
#include "hidden_ui.h"
#include "help_logic.h"
#include "ui_scroll.h"
#include "ui_controls.h"

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
    std::vector<hubdesk::Game> games;   // built from A.lib (same order), then A.ghosts
    hubdesk::Model model;
    hubdesk::OpenMap open;
    std::string open_src = "\x01";      // A.st.desk_groups the map was parsed from
    std::set<std::string> search_closed;   // groups closed during the current search (session only)
    int closed_ver = 0, built_closed_ver = -1;
    std::string query = "\x01";         // A.search_buf the model was built for
    uint32_t vis_gen = ~0u;
    size_t lib_n = ~(size_t)0, ghost_n = ~(size_t)0;
    bool dirty = true;
    int builds = 0;
    /* cursor / selection */
    int cursor = -1;                    // the row the list cursor is on
    hubdesk::RowRef cursor_ref;
    int sel_row = -1;                   // the row that shows the selection fill (the selected game's row)
    std::string sel_group;              // the group the selection was picked in: a game can be in Recent, Favorites and its console group
    std::string ghost_sel;              // the cursor is on a friend's game ("ghost:<id>"): the pane shows its card, A.sel_game stays
    std::string seen_sel = "\x01";      // A.sel_game as last seen: an outside change moves the cursor to it
    hubdesk::TypeAhead ta;              // type to jump
    /* remembered across page switches (this state) and restarts (hub.json desk_sel / desk_scroll) */
    bool restore_done = false, skip_restore = false, user_moved = false;
    float restore_rows = -1.f;          // scroll to this many rows once the rows are laid out
    double persist_at = -1.0;           // the cursor or the scroll changed at this time: saved once they have been still a moment
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
    bool focus_list = false, focus_list_visible = false, focus_pane = false, focus_pane_visible = true, focus_sort = false;
    bool list_focused = false, pane_focused = false, sort_focused = false;
    bool list_tip = false;              // the list is hovered long enough for a tooltip
    ImGuiID list_id = 0, pane_child = 0;
    bool health_shown = false;          // the library health chip had something to say last frame (its row is reserved)
    /* the divider between the list and the pane */
    ImRect divider;
    float drag_dx = 0.f;
    /* Coverflow (Traditional's Cases carousel, full-window) opened from here */
    bool cf_open = false;
    std::string cf_prev;                // the Traditional All Games layout to put back
    /* "You can switch back in Settings" (once, after the Home card's Try it) */
    bool note_on = false;
    bool folders_not_now = false;       // the "Add your game folders" banner's Not now (this run only)
    /* the game page in the pane */
    gamepage::View page;
    std::string want_page;              // desk_show_game_page: select this game and enter the pane (next frame, after the model is built)
    std::string pane_shown;             // the game whose page the pane drew this frame
    int pane_shown_frame = -10;
} D;

/* ---------- small helpers ---------- */

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
    g.NavInitRequest = false;   // a "first item" request made earlier this frame (frame.cpp focuses the page window on arrival) must not take it back:
    g.NavInitResult.ID = 0;     // with friends' games listed at once the list exists on the very first frame, and the top bar would win
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

bool filters_on() { return A.st.lib_favs_only || !A.st.lib_genre.empty() || A.online_only; }

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

/* Play: the game page's Play button, exactly (play_entry_anim checks for a running game, the lock, missing files, the cloud...).
 * Headless runs never start anything: spawn() refuses there and traces it. */
void desk_play(const hub::GameEntry& e) {
    shot_trace("desk-play", e.key);
    ui_sound::play(ui_sound::Sfx::Select);
    hub::GameEntry c = e;
    play_entry_anim(c);
}

const hubdesk::Game* row_game(int row) {
    if (row < 0 || row >= (int)D.model.rows.size()) return nullptr;
    const hubdesk::Row& r = D.model.rows[(size_t)row];
    return r.header() ? nullptr : &D.games[(size_t)r.game];
}
const hub::GameEntry* row_entry(int row) {
    const hubdesk::Game* g = row_game(row);
    return g && !g->ghost ? find_entry(g->key) : nullptr;
}
const hubui::fg::Ghost* ghost_of_key(const std::string& key) {
    return key.compare(0, 6, "ghost:") == 0 ? find_ghost(key.substr(6)) : nullptr;
}

/* A friend's game: the same view Traditional opens from its greyed tile (friend_games_ui.cpp draw_ghost_modal). */
void open_ghost(const std::string& key) {
    if (!ghost_of_key(key)) return;
    A.sel_ghost = key.substr(6);
    A.show_ghost = true;
    ui_sound::play(ui_sound::Sfx::Select);
    shot_trace("desk-ghost", key);
}

/* ---------- model ---------- */

/* The key the selection fill marks: the friend's game under the cursor, else the selected game. */
const std::string& sel_key() { return D.ghost_sel.empty() ? A.sel_game : D.ghost_sel; }

void sync_sel_row() { D.sel_row = hubdesk::sel_row_for(D.model, D.games, sel_key(), D.sel_group); }

void select_row(int row) {
    const hubdesk::Game* g = row_game(row);
    if (!g) return;   // a header: the last game stays selected
    D.sel_group = D.model.groups[(size_t)D.model.rows[(size_t)row].group].id;
    if (g->ghost) {   // a friend's game: the pane shows its card; your own selection stays for the shortcuts that need one
        D.ghost_sel = g->key;
        A.sel_ghost = g->key.substr(6);
        return;
    }
    D.ghost_sel.clear();
    A.sel_game = g->key;
    A.sel_ghost.clear();
    D.seen_sel = A.sel_game;
}

void trace_cursor() {
    if (D.cursor < 0 || D.cursor >= (int)D.model.rows.size()) return;
    const hubdesk::Row& r = D.model.rows[(size_t)D.cursor];
    shot_trace("desk-cursor", "row=" + std::to_string(D.cursor) + "/" + std::to_string(D.model.rows.size()) + " group=" + D.model.groups[(size_t)r.group].id +
               " key=" + (r.header() ? std::string("-") : D.games[(size_t)r.game].key) + " sel=" + A.sel_game + " selrow=" + std::to_string(D.sel_row));
}

void set_cursor(int row, bool sound, bool user = true) {
    if (row < 0 || row >= (int)D.model.rows.size()) return;
    const bool moved = row != D.cursor;
    D.cursor = row;
    D.cursor_ref = hubdesk::ref_of(D.model, D.games, row);
    select_row(row);   // selection follows the cursor onto games; on a header the last game stays selected
    sync_sel_row();
    D.reveal = true;
    if (user) D.user_moved = true;
    if (moved && sound) ui_sound::play(ui_sound::Sfx::Move);
    if (moved) trace_cursor();
}

void rebuild() {
    const std::string q = A.search_buf;
    if (A.st.desk_groups != D.open_src) { D.open = hubdesk::parse_open(A.st.desk_groups); D.open_src = A.st.desk_groups; D.dirty = true; }
    const bool query_changed = q != D.query;
    if (query_changed) { D.search_closed.clear(); ++D.closed_ver; }
    if (!D.dirty && !query_changed && D.vis_gen == A.vis_gen && D.lib_n == A.lib.size() && D.ghost_n == A.ghosts.size() && D.built_closed_ver == D.closed_ver) return;
    D.dirty = false;
    D.query = q;
    D.vis_gen = A.vis_gen;
    D.lib_n = A.lib.size();
    D.ghost_n = A.ghosts.size();
    D.built_closed_ver = D.closed_ver;
    ++D.builds;
    /* The library's Sort order and filters: A.vis is the list rebuild_vis made for Traditional's All Games (same search text, the console
     * chips cleared by desk_pre_frame). Its position is each game's rank; a game it leaves out is not listed. An empty A.vis with nothing
     * filtering and no search has not been built yet: everything is listed then. */
    const size_t nl = A.lib.size(), ng = A.ghosts.size();
    std::vector<int> rank(nl + ng, -1);
    for (size_t p = 0; p < A.vis.size(); ++p) {
        const int i = A.vis[p];
        if (i >= 0 && (size_t)i < rank.size()) rank[(size_t)i] = (int)p;
    }
    const bool vis_ok = !A.vis.empty() || filters_on() || !hubdesk::trim_query(q).empty();
    D.games.clear();
    D.games.reserve(nl + ng);
    for (size_t i = 0; i < nl; ++i) {   // A.lib never holds hidden games (apply_scan takes them out)
        const hub::GameEntry& e = A.lib[i];
        const hub::GameState& gs = gstate(e.key);
        hubdesk::Game g;
        g.key = e.key;
        g.title = e.title;
        g.console = hub::console_id(e);
        if (custom_is_renamed(e.key)) g.alt_title = custom_original_title(e);   // the search finds a renamed game by its scanned name too
        g.last_played = gs.last_played;
        g.favorite = gs.favorite;
        g.pinned = is_native(e);
        g.rank = rank[i];
        g.listed = !vis_ok || rank[i] >= 0;
        D.games.push_back(std::move(g));
    }
    for (size_t k = 0; k < ng; ++k) {   // friends' games you do not own (empty while the Friends' games filter is off)
        const hubui::fg::Ghost& gh = A.ghosts[k];
        hubdesk::Game g;
        g.key = "ghost:" + gh.id;
        g.title = gh.title;
        g.console = gh.sys;
        g.ghost = true;
        g.rank = rank[nl + k];
        g.listed = !vis_ok || rank[nl + k] >= 0;
        D.games.push_back(std::move(g));
    }
    D.model = hubdesk::build(D.games, q, D.open, D.search_closed);
    if (!D.ghost_sel.empty() && !hubdesk::has_game(D.model, D.games, D.ghost_sel)) D.ghost_sel.clear();   // that friend's game left the list
    const int n = (int)D.model.rows.size();
    const int old_cursor = D.cursor;
    int c = hubdesk::find_row(D.model, D.games, D.cursor_ref);
    if (c < 0) c = D.sel_group.empty() ? hubdesk::row_of_key(D.model, D.games, A.sel_game)   // the first time: its first row (Recent sits at the top)
                                       : hubdesk::sel_row_for(D.model, D.games, A.sel_game, D.sel_group);
    if (c < 0 && n > 0) c = std::max(0, std::min(n - 1, D.cursor));
    if ((query_changed || D.model.searching || filters_on()) && !hubdesk::has_game(D.model, D.games, sel_key())) {   // a search or a filter hides the selected game: the first match takes over
        const int f = hubdesk::first_game_row(D.model);
        if (f >= 0) { c = f; select_row(f); }
    }
    D.cursor = n > 0 ? c : -1;
    D.cursor_ref = hubdesk::ref_of(D.model, D.games, D.cursor);
    if (D.sel_group.empty() && !D.cursor_ref.key.empty() && D.cursor_ref.key == sel_key()) D.sel_group = D.cursor_ref.group;
    /* D.seen_sel is left alone: a selection made elsewhere meanwhile (Coverflow, the palette) still moves the cursor (follow_outside_selection) */
    sync_sel_row();
    if (D.cursor != old_cursor || D.builds == 1) D.reveal = true;   // a rebuild that leaves the cursor where it was keeps the scroll too
    shot_trace("desk-model", "games=" + std::to_string(D.model.total) + " matches=" + std::to_string(D.model.matches) + " ghosts=" + std::to_string(D.model.ghosts) +
               " groups=" + std::to_string(D.model.groups.size()) + " rows=" + std::to_string(n) + " cursor=" + std::to_string(D.cursor) +
               " sort=" + A.st.library_sort + (A.st.library_sort_rev ? "+rev" : "") + (filters_on() ? " filtered" : ""));
}

/* Something else chose a game (the palette, the start animation, Coverflow, the full-window game page...): the cursor follows when the game is in
 * view, in the group the cursor is in when that group shows it (else the game's console group). */
void follow_outside_selection() {
    if (A.sel_game == D.seen_sel) return;
    D.seen_sel = A.sel_game;
    const hub::GameEntry* cur = row_entry(D.cursor);
    if (!cur || cur->key != A.sel_game) {
        const int r = hubdesk::sel_row_for(D.model, D.games, A.sel_game, D.cursor_ref.group);
        if (r >= 0) {   // in view: the cursor goes there (and off a friend's game it was on)
            D.cursor = r;
            D.cursor_ref = hubdesk::ref_of(D.model, D.games, r);
            D.sel_group = D.cursor_ref.group;
            D.ghost_sel.clear();
            D.reveal = true;
            trace_cursor();
        }
    }
    sync_sel_row();
}

/* hub.json desk_sel / desk_scroll, once at start-up: the row the cursor was on and the scroll, unless something else already chose a
 * game (HUB_SHOT_DETAILS, a ROM hack just added ...) or the player moved first. Waits for a scan that may still bring the game. */
void restore_once() {
    if (D.restore_done) return;
    if (D.skip_restore || D.user_moved) { D.restore_done = true; return; }
    const hubdesk::RowRef ref = hubdesk::parse_ref(A.st.desk_sel);
    if (ref.empty()) { D.restore_done = true; return; }
    if (A.lib.empty() && !A.scanned_once) return;
    if (!ref.key.empty() && !hubdesk::has_game(D.model, D.games, ref.key) && (A.scanning || !A.scanned_once)) return;
    D.restore_done = true;
    const int r = hubdesk::find_row(D.model, D.games, ref);
    if (r < 0) return;
    set_cursor(r, false, false);
    D.restore_rows = A.st.desk_scroll;
    shot_trace("desk-restore", A.st.desk_sel + " rows=" + std::to_string(A.st.desk_scroll));
}

/* The cursor and the scroll go to hub.json once they have been still for a moment (not on every D-pad step). */
void persist_tick() {
    if (!D.restore_done || D.model.rows.empty() || D.mt.row_h <= 0.f) return;
    const std::string sel = hubdesk::format_ref(D.cursor_ref);
    const float rows = std::floor(D.scroll / D.mt.row_h * 10.f + 0.5f) / 10.f;
    if (sel == A.st.desk_sel && std::fabs(rows - A.st.desk_scroll) < 0.05f) { D.persist_at = -1.0; return; }
    const double now = ImGui::GetTime();
    if (D.persist_at < 0.0) { D.persist_at = now; return; }
    if (now - D.persist_at < 1.2 || std::fabs(D.target - D.scroll) > 0.5f) return;
    A.st.desk_sel = sel;
    A.st.desk_scroll = rows;
    mark_dirty();
    D.persist_at = -1.0;
    shot_trace("desk-persist", sel + " rows=" + std::to_string(rows));
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

/* More > Open all groups / Close all groups. The cursor stays on its game when that stays visible, else on its group's header. */
void set_all_groups(bool open) {
    if (D.model.groups.empty()) return;
    if (D.model.searching) {
        if (open) D.search_closed.clear();
        else for (const hubdesk::Group& g : D.model.groups) D.search_closed.insert(g.id);
        ++D.closed_ver;
    } else {
        D.open = hubdesk::with_all(D.open, D.model, open);
        A.st.desk_groups = hubdesk::format_open(D.open);
        D.open_src = A.st.desk_groups;
        D.dirty = true;
        mark_dirty();
    }
    rebuild();
    D.reveal = true;
    ui_sound::play(ui_sound::Sfx::Select);
    shot_trace("desk-groups", open ? "all open" : "all closed");
}

/* One step of the list cursor; true = let ImGui's own move go on (the focus leaves the list). */
bool apply_step(const hubdesk::Step& s) {
    switch (s.act) {
    case Act::None: return false;
    case Act::Move: set_cursor(s.row, true); return false;
    case Act::Open: set_group_open(s.group, true); return false;
    case Act::Close: set_group_open(s.group, false); return false;
    case Act::EnterPane: D.focus_pane = true; ui_sound::play(ui_sound::Sfx::Move); return false;
    case Act::Play:
        if (const hubdesk::Game* g = row_game(s.row)) {
            if (g->ghost) open_ghost(g->key);
            else if (const hub::GameEntry* e = find_entry(g->key)) desk_play(*e);
        }
        return false;
    case Act::Edge: case Act::Leave: return true;
    }
    return false;
}

/* Type to jump: a letter or digit typed while the list has the focus (not the search box) moves the cursor to the next title that starts
 * with what was typed. */
void type_jump(char c) {
    if (hubdesk::type_key((unsigned char)c) != hubdesk::TypeKey::Jump) return;
    hubdesk::type_ahead(&D.ta, c, ImGui::GetTime());
    const int row = hubdesk::type_jump_row(D.model, D.games, D.cursor, &D.ta);
    shot_trace("desk-jump", D.ta.text + " row=" + std::to_string(row));
    if (row >= 0) set_cursor(row, true);
}

/* Would a character typed this frame jump in the list (so it must not start a search)? The list has the focus and only letters / digits
 * are waiting (any other printable character starts a search, as before). */
bool typing_jumps() {
    ImGuiIO& io = ImGui::GetIO();
    if (D.list_id == 0 || ImGui::GetCurrentContext()->NavId != D.list_id || io.KeyCtrl || io.KeyAlt) return false;
    bool jump = false;
    for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
        const hubdesk::TypeKey k = hubdesk::type_key((unsigned)io.InputQueueCharacters[i]);
        if (k == hubdesk::TypeKey::Search) return false;
        if (k == hubdesk::TypeKey::Jump) jump = true;
    }
    return jump;
}

/* ---------- small glyphs (theme colours) ---------- */

void glyph_sort(ImDrawList* dl, ImVec2 c, float u, ImU32 col) {   // three bars, longest on top
    for (int i = 0; i < 3; ++i)
        dl->AddRectFilled(ImVec2(c.x - u * (3.0f - i), c.y + (i - 1) * u * 1.5f - u * 0.3f), ImVec2(c.x + u * (3.0f - i), c.y + (i - 1) * u * 1.5f + u * 0.3f), col, u * 0.3f);
}
void glyph_filter(ImDrawList* dl, ImVec2 c, float u, ImU32 col) {   // a funnel
    const ImVec2 p[] = {ImVec2(c.x - u * 3.f, c.y - u * 2.2f), ImVec2(c.x + u * 3.f, c.y - u * 2.2f), ImVec2(c.x + u * 0.8f, c.y + u * 0.3f),
                        ImVec2(c.x + u * 0.8f, c.y + u * 2.4f), ImVec2(c.x - u * 0.8f, c.y + u * 1.6f), ImVec2(c.x - u * 0.8f, c.y + u * 0.3f)};
    dl->AddPolyline(p, 6, col, ImDrawFlags_Closed, std::max(1.f, u * 0.55f));
}
void glyph_cases(ImDrawList* dl, ImVec2 c, float u, ImU32 col) {   // a tall case between two turned ones, on a floor line (the Cases switcher's mark)
    dl->AddRectFilled(ImVec2(c.x - u * 0.62f, c.y - u * 1.35f), ImVec2(c.x + u * 0.62f, c.y + u * 0.95f), col, 1.5f);
    dl->AddQuadFilled(ImVec2(c.x - u * 1.5f, c.y - u * 0.9f), ImVec2(c.x - u * 0.95f, c.y - u * 0.62f), ImVec2(c.x - u * 0.95f, c.y + u * 0.62f), ImVec2(c.x - u * 1.5f, c.y + u * 0.9f), col);
    dl->AddQuadFilled(ImVec2(c.x + u * 0.95f, c.y - u * 0.62f), ImVec2(c.x + u * 1.5f, c.y - u * 0.9f), ImVec2(c.x + u * 1.5f, c.y + u * 0.9f), ImVec2(c.x + u * 0.95f, c.y + u * 0.62f), col);
    dl->AddRectFilled(ImVec2(c.x - u * 1.5f, c.y + u * 1.25f), ImVec2(c.x + u * 1.5f, c.y + u * 1.55f), col, 1.f);
}
void glyph_people(ImDrawList* dl, ImVec2 c, float u, ImU32 col) {   // two people (friends)
    dl->AddCircleFilled(ImVec2(c.x - u * 1.5f, c.y - u * 1.1f), u * 1.2f, col, 12);
    dl->AddCircleFilled(ImVec2(c.x + u * 1.6f, c.y - u * 0.7f), u * 1.0f, col, 12);
    dl->AddRectFilled(ImVec2(c.x - u * 3.1f, c.y + u * 0.5f), ImVec2(c.x + u * 0.1f, c.y + u * 2.2f), col, u * 0.8f);
    dl->AddRectFilled(ImVec2(c.x + u * 0.5f, c.y + u * 0.7f), ImVec2(c.x + u * 3.2f, c.y + u * 2.2f), col, u * 0.7f);
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
    /* the count: a small muted pill, right-aligned before the open / closed mark */
    const std::string count = std::to_string(grp.items.size());
    const float cw = cf->CalcTextSizeA(fs, FLT_MAX, 0.f, count.c_str()).x;
    const float pw = std::max(cw + sp(1.5f), fs + sp(0.5f)), ph = fs + sp(0.4f);
    const ImVec2 q1(mc.x - sp(1.75f), mc.y + ph * 0.5f), q0(q1.x - pw, mc.y - ph * 0.5f);
    dl->AddRectFilled(q0, q1, ui::veil(hov ? 0.10f : 0.06f), ph * 0.5f);
    dl->AddText(cf, fs, ImVec2(q0.x + (pw - cw) * 0.5f, mc.y - fs * 0.5f), ui::TEXT2, count.c_str());
    const float x = rr.Min.x + sp(1.f);
    const float room = std::max(sp(3.f), q0.x - sp(0.75f) - x);
    const std::string full = group_title(grp);
    const std::string title = ui::ellipsize(bf, fs, full, room);
    dl->AddText(bf, fs, ImVec2(x, y), hov ? ui::TEXT : ui::TEXT2, title.c_str());
    const float s = sp(0.6f), th = std::max(1.f, sp(0.2f));   // open: minus, closed: plus
    const ImU32 mcol = hov ? ui::TEXT : ui::TEXT2;
    dl->AddLine(ImVec2(mc.x - s, mc.y), ImVec2(mc.x + s, mc.y), mcol, th);
    if (!grp.open) dl->AddLine(ImVec2(mc.x, mc.y - s), ImVec2(mc.x, mc.y + s), mcol, th);
    if (ring) ui::focus_ring(dl, rr.Min, rr.Max, rad);
    if (hov && D.list_tip) {
        const std::string tip = full + "  (" + count + (grp.items.size() == 1 ? " game)" : " games)");
        ImGui::SetTooltip("%s", tip.c_str());
    }
}

void draw_game_row(ImDrawList* dl, const ImRect& rr, int row, const hubdesk::Group& grp, const hubdesk::Game& gm, bool hov, bool ring) {
    const hub::GameEntry* e = gm.ghost ? nullptr : find_entry(gm.key);
    const hubui::fg::Ghost* gh = gm.ghost ? ghost_of_key(gm.key) : nullptr;
    const float rad = sp(0.75f);
    if (row == D.sel_row) {
        dl->AddRectFilled(rr.Min, rr.Max, ui::PANEL2, rad);
        dl->AddRectFilled(ImVec2(rr.Min.x, rr.Min.y + sp(0.75f)), ImVec2(rr.Min.x + sp(0.375f), rr.Max.y - sp(0.75f)), ui::ACCENT, sp(0.2f));
    } else if (hov) dl->AddRectFilled(rr.Min, rr.Max, ui::veil(0.04f), rad);
    const char* why = e ? not_ready_reason(*e) : "";
    const bool ready = !*why && !gm.ghost;
    const float th = rr.GetHeight() - sp(1.25f);
    const ImVec2 a0(rr.Min.x + sp(1.f), rr.Min.y + (rr.GetHeight() - th) * 0.5f), a1(a0.x + th, a0.y + th);
    const float trad = sp(0.5f);
    ImU32 c0 = ui::PANEL3, c1 = ui::PANEL2;
    if (e) entry_colors(*e, &c0, &c1);
    else if (gh) console_colors(gh->sys, &c0, &c1);
    const ArtRef art = e ? art_for_entry(*e) : gh ? art_for_ghost(*gh) : ArtRef();   // only rows on screen ask for art
    if (art.tex) ui::draw_art(dl, art.tex, art.w, art.h, a0, a1, trad, ready ? 1.f : 0.45f);
    else gradient_round_rect(dl, a0, a1, ready ? c0 : mix(ui::PANEL2, c0, 0.3f), ready ? c1 : mix(ui::PANEL2, c1, 0.3f), trad);
    if (art.loading) ui::shimmer(dl, a0, a1, trad, ui::with_alpha(ui::PANEL, 0.f));
    dl->AddRect(a0, a1, ui::HAIRLINE, trad, 0, 1.f);
    ImFont* bf = ui::F.body;
    ImFont* cf = ui::F.cap;
    const float cy = (rr.Min.y + rr.Max.y) * 0.5f;
    float rx = rr.Max.x - sp(1.f);
    if (gm.ghost) {   // a friend's game you do not own: two people, muted
        glyph_people(dl, ImVec2(rx - sp(1.f), cy), sp(0.32f), ui::TEXT3);
        rx -= sp(2.75f);
    } else if (gm.favorite) {   // favourite: a small star
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
        if (gm.ghost) ImGui::SetTooltip("%s\n%s", gm.title.c_str(), gh && gh->uids.empty() ? "In a shared game pack. You don't have it yet." : "A friend has this game. You don't have it yet.");
        else if (!ready) ImGui::SetTooltip("%s\n%s", gm.title.c_str(), why);
        else if (t != gm.title) ImGui::SetTooltip("%s", gm.title.c_str());
    }
}

void search_box(const ImRect& r, bool toolbar) {
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
    if (type_to_search_pending() && !typing_jumps()) A.focus_search = true;            // just start typing (letters in the list jump instead)
    ImGui::SetCursorScreenPos(r.Min);
    if (A.focus_search) { ImGui::SetKeyboardFocusHere(); A.focus_search = false; }
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(sp(4.5f), (bh - ImGui::GetFontSize()) * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, bh * 0.5f);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNavDefaultFocus, true);
    ImGui::SetNextItemWidth(r.GetWidth());
    osk::input("##desk_search", "Search games", A.search_buf, sizeof A.search_buf);
    const bool active = ImGui::IsItemActive();
    {   // Down from the (not typing) search box: the toolbar under it (or the list, on its cursor row); the pane beside it is not "below"
        ImGuiContext& g = *ImGui::GetCurrentContext();
        if (ImGui::IsItemFocused() && !active && g.NavMoveSubmitted && g.NavMoveDir == ImGuiDir_Down && (toolbar || !D.model.rows.empty()) &&
            !(g.NavMoveFlags & (ImGuiNavMoveFlags_IsTabbing | ImGuiNavMoveFlags_FocusApi | ImGuiNavMoveFlags_IsPageMove | ImGuiNavMoveFlags_ScrollToEdgeY))) {
            ImGui::NavMoveRequestCancel();
            if (toolbar) D.focus_sort = true;
            else D.focus_list = D.focus_list_visible = true;
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

/* ---------- the toolbar: Sort, Filters, Surprise me, More ---------- */

/* The Sort chip: A / click cycles Title / Last played / Most played / ... (Traditional's Sort chip, the same saved setting); X, right-click,
 * Shift+F10 or the Menu key reverse it. The order applies inside each group (Recent stays newest first). */
void sort_chip(const ImRect& r) {
    ImGuiIO& io = ImGui::GetIO();
    const hubui::SortSpec spec(hubui::sort_from_key(A.st.library_sort), A.st.library_sort_rev, A.st.library_favs_first);
    const bool on = spec.mode != hubui::SortMode::Title || spec.reverse || spec.favs_first;
    ImGui::SetCursorScreenPos(r.Min);
    const bool pressed = ImGui::InvisibleButton("##desk_sort", r.GetSize(), ImGuiButtonFlags_EnableNav | ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    if (D.focus_sort) { D.focus_sort = false; focus_item_here(true); }
    const bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && io.NavVisible;
    D.sort_focused = ImGui::IsItemFocused();
    const bool right = pressed && ImGui::IsMouseReleased(ImGuiMouseButton_Right);
    ui::item_sound(foc, pressed && !right);
    const bool rev = hubui::padspots::sort_reverse_press({ImGui::IsItemFocused(), io.NavVisible, io.WantTextInput, ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false),
                                                         ImGui::IsKeyPressed(ImGuiKey_Menu, false), io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F10, false)});
    if (rev) lock_key(ImGuiKey_GamepadFaceLeft);   // this X reverses: the library's X (Favorite) must not also fire
    if (pressed && !right) cycle_sort(1);
    else if (right || rev) { A.st.library_sort_rev = !A.st.library_sort_rev; sort_changed(); }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = r.GetHeight(), rad = h * 0.5f;
    dl->AddRectFilled(r.Min, r.Max, on ? ui::with_alpha(ui::ACCENT, hov ? 0.30f : 0.20f) : (hov ? ui::FRAME_HOV : ui::PANEL2), rad);
    dl->AddRect(r.Min, r.Max, on ? ui::with_alpha(ui::ACCENT, 0.55f) : ui::HAIRLINE, rad, 0, 1.f);
    /* the direction arrow at the right: down = newest / most / Z first (Traditional's Sort chip) */
    const bool down = hubui::padspots::sort_arrow_down(spec);
    const float s = sp(0.5f), ax = r.Max.x - h * 0.5f, ay = (r.Min.y + r.Max.y) * 0.5f, d = down ? 1.f : -1.f;
    dl->AddTriangleFilled(ImVec2(ax - s, ay - d * s * 0.6f), ImVec2(ax + s, ay - d * s * 0.6f), ImVec2(ax, ay + d * s * 0.8f), ui::TEXT2);
    const std::string full = hubui::sort_label(spec);
    ImFont* f = ui::F.body;
    const float fs = ImGui::GetFontSize();
    const float text_end = ax - s - sp(0.75f), full_w = f->CalcTextSizeA(fs, FLT_MAX, 0.f, full.c_str()).x;
    const bool glyph = text_end - (r.Min.x + h * 1.05f) >= full_w;   // the bars only while the words fit beside them
    if (glyph) glyph_sort(dl, ImVec2(r.Min.x + h * 0.55f, ay), h * 0.11f, ui::TEXT2);
    const float tx = r.Min.x + (glyph ? h * 1.05f : h * 0.45f), room = std::max(sp(2.f), text_end - tx);
    const std::string shown = ui::ellipsize(f, fs, full, room);
    dl->AddText(f, fs, ImVec2(tx, (r.Min.y + r.Max.y - fs) * 0.5f), hov || foc ? ui::TEXT : ui::TEXT2, shown.c_str());
    if (foc) ui::focus_ring(dl, r.Min, r.Max, rad);
    if (foc && g_headless) shot_trace_once("desk-sort-focus", full);
    if (hov) ImGui::SetTooltip("Sort: %s\nChange the order of your games\nRight-click, X on a controller or Shift+F10: reverse the order", full.c_str());
}

/* A round icon button with a glyph drawn by `glyph` (ui::icon_button has no funnel or cases icon). */
template <class Glyph>
bool tool_button(const char* id, ImVec2 at, float size, bool active, const char* tip, Glyph glyph) {
    ImGui::SetCursorScreenPos(at);
    const bool pressed = ui::icon_button(id, ui::Icon::None, size, active, nullptr);
    const bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
    glyph(ImGui::GetWindowDrawList(), (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) * 0.5f, active ? IM_COL32_WHITE : ((hov || foc) ? ui::TEXT : ui::TEXT2));
    if (tip && (hov || foc)) ImGui::SetTooltip("%s", tip);
    return pressed;
}

void set_genre(const std::string& g) {
    A.st.lib_genre = g;
    mark_dirty();
    A.vis_dirty = true;
    shot_trace("desk-filter", "genre=" + g);
}
void clear_filters() {
    A.st.lib_favs_only = false;
    A.st.lib_genre.clear();
    A.online_only = false;
    mark_dirty();
    A.vis_dirty = true;
    shot_trace("desk-filter", "cleared");
}

/* Filters: the same ones Traditional's All Games has (Favorites chip, Online chip, Friends' games chip, Genre picker), the same state. */
void filter_menu() {
    if (ImGui::MenuItem("Favorites only", nullptr, A.st.lib_favs_only)) {
        A.st.lib_favs_only = !A.st.lib_favs_only; mark_dirty(); A.vis_dirty = true;
        shot_trace("desk-filter", std::string("favs=") + (A.st.lib_favs_only ? "1" : "0"));
    }
    if (ImGui::MenuItem("Online games only", nullptr, A.online_only)) {   // this session only, like Traditional's Online chip
        A.online_only = !A.online_only; A.vis_dirty = true;
        shot_trace("desk-filter", std::string("online=") + (A.online_only ? "1" : "0"));
    }
    if (A.st.show_friend_games && ImGui::MenuItem("Friends' games", nullptr, A.friends_chip)) {   // this session only, like the chip
        A.friends_chip = !A.friends_chip; A.vis_dirty = true;
        shot_trace("desk-filter", std::string("friends=") + (A.friends_chip ? "1" : "0"));
    }
    ImGui::Separator();
    const std::string gl = A.st.lib_genre.empty() ? std::string("Genre: All") : "Genre: " + A.st.lib_genre;
    if (ImGui::BeginMenu(gl.c_str())) {
        if (ImGui::MenuItem("All genres", nullptr, A.st.lib_genre.empty())) set_genre("");
        char row[96];
        for (const auto& kv : A.genre_counts) {
            if (kv.first == "Other") continue;
            std::snprintf(row, sizeof row, "%s  (%d)", kv.first.c_str(), kv.second);
            if (ImGui::MenuItem(row, nullptr, A.st.lib_genre == kv.first)) set_genre(kv.first);
        }
        auto oi = A.genre_counts.find("Other");
        if (oi != A.genre_counts.end()) {
            std::snprintf(row, sizeof row, "Other  (%d)", oi->second);
            if (ImGui::MenuItem(row, nullptr, A.st.lib_genre == "Other")) set_genre("Other");
        }
        ImGui::EndMenu();
    }
    if (filters_on()) {
        ImGui::Separator();
        if (ImGui::MenuItem("Clear filters")) clear_filters();
    }
}

void more_menu() {
    if (ImGui::MenuItem(A.scanning ? "Scanning..." : "Rescan library", nullptr, false, !A.scanning)) { start_scan(true); shot_trace("desk-rescan", ""); }
    const bool all = hubdesk::all_open(D.model);
    if (ImGui::MenuItem(all ? "Close all groups" : "Open all groups", nullptr, false, !D.model.groups.empty())) set_all_groups(!all);
    {   // the same list as Settings > General > Hidden games (it says "Nothing is hidden." when the list is empty)
        const size_t nh = hidden_stash_count();
        const std::string hl = nh ? "Hidden games (" + std::to_string(nh) + ")" : std::string("Hidden games");
        if (ImGui::MenuItem(hl.c_str())) { hidden_games_open_shot(); shot_trace("desk-hidden", std::to_string(nh)); }
    }
}

void popup_below(const char* id, const ImRect& anchor, void (*body)()) {
    ImGui::SetNextWindowPos(ImVec2(anchor.Min.x, anchor.Max.y + sp(0.5f)), ImGuiCond_Appearing);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(1.25f), sp(1.f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(sp(1.f), sp(0.75f)));
    if (ImGui::BeginPopup(id)) {
        body();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

void toolbar(const ImRect& tr) {
    const float h = tr.GetHeight(), gap = sp(0.75f);
    float x = tr.Max.x - h;
    ImGui::PushItemFlag(ImGuiItemFlags_NoNavDefaultFocus, true);
    /* right to left: More, Surprise me, Filters; the Sort chip takes what is left */
    const ImRect more_r(ImVec2(x, tr.Min.y), ImVec2(x + h, tr.Max.y));
    const bool more_open = ImGui::IsPopupOpen("##desk_more");
    ImGui::SetCursorScreenPos(more_r.Min);
    if (ui::icon_button("desk_more", ui::Icon::Dots, h, more_open, nullptr)) ImGui::OpenPopup("##desk_more");
    if (ImGui::IsItemHovered() || (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)) ImGui::SetTooltip("More: rescan, open or close all groups, hidden games");
    popup_below("##desk_more", more_r, more_menu);
    if (!A.lib.empty()) {   // Surprise me (Traditional's dice): a random game of yours you have not played for a while
        x -= gap + h;
        ImGui::SetCursorScreenPos(ImVec2(x, tr.Min.y));
        surprise_ui_button(h);
    }
    x -= gap + h;
    const ImRect fr(ImVec2(x, tr.Min.y), ImVec2(x + h, tr.Max.y));
    const bool fil = filters_on();
    if (tool_button("desk_filter", fr.Min, h, fil, "Filters: favorites, online games, friends' games, genre",
                    [](ImDrawList* dl, ImVec2 c, ImU32 col) { glyph_filter(dl, c, sp(0.3f), col); }))
        ImGui::OpenPopup("##desk_filter");
    popup_below("##desk_filter", fr, filter_menu);
    sort_chip(ImRect(tr.Min, ImVec2(x - gap, tr.Max.y)));
    ImGui::PopItemFlag();
}

/* The line under the toolbar, one thing at a time: the active filters (with Clear), else the scan progress, else the library health chip
 * (Traditional's, with its popup) when it has something to say. */
void status_row(const ImRect& sr) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* cf = ui::F.cap;
    const float x = sr.Min.x;
    std::string text;
    bool clear = false;
    if (filters_on()) { text = hubdesk::filter_line(A.st.lib_favs_only, A.st.lib_genre, A.online_only); clear = true; }
    else if (A.scanning) text = hubhelp::scan_caption_base(A.scan_done, A.scan_total);
    if (text.empty()) {
        ImGui::SetCursorScreenPos(sr.Min);
        ImGui::PushID("desk_health");
        ImGui::PushItemFlag(ImGuiItemFlags_NoNavDefaultFocus, true);
        D.health_shown = verify_library_card(true);   // verify_ui.cpp: the chip and its popup
        ImGui::PopItemFlag();
        ImGui::PopID();
        return;
    }
    float right = sr.Max.x;
    if (clear) {   // Clear: back to every game
        const char* cl = "Clear";
        const float cw = cf->CalcTextSizeA(cf->FontSize, FLT_MAX, 0.f, cl).x + sp(2.f);
        const ImRect b(ImVec2(right - cw, sr.Min.y), ImVec2(right, sr.Max.y));
        ImGui::SetCursorScreenPos(b.Min);
        ImGui::PushItemFlag(ImGuiItemFlags_NoNavDefaultFocus, true);
        const bool pressed = ImGui::InvisibleButton("##desk_clear_filters", b.GetSize(), ImGuiButtonFlags_EnableNav);
        ImGui::PopItemFlag();
        const bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
        ui::item_sound(foc, pressed);
        dl->AddRectFilled(b.Min, b.Max, hov || foc ? ui::FRAME_HOV : ui::PANEL2, b.GetHeight() * 0.5f);
        dl->AddText(cf, cf->FontSize, ImVec2(b.Min.x + sp(1.f), (b.Min.y + b.Max.y - cf->FontSize) * 0.5f), ui::TEXT, cl);
        if (foc) ui::focus_ring(dl, b.Min, b.Max, b.GetHeight() * 0.5f);
        if (hov) ImGui::SetTooltip("Show every game again");
        if (pressed) clear_filters();
        right = b.Min.x - sp(0.75f);
    }
    const std::string shown = ui::ellipsize(cf, cf->FontSize, text, std::max(sp(2.f), right - x));
    dl->AddText(cf, cf->FontSize, ImVec2(x, (sr.Min.y + sr.Max.y - cf->FontSize) * 0.5f), ui::TEXT2, shown.c_str());
    if (shown != text && ImGui::IsMouseHoveringRect(ImVec2(x, sr.Min.y), ImVec2(right, sr.Max.y)) && ImGui::IsWindowHovered()) ImGui::SetTooltip("%s", text.c_str());
}

/* The list area when there is nothing to list: a plain line, and Clear search / Clear filters when they hide everything. */
void list_empty(const ImRect& vp) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cx = (vp.Min.x + vp.Max.x) * 0.5f, w = vp.GetWidth() - sp(3.f);
    float y = vp.Min.y + sp(4.f);
    std::string msg;
    const bool search = A.search_buf[0] != 0;
    if (A.lib.empty() && A.ghosts.empty()) msg = A.scanning ? "Looking for your games..." : "Your games will show up here.";
    else if (search) msg = std::string("No games match \"") + A.search_buf + "\".";
    else msg = "No games match the filters.";
    for (const std::string& ln : ui::wrap_lines(ui::F.body, ui::F.body->FontSize, msg, w, 3)) {
        centered_text(dl, ui::F.body, cx, y, ui::TEXT2, ln.c_str());
        y += ui::F.body->FontSize * 1.3f;
    }
    if (!(A.lib.empty() && A.ghosts.empty()) && (search || filters_on())) {
        /* the focus comes here when it was on the list (now gone), was asked for (Enter in the search box) or the page just opened */
        const bool want = (A.nav_seed || D.focus_list || (D.list_id != 0 && ImGui::GetCurrentContext()->NavId == D.list_id)) && !ImGui::GetIO().WantTextInput;
        const bool visible = D.focus_list_visible || D.list_focused;
        const float bw = std::min(w, sp(18.f)), bh = sp(4.5f);
        ImGui::SetCursorScreenPos(ImVec2(cx - bw * 0.5f, y + sp(1.5f)));
        bool pressed = false;
        if (search) { if ((pressed = ui::button("Clear search", ImVec2(bw, bh), false, true, ui::Icon::Close))) A.search_buf[0] = 0; }
        else if ((pressed = ui::button("Clear filters", ImVec2(bw, bh), false, true, ui::Icon::Close))) clear_filters();
        if (pressed) D.focus_list = D.focus_list_visible = true;   // the list again, focused
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
    if ((A.nav_seed && !io.WantTextInput) || D.focus_list) {   // arriving on the page, back from the pane or Coverflow, Enter in the search box
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
            if (ImGui::IsMouseClicked(1) && !head) { set_cursor(hover_row, false); D.focus_pane = true; D.focus_pane_visible = false; }   // right-click: into the game's page
            if (ImGui::IsMouseClicked(0)) {
                const bool twice = ImGui::IsMouseDoubleClicked(0);   // the second press of a double-click is a click too
                if (head) { if (!twice) set_group_open(rows[(size_t)hover_row].group, !D.model.groups[(size_t)rows[(size_t)hover_row].group].open); }
                else {
                    set_cursor(hover_row, false);
                    if (twice) apply_step(hubdesk::Step{Act::Play, hover_row, -1});   // play (a friend's game: its details)
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
        if (!io.KeyCtrl && !io.KeyAlt) {   // type to jump (F / I / S stay shortcuts: type_key leaves them out)
            for (int k = ImGuiKey_A; k <= ImGuiKey_Z; ++k) if (ImGui::IsKeyPressed((ImGuiKey)k, false)) type_jump((char)('a' + (k - ImGuiKey_A)));
            for (int k = ImGuiKey_0; k <= ImGuiKey_9; ++k) if (ImGui::IsKeyPressed((ImGuiKey)k, false)) type_jump((char)('0' + (k - ImGuiKey_0)));
            for (int k = ImGuiKey_Keypad0; k <= ImGuiKey_Keypad9; ++k) if (ImGui::IsKeyPressed((ImGuiKey)k, false)) type_jump((char)('0' + (k - ImGuiKey_Keypad0)));
        }
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
    if (D.restore_rows >= 0.f) {   // a restart: the scroll the player left the list at (the cursor row is then kept in view below)
        D.scroll = D.target = hubdesk::clamp_scroll(D.restore_rows * D.mt.row_h, total, view_h);
        D.restore_rows = -1.f;
    }

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
    if (sb_now) {   // rows cut by the edges fade into the column (no hard cut against the search area or the buttons below)
        const float fh = sp(1.5f), fx1 = vp.Max.x - sb_hit;
        if (D.scroll > 0.5f) ui::vfade_round_rect(dl, vp.Min, ImVec2(fx1, vp.Min.y + fh), ui::PANEL, 0.f, 0, 1.f, 0.f);
        if (D.scroll < hubdesk::max_scroll(total, view_h) - 0.5f) ui::vfade_round_rect(dl, ImVec2(vp.Min.x - sp(0.6f), vp.Max.y - fh), ImVec2(fx1, vp.Max.y), ui::PANEL, 0.f, 0, 0.f, 1.f);
    }
    dl->PopClipRect();
    if (sb_now) {
        const hubdesk::Thumb t = hubdesk::thumb(D.scroll, total, view_h, track.GetHeight(), sp(3.f));
        const float bw = (sb_hov || sb_act) ? sp(0.75f) : sp(0.5f), bx = track.Min.x + (track.GetWidth() - bw) * 0.5f;
        dl->AddRectFilled(ImVec2(bx, track.Min.y), ImVec2(bx + bw, track.Max.y), ui::HAIRLINE, bw * 0.5f);
        dl->AddRectFilled(ImVec2(bx, track.Min.y + t.y0), ImVec2(bx + bw, track.Min.y + t.y1), (sb_hov || sb_act) ? ui::TEXT2 : ui::TEXT3, bw * 0.5f);
    }
}

/* Coverflow: Traditional's Cases carousel (All Games, Cases layout), full-window, through its own entry: the saved All Games layout is
 * set to Cases while it shows and put back when it closes (B, its menu's Home tab, another page). desk_pre_frame notices the close. */
void open_coverflow() {
    if (A.lib.empty() && A.ghosts.empty()) return;
    D.cf_prev = A.st.lib_layout;
    D.cf_open = true;
    A.search_buf[0] = 0;   // the carousel shows the whole library
    D.ghost_sel.clear();
    A.vis_dirty = true;
    if (lib_layout() != hubui::LibLayout::Cases) lib_set_layout(hubui::LibLayout::Cases);
    A.view = LibView::All;
    ui_sound::play(ui_sound::Sfx::Select);
    shot_trace("desk-coverflow", "open " + A.sel_game);
}
void close_coverflow() {
    D.cf_open = false;
    if (lib_layout() == hubui::LibLayout::Cases && D.cf_prev != hubui::layout_key(hubui::LibLayout::Cases)) lib_set_layout(hubui::layout_from_key(D.cf_prev));
    D.focus_list = true;   // back on the list, on the carousel's game
    D.focus_list_visible = A.pad_mode;
    D.seen_sel = "\x01";
    shot_trace("desk-coverflow", "closed " + A.sel_game);
}

/* The buttons under the list: Add games and Coverflow (labelled while they fit, else Coverflow keeps its picture and a tooltip). */
void footer_buttons(const ImRect& ar) {
    const bool cf = !(A.lib.empty() && A.ghosts.empty());
    const float gap = sp(1.f);
    ImFont* f = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    const float need_add = f->CalcTextSizeA(fs, FLT_MAX, 0.f, "Add games").x + sp(2.25f) + sp(1.f) + sp(2.f);
    const float need_cf = f->CalcTextSizeA(fs, FLT_MAX, 0.f, "Coverflow").x + sp(3.f);
    const bool cf_label = cf && ar.GetWidth() - gap >= need_add + need_cf;
    const float cf_w = !cf ? 0.f : cf_label ? std::max(need_cf, (ar.GetWidth() - gap) * 0.5f) : ar.GetHeight() * 1.3f;
    const float add_w = ar.GetWidth() - (cf ? cf_w + gap : 0.f);
    ImGui::SetCursorScreenPos(ar.Min);
    ImGui::PushID("desk_add");
    ImGui::PushItemFlag(ImGuiItemFlags_NoNavDefaultFocus, true);
    if (ui::button("Add games", ImVec2(add_w, ar.GetHeight()), A.lib.empty(), true, ui::Icon::Plus)) { shot_trace("desk-add-games", ""); add_games_open(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy your own game files into RetroHub, or add a folder");
    if (A.lib.empty() && A.scanned_once && !A.scanning && A.nav_seed && !ImGui::GetIO().WantTextInput) { A.nav_seed = false; focus_item_here(false); }   // nothing to list: the pad starts here (while loading the seed waits for the list)
    if (cf) {
        ImGui::SetCursorScreenPos(ImVec2(ar.Max.x - cf_w, ar.Min.y));
        const bool pressed = ui::button(cf_label ? "Coverflow###desk_cf" : "###desk_cf", ImVec2(cf_w, ar.GetHeight()));
        const bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
        if (!cf_label) glyph_cases(ImGui::GetWindowDrawList(), (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) * 0.5f, ar.GetHeight() * 0.17f, hov || foc ? ui::TEXT : ui::TEXT2);
        if (hov || (foc && !cf_label)) ImGui::SetTooltip(cf_label ? "Browse your games as game cases on the whole screen" : "Coverflow: browse your games as game cases on the whole screen");
        if (pressed) open_coverflow();
    }
    ImGui::PopItemFlag();
    ImGui::PopID();
}

/* "You can switch back in Settings": once, after the Home card's Try it, until closed or the page changes. */
float switch_note_h(float w) {
    if (A.st.desk_switch_note) { A.st.desk_switch_note = false; mark_dirty(); D.note_on = true; shot_trace("desk-note", "shown"); }
    if (!D.note_on) return 0.f;
    const int lines = (int)ui::wrap_lines(ui::F.cap, ui::F.cap->FontSize, "You can switch back to Traditional in Settings.", w - sp(4.f), 2).size();
    return (float)lines * ui::F.cap->FontSize * 1.3f + sp(1.5f);
}
void switch_note(const ImRect& r) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(r.Min, r.Max, ui::with_alpha(ui::ACCENT, 0.12f), sp(1.f));
    float y = r.Min.y + sp(0.75f);
    for (const std::string& ln : ui::wrap_lines(ui::F.cap, ui::F.cap->FontSize, "You can switch back to Traditional in Settings.", r.GetWidth() - sp(4.f), 2)) {
        dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(r.Min.x + sp(1.f), y), ui::TEXT, ln.c_str());
        y += ui::F.cap->FontSize * 1.3f;
    }
    const float bs = sp(2.5f);
    ImGui::SetCursorScreenPos(ImVec2(r.Max.x - bs - sp(0.5f), r.Min.y + (r.GetHeight() - bs) * 0.5f));
    ImGui::PushItemFlag(ImGuiItemFlags_NoNavDefaultFocus, true);
    const bool pressed = ImGui::InvisibleButton("##desk_note_close", ImVec2(bs, bs), ImGuiButtonFlags_EnableNav);
    ImGui::PopItemFlag();
    const bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
    ui::icon(dl, ui::Icon::Close, (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) * 0.5f, sp(1.5f), hov || foc ? ui::TEXT : ui::TEXT2);
    if (foc) ui::focus_ring(dl, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), bs * 0.5f);
    if (hov) ImGui::SetTooltip("Close");
    if (pressed) { D.note_on = false; shot_trace("desk-note", "closed"); }
}

void draw_column(const ImRect& col) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rad = sp(2.f), pad = sp(1.25f);
    dl->AddRectFilled(col.Min, col.Max, ui::PANEL, rad);
    dl->AddRect(col.Min, col.Max, ui::HAIRLINE, rad, 0, 1.f);
    const bool has_games = !(A.lib.empty() && A.ghosts.empty());
    const bool tight = col.GetHeight() < sp(52.f);   // a short column (big UI size, small window): slimmer controls, so the list keeps a few rows
    const float sh = sp(tight ? 4.f : 4.5f), ah = sp(tight ? 4.f : 4.5f), tbh = ImGui::GetFontSize() + sp(tight ? 1.25f : 1.75f), g1 = sp(tight ? 0.5f : 1.f);
    const ImRect sr(ImVec2(col.Min.x + pad, col.Min.y + pad), ImVec2(col.Max.x - pad, col.Min.y + pad + sh));
    float y = sr.Max.y;
    ImRect tr, st;
    if (has_games) { tr = ImRect(ImVec2(sr.Min.x, y + g1), ImVec2(sr.Max.x, y + g1 + tbh)); y = tr.Max.y; }
    const bool status = has_games && (D.health_shown || A.scanning || filters_on());   // the health chip's answer is last frame's
    if (status) { st = ImRect(ImVec2(sr.Min.x, y + g1 * 0.75f), ImVec2(sr.Max.x, y + g1 * 0.75f + tbh)); y = st.Max.y; }
    const ImRect ar(ImVec2(col.Min.x + pad, col.Max.y - pad - ah), ImVec2(col.Max.x - pad, col.Max.y - pad));
    const float nh = switch_note_h(ar.GetWidth());
    const ImRect nr(ImVec2(ar.Min.x, ar.Min.y - sp(1.f) - nh), ImVec2(ar.Max.x, ar.Min.y - sp(1.f)));
    const float list_bottom = (nh > 0.f ? nr.Min.y : ar.Min.y) - (tight ? sp(0.9f) : sp(1.25f));
    const ImRect vp(ImVec2(col.Min.x + sp(0.75f), y + g1 * 0.75f), ImVec2(col.Max.x - sp(0.75f), std::max(y + g1 * 0.75f + 1.f, list_bottom)));
    search_box(sr, has_games);
    if (has_games) toolbar(tr);
    if (status) status_row(st);
    else D.health_shown = false;
    if (D.model.rows.empty()) list_empty(vp);
    else draw_list(vp);
    const float line_y = (nh > 0.f ? nr.Min.y : ar.Min.y) - sp(0.6f);
    dl->AddLine(ImVec2(col.Min.x + pad, line_y), ImVec2(col.Max.x - pad, line_y), ui::HAIRLINE, 1.f);
    if (nh > 0.f) switch_note(nr);
    footer_buttons(ar);
}

/* ---------- the pane ---------- */

/* A plain message in the pane (still scanning, nothing selected), with the Add games button while there are no games yet. */
void pane_message(const ImRect& r, const char* title, const char* body, bool add_games) {
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
    if (!add_games) return;
    const float bw = sp(22.f);
    ImGui::SetCursorScreenPos(ImVec2(cx - bw * 0.5f, y + sp(2.f)));
    if (ui::button("Add games", ImVec2(bw, sp(5.5f)), true, true, ui::Icon::Plus)) { shot_trace("desk-add-games", "pane"); add_games_open(); }
}

/* No games at all: Traditional's first-run card ("Let's find your games": Add ROM folder, Find my games, the folders found), with
 * Finish setup above it while it applies, in a scroll area so a short window reaches every button. */
void pane_empty(const ImRect& r) {
    ImGui::SetCursorScreenPos(r.Min);   // the cards are panels themselves: no frame around them (as on Traditional's Home)
    ui::scroll_begin("desk_empty", r.GetSize(), ui::ScrollFlags_OverlayBar, ImVec2(0.f, 0.f));
    ImGui::BeginChild("##desk_empty_cards", ImVec2(std::max(1.f, ImGui::GetContentRegionAvail().x - sp(1.5f)), 0.f), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    finish_setup_card();
    empty_library_state();
    ImGui::EndChild();
    ui::scroll_end();
}

/* A friend's game under the cursor: what it is, and Details (the same friend-game view as Traditional's greyed tile). */
void pane_ghost(const ImRect& r, const hubui::fg::Ghost& g) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rad = sp(2.f), pad = sp(3.f);
    dl->AddRectFilled(r.Min, r.Max, ui::PANEL, rad);
    dl->AddRect(r.Min, r.Max, ui::HAIRLINE, rad, 0, 1.f);
    const float ah = std::min(sp(22.f), std::max(sp(10.f), (r.GetHeight() - pad * 2.f) * 0.5f)), aw = ah * kTileAspect;
    const ImVec2 a0(r.Min.x + pad, r.Min.y + pad), a1(a0.x + aw, a0.y + ah);
    TileSpec ts = spec_for_ghost(g, "desk", false);
    const ArtRef art = art_for_ghost(g);
    if (art.tex) ui::draw_art(dl, art.tex, art.w, art.h, a0, a1, sp(1.75f), 0.8f);
    else draw_no_art(dl, ts, a0, a1, sp(1.75f), 0.8f);
    dl->AddRectFilled(a0, a1, (ui::WASH2 & 0x00FFFFFFu) | (110u << 24), sp(1.75f));   // greyed, as on the friend-game view
    dl->AddRect(a0, a1, ui::HAIRLINE, sp(1.75f), 0, 1.f);
    const float tx = a1.x + pad, tw = std::max(sp(10.f), r.Max.x - pad - tx);
    float y = a0.y;
    for (const std::string& ln : ui::wrap_lines(ui::F.title, ui::F.title->FontSize, g.title, tw, 2)) {
        dl->AddText(ui::F.title, ui::F.title->FontSize, ImVec2(tx, y), ui::TEXT, ln.c_str());
        y += ui::F.title->FontSize * 1.1f;
    }
    y += sp(0.5f);
    const char* cn = hub::full_name_id(g.sys);
    if (cn && *cn) { dl->AddText(ui::F.body, ui::F.body->FontSize, ImVec2(tx, y), ui::TEXT2, cn); y += ui::F.body->FontSize * 1.5f; }
    const char* line = g.uids.empty() ? "This game is in a shared game pack. You don't have it yet." : "A friend has this game. You don't have it yet.";
    for (const std::string& ln : ui::wrap_lines(ui::F.body, ui::F.body->FontSize, line, tw, 3)) {
        dl->AddText(ui::F.body, ui::F.body->FontSize, ImVec2(tx, y), ui::TEXT2, ln.c_str());
        y += ui::F.body->FontSize * 1.3f;
    }
    ImGui::SetCursorScreenPos(ImVec2(tx, y + sp(1.5f)));
    if (ui::button("Details", ImVec2(sp(18.f), sp(5.f)), true, true, ui::Icon::Dots)) open_ghost("ghost:" + g.id);
}

/* What Traditional's Home asks for with its setup cards, as ONE slim banner above the game page: "Add your game folders" while only the
 * bundled game is here, otherwise "Finish setup" with the steps still open. The title and one short line on the left (cut with "...", or
 * left out when there is too little room; the whole text is in its tooltip), the two buttons on the right. When even the title and the
 * buttons do not fit (a narrow pane at a big UI size) the buttons go on a line of their own under the text, sharing its width, or one
 * under the other. Not now hides it for this run (Finish setup: on Home too). Returns the height it takes, gap under it included. */
float pane_banner(const ImRect& r) {
    if (A.lib.empty()) return 0.f;   // the empty-library pane shows Traditional's cards itself
    bool own_empty = true;
    for (const hub::GameEntry& e : A.lib) if (e.key != kNativeKey) { own_empty = false; break; }
    const bool folders = own_empty && !A.scanning && !D.folders_not_now;
    const std::vector<std::string> steps = folders ? std::vector<std::string>() : finish_setup_steps(false);
    if (!folders && steps.empty()) return 0.f;
    const char* title = folders ? "Add your game folders" : "Finish setup";
    const std::string line = folders ? std::string("Only the bundled game is here.") : hubhelp::join_steps(steps);
    const char* b1 = folders ? "Add ROM folder" : "Continue setup";
    const ui::Icon i1 = folders ? ui::Icon::Folder : ui::Icon::ChevronR;
    const char* b2 = "Not now";
    shot_trace_once("desk-banner", folders ? "folders" : "setup");

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* tf = ui::F.bold;
    ImFont* bf = ui::F.body;
    const float padx = sp(1.5f), pady = sp(0.75f), gap = sp(1.f), lgap = sp(0.75f), bh = sp(4.f);
    float w1 = ui::button_width(b1, i1), w2 = ui::button_width(b2);
    const float btns = w1 + gap + w2;
    const float inner = std::max(1.f, r.GetWidth() - padx * 2.f);
    const float title_w = tf->CalcTextSizeA(tf->FontSize, FLT_MAX, 0.f, title).x, sep = sp(1.5f);
    const bool one = title_w + gap * 2.f + btns <= inner;                     // one line: the title, what fits of the line, the buttons
    int rows = 1;                                                             // otherwise the buttons go on a line under the text:
    if (!one && btns > inner) {
        const float c1 = w1 - sp(5.f), c2 = w2 - sp(5.f);                    // (the labels, without the buttons' side padding)
        if (c1 + c2 + gap + sp(5.f) <= inner) {                               // side by side with less padding, sharing the width
            const float side = (inner - gap - c1 - c2) * 0.5f;
            w1 = c1 + side;
            w2 = c2 + side;
        } else rows = 2;                                                      // or one under the other
    }
    const float text_h = std::max(tf->FontSize, bf->FontSize);
    const float h = one ? pady * 2.f + std::max(bh, text_h) : pady * 2.f + text_h + lgap + (float)rows * bh + (float)(rows - 1) * lgap;
    const ImRect br(r.Min, ImVec2(r.Max.x, r.Min.y + h));
    dl->AddRectFilled(br.Min, br.Max, ui::PANEL, sp(2.f));
    dl->AddRect(br.Min, br.Max, ui::HAIRLINE, sp(2.f), 0, 1.f);

    /* the text: the title in bold, the line after it (cut to the room left) */
    const float tx0 = br.Min.x + padx, tx1 = one ? br.Max.x - padx - (w1 + gap + w2) - gap * 2.f : br.Max.x - padx;
    const float ty = one ? br.Min.y + (h - text_h) * 0.5f : br.Min.y + pady;
    const std::string t = ui::ellipsize(tf, tf->FontSize, title, tx1 - tx0);
    dl->AddText(tf, tf->FontSize, ImVec2(tx0, ty + (text_h - tf->FontSize) * 0.5f), ui::TEXT, t.c_str());
    const float lx = tx0 + tf->CalcTextSizeA(tf->FontSize, FLT_MAX, 0.f, t.c_str()).x + sep;
    bool cut = t != title;
    if (lx < tx1 - sp(10.f)) {   // (less room than that: the title alone, the line in its tooltip)
        const std::string l = ui::ellipsize(bf, bf->FontSize, line, tx1 - lx);
        dl->AddText(bf, bf->FontSize, ImVec2(lx, ty + (text_h - bf->FontSize) * 0.5f), ui::TEXT2, l.c_str());
        cut = cut || l != line;
    } else cut = true;
    if (cut && ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(ImVec2(tx0, ty), ImVec2(tx1, ty + text_h)))
        ImGui::SetTooltip("%s: %s", title, line.c_str());

    /* the buttons: at the right end of the line, or on a line of their own under the text. Their focus ring and the primary button's glow
     * reach past the slim banner, above the pane's top edge too: they are not cut there (the clip rect is widened for them) */
    const float room = sp(3.f);
    ImGui::PushClipRect(ImVec2(br.Min.x - room, br.Min.y - room), ImVec2(br.Max.x + room, br.Max.y + room), false);
    const float by = one ? br.Min.y + (h - bh) * 0.5f : ty + text_h + lgap;
    ImGui::SetCursorScreenPos(ImVec2(one ? br.Max.x - padx - (w1 + gap + w2) : tx0, by));
    const bool p1 = ui::button(b1, ImVec2(rows == 2 ? std::min(w1, inner) : w1, bh), true, true, i1);
    if (rows == 1) ImGui::SameLine(0.f, gap);
    else ImGui::SetCursorScreenPos(ImVec2(tx0, by + bh + lgap));
    const bool p2 = ui::button(b2, ImVec2(rows == 2 ? std::min(w2, inner) : w2, bh));
    const bool p2_pad = p2 && ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
    ImGui::PopClipRect();
    if (folders) add_rom_folder(p1);   // every frame while shown (the pad folder browser)
    else if (p1) { shot_trace("desk-banner", "continue"); wizard_open(); }
    if (p2) {
        if (folders) D.folders_not_now = true;
        else finish_setup_dismiss();
        shot_trace("desk-banner", std::string("not-now ") + (folders ? "folders" : "setup"));
        if (p2_pad) { D.focus_pane = true; D.focus_pane_visible = true; }   // the banner is gone: the focus goes to Play, not nowhere
    }
    return h + sp(1.25f);
}

const hub::GameEntry* shown_game() {
    if (A.lib.empty() || !D.ghost_sel.empty()) return nullptr;
    const hub::GameEntry* e = find_entry(A.sel_game);
    if (!e || !hubdesk::has_game(D.model, D.games, e->key)) return nullptr;   // a search or a filter that hides it: nothing selected
    return e;
}

void pane_host(const ImRect& r) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGui::SetCursorScreenPos(r.Min);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##desk_pane", r.GetSize(), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    D.pane_child = ImGui::GetCurrentWindow()->ChildId;
    const hub::GameEntry* shown = shown_game();
    const hubui::fg::Ghost* ghost = D.ghost_sel.empty() ? nullptr : ghost_of_key(D.ghost_sel);
    ImRect page = r;
    page.Min.y += pane_banner(r);
    if (D.focus_pane) {   // entering the pane: the game page puts the focus on Play when it draws it
        D.focus_pane = false;
        if (shown) { D.page.focus_play = true; D.page.focus_visible = D.focus_pane_visible; }
        else focus_first_item_here();
        D.focus_pane_visible = true;
        shot_trace("desk-focus", "pane");
    }
    if (ghost) pane_ghost(page, *ghost);
    else if (shown) desk_draw_game_pane(*shown, page);
    else if (A.lib.empty()) {
        if (A.scanning) pane_message(page, "Looking for your games", "RetroHub is looking through your folders. Your games show up in the list as they are found.", true);
        else pane_empty(page);
    } else pane_message(page, "No game selected", "Pick a game from the list.", false);
    D.pane_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
    ImGui::EndChild();
    /* Left with nothing further left inside the pane: back to the list, on the same row (instead of the sidebar). Not inside a menu or
     * dialog opened from the pane (they count as "in the pane" for ImGui): there Left belongs to them. */
    if (D.pane_focused && !any_popup() && g.NavMoveSubmitted && g.NavMoveDir == ImGuiDir_Left &&
        !(g.NavMoveFlags & (ImGuiNavMoveFlags_IsTabbing | ImGuiNavMoveFlags_FocusApi | ImGuiNavMoveFlags_IsPageMove | ImGuiNavMoveFlags_ScrollToEdgeY)) &&
        g.NavMoveResultLocal.ID == 0 && g.NavMoveResultLocalVisible.ID == 0 && g.NavMoveResultOther.ID == 0) {
        ImGui::NavMoveRequestCancel();
        D.focus_list = D.focus_list_visible = true;
        ui_sound::play(ui_sound::Sfx::Move);
    }
}

/* The divider between the list and the pane: drag it (mouse) to change the list width, double-click for Normal again. Controller players
 * pick Narrow / Normal / Wide in Settings > General. Saved as hub.json desk_list_w (units, so it holds at every UI size). */
void divider(const ImRect& dr, float x0, float content_w) {
    ImGuiIO& io = ImGui::GetIO();
    D.divider = dr;
    ImGui::SetCursorScreenPos(dr.Min);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    ImGui::InvisibleButton("##desk_divider", dr.GetSize());
    ImGui::PopItemFlag();
    const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    if (hov || act) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActivated()) { D.drag_dx = io.MousePos.x - dr.Min.x; shot_trace("desk-drag", "start w=" + std::to_string(A.st.desk_list_w)); }
    if (act) {
        const int u = hubdesk::list_units_for(io.MousePos.x - D.drag_dx - x0, content_w, sp(1.f));
        if (u != A.st.desk_list_w) A.st.desk_list_w = u;
    }
    if (ImGui::IsItemDeactivated()) { mark_dirty(); shot_trace("desk-drag", "w=" + std::to_string(A.st.desk_list_w)); }
    if (hov && ImGui::IsMouseDoubleClicked(0)) { A.st.desk_list_w = 0; mark_dirty(); shot_trace("desk-drag", "normal"); }
    if (hov || act) {
        const float cx = (dr.Min.x + dr.Max.x) * 0.5f, w = std::max(1.f, sp(0.25f));
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(cx - w, dr.Min.y + sp(2.f)), ImVec2(cx + w, dr.Max.y - sp(2.f)), act ? ui::ACCENT : ui::with_alpha(ui::ACCENT, 0.5f), w);
    }
    if (hov && !act && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("Drag to change the width of the list. Double-click for the normal width.");
}

/* HUB_SHOT_DESK_DRAG=<dx> (headless only): the virtual mouse drags the divider dx window pixels, through the real drag code. */
void shot_drag() {
    static const char* v = g_headless ? std::getenv("HUB_SHOT_DESK_DRAG") : nullptr;
    static int stage = 0;
    static float dx = 0.f;
    static ImVec2 p0;
    if (!v || stage > 9 || !A.scanned_once || SH.el < 2.0 || D.divider.GetWidth() <= 0.f) return;
    ImGuiIO& io = ImGui::GetIO();
    if (stage == 0) { dx = (float)std::atof(v); p0 = D.divider.GetCenter(); io.AddMousePosEvent(p0.x, p0.y); }
    else if (stage == 1) io.AddMouseButtonEvent(0, true);
    else if (stage < 8) io.AddMousePosEvent(p0.x + dx * (float)(stage - 1) / 6.f, p0.y);
    else if (stage == 8) io.AddMouseButtonEvent(0, false);
    ++stage;
}

/* HUB_SHOT_DESK_TYPE=<letters> (headless only): types those characters into the hub as a keyboard does (the key, then its character),
 * one per 0.4 s from 2 s on (type to jump / search). */
void shot_type() {
    static const char* v = g_headless ? std::getenv("HUB_SHOT_DESK_TYPE") : nullptr;
    static size_t i = 0;
    static double at = 0.0;
    static ImGuiKey down = ImGuiKey_None;
    if (!v) return;
    ImGuiIO& io = ImGui::GetIO();
    if (down != ImGuiKey_None) { io.AddKeyEvent(down, false); down = ImGuiKey_None; }
    if (i >= std::strlen(v) || !A.scanned_once || SH.el < 2.0 || SH.el - at < 0.4) return;
    at = SH.el;
    const char c = v[i++];
    const char l = (char)std::tolower((unsigned char)c);
    if (l >= 'a' && l <= 'z') down = (ImGuiKey)(ImGuiKey_A + (l - 'a'));
    else if (c >= '0' && c <= '9') down = (ImGuiKey)(ImGuiKey_0 + (c - '0'));
    else if (c == '/') down = ImGuiKey_Slash;
    if (down != ImGuiKey_None) io.AddKeyEvent(down, true);
    io.AddInputCharacter((unsigned)c);
    shot_trace("desk-type", std::string(1, c));
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

int desk_list_width_choice() { return hubdesk::list_choice(A.st.desk_list_w); }
void desk_set_list_width_choice(int choice) {
    A.st.desk_list_w = hubdesk::list_units_of_choice(choice);
    mark_dirty();
    shot_trace("desk-width", std::to_string(A.st.desk_list_w));
}

void desk_pre_frame() {
    if (!desk_layout_on()) { D.cf_open = false; return; }
    if (!D.restore_done && (!A.env_select.empty() || A.show_details)) D.skip_restore = true;   // something already chose a game to show
    if (D.cf_open) {
        if (A.page == Page::Library && A.view == LibView::All && lib_layout() == hubui::LibLayout::Cases) return;   // Coverflow is showing
        close_coverflow();   // B, its menu's Home tab, another page or another layout closed it: back to the desk
    }
    if (A.view != LibView::Home) A.view = LibView::Home;   // All Games (and its Cases layout) belongs to Traditional: the desk is one view
    if (!A.sys_filter.empty()) { A.sys_filter.clear(); A.vis_dirty = true; }   // All Games' console chips too: the desk groups by console itself
    if (A.page != Page::Library) { A.lib_pg = false; D.note_on = false; }   // the list pages itself only while it is showing
}

/* A page request (desk_show_game_page): the game becomes the selection, in view in the list, and the focus goes into the pane. */
void apply_page_request() {
    if (D.want_page.empty()) return;
    const std::string key = D.want_page;
    D.want_page.clear();
    if (!find_entry(key)) return;
    if (!hubdesk::has_game(D.model, D.games, key) && (A.search_buf[0] || filters_on())) {   // a search or a filter hides it: clear them
        A.search_buf[0] = 0;
        if (filters_on()) { clear_filters(); rebuild_vis(); }
        rebuild();
    }
    if (hubdesk::row_of_key(D.model, D.games, key) < 0) {   // its group is closed: open one that holds it (its console's first)
        int gi = -1;
        const hub::GameEntry* e = find_entry(key);
        const std::string console = e ? hubdesk::console_group_id(hub::console_id(*e)) : std::string();
        for (size_t i = 0; i < D.model.groups.size(); ++i)
            for (int k : D.model.groups[i].items)
                if (D.games[(size_t)k].key == key && (gi < 0 || D.model.groups[i].id == console)) gi = (int)i;
        if (gi >= 0) set_group_open(gi, true);
    }
    A.sel_game = key;
    A.sel_ghost.clear();
    D.ghost_sel.clear();
    const int row = hubdesk::sel_row_for(D.model, D.games, key, D.cursor_ref.group);
    if (row >= 0) set_cursor(row, false);
    D.seen_sel = A.sel_game;
    sync_sel_row();
    if (!D.pane_focused) D.focus_pane = true;
    shot_trace("desk-page", key);
}

void desk_page() {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiIO& io = ImGui::GetIO();
    A.page_scrollable = false;
    shot_drag();
    shot_type();
    rebuild();
    restore_once();
    follow_outside_selection();
    apply_page_request();
    /* B / Esc inside the pane: ImGui leaves the pane window (the focus lands on the pane itself, as an item of the page). Back to the list.
     * Not while an option row's dropdown list is open: that B closes only the list, and the dropdown puts the focus back on its row when
     * it draws, later this frame.
     * The same item reached by a move from outside (Right from the search box): into the pane instead. */
    if (g.NavId != 0 && g.NavId == D.pane_child && !(D.pane_focused && ui::dropdown_list_open())) {
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
    const float col_w = hubdesk::list_width(A.st.desk_list_w, avail.x, sp(1.f));
    const ImRect col(o, ImVec2(o.x + col_w, o.y + avail.y));
    const ImRect pane(ImVec2(col.Max.x + gap, o.y), ImVec2(o.x + avail.x, o.y + avail.y));
    draw_column(col);
    pane_host(pane);
    divider(ImRect(ImVec2(col.Max.x, o.y + sp(1.f)), ImVec2(pane.Min.x, o.y + avail.y - sp(1.f))), o.x, avail.x);
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR2, false)) lock_key(ImGuiKey_GamepadR2);   // RT pages the list here: it must not also change the sort order
    /* A friend's game under the cursor: Y / I open its details (the friend-game view); X / F, R3 / S and Traditional's Y / I would act on
     * your own selected game, which the pane is not showing, so they wait. */
    if (!D.ghost_sel.empty() && (D.list_focused || D.pane_focused) && !io.WantTextInput && !any_popup()) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false) || (ImGui::IsKeyPressed(ImGuiKey_I, false) && !io.KeyCtrl)) open_ghost(D.ghost_sel);
        for (ImGuiKey k : {ImGuiKey_GamepadFaceUp, ImGuiKey_GamepadFaceLeft, ImGuiKey_GamepadR3, ImGuiKey_I, ImGuiKey_S}) lock_key(k);
        if (!io.KeyCtrl) lock_key(ImGuiKey_F);
    }
    persist_tick();
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(avail);
}

bool desk_footer_hints(std::vector<Hint>* out) {
    if (!desk_layout_on() || A.page != Page::Library || lib_cases_active()) return false;
    if ((A.lib.empty() && A.ghosts.empty())) { *out = {{"A", "Enter", "Select"}}; return true; }
    if (D.pane_focused) {   // the game page in the pane: its hints live here (the pane has no footer of its own)
        if (!D.ghost_sel.empty()) *out = {{"A", "Enter", "Select"}, {"B", "Esc", "Back"}};   // a friend's game: its card and Details
        else *out = {{"A", "Enter", "Select"}, {"B", "Esc", "Back"}, {"LB", "", "Tabs"}, {"LT", "", "Page up"}, {"RT", "", "Page down"}, {"X", "F", "Favorite"}};
        return true;
    }
    if (!D.list_focused || D.model.rows.empty()) {   // the search box, the toolbar, the buttons under the list (or nothing to list)
        *out = {{"A", "Enter", "Select"}};
        if (D.sort_focused) out->push_back({"X", "", "Reverse order"});
        return true;
    }
    const hubdesk::Row* r = D.cursor >= 0 && D.cursor < (int)D.model.rows.size() ? &D.model.rows[(size_t)D.cursor] : nullptr;
    if (r && r->header()) {
        const bool open = D.model.groups[(size_t)r->group].open;
        *out = {{"A", "Enter", open ? "Close" : "Open"}, {"Y", "I", "Details"}, {"X", "F", "Favorite"}};
    } else if (r && D.games[(size_t)r->game].ghost) {
        *out = {{"A", "Enter", "Details"}};
    } else {
        *out = {{"A", "Enter", "Play"}, {"Y", "I", "Details"}, {"X", "F", "Favorite"}, {"R3", "S", "Add to shelf"}};
    }
    if (A.search_buf[0]) out->push_back({"B", "Esc", "Clear search"});   // B / Esc on the list clear the search
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

/* ---------- Traditional Home: "Try the new Library desk layout" ---------- */

void desk_try_card() {
    if (desk_layout_on() || A.st.desk_card_done || !A.st.setup_done) return;   // answered, or a new install still in setup
    if (A.proc || game_active() || session_active() || wizard_state() != -1) return;   // never while a game or session runs
    const char* title = "Try the new Library desk layout";
    const char* body = "Your games in a list, with the selected game always open beside it.";
    bool try_it = false, not_now = false;
    shot_trace_once("desk-card", "shown");
    if (hubhelp::finish_card_compact(ImGui::GetIO().DisplaySize.y, ui::sp(1.f) / 8.f)) {   // a short window: one line, like Finish setup
        begin_panel("desk_try_card", ImVec2(0, 0));
        const float bw1 = sp(12.f), bw2 = sp(10.f), gp = sp(1.f);
        const float text_w = std::max(sp(10.f), ImGui::GetContentRegionAvail().x - bw1 - bw2 - gp * 2);
        ImGui::AlignTextToFramePadding();
        ImGui::PushFont(ui::F.bold);
        const std::string line = ui::ellipsize(ui::F.bold, ui::F.bold->FontSize, std::string(title) + ":  " + body, text_w);
        const std::string shown = line.size() >= 3 && line.compare(line.size() - 3, 3, "...") == 0 ? ui::ellipsize(ui::F.bold, ui::F.bold->FontSize, title, text_w) : line;   // no half sentence: the title alone
        ImGui::TextUnformatted(shown.c_str());
        ImGui::PopFont();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", body);
        ImGui::SameLine(0, 0);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, ImGui::GetContentRegionAvail().x - bw1 - bw2 - gp));
        try_it = button("Try it", ImVec2(bw1, 0), true, true);
        ImGui::SameLine(0, gp);
        not_now = button("Not now", ImVec2(bw2, 0));
        end_panel();
    } else {
        begin_panel("desk_try_card", ImVec2(0, 0));
        ImGui::PushFont(ui::F.bold);
        ImGui::TextUnformatted(title);
        ImGui::PopFont();
        text_wrapped_disabled(body);
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        try_it = button("Try it", ImVec2(0, 0), true, true);
        ImGui::SameLine();
        not_now = button("Not now", ImVec2(0, 0));
        end_panel();
    }
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    if (try_it) {
        A.st.desk_card_done = true;
        A.st.desk_switch_note = true;   // the desk says once where to switch back
        mark_dirty();
        desk_set_layout(true);
        D.focus_list = true;   // the desk's list takes the pad focus on its first frame (nav_seed would be spent by Home's hero below this card)
        D.focus_list_visible = A.pad_mode;
        shot_trace("desk-card", "try");
    } else if (not_now) {
        A.st.desk_card_done = true;
        mark_dirty();
        A.nav_seed = true;   // the card is gone: the pad focus goes back to Home's first item, not to the top bar
        shot_trace("desk-card", "not-now");
    }
}

/* ---------- the selected game's page in the pane ---------- */

void desk_draw_game_pane(const hub::GameEntry& e, const ImRect& r) {
    const gamepage::Drawn d = gamepage::draw_view(D.page, &e, gamepage::HostKind::Pane, r.Min, r.GetSize(), nullptr);
    if (d.gone) return;   // Hide game: the entry left the library; the list picks the next selection on its next frame
    if (d.shown) {
        D.pane_shown = D.page.shown;
        D.pane_shown_frame = ImGui::GetFrameCount();
        ImGui::GetWindowDrawList()->AddRect(r.Min, r.Max, ui::HAIRLINE, sp(2.f), 0, 1.f);   // the same edge as the list column
    }
}

bool desk_show_game_page(const std::string& key) {
    if (!desk_layout_on() || A.page != Page::Library || lib_cases_active() || !find_entry(key)) return false;   // Coverflow: the full-window page goes over it
    D.want_page = key;   // applied on the desk's next frame, once its list model is current (desk_page)
    A.sel_game = key;
    A.sel_ghost.clear();
    return true;
}

std::string desk_pane_shown() { return D.pane_shown_frame == ImGui::GetFrameCount() ? D.pane_shown : std::string(); }
