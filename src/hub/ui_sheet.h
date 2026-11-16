#pragma once
/* UI kit: page sheet and dialog, the two hosts every RetroHub screen and modal should use.
 *
 * ---- PAGE SHEET -------------------------------------------------------------------------------------------------------------
 * A "page" with a pinned header, a pinned footer and a scrolling body between them that takes ALL the remaining height. It is
 * exactly the rect you pass, every frame: no auto-resize, no re-centring. The same three callbacks work in both hosting modes:
 *   SheetHost::Overlay  covers the rect (normally the whole hub window) as a modal: it captures the D-pad, B / Esc
 *                       close it through ui::modal_back() (after your callbacks ran, so a widget that eats B, like an open
 *                       dropdown, wins). `*open` drives it: set it true to show the sheet, it becomes false when the sheet closes.
 *   SheetHost::Pane     draws into the rect inside the current window (the "Library desk" pane). No modality, no B handling:
 *                       the screen around it decides what B does. `open` may be nullptr.
 * Call it EVERY frame while it may be shown (Overlay: also while *open is false, so it can close cleanly):
 *
 *       static bool page_open = true;
 *       static int tab = 0;
 *       ui::SheetParts parts;
 *       parts.header = [&] { draw_title(); ui::tab_bar("tabs", kTabs, 3, &tab); };
 *       parts.body   = [&] { ui::option_group_begin("opts"); ...rows...; ui::option_group_end(); };
 *       parts.footer = [&] { if (ui::button("Close")) page_open = false; };
 *       ui::SheetOpts so;
 *       so.body_key = kTabs[tab];                        // each tab keeps its own scroll position
 *       ui::sheet("game_page", ui::SheetHost::Overlay, ImVec2(0, 0), ImGui::GetIO().DisplaySize, &page_open, parts, so);
 *
 * Inside the callbacks use ordinary ImGui / ui:: widgets. The body is a ui::scroll_begin area (themed scrollbar, focus kept in view
 * with a margin, LT / RT page it even while the focus is in the header or footer). Open dialogs from INSIDE a callback, so they
 * stack on top of an Overlay sheet (opening one from outside replaces the sheet: ImGui popups at the same level replace each other).
 *
 * ---- DIALOG ------------------------------------------------------------------------------------------------------------------
 * A centred modal with a pinned title row, a scrolling middle and a pinned button row. Width from three sizes (capped at 92 % of the
 * window), height grows with the content up to 90 % of the window, then the middle scrolls. B / Esc set *open = false.
 *
 *       if (ui::dialog_begin("reset", "Reset this game's settings?", &ask_reset, ui::DialogSize::Small)) {
 *           ImGui::TextWrapped("...");                   // the middle: scrolls when long
 *           ui::dialog_buttons();                        // from here on: the pinned button row
 *           if (ui::button("Reset", ImVec2(0, 0), true)) { do_reset(); ask_reset = false; }
 *           ImGui::SameLine();
 *           if (ui::button("Cancel")) ask_reset = false;
 *           ui::dialog_end();                            // only when dialog_begin returned true
 *       }
 *
 * Call dialog_begin every frame while *open may be true, from the window it belongs to (inside a sheet callback for a sheet's dialog).
 * open = nullptr: the dialog is up whenever you call dialog_begin (your own state decides, like the old "OpenPopup every frame" code).
 * Close it with ImGui::CloseCurrentPopup() before dialog_end(), then stop calling it. B / Esc are yours then: read ui::modal_back().
 *
 * CONVERTING AN OLD BeginPopupModal DIALOG (the recipe; batch 1 = emu_dialogs, add / find games, hacks, cloud):
 *   1. OpenPopup + SetNextWindow* + PushStyleVar + BeginPopupModal -> dialog_begin(<the SAME popup name>, <title>, ...) with
 *      opts.keep_id = true (QA scripts match window names, other code calls IsPopupOpen(name)). ImGui's title-bar text or the old big
 *      heading becomes `title`; pick Small / Medium / Large nearest the old width.
 *   2. "Open while my state says so" code: open = nullptr, keep the state machine and its CloseCurrentPopup(). "OpenPopup once" code: a bool.
 *   3. Keep each B / Esc rule as it was: back_closes = false and the dialog's own ui::modal_back() where B is conditional (busy, no Cancel).
 *   4. Buttons after dialog_buttons(), in the same order; drop the Dummy spacer above them; SameLine between buttons -> dialog_same_line(w).
 *   5. A nested BeginChild list becomes plain rows in the middle (one scrollbar). Keep the item order: the default focus is the first item
 *      (or SetItemDefaultFocus / SetKeyboardFocusHere) and works the same from the middle or the button row.
 *   Pitfalls: every early return must call dialog_end(), never ImGui::EndPopup(); a PushTextWrapPos(x) sized for the old window wraps
 *   wrongly in the middle (use TextWrapped / wrap pos 0); a fixed-width button wider than the dialog still clips (size it from the row).
 *
 * ---- FOOTER HINTS ------------------------------------------------------------------------------------------------------------
 * An Overlay sheet covers the hub's own footer: put the controller / keyboard hints in the sheet footer with ui::hint_bar().
 *
 * ---- GOOD TO KNOW ------------------------------------------------------------------------------------------------------------
 * - The B / Esc that closes a dialog or sheet is consumed: it never also closes what is under it or reaches the hub's "back".
 * - Like every ImGui popup, an Overlay sheet loops the D-pad vertically at its own level: Down on the last footer button goes to
 *   the first header item (inside the body the D-pad stops at the body's ends and steps into the footer / header).
 * - The library page seeds its focus with SetNavID even while a modal is open (page_library.cpp seed_nav_here). An Overlay that
 *   opens on the very first frames after start-up (before the library has drawn) can lose its first focus to it. */

