#pragma once
/* WsReconnect: the pure state machine behind "a game-room WebSocket blip must not end a running match".
 *
 * A Session whose lobby socket dies AFTER the game became ready keeps its UDP path and tries to get the socket back for a
 * while. The server holds the dropped host's slot for a short grace (about 10 s, hub.js HOST_GRACE_MS), so the reconnect of the
 * same host reclaims it; a guest's old socket may not be reaped yet, so room-in-use / bad-slot / room-full / host-reconnecting
 * are all "try again shortly" answers while the window is open. No clocks inside: callers pass now_ms. */
#include <cstdint>
#include <string>

namespace hubnet {

class WsReconnect {
  public:
    static constexpr uint32_t kWindowMs = 25000;     // give up reconnecting this long after the blip (UDP-only from then on)
    static constexpr uint32_t kFirstDelayMs = 400;
    static constexpr uint32_t kMaxDelayMs = 2000;

    /* Server answers that mean "not yet, retry" during a reconnect. */
    static bool retryable_reason(const std::string& reason)
    {
        return reason == "room-in-use" || reason == "bad-slot" || reason == "room-full" || reason == "host-reconnecting" ||
               reason == "no-such-room";
    }
    /* Close codes that must NOT start a reconnect: 1008 = the server cut this socket for flooding the relay (a retry would loop). */
    static bool reconnectable_close(int code) { return code != 1008; }

    void reset() { *this = WsReconnect(); }
    bool active() const { return active_; }
    uint32_t blip_ms() const { return blip_at_; }
    int attempts() const { return attempts_; }

    /* The socket was lost (or refused with a retryable reason). Idempotent while a reconnect is already running. */
    void blip(uint32_t now)
    {
        if (active_) return;
        active_ = true;
        blip_at_ = now;
        next_at_ = now + kFirstDelayMs;
        attempts_ = 0;
        gave_up_ = false;
    }
    bool expired(uint32_t now) const { return active_ && (int32_t)(now - blip_at_) >= (int32_t)kWindowMs; }
    /* Time to open a new socket? false once the window ran out (then gave_up() is true). */
    bool should_retry(uint32_t now)
    {
        if (!active_ || gave_up_) return false;
        if (expired(now)) { gave_up_ = true; return false; }
        return (int32_t)(now - next_at_) >= 0;
    }
    /* A new socket was opened at `now`: schedule the next try with a growing, capped delay. */
    void attempted(uint32_t now)
    {
        attempts_++;
        uint32_t d = kFirstDelayMs << (attempts_ < 3 ? attempts_ : 3);
        if (d > kMaxDelayMs) d = kMaxDelayMs;
        next_at_ = now + d;
    }
    bool gave_up() const { return gave_up_; }
    /* welcome received: the blip is over. */
    void welcomed() { reset(); }

  private:
    bool active_ = false, gave_up_ = false;
    uint32_t blip_at_ = 0, next_at_ = 0;
    int attempts_ = 0;
};

/* Peer bookkeeping for "left the room, but is its UDP still alive?". `ws_gone_at` != 0 marks a member that vanished from the
 * server's roster; it only really counts as departed once its direct path went stale too. */
inline bool member_departed(bool ws_gone, bool udp_fresh) { return ws_gone && !udp_fresh; }

/* After a guest's session gave up ("lost the host"), ask the server whether the host is really gone: a crashed host's seat is held
 * for a grace period (hub.js HOST_GRACE_MS ~10 s), and the server's verdict ("host-left-game" / close 4001) arrives about when the
 * guest's own silence timer runs out. The guest then wants to say "the host left" (exit 8), not "connection lost" (exit 4).
 * Pure rules; the caller measures time and reports what arrived. */
enum class HostVerdict { Wait, HostLeft, Lost };
constexpr uint32_t kVerdictAckMs = 1500;   // no word at all from the server this long: our own link is down too
struct VerdictInput {
    bool host_left_signal = false;   // error host-left-game, a close with code 4001, or a members list without slot 0
    bool socket_dead_other = false;  // the lobby socket failed with any other close
    bool any_text_seen = false;      // keepalive_ack or any other text frame arrived since we asked
    uint32_t elapsed_ms = 0;         // since the question was sent
    uint32_t max_ms = 0;             // the caller's total budget
};
inline HostVerdict host_verdict(const VerdictInput& in)
{
    if (in.host_left_signal) return HostVerdict::HostLeft;
    if (in.socket_dead_other) return HostVerdict::Lost;
    if (in.elapsed_ms >= in.max_ms) return HostVerdict::Lost;
    if (!in.any_text_seen && in.elapsed_ms >= kVerdictAckMs) return HostVerdict::Lost;
    return HostVerdict::Wait;
}

} // namespace hubnet
