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
    std::printf(g_fail ? "FAILED\n" : "ok\n");
    return g_fail ? 1 : 0;
}