#include "imgui.h"
#include "ui_kit.h"
#include "ui_scroll.h"

#include <functional>

namespace ui {

enum class SheetHost { Overlay, Pane };

struct SheetParts {
    std::function<void()> header;   // pinned at the top (title, facts, tab bar ...)
    std::function<void()> body;     // scrolls; gets all the height between header and footer
    std::function<void()> footer;   // pinned at the bottom (buttons, hints); its height is measured each frame
};

struct SheetOpts {
    float pad = -1.f;               // inner padding; -1 = sp(3), or sp(2) on a small sheet
    ImU32 bg = 0;                   // background; 0 = BG (Overlay) or PANEL (Pane)
    float rounding = -1.f;          // corner rounding; -1 = 0 (Overlay) or sp(2) (Pane)
    bool back_closes = true;        // Overlay: B / Esc close the sheet
    bool separators = true;         // hairlines under the header and above the footer
    const char* body_key = nullptr; // optional: a separate scroll position per key (e.g. the tab's name)
    bool nav_flatten = true;        // Pane: the D-pad moves freely between the pane and the items around it
    float gap = -1.f;               // space between header, body and footer; -1 = sp(1.5)
};

struct SheetResult {
    bool shown = false;             // drawn this frame
    bool closed = false;            // Overlay: closed this frame (B / Esc, or *open became false)
    ScrollInfo body;                // the body's scroll numbers (overflows ...)
};

/* Draw a page sheet into the rect at `pos` (screen pixels) of `size`. See the guide above. For the whole hub window pass
 * ImVec2(0, 0) and ImGui::GetIO().DisplaySize. */
SheetResult sheet(const char* id, SheetHost host, ImVec2 pos, ImVec2 size, bool* open, const SheetParts& parts, const SheetOpts& opts = SheetOpts());
/* The inner padding sheet() uses for that size and opts (opts.pad, or its own choice when that is -1). */
float sheet_pad(ImVec2 size, const SheetOpts& opts);

enum class DialogSize { Small, Medium, Large };   // about 416 / 528 / 704 px at 100 % UI size

struct DialogOpts {
    const char* subtitle = nullptr; // muted line under the title
    bool back_closes = true;        // B / Esc set *open = false
    bool close_x = false;           // a small close button at the right of the title row (needs `open`)
    bool keep_id = false;           // the ImGui popup name is exactly `id` (no suffix): a converted dialog keeps the name others look up
};

/* Begin a dialog. Shows it while *open is true (opens it the first time), closes it when *open turns false; open = nullptr: see above.
 * Returns true when it is on screen: then draw the middle, call dialog_buttons(), draw the buttons and call dialog_end(). */
bool dialog_begin(const char* id, const char* title, bool* open, DialogSize size = DialogSize::Medium, const DialogOpts& opts = DialogOpts());
void dialog_buttons();   // ends the scrolling middle, starts the pinned button row (optional: dialog_end() does it if you did not)
void dialog_end();
/* Between buttons (button row or middle): the next item, `next_w` wide, goes on this line when it fits, else it starts a new line, so a
 * row wraps instead of running past the edge at a big UI size. gap < 0 = the style's item spacing. */
void dialog_same_line(float next_w, float gap = -1.f);
float dialog_button_w(const char* label, bool icon = false);   // the width ui::button() gives `label` with size.x = 0

/* One line of controller / keyboard hints ("(A) Select  (B) Close"). Pad glyphs while a pad is in use, key caps otherwise; a hint
 * with no key is shown only with a pad. A hint with no label sits tight against the next one, as a pair ({"LB", "[", ""},
 * {"RB", "]", "Tabs"}), shown whole or not at all. Stops at the right edge rather than overflow. */
struct KeyHint { const char* pad; const char* key; const char* label; };
void hint_bar(const KeyHint* hints, int n);
float hint_bar_width(const KeyHint* hints, int n);   // the width hint_bar() needs right now (e.g. to right-align it with SameLine)

}  // namespace ui
