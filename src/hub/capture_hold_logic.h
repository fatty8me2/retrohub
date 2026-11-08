#pragma once
/* "Press a button" dialog on the Controllers page: how a controller user cancels (pure logic, unit tested by hub_launchset_test).
 * B (and Back) may be the very button being assigned, so a short press is captured as usual and HOLDING B / Back for about a second
 * cancels instead. Every other button is captured at once. Mouse click and Esc still cancel. */
#include <string>

namespace hubui {

constexpr double kCaptureHoldSecs = 1.0;
constexpr double kCaptureGraceSecs = 0.1;   // the UI key state can lag the poll by a frame: do not read that as "released"

inline bool capture_cancel_candidate(const std::string& tok) { return tok == "b" || tok == "back"; }

struct CaptureHold {
    bool pending = false;   // a B / Back press is waiting to be told apart: tap (assign it) or hold (cancel)
    double t0 = 0;
    std::string tok;
};
enum class CaptureStep { Wait, Commit, Cancel };

/* Call once per frame. `tok` = what the capture poll returned this frame (empty = nothing new), `still_held` = the pending button
 * is still down, `now` = seconds. On Commit the caller assigns h.tok (it is kept); on Cancel it closes the dialog. */
inline CaptureStep capture_hold_step(CaptureHold& h, const std::string& tok, bool still_held, double now) {
    if (!h.pending) {
        if (tok.empty()) return CaptureStep::Wait;
        h.tok = tok;
        if (!capture_cancel_candidate(tok)) return CaptureStep::Commit;
        h.pending = true;
        h.t0 = now;
        return CaptureStep::Wait;
    }
    if (!still_held && now - h.t0 >= kCaptureGraceSecs) { h.pending = false; return CaptureStep::Commit; }   // released early: a normal press
    if (now - h.t0 >= kCaptureHoldSecs) { h.pending = false; h.tok.clear(); return CaptureStep::Cancel; }
    return CaptureStep::Wait;
}

}  // namespace hubui
