#pragma once
/* Wave 3 L6/L7/L8 hooks: Attract mode, Surprise me and RetroHub Wrapped. The logic lives in attract.cpp, surprise.cpp and
 * wrapped_ui.cpp (pure parts in attract_logic.h, surprise_logic.h, wrapped_logic.h); the pages only call these. */

#include <SDL.h>

/* ---- attract.cpp (L6) ---- */
/* Call once per frame at the top of the Home page. Returns true while the hero should be replaced by the slideshow
 * (then call attract_draw_hero instead of draw_hero). On the frame it ends it hands the focus back to the hero. */
bool attract_update();
void attract_draw_hero(float h);
/* Main loop: while attract mode runs, keyboard / mouse events only wake it up and are not passed on (true = swallowed). */
bool attract_swallow_event(const SDL_Event& e);
bool attract_active();

/* ---- surprise.cpp (L7) ---- */
/* The dice button for the Home top bar (size = button height). Also runs the spin popup and opens the picked game's details. */
void surprise_ui_button(float size);

/* ---- wrapped_ui.cpp (L8) ---- */
void wrapped_ui_open();     // from the Your stats card
void wrapped_ui_modal();    // draw the Wrapped popup (call from the same place every frame)
void wrapped_ui_shot_init();   // HUB_SHOT_WRAPPED* aids
