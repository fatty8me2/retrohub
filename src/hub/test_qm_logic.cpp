// Console test for the quick menu's pure speed / volume logic (qm_logic.h).
#include "qm_logic.h"
#include <cstdio>
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
int main() {
    using namespace hubqm;
    CHECK(speed_text(0) == "0.25x" && speed_text(1) == "0.5x" && speed_text(2) == "1x" && speed_text(3) == "1.5x");
    CHECK(speed_text(4) == "2x" && speed_text(5) == "3x" && speed_text(6) == "5x" && speed_text(7) == "8x" && speed_text(8) == "Max");
    CHECK(speed_value(kSpeedNormal) == 1.0 && speed_value(8) == 0.0 && speed_value(99) == 0.0 && speed_value(-3) == 0.25);
    for (int i = 0; i < kSpeedSteps; ++i) CHECK(runctl::speed_valid(speed_value(i)));
    CHECK(speed_step(0, -1) == 0 && speed_step(8, 1) == 8 && speed_step(2, 1) == 3 && speed_step(2, -1) == 1);
    CHECK(volume_step(0, -1) == 0 && volume_step(100, 1) == 100 && volume_step(50, 1) == 55 && volume_step(2, -1) == 0);
    CHECK(ext_session_scalar(100, 100) == 1.f && ext_session_scalar(50, 100) == 0.5f && ext_session_scalar(0, 100) == 0.f);
    CHECK(ext_session_scalar(80, 40) == 1.f && ext_session_scalar(20, 40) == 0.5f && ext_session_scalar(10, 0) == 1.f && ext_session_scalar(0, 0) == 0.f);
    CHECK(speed_available(true, false) && !speed_available(true, true) && !speed_available(false, false));
    // volume / mute are local: allowed online; speed is not
    CHECK(!runctl::offline_only(runctl::Cmd::Volume) && !runctl::offline_only(runctl::Cmd::Mute) && runctl::offline_only(runctl::Cmd::Speed));
    // ---- Dolphin speed: 0.1 hotkey steps from 1x
    CHECK(dolphin_speed_text(0) == "0.3x" && dolphin_speed_text(1) == "0.5x" && dolphin_speed_text(2) == "1x" && dolphin_speed_text(3) == "1.5x");
    CHECK(dolphin_speed_text(4) == "2x" && dolphin_speed_text(5) == "3x" && dolphin_speed_text(6) == "5x" && dolphin_speed_text(7) == "8x" && dolphin_speed_text(8) == "Max");
    {
        auto count = [](const std::vector<DolphinKey>& v, DolphinKey k) { int n = 0; for (auto x : v) n += x == k; return n; };
        DolphinSpeed s;
        CHECK(dolphin_plan(s, 2).empty() && dolphin_idx(s) == 2);
        auto p = dolphin_plan(s, 3);   // 1x -> 1.5x: 5 increases
        CHECK(p.size() == 5 && count(p, DolphinKey::Increase) == 5 && dolphin_idx(s) == 3);
        p = dolphin_plan(s, 7);        // 1.5x -> 8x: 65 increases
        CHECK(p.size() == 65 && count(p, DolphinKey::Increase) == 65 && dolphin_idx(s) == 7);
        p = dolphin_plan(s, 8);        // Max: one toggle, base kept
        CHECK(p.size() == 1 && p[0] == DolphinKey::ToggleLimit && s.unlimited && s.base == 7 && dolphin_idx(s) == 8);
        CHECK(dolphin_plan(s, 8).empty());
        p = dolphin_plan(s, 1);        // out of Max to 0.5x: toggle, then 70 - 5 = 75 decreases
        CHECK(p.size() == 76 && p[0] == DolphinKey::ToggleLimit && count(p, DolphinKey::Decrease) == 75 && !s.unlimited && dolphin_idx(s) == 1);
        p = dolphin_plan(s, 0);        // 0.5x -> 0.3x
        CHECK(p.size() == 2 && count(p, DolphinKey::Decrease) == 2);
        DolphinSpeed m; dolphin_plan(m, 8); p = dolphin_plan(m, 2);   // Max -> 1x: only the toggle
        CHECK(p.size() == 1 && p[0] == DolphinKey::ToggleLimit && dolphin_idx(m) == 2);
    }
    CHECK(dolphin_vk(DolphinKey::Increase) == 0x7C && dolphin_vk(DolphinKey::Decrease) == 0x7D && dolphin_vk(DolphinKey::ToggleLimit) == 0x7E);
    CHECK(speed_row(true, false, false, false) == SpeedRow::Enabled && speed_row(true, false, true, false) == SpeedRow::Online);
    CHECK(speed_row(false, true, false, true) == SpeedRow::Enabled && speed_row(false, true, true, true) == SpeedRow::Online);
    CHECK(speed_row(false, false, false, true) == SpeedRow::Unsupported && speed_row(false, false, true, true) == SpeedRow::Unsupported);
    CHECK(speed_row(false, false, false, false) == SpeedRow::Hidden);
    std::printf(g_fail ? "FAILED\n" : "ok\n");
    return g_fail ? 1 : 0;
}
