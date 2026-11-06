#pragma once
/* Shelf library view: the procedural drawing (wooden bays, cartridges, discs and cases drawn from ImDrawList primitives only; the only
 * images are the cover art the app already caches). Everything here takes plain values: the integration with the library state is
 * libview_ui.cpp, the pure layout / animation maths is shelf_logic.h. */
#include "shelf_logic.h"
#include "ui_kit.h"

#include <string>
#include <vector>

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


/* ============================ Cases view (cases_draw.cpp) ============================
 * A 3D carousel of game cases on a glossy floor. World units: a tall keep case is 1.0 high. The camera looks straight ahead from `eye`
 * above the floor; `cx` is the screen x of the centre line, `hy` the screen y of the horizon, `U` pixels per world unit at depth 0. */
struct CaseScene {
    ImDrawList* dl = nullptr;
    float cx = 0, hy = 0, U = 100, dist = 6.5f, eye = 0.55f;
    /* stage lighting (car-stage) */
    float time = 0;            // seconds, drives the drifting light motes
    bool motion = true;        // false under reduced motion: no motes, no breathing glow
    float spot = 1;            // 0..1 strength of the spotlight on the centre case (dips a little while the carousel moves)
    float dim = 0;             // 0..1 inspect: the whole stage darkens around the case
    float floor_y(float z = 0.f) const { return hy + eye * U * dist / (dist + z); }   // screen y of the floor at depth z
};
struct CaseShot { SDL_Texture* tex = nullptr; int w = 0, h = 0; };
/* What the back of the centre case shows. Text pointers must outlive the frame. `lines` is a cache the painter fills when the size changes. */
struct CaseBack {
    const char* desc = "";
    const char* genre = "";
    const char* maker = "";
    int year = 0, players = 0;
    CaseShot shots[3];
    int nshots = 0;
    const char* shots_label = "";
    mutable std::vector<std::string> lines;
    mutable float lines_w = -1, lines_size = -1;
    mutable int lines_max = -1;
};
struct CaseDraw {
    hubui::shelf::Media media = hubui::shelf::Media::Generic;
    uint32_t seed = 0;
    const char* title = "";
    const char* sys_name = "";    // long console name for the header band
    const char* sys_short = "";   // short one for the spine band
    ImU32 c0 = 0, c1 = 0;         // console colours
    SDL_Texture* art = nullptr;
    int art_w = 0, art_h = 0;
    bool ghost = false, fav = false;
    const CaseBack* back = nullptr;
};
struct CasePose {
    float x = 0, z = 0;        // world offset from the centre line, depth behind the centre plane
    float yaw = 0, pitch = 0;  // radians (yaw 0 = the cover faces the viewer; pi = the back)
    float scale = 1;
    float alpha = 1;
    float lift = 0;            // raised off the floor (inspect)
    ImU32 fog = IM_COL32(6, 7, 11, 255);   // the colour a faded case blends toward (alpha < 1 fades by fog, never by transparency)
    float refl = 0;            // reflection strength used by cases_case(reflection = true)
    bool refl_detail = false;  // text in the reflection
};
/* Wall, floor and the soft horizon in the console tint (c0 / c1 of the centre case), inside [p0,p1]. Draw first. */
void cases_stage_back(const CaseScene& sc, ImVec2 p0, ImVec2 p1, ImU32 c0, ImU32 c1);
/* Fades the reflections into the floor: call after every cases_case(reflection = true) and before the floor details and the cases. */
void cases_veil(const CaseScene& sc, ImVec2 p0, ImVec2 p1, ImU32 c0, ImU32 c1, float case_h);
/* Floor details drawn over the veil: perspective lines, the pool of light under the centre case, a gloss streak. */
void cases_stage_front(const CaseScene& sc, ImVec2 p0, ImVec2 p1, ImU32 c0, ImU32 c1);
/* Softly darkens the screen edges: call after the cases, before the text. */
void cases_vignette(const CaseScene& sc, ImVec2 p0, ImVec2 p1, float strength);
void cases_shadow(const CaseScene& sc, const CaseDraw& d, const CasePose& ps);   // contact shadow on the floor
void cases_case(const CaseScene& sc, const CaseDraw& d, const CasePose& ps, bool reflection);
/* Screen box of a case (for hit tests). */
void cases_bounds(const CaseScene& sc, hubui::shelf::Media m, const CasePose& ps, ImVec2* mn, ImVec2* mx);
/* Title with a soft glow in `glow`. */
void glow_text(ImDrawList* dl, ImFont* font, float size, ImVec2 p, ImU32 col, ImU32 glow, float strength, const char* text);

}  // namespace shelfdraw
