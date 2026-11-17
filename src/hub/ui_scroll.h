#pragma once
/* UI kit: the one vertical scroll area for RetroHub screens.
 *
 * WHAT IT DOES
 *   - Shows a themed scrollbar whenever the content is taller than the area, and none when it fits. The space for it is always
 *     reserved, so the content never reflows when the scrollbar comes and goes. The mouse wheel and dragging the bar work as usual.
 *   - Keeps the controller focus in view WITH A MARGIN (about one row): moving the D-pad down a long list never parks the focus on the
 *     bottom edge.
 *   - LT / RT page it while the pad focus is inside it (innermost area wins): the view moves by most of a page and the focus lands on
 *     the first item of the new view (on the last item when it is already at the end). The press is taken, so main.cpp's global
 *     LT / RT paging does not also run.
 *   - Works inside ImGui popups, child windows, ui::sheet and ui::dialog. It is a NavFlattened child: the D-pad moves freely between
 *     the area and the items around it.
 *
 * HOW TO USE
 *   Call scroll_begin() and ALWAYS call scroll_end() (like BeginChild / EndChild), in the same ID scope:
 *
 *       if (ui::scroll_begin("rows", ImVec2(0, 0))) {          // 0 = take all remaining width / height
 *           for (...) ui::button(...);
 *       }
 *       ui::ScrollInfo si = ui::scroll_end();                   // si.overflows: the content does not fit
 *
 *   size.y > 0: exactly that tall; 0: the rest of the window; < 0: the rest minus that much (room for a footer).
 *   ScrollFlags_AutoHeight: as tall as the content but at most the height above (dialogs). The first frame measures.
 *   padding: inner padding (default sp(1.5) x sp(1)); focus rings of edge items need about half of it.
 *
 * OPTIONS (flags, all opt-in; without them the area behaves as described above)
 *   ScrollFlags_Eased       the view glides (wheel, LT / RT, focus moves, ImGui's own scroll requests) on a spring; Reduce motion jumps.
 *   ScrollFlags_OverlayBar  no gutter at all: the thumb floats over the area's right edge (keep about sp(1.5) there free, e.g. padding).
 *                           For a screen that must keep its exact layout whether or not it overflows (Home, the Cases menu).
 *   ScrollFlags_AutoGutter  ImGui's own gutter, only while the content overflows (the content reflows when it comes and goes, as a plain
 *                           ImGui child does): for a page that must look exactly as before when it fits.
 *   scroll_by()             moves an area by pixels from outside (a wheel routed by hand), eased in an Eased area.
 *   scrollbar_theme_*()     gives a plain ImGui child (a view that keeps its own scrolling) the same thumb, without making it an area.
 *
 * DO / DON'T
 *   - Give the area a fixed or "rest of the window" height. Do not put it in a host that grows with its content without a cap, or the
 *     host overflows and shows a second scrollbar (ui::sheet and ui::dialog already do this right).
 *   - Use a different id per page / tab when each should keep its own scroll position (e.g. PushID(tab) around it).
 *   - Inside the area, layout is ordinary ImGui: use ui:: widgets, ImGui items or the option rows from ui_controls.h. */

#include "imgui.h"

namespace ui {

enum ScrollFlags_ {
    ScrollFlags_None          = 0,
    ScrollFlags_AutoHeight    = 1 << 0,   // shrink to the content, at most the given height
    ScrollFlags_PadOwner      = 1 << 1,   // LT / RT page this area while the pad focus is anywhere in its host window (sheet / dialog bodies)
    ScrollFlags_NoFocusFollow = 1 << 2,   // leave the scroll alone when the focus moves (the caller scrolls itself)
    ScrollFlags_NoPadPaging   = 1 << 3,   // do not take LT / RT
    ScrollFlags_Eased         = 1 << 4,   // glide to every new position (spring; Reduce motion jumps)
    ScrollFlags_OverlayBar    = 1 << 5,   // no gutter: the thumb floats over the right edge, the content never moves
    ScrollFlags_AutoGutter    = 1 << 6,   // ImGui's own gutter, only while the content overflows (exactly a plain child's layout)
};
typedef int ScrollFlags;

struct ScrollInfo {
    bool visible = false;      // the area was drawn this frame
    bool overflows = false;    // the content is taller than the view (a scrollbar shows)
    float scroll = 0.f;        // current offset, 0 = top
    float scroll_max = 0.f;    // largest offset (0 when it fits)
    float view_h = 0.f;        // visible height
    float content_h = 0.f;     // full content height (padding included)
    ImGuiID window_id = 0;     // the ImGui child window (for ImGui::FindWindowByID)
};

/* Begin a scroll area. Returns false when nothing needs drawing (fully clipped); call scroll_end() either way. */
bool scroll_begin(const char* id, ImVec2 size = ImVec2(0, 0), ScrollFlags flags = ScrollFlags_None, ImVec2 padding = ImVec2(-1.f, -1.f));
/* End it: draws the scrollbar and returns the area's numbers for this frame. */
ScrollInfo scroll_end();

/* Ask an area (by the window id from ScrollInfo) to jump to `y` on its next frame, e.g. 0 to go back to the top. */
void scroll_set(ImGuiID window_id, float y);
/* Move an area (by the window id from ScrollInfo) by dy pixels on its next frame (+ = down); an Eased area glides there and adds up
 * quick repeats, like wheel notches. For a wheel routed by hand. */
void scroll_by(ImGuiID window_id, float dy);

/* The kit's thumb on a plain ImGui child that keeps its own scrolling (a virtualised list that sets its scroll itself): ImGui keeps the
 * gutter, the wheel and the dragging, only the look changes. Call scrollbar_theme_push() right before ImGui::BeginChild,
 * scrollbar_theme_pop() right after it, and scrollbar_theme_draw() inside the child just before ImGui::EndChild. */
void scrollbar_theme_push();
void scrollbar_theme_pop();
void scrollbar_theme_draw();

/* After an LT / RT page that found nothing focusable in the new view (a long text, a tall panel) the focus stays on an item that is now out
 * of view, with its cursor hidden: A presses nothing (ImGui activates only a visible cursor) until the next D-pad press, which is scored
 * from the edge of the view and so lands on what is on screen. True while that holds: the caller must not show the cursor again
 * (main.cpp re-shows it every frame for pad users). */
bool nav_cursor_parked();

}  // namespace ui
