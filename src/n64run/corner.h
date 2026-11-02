#pragma once
/* OsdCorner (docs/RUNNER_CTL.md): persistent corner text the hub sets for party standings and challenge timers.
 * Pure logic, header only, unit tested in test_corner.cpp. The frontend keeps one CornerBoard behind a mutex and mirrors its text to
 * the OSD corner slot (osd::set_corner) and to the window caption.
 *
 * Corners: 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right. Until osd.h grows a slot per corner (osd::set_corner_at), the
 * runner has ONE corner slot (top-right): combined() joins the non-empty corners in corner order so nothing the hub sets is lost. */
#include <cstdint>
#include <string>

namespace n64run {

class CornerBoard {
  public:
    static constexpr int kCorners = 4;
    static constexpr size_t kMaxText = 120;   // the same cap as every control-file text

    /* Sets (text non-empty) or clears (text empty) a corner. seconds <= 0 keeps it until replaced, otherwise it expires after that long.
     * False for a corner outside 0..3. Returns through *changed whether the visible text differs from before. */
    bool set(int corner, const std::string& text, int seconds, uint64_t now_ms, bool* changed = nullptr) {
        if (corner < 0 || corner >= kCorners) return false;
        std::string t = text.size() > kMaxText ? text.substr(0, kMaxText) : text;
        const bool diff = text_[corner] != t;
        text_[corner] = t;
        until_[corner] = (!t.empty() && seconds > 0) ? now_ms + (uint64_t)seconds * 1000ull : 0;
        if (changed) *changed = diff;
        return true;
    }

    /* Drops texts whose time is up. True when a corner became empty. */
    bool expire(uint64_t now_ms) {
        bool any = false;
        for (int c = 0; c < kCorners; c++)
            if (until_[c] && now_ms >= until_[c]) { text_[c].clear(); until_[c] = 0; any = true; }
        return any;
    }

    void clear() { for (int c = 0; c < kCorners; c++) { text_[c].clear(); until_[c] = 0; } }

    const std::string& text(int corner) const { static const std::string none; return corner >= 0 && corner < kCorners ? text_[corner] : none; }
    bool any() const { for (int c = 0; c < kCorners; c++) if (!text_[c].empty()) return true; return false; }

    /* Everything on display, in corner order, joined with "  |  " ("" when nothing is set). */
    std::string combined() const {
        std::string out;
        for (int c = 0; c < kCorners; c++) {
            if (text_[c].empty()) continue;
            if (!out.empty()) out += "  |  ";
            out += text_[c];
        }
        return out;
    }

  private:
    std::string text_[kCorners];
    uint64_t until_[kCorners] = {0, 0, 0, 0};
};

}  // namespace n64run
