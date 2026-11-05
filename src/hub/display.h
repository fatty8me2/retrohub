#pragma once
/* Plan 4 Wave 3 D1/D2: monitors, remembered window rect, UI scale glue (logic in display_logic.h). Nothing here ever raises, shows
 * or activates the window: it only sets position/size at start-up or when the player changes a setting. */
#include "display_logic.h"
#include <SDL.h>

namespace hubdisplay {

std::vector<Monitor> monitors();                       // live monitors (Win32 EnumDisplayMonitors)
std::string monitor_label(const Monitor& m);           // "Monitor 2 (2560x1440)", with "- main" on the primary
float dpi_scale_for(const Monitor* m);                 // that monitor's DPI scale (1.0 = 96 dpi); 1.0 when unknown
float dpi_scale();                                     // the DPI scale of the monitor the UI was last built for
void set_dpi_scale(float s);
void seed_monitor(SDL_Window* win);                    // remember which monitor the window opened on (so the first move to another one rescales)
bool os_animations_on();                               // Windows "Show animations" (SPI_GETCLIENTAREAANIMATION)
Rect work_area_of(SDL_Window* win);                    // work area of the monitor the window is on (empty when unknown)
void apply_min_size(SDL_Window* win, float scale);     // minimum window size for a scale, clamped to the work area (L3)

/* Main loop hooks (no-ops in headless runs). */
void note_event(const SDL_Event& e);                   // window moved / resized / display changed
/* Commits a settled rect into the settings (returns true when something changed). `force` = at exit. */
bool tick(SDL_Window* win, bool force, bool tv_active);
/* Display changed under the window: the new DPI scale for it, or 0 when nothing changed. */
float take_display_changed(SDL_Window* win);
/* Settings > Open on: move to that monitor now (never raises or activates). */
bool move_to_monitor(SDL_Window* win, const std::string& monitor);
/* Entering TV mode: make sure the window sits on the chosen monitor so fullscreen lands there. */
void prepare_fullscreen_monitor(SDL_Window* win);

}  // namespace hubdisplay
