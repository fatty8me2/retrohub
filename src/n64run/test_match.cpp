// Console test for --match parsing and the RDRAM writes of MatchDriver (no emulator needed).
#include <cstdio>
#include <vector>

#include "match.h"
#include "remix_data.h"

using namespace n64run;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main() {
    MatchSpec m; std::string err;
    CHECK(parse_match_spec("stage=0x31;stocks=2;p0=0x37:1;p1=33", &m, &err));
    CHECK(m.stage == 0x31 && m.stocks == 2 && m.fighter[0] == 0x37 && m.costume[0] == 1 && m.fighter[1] == 33 && m.costume[1] == 0);
    CHECK(m.present[0] && m.present[1] && !m.present[2] && !m.present[3]);
    CHECK(!parse_match_spec("stage=1;p0=0:0", &m, &err));               // one player
    CHECK(!parse_match_spec("stage=1;p0=0:0;p1=200:0", &m, &err));      // fighter id out of range
    CHECK(!parse_match_spec("stage=1;p0=0;p1=1;bogus=2", &m, &err));
    CHECK(!parse_match_spec("stage=;p0=0;p1=1", &m, &err));

    // Driver: waits for the title scene, then writes the VS block and requests scene 0x16.
    std::vector<uint8_t> ram(8u * 1024 * 1024, 0);
    CHECK(parse_match_spec("stage=2;stocks=4;p0=1:2;p3=0x3B:5", &m, &err));
    int owner[4] = {0, 1, -1, -1};
    MatchDriver d(m, owner);
    ram_w8(ram.data(), remix::kCurrentScreen, 0x37);
    for (int vi = 1; vi < 100; vi++) d.step(ram.data(), vi);
    CHECK(!d.done());
    ram_w8(ram.data(), remix::kCurrentScreen, remix::screen::kTitle);
    for (int vi = 100; vi < 200; vi++) d.step(ram.data(), vi);
    CHECK(d.done());
    CHECK(ram_r8(ram.data(), remix::kCurrentScreen) == remix::screen::kVsBattle);
    CHECK(ram_r32(ram.data(), remix::kScreenInterrupt) == 1);
    CHECK(ram_r8(ram.data(), remix::kVsBase + remix::vs::kStage) == 2);
    CHECK(ram_r8(ram.data(), remix::kVsBase + remix::vs::kStocks) == 3);     // 0-based
    CHECK(ram_r8(ram.data(), remix::kVsPlayer1 + remix::player::kFkind) == 1);
    CHECK(ram_r8(ram.data(), remix::kVsPlayer1 + remix::player::kCostume) == 2);
    CHECK(ram_r8(ram.data(), remix::kVsPlayer1 + remix::kVsPlayerStride + remix::player::kPkind) == remix::kPkindNone);
    CHECK(ram_r8(ram.data(), remix::kVsPlayer1 + 3 * remix::kVsPlayerStride + remix::player::kFkind) == 0x3B);
    std::printf(fails ? "match test: %d FAILED\n" : "match test: all passed\n", fails);
    return fails ? 1 : 0;
}
