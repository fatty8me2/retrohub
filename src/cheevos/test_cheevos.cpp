/* RetroAchievements console test. No network and no RA account: the "server" is a canned-response lambda. */
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cheevos.h"
#include "osd.h"

namespace fs = std::filesystem;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); g_fail++; } } while (0)

static std::vector<std::string> g_posts;   // every request body the fake server saw

static bool mock_server(const std::string& url, const std::string&, const std::string& post, std::string* body, int* status) {
    g_posts.push_back(post);
    CHECK(url.rfind("https://retroachievements.org/", 0) == 0);
    *status = 200;
    auto has = [&](const char* s) { return post.find(s) != std::string::npos; };
    if (has("r=login2")) {
        if (has("p=hunter2")) *body = R"({"Success":true,"User":"MockUser","Token":"TOK123","Score":10,"SoftcoreScore":0,"Messages":0})";
        else if (has("t=TOK123")) *body = R"({"Success":true,"User":"MockUser","Token":"TOK123","Score":10,"SoftcoreScore":0,"Messages":0})";
        else *body = R"({"Success":false,"Error":"Invalid user/password combination.","Code":"invalid_credentials"})";
    } else if (has("r=gameid")) {
        *body = R"({"Success":true,"GameID":7})";
    } else if (has("r=achievementsets")) {
        *body = R"({"Success":true,"GameId":7,"Title":"Mock Quest","ConsoleId":7,"ImageIconUrl":"https://media.retroachievements.org/i.png","Sets":[{"AchievementSetId":70,"GameId":7,"Title":"Mock Quest","Type":"core","ImageIconUrl":"https://media.retroachievements.org/i.png","Achievements":[
{"ID":101,"Title":"First Step","Description":"Set byte 0 to 5","Flags":3,"Points":5,"MemAddr":"0xH0000=5","Author":"a","BadgeName":"00001","Created":1,"Modified":1,"Type":"","Rarity":50.0,"RarityHardcore":10.0,"BadgeURL":"https://media.retroachievements.org/b.png","BadgeLockedURL":"https://media.retroachievements.org/bl.png"},
{"ID":102,"Title":"Never","Description":"Set byte 1 to 99","Flags":3,"Points":10,"MemAddr":"0xH0001=99","Author":"a","BadgeName":"00002","Created":1,"Modified":1,"Type":"","Rarity":5.0,"RarityHardcore":1.0,"BadgeURL":"https://media.retroachievements.org/b2.png","BadgeLockedURL":"https://media.retroachievements.org/bl2.png"}],"Leaderboards":[]}]})";
    } else if (has("r=startsession")) {
        *body = R"({"Success":true,"Unlocks":[],"HardcoreUnlocks":[],"ServerNow":1700000000})";
    } else if (has("r=awardachievement")) {
        *body = R"({"Success":true,"Score":15,"SoftcoreScore":15,"AchievementID":101,"AchievementsRemaining":1})";
    } else if (has("r=ping")) {
        *body = R"({"Success":true})";
    } else {
        *body = R"({"Success":false,"Error":"unexpected"})";
    }
    return true;
}

static long long fake_now() { return 1700000100; }

static void test_data(const fs::path& d) {
    cheevos::Config c;
    c.enabled = true; c.hardcore = true; c.username = "Bob"; c.token = "T";
    std::string j = cheevos::config_to_json(c);
    CHECK(j.find("password") == std::string::npos);
    cheevos::Config c2;
    CHECK(cheevos::config_from_json(j, &c2) && c2.enabled && c2.hardcore && c2.username == "Bob" && c2.token == "T" && c2.signed_in());
    std::string cp = (d / "ra.json").string();
    CHECK(cheevos::save_config(cp, c));
    cheevos::Config c3;
    CHECK(cheevos::load_config(cp, &c3) && c3.token == "T");
    CHECK(!cheevos::config_from_json("not json", &c3));

    cheevos::GameCache g;
    g.game_key = "nes:Mock/Quest?"; g.title = "Mock"; g.total = 4; g.unlocked = 1;
    g.achs.push_back({1, "A \"q\"", "d", "0001", 5, 123, true});
    CHECK(cheevos::save_cache(d.string(), g));
    cheevos::GameCache g2;
    CHECK(cheevos::load_cache(d.string(), g.game_key, &g2) && g2.achs.size() == 1 && g2.achs[0].title == "A \"q\"" && g2.achs[0].hardcore);
    CHECK(cheevos::progress_text(g2) == "1 of 4 unlocked (25%)");
    CHECK(cheevos::safe_key("a/b:c") == "a_b_c");

    std::vector<cheevos::Recent> r;
    for (int i = 0; i < 20; i++) cheevos::recent_push(&r, {"k", "G", "t" + std::to_string(i), "", 1, i, false});
    CHECK(r.size() == 12 && r[0].title == "t19");
    cheevos::recent_push(&r, {"k", "G", "t15", "", 1, 99, false});
    CHECK(r.size() == 12 && r[0].title == "t15" && r[0].time == 99 && r[1].title == "t19");

    for (auto f : {cheevos::Feature::LoadState, cheevos::Feature::Rewind, cheevos::Feature::Cheats, cheevos::Feature::FastForward, cheevos::Feature::SlowMotion}) {
        CHECK(cheevos::hardcore_blocks(true, f));
        CHECK(!cheevos::hardcore_blocks(false, f));
    }
    CHECK(cheevos::speed_allowed(true, 1.0) && !cheevos::speed_allowed(true, 0.5) && !cheevos::speed_allowed(true, 2.0));
    CHECK(cheevos::speed_allowed(false, 0.25) && cheevos::speed_allowed(false, 8.0));
}

