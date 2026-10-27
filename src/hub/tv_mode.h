#pragma once
/* TV mode (L5): the same hub at 10-foot scale, borderless fullscreen on the current monitor. Logic in tv_mode.cpp; the rest of the
 * UI only calls the small hooks below. */
#include <SDL.h>
#include "imgui.h"

namespace tvmode {

bool active();
/* Called once after the window exists and BEFORE fonts are built: base_scale = desktop DPI scale. Returns the scale to build the
 * fonts at (the TV scale when TV mode starts on). `cmd_flag` = launched with --tv. */
float init(SDL_Window* win, bool headless, float base_scale, bool cmd_flag, bool start_setting, int shot_h);
void toggle();
/* Main loop hooks. */
void handle_event(const SDL_Event& e, bool want_text);
void frame_begin(SDL_Window* win, SDL_Renderer* ren);   // applies a pending on/off (window + font rebuild) between frames
void tick(SDL_Window* win, bool game_running);          // back to fullscreen after a game exits

/* UI hooks. */
float hero_h(float normal, float avail_h);
bool force_icon_sidebar();
void draw_corner(float right_x, float top_y, float h);  // clock + online friends, top-right
void settings_ui();                                     // General tab block

}  // namespace tvmode
