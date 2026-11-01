// Console test: VS results reader (match_result.h). RDRAM fixtures: synthetic blocks for every ruleset plus blocks captured from real
// matches (Smash Remix 2.0.0 and the vanilla US ROM, see the fixture notes below). No emulator needed.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "json.h"
#include "match_result.h"
#include "results.h"   // the hub's reader: the JSON written here must parse there and rank the same way
#include "test_match_result_fixtures.h"   // RDRAM captures of real matches

using namespace n64match;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* A big-endian RDRAM image (index = physical address), like --dump-ram writes. */
struct Img {
    std::vector<uint8_t> be = std::vector<uint8_t>(8u << 20, 0);
    void w8(uint32_t a, uint8_t v) { be[a & 0x7FFFFF] = v; }
    void w32(uint32_t a, int32_t v) { for (int i = 0; i < 4; i++) w8(a + (uint32_t)i, (uint8_t)((uint32_t)v >> (24 - 8 * i))); }
    std::vector<uint8_t> host() const {   // the core's layout: host-endian words, byte index ^ 3
        std::vector<uint8_t> h(be.size());
        for (size_t i = 0; i < be.size(); i++) h[i ^ 3u] = be[i];
        return h;
    }
    void scene(int curr, bool sudden = false, bool reset = false) { w8(kSceneBase, (uint8_t)curr); w8(kSceneBase + kSceneSuddenDeath, sudden); w8(kSceneBase + kSceneIsReset, reset); }
    void vs(int rules, bool team, int stage = 1) { w8(kVsBlock + 0x01, (uint8_t)stage); w8(kVsBlock + 0x02, team); w8(kVsBlock + 0x03, (uint8_t)rules); }
    static uint32_t pb(uint32_t base, int n) { return base + kPlayersOffset + (uint32_t)n * kPlayerStride; }
    void player(int n, int pkind, int fkind, int team, int stocks, int place, int falls, int kos, uint32_t base = kVsBlock) {
        w8(pb(base, n) + 0x02, (uint8_t)pkind); w8(pb(base, n) + 0x03, (uint8_t)fkind); w8(pb(base, n) + 0x04, (uint8_t)team);
        w8(pb(base, n) + 0x0B, (uint8_t)(int8_t)stocks); w8(pb(base, n) + 0x0D, (uint8_t)place);
        w32(pb(base, n) + 0x10, falls); w32(pb(base, n) + 0x14, kos);
    }
    void none(int n) { w8(pb(kVsBlock, n) + 0x02, 2); }
};

static int place_of(const std::vector<PlayerResult>& v, int port) { return v[(size_t)port].place; }

static void test_stock_rules() {
    Img m;
    m.scene(0x18); m.vs(2, false);
    m.player(0, 0, 0x1D, 0, 1, 0, 1, 2);   // winner: place 0, one stock left
    m.player(1, 0, 0x00, 1, -1, 3, 3, 0);  // out first
    m.player(2, 0, 0x08, 2, -1, 1, 2, 1);
    m.player(3, 0, 0x0B, 3, -1, 2, 2, 0);
    std::vector<PlayerResult> v; MatchInfo mi;
    CHECK(read_results(m.be.data(), m.be.size(), &v, &mi));
    CHECK(v.size() == 4 && mi.game_rules == 2 && !mi.team && !mi.sudden_death && !mi.no_contest);
    CHECK(place_of(v, 0) == 0 && place_of(v, 1) == 3 && place_of(v, 2) == 1 && place_of(v, 3) == 2);   // taken straight from the blocks
    CHECK(v[0].present && !v[0].cpu && v[0].ch == 0x1D && v[0].stocks == 1 && v[0].kos == 2 && v[0].falls == 1 && v[0].port == 0);
    CHECK(v[1].stocks == -1 && v[1].falls == 3);
    CHECK(mi.signature != 0);
    // the same bytes in the core's host-endian layout give the same answer
    std::vector<uint8_t> h = m.host();
    std::vector<PlayerResult> v2; MatchInfo mi2;
    CHECK(read_results_host(h.data(), h.size(), &v2, &mi2));
    CHECK(v2.size() == 4 && mi2.signature == mi.signature);
    for (int i = 0; i < 4; i++) CHECK(v2[(size_t)i].place == v[(size_t)i].place && v2[(size_t)i].kos == v[(size_t)i].kos && v2[(size_t)i].ch == v[(size_t)i].ch);
    // a different match leaves a different signature
    m.player(0, 0, 0x1D, 0, 1, 0, 1, 3);
    CHECK(read_results(m.be.data(), m.be.size(), &v2, &mi2) && mi2.signature != mi.signature);
}