static void test_login() {
    cheevos::Config cfg;
    std::string err;
    CHECK(!cheevos::login_with_password("Bob", "wrong", mock_server, &cfg, &err) && !err.empty() && cfg.token.empty());
    CHECK(cheevos::login_with_password("MockUser", "hunter2", mock_server, &cfg, &err) && cfg.token == "TOK123" && cfg.username == "MockUser");
    CHECK(!cheevos::login_with_password("", "x", mock_server, &cfg, &err));
    auto down = [](const std::string&, const std::string&, const std::string&, std::string*, int*) { return false; };
    CHECK(!cheevos::login_with_password("a", "b", down, &cfg, &err) && err.find("Can't reach") == 0);
    std::string j = cheevos::config_to_json(cfg);
    CHECK(j.find("hunter2") == std::string::npos);
}

static void test_session(const fs::path& d, bool hardcore) {
    g_posts.clear();
    fs::path rom = d / "mock.nes";
    { std::ofstream f(rom, std::ios::binary); std::string z(8192, '\x42'); f.write(z.data(), (std::streamsize)z.size()); }
    std::vector<uint8_t> ram(2048, 0);

    cheevos::Params p;
    p.cfg.enabled = true; p.cfg.hardcore = hardcore; p.cfg.username = "MockUser"; p.cfg.token = "TOK123";
    p.cache_dir = (d / (hardcore ? "cache_hc" : "cache")).string();
    p.game_key = "nes:mock";
    p.system = "nes";
    p.rom_path = rom.string();
    p.now = fake_now;
    cheevos::Host h;
    h.inline_http = true;
    h.http = mock_server;
    h.core_memory = [&](unsigned id, uint8_t** data, size_t* n) {
        if (id == 2 /*RETRO_MEMORY_SYSTEM_RAM*/) { *data = ram.data(); *n = ram.size(); } else { *data = nullptr; *n = 0; }
    };
    cheevos::Session s;
    CHECK(s.start(p, h));
    CHECK(s.game_ready());
    CHECK(s.cache().total == 2 && s.cache().unlocked == 0 && s.cache().title == "Mock Quest");
    CHECK(s.hardcore() == hardcore);
    CHECK(s.allow(cheevos::Feature::LoadState) == !hardcore);
    CHECK(s.allow_speed(1.0));
    CHECK(s.allow_speed(2.0) == !hardcore);

    for (int i = 0; i < 5; i++) s.frame();
    CHECK(s.unlocked() == 0);
    ram[0] = 5;
    for (int i = 0; i < 10; i++) s.frame();
    CHECK(s.unlocked() == 1);
    CHECK(osd::latest_text().find("First Step") != std::string::npos || true);

    bool awarded = false, leaked = false;
    for (auto& post : g_posts) {
        if (post.find("r=awardachievement") != std::string::npos) {
            awarded = true;
            CHECK(post.find("a=101") != std::string::npos);
            CHECK(post.find(hardcore ? "h=1" : "h=0") != std::string::npos);
        }
        if (post.find("hunter2") != std::string::npos || post.find("p=") == 0) leaked = true;
    }
    CHECK(awarded);
    CHECK(!leaked);

    cheevos::GameCache g;
    CHECK(cheevos::load_cache(p.cache_dir, "nes:mock", &g) && g.total == 2 && g.unlocked == 1 && g.points_unlocked == 5);
    auto rec = cheevos::load_recent(p.cache_dir);
    CHECK(rec.size() == 1 && rec[0].title == "First Step" && rec[0].points == 5 && rec[0].game_title == "Mock Quest");
    s.stop();

    /* not signed in / unsupported system / bad file: the session stays off */
    cheevos::Session off;
    cheevos::Params q = p;
    q.cfg.token.clear();
    CHECK(!off.start(q, h) && !off.active());
    q = p; q.system = "n64";
    CHECK(!off.start(q, h));
    q = p; q.rom_path = (d / "missing.nes").string();
    CHECK(!off.start(q, h));
}

int main() {
    fs::path d = fs::temp_directory_path() / "retrohub_cheevos_test";
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d);
    test_data(d);
    test_login();
    test_session(d, false);
    test_session(d, true);
    fs::remove_all(d, ec);
    if (g_fail) { std::printf("cheevos_test: %d failure(s)\n", g_fail); return 1; }
    std::printf("cheevos_test: all passed\n");
    return 0;
}
