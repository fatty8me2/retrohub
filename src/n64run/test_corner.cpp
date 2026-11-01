// Console test: OsdCorner board (set / clear / expire / combined text).
#include <cstdio>
#include <string>

#include "corner.h"

using namespace n64run;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main() {
    CornerBoard b;
    CHECK(!b.any() && b.combined().empty());

    bool ch = false;
    CHECK(b.set(1, "P1 3  P2 1", 0, 1000, &ch) && ch);
    CHECK(b.any() && b.text(1) == "P1 3  P2 1" && b.combined() == "P1 3  P2 1");
    CHECK(b.set(1, "P1 3  P2 1", 0, 2000, &ch) && !ch);   // same text: nothing visible changed
    CHECK(!b.expire(1000000));                               // seconds 0 = until replaced: never expires

    // two corners: joined in corner order, whatever the order they were set in
    CHECK(b.set(0, "TIME 0:42", 0, 3000, &ch) && ch);
    CHECK(b.combined() == "TIME 0:42  |  P1 3  P2 1");

    // seconds: expires exactly when its time is up
    CHECK(b.set(3, "GO!", 5, 10000));
    CHECK(!b.expire(14999) && b.text(3) == "GO!");
    CHECK(b.expire(15000) && b.text(3).empty());
    CHECK(b.combined() == "TIME 0:42  |  P1 3  P2 1");

    // replacing a timed corner with an untimed text drops the timer
    CHECK(b.set(2, "A", 1, 20000));
    CHECK(b.set(2, "B", 0, 20500));
    CHECK(!b.expire(99999) && b.text(2) == "B");

    // empty text clears
    CHECK(b.set(2, "", 0, 30000, &ch) && ch && b.text(2).empty());
    CHECK(b.set(0, "", 0, 30000) && b.set(1, "", 0, 30000));
    CHECK(!b.any() && b.combined().empty());

    // bad corners
    CHECK(!b.set(-1, "x", 0, 0) && !b.set(4, "x", 0, 0));
    CHECK(b.text(7).empty() && !b.any());

    // length cap
    CHECK(b.set(1, std::string(500, 'x'), 0, 0));
    CHECK(b.text(1).size() == CornerBoard::kMaxText);

    // clear()
    b.set(0, "a", 3, 0); b.set(3, "b", 0, 0);
    b.clear();
    CHECK(!b.any() && !b.expire(1000000));

    std::printf(fails ? "corner test: %d FAILED\n" : "corner test: all passed\n", fails);
    return fails ? 1 : 0;
}
