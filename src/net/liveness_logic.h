#pragma once
/* Socket liveness (SO-F6): rides the existing keepalive. The client pings every ping_ms; ANY frame received (the server answers
 * keepalive with keepalive_ack) refreshes last_rx. If nothing arrived for dead_ms the socket is considered dead (PC slept, Wi-Fi
 * dropped without a close) and the caller closes it and reconnects with the normal backoff. Times are uint32 ms (wrap safe). */
#include <cstdint>

namespace hubnet {

struct Liveness {
    uint32_t ping_ms = 20000, dead_ms = 60000;
    uint32_t last_rx = 0, last_ping = 0;
    bool armed = false;
    void reset(uint32_t now) { last_rx = last_ping = now; armed = true; }
    void disarm() { armed = false; }
    void on_rx(uint32_t now) { last_rx = now; }
    bool ping_due(uint32_t now) const { return armed && now - last_ping >= ping_ms; }
    void pinged(uint32_t now) { last_ping = now; }
    /* A long gap between update() calls (the whole PC was asleep) shows up as a huge now - last_rx: dead as well. */
    bool dead(uint32_t now) const { return armed && now - last_rx >= dead_ms; }
};

} // namespace hubnet