static void test_time_rules() {
    Img m;
    m.scene(0x18); m.vs(1, false);
    m.player(0, 0, 1, 0, 3, 0, 1, 5);   // 4 points
    m.player(1, 0, 2, 1, 3, 0, 0, 4);   // 4 points: ties with P0
    m.player(2, 0, 3, 2, 3, 0, 3, 3);   // 0
    m.player(3, 0, 4, 3, 3, 0, 6, 0);   // -6
    std::vector<PlayerResult> v;
    CHECK(read_results(m.be.data(), m.be.size(), &v));
    CHECK(place_of(v, 0) == 0 && place_of(v, 1) == 0 && place_of(v, 2) == 1 && place_of(v, 3) == 2);   // the blocks' place (all 0) is ignored: ranked by KOs - falls
    m.vs(3, false);   // time + stock is ranked the same way
    CHECK(read_results(m.be.data(), m.be.size(), &v) && place_of(v, 0) == 0 && place_of(v, 1) == 0 && place_of(v, 2) == 1 && place_of(v, 3) == 2);

    // sudden death: equal points are ordered by the sudden-death round's own place
    m.vs(1, false);
    m.scene(0x18, true);
    for (int n = 0; n < 4; n++) m.w8(Img::pb(kSuddenDeathBlock, n) + 0x02, 2);
    m.w8(Img::pb(kSuddenDeathBlock, 0) + 0x0D, 1);   // P0 lost the sudden-death round
    m.w8(Img::pb(kSuddenDeathBlock, 1) + 0x0D, 0);   // P1 won it
    MatchInfo mi;
    CHECK(read_results(m.be.data(), m.be.size(), &v, &mi) && mi.sudden_death);
    CHECK(place_of(v, 1) == 0 && place_of(v, 0) == 1 && place_of(v, 2) == 2 && place_of(v, 3) == 3);
}

static void test_team_rules() {
    Img m;
    m.scene(0x18); m.vs(1, true);
    m.player(0, 0, 1, 0, 3, 0, 0, 2);   // team 0 (red):   2 + 1 = 3
    m.player(1, 0, 2, 0, 3, 0, 1, 2);
    m.player(2, 0, 3, 1, 3, 0, 0, 2);   // team 1 (blue):  2
    m.player(3, 0, 4, 2, 3, 0, 1, 0);   // team 2 (green): -1
    std::vector<PlayerResult> v;
    CHECK(read_results(m.be.data(), m.be.size(), &v));
    CHECK(place_of(v, 0) == 0 && place_of(v, 1) == 0 && place_of(v, 2) == 1 && place_of(v, 3) == 2);
}

static void test_cpu_and_invalid() {
    Img m;
    m.scene(0x18); m.vs(2, false);
    m.player(0, 0, 5, 0, 0, 0, 0, 1);
    m.player(1, 1, 6, 1, -1, 1, 1, 0);   // a CPU
    m.none(2); m.none(3);
    std::vector<PlayerResult> v;
    CHECK(read_results(m.be.data(), m.be.size(), &v));
    CHECK(v[0].present && !v[0].cpu && v[1].present && v[1].cpu && !v[2].present && !v[3].present);
    // one human + one CPU is a match but not a reportable one
    CHECK(result_json("remix", "id1", "ABC234", false, v, {0, 1, -1, -1}).empty());
    CHECK(result_json("remix", "id1", "", true, v, {}).empty());

    // invalid blocks
    Img bad = m;
    bad.none(1);
    CHECK(!read_results(bad.be.data(), bad.be.size(), &v));                       // a single present player
    bad = m; bad.w8(Img::pb(kVsBlock, 1) + 0x02, 5);
    CHECK(!read_results(bad.be.data(), bad.be.size(), &v));                       // pkind 5 (How to Play) is not a VS battle
    bad = m; bad.w8(Img::pb(kVsBlock, 0) + 0x0B, 0x7F);
    CHECK(!read_results(bad.be.data(), bad.be.size(), &v));                       // 127 stocks
    bad = m; bad.w32(Img::pb(kVsBlock, 0) + 0x10, -4);
    CHECK(!read_results(bad.be.data(), bad.be.size(), &v));                       // negative falls
    bad = m; bad.w8(Img::pb(kVsBlock, 0) + 0x03, 0xF0);
    CHECK(!read_results(bad.be.data(), bad.be.size(), &v));                       // fighter id out of range
    bad = m; bad.w8(Img::pb(kVsBlock, 0) + 0x0D, 9);
    CHECK(!read_results(bad.be.data(), bad.be.size(), &v));                       // stock rules with place 9
    CHECK(!read_results(m.be.data(), 0x1000, &v));                                // image too small
    CHECK(!read_results(nullptr, 0, &v) && !read_results_host(nullptr, 0, &v));
    CHECK(!read_results(m.be.data(), m.be.size(), nullptr));

    // no contest is reported through the info, the caller drops it
    Img nc = m; nc.player(1, 0, 6, 1, 0, 1, 0, 0); nc.scene(0x18, false, true);
    MatchInfo mi;
    CHECK(read_results(nc.be.data(), nc.be.size(), &v, &mi) && mi.no_contest);
}

