#include <cstdio>
#include <cstring>

#include "games.h"
#include "netplay_3ds.h"
#include "netplay_3ds_join.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

using namespace hubnp3ds;

int main() {
    /* Azahar join helper (the Qt frontend ignores -m) */
    CHECK(join_state("stage:driver-start\r\n") == "");
    CHECK(join_state("stage:driver-start\nstage:connect-clicked\njoined\n") == "joined");
    CHECK(join_state("error:join timed out\n").rfind("error:", 0) == 0);
    CHECK(join_driver_args(42, "s.txt", "Host_1", "", "127.0.0.1", 24990, 120).find("-Port 24990") != std::string::npos);
    CHECK(std::string(join_driver_script()).find("DirectConnectWindow") != std::string::npos);
    /* launch spec key */
    Plan p;
    p.port = 24872;
    p.password = "";
    p.expected = 3;
    std::string msg = hubui::launch_json("pokex", 2, 0, 5u, "", false, nullptr, nullptr, plan_json_fragment(p));
    hubui::Json j;
    CHECK(json::parse(msg, &j));
    Plan q;
    CHECK(parse_plan(j, &q) && q.port == 24872 && q.expected == 3 && q.password.empty());
    CHECK(j.str("game") == "pokex" && j.num("launch") == 2);   // existing keys untouched
    hubui::Json none;
    CHECK(json::parse(hubui::launch_json("pokex", 1, 0, 5u), &none) && !parse_plan(none, &q));   // older host
    Plan bad = p;
    bad.expected = 1;
    CHECK(json::parse(hubui::launch_json("x", 1, 0, 1u, "", false, nullptr, nullptr, plan_json_fragment(bad)), &none) && !parse_plan(none, &q));
    bad = p;
    bad.port = 80;
    CHECK(json::parse(hubui::launch_json("x", 1, 0, 1u, "", false, nullptr, nullptr, plan_json_fragment(bad)), &none) && !parse_plan(none, &q));
    bad = p;
    bad.password = "a@b:c";   // would corrupt the -m argument
    CHECK(json::parse(hubui::launch_json("x", 1, 0, 1u, "", false, nullptr, nullptr, plan_json_fragment(bad)), &none) && !parse_plan(none, &q));
    std::string pw = random_password(7u);
    CHECK(pw.size() == 12 && pw == random_password(7u) && pw != random_password(8u));

    /* nicknames: Azahar accepts 4..20 of [A-Za-z0-9 ._-]; unique per slot */
    CHECK(nickname("Player", 0) == "Player_1" && nickname("Player", 2) == "Player_3");
    CHECK(nickname("a", 1) == "ax_2" || nickname("a", 1).size() >= 4);
    CHECK(nickname("Ma\xC3\xA9 @:x y", 0) == "Ma_x_y_1");
    CHECK(nickname("ABCDEFGHIJKLMNOPQRSTUVWXYZ", 0).size() <= 20);
    for (int s = 0; s < 4; ++s) { auto n = nickname("", s); CHECK(n.size() >= 4 && n.size() <= 20); }

    /* command lines */
    CHECK(multiplayer_arg("Player_1", "", "127.0.0.1", 24872) == "-m \"Player_1@127.0.0.1:24872\"");
    CHECK(multiplayer_arg("Player_1", "pw", "127.0.0.1", 5) == "-m \"Player_1:pw@127.0.0.1:5\"");
    std::string ra = room_args(24872, "", 8, "C:\\x\\room.log", "");
    CHECK(ra.find("--port 24872") != std::string::npos && ra.find("--preferred-app RetroHub") != std::string::npos &&
          ra.find("--preferred-app-id 0") != std::string::npos && ra.find("--password") == std::string::npos &&
          ra.find("--log-file \"C:\\x\\room.log\"") != std::string::npos);   // azahar-room refuses to start without preferred-app
    CHECK(room_args(1, "pw", 4, "", "").find("--password pw") != std::string::npos);
    CHECK(session_room("ABC234", 3) == "ABC234-3");

    /* config prefill for Direct Connect */
    std::string ini = config_patch("[UI]\nfirstStart=false\n", "127.0.0.1", 24872, "Player_1");
    CHECK(hubui::ini_get(ini, "UI", "Multiplayer\\ip", "") == "127.0.0.1" && hubui::ini_get(ini, "UI", "Multiplayer\\port", "") == "24872" &&
          hubui::ini_get(ini, "UI", "Multiplayer\\nickname", "") == "Player_1" && hubui::ini_get(ini, "UI", "firstStart", "") == "false");

    /* fragmentation: sizes around the 1100-byte hubnet limit, out of order, duplicates, interleaved sources */
    std::string big;
    for (int i = 0; i < 4000; ++i) big += (char)(i * 7);
    for (size_t len : {0u, 1u, 1094u, 1095u, 1096u, 2190u, 4000u}) {
        auto fr = fragment(9, (const uint8_t*)big.data(), len);
        CHECK(!fr.empty());
        for (auto& f : fr) CHECK(f.size() <= 1100);
        Reassembler r;
        std::string out;
        bool done = false;
        for (size_t k = fr.size(); k-- > 0;) {   // reversed
            const std::string& f = fr[k];
            done = r.add(1, (const uint8_t*)f.data(), f.size(), 100, &out);
            if (k) CHECK(!done);
            if (fr.size() > 1 && k == fr.size() - 1) CHECK(!r.add(1, (const uint8_t*)f.data(), f.size(), 100, &out));   // duplicate
        }
        CHECK(done && out == big.substr(0, len) && r.pending() == 0);
    }
    {
        auto a = fragment(1, (const uint8_t*)big.data(), 2500), b = fragment(1, (const uint8_t*)big.data() + 1, 2500);   // same id, different senders
        Reassembler r;
        std::string o1, o2;
        bool d1 = false, d2 = false;
        for (size_t i = 0; i < a.size(); ++i) {
            d1 = r.add(0, (const uint8_t*)a[i].data(), a[i].size(), 5, &o1);
            d2 = r.add(2, (const uint8_t*)b[i].data(), b[i].size(), 5, &o2);
        }
        CHECK(d1 && d2 && o1 == big.substr(0, 2500) && o2 == big.substr(1, 2500));
        auto c = fragment(5, (const uint8_t*)big.data(), 3000);   // stale partial dropped
        std::string o;
        CHECK(!r.add(0, (const uint8_t*)c[0].data(), c[0].size(), 10, &o) && r.pending() == 1);
        CHECK(!r.add(0, (const uint8_t*)"Fxx", 3, 5000, &o) && r.pending() == 0);
        CHECK(!r.add(0, (const uint8_t*)"Z12345", 6, 5000, &o));   // not a fragment
    }

    /* catalog: all 8 3DS Pokemon games are online-capable for 2 players */
    int n = 0;
    for (const hub::GameInfo& g : hub::catalog())
        if (g.engine == hub::Engine::Azahar) { ++n; CHECK(g.online && hub::online_max_for(g) == 2 && g.max_players >= 2); }
    CHECK(n == 8);

    std::printf(g_fail ? "FAILED (%d)\n" : "np3ds tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
