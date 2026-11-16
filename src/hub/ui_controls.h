#pragma once
/* UI kit: collapsible sections, option rows (dropdown, toggle, stepper), tab bar, and a wrapping button row.
 * They look and sound like ui::button / ui::chip / ui::segmented (same focus ring, menu sounds, theme colours) and work with the
 * mouse, the keyboard and a controller. Use them inside a ui::sheet / ui::dialog / ui::scroll_begin area.
 *
 * ---- SECTIONS ------------------------------------------------------------------------------------------------------------------
 *       if (ui::section("Graphics")) { ...rows... }             // returns true while open; click / A toggles, Left closes, Right opens
 *       ui::SectionOpts so; so.default_open = false; so.summary = "4 options"; so.persist_key = "game.advanced";
 *       if (ui::section("Advanced", so)) { ... }
 *   Open / closed is remembered per id for the session. To keep it across runs, install ONE store at startup:
 *       ui::section_set_store({ [](const std::string& k) { return my_load(k); },          // -1 unknown, 0 closed, 1 open
 *                               [](const std::string& k, bool open) { my_save(k, open); } });
 *   Only sections with a persist_key use it (the kit never writes a settings file itself).
 *
 * ---- OPTION ROWS -------------------------------------------------------------------------------------------------------------
 * A row = label (+ optional one-line muted help under it, + optional status pill) and a control on the right. Rows between
 * option_group_begin / _end share their columns: the label column and the control width come from the group's longest label and
 * longest value (every dropdown choice counts), so nothing is clipped. When the row is too narrow the control shrinks and its value
 * wraps, then the label wraps, and at last the control moves under the label. The whole row is one focus stop. Rows and section
 * headers are at most sp(112) wide (about 900 px at 100 %), so on a wide window a label stays close to its control.
 *       ui::option_group_begin("graphics");                     // one group per page keeps every section aligned
 *       ui::RowOpts ro; ro.help = "Smoother motion on some games."; ro.pill = "overridden";
 *       if (ui::dropdown("Frame rate", &fps_idx, kFps, 3, ro)) save();
 *       if (ui::toggle("Widescreen", &wide)) save();
 *       if (ui::stepper_int("Frames ahead", &ahead, 0, 4, 1, "%d")) save();
 *       ui::option_group_end();
 *   Controls return true on the frame the value changed.
 *   - dropdown: A / click opens a list right under the row (it scrolls with the page), D-pad moves, A picks, B / Esc closes only
 *     the list (the sheet or dialog stays open). Only one list is open at a time.
 *   - toggle:   A / click flips it.
 *   - stepper:  Left / Right change it while the row has the focus; A / click on the value steps forward (wrapping); click on an
 *     arrow steps that way.
 *   Text entry is not part of the kit: keep using osk::input for it.
 *   Hosting kit controls in your OWN modal: call ui::modal_back() after drawing them, so an open dropdown takes B first.
 *
 * ---- TAB BAR -----------------------------------------------------------------------------------------------------------------
 *       static int tab = 0;
 *       const char* tabs[] = {"About", "Saves", "Settings"};
 *       if (ui::tab_bar("game_tabs", tabs, 3, &tab)) { ... the tab changed ... }
 *   Mouse, D-pad + A, and LB / RB. LB / RB work while the focus is anywhere in the tab bar's window (also inside a popup, where the
 *   hub's global LB / RB is off); the press is taken so the hub does not also switch pages. The innermost tab bar around the focus
 *   wins. TabBarOpts::shoulders_anywhere also takes LB / RB when the focus is elsewhere (a desk pane next to a list).
 *
 * ---- WRAPPING BUTTON ROW -----------------------------------------------------------------------------------------------------
 *       ui::FlowRow row;
 *       row.next(ui::button_width("Play", ui::Icon::Play));  if (ui::button("Play", ImVec2(0, 0), true, true, ui::Icon::Play)) ...
 *       row.next(ui::button_width("Close"));                 if (ui::button("Close")) ...
 *   Each item joins the line if it fits, otherwise it starts a new line. */

#include "imgui.h"
#include "ui_kit.h"

#include <functional>
#include <string>
#include <vector>

namespace ui {

/* ---- sections ---- */
struct SectionOpts {
    bool default_open = true;
    const char* summary = nullptr;       // muted text at the right of the header (e.g. "2 changed")
    const char* persist_key = nullptr;   // key for the persistence store (none = session memory only)
};
bool section(const char* label, const SectionOpts& opts = SectionOpts());

struct SectionStore {
    std::function<int(const std::string& key)> load;               // -1 unknown, 0 closed, 1 open
    std::function<void(const std::string& key, bool open)> save;   // called on every toggle
};
void section_set_store(const SectionStore& store);

/* ---- option rows ---- */
void option_group_begin(const char* id);
void option_group_end();

struct RowOpts {
    const char* help = nullptr;      // short muted text under the label
    const char* pill = nullptr;      // status pill after the label ("overridden", "host decides online")
    ImU32 pill_col = 0;              // 0 = ACCENT
    bool enabled = true;
    const char* tooltip = nullptr;   // hover text (mouse only)
};

bool dropdown(const char* label, int* idx, const char* const* items, int n, const RowOpts& opts = RowOpts());
bool dropdown(const char* label, int* idx, const std::vector<std::string>& items, const RowOpts& opts = RowOpts());
bool toggle(const char* label, bool* v, const RowOpts& opts = RowOpts());
bool stepper(const char* label, int* idx, const char* const* items, int n, const RowOpts& opts = RowOpts());   // steps through named values
bool stepper_int(const char* label, int* v, int lo, int hi, int step, const char* fmt = "%d", const RowOpts& opts = RowOpts());

/* ---- tab bar ---- */
struct TabBarOpts {
    bool wrap = false;                 // LB on the first tab goes to the last (and back)
    bool shoulders_anywhere = false;   // take LB / RB even when the focus is outside this window (no popup above it)
    bool show_hints = true;            // LB / RB glyphs at the ends while a pad is in use
};
bool tab_bar(const char* id, const char* const* labels, int n, int* cur, const TabBarOpts& opts = TabBarOpts());

/* ---- wrapping row of buttons ---- */
float button_width(const char* label, Icon ic = Icon::None);   // the width ui::button(label, ImVec2(0, 0), ..., ic) takes
class FlowRow {
public:
    explicit FlowRow(float gap = -1.f);   // -1 = sp(1.5)
    void next(float item_w);              // call before each item
private:
    float avail_ = 0.f, used_ = 0.f, gap_ = 0.f;
    bool started_ = false;
};

}  // namespace ui
