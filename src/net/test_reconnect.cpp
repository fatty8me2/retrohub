/* hubreconnect_test: the WebSocket reconnect state machine (offline, no server). */
#include "reconnect.h"

#include <cstdio>

using namespace hubnet;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main()
{
    WsReconnect r;
    CHECK(!r.active());
    CHECK(!r.should_retry(1000));

    r.blip(1000);
    CHECK(r.active());
    CHECK(!r.should_retry(1100));                       // first try waits kFirstDelayMs
    CHECK(r.should_retry(1000 + WsReconnect::kFirstDelayMs));
    r.attempted(1400);
    CHECK(!r.should_retry(1500));                       // backoff
    r.blip(1450);                                       // idempotent: a second failure does not restart the window
    CHECK(r.blip_ms() == 1000);

    uint32_t t = 1400, last_gap = 0;
    for (int i = 0; i < 6; i++) {                       // delays grow but stay capped
        uint32_t before = t;
        while (!r.should_retry(t)) t += 10;
        last_gap = t - before;
        r.attempted(t);
    }
    CHECK(last_gap <= WsReconnect::kMaxDelayMs + 10);
    CHECK(last_gap >= WsReconnect::kMaxDelayMs - 10);

    CHECK(!r.expired(1000 + WsReconnect::kWindowMs - 1));
    CHECK(r.expired(1000 + WsReconnect::kWindowMs));
    CHECK(!r.should_retry(1000 + WsReconnect::kWindowMs + 5000));
    CHECK(r.gave_up());

    r.welcomed();
    CHECK(!r.active() && !r.gave_up() && r.attempts() == 0);

    // a later blip starts a fresh window
    r.blip(100000);
    CHECK(r.active() && r.blip_ms() == 100000);
    CHECK(r.should_retry(100000 + WsReconnect::kFirstDelayMs));

    // wrap-around safe
    WsReconnect w;
    w.blip(0xFFFFFF00u);
    CHECK(!w.expired(0xFFFFFF00u + 1000));
    CHECK(w.should_retry(0xFFFFFF00u + 500));
    CHECK(w.expired(0xFFFFFF00u + WsReconnect::kWindowMs));

    // reasons / codes
    CHECK(WsReconnect::retryable_reason("room-in-use"));
    CHECK(WsReconnect::retryable_reason("bad-slot"));
    CHECK(WsReconnect::retryable_reason("host-reconnecting"));
    CHECK(!WsReconnect::retryable_reason("spectators-full"));
    CHECK(!WsReconnect::retryable_reason("server-busy"));
    CHECK(WsReconnect::reconnectable_close(1006));
    CHECK(WsReconnect::reconnectable_close(4000));
    CHECK(!WsReconnect::reconnectable_close(1008));

    // departed rule: gone from the roster but UDP alive -> still a member
    CHECK(!member_departed(true, true));
    CHECK(member_departed(true, false));
    CHECK(!member_departed(false, false));

    printf(fails ? "hubreconnect_test: %d FAILED\n" : "hubreconnect_test: all ok\n", fails);
    return fails ? 1 : 0;
}
