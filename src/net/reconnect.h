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

} // namespace hubnet
