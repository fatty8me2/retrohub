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

/* B (or Esc) ends the mode and keeps the ports set so far. */
inline bool assign_end_requested(bool pad_b_pressed, bool esc_pressed) { return pad_b_pressed || esc_pressed; }

}  // namespace hubui
