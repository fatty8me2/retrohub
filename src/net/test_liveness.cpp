/* hubfriends_live_test: socket liveness rules (pure logic, no network). */
#include "liveness_logic.h"

#include <cstdio>

using namespace hubnet;

static int g_fail = 0;
#define CHECK(c) do { if (c) printf("  ok   %s\n", #c); else { g_fail++; printf("  FAIL %s\n", #c); } } while (0)

int main()
{
    Liveness l;
    CHECK(!l.dead(100000) && !l.ping_due(100000));   // disarmed: never dead (not connected)
    l.reset(1000);
    CHECK(!l.ping_due(1000 + 19999) && l.ping_due(1000 + 20000));
    l.pinged(21000);
    CHECK(!l.ping_due(21001));
    CHECK(!l.dead(1000 + 59999) && l.dead(1000 + 60000));   // nothing heard for 60 s
    l.on_rx(30000);   // any frame (keepalive_ack included) keeps it alive
    CHECK(!l.dead(30000 + 59999) && l.dead(30000 + 60000));
    l.on_rx(80000);
    CHECK(!l.dead(80000 + 20000));
    {   // the PC slept for an hour: the very next check says dead
        Liveness s; s.reset(5000);
        CHECK(s.dead(5000 + 3600000u));
    }
    {   // wrap-around of the 32-bit ms clock
        Liveness w; w.reset(0xFFFFFFF0u);
        CHECK(!w.dead(0xFFFFFFF0u + 1000u) && w.dead(0xFFFFFFF0u + 60000u));
        w.on_rx(5);   // wrapped
        CHECK(!w.dead(5 + 59000u) && w.dead(5 + 60000u));
    }
    l.disarm();
    CHECK(!l.dead(9999999));
    printf("%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
