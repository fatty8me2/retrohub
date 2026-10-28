#pragma once
/* Quick menu (View+Y "Game menu") controls: pure logic, no SDL / ImGui / Win32.
 *
 * The menu does NOT use ImGui's gamepad/keyboard navigation and does not depend on which window has the OS focus. Every frame the
 * hub reads the controllers directly (qm_pad.cpp: SDL game controllers + XInput), turns that into a NavBits mask and feeds it here:
 *
 *   MenuInput  edge detection, a per-button release gate (a button held when the menu opened -- View+Y itself, or A/B still held from
 *              the game -- does nothing until it has been seen released) and d-pad auto-repeat.
 *   MenuNav    the selected row, by id, over the rows the menu drew last frame; turns events into "activate row X" / "step row X by d".
 *
 * Tested in test_qm_nav.cpp (unit) and the headless end-to-end run qm_e2e.cpp (virtual SDL joystick). */
#include <cstdint>
#include <string>
#include <vector>

namespace hubqm {

enum NavBit : uint32_t {
    NB_UP = 1u << 0, NB_DOWN = 1u << 1, NB_LEFT = 1u << 2, NB_RIGHT = 1u << 3,
    NB_A = 1u << 4,   // activate (A, Enter, Space)
    NB_B = 1u << 5,   // back (B, Esc, Backspace)
};

struct NavEvents {
    int dx = 0, dy = 0;
    bool activate = false, back = false;
    bool any() const { return dx || dy || activate || back; }
};

struct MenuInput {
    static constexpr long long kRepeatDelayMs = 380, kRepeatRateMs = 110;
    uint32_t prev = 0, armed = 0;
    long long rep_at[4] = {0, 0, 0, 0};
    /* The menu just opened with `held` down: those buttons stay disarmed until released. */
    void open(uint32_t held) { prev = held; armed = ~held; for (auto& r : rep_at) r = 0; }
    /* now: buttons down this frame (polled state, the source of truth); tapped: buttons with a button-down EVENT since the last
     * update. A tap only adds a press the state could not show: down and up again between two frames (up now and up last frame).
     * A down event for a button the state shows held adds nothing (no double press from a duplicate or late event). */
    NavEvents update(uint32_t now, long long now_ms, uint32_t tapped = 0) {
        armed |= ~now;        // seen up (or never held): armed
        const uint32_t pressed = ((now & ~prev) & armed) | (tapped & ~now & ~prev);
        NavEvents ev;
        static const uint32_t dirs[4] = {NB_UP, NB_DOWN, NB_LEFT, NB_RIGHT};
        int fire[4] = {0, 0, 0, 0};
        for (int i = 0; i < 4; ++i) {
            const uint32_t b = dirs[i];
            if (pressed & b) { fire[i] = 1; rep_at[i] = now_ms + kRepeatDelayMs; }
            else if ((now & b) && (armed & b) && rep_at[i] && now_ms >= rep_at[i]) { fire[i] = 1; rep_at[i] = now_ms + kRepeatRateMs; }
            if (!(now & b) && !(pressed & b)) rep_at[i] = 0;
        }
        ev.dy = fire[1] - fire[0];
        ev.dx = fire[3] - fire[2];
        ev.activate = (pressed & NB_A) != 0;
        ev.back = (pressed & NB_B) != 0;
        if (ev.back) ev.activate = false;   // both in one frame: back wins (never an accidental action)
        prev = now;
        return ev;
    }
};

/* One selectable thing the menu drew: a button (A activates) or a stepper (left / right step it, A steps +1). x/y place it on a grid
 * (lists: x = 0, y = row; the slot pages: 5 columns). */
struct NavRow {
    std::string id;
    bool stepper = false, enabled = true;
    int x = 0, y = 0;
};

struct NavAction {
    std::string id;        // "" = nothing this frame
    bool activate = false;
    int step = 0;          // stepper: -1 / +1
};

struct MenuNav {
    std::vector<NavRow> rows, building;
    std::string sel;       // selected row id ("" = the first enabled row)
    NavAction act;
    bool back = false;

