#pragma once
/* Home page scrolling (pure logic, no ImGui): which axis a wheel / touchpad event drives, and the eased page scroll.
 *
 * Before: any wheel event over an overflowing shelf row was turned into a HORIZONTAL scroll of that row, so the page stalled
 * ("caught") whenever the pointer crossed a row. Now:
 *   - a vertical wheel / touchpad gesture always scrolls the PAGE, wherever the pointer is;
 *   - a row scrolls sideways only for a horizontal wheel / touchpad gesture, or Shift + wheel, while the pointer is over it;
 *   - a diagonal touchpad gesture takes its dominant axis and keeps it for the rest of the gesture (axis lock).
 * Conventions: SDL wheel (wx > 0 = right, wy > 0 = up). Route::page > 0 scrolls the page DOWN (scroll offset grows),
 * Route::row > 0 scrolls the row to the RIGHT (scroll offset grows); both in wheel notches. */

#include <cmath>

namespace hubui {
namespace hs {

struct Route {
    float page = 0.f;   /* notches: + = page scrolls down */
    float row = 0.f;    /* notches: + = row scrolls right (only ever non-zero when the pointer is over a row) */
};

class WheelRouter {
public:
    static constexpr double kLockS = 0.18;   /* a gesture keeps its axis until the wheel has been quiet this long */

    Route route(float wx, float wy, bool shift, bool over_row, double now) {
        Route r;
        if (wx == 0.f && wy == 0.f) return r;
        const bool fresh = last_t_ < 0 || now - last_t_ > kLockS;
        last_t_ = now;
        if (shift && over_row) {   /* Shift + wheel pans the row under the pointer (wheel up = left) */
            axis_ = kHoriz;
            r.row = wx - wy;
            return r;
        }
        if (wy == 0.f) axis_ = kHoriz;        /* a pure event always follows its own axis (a mouse wheel never gets locked sideways) */
        else if (wx == 0.f) axis_ = kVert;
        else if (fresh) axis_ = std::fabs(wx) > std::fabs(wy) ? kHoriz : kVert;   /* diagonal: dominant axis wins, then holds */
        if (axis_ == kHoriz) {
            if (over_row) r.row = wx;   /* sideways off a row does nothing: never a page jitter */
        } else {
            r.page = -wy;
        }
        return r;
    }
    void reset() { last_t_ = -1; axis_ = kVert; }

private:
    enum { kVert = 1, kHoriz = 2 };
    int axis_ = kVert;
    double last_t_ = -1;
};

/* Pixels one wheel notch scrolls the page (a few rows' worth; `unit` is the UI scale unit sp(1)). */
inline float notch_px(float unit) { return unit * 10.f; }

/* Critically damped spring toward `target` (exact solution, so the result does not depend on the frame rate). `vel` keeps the
 * motion going between frames: the page eases out and carries a little momentum instead of stopping dead. */
inline void spring_step(float& pos, float& vel, float target, float dt, float omega = 15.f) {
    if (dt <= 0.f) return;
    if (dt > 0.1f) dt = 0.1f;
    const float d = pos - target;
    const float e = std::exp(-omega * dt);
    const float t = (vel + omega * d) * dt;
    pos = target + (d + t) * e;
    vel = (vel - omega * t) * e;
    if (std::fabs(pos - target) < 0.35f && std::fabs(vel) < 4.f) { pos = target; vel = 0.f; }
}

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace hs
}  // namespace hubui