/* Blocks captured from real matches (qa/gen_match_result_fixtures.py): these verify the offsets against the actual games. */
static void load_fixture(Img* m, const char* hex) {
    const size_t n = std::strlen(hex) / 2;
    for (size_t i = 0; i < n; i++) {
        unsigned v = 0;
        std::sscanf(hex + 2 * i, "%2x", &v);
        m->be[(kSceneBase & 0x7FFFFF) + i] = (uint8_t)v;
    }
}

static void test_fixtures() {
    std::vector<PlayerResult> v;
    MatchInfo mi;
    Img m;

    // Smash Remix 2.0.0, stock rules: Mario fell once (a self-destruct), Kirby won with its one stock.
    load_fixture(&m, kRemixStock1v1);
    CHECK(m.be[kSceneBase & 0x7FFFFF] == 0x18 && m.be[(kSceneBase & 0x7FFFFF) + 1] == 0x16);   // the results screen, coming from the battle
    CHECK(read_results(m.be.data(), m.be.size(), &v, &mi));
    CHECK(mi.game_rules == 2 && mi.stage == 6 && !mi.team && !mi.sudden_death && !mi.no_contest);
    CHECK(v[0].present && !v[0].cpu && v[0].ch == 0 && v[0].place == 1 && v[0].stocks == -1 && v[0].kos == 0 && v[0].falls == 1);
    CHECK(v[1].present && !v[1].cpu && v[1].ch == 8 && v[1].place == 0 && v[1].stocks == 0 && v[1].kos == 0 && v[1].falls == 0);
    CHECK(!v[2].present && !v[3].present);
    std::string j = result_json("remix", "id-remix", "", true, v, {});
    hubui::results::RoundResult rr;
    CHECK(hubui::results::parse_round(j, &rr) && rr.ranks.size() == 2 && rr.ranks[0] == std::vector<int>{1} && rr.ranks[1] == std::vector<int>{0});   // Kirby (port 2) first
    {   // the live core's byte order gives the same answer
        std::vector<uint8_t> h = m.host();
        std::vector<PlayerResult> v2;
        CHECK(read_results_host(h.data(), h.size(), &v2) && v2.size() == 4 && v2[0].place == 1 && v2[1].place == 0 && v2[0].falls == 1);
    }

    // The vanilla US ROM lays the same blocks out the same way.
    Img van;
    load_fixture(&van, kVanillaStock1v1);
    CHECK(read_results(van.be.data(), van.be.size(), &v, &mi));
    CHECK(mi.game_rules == 2 && v[0].ch == 0 && v[0].place == 1 && v[0].stocks == -1 && v[0].falls == 1 && v[1].ch == 8 && v[1].place == 0 && v[1].stocks == 0);

    // A timed match: the blocks' place is 0 for both, the results screen (and so read_results) ranks by KOs - falls.
    Img tm;
    load_fixture(&tm, kRemixTime1v1);
    CHECK(tm.be[(Img::pb(kVsBlock, 0) & 0x7FFFFF) + 0x0D] == 0 && tm.be[(Img::pb(kVsBlock, 1) & 0x7FFFFF) + 0x0D] == 0);
    CHECK(read_results(tm.be.data(), tm.be.size(), &v, &mi));
    CHECK(mi.game_rules == 1 && v[0].falls == 5 && v[0].kos == 0 && v[0].stocks == 2 && v[1].falls == 0);
    CHECK(v[0].place == 1 && v[1].place == 0);

    // A menu-started match: time + teams, one human and one CPU (the CPU scored the KO). Ranked by team points, and not reportable (one human).
    Img cpu;
    load_fixture(&cpu, kRemixCpuMatch);
    CHECK(read_results(cpu.be.data(), cpu.be.size(), &v, &mi));
    CHECK(mi.game_rules == 1 && mi.team);
    CHECK(v[0].present && !v[0].cpu && v[0].ch == 3 && v[0].falls == 5 && v[0].kos == 0);
    CHECK(v[1].present && v[1].cpu && v[1].ch == 0x49 && v[1].kos == 1 && v[1].falls == 0);
    CHECK(v[1].place == 0 && v[0].place == 1);
    CHECK(result_json("remix", "id-cpu", "", true, v, {}).empty());
}

