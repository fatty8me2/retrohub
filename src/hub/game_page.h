#pragma once
/* The game page as pieces any host can lay out (game_page.cpp). The pieces draw into whatever window or child the host has begun and
 * never open, close or size a window themselves; leaving the page goes through Host::close.
 *
 * Hosts: the ##details modal (page_library.cpp, draw_details_modal). A full-window page and the Library desk's right-hand pane come next.
 * A host owns one gamepage::State, and only one host draws the page in a frame. Per frame, in this order:
 *
 *     gamepage::Ctx c = gamepage::begin(state, entry, host, appearing);   // first: save info refresh, launch state, colours
 *     gamepage::draw_header(c);                                           // cover, title, facts, play time, description (More)
 *     gamepage::draw_notices(c);                                          // archive / keys warnings, Kanto how-to
 *     gamepage::draw_action_row(c, gamepage::primary_actions(c), h);      // Play / Resume (default focus), Play Online or why not, Close
 *     gamepage::draw_tab_pills(c);                                        // or lay out gamepage::tabs(c) yourself and call select_tab
 *     ... begin the body region ...
 *     gamepage::draw_tab_body(c);                                         // About / Saves / Screenshots / Settings
 *     ... end it ...
 *     if (gamepage::draw_action_row(c, gamepage::secondary_actions(c), h) == gamepage::Row::Gone) { the game was hidden: stop }
 *     custom_draw(1);                                                     // Customize / Add to shelf, opened by the secondary actions
 *
 * Spacing between the pieces belongs to the host. Every piece may be skipped or moved; tabs(c) / draw_tab_body(c) work out the tab once
 * per frame on first use. A host that does not draw the action buttons itself (a "More" menu) still calls trace_actions() every frame
 * and poll() for every action that has one, and uses shot_forced() for the HUB_SHOT_ACT presses. */

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
    bool ok = false;
    hubui::pt::GameStats st;
};

/* Everything the page keeps between frames. Owned by the host, one per container. */
struct State {
    std::string tab_key;        // the game `tab` belongs to: another game, or the host (re)opening the page, starts on About
    int tab = TabAbout;         // the requested tab; kept when the game lacks it, so a gallery seeded a frame later still lands on its tab
    double desc_hold = 0.0;     // ImGui time until which the description keeps the window scrolled to the top (set when the window appears)
    float footer_h = 0.f;       // height of the secondary actions measured last frame (the modal sizes its scrolling body with it)
    std::string act_done;       // HUB_SHOT_ACT: the game + action already pressed once
    int pt_tz = 0;              // local time minus UTC for the play-time strip, refreshed every 30 s (mktime is not free)
    double pt_tz_at = -1e9;
    PlayTimeCache pt;
};

/* What the host does for the page. */
struct Host {
    std::function<void()> close;   // leave the page; called just before the action that leads elsewhere (the modal: CloseCurrentPopup)
};

/* One frame of the page. Made by begin(); valid for this frame only. */
struct Ctx {
    State& st;
    const hub::GameEntry& e;
    Host host;
    bool appearing = false;            // the host has just opened the page (the modal: its window is appearing)
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

/* ---- header ---- */
void draw_header(Ctx& c);                                        // today's arrangement of the four pieces below
void draw_cover(Ctx& c, ImDrawList* dl, ImVec2 a0, ImVec2 a1);   // box art (or the no-art tile) with shadow and hairline
void draw_title_block(Ctx& c, float w);                          // title, facts line, play time; wraps at w
std::string facts_line(const Ctx& c);                            // "Nintendo 64 · 1-4 players · Online · 1999"
void draw_play_time(Ctx& c, float w);                            // "Not played yet" or the stats strip
void draw_description(Ctx& c);                                   // short description (More / Less) or the built-in blurb
void draw_notices(Ctx& c);                                       // archive / encryption / keys warnings, Kanto how-to

/* ---- tabs ---- */
const std::vector<TabInfo>& tabs(Ctx& c);   // the tabs this game has (About always; Saves, Screenshots (n), Settings when they have content)
int shown_tab(Ctx& c);                      // the tab drawn this frame
void select_tab(Ctx& c, int id);
void draw_tab_pills(Ctx& c);
void draw_tab_body(Ctx& c);                 // the shown tab
void draw_tab(Ctx& c, int id);              // one tab's content

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

enum class Row { Drawn, Gone };
/* Buttons of height h in rows that wrap to the width left in the current region. Runs a pressed action at once (before the next button);
 * returns Gone right after an ends_page action ran. */
Row draw_action_row(Ctx& c, const std::vector<Action>& actions, float h);
float action_width(const Action& a);                          // the width draw_action_row gives a button
void trace_actions(Ctx& c, const std::vector<Action>& actions);   // the QA trace lines, for hosts that draw the actions their own way
bool shot_forced(Ctx& c, const char* act);                    // HUB_SHOT_ACT asks for this action now (true once per game)

}  // namespace gamepage
