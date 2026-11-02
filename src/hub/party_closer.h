#pragma once
/* Party mode (Plan 4 P1): the "round is over, close the game" sequence, as a pure state machine so its ORDER and TIMING are tested
 * (test_party.cpp) without a runner:
 *   1. OsdMsg    "Round 2 over! Mia wins (+3)"      (a 2.5 s toast)
 *   2. OsdCorner the standings, kept for a few seconds   (runners that don't know it ack ok:false "unsupported": fall back to a
 *                second OsdMsg with the standings)
 *   3. hold      so the players can read it
 *   4. Quit      through the control channel (graceful: saves flush exactly as on a normal exit)
 *   5. WindowClose  (request_game_close) if the game is still running quit_wait_ms after Quit - never a kill
 * The control file holds ONE command at a time and the runner polls it about 4 times a second, so each command waits for the
 * previous one's ack (or 600 ms) before the next is written, so a toast is never replaced by the next command before the runner
 * read it (send_command_ex only protects a Quit that is not acked yet, so Quit goes last). A game without a control channel (the native Smash port) skips to
 * the hold and then gets WindowClose.
 *
 * The caller (party_ui.cpp) owns the I/O: it performs the CloseCmd it gets from step(), reports the seq runctl returned with
 * sent(), and passes the latest ack it read from control_ack.json. */
#include <cstdint>
#include <string>

namespace hubui {
namespace party {

struct CloseCmd {
    enum Kind { None, Msg, Corner, Quit, WindowClose } kind = None;
    std::string text;
    int corner = 1;      // OsdCorner: 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right
    int keep_secs = 0;   // OsdCorner: seconds the runner keeps the text
};

struct AckInfo {
    long long seq = 0;   // 0 = no ack file / nothing acked yet
    bool ok = false;
    std::string msg;
};

class Closer {
  public:
    struct Plan {
        bool has_ctl = false;                 // the runner has a control channel (A.run.ctl_path)
        std::string msg, corner, fallback;    // toast, corner text, toast with the standings for runners without OsdCorner
        int hold_ms = 6000;                   // how long the standings stay up before Quit
        int hold_ms_noctl = 4000;             // no OSD: a short pause (the game's own results screen is up) before the window is closed
        int ack_wait_ms = 600;                // wait for the previous command's ack (or this long: the runner polls 4x a second) before the next one
        int quit_wait_ms = 12000;             // Quit not honoured by then -> WindowClose
        int give_up_ms = 25000;               // nothing more after WindowClose
    };

    void start(uint64_t now_ms, const Plan& p) {
        plan_ = p;
        t0_ = now_ms;
        state_ = p.has_ctl ? NeedMsg : Hold;
        seq_msg_ = seq_corner_ = seq_quit_ = 0;
        unsupported_ = false;
        t_state_ = now_ms;
    }
    bool active() const { return state_ != Idle; }
    bool corner_unsupported() const { return unsupported_; }
    void finish() { state_ = Idle; }          // the game ended (or the party was closed)
    /* Records the seq runctl::send_command_ex returned for the command step() just handed out (0 = it could not be written). */
    void sent(long long seq) {
        if (state_ == WaitMsg) seq_msg_ = seq;
        else if (state_ == WaitCorner) seq_corner_ = seq;
        else if (state_ == WaitFallback) {}
        else if (state_ == WaitQuit) seq_quit_ = seq;
    }

    CloseCmd step(uint64_t now_ms, const AckInfo& ack) {
        CloseCmd c;
        const uint64_t in_state = now_ms - t_state_;
        auto got = [&](long long seq) { return seq > 0 && ack.seq >= seq; };
        switch (state_) {
            case Idle: break;
            case NeedMsg:
                c.kind = CloseCmd::Msg; c.text = plan_.msg;
                go(WaitMsg, now_ms);
                break;
            case WaitMsg:
                if (seq_msg_ == 0 || got(seq_msg_) || in_state >= (uint64_t)plan_.ack_wait_ms) go(NeedCorner, now_ms);
                break;
            case NeedCorner:
                c.kind = CloseCmd::Corner; c.text = plan_.corner; c.corner = 1;
                c.keep_secs = plan_.hold_ms / 1000 + 4;
                go(WaitCorner, now_ms);
                break;
            case WaitCorner:
                if (got(seq_corner_)) { if (!ack.ok) { unsupported_ = true; go(NeedFallback, now_ms); } else go(Hold, now_ms); }
                else if (seq_corner_ == 0 || in_state >= (uint64_t)plan_.ack_wait_ms) go(Hold, now_ms);
                break;
            case NeedFallback:
                c.kind = CloseCmd::Msg; c.text = plan_.fallback;
                go(WaitFallback, now_ms);
                break;
            case WaitFallback:
                go(Hold, now_ms);
                break;
            case Hold: {
                const uint64_t hold = plan_.has_ctl ? (uint64_t)plan_.hold_ms : (uint64_t)plan_.hold_ms_noctl;
                if (now_ms - t0_ >= hold) go(plan_.has_ctl ? NeedQuit : NeedWmClose, now_ms);
                break;
            }
            case NeedQuit:
                c.kind = CloseCmd::Quit;
                go(WaitQuit, now_ms);
                break;
            case WaitQuit:
                if (in_state >= (uint64_t)plan_.quit_wait_ms) go(NeedWmClose, now_ms);
                break;
            case NeedWmClose:
                c.kind = CloseCmd::WindowClose;
                go(Final, now_ms);
                break;
            case Final:
                if (in_state >= (uint64_t)plan_.give_up_ms) state_ = Idle;
                break;
        }
        return c;
    }

  private:
    enum State { Idle, NeedMsg, WaitMsg, NeedCorner, WaitCorner, NeedFallback, WaitFallback, Hold, NeedQuit, WaitQuit, NeedWmClose, Final };
    void go(State s, uint64_t now_ms) { state_ = s; t_state_ = now_ms; }
    Plan plan_;
    State state_ = Idle;
    uint64_t t0_ = 0, t_state_ = 0;
    long long seq_msg_ = 0, seq_corner_ = 0, seq_quit_ = 0;
    bool unsupported_ = false;
};

}  // namespace party
}  // namespace hubui
