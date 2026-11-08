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
    double time = 0;           // seconds, drives the drifting light motes
    bool motion = true;        // false under reduced motion: no drifting light motes
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
    const char* publisher = "";   // "Published by ..." (the maker above is the developer)
    const char* age = "";         // "ESRB E": printed in a plain rating box (text only)
    bool desc_generated = false;  // the description is a factual line built from the metadata, not an online summary
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
    const uint32_t* art_blur = nullptr;   // 64 colours: an 8x8 average of `art` (owned by the art cache; null = none): the soft background under a square icon
    bool ghost = false, fav = false;
    bool nx2 = false;             // a Switch 2 game (Media::SwitchCase with the wide red "NINTENDO SWITCH 2" banner)
    const CaseBack* back = nullptr;
    ImU32 art_col = 0;            // average colour of the front art (0 = unknown): the generated spine / insert colours follow it
    /* A full cover scan (BACK | SPINE | FRONT laid flat, hubart::Kind::FullCover): back / spine / front are uv sub-rects of this one texture,
     * split at the pixel columns back_x1 and front_x0 (hubart::fullcover_split). Null = none. */
    SDL_Texture* full = nullptr;
    int full_w = 0, full_h = 0, full_back_x1 = 0, full_front_x0 = 0;
};
struct CasePose {
    float x = 0, z = 0;        // world offset from the centre line, depth behind the centre plane
    float yaw = 0, pitch = 0;  // radians (yaw 0 = the cover faces the viewer; pi = the back)
    float scale = 1;
    float alpha = 1;
    float lift = 0;            // raised off the floor (inspect)
    ImU32 fog = IM_COL32(6, 7, 11, 255);   // the colour a faded case blends toward (alpha < 1 fades by fog, never by transparency)
    float refl = 0;            // reflection strength used by cases_case(reflection = true)
    float dim = 0;             // Coverflow: 0..1 flat dark overlay on the front (the cases that are not selected)
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


/* ============================ Coverflow (cases_flow.cpp, cases_media.cpp) ============================
 * The default carousel style: a blurred cover-coloured room, soft ellipse shadows under every case, a faint reflection of the selected one, and the disc /
 * cartridge that slides out from behind it. */
/* The whole background of [p0,p1]: the 8x8 colour grid of the selected cover (already darkened / saturated) as a smooth mesh, a radial glow of `tint`
 * (tint_k 0..1), a heavy radial vignette and a darkening top and bottom. Draw first. */
void flow_background(const CaseScene& sc, ImVec2 p0, ImVec2 p1, const ImU32 grid[64], ImU32 tint, float tint_k);
/* The faint radial glow on the floor under the stage. */
void flow_floor_glow(const CaseScene& sc, ImVec2 p0, ImVec2 p1, ImU32 tint, float floor_y);
/* Two soft ellipses under a case standing at height `base` (a broad blur and a tight darker core); strength 0..1 (the fade of the case). */
void flow_shadow(const CaseScene& sc, hubui::shelf::Media m, const CasePose& ps, float base, float strength);
/* A short, faint reflection of a case (about 22% opacity, 20% of its height, fading out); `bg` is not needed: it fades by alpha bands. */
void flow_reflection(const CaseScene& sc, const CaseDraw& d, const CasePose& ps, float strength);
/* The colour of the background grid at screen fractions (u, v) (bilinear), for the colour the cases fade into. */
ImU32 flow_grid_at(const ImU32 grid[64], float u, float v);


}  // namespace shelfdraw
