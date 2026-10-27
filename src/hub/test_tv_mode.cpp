#include "tv_logic.h"
#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main() {
    using namespace hubtv;
    CHECK(tv_scale(1.f, 1080) == 1.5f);
    CHECK(tv_scale(1.f, 2160) == 3.f);
    CHECK(tv_scale(2.f, 720) == 2.f);      // never below the desktop scale
    CHECK(tv_scale(1.f, 8640) == kMaxScale);
    CHECK(tv_scale(1.25f, 0) == 1.25f);
    CHECK(hero_height(false, 300.f, 900.f, 1080.f) == 300.f);
    CHECK(hero_height(true, 300.f, 900.f, 1080.f) > 300.f);
    CHECK(hero_height(true, 300.f, 900.f, 1080.f) <= 1080.f * 0.44f + 0.01f);
    CHECK(run_value(L"C:\\a b\\RetroHub.exe") == L"\"C:\\a b\\RetroHub.exe\" --tv");
    CHECK(has_tv_flag(L"RetroHub.exe --tv") && has_tv_flag(L"\"C:\\x\\RetroHub.exe\" --tv"));
    CHECK(!has_tv_flag(L"RetroHub.exe --tvx") && !has_tv_flag(L"RetroHub.exe") && !has_tv_flag(L"a--tv"));
    CHECK(start_in_tv(true, false) && start_in_tv(false, true) && !start_in_tv(false, false));
    CHECK(clock_text(0, 5) == "12:05 AM" && clock_text(15, 7) == "3:07 PM" && clock_text(12, 0) == "12:00 PM");
    CHECK(clock_text(15, 7, true) == "15:07");
    AvatarPlan a = avatar_plan(3, 5);
    CHECK(a.shown == 3 && a.extra == 0);
    a = avatar_plan(9, 5);
    CHECK(a.shown == 4 && a.extra == 5);
    a = avatar_plan(-2, 5);
    CHECK(a.shown == 0 && a.extra == 0);
    std::printf(fails ? "tv: %d failures\n" : "tv: all ok\n", fails);
    return fails ? 1 : 0;
}
