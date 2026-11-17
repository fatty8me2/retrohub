#pragma once
/* The game page: host-independent pieces (game_page.cpp) and ONE layout of them inside a ui::sheet (game_page_view.cpp).
 *
 * Hosts (both call gamepage::draw_view; it lays the page out the same way in each):
 *   - Traditional layout: the full-window page over the hub (page_library.cpp, draw_details_modal), a SheetHost::Overlay sheet named
 *     "details" (QA scripts match the window name). B / Esc close it, after an open dropdown or menu took its B.
 *   - Library desk: the selected game's page, always open in the right-hand pane (desk_view.cpp, desk_draw_game_pane), a
 *     SheetHost::Pane sheet with no Close button; the desk decides what B does.
 *
 * The layout (game_page_layout.h has the sizing rules):
 *   header (pinned)  cover, title, facts, one play-time line; Play (default focus), Play Online or why not, More (a menu with every
 *                    secondary action), Close (Overlay only); the notices; the tab bar (LB / RB, mouse, D-pad + A)
 *   body (scrolls)   the shown tab, at a readable width; About starts with the play-time stats strip and the description
 *   footer (pinned)  Overlay only: the controller / keyboard hints (the desk's hints are in the hub's own footer)
 *
 * The pieces draw into whatever window or child the caller has begun and never open, close or size a window themselves; leaving the
 * page goes through Host::close. A host owns one View (one State), and only one host draws the page in a frame. A host that draws the
 * actions its own way (the More menu does) calls trace_actions() every frame, poll() every frame for every action that has one, uses
 * shot_forced() for the HUB_SHOT_ACT presses, and stops drawing at once after an ends_page action ran. */

#include "playtime_logic.h"
#include "ui_kit.h"

#include <functional>
#include <string>
#include <vector>

namespace hub {
struct GameEntry;
struct GameState;
struct GameInfo;
}  // namespace hub

