#pragma once
/* Gamepad-only decisions for the Controllers page and the Undo card (pure, no SDL / ImGui; unit tested by hub_launchset_test). */
#include <map>
#include <string>

namespace hubui {

/* Undo card (after "Remove from library"): the face-up button (Y) undoes while the card is visible and the hub owns the pad. Without a
 * card, or with the hub unfocused, a typing field open or a mouse / keyboard player (pad_mode off), the press is left to the screen
 * underneath. */
struct UndoPadIn {
    bool card_visible = false;
    bool pad_mode = false;
    bool input_blocked = false;   // the hub is unfocused / a game owns the pad
    bool text_input = false;
    bool y_pressed = false;       // rising edge this frame
};
inline bool undo_card_pad_press(const UndoPadIn& in) {
    return in.card_visible && in.pad_mode && !in.input_blocked && !in.text_input && in.y_pressed;
}

/* "Assign players": A on a pad claims the next port, B ends the mode. ImGui's own navigation would also act on that A (it presses the
 * focused Done / Start over button), so the page locks A away from navigation while the mode runs and claims only through this tracker. */
class AssignAGate {
  public:
    /* A button already held when the mode opens (or when a pad is plugged in): remembered, never a press. */
    void prime(const std::string& pad_key, bool a_down) { prev_[pad_key] = a_down; }
    void forget(const std::string& pad_key) { prev_.erase(pad_key); }
    bool known(const std::string& pad_key) const { return prev_.count(pad_key) != 0; }
    void clear() { prev_.clear(); }
    /* Once per pad per frame. True on the rising edge, unless `ui_activating` (a focused on-screen button is being activated with this
     * very press) or the pad was never primed (then it is primed now). */
    bool a_press(const std::string& pad_key, bool a_down, bool ui_activating) {
        auto it = prev_.find(pad_key);
        if (it == prev_.end()) { prev_[pad_key] = a_down; return false; }
        const bool edge = a_down && !it->second;
        it->second = a_down;
        return edge && !ui_activating;
    }
  private:
    std::map<std::string, bool> prev_;
};

/* One frame of "Assign players", decided without any UI. `lock_a` = keep A away from ImGui navigation this frame (ImGui keeps a key
 * owner while the key is down, so the page asks for it again every frame and never relies on it persisting). Every way out of the mode
 * (B, Esc, Done, all ports set, timeout, hub unfocused, page not shown) reports end + release_a, and an inactive mode always reports
 * release_a, so A can never stay dead for navigation. A B / Esc that ends the mode is swallowed so it does not also leave the page. */
struct AssignFrameIn {
    bool active = false;
    bool input_blocked = false;   // hub unfocused / a game owns the pad
    bool page_visible = true;     // the Controllers page is being drawn
    bool b_pressed = false, esc_pressed = false, done_clicked = false;
    bool all_ports_set = false, timed_out = false;
    bool y_pressed = false, pad_mode = false;
};
struct AssignFrameOut {
    bool end = false, lock_a = false, release_a = false, swallow_b = false, start_over = false;
};
inline AssignFrameOut assign_frame(const AssignFrameIn& in) {
    AssignFrameOut o;
    if (!in.active) { o.release_a = true; return o; }
    const bool back = in.b_pressed || in.esc_pressed;
    if (in.input_blocked || !in.page_visible || back || in.done_clicked || in.all_ports_set || in.timed_out) {
        o.end = true;
        o.release_a = true;
        o.swallow_b = back && !in.input_blocked && in.page_visible;
        return o;
    }
    o.lock_a = true;
    o.start_over = in.pad_mode && in.y_pressed;   // Start over has a pad route: Y
    return o;
}

}  // namespace hubui
