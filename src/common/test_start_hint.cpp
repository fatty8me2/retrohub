#include <cstdio>
#include <cstdlib>
#include <vector>

#include "exit_combo.h"
#include "start_hint.h"

using namespace starthint;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main() {
    CHECK(alpha_at(-1) == 0.f && !visible(-1));
    CHECK(alpha_at(0) == 0.f);
    CHECK(alpha_at(kFadeInMs / 2) > 0.4f * kMaxAlpha && alpha_at(kFadeInMs / 2) < 0.6f * kMaxAlpha);
    CHECK(alpha_at(kFadeInMs + 1000) == kMaxAlpha);
    CHECK(alpha_at(kTotalMs - 1) < 0.01f);
    CHECK(alpha_at(kTotalMs) == 0.f && finished(kTotalMs) && !visible(kTotalMs));
    CHECK(kTotalMs >= 4500 && kTotalMs <= 6000);
    CHECK(kMaxAlpha < 1.f);
    float prev = -1.f;
    for (int t = 0; t < kFadeInMs; t += 20) { float a = alpha_at(t); CHECK(a >= prev); prev = a; }

    auto r = rows(Mode::Pad);
    CHECK(r.size() == 2);
    CHECK(plain(r[0]) == "Hold View + Y - game menu");
    CHECK(plain(r[1]) == "Hold View + Menu - back to RetroHub");
    CHECK(plain(rows(Mode::Keyboard)[0]) == "Esc - back to RetroHub");
    CHECK(hubctl::kBtnBack == 4 && hubctl::kBtnStart == 6 && hubctl::kExitHoldMs == 1000);   // wording says "hold View + Menu"
    CHECK(pick_mode(true, true) == Mode::Keyboard);
    CHECK(pick_mode(true, false) == Mode::Pad);   // external emulators have no keyboard equivalent
    CHECK(pick_mode(false, true) == Mode::Pad);
    CHECK(scale_for_height(300) == 1.f && scale_for_height(1080) == 2.f && scale_for_height(8000) == 3.f);

#ifdef _WIN32
    Bitmap b = render(Mode::Pad, 1.f);
    CHECK(b.w > 120 && b.h > 40 && b.px.size() == (size_t)b.w * b.h);
    int opaque = 0;
    for (uint32_t p : b.px) if ((p >> 24) > 100) opaque++;
    CHECK(opaque > b.w * b.h / 3);
    CHECK((b.px[0] >> 24) < 40);   // rounded corner is transparent
    std::vector<uint32_t> fr(320 * 240, 0xFF000000u);
    composite(fr.data(), 320, 240, 320, b, 1.f);
    int x, y;
    placement(320, 240, b, &x, &y);
    CHECK(x >= 0 && y + b.h <= 240 && x + b.w <= 320);
    CHECK(fr[(size_t)(y + b.h / 2) * 320 + x + 3] != 0xFF000000u);
    CHECK(fr[0] == 0xFF000000u);
    std::vector<uint32_t> fr2(320 * 240, 0xFF000000u);
    composite(fr2.data(), 320, 240, 320, b, 0.f);
    CHECK(fr2 == std::vector<uint32_t>(320 * 240, 0xFF000000u));
#endif
    std::printf(fails ? "starthint_test: %d FAILED\n" : "starthint_test: ok\n", fails);
    return fails ? 1 : 0;
}
