#pragma once
/* All Games layouts (grid / list / compact grid / showcase), the A-Z jump strip and the sortable list columns.
 * Pure logic (no ImGui): unit-tested in test_libview.cpp. The drawing is libview_ui.cpp. */

#include <string>
#include <vector>

namespace hubui {

/* ---- which layout All Games uses (saved in Settings::lib_layout) ---- */
enum class LibLayout { Grid = 0, List = 1, Compact = 2, Showcase = 3, Shelf = 4 };
constexpr int kLibLayouts = 5;
const char* layout_key(LibLayout l);                 // "grid" "list" "compact" "showcase" "shelf"
const char* layout_name(LibLayout l);                // "Grid" "List" "Compact" "Showcase" "Shelf"
LibLayout layout_from_key(const std::string& k);     // unknown -> Grid

/* ---- A-Z jump strip ---- */
/* Strip letter of a title: the first letter or digit, upper-cased; "#" for digits and anything else ("" for an empty title). */
std::string letter_of(const std::string& title);

/* A run of consecutive items with the same label ("" = unlabelled, e.g. the pinned native port: never a jump target). */
struct JumpRun { std::string label; int first = 0; };
std::vector<JumpRun> build_runs(const std::vector<std::string>& labels);
/* One strip mark per distinct label, pointing at its first run (the strip lists each letter once). */
std::vector<JumpRun> strip_marks(const std::vector<JumpRun>& runs);
/* Index of the run that holds item `idx` (-1 when there are no runs). */
int run_of(const std::vector<JumpRun>& runs, int idx);
/* Item to land on for "next letter" (dir > 0) / "previous letter" (dir < 0) from item `cur`; unlabelled runs are skipped. Previous
 * goes to the start of the current run first, then to the run before it. -1 when there is nowhere to go. */
int jump_target(const std::vector<JumpRun>& runs, int cur, int dir);
/* The strip mark nearest to a pointer: y in [0,h] over `n` equal slots -> slot index (clamped; -1 when n == 0). */
int strip_slot(float y, float h, int n);
/* Letters shown on the strip: all of A-Z and # when `letters`, otherwise just the marks. For each slot, the index into `marks`
 * of the first mark at or after it (so a click on an empty letter lands on the next present one), -1 past the end. */
std::vector<int> strip_targets(const std::vector<JumpRun>& marks);

/* ---- List view columns ---- */
enum class ListCol { None = 0, Title, Console, Players, Last, Time };
const char* col_key(ListCol c);                      // "" "title" "console" "players" "last" "time"
ListCol col_from_key(const std::string& k);          // unknown -> None
const char* col_name(ListCol c);                     // header text
bool col_default_desc(ListCol c);                    // Players / Last / Time start biggest first
/* A header click: the same column flips its direction, a new column starts at its default direction. */
void col_click(ListCol cur, bool cur_desc, ListCol clicked, ListCol* out, bool* out_desc);

struct ListRow {
    std::string key, title, system;       // system short code
    bool pinned = false, ghost = false;   // pinned (the native port) always first, friends' games always last
    int players = 0;                      // local players
    unsigned long long last_played = 0, play_seconds = 0;
};
/* Indices (into rows) of `idx` (all rows when null) ordered by the column. Title A-Z / Console (in console order, then title) /
 * Players / Last played / Play time (ties by title); `desc` reverses the column, never the pinned/ghost placement; a game
 * that was never played / has no time is always after the played ones. ListCol::None keeps the incoming order. */
std::vector<int> sort_list_rows(const std::vector<ListRow>& rows, const std::vector<int>* idx, ListCol col, bool desc);

/* Which columns fit in `w` px (unit = the DPI-scaled 8 px step). x offsets are relative to the row's left edge. */
struct ListCols {
    bool console = true, players = true, last = true, time = true;
    float thumb_x = 0, title_x = 0, title_w = 0, console_x = 0, players_x = 0, last_x = 0, time_x = 0, icons_x = 0;
    float console_w = 0, players_w = 0, last_w = 0, time_w = 0, icons_w = 0;
};
ListCols list_columns(float w, float unit);

/* ---- virtual rows (grid / compact / list share this) ---- */
struct VRow { int kind = 0; float y = 0, h = 0; int first = 0, count = 0; };   // kind 0 = section header (first = section), 1 = items [first, first+count)
struct VLayout {
    std::vector<VRow> rows;
    std::vector<int> item_row;   // row of each flat item
    float total_h = 0;
    int cols = 1;
};
/* Rows for sections of the given sizes (a section with title_h == 0 gets no header row): `sec_sizes` items per section,
 * `cols` items per row, `item_h` per item row, `gap` between rows, `head_h` per header. Section starts are consecutive. */
VLayout build_vlayout(const std::vector<int>& sec_sizes, const std::vector<bool>& sec_has_header, int cols, float item_h, float head_h, float gap, float sec_gap);
/* First/last row intersecting [y0,y1] (binary search; -1 / -1 when empty). */
void rows_in_range(const VLayout& l, float y0, float y1, int* first, int* last);
/* Scroll that brings row `r` into view with `margin` px of room above, clamped to [0,max_scroll]; keeps `cur` when it is already visible. */
float scroll_to_row(const VLayout& l, int r, float cur, float view_h, float margin, float max_scroll);

}  // namespace hubui
