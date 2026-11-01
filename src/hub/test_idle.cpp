// Console test for the idle throttle decision (idle_logic.h): when the hub's loop may skip drawing a frame, and how long it then waits.
// The pumps (sockets, drivers, pad watchers) run every loop iteration regardless; only drawing is thinned.
#include "idle_logic.h"

#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

using namespace hubidle;

static Inputs bg(unsigned long long now, unsigned long long last_render, unsigned long long last_act) {
    Inputs i;
    i.background = true;
    i.now_ms = now; i.last_render_ms = last_render; i.last_activity_ms = last_act;
    return i;
}

int main() {
    // a focused hub with no game is never throttled, however static the UI is
    {   Inputs i; i.now_ms = 10000; i.last_render_ms = 9999; i.last_activity_ms = 0;
        CHECK(decide(i).render);
        i.enabled = false; i.background = true; CHECK(decide(i).render);   // switched off (HUB_IDLE_THROTTLE=0, QA drivers)
    }
    // background and static: draws about every 100 ms, waits in between (never longer than the pump interval)
    {   Inputs i = bg(10000, 9950, 0);   // 50 ms since the last frame, no recent activity
        Decision d = decide(i);
        CHECK(!d.render && d.wait_ms >= 1 && d.wait_ms <= kPumpMs);
        i.now_ms = 10100;                 // 150 ms: due
        CHECK(decide(i).render);
        i.now_ms = 10049; i.last_render_ms = 9950;   // 99 ms
        CHECK(!decide(i).render);
        i.now_ms = 10050;                 // exactly 100 ms
        CHECK(decide(i).render);
    }
    // the wait is clamped: 90 ms to go still waits at most kPumpMs, 3 ms to go waits 3
    {   Decision d = decide(bg(10010, 10000, 0));
        CHECK(!d.render && d.wait_ms == kPumpMs);
        d = decide(bg(10097, 10000, 0));
        CHECK(!d.render && d.wait_ms == 3);
    }
    // recent activity (input or a visible change): capped at about 30 fps instead
    {   Inputs i = bg(10000, 9990, 9900);      // 100 ms since activity < linger, 10 ms since the last frame
        Decision d = decide(i);
        CHECK(!d.render && d.wait_ms <= kPumpMs);
        i.now_ms = 10023;                        // 33 ms since the last frame
        CHECK(decide(i).render);
        i.now_ms = 10022; CHECK(!decide(i).render);
        // after the linger the cadence relaxes to 10 fps
        Inputs j = bg(10500, 10450, 10000);      // 500 ms since activity
        CHECK(!decide(j).render);                // only 50 ms since the last frame
        j.now_ms = 10550; CHECK(decide(j).render);
    }
    // input, a repaint request, or anything busy draws at once, even in the background
    {   Inputs i = bg(10000, 9999, 0);
        CHECK(!decide(i).render);
        i.input = true; CHECK(decide(i).render);
        i.input = false; i.dirty = true; CHECK(decide(i).render);
        i.dirty = false; i.busy = true; CHECK(decide(i).render);    // boot animation, toasts, the View+Y menu, a netplay driver session
    }
    // clock quirks never stall the UI: a render time in the future or an activity time in the future
    {   Inputs i = bg(5000, 9000, 0);
        CHECK(decide(i).render);                  // last_render in the future: draw
        Inputs j = bg(5000, 1000, 9000);          // activity in the future counts as "just now"
        Decision d = decide(j);
        CHECK(d.render);                          // 4000 ms since the last frame
    }
    // the first frame (nothing drawn yet) is always drawn
    CHECK(decide(bg(10000, 0, 0)).render);
    // background = not focused, hidden, or a game session is running
    CHECK(!is_background(true, true, false));
    CHECK(is_background(false, true, false) && is_background(true, false, false) && is_background(true, true, true));
    // padding: an iteration woken early by noise is padded to the floor, a slow one is not
    CHECK(pad_ms(0) == kMinIterMs && pad_ms(1) == kMinIterMs - 1 && pad_ms(kMinIterMs) == 0 && pad_ms(500) == 0);
    // the pumps keep their cadence: the longest wait is the pump interval, i.e. about 60 Hz or better
    CHECK(kPumpMs <= 17 && kIdleGapMs >= 50 && kActiveGapMs < kIdleGapMs);
    // simulated second of a static background hub: it draws about 10 frames, and the loop iterates at least 60 times
    {   unsigned long long now = 20000, last_render = 20000, draws = 0, iters = 0;
        const unsigned long long end = now + 1000;
        while (now < end) {
            ++iters;
            Decision d = decide(bg(now, last_render, 0));
            if (d.render) { ++draws; last_render = now; now += 1; }
            else now += (unsigned long long)d.wait_ms;
        }
        CHECK(draws >= 8 && draws <= 12);
        CHECK(iters >= 60);
    }
    std::printf(fails ? "hub_idle_test FAILED\n" : "hub_idle_test OK\n");
    return fails ? 1 : 0;
}