namespace gamepage {

enum Tab : int { TabAbout = 0, TabSaves = 1, TabShots = 2, TabSettings = 3 };

struct TabInfo {
    int id = TabAbout;
    std::string label;   // "Screenshots (6)" carries its count
};

/* The shown game's play-time numbers: recomputed when the game changes, when its totals change (a session just ended), when the page was
 * not drawn for a few frames, and otherwise every 30 s. Never reads files every frame. */
struct PlayTimeCache {
    std::string key;
    unsigned long long ps = 0, lp = 0;
    double at = -1e9;
    int frame = -100;
    unsigned gen = 0;    // pagecache::generation() when it was read
    bool ok = false;
    hubui::pt::GameStats st;
};

/* Everything the page pieces keep between frames. Owned by the host (inside its View). */
struct State {
    std::string tab_key;        // the game `tab` belongs to: another game, or the host (re)opening the page, starts on About
    int tab = TabAbout;         // the requested tab; kept when the game lacks it, so a gallery seeded a frame later still lands on its tab
    int pt_tz = 0;             // local time minus UTC for the play-time strip, refreshed every 30 s (mktime is not free)
    double pt_tz_at = -1e9;
    PlayTimeCache pt;
};

/* What the host does for the page. */
struct Host {
    std::function<void()> close;   // leave the page; called just before the action that leads elsewhere (the pane: nothing to do)
};

/* One frame of the page. Made by begin(); valid for this frame only. */
struct Ctx {
    State& st;
    const hub::GameEntry& e;
    Host host;
    bool appearing = false;            // the host has just opened the page (starts on About, or the HUB_SHOT_TAB tab)
    const hub::GameState& gs;
    const hub::GameInfo* cat = nullptr;   // catalog entry, may be null
    bool idle = false;                 // a launch is allowed now (no game, no session)
    bool native = false;               // the bundled native port
    ImU32 c0 = 0, c1 = 0;              // tile colours for the no-art cover
    /* tabs, worked out by the first piece that needs them this frame */
    bool tabs_ready = false;
    std::vector<TabInfo> tab_list;
    int tab_shown = TabAbout;
};

Ctx begin(State& st, const hub::GameEntry& e, const Host& host, bool appearing);

/* ---- header pieces ---- */
void draw_cover(Ctx& c, ImDrawList* dl, ImVec2 a0, ImVec2 a1);   // box art (or the no-art tile) with shadow and hairline
std::string facts_line(const Ctx& c);                            // "Nintendo 64 · 1-4 players · Online · 1999"
std::string play_time_line(Ctx& c);                              // "4.5 hours played · Last played: Today" or "Not played yet"
bool draw_play_time(Ctx& c, float w);                            // the full stats strip, at most w wide; false = never played (nothing drawn)
void draw_description(Ctx& c);                                   // short description (More / Less) or the built-in blurb
void draw_notices(Ctx& c);                                       // archive / encryption / keys warnings, Kanto how-to

/* ---- tabs ---- */
const std::vector<TabInfo>& tabs(Ctx& c);   // the tabs this game has (About always; Saves, Screenshots (n), Settings when they have content)
int shown_tab(Ctx& c);                      // the tab drawn this frame
void select_tab(Ctx& c, int id);
void draw_tab(Ctx& c, int id);              // one tab's content, laid out for the width of the current region (About: two columns when wide)

/* ---- actions ---- */
struct Action {
    const char* id = "";              // play, online, close, favorite, savefolder, smash, cheats, controls, desktop, customize, shelf, hack, hide
    std::string label;
    ui::Icon icon = ui::Icon::None;
    float width = 0.f;                // 0 = fit the label
    bool enabled = true;
    bool primary = false;             // drawn highlighted
    bool default_focus = false;       // takes the focus when the page opens (Play)
    bool ends_page = false;           // run() removes the game from the library: the host must stop drawing the page at once
    const char* act = nullptr;        // HUB_SHOT_ACT name that presses it once in screenshot mode
    std::string tooltip;              // shown on hover, also while disabled ("" = none)
    std::vector<std::string> traces;  // QA "act" trace lines written just before this action is drawn (trace_actions)
    std::function<void()> run;        // on press; calls Host::close itself when the action leaves the page
    std::function<void(bool pressed)> poll;   // instead of run: a picker that must see every frame's press state (call it every frame)
};

std::vector<Action> primary_actions(Ctx& c);     // Play / Resume, Play Online (or "Online soon" / "Offline only", disabled), Close
std::vector<Action> secondary_actions(Ctx& c);   // Favorite, Save folder / Find Smash 64 game, Cheats, Controls, Add to desktop, Customize,
                                                 // Add to shelf..., Add hack or translation, Hide game (only the ones this game offers)

void trace_actions(Ctx& c, const std::vector<Action>& actions);   // the QA trace lines, for hosts that draw the actions their own way
bool shot_forced(Ctx& c, const char* act);                    // HUB_SHOT_ACT asks for this action now (true once per game)

/* ---- the page layout (game_page_view.cpp) ---- */
enum class HostKind { Overlay, Pane };

/* One per host: the pieces' State plus what the layout keeps between frames. */
struct View {
    State st;
    std::string key;               // the game drawn last time ("" = none yet)
    bool fresh = true;             // the page was (re)opened: About (or HUB_SHOT_TAB), the top of the body, the focus on Play
    bool focus_play = false;       // put the pad focus on Play the next time it is drawn (the desk: entering the pane)
    bool focus_visible = true;     // ... with the focus ring showing (not after a mouse click)
    int focus_until = -1;          // Overlay: until this frame, Play takes the focus back if it is outside the page (first frames)
    bool popup_above = false;      // a popup sat above the page last frame: its B must not also close the page
    unsigned body_win[4] = {0, 0, 0, 0};   // each tab's body scroll window (ImGuiID), sent back to the top for another game
    bool to_top = false;
    std::string shown;             // the game whose page was drawn this frame ("" = none)
    double key_at = -1e9;          // when `key` last changed (the pane's heavy facts wait pagecache::kSettleSecs after it; the first game: at once)
    std::string settled_key;       // the last game the pane settled on (trace "gamepage-settled")
};

void view_reset(View& v);   // the next draw opens the page afresh (Overlay: every time it opens)

struct Drawn {
    bool shown = false;    // drawn this frame
    bool closed = false;   // Overlay: closed this frame (B / Esc, Close, an action that left the page)
    bool gone = false;     // an ends_page action ran (Hide game): the entry is no longer in the library
};

/* Lay the page of `e` out in the rect pos / size (screen pixels). Overlay: `*open` drives the sheet (call this every frame, also while
 * closed and with e == nullptr, so it can close cleanly). Pane: open may be nullptr. */
Drawn draw_view(View& v, const hub::GameEntry* e, HostKind host, ImVec2 pos, ImVec2 size, bool* open);

}  // namespace gamepage
