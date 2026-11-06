/* hub_iconrefresh_test: the once-per-version icon refresh policy (pure; never touches the shell). */
#include "icon_refresh_logic.h"

#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (c) printf("  ok   %s\n", #c); else { g_fail++; printf("  FAIL %s\n", #c); } } while (0)

int main()
{
    using namespace hubicon;
    CHECK(refresh_due("", "1.31.11"));            // never refreshed
    CHECK(refresh_due("1.31.10", "1.31.11"));     // updated
    CHECK(!refresh_due("1.31.11", "1.31.11"));    // already done for this version
    CHECK(!refresh_due("", ""));                  // unknown running version: do nothing

    int runs = 0;
    std::string rec;
    auto ok = [&] { ++runs; return true; };
    auto bad = [&] { ++runs; return false; };

    CHECK(step("1.31.10", "1.31.11", true, false, ok, &rec) && runs == 1 && rec == "1.31.11");   // version changed: refresh once
    CHECK(!step(rec, "1.31.11", true, false, ok, &rec) && runs == 1);                             // same version again: not again

    std::string r2;
    CHECK(!step("", "1.31.11", true, /*busy=*/true, ok, &r2) && runs == 1 && r2.empty());         // a game is running: wait, record nothing
    CHECK(step("", "1.31.11", true, false, ok, &r2) && runs == 2 && r2 == "1.31.11");              // later, idle: it runs
    CHECK(!step("", "1.31.11", /*allowed=*/false, false, ok, &r2 /*unchanged*/) && runs == 2);     // tests / headless / dev build: never

    std::string r3;
    CHECK(!step("", "1.31.11", true, false, bad, &r3) && runs == 3 && r3.empty());                 // failed: not recorded, retried next start
    CHECK(step("1.31.11", "1.31.12", true, false, ok, &r3) && r3 == "1.31.12");                    // next release refreshes again

    printf(g_fail ? "FAILED (%d)\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
