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
    Settings t;
    CHECK(settings_from_json(settings_to_json(s), &t));
    CHECK(t.name == s.name && t.rom_folders == s.rom_folders && t.server == s.server);
    CHECK(t.input_delay == 4 && t.last_game == "mp2" && t.smash_exe == s.smash_exe);
    CHECK(t.gb_core == "gambatte" && t.fullscreen && t.sync_folder == s.sync_folder);
    CHECK(!t.sync_auto && t.sync_states);
    Settings u;
    CHECK(settings_from_json("{\"input_delay\": 99}", &u) && u.input_delay == 8 && u.delay_auto && u.name.empty());
    CHECK(u.gb_core == "sameboy" && u.sync_auto && !u.sync_states && u.rom_folders.empty());
    CHECK(!settings_from_json("{bad", &u));

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

    /* hello v2 (+ old v1 shape without keys) */
    Json j;
    PeerInfo h;
    CHECK(json_parse(hello_json("2", {{"mp1", "AAA"}, {"mp2", "BBB"}}, {"n64:aaa", "gb:x-1"}), &j));
    CHECK(j.str("version") == "2");
    CHECK(parse_hello(j, &h) && h.games.size() == 2 && h.games["mp2"] == "BBB");
    CHECK(h.keys.size() == 2 && h.keys[0] == "n64:aaa" && h.keys[1] == "gb:x-1");
    CHECK(json_parse("{\"kind\":\"hello\",\"version\":\"1\",\"games\":{\"mp1\":\"AAA\"}}", &j));
    PeerInfo old;
    CHECK(parse_hello(j, &old) && old.hello && old.keys.empty() && old.games["mp1"] == "AAA");
    CHECK(json_parse(hello_json("2", {}, {}), &j) && parse_hello(j, &h) && h.games.empty() && h.keys.empty());
    {   /* long key lists are split into <= 4096 byte messages and reassemble */
        std::vector<std::string> many;
        for (int i = 0; i < 500; ++i) many.push_back("gba:abcd-" + std::to_string(100000 + i));
        auto msgs = hello_messages("2", {{"mp1", "AAA"}}, many);
        CHECK(msgs.size() > 1);
        PeerInfo r;
        bool allok = true;
        for (auto& m : msgs) {
            allok = allok && m.size() <= 3100 && json_parse(m, &j) && parse_hello(j, &r);
        }
        CHECK(allok && r.keys == many && r.games["mp1"] == "AAA" && r.hello);
        CHECK(hello_messages("2", {}, {}).size() == 1);
    }
    CHECK(json_parse(launch_json("mp1", 3, 2, 77u), &j));
    CHECK(j.str("game") == "mp1" && (int)j.num("launch") == 3 && (int)j.num("seed") == 77 && j.str("core").empty());
    CHECK(json_parse(launch_json("gb:x-1", 1, 2, 5u, "gambatte"), &j) && j.str("core") == "gambatte");

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

    /* shelves */
    {
        std::vector<ShelfGame> g(9);
        g[0] = {"native:smash64", "Super Smash Bros.", "n64", false, 0, true, true, "Fighting"};
        g[1] = {"n64:1", "Mario Party", "n64", true, 300, true, false, "Party"};
        g[2] = {"n64:2", "banjo", "n64", false, 100, false, false, "Platformer"};
        g[3] = {"gba:1", "Wario Ware", "gba", true, 0, false, false, "Party"};
        g[4] = {"gb:1", "Tetris", "gb", false, 200, false, false, ""};
        g[5] = {"gc:1", "Melee", "gc", false, 0, false, false, "Bogus"};
        g[6] = {"gbc:1", "Crystal", "gbc", false, 0, false, false, ""};
        g[7] = {"n64:3", "Zelda", "n64", false, 0, false, false, "Adventure"};
        g[8] = {"n64:4", "Unknown Rom", "n64", false, 0, false, false, ""};
        auto sh = compose_shelves(g);
        std::vector<std::string> ids;
        for (auto& s2 : sh) ids.push_back(s2.id);
        std::vector<std::string> want = {"recent", "favs", "genre:Party", "genre:Fighting", "genre:Platformer", "genre:Adventure",
                                         "genre:Other:n64", "genre:Other:gc", "genre:Other:gbc", "genre:Other:gb"};
        CHECK(ids == want);
        CHECK(sh[0].items == std::vector<int>({1, 4, 2}));          // newest first
        CHECK(sh[1].items == std::vector<int>({1, 3}));             // favorites by title
        CHECK(sh[2].items == std::vector<int>({1, 3}));             // Party by title
        CHECK(sh[6].items == std::vector<int>({8}));                // Other, n64
        CHECK(sh[0].title == "Continue Playing" && sh[9].title == "Other - Game Boy" && sh[2].title == "Party");
        /* each game appears in exactly one genre shelf */
        std::vector<int> seen(g.size(), 0);
        for (auto& s2 : sh)
            if (s2.id.compare(0, 6, "genre:") == 0) for (int i : s2.items) ++seen[i];
        for (int n : seen) CHECK(n == 1);
        CHECK(compose_shelves({}).empty());
        CHECK(compose_shelves(g, 2)[0].items.size() == 2);
        CHECK(genre_order().size() == 11 && genre_order()[0] == "Party" && genre_order()[4] == "Platformer");
        CHECK(effective_genre("RPG") == "RPG" && effective_genre("") == "Other" && effective_genre("rpg") == "Other");
        std::vector<ShelfGame> odd(1);
        odd[0] = {"x:1", "Odd", "ps2", false, 0, false, false, ""};
        auto so = compose_shelves(odd);
        CHECK(so.size() == 1 && so[0].id == "genre:Other" && so[0].items.size() == 1);
        std::vector<ShelfGame> ord(3);   // display order follows genre_order(), not input order
        ord[0] = {"a", "A", "n64", false, 0, false, false, "Action"};
        ord[1] = {"b", "B", "n64", false, 0, false, false, "Racing"};
        ord[2] = {"c", "C", "n64", false, 0, false, false, "Party"};
        auto sord = compose_shelves(ord);
        CHECK(sord.size() == 3 && sord[0].id == "genre:Party" && sord[1].id == "genre:Racing" && sord[2].id == "genre:Action");
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
        CHECK(format_ago_ms(100000, 90000) == "just now" && format_ago_ms(1000 * 600, 0) == "10 min ago" && format_ago_ms(86400000LL * 2, 0) == "2d ago");
        CHECK(normalize_friend_code("7kq2m9xd") == "RH-7KQ2-M9XD");
        CHECK(normalize_friend_code(" rh-7kq2-m9xd ") == "RH-7KQ2-M9XD");
        CHECK(normalize_friend_code("RH7KQ2M9XD") == "RH-7KQ2-M9XD" && normalize_friend_code("7KQ2 M9XD") == "RH-7KQ2-M9XD");
        CHECK(normalize_friend_code("").empty() && normalize_friend_code("RH-7KQ2-M9X").empty() && normalize_friend_code("RH-7KQ!-M9XD").empty());
        CHECK(!invite_expired(5000, 4999) && invite_expired(5000, 5000) && !invite_expired(0, 99999));
        CHECK(invite_seconds_left(5000, 4000) == 1 && invite_seconds_left(5000, 5000) == 0 && invite_seconds_left(31000, 1000) == 30);
        CHECK(lobby_joinable(2, 4, false) && !lobby_joinable(4, 4, false) && !lobby_joinable(2, 4, true) && !lobby_joinable(0, 4, false));
        Settings fs;
        CHECK(fs.show_friends && !fs.appear_offline && fs.notif_sound);
        fs.show_friends = false; fs.appear_offline = true; fs.notif_sound = false;
        Settings ft;
        CHECK(settings_from_json(settings_to_json(fs), &ft) && !ft.show_friends && ft.appear_offline && !ft.notif_sound);
    }

    std::printf(fails ? "FAILED (%d)\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
