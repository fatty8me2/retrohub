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

/* The selected game's pane, drawn inside the pane window in the rectangle r (screen coordinates). Stage 1 draws a summary here (cover,
 * title, console and players, play time, Play / Play Online / Options, description, screenshots); the next stage replaces the body of
 * this function with the real game page. Focus: the pane's first focusable item gets the focus when the player enters the pane. */
void desk_draw_game_pane(const hub::GameEntry& e, const ImRect& r);
