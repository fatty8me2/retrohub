// "Larger small text": font_px() sizes.
#include "font_size.h"
#include <cmath>
#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
static bool eq(float a, float b) { return std::fabs(a - b) < 1e-4f; }

int main() {
    using hubfont::font_px;
    // off: exactly base * scale (today's look)
    CHECK(eq(font_px(11, 1.f, false), 11.f));
    CHECK(eq(font_px(15, 1.5f, false), 22.5f));
    CHECK(eq(font_px(19, 2.f, false), 38.f));
    // on: small faces floored, scaled with UI scale
    CHECK(eq(font_px(11, 1.f, true), 14.f));
    CHECK(eq(font_px(15, 1.f, true), 18.f));
    CHECK(eq(font_px(11, 1.5f, true), 21.f));
    CHECK(eq(font_px(15, 2.f, true), 36.f));
    // bigger faces never change
    CHECK(eq(font_px(19, 1.f, true), 19.f));
    CHECK(eq(font_px(24, 1.25f, true), 30.f));
    CHECK(eq(font_px(84, 1.f, true), 84.f));
    // never smaller than base
    CHECK(font_px(14, 1.f, true) >= 14.f);
    if (fails) { std::printf("%d failures\n", fails); return 1; }
    std::printf("font_size ok\n");
    return 0;
}
