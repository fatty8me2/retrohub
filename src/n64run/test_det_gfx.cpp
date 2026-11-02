#include <cstdio>
#include "det_gfx.h"
using namespace n64run;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main() {
    CHECK(det_gfx_game("mp2", ""));
    CHECK(det_gfx_game("x", "MarioParty3"));
    CHECK(det_gfx_game("mariogolf", ""));          // the reported 4-player desync
    CHECK(det_gfx_game("", "MarioGolf64"));
    CHECK(det_gfx_game("mariotennis", ""));
    CHECK(!det_gfx_game("mk64", "MARIOKART64"));   // no framebuffer readback: keeps its effects
    CHECK(!det_gfx_game("remix", "SMASH"));
    std::printf(fails ? "FAILED\n" : "PASS\n");
    return fails ? 1 : 0;
}
