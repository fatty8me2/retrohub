#pragma once
/* Shelf library view (a wooden bookcase of cartridges, discs and cases): the pure parts. No ImGui here, unit-tested in test_shelf.cpp.
 * Media type per console, item dimensions, row packing (a variable-width item flow, one VLayout row per shelf plank), focus movement
 * (left/right wrap across shelves, up/down to the nearest item), and the slide-out spring + pose. The drawing is shelf_draw.cpp, the
 * integration with the All Games state is libview_ui.cpp. */

#include "libview_logic.h"

#include <cstdint>
#include <string>
#include <vector>

namespace hubui::shelf {

/* ---- what a game of a console looks like on the shelf ---- */
enum class Media { NesCart, SnesCart, N64Cart, GbCart, GbcCart, GbaCart, DsCase, N3dsCase, Ps1Jewel, Ps2Case, GcCase, WiiUCase, PspCase, SwitchCase, Generic };
constexpr int kMediaCount = 15;
Media media_for(const std::string& system_short);   // "n64" "nes" "gb" ... "ps1" "ps2" "gc" "wiiu" "psp" "nds" "3ds" "switch"; unknown -> Generic (a plain keep-case)
const char* media_key(Media m);                     // "nes-cart" "ps1-jewel" ...
bool is_cart(Media m);                              // a cartridge: stands label-out on the shelf (the others stand spine-out)

/* Dimensions in units of the shelf's base height B (the tallest thing on a shelf is about 1.0): face width w, height h, thickness d.
 * Carts rest face-out (rest_theta ~ 0), cases rest spine-out (rest_theta = -90 degrees: the spine faces the viewer). */
struct Dims { float w = 0, h = 0, d = 0; float rest_theta = 0; };
Dims base_dims(Media m);
uint32_t hash32(const std::string& s);              // FNV-1a: the per-game seed for small size / colour variations
Dims item_dims(Media m, uint32_t seed);             // base_dims with a small deterministic jitter (spines differ a little in height and thickness)
float rest_width(const Dims& d);                    // width the item takes on the shelf, in B units (face width for carts, thickness for cases)
float slot_gap(Media m);                            // empty space after the item, in B units

/* ---- rows ---- */
struct Layout {
    VLayout L;                  // kind-1 rows only; y/h are the shelf pitch (top margin included in the first row's y); item_row per flat item
    std::vector<float> x, w;    // per flat item: left edge from the inner left, and slot width (px, gap included)
    std::vector<int> row_sec;   // section of each row
    std::vector<char> row_first;// the row starts its section (the label plaque shows the count there)
};
/* Packs the flat items (slot widths in px, in display order) into shelf rows of `inner_w`: a new section always starts a new row, an
 * item that would run past the right margin starts the next row, an item wider than the row sits alone on its row. `margin_x` is the
 * empty space at each end of a row, `row_h` the pitch between planks, `top` the space above the first row, `bottom` below the last. */
Layout pack_rows(const std::vector<float>& slot_w, const std::vector<int>& sec_sizes, float inner_w, float margin_x, float row_h, float top, float bottom);

enum class Dir { Left, Right, Up, Down };
/* Where the focus goes from item `cur` (of `n`): Left/Right step through the flat order (so they wrap from the start of a shelf to the end
 * of the previous one, across sections); Up/Down go to the item of the adjacent row whose centre is nearest (first of the row on a tie).
 * Returns `cur` when there is nowhere to go (the first/last item, the top/bottom row): the caller lets the UI leave the view. */
int move_focus(const Layout& l, int cur, Dir dir);
/* Row of an item, clamped (0 when empty). */
int row_of(const Layout& l, int item);
/* The rows to submit (and the items inside them): first/last row touching [y0,y1], -1/-1 when there are none. */
void visible_rows(const Layout& l, float y0, float y1, int* first_row, int* last_row);

/* ---- slide-out ---- */
struct Spring { float x = 0, v = 0; };
/* One frame of a slightly under-damped spring toward `target` (sub-stepped, safe for any dt). reduced_motion snaps to the target. */
void spring_step(Spring& s, float target, float dt, bool reduced_motion = false);
bool spring_settled(const Spring& s, float target);
/* Target of the slide-out: 1 for the hovered / pad-focused game, a half pull for the selected one, 0 for the rest. */
float slide_target(bool active, bool selected);

struct SlidePose {
    float scale = 1;     // grows toward the viewer
    float lift = 0;      // extra drop toward the viewer, in B units (+ = down the screen)
    float theta = 0;     // rotation about the vertical axis, radians (0 = face-on; -pi/2 = the spine faces the viewer)
    float shadow = 0;    // 0..1+ drop shadow strength
    float persp = 0;     // perspective strength
    float card = 0;      // 0..1: how far the game card peeks out of its case (DS / 3DS)
};
/* The pose for an item with `rest` dimensions at spring value s (0 = on the shelf, 1 = pulled out) and spring velocity v. */
SlidePose slide_pose(Media m, const Dims& d, float s, float v, bool reduced_motion = false);

}  // namespace hubui::shelf