    void reset(const std::string& first = "") { rows.clear(); building.clear(); sel = first; act = NavAction(); back = false; }

    int index_of(const std::vector<NavRow>& v, const std::string& id) const {
        for (size_t i = 0; i < v.size(); ++i) if (v[i].id == id) return (int)i;
        return -1;
    }
    int first_enabled(const std::vector<NavRow>& v) const {
        for (size_t i = 0; i < v.size(); ++i) if (v[i].enabled) return (int)i;
        return v.empty() ? -1 : 0;
    }
    /* Nearest enabled row strictly in direction (dx, dy) from `from` (one of them is 0). Rows on the next line are preferred, then
     * the closest column. */
    int neighbour(int from, int dx, int dy) const {
        const NavRow& c = rows[from];
        int best = -1;
        long long best_score = 0;
        for (size_t i = 0; i < rows.size(); ++i) {
            if ((int)i == from || !rows[i].enabled) continue;
            const NavRow& r = rows[i];
            long long primary, secondary;
            if (dy) { primary = (long long)(r.y - c.y) * dy; secondary = r.x > c.x ? r.x - c.x : c.x - r.x; }
            else { if (r.y != c.y) continue; primary = (long long)(r.x - c.x) * dx; secondary = 0; }
            if (primary <= 0) continue;
            long long score = primary * 1000 + secondary;
            if (best < 0 || score < best_score) { best = (int)i; best_score = score; }
        }
        return best;
    }

    /* Start of a frame: apply the events to last frame's rows. The row whose id is in `act` acts when it is drawn this frame. */
    void begin(const NavEvents& ev) {
        act = NavAction();
        back = ev.back;
        building.clear();
        if (rows.empty()) return;
        int cur = index_of(rows, sel);
        if (cur < 0 || !rows[cur].enabled) { int f = first_enabled(rows); if (cur < 0 || f >= 0) cur = f; }
        if (cur < 0) return;
        if (ev.dy) { int n = neighbour(cur, 0, ev.dy); if (n >= 0) cur = n; }
        if (ev.dx) {
            if (rows[cur].stepper) { if (rows[cur].enabled) { act.id = rows[cur].id; act.step = ev.dx; } }
            else { int n = neighbour(cur, ev.dx, 0); if (n >= 0) cur = n; }
        }
        sel = rows[cur].id;
        if (ev.activate && !ev.back && rows[cur].enabled) {
            act.id = rows[cur].id;
            if (rows[cur].stepper) act.step = 1; else act.activate = true;
        }
    }
    /* While drawing: register a row; returns the action aimed at it this frame (empty id = none). */
    NavAction add(const std::string& id, bool stepper, bool enabled, int x, int y) {
        NavRow r; r.id = id; r.stepper = stepper; r.enabled = enabled; r.x = x; r.y = y;
        building.push_back(r);
        NavAction none;
        if (act.id == id && enabled) { NavAction a = act; act = NavAction(); return a; }
        return none;
    }
    bool selected(const std::string& id) const {
        if (sel == id) return true;
        if (index_of(building, sel) >= 0 || index_of(rows, sel) >= 0) return false;
        // nothing selected yet (first frame of a page): the first enabled row drawn shows as selected
        int f = first_enabled(building);
        return f >= 0 && building[f].id == id;
    }
    /* End of the frame: these rows are what the next frame's events apply to. */
    void end() {
        rows.swap(building);
        building.clear();
        int cur = index_of(rows, sel);
        if (cur < 0 || !rows[cur].enabled) { int f = first_enabled(rows); if (cur < 0 && f >= 0) sel = rows[f].id; else if (cur >= 0 && f >= 0 && !rows[cur].enabled) sel = rows[f].id; }
        act = NavAction();
    }
};

}  // namespace hubqm
