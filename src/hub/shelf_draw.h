#pragma once
/* Shelf library view: the procedural drawing (wooden bays, cartridges, discs and cases drawn from ImDrawList primitives only; the only
 * images are the cover art the app already caches). Everything here takes plain values: the integration with the library state is
 * libview_ui.cpp, the pure layout / animation maths is shelf_logic.h. */
#include "shelf_logic.h"
#include "ui_kit.h"

#include <string>

struct SDL_Texture;

namespace shelfdraw {

/* Vertical geometry of one shelf row for a base item height B (px). The row is: head room, the items (up to B tall), the plank top
 * (depth) and the plank's front edge. Items stand on the plank at row_top + base_dy. */
struct RowGeo {
    float B = 100, head = 0, depth = 0, edge = 0, pitch = 0, side = 0, base_dy = 0;
};
RowGeo row_geo(float B, float cap_font_px);

/* One shelf bay [x0,x1] whose row starts at y: back panel, side uprights, the plank, and a plaque on the plank front edge
 * (label empty = no plaque). `chip` is the console colour dot on the plaque. */
void bay(ImDrawList* dl, const RowGeo& g, float x0, float x1, float y, const std::string& label, const std::string& count_text, ImU32 chip, bool section_start);

struct Item {
    hubui::shelf::Media media = hubui::shelf::Media::Generic;
    hubui::shelf::Dims dims;
    uint32_t seed = 0;
    std::string title, sys_name;      // sys_name: the short console name for the colour band ("PS2")
    ImU32 c0 = 0, c1 = 0;             // console colours
    SDL_Texture* art = nullptr;
    int art_w = 0, art_h = 0;
    bool ghost = false, fav = false;
};
/* Draws one item standing at `base` (bottom centre, on the plank). pose = rest or slide-out pose; ring draws the pad focus ring.
 * Returns the screen bounds of what was drawn. */
void item(ImDrawList* dl, const Item& it, float B, ImVec2 base, const hubui::shelf::SlidePose& pose, bool ring, ImVec2* bmin = nullptr, ImVec2* bmax = nullptr);
/* The title plaque that hangs under a pulled-out game (clamped into [x0,x1]). */
void caption(ImDrawList* dl, ImVec2 anchor, const std::string& title, const std::string& sub, float x0, float x1, float alpha);

}  // namespace shelfdraw
