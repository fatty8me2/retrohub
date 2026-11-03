#pragma once
/* All Games layouts: grid / list / compact grid / showcase, the A-Z jump strip, the layout switcher. libview_ui.cpp.
 * Pure parts (letters, column sort, virtual rows) are libview_logic.h. */
#include "libview_logic.h"
#include <string>

struct SDL_Renderer;
struct SDL_Texture;

hubui::LibLayout lib_layout();                        // the saved layout (Settings::lib_layout)
void lib_set_layout(hubui::LibLayout l);              // saves it, keeps the selected game in view
float lib_switcher_width();                           // the top bar's layout switcher (5 icon cells, + the Mixed toggle on Shelf), 0 when it is not shown
void lib_switcher();                                  // draws it at the cursor
/* The body of All Games under the chips: fills the rest of the page (grid child + jump strip). `bar_h` = selected-bar height below. */
void lib_body(float bar_h);
bool lib_layout_has_bar();                            // the selected-game bar below the body (not in Showcase)
void lib_sort_chip_changed();                         // the Sort chip / RT was used: the list follows it again
void lib_fake_art_init(SDL_Renderer* ren);     // HUB_SHOT_FAKE_ART=1 (headless): procedural covers for screenshots
bool lib_fake_art(const std::string& key, SDL_Texture** tex, int* w, int* h);
void lib_shot_seed();                                 // HUB_SHOT_LIBVIEW=grid|list|compact|showcase|shelf (+ HUB_SHOT_SHELF_MIXED=1)
