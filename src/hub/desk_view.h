#pragma once
/* Library desk: the second library layout (Settings > General > Layout). desk_view.cpp; the list model is desk_logic.h.
 * The games are a list in a left column (search box, groups that open and close, an Add games button) and the selected game's page is
 * always open in the pane on the right. The sidebar shows icons only. Traditional (the default) is untouched. */
#include "app.h"

bool desk_layout_on();                         // Settings::ui_layout is "desk"
void desk_set_layout(bool desk);               // Settings > General > Layout: applies at once and saves
void desk_page();                              // the Library page while the layout is Library desk (page_library.cpp)
void desk_pre_frame();                         // frame.cpp, first thing every frame: keeps the shared library state right for this layout
bool desk_footer_hints(std::vector<Hint>* out);   // frame.cpp footer: the Library page's hints in this layout (false = not in this layout)
void desk_shot_seed();                         // HUB_SHOT_LAYOUT=desk|traditional (headless only; an unknown value exits 2)

/* The selected game's page (game_page.h, the same layout as the full-window page, without Close), drawn inside the pane window in the
 * rectangle r (screen coordinates). Another game picked in the list updates it without taking the focus from the list; it keeps the tab
 * when the new game has it (otherwise About) and starts at the top. Entering the pane (Right, Y / I, right-click) puts the focus on Play. */
void desk_draw_game_pane(const hub::GameEntry& e, const ImRect& r);

/* Somebody asks for a game's page (Y / I, the palette, a friend's game, a notification ...). With the Library desk on and its Library
 * page showing: selects that game in the list (clears a search that hides it, opens its group) and moves the focus into the pane, then
 * returns true. Otherwise returns false and the caller opens the full-window page. */
bool desk_show_game_page(const std::string& key);
std::string desk_pane_shown();   // the game whose page the pane drew this frame ("" = none): the QA "captured ... details=yes" line
