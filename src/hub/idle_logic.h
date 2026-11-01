#pragma once
/* Idle throttle for the hub's frame loop (Plan 4, M3). Pure decision logic (no SDL, no ImGui), unit-tested by hub_idle_test.
 *
 * Before: every loop iteration drew a full ImGui frame and presented it at vsync (60-144 fps), even with the window behind a game or
 * minimised-but-not-iconic, and with nothing on screen changing.
 *
 * Now the loop still iterates at (at least) its old cadence, so every poller it pumps keeps its rate: the lobby and friends sockets, the
 * Dolphin / 3DS / remote-play / Kanto drivers, the pad watchers (View+Y, View+Menu), the process watcher, the invite pipe. Only the
 * DRAWING is thinned, and only while the hub is in the background (not the focused window, hidden, or a game session running):
 *   - anything animating or time-critical named by the caller (`busy`: boot animation, toasts, the View+Y menu, a netplay driver
 *     session ...) keeps drawing every iteration, as does any iteration that handled input or was asked for a repaint (`dirty`);
 *   - for kLingerMs after the last input or visible change it draws at most every kActiveGapMs (about 30 fps);
 *   - after that, a static UI draws every kIdleGapMs (about 10 fps: clocks, "ago" texts and timers still refresh);
 *   - in between, the loop waits for an SDL event for at most kPumpMs (SDL_WaitEventTimeout: input wakes it at once).
 * A focused hub with no game is never throttled. */

#include <algorithm>

namespace hubidle {

constexpr unsigned long long kLingerMs = 400;   // after the last input / visible change the UI keeps its fast cadence (hover and fade transitions finish)
constexpr int kActiveGapMs = 33;                // background but still changing: about 30 fps
constexpr int kIdleGapMs = 100;                 // background and static: about 10 fps
constexpr int kPumpMs = 16;                     // longest wait between loop iterations, so every poller keeps about 60 Hz or better
constexpr int kMinIterMs = 4;                   // an iteration woken early by noise (pad axis events) is padded to this

struct Inputs {
    bool enabled = true;       // not switched off (HUB_IDLE_THROTTLE=0, QA drivers)
    bool background = false;   // hub not focused / hidden / occluded, or a game session is running
    bool busy = false;         // animating or time-critical: boot animation, toasts, the View+Y menu, a netplay driver session ...
    bool input = false;        // an input event reached the UI this iteration
    bool dirty = false;        // something asked for a repaint (library applied, notice shown ...)
    unsigned long long now_ms = 0;
    unsigned long long last_render_ms = 0;     // when the last frame was drawn
    unsigned long long last_activity_ms = 0;   // last input event or last frame that looked different from the one before
};

struct Decision {
    bool render = true;   // draw this iteration
    int wait_ms = 0;      // when not drawing: wait for an event for at most this long (1..kPumpMs)
};

inline Decision decide(const Inputs& in) {
    Decision d;
    if (!in.enabled || !in.background || in.busy || in.input || in.dirty) return d;   // draw now
    const unsigned long long since_act = in.now_ms >= in.last_activity_ms ? in.now_ms - in.last_activity_ms : 0;
    const int gap = since_act < kLingerMs ? kActiveGapMs : kIdleGapMs;
    const unsigned long long since_render = in.now_ms >= in.last_render_ms ? in.now_ms - in.last_render_ms : (unsigned long long)gap;
    if (since_render >= (unsigned long long)gap) return d;                              // due
    d.render = false;
    d.wait_ms = (int)std::min<unsigned long long>((unsigned long long)kPumpMs, (unsigned long long)gap - since_render);
    if (d.wait_ms < 1) d.wait_ms = 1;
    return d;
}

/* Is the hub "in the background" for the throttle? A focused, visible window with no game session is the only foreground. */
inline bool is_background(bool has_focus, bool shown, bool game_session) { return !has_focus || !shown || game_session; }

/* Milliseconds still to pad after an iteration that took `elapsed_ms` (woken early by noise events), so pumps never spin. */
inline int pad_ms(unsigned long long elapsed_ms) { return elapsed_ms >= (unsigned long long)kMinIterMs ? 0 : kMinIterMs - (int)elapsed_ms; }

}  // namespace hubidle