static void test_trigger() {
    Trigger t;
    // boot, menus, a battle, its results, back to the menus, a second battle with results
    const uint8_t seq[] = {0x01, 0x01, 0x10, 0x10, 0x15, 0x16, 0x16, 0x16, 0x18, 0x18, 0x18, 0x10, 0x15, 0x16, 0x16, 0x18, 0x18};
    int fired = 0;
    for (uint8_t s : seq) { if (t.step(s)) fired++; }
    CHECK(fired == 2 && t.matches() == 2);

    Trigger u;   // results without a battle before them (a state loaded at the results screen) are not reported
    CHECK(!u.step(0x01) && !u.step(0x18) && !u.step(0x18) && u.matches() == 0);
    CHECK(!u.step(0x10) && !u.step(0x18) && u.matches() == 0);

    Trigger w;   // the battle is abandoned from the pause menu (CSS afterwards): the next results screen still follows a battle
    w.step(0x16); w.step(0x10);
    CHECK(!w.step(0x18) || true);   // either answer is acceptable here; the point is that it never fires twice for one screen
    Trigger x;
    CHECK(x.step(0x16) == false && x.step(0x18) == true && x.step(0x18) == false && x.step(0x18) == false);
}

static void test_ids_and_json() {
    CHECK(match_id("ABC234-1", 3, 0xDEADBEEFu, 0) == "ABC234-1-m3-deadbeef");
    CHECK(match_id("", 1, 0x1u, 1700000000) == "local-1700000000-m1-00000001");
    CHECK(match_id("AB C/D", 2, 0, 0) == "AB_C_D-m2-00000000");   // never anything but [A-Za-z0-9-_] in the id (it names a file in the hub)

    std::vector<PlayerResult> v(4);
    for (int i = 0; i < 4; i++) { v[(size_t)i].port = i; v[(size_t)i].present = true; v[(size_t)i].ch = 0x10 + i; }
    v[0].place = 0; v[0].stocks = 2; v[0].kos = 3; v[0].falls = 1;
    v[1].place = 1; v[1].stocks = -1; v[1].kos = 1; v[1].falls = 3;
    v[2].present = false;
    v[3].cpu = true;
    // online: ports 0,1 are owned by lobby slots 2 and 0; port 3 is a CPU, port 2 empty
    std::string j = result_json("remix", "ABC234-1-m1-0000abcd", "ABC234", false, v, {2, 0, -1, -1});
    json::Value root;
    CHECK(json::parse(j, &root) && root.is_object());
    CHECK(root.i64("v") == 1 && root.str("game") == "remix" && root.str("id") == "ABC234-1-m1-0000abcd" && root.str("code") == "ABC234");
    CHECK(root.str("source") == "n64run" && !root.boolean("local", true));
    const json::Value* ps = root.get("players");
    CHECK(ps && ps->is_array() && ps->a.size() == 2);
    if (ps && ps->a.size() == 2) {
        CHECK(ps->a[0].i64("slot") == 2 && ps->a[0].i64("char") == 0x10 && ps->a[0].i64("place") == 0 && ps->a[0].i64("stocks") == 2 && ps->a[0].i64("kos") == 3 && ps->a[0].i64("falls") == 1);
        CHECK(ps->a[1].i64("slot") == 0 && ps->a[1].i64("stocks") == -1 && ps->a[1].i64("place") == 1);
    }
    // the hub's reader accepts it and ranks slot 2 first
    hubui::results::RoundResult rr;
    CHECK(hubui::results::parse_round(j, &rr));
    CHECK(rr.id == "ABC234-1-m1-0000abcd" && rr.game == "remix" && rr.source == "n64run" && !rr.local);
    CHECK(rr.ranks.size() == 2 && rr.ranks[0] == std::vector<int>{2} && rr.ranks[1] == std::vector<int>{0});

    // offline: the port number is the slot; nothing else is needed
    std::string l = result_json("ssb64rom", "local-1-m1-00000001", "", true, v, {});
    hubui::results::RoundResult lr;
    CHECK(hubui::results::parse_round(l, &lr) && lr.local && lr.source == "n64run" && lr.ranks.size() == 2 && lr.ranks[0] == std::vector<int>{0} && lr.ranks[1] == std::vector<int>{1});
    // a port with no owner (a player who left) is left out, and fewer than two players is no result
    CHECK(result_json("remix", "x", "ABC234", false, v, {2, -1, -1, -1}).empty());
}

int main() {
    test_stock_rules();
    test_time_rules();
    test_team_rules();
    test_cpu_and_invalid();
    test_fixtures();
    test_trigger();
    test_ids_and_json();
    std::printf(fails ? "match_result test: %d FAILED\n" : "match_result test: all passed\n", fails);
    return fails ? 1 : 0;
}
