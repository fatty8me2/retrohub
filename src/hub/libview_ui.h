#pragma once
/* All Games layouts: grid / list / compact grid / showcase, the A-Z jump strip, the layout switcher. libview_ui.cpp.
 * Pure parts (letters, column sort, virtual rows) are libview_logic.h. */
#include "libview_logic.h"
#include <string>

struct SDL_Renderer;
struct SDL_Texture;

hubui::LibLayout lib_layout();                        // the saved layout (Settings::lib_layout)
void lib_set_layout(hubui::LibLayout l);              // saves it, keeps the selected game in view
float lib_switcher_width();                           // the top bar's layout switcher (6 icon cells, + the Mixed toggle on Shelf), 0 when it is not shown
void lib_switcher();                                  // draws it at the cursor
/* The body of All Games under the chips: fills the rest of the page (grid child + jump strip). `bar_h` = selected-bar height below. */
void lib_body(float bar_h);
bool lib_layout_has_bar();                            // the selected-game bar below the body (not in Showcase)
bool lib_cases_active();                              // All Games is showing the Cases layout (it owns LB/RB, X, R3, L3 and B)
bool lib_cases_back(bool from_esc);                   // B / Esc: closes the menu / leaves a flipped or inspected case (Esc on the plain carousel opens the menu); true when it used the press
bool lib_cases_nav_blocking();                       // the Cases sort picker / quick-jump overlay is open (or closed this frame): game hotkeys (F / I / S) must wait
void lib_cases_menu_toggle();                         // Start / Menu: the slide-down overlay with the hidden toolbar
void lib_cases_closed();                              // Cases is not showing: drop an open menu
void lib_menu_overlay_body(bool* close);              // page_library.cpp: the toolbar + filters + page buttons shown in that overlay
void lib_cases_tv_enter();                            // TV mode on + "TV mode opens in Cases view": switch to it
void lib_sort_chip_changed();                         // the Sort chip / RT was used: the list follows it again
void lib_fake_art_init(SDL_Renderer* ren);     // HUB_SHOT_FAKE_ART=1 (headless): procedural covers for screenshots
bool lib_fake_art(const std::string& key, SDL_Texture** tex, int* w, int* h);
void lib_shot_seed();                                 // HUB_SHOT_LIBVIEW=grid|list|compact|showcase|shelf|cases (+ HUB_SHOT_SHELF_MIXED=1)
