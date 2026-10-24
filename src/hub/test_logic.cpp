#include "logic.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main() {
    using namespace hubui;
    Settings s;
    s.name = "Zed \"the\" \\ \xC3\xA9";
    s.rom_folders = {"C:\\roms\\n64", "D:\\more \"roms\""};
    s.server = "wss://x.example";
    s.input_delay = 4;
    s.last_game = "mp2";
    s.smash_exe = "D:\\g\\BattleShip.exe";
    s.gb_core = "gambatte";
    s.fullscreen = true;
    s.sync_folder = "C:\\Users\\a\\OneDrive\\RetroHub";
    s.sync_auto = false;
    s.sync_states = true;
    s.cloud_saves = true;
    s.sync_card_dismissed = true;
    s.smash_save_notice_shown = true;
    s.cemu_exe = "D:\Wiiu\Cemu.exe";
    Settings t;
    CHECK(settings_from_json(settings_to_json(s), &t));
    CHECK(t.name == s.name && t.rom_folders == s.rom_folders && t.server == s.server);
    CHECK(t.input_delay == 4 && t.last_game == "mp2" && t.smash_exe == s.smash_exe);
    CHECK(t.gb_core == "gambatte" && t.fullscreen && t.sync_folder == s.sync_folder);
    CHECK(!t.sync_auto && t.sync_states && t.sync_card_dismissed && t.smash_save_notice_shown && t.cemu_exe == s.cemu_exe);
    {   Settings g; g.games_dest = "E:\\MyGames"; Settings h; CHECK(settings_from_json(settings_to_json(g), &h) && h.games_dest == g.games_dest && Settings().games_dest.empty()); }
    CHECK(t.cloud_saves && !Settings().cloud_saves);   // cloud saves: stored, off by default
    Settings u;
    CHECK(u.cemu_exe.empty());
    CHECK(settings_from_json("{\"input_delay\": 99}", &u) && u.input_delay == 8 && u.delay_auto && u.name.empty());
    CHECK(u.gb_core == "sameboy" && u.sync_auto && !u.sync_states && u.rom_folders.empty() && !u.sync_card_dismissed && !u.smash_save_notice_shown);
    CHECK(!settings_from_json("{bad", &u));

    {   Settings f; f.find_games_done = true; Settings g; CHECK(settings_from_json(settings_to_json(f), &g) && g.find_games_done); CHECK(!t.find_games_done); }
    /* migration rom_folder -> rom_folders */
    Settings m;
    CHECK(settings_from_json("{\"name\":\"A\",\"rom_folder\":\"C:\\\\old\"}", &m));
    CHECK(m.rom_folders.size() == 1 && m.rom_folders[0] == "C:\\old");
    CHECK(settings_to_json(m).find("\"rom_folder\"") == std::string::npos);
    Settings m2;   // rom_folders wins over the legacy key; duplicates and empties dropped
    CHECK(settings_from_json("{\"rom_folder\":\"X\",\"rom_folders\":[\"A\",\"\",\"A\",\"B\"],\"gb_core\":\"bogus\"}", &m2));
    CHECK(m2.rom_folders.size() == 2 && m2.rom_folders[0] == "A" && m2.rom_folders[1] == "B" && m2.gb_core == "sameboy");

    /* lobby: catalog ids (md5) and library keys */
    PeerInfo a{0, "Ann", true, {{"mp1", "AAA"}, {"smash64", "native"}}, {"n64:aaa", "gba:abcd-1234"}};
    PeerInfo b{1, "Bob", true, {{"mp1", "AAA"}}, {"n64:aaa", "gba:abcd-1234", "gb:tetris-1"}};
    PeerInfo c{2, "Cy", true, {{"mp1", "BBB"}}, {"gba:abcd-1234"}};
    CHECK(game_selectable("mp1", 4, {a, b}, 0).ok);
    CHECK(game_selectable("smash64", 4, {a, b}, 0).reason == "Bob doesn't have this game");
    CHECK(game_selectable("mp1", 4, {a, b, c}, 0).reason == "different ROM version (Cy)");
    CHECK(game_selectable("gba:abcd-1234", 4, {a, b, c}, 0).ok);
    CHECK(game_selectable("gb:tetris-1", 4, {a, b}, 0).reason == "Ann doesn't have this game");
    CHECK(game_selectable("n64:aaa", 4, {a, b}, 0).ok);
    CHECK(game_selectable("gba:abcd-1234", 2, {a, b, c}, 0).reason.find("too many") == 0);
    PeerInfo d; d.slot = 3; d.name = "Di";
    CHECK(game_selectable("mp1", 4, {a, d}, 0).reason == "waiting for Di");
    CHECK(!game_selectable("mp1", 1, {a, b}, 0).ok);

    /* Smash Classic online is up to 4 players (no online cap): 3- and 4-member rooms are allowed, no notice */
    {
        PeerInfo sa{0, "Ann", true, {{"smash64", "native"}}, {}};
        PeerInfo sb{1, "Bob", true, {{"smash64", "native"}}, {}};
        PeerInfo sc{2, "Cy", true, {{"smash64", "native"}}, {}};
        PeerInfo sd{3, "Di", true, {{"smash64", "native"}}, {}};
        CHECK(game_selectable("smash64", 4, {sa}, 0).ok);
        CHECK(game_selectable("smash64", 4, {sa, sb}, 0).ok);
        CHECK(game_selectable("smash64", 4, {sa, sb, sc}, 0).ok);
        CHECK(game_selectable("smash64", 4, {sa, sb, sc, sd}, 0).ok);
        CHECK(game_selectable("smash64", 4, {sa, sb, sc, sd}, 0).reason.empty());
        CHECK(!game_selectable("smash64", 2, {sa, sb, sc}, 0).ok);   /* the generic max_players check still works */
        CHECK(game_selectable("smash64", 2, {sa, sb, sc}, 0).reason == "too many players (max 2)");
    }

    /* hello v2 (+ old v1 shape without keys) */
    Json j;
    PeerInfo h;
    CHECK(json::parse(hello_json("2", {{"mp1", "AAA"}, {"mp2", "BBB"}}, {"n64:aaa", "gb:x-1"}), &j));
    CHECK(j.str("version") == "2");
    CHECK(parse_hello(j, &h) && h.games.size() == 2 && h.games["mp2"] == "BBB");
    CHECK(h.keys.size() == 2 && h.keys[0] == "n64:aaa" && h.keys[1] == "gb:x-1");
    CHECK(json::parse("{\"kind\":\"hello\",\"version\":\"1\",\"games\":{\"mp1\":\"AAA\"}}", &j));
    PeerInfo old;
    CHECK(parse_hello(j, &old) && old.hello && old.keys.empty() && old.games["mp1"] == "AAA");
    CHECK(json::parse(hello_json("2", {}, {}), &j) && parse_hello(j, &h) && h.games.empty() && h.keys.empty());
    {   /* long key lists are split into <= 4096 byte messages and reassemble */
        std::vector<std::string> many;
        for (int i = 0; i < 500; ++i) many.push_back("gba:abcd-" + std::to_string(100000 + i));
        auto msgs = hello_messages("2", {{"mp1", "AAA"}}, many);
        CHECK(msgs.size() > 1);
        PeerInfo r;
        bool allok = true;
        for (auto& m : msgs) {
            allok = allok && m.size() <= 3100 && json::parse(m, &j) && parse_hello(j, &r);
        }
        CHECK(allok && r.keys == many && r.games["mp1"] == "AAA" && r.hello);
        CHECK(hello_messages("2", {}, {}).size() == 1);
    }
    {   /* app version gate */
        Json aj;
        PeerInfo ap;
        CHECK(json::parse(hello_json("2", {}, {}, "1.10.0"), &aj) && parse_hello(aj, &ap) && ap.app == "1.10.0");
        PeerInfo ap2;
        auto am = hello_messages("2", {{"mp1", "A"}}, {"n64:a"}, 3000, "1.9.1");
        CHECK(json::parse(am[0], &aj) && parse_hello(aj, &ap2) && ap2.app == "1.9.1");
        PeerInfo ap3;
        CHECK(json::parse(hello_json("2", {}, {}), &aj) && parse_hello(aj, &ap3) && ap3.app.empty());
        CHECK(compare_app_versions("1.9.1", "1.10.0") == -1 && compare_app_versions("v1.10.0", "1.10") == 0 &&
              compare_app_versions("2.0.0", "1.99.9") == 1 && compare_app_versions("1.10.0-beta", "1.10.0") == 0 &&
              compare_app_versions("dev", "1.0") == 2);
        CHECK(app_version_label("1.10.0") == "v1.10.0" && app_version_label("v1.2") == "v1.2" && app_version_label("").empty());
        PeerInfo hh; hh.slot = 0; hh.name = "Host"; hh.hello = true; hh.app = "1.10.0";
        PeerInfo gg; gg.slot = 1; gg.name = "Mia"; gg.hello = true; gg.app = "1.10.0";
        CHECK(version_gate("1.10.0", {hh, gg}).ok);
        gg.app = "1.9.1";
        VersionGate vg = version_gate("1.10.0", {hh, gg});
        CHECK(!vg.ok && vg.reason == "Mia is on v1.9.1 - everyone needs v1.10.0. Ask them to update.");
        CHECK(version_gate("1.10.0", {hh, gg}, true).ok);
        gg.app.clear();
        vg = version_gate("1.10.0", {hh, gg});
        CHECK(!vg.ok && vg.reason.find("Mia is on an old RetroHub") == 0 && vg.reason.find("update") != std::string::npos);
        gg.hello = false;   /* not yet heard from: not this gate's job */
        CHECK(version_gate("1.10.0", {hh, gg}).ok);
        CHECK(guest_version_action("1.9.1", "1.10.0", true) == GuestVersionAction::UpdateNow);
        CHECK(guest_version_action("1.10.0", "1.9.1", true) == GuestVersionAction::HostNeedsUpdate);
        CHECK(guest_version_action("1.10.0", "", true) == GuestVersionAction::HostNeedsUpdate);   /* hello, no app: old host */
        CHECK(guest_version_action("1.10.0", "v1.10.0", true) == GuestVersionAction::None);
        CHECK(guest_version_action("1.10.0", "", false) == GuestVersionAction::Waiting);          /* no hello yet */
        CHECK(guest_version_action("1.10.0", "1.9.1", false) == GuestVersionAction::Waiting);
    }
    CHECK(json::parse(launch_json("mp1", 3, 2, 77u), &j));
    CHECK(j.str("game") == "mp1" && (int)j.num("launch") == 3 && (int)j.num("seed") == 77 && j.str("core").empty());
    CHECK(json::parse(launch_json("gb:x-1", 1, 2, 5u, "gambatte"), &j) && j.str("core") == "gambatte");
    CHECK(json::parse(launch_json("pokered", 1, 0, 5u, "", false, nullptr, nullptr, ",\"np_link\":{\"core\":\"sameboy\",\"consoles\":2}"), &j) && j.get("np_link") &&
          j.get("np_link")->str("core") == "sameboy");
    CHECK(json::parse(launch_json("pokered", 1, 0, 5u), &j) && !j.get("np_link"));

    {   /* DS wireless launch spec */
        Json lj;
        std::string mode;
        CHECK(json::parse(launch_json("nds:x", 1, 0, 5u, "", false, nullptr, nullptr, ",\"np_ds\":" + np_ds_json()), &lj));
        CHECK(parse_np_ds(lj, &mode) && mode == "wireless");
        CHECK(json::parse(launch_json("nds:x", 1, 0, 5u), &lj) && !parse_np_ds(lj, &mode));   /* older host: absent */
        CHECK(json::parse("{\"np_ds\":{\"v\":2,\"mode\":\"wireless\"}}", &lj) && !parse_np_ds(lj, &mode));   /* unknown version */
        CHECK(json::parse("{\"np_ds\":{\"v\":1,\"mode\":\"other\"}}", &lj) && !parse_np_ds(lj, &mode) && mode == "other");
    }

    /* library filtering / search / sorting */
    std::vector<ListItem> items = {
        {"n64:1", "Mario Party 2", "n64", false}, {"gba:1", "advance Wars", "gba", false},
        {"gb:1", "Tetris", "gb", false},          {"gbc:1", "Pokemon Crystal", "gbc", false},
        {"native:smash64", "Super Smash Bros.", "n64", true}, {"gc:1", "Super Mario Sunshine", "gc", false},
        {"n64:2", "mario party", "n64", false}};
    auto all = filter_sort(items, "", "");
    CHECK(all.size() == 7 && all[0] == 4);                       // pinned first
    CHECK(items[all[1]].title == "advance Wars");                // then case-insensitive alphabetical
    CHECK(items[all[2]].title == "mario party" && items[all[3]].title == "Mario Party 2");
    auto q = filter_sort(items, "", "  MARIO ");
    CHECK(q.size() == 3);
    auto n = filter_sort(items, "n64", "");
    CHECK(n.size() == 3 && n[0] == 4);
    CHECK(filter_sort(items, "gc", "mario").size() == 1);
    CHECK(filter_sort(items, "gba", "mario").empty());
    CHECK(filter_sort(items, "", "zzz").empty());
    auto rec = pick_recent({0, 50, 10, 0, 99, 10}, 3);
    CHECK(rec.size() == 3 && rec[0] == 4 && rec[1] == 1 && rec[2] == 2);   // ties keep list order
    CHECK(pick_recent({0, 0}, 6).empty());

    /* settings: new UI fields */
    {
        Settings d;
        CHECK(d.download_art && d.sidebar_mode == 0);
        Settings x;
        x.download_art = false;
        x.sidebar_mode = 2;
        Settings y;
        CHECK(settings_from_json(settings_to_json(x), &y) && !y.download_art && y.sidebar_mode == 2);
        CHECK(settings_from_json("{\"sidebar_mode\": 9}", &y) && y.sidebar_mode == 2);
        CHECK(settings_from_json("{}", &d) && d.download_art);
    }

    /* All Games: grouped by console */
    {
        const std::vector<std::string> want = {"n64", "gc", "wiiu", "3ds", "nds", "gba", "gbc", "gb"};
        CHECK(console_order() == want);
        std::vector<ListItem> it = {
            {"gb:1", "Tetris", "gb", false},          // 0
            {"gbc:1", "Crystal", "gbc", false},       // 1
            {"n64:1", "zelda", "n64", false},         // 2
            {"native:smash64", "Super Smash Bros.", "n64", true},   // 3
            {"n64:2", "Banjo", "n64", false},         // 4
            {"wiiu:1", "Mario Kart 8", "wiiu", false},// 5
            {"gba:1", "Metroid", "gba", false},       // 6
            {"x:1", "Weird", "ps2", false},           // 7
        };
        auto secs = group_by_console(it);
        std::vector<std::string> sys;
        for (auto& s2 : secs) sys.push_back(s2.system);
        CHECK((sys == std::vector<std::string>{"n64", "wiiu", "gba", "gbc", "gb", "other"}));   // fixed order, empty consoles skipped, unknown last
        CHECK(secs[0].title == "Nintendo 64" && secs[1].title == "Wii U" && secs[3].title == "Game Boy Color");
        CHECK(secs[0].items == std::vector<int>({3, 4, 2}));    // native Smash under N64 (pinned), then A-Z case-insensitive
        CHECK(secs[3].items == std::vector<int>({1}) && secs[4].items == std::vector<int>({0}));   // gbc vs gb separate
        /* console filter: filter_sort -> one section */
        auto f = group_by_console(it, nullptr);
        std::vector<int> vis = filter_sort(it, "gb", "");
        auto fs = group_by_console(it, &vis);
        CHECK(fs.size() == 1 && fs[0].system == "gb" && fs[0].items == std::vector<int>({0}));
        std::vector<int> vn = filter_sort(it, "n64", "");
        CHECK(group_by_console(it, &vn).size() == 1 && group_by_console(it, &vn)[0].items.size() == 3);
        std::vector<int> none;
        CHECK(group_by_console(it, &none).empty() && group_by_console({}).empty());
        (void)f;
    }

    /* shelves */
    {
        std::vector<ShelfGame> g(9);
        g[0] = {"native:smash64", "Super Smash Bros.", "n64", false, 0, true, true};
        g[1] = {"n64:1", "Mario Party", "n64", true, 300, true, false};
        g[2] = {"n64:2", "banjo", "n64", false, 100, false, false};
        g[3] = {"gba:1", "Wario Ware", "gba", true, 0, false, false};
        g[4] = {"gb:1", "Tetris", "gb", false, 200, false, false};
        g[5] = {"gc:1", "Melee", "gc", false, 0, false, false};
        g[6] = {"gbc:1", "Crystal", "gbc", false, 0, false, false};
        g[7] = {"n64:3", "Zelda", "n64", false, 0, false, false};
        g[8] = {"n64:4", "Unknown Rom", "n64", false, 0, false, false};
        auto sh = compose_shelves(g);
        std::vector<std::string> ids;
        for (auto& s2 : sh) ids.push_back(s2.id);
        std::vector<std::string> want = {"recent", "favs", "console:n64", "console:gc", "console:gba", "console:gbc", "console:gb"};
        CHECK(ids == want);
        CHECK(sh[0].items == std::vector<int>({1, 4, 2}));          // newest first
        CHECK(sh[1].items == std::vector<int>({1, 3}));             // favorites by title
        CHECK(sh[2].items == std::vector<int>({0, 2, 1, 8, 7}));    // n64: Smash pinned, then banjo, Mario Party, Unknown Rom, Zelda
        CHECK(sh[0].title == "Continue Playing" && sh[2].title == "Nintendo 64" && sh[6].title == "Game Boy");
        /* each game appears in exactly one console shelf */
        std::vector<int> seen(g.size(), 0);
        for (auto& s2 : sh)
            if (s2.id.compare(0, 8, "console:") == 0) for (int i : s2.items) ++seen[i];
        for (int n : seen) CHECK(n == 1);
        CHECK(compose_shelves({}).empty());
        CHECK(compose_shelves(g, 2)[0].items.size() == 2);
        std::vector<ShelfGame> odd(1);
        odd[0] = {"x:1", "Odd", "ps2", false, 0, false, false};
        auto so = compose_shelves(odd);
        CHECK(so.size() == 1 && so[0].id == "console:other" && so[0].items.size() == 1);
        CHECK(pick_hero(g, "gb:1") == 4 && pick_hero(g, "nope") == 1 && pick_hero(g, "") == 1);
        std::vector<ShelfGame> fresh(2);
        fresh[1].favorite = true;
        CHECK(pick_hero(fresh, "") == 1 && pick_hero({}, "x") == -1);
        fresh[1].favorite = false;
        CHECK(pick_hero(fresh, "") == 0);
        auto pk = pick_unowned({false, true, false, false, false}, {false, true, false, true, false}, 3);
        CHECK(pk == std::vector<int>({3, 0, 2}));                   // online first, then catalog order
        CHECK(pick_unowned({true, true}, {}, 5).empty());
        CHECK(pick_unowned({false, false, false}, {}, 2) == std::vector<int>({0, 1}));
    }

    /* horizontal shelf scrolling: wheel routing, drag threshold, paging, reveal */
    {
        std::vector<FRect> rs = {{10, 10, 110, 60}, {10, 100, 110, 160}};
        CHECK(rect_hit(rs, 50, 30) == 0 && rect_hit(rs, 50, 120) == 1);
        CHECK(rect_hit(rs, 50, 80) == -1 && rect_hit(rs, 110, 30) == -1 && rect_hit({}, 1, 1) == -1);
        CHECK(std::fabs(shelf_wheel_delta(0, -1, 200, 20) - 330.f) < 1e-3f);   // wheel down = scroll right, 1.5 tiles
        CHECK(std::fabs(shelf_wheel_delta(0, 1, 200, 20) + 330.f) < 1e-3f);    // wheel up = left
        CHECK(std::fabs(shelf_wheel_delta(1, 0, 200, 20) - 330.f) < 1e-3f);    // horizontal wheel right
        CHECK(!drag_exceeds(3, 3, 6) && drag_exceeds(7, 0, 6) && drag_exceeds(0, -7, 6) && !drag_exceeds(0, 0, 6));
        CHECK(std::fabs(shelf_page_step(1000, 200, 20, 12) - 4 * 220.f) < 1e-3f);
        CHECK(shelf_page_step(50, 200, 20, 12) == 220.f);                      // at least one tile
        float ms = shelf_max_scroll(10, 200, 20, 12, 1000);
        CHECK(std::fabs(ms - (24 + 2000 + 180 - 1000)) < 1e-3f && shelf_max_scroll(2, 200, 20, 12, 1000) == 0.f);
        CHECK(shelf_clamp(-5, ms) == 0.f && shelf_clamp(1e6f, ms) == ms && shelf_clamp(5, 0) == 0.f);
        float last_right = 12 + 9 * 220.f + 200;   // the last tile is fully visible at max scroll
        CHECK(std::fabs((last_right - ms) - (1000 - 12)) < 1e-3f);
        float t1 = shelf_reveal(12 + 9 * 220.f, last_right, 0, 1000, 28, ms);   // focus walks to the last tile and back
        CHECK(last_right - t1 <= 1000 - 28 + 1e-3f || t1 == ms);
        CHECK(shelf_reveal(12, 212, t1, 1000, 28, ms) == 0.f);
        CHECK(shelf_reveal(12 + 2 * 220.f, 12 + 2 * 220.f + 200, 0, 1000, 28, ms) == 0.f);   // already visible: no move
    }

    /* layout + animation math */
    {
        GridLayout gl = grid_layout(1000, 230, 330, 18);
        CHECK(gl.cols == 4 && gl.tile_w > 230 && gl.tile_w <= 330);
        CHECK(gl.cols * gl.tile_w + (gl.cols - 1) * 18 <= 1000.01f);
        GridLayout narrow = grid_layout(100, 230, 330, 18);
        CHECK(narrow.cols == 1 && narrow.tile_w == 100);
        CHECK(grid_layout(600, 230, 250, 18).tile_w == 250);
        for (float a = 300; a < 3000; a += 37) {
            float w = shelf_tile_w(a, 200, 300, 16);
            CHECK(w >= 200 && w <= 300);
        }
        CHECK(shelf_tile_w(900, 200, 300, 16) < 300 + 0.01f);
        float v = 0;
        for (int i = 0; i < 600; ++i) v = approach(v, 1.f, 1.f / 60.f, 14.f);
        CHECK(v > 0.999f && v <= 1.f);
        float a1 = approach(0, 1, 0.016f, 14), a2 = approach(approach(0, 1, 0.008f, 14), 1, 0.008f, 14);
        CHECK(std::fabs(a1 - a2) < 1e-5f);                          // frame-rate independent
        CHECK(approach(0.5f, 1, 0, 14) == 0.5f);
        CHECK(!sidebar_collapsed(0, 1200, 1100) && sidebar_collapsed(0, 900, 1100));
        CHECK(!sidebar_collapsed(1, 500, 1100) && sidebar_collapsed(2, 3000, 1100));

        ArtFit c = fit_art(400, 300, 260, 200);                     // landscape box into landscape tile
        CHECK(c.cover && c.w == 260 && c.h == 200 && c.v0 == 0 && c.v1 == 1 && c.u0 > 0 && c.u1 < 1);
        CHECK(std::fabs((c.u1 - c.u0) * 400.f / ((c.v1 - c.v0) * 300.f) - 260.f / 200.f) < 1e-3f);   // crop matches tile aspect
        ArtFit p = fit_art(300, 420, 260, 200);                     // portrait box: letterboxed
        CHECK(!p.cover && p.h == 200 && p.w < 260 && std::fabs(p.x - (260 - p.w) * 0.5f) < 1e-3f && p.u0 == 0 && p.u1 == 1);
        ArtFit z = fit_art(0, 0, 260, 200);
        CHECK(z.cover && z.w == 260 && z.h == 200);
        ArtFit wide = fit_art(1000, 200, 260, 200);                 // panorama: contain, centered vertically
        CHECK(!wide.cover && wide.w == 260 && wide.h < 100 && wide.y > 0);
    }

    CHECK(format_play_time(0) == "Never played");
    CHECK(format_play_time(30) == "Under a minute");
    CHECK(format_play_time(35 * 60) == "35 min");
    CHECK(format_play_time(2 * 3600 + 5 * 60) == "2h 05m");
    CHECK(sanitize_player_name("") == "Player");
    CHECK(sanitize_player_name("  Bob \"x\" ") == "Bob x");
    CHECK(sanitize_player_name("ABCDEFGHIJKLMNOPQRSTUVWXYZ").size() == 16);

    /* ini */
    std::string ini = "[General]\nA = 1\n\n[Core]\nSlotA = 1\nOther=3\n";
    ini = ini_set(ini, "core", "slota", "8");
    CHECK(ini_get(ini, "Core", "SlotA") == "8" && ini_get(ini, "Core", "Other") == "3");
    ini = ini_set(ini, "Core", "SIDevice0", "6");
    CHECK(ini_get(ini, "Core", "SIDevice0") == "6" && ini_get(ini, "General", "A") == "1");
    CHECK(ini.find("SIDevice0 = 6\n") < ini.find("[General]") + 1000 && ini_get(ini, "General", "SIDevice0", "x") == "x");
    ini = ini_set(ini, "Analytics", "Enabled", "False");
    CHECK(ini_get(ini, "Analytics", "Enabled") == "False");
    CHECK(ini_get(ini_set("", "S", "k", "v"), "S", "k") == "v");
    CHECK(ini_get(ini_set("[S]\r\nk = 1\r\n", "S", "k", "2"), "S", "k") == "2");

    VaultMeta vm;
    CHECK(parse_vault_meta("{\"key\":\"gba:a-1\",\"title\":\"T\",\"system\":\"gba\",\"updated\":1700000000,\"device\":\"PC\"}", &vm));
    CHECK(vm.key == "gba:a-1" && vm.updated == 1700000000ull && vm.device == "PC");
    CHECK(!parse_vault_meta("{}", &vm));

    CHECK(ports_arg({1, 0, 2}) == "0,1,2,-1");
    CHECK(ports_arg({0}) == "0,-1,-1,-1");
    CHECK(normalize_code(" ab c2d3efg") == "ABC2D3");
    CHECK(exit_code_text(5).find("sync") != std::string::npos);
    CHECK(exit_code_text(6).find("share the host's save") != std::string::npos);
    CHECK(exit_code_text(2).find("couldn't start") != std::string::npos);

    /* ports with sparse slots and local players */
    {
        PortPlan a = build_ports({{2, 1}, {0, 1}});
        CHECK(a.ports == "0,2,-1,-1" && a.players == 2);
        PortPlan b = build_ports({{0, 2}, {1, 1}});
        CHECK(b.ports == "0,0,1,-1" && b.players == 2);
        PortPlan c = build_ports({{0, 2}, {2, 2}, {3, 1}});   // 5 wanted: cap at 4
        CHECK(c.ports == "0,0,2,3" && c.locals[0] == 2 && c.locals[1] == 1);
        PortPlan d = build_ports({{0, 2}, {1, 2}, {2, 2}, {3, 2}});
        CHECK(d.ports == "0,1,2,3" && d.players == 4);
        CHECK(build_ports({{0, 9}}).ports == "0,0,-1,-1");
        CHECK(delay_arg(0) == " --delay auto" && delay_arg(3) == " --delay 3" && delay_arg(99) == " --delay 8");
        CHECK(launch_json("g", 1, 0, 5, "", true).find("\"share_save\":true") != std::string::npos);
        /* host-owned port plan travels in the launch message */
        {
            Json lj; std::string pp; int pn = 0;
            PortPlan hp = build_ports({{0, 2}, {2, 1}});
            CHECK(json::parse(launch_json("g", 1, 0, 5, "", false, &hp), &lj) && parse_launch_plan(lj, &pp, &pn));
            CHECK(pp == "0,0,2,-1" && pn == 2 && pp == hp.ports && pn == hp.players);
            CHECK(json::parse(launch_json("g", 1, 0, 5), &lj) && !parse_launch_plan(lj, &pp, &pn));   // older host: absent
            const char* bad[] = {
                "{\"ports\":[0,1,2],\"players\":3}",          "{\"ports\":[0,1,2,9],\"players\":4}",
                "{\"ports\":[0,-1,1,-1],\"players\":2}",      "{\"ports\":[0,1,-1,-1],\"players\":3}",
                "{\"ports\":[\"0\",1,-1,-1],\"players\":2}",   "{\"ports\":[0,1,0,-1],\"players\":2}",
                "{\"ports\":[-1,-1,-1,-1],\"players\":0}",    "{\"ports\":[0,1,-1,-1]}",
                "{\"ports\":[0,1.5,-1,-1],\"players\":2}",
            };
            for (const char* b : bad) CHECK(json::parse(b, &lj) && !parse_launch_plan(lj, &pp, &pn));
            CHECK(json::parse("{\"ports\":[3,3,-1,-1],\"players\":1}", &lj) && parse_launch_plan(lj, &pp, &pn) && pp == "3,3,-1,-1" && pn == 1);
        }
        Settings s2; s2.delay_auto = false; Settings s3;
        CHECK(settings_from_json(settings_to_json(s2), &s3) && !s3.delay_auto);
    }

    /* sync gating while a game runs */
    {
        SyncGate g;
        bool man = false;
        CHECK(sync_request(g, true, true, false) == SyncAction::Queue && g.pending);
        CHECK(!sync_take_pending(g, true, false, &man));      // game still running
        CHECK(!sync_take_pending(g, false, true, &man));      // job running
        CHECK(sync_take_pending(g, false, false, &man) && man && !g.pending);
        CHECK(!sync_take_pending(g, false, false, &man));
        CHECK(sync_request(g, false, false, false) == SyncAction::Start && !g.pending);
        CHECK(sync_request(g, false, false, true) == SyncAction::Queue);
    }

    /* friends */
    {
        auto row = [](const char* uid, const char* nm, int st, bool j, long long upd, const char* game = "", int p = 0, int mx = 0) {
            FriendRow r; r.uid = uid; r.name = nm; r.state = st; r.joinable = j; r.updated_ms = upd; r.game_title = game; r.players = p; r.max_players = mx;
            return r;
        };
        std::vector<FriendRow> rows = {row("a", "zed", FS_Online, false, 5), row("b", "Amy", FS_InGame, false, 5, "Mario Kart 64"),
                                       row("c", "bob", FS_InLobby, true, 5, "Mario Party 2", 2, 4), row("d", "old", FS_Offline, false, 1000),
                                       row("e", "new", FS_Offline, false, 9000), row("f", "cat", FS_InLobby, false, 5, "X", 4, 4),
                                       row("g", "Ann", FS_Online, false, 5)};
        FriendGroups g = group_friends(rows);
        CHECK((g.online == std::vector<int>{2, 1, 5, 6, 0}));    // joinable lobby, in game, full lobby, online by name (Ann, zed)
        CHECK((g.offline == std::vector<int>{4, 3}));            // most recently seen first
        CHECK(friend_status_line(rows[1], 0) == "Playing Mario Kart 64");
        CHECK(friend_status_line(rows[2], 0) == "In lobby \xC2\xB7 Mario Party 2 \xC2\xB7 2/4");
        CHECK(friend_status_line(rows[0], 0) == "Online" && friend_status_line(row("x", "x", FS_InGame, false, 0), 0) == "Playing");
        CHECK(friend_status_line(rows[4], 9000 + 3 * 3600 * 1000LL) == "Last seen 3h ago");
        CHECK(friend_status_line(row("x", "x", FS_Offline, false, 0), 100000) == "Offline");
        {   // duplicates: a uid is placed once; a friend in our room is flagged once and is not "joinable"
            std::vector<FriendRow> dup = {row("a", "Mia", FS_Online, false, 5), row("a", "Mia", FS_Offline, false, 1), row("b", "Jo", FS_Offline, false, 3)};
            FriendGroups dg = group_friends(dup);
            CHECK(dg.online.size() == 1 && dg.offline.size() == 1 && dg.online[0] == 0 && dg.offline[0] == 2);
            FriendRow rm = row("r", "Rae", FS_InLobby, true, 5, "Mario Kart 64", 2, 4);
            CHECK(friend_can_join(rm));
            rm.in_my_room = true;
            CHECK(!friend_can_join(rm) && friend_status_line(rm, 0) == "In your room");
            rm.state = FS_InGame;
            CHECK(friend_status_line(rm, 0) == "In your room Â· Playing");
        }
        CHECK(format_ago_ms(100000, 90000) == "just now" && format_ago_ms(1000 * 600, 0) == "10 min ago" && format_ago_ms(86400000LL * 2, 0) == "2d ago");
        CHECK(normalize_friend_code("7kq2m9xd") == "RH-7KQ2-M9XD");
        CHECK(normalize_friend_code(" rh-7kq2-m9xd ") == "RH-7KQ2-M9XD");
        CHECK(normalize_friend_code("RH7KQ2M9XD") == "RH-7KQ2-M9XD" && normalize_friend_code("7KQ2 M9XD") == "RH-7KQ2-M9XD");
        CHECK(normalize_friend_code("").empty() && normalize_friend_code("RH-7KQ2-M9X").empty() && normalize_friend_code("RH-7KQ!-M9XD").empty());
        CHECK(!invite_expired(5000, 4999) && invite_expired(5000, 5000) && !invite_expired(0, 99999));
        CHECK(invite_seconds_left(5000, 4000) == 1 && invite_seconds_left(5000, 5000) == 0 && invite_seconds_left(31000, 1000) == 30);
        CHECK(lobby_joinable(2, 4, false) && !lobby_joinable(4, 4, false) && !lobby_joinable(2, 4, true) && !lobby_joinable(0, 4, false));
        {   // join while the friend's game runs (room kept in presence)
            FriendRow ig = row("h", "Hal", FS_InGame, true, 5, "Smash", 2, 4), full = row("i", "Ivy", FS_InGame, false, 5, "Smash", 4, 4);
            CHECK(friend_can_join(ig) && !friend_can_join(full) && friend_can_join(rows[2]) && !friend_can_join(rows[1]) && !friend_can_join(rows[0]));
            CHECK(friend_status_line(ig, 0) == "Playing Smash \xC2\xB7 2/4" && friend_status_line(full, 0) == "Playing Smash");
            std::vector<FriendRow> r2 = {row("p", "plain", FS_InGame, false, 5), ig, rows[2], rows[0]};
            CHECK((group_friends(r2).online == std::vector<int>{2, 1, 0, 3}));   // joinable lobby, joinable in-game, in game, online
            CHECK(queued_join_text("Mia") == "Will join Mia's room when your game closes." && queued_join_text("") == "Will join that room when your game closes.");
        }
        Settings fs;
        CHECK(fs.show_friends && !fs.appear_offline && fs.notif_sound);
        fs.show_friends = false; fs.appear_offline = true; fs.notif_sound = false;
        Settings ft;
        CHECK(settings_from_json(settings_to_json(fs), &ft) && !ft.show_friends && ft.appear_offline && !ft.notif_sound);
    }

    {   // Smash vault / hand-off
        CHECK(smash_save_file("C:\\v\\n64\\native_smash64\\battery\\") == "C:\\v\\n64\\native_smash64\\battery\\ssb64_save.bin");
        CHECK(smash_save_file("D:\\b") == "D:\\b\\ssb64_save.bin");
        CHECK(smash_should_migrate_save(false, true) && !smash_should_migrate_save(true, true) && !smash_should_migrate_save(false, false));
        CHECK(smash_save_conflict(true, true, "AAA", "AAB"));                 /* both exist, differ */
        CHECK(!smash_save_conflict(true, true, "AAA", "AAA"));                /* identical: no notice */
        CHECK(!smash_save_conflict(true, false, "AAA", ""));                  /* no legacy file */
        CHECK(!smash_save_conflict(false, true, "", "AAA"));                  /* migration case, not a conflict */
        CHECK(smash_save_conflict_text("C:\\BS\\ssb64_save.bin") ==
              "Using your synced Smash save. Your old local save is still at C:\\BS\\ssb64_save.bin.");
        std::vector<RomRef> lib = {{"mp1", "NMPE", "a.z64"}, {"ssb64rom", "NALE", "C:\\r\\ssb.z64"}};
        CHECK(find_smash_rom(lib) == "C:\\r\\ssb.z64");
        CHECK(find_smash_rom({{"", "NALE", "x.z64"}}) == "x.z64" && find_smash_rom({{"mp1", "NMPE", "a"}}).empty() && find_smash_rom({{"ssb64rom", "NALE", ""}}).empty());
        auto env = smash_launch_env("r.z64", "s.bin");
        CHECK(env.size() == 2 && env[0].first == "SSB64_BASEROM" && env[1].first == "SSB64_SAVE_PATH" && env[1].second == "s.bin");
        CHECK(smash_launch_env("", "s.bin").size() == 1);
        /* Play decision: ROM identity and first-run extraction */
        CHECK(smash_md5_supported("f7c52568a31aadf26e14dc2b6416b2ed") && smash_md5_supported("F7C52568A31AADF26E14DC2B6416B2ED"));
        CHECK(!smash_md5_supported("") && !smash_md5_supported("4b71f0e01878696733eefa9c80d11c147ecb4984"));
        CHECK(smash_play_decision(true, false, "") == SmashPlay::Launch);                       /* o2r exists: no ROM needed */
        CHECK(smash_play_decision(false, false, "") == SmashPlay::NeedRom);                     /* no ROM, no o2r: message, no launch */
        CHECK(smash_play_decision(false, true, "f7c52568a31aadf26e14dc2b6416b2ed") == SmashPlay::Launch);
        CHECK(smash_play_decision(false, true, "66db457b130d31a286a23d6e4dd9726e") == SmashPlay::WarnMd5);   /* JP dump */
        CHECK(smash_play_decision(false, true, "") == SmashPlay::WarnMd5);                       /* unreadable */
        CHECK(smash_play_decision(true, true, "66db457b130d31a286a23d6e4dd9726e") == SmashPlay::Launch);
        CHECK(smash_prep_visible(true, false) && !smash_prep_visible(true, true) && !smash_prep_visible(false, false));
        CHECK(smash_md5_warn_text("").find("unreadable") != std::string::npos);
    }

    {   // Smash match picker
        auto fc = fighter_choices(true), fv = fighter_choices(false);
        CHECK(fv.size() == 12 && fc.size() == 30 && !fv.back().remix && fc.back().remix && fc[12].remix && fc[11].name == "Ness");
        auto sc = stage_choices(true), sv = stage_choices(false);
        CHECK(sv.size() == 9 && sc.size() > 9 && !sc[8].remix && sc[9].remix);
        CHECK(fighter_valid(-1, true) && fighter_valid(0x1D, true) && !fighter_valid(0x1D, false) && !fighter_valid(0x1B, true) && !fighter_valid(0x22, true) && !fighter_valid(999, true));
        CHECK(stage_valid(-1, true) && stage_valid(6, false) && stage_valid(0x2A, true) && !stage_valid(0x2A, false) && !stage_valid(0x29, true) && !stage_valid(0xD3, true));
        CHECK(fighter_name(-1) == "Random" && fighter_name(0x1D) == "Falco" && stage_name(6) == "Dream Land" && fighter_costumes(0) == 5);
        CHECK(match_game_supported("remix") && !match_game_supported("smash64"));

        MemberPick mp;
        mp.p[0] = {0x1D, 2}; mp.p[1] = {0, 4};
        Json j;
        MemberPick got;
        CHECK(json::parse(pick_json(mp), &j) && j.str("kind") == "pick" && parse_pick(j, &got) && got.p[0].fighter == 0x1D && got.p[0].costume == 2 && got.p[1].fighter == 0 && got.p[1].costume == 4);
        CHECK(json::parse("{\"kind\":\"pick\",\"fighter\":3}", &j) && parse_pick(j, &got) && got.p[0].fighter == 3 && got.p[0].costume == 0 && got.p[1].fighter == -1);
        CHECK(json::parse("{\"fighter\":0,\"costume\":5}", &j) && !parse_pick(j, &got));          // costume beyond Mario's 5
        CHECK(json::parse("{\"fighter\":-1,\"costume\":2}", &j) && !parse_pick(j, &got));         // Random has no costume
        CHECK(json::parse("{\"fighter\":34}", &j) && !parse_pick(j, &got));                        // 0x22 Dark Samus: not on the CSS
        CHECK(json::parse("{\"fighter\":29}", &j) && parse_pick(j, &got) && !parse_pick(j, &got, false));
        CHECK(json::parse("{\"fighter\":\"a\"}", &j) && !parse_pick(j, &got));
        CHECK(json::parse("{\"costume\":1}", &j) && !parse_pick(j, &got));
        CHECK(pick_label(mp, 1) == "Falco" && pick_label(mp, 2) == "Falco + Mario" && pick_label(MemberPick{}, 1) == "Random");

        MatchRules r;
        r.stage = 0x2A; r.stocks = 4;
        MatchRules rg;
        CHECK(json::parse(rules_json(r), &j) && j.str("kind") == "rules" && parse_rules(j, &rg) && rg.stage == 0x2A && rg.stocks == 4);
        CHECK(json::parse("{\"stage\":-1,\"stocks\":1}", &j) && parse_rules(j, &rg) && rg.stage == -1);
        CHECK(json::parse("{\"stage\":6,\"stocks\":0}", &j) && !parse_rules(j, &rg));
        CHECK(json::parse("{\"stage\":6,\"stocks\":6}", &j) && !parse_rules(j, &rg));
        CHECK(json::parse("{\"stage\":41,\"stocks\":3}", &j) && !parse_rules(j, &rg));            // 0x29: not netplay legal
        CHECK(json::parse("{\"stage\":6}", &j) && !parse_rules(j, &rg));

        /* ports "0,0,1,2": slot 0 has two pads */
        std::map<int, MemberPick> picks;
        picks[0] = mp;
        picks[1].p[0] = {1, 3};
        MatchRules fixed;
        fixed.stage = 6; fixed.stocks = 2;
        MatchSpec ms = resolve_match("0,0,1,2", picks, fixed, 99u);
        CHECK(ms.stage == 6 && ms.stocks == 2 && ms.players.size() == 4);
        CHECK(ms.players[0].fighter == 0x1D && ms.players[0].costume == 2 && ms.players[1].fighter == 0 && ms.players[1].costume == 4);
        CHECK(ms.players[2].fighter == 1 && ms.players[2].costume == 3);
        CHECK(ms.players[3].fighter >= 0 && fighter_valid(ms.players[3].fighter, true) && ms.players[3].costume < fighter_costumes(ms.players[3].fighter));   // unpicked -> Random
        CHECK(match_arg(ms).rfind("stage=6;stocks=2;p0=29:2;p1=0:4;p2=1:3;p3=", 0) == 0);
        MatchSpec ms2 = resolve_match("0,0,1,2", picks, fixed, 99u);
        CHECK(match_arg(ms) == match_arg(ms2));                                                       // same seed, same result
        bool varied = false;
        MatchRules rnd;
        for (unsigned sd = 1; sd < 40 && !varied; ++sd) {
            MatchSpec a = resolve_match("0,1", {}, rnd, sd);
            CHECK(stage_valid(a.stage, true) && a.stage >= 0 && a.players.size() == 2 && a.players[0].fighter >= 0 && a.players[1].fighter >= 0);
            CHECK(a.players[0].costume >= 0 && a.players[0].costume < fighter_costumes(a.players[0].fighter));
            varied = a.stage != resolve_match("0,1", {}, rnd, sd + 1).stage;
        }
        CHECK(varied);
        CHECK(resolve_match("0,1", {}, rnd, 5u, false).stage <= 8 && resolve_match("0,1", {}, rnd, 5u, false).players[0].fighter <= 0x0B);
        CHECK(resolve_match("0,1", {}, MatchRules{6, 99}, 1u).stocks == 5);
        CHECK(resolve_match("3,-1,-1,-1", picks, fixed, 1u).players.size() == 1);

        PortPlan pp;
        pp.ports = "0,0,1,2"; pp.players = 3;
        std::string lm = launch_json("remix", 1, 0, 7u, "", false, &pp, &ms);
        MatchSpec back;
        CHECK(json::parse(lm, &j) && parse_match(j, pp.ports, true, &back) && match_arg(back) == match_arg(ms));
        CHECK(!parse_match(j, "0,1,2", true, &back) && !parse_match(j, "0,0,1,2", false, &back));      // wrong port count / vanilla roster
        CHECK(json::parse(launch_json("remix", 1, 0, 7u), &j) && !parse_match(j, pp.ports, true, &back));   // older host: absent
        CHECK(json::parse("{\"match\":{\"stage\":6,\"stocks\":3,\"players\":[[0,0],[-1,0]]}}", &j) && !parse_match(j, "0,1", true, &back));
        CHECK(json::parse("{\"match\":{\"stage\":6,\"stocks\":3,\"players\":[[0,0],[1,9]]}}", &j) && !parse_match(j, "0,1", true, &back));
        CHECK(json::parse("{\"match\":{\"stage\":6,\"stocks\":3,\"players\":[[0,0],[1,3]]}}", &j) && parse_match(j, "0,1", true, &back) && match_arg(back) == "stage=6;stocks=3;p0=0:0;p1=1:3");
    }


    {   // lobby host migration: per-slot state follows the promoted member to slot 0
        std::map<int, std::string> m{{0, "oldhost"}, {1, "promoted"}, {3, "other"}};
        remap_slot_key(m, 1, 0);
        CHECK(m.size() == 2 && m[0] == "promoted" && m[3] == "other" && !m.count(1));   // old host's entry dropped, others kept
        std::map<int, int> e{{2, 5}};
        remap_slot_key(e, 1, 0);                                                         // nothing under `from`: unchanged
        CHECK(e.size() == 1 && e[2] == 5);
        std::map<int, int> g{{0, 9}, {2, 5}};
        remap_slot_key(g, 1, 0);                                                         // promoted had no state: departed host's state is dropped
        CHECK(g.size() == 1 && g[2] == 5);
        std::map<int, int> same{{0, 1}};
        remap_slot_key(same, 0, 0);
        CHECK(same.size() == 1 && same[0] == 1);
        CHECK(host_changed_text("Mia", false) == "Mia is now the host" && host_changed_text("Mia", true) == "You are now the host");
    }

    {   /* installer games-folder hand-off */
        Settings g;
        g.rom_folders = {"D:\\mine"};
        CHECK(merge_installer_games_dir(&g, "\xEF\xBB\xBF\"C:\\Users\\a\\Documents\\RetroHub Games\\\"\r\n"));
        CHECK(g.rom_folders.size() == 2 && g.rom_folders[0] == "D:\\mine" && g.rom_folders[1] == "C:\\Users\\a\\Documents\\RetroHub Games\\");
        CHECK(!merge_installer_games_dir(&g, "c:/users/A/documents/retrohub games\n"));   // duplicate: ignored
        CHECK(!merge_installer_games_dir(&g, "") && !merge_installer_games_dir(&g, "relative\\x") && g.rom_folders.size() == 2);
        Settings e;
        CHECK(merge_installer_games_dir(&e, "E:\\G") && e.rom_folders.size() == 1);
    }

    {   /* friends error mapping, invite result, queued-join replacement */
        CHECK(friends_error_text("bad-request", "friends-full") == "Your friends list is full.");
        CHECK(friends_error_text("bad-request", "target-full").find("That player") == 0);
        CHECK(!friends_error_text("bad-request", "too-many-requests").empty());
        CHECK(!friends_error_text("bad-request", "already-requested").empty());
        CHECK(!friends_error_text("bad-request", "undeliverable").empty());
        CHECK(friends_error_text("bad-request", "").empty() && friends_error_text("bad-request", "internal").empty());
        CHECK(friends_error_text("blocked", "") == "That player is blocked." && friends_error_text("weird", "") == "Friends: weird");
        CHECK(invite_result_text({"A", "B"}, {}, "", "ABC234") == "Invited A, B to room ABC234.");
        CHECK(invite_result_text({"A"}, {"C"}, "not-friends", "ABC234") == "Invited A to room ABC234. Could not invite C.");
        CHECK(invite_result_text({}, {"C"}, "rate-limited", "X").find("Slow down") == 0);
        CHECK(invite_result_text({}, {}, "", "X").empty());
        CHECK(queued_join_replaced_text("Mia", "Bo").find("Replaced the queued join (Mia's room)") == 0 && queued_join_replaced_text("", "Bo").find("another room") != std::string::npos);
    }

    std::printf(fails ? "FAILED (%d)\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
