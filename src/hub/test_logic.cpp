#include "logic.h"

#include <cstdio>
#include <cstdlib>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main() {
    using namespace hubui;
    Settings s;
    s.name = "Zed \"the\" \\ \xC3\xA9";
    s.rom_folder = "C:\\roms\\n64";
    s.server = "wss://x.example";
    s.input_delay = 4;
    s.last_game = "mp2";
    s.smash_exe = "D:\\g\\BattleShip.exe";
    Settings t;
    CHECK(settings_from_json(settings_to_json(s), &t));
    CHECK(t.name == s.name && t.rom_folder == s.rom_folder && t.server == s.server);
    CHECK(t.input_delay == 4 && t.last_game == "mp2" && t.smash_exe == s.smash_exe);
    Settings u;
    CHECK(settings_from_json("{\"input_delay\": 99}", &u) && u.input_delay == 6 && u.name == "Player");
    CHECK(!settings_from_json("{bad", &u));

    PeerInfo a{0, "Ann", true, {{"mp1", "AAA"}, {"smash64", "native"}}};
    PeerInfo b{1, "Bob", true, {{"mp1", "AAA"}}};
    PeerInfo c{2, "Cy", true, {{"mp1", "BBB"}}};
    CHECK(game_selectable("mp1", 4, {a, b}, 0).ok);
    CHECK(game_selectable("smash64", 4, {a, b}, 0).reason == "Bob doesn't have this game");
    CHECK(game_selectable("mp1", 4, {a, b, c}, 0).reason == "different ROM version (Cy)");
    PeerInfo d; d.slot = 3; d.name = "Di";
    CHECK(game_selectable("mp1", 4, {a, d}, 0).reason == "waiting for Di");
    CHECK(!game_selectable("mp1", 1, {a, b}, 0).ok);

    Json j;
    PeerInfo h;
    CHECK(json_parse(hello_json("1", {{"mp1", "AAA"}, {"mp2", "BBB"}}), &j));
    CHECK(parse_hello(j, &h) && h.games.size() == 2 && h.games["mp2"] == "BBB");
    CHECK(json_parse(launch_json("mp1", 3, 2, 77u), &j));
    CHECK(j.str("game") == "mp1" && (int)j.num("launch") == 3 && (int)j.num("seed") == 77);

    CHECK(ports_arg({1, 0, 2}) == "0,1,2,-1");
    CHECK(ports_arg({0}) == "0,-1,-1,-1");
    CHECK(normalize_code(" ab c2d3efg") == "ABC2D3");
    CHECK(exit_code_text(5).find("sync") != std::string::npos);

    std::printf(fails ? "FAILED (%d)\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
