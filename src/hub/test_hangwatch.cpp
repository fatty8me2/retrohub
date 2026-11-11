/* hub_hangwatch_test: the Lichii/Fritchii exit-hang watchdog decision (pure logic, no Win32, no processes). */
#include "hang_watch_logic.h"

#include <cstdio>

using namespace hubhang;

static int g_fail = 0;
#define CHECK(c) do { if (c) printf("  ok   %s\n", #c); else { g_fail++; printf("  FAIL %s\n", #c); } } while (0)

/* Drives a watch in 1 s steps from t0; returns the first time (ms) at which it said End, or 0 if never before `until`. */
struct Sim {
    HangWatch w;
    uint64_t cpu = 0, io = 0;
    uint64_t end_at = 0;
    Why end_why = Why::None;
    void step(uint64_t now, bool window) {
        if (w.tick(now, window, window ? 0 : cpu, window ? 0 : io) == Verdict::End && !end_at) { end_at = now; end_why = w.why; }
    }
};

int main()
{
    const uint64_t T0 = 1000000;
    // window never closes: never ends
    {
        Sim s;
        for (uint64_t t = T0; t < T0 + 600000; t += 1000) s.step(t, true);
        CHECK(s.end_at == 0);
    }
    // never had a window (still starting up): never ends, however long
    {
        Sim s;
        for (uint64_t t = T0; t < T0 + 600000; t += 1000) s.step(t, false);
        CHECK(s.end_at == 0);
    }
    // window closes, process completely quiet: ends at 15 s (not before)
    {
        Sim s;
        s.step(T0, true);
        for (uint64_t t = T0 + 1000; t < T0 + 60000; t += 1000) s.step(t, false);   // gone_since = T0 + 1000
        CHECK(s.end_at == T0 + 1000 + 15000);
        CHECK(s.end_why == Why::Quiet);
    }
    // not before 15 s even with a long-quiet history: 14.999 s waits, 15.000 s ends
    {
        HangWatch w;
        w.tick(T0, true, 0, 0);
        CHECK(w.tick(T0 + 1, false, 5, 5) == Verdict::Wait);
        CHECK(w.tick(T0 + 1 + 14999, false, 5, 5) == Verdict::Wait);
        CHECK(w.tick(T0 + 1 + 15000, false, 5, 5) == Verdict::End && w.why == Why::Quiet);
    }
    // steady CPU activity keeps it alive until the 180 s cap
    {
        Sim s;
        s.step(T0, true);
        for (uint64_t t = T0 + 1000; t < T0 + 400000; t += 1000) { s.cpu += 3000000; s.step(t, false); }
        CHECK(s.end_at == T0 + 1000 + 180000);
        CHECK(s.end_why == Why::Cap);
    }
    // I/O-only activity (cache being written) keeps it alive until the cap
    {
        Sim s;
        s.step(T0, true);
        for (uint64_t t = T0 + 1000; t < T0 + 400000; t += 1000) { s.io += 100000; s.step(t, false); }
        CHECK(s.end_at == T0 + 1000 + 180000);
        CHECK(s.end_why == Why::Cap);
    }
    // activity below the thresholds does not count as busy
    {
        Sim s;
        s.step(T0, true);
        s.step(T0 + 1000, false);
        s.cpu += 2000000; s.io += 65536;   // exactly the threshold: not "more than"
        for (uint64_t t = T0 + 2000; t < T0 + 60000; t += 1000) s.step(t, false);
        CHECK(s.end_at == T0 + 1000 + 15000);
    }
    // activity that stops: ends 10 s after the last busy tick (once past the 15 s grace)
    {
        Sim s;
        s.step(T0, true);
        uint64_t last_busy = 0;
        for (uint64_t t = T0 + 1000; t < T0 + 100000; t += 1000) {
            if (t < T0 + 40000) { s.cpu += 3000000; last_busy = t; }
            s.step(t, false);
        }
        CHECK(s.end_at == last_busy + 10000);
        CHECK(s.end_why == Why::Quiet);
    }
    // activity that stops inside the grace: still waits for 15 s
    {
        Sim s;
        s.step(T0, true);
        for (uint64_t t = T0 + 1000; t < T0 + 60000; t += 1000) {
            if (t < T0 + 3000) s.io += 1000000;
            s.step(t, false);
        }
        CHECK(s.end_at == T0 + 1000 + 15000);
    }
    // slow trickle accumulates across ticks past the threshold (baseline moves only when counted)
    {
        Sim s;
        s.step(T0, true);
        s.step(T0 + 1000, false);
        for (uint64_t t = T0 + 2000; t < T0 + 100000; t += 1000) {
            if (t < T0 + 30000) s.io += 10000;
            s.step(t, false);
        }
        CHECK(s.end_at != 0 && s.end_at < T0 + 1000 + 180000);
    }
    // window reappears: state resets; the 15 s grace restarts from the second disappearance
    {
        Sim s;
        s.step(T0, true);
        for (uint64_t t = T0 + 1000; t < T0 + 10000; t += 1000) s.step(t, false);
        CHECK(s.end_at == 0);
        s.step(T0 + 10000, true);                      // back
        for (uint64_t t = T0 + 11000; t < T0 + 20000; t += 1000) s.step(t, true);
        s.step(T0 + 20000, false);                     // gone again: grace restarts here
        for (uint64_t t = T0 + 21000; t < T0 + 35000; t += 1000) s.step(t, false);
        CHECK(s.end_at == 0);                          // 15 s not yet reached (gone_since = T0 + 20000)
        s.step(T0 + 35000, false);
        CHECK(s.end_at == T0 + 35000);
    }
    // counter read failure at the first gone tick (reads as 0): the first good read counts as one burst of activity, as in the original
    {
        Sim s;
        s.step(T0, true);
        s.cpu = 0; s.io = 0; s.step(T0 + 1000, false);          // baseline 0
        s.cpu = 50000000; s.io = 900000000;                     // reads start working: big jump
        s.step(T0 + 12000, false);                              // counts busy, idle_since = T0 + 12000
        for (uint64_t t = T0 + 13000; t < T0 + 60000; t += 1000) s.step(t, false);
        CHECK(s.end_at == T0 + 22000);
    }
    // counter failing mid-way (reads 0 after real values): never counts as busy, so a quiet process still ends on time
    {
        Sim s;
        s.step(T0, true);
        s.cpu = 50000000; s.io = 900000000; s.step(T0 + 1000, false);
        s.cpu = 0; s.io = 0;
        for (uint64_t t = T0 + 2000; t < T0 + 60000; t += 1000) s.step(t, false);
        CHECK(s.end_at == T0 + 1000 + 15000);
    }
    // counters failing for the whole session: ends at 15 s
    {
        Sim s;
        s.step(T0, true);
        for (uint64_t t = T0 + 1000; t < T0 + 60000; t += 1000) s.step(t, false);
        CHECK(s.end_at == T0 + 1000 + 15000);
    }
    // after an End the watch is disarmed (needs a new window before it can end again)
    {
        HangWatch w;
        w.tick(T0, true, 0, 0);
        w.tick(T0 + 1, false, 0, 0);
        CHECK(w.tick(T0 + 20000, false, 0, 0) == Verdict::End);
        CHECK(w.tick(T0 + 40000, false, 0, 0) == Verdict::Wait);
    }
    // reset() (watch switched off) disarms too
    {
        HangWatch w;
        w.tick(T0, true, 0, 0);
        w.reset();
        for (uint64_t t = T0 + 1000; t < T0 + 100000; t += 1000) CHECK(w.tick(t, false, 0, 0) == Verdict::Wait);
    }
    // large tick-count values (uptime of weeks) are fine: only differences are used, 64-bit
    {
        Sim s;
        const uint64_t big = 40ull * 24 * 3600 * 1000;   // 40 days of uptime, past 32-bit wrap (49.7 d) soon after
        s.step(big, true);
        s.step(big + 1000, false);
        for (uint64_t t = big + 2000; t < big + 60000; t += 1000) s.step(t, false);
        CHECK(s.end_at == big + 1000 + 15000);
        Sim s2;
        const uint64_t past32 = 0x100000000ull - 5000;   // GetTickCount64 never wraps: crossing 2^32 must not matter
        s2.step(past32, true);
        for (uint64_t t = past32 + 1000; t < past32 + 60000; t += 1000) s2.step(t, false);
        CHECK(s2.end_at == past32 + 1000 + 15000);
    }

    printf(g_fail ? "FAILED (%d)\n" : "all ok\n", g_fail);
    return g_fail ? 1 : 0;
}
