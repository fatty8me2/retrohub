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
    // ---- Dolphin speed: 0.1 hotkey steps from 1x, no Max (Dolphin's speed-limit hotkey is hold-only)
    CHECK(dolphin_speed_text(0) == "0.3x" && dolphin_speed_text(1) == "0.5x" && dolphin_speed_text(2) == "1x" && dolphin_speed_text(3) == "1.5x");
    CHECK(dolphin_speed_text(4) == "2x" && dolphin_speed_text(5) == "3x" && dolphin_speed_text(6) == "5x" && dolphin_speed_text(7) == "8x");
    CHECK(dolphin_speed_step(7, 1) == 7 && dolphin_speed_step(0, -1) == 0 && dolphin_speed_step(2, 1) == 3);
    {
        auto count = [](const std::vector<DolphinKey>& v, DolphinKey k) { int n = 0; for (auto x : v) n += x == k; return n; };
        DolphinSpeed s;
        CHECK(dolphin_plan(s, 2).empty() && dolphin_idx(s) == 2);
        auto p = dolphin_plan(s, 3);   // 1x -> 1.5x: 5 increases
        CHECK(p.size() == 5 && count(p, DolphinKey::Increase) == 5 && dolphin_idx(s) == 3);
        p = dolphin_plan(s, 7);        // 1.5x -> 8x: 65 increases
        CHECK(p.size() == 65 && count(p, DolphinKey::Increase) == 65 && dolphin_idx(s) == 7);
        p = dolphin_plan(s, 1);        // 8x -> 0.5x: 75 decreases
        CHECK(p.size() == 75 && count(p, DolphinKey::Decrease) == 75 && dolphin_idx(s) == 1);
        p = dolphin_plan(s, 0);        // 0.5x -> 0.3x
        CHECK(p.size() == 2 && count(p, DolphinKey::Decrease) == 2);
        p = dolphin_plan(s, 2);        // 0.3x -> 1x: 7 increases (never below Dolphin's 0.1 floor on the way)
        CHECK(p.size() == 7 && count(p, DolphinKey::Increase) == 7);
        CHECK(dolphin_plan(s, 99).size() == 70 && dolphin_idx(s) == 7);   // out-of-range clamps to 8x
    }
    CHECK(dolphin_vk(DolphinKey::Increase) == 0x7C && dolphin_vk(DolphinKey::Decrease) == 0x7D);
    CHECK(dolphin_scan(0x7C) == 0x64 && dolphin_scan(0x7D) == 0x65);   // DIK_F13 / DIK_F14
    CHECK(std::string(dolphin_hotkey_name(DolphinKey::Increase)) == "Emulation Speed/Increase Emulation Speed" && std::string(dolphin_hotkey_name(DolphinKey::Decrease)) == "Emulation Speed/Decrease Emulation Speed");
    CHECK(std::string(dolphin_hotkey_expr(DolphinKey::Increase)) == "@(Ctrl+Shift+F13)" && std::string(dolphin_hotkey_expr(DolphinKey::Decrease)) == "@(Ctrl+Shift+F14)");
    CHECK(speed_row(true, false, false, false) == SpeedRow::Enabled && speed_row(true, false, true, false) == SpeedRow::Online);
    CHECK(speed_row(false, true, false, true) == SpeedRow::Enabled && speed_row(false, true, true, true) == SpeedRow::Online);
    CHECK(speed_row(false, false, false, true) == SpeedRow::Unsupported && speed_row(false, false, true, true) == SpeedRow::Unsupported);
    CHECK(speed_row(false, false, false, false) == SpeedRow::Hidden);
    std::printf(g_fail ? "FAILED\n" : "ok\n");
    return g_fail ? 1 : 0;
}
