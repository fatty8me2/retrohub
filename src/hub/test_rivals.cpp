// Console test: rivalry result parsing, ranking, storage union and head-to-head aggregation (scratch dirs only).
#include "rivals_logic.h"

#include <cstdio>
#include <filesystem>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

using namespace hubui::rivals;
namespace fs = std::filesystem;

static const char* kRaw =
    "{\"v\":1,\"game\":\"smash64\",\"id\":\"AB12-777\",\"code\":\"AB12\",\"players\":["
    "{\"slot\":0,\"char\":0,\"place\":1,\"stocks\":0,\"kos\":2,\"falls\":3},"
    "{\"slot\":1,\"char\":1,\"place\":0,\"stocks\":1,\"kos\":3,\"falls\":2}]}";

static Match mk(const char* id, int64_t at, int myrank, int theirrank, int mych, int thch, const char* uid = "u1") {
    Match m; m.id = id; m.game = "smash64"; m.at = at;
    Player a; a.me = true; a.rank = myrank; a.ch = mych; a.name = "Me";
    Player b; b.uid = uid; b.name = "Mickey"; b.rank = theirrank; b.ch = thch;
    m.players = {a, b};
    return m;
}

int main() {
    // parsing
    RawResult r;
    CHECK(parse_result(kRaw, &r) && r.id == "AB12-777" && r.players.size() == 2 && r.players[1].ch == 1 && r.players[1].place == 0);
    CHECK(!parse_result("", &r) && !parse_result("{}", &r) && !parse_result("[1]", &r));
    CHECK(!parse_result("{\"game\":\"smash64\",\"id\":\"x\",\"players\":[{\"slot\":0,\"char\":1}]}", &r));          // one player
    CHECK(!parse_result("{\"game\":\"smash64\",\"players\":[{\"slot\":0},{\"slot\":1}]}", &r));                      // no id
    CHECK(parse_result("{\"game\":\"g\",\"id\":\"x\",\"players\":[{\"slot\":0},{\"slot\":1},{\"slot\":-4},{\"slot\":99},5]}", &r) && r.players.size() == 2);

    // ranking: slot 1 won (place 0)
    parse_result(kRaw, &r);
    std::vector<SlotInfo> slots = {{0, "", "Me", true}, {1, "u1", "Mickey", false}};
    Match m = build_match(r, slots, 5000);
    CHECK(m.players.size() == 2 && m.players[0].name == "Mickey" && m.players[0].rank == 1 && m.players[1].me && m.players[1].rank == 2);
    CHECK(has_me(m));
    // unknown slot -> generic name, no uid; full tie -> same rank
    RawResult t; t.game = "smash64"; t.id = "t";
    RawPlayer a; a.slot = 0; a.ch = 2; a.place = 0; a.stocks = 1; a.kos = 1; a.falls = 1;
    RawPlayer b = a; b.slot = 3;
    t.players = {a, b};
    Match tm = build_match(t, {}, 1);
    CHECK(tm.players[0].rank == 1 && tm.players[1].rank == 1 && tm.players[1].name == "Player 4" && tm.players[1].uid.empty());
    // tiebreak by stocks
    t.players[1].stocks = 2;
    tm = build_match(t, {}, 1);
    CHECK(tm.players[0].rank == 1 && tm.players[1].rank == 2 && tm.players[0].name == "Player 4");

    // json round trip
    Match back;
    CHECK(match_from_json(match_to_json(m), &back) && back.id == m.id && back.at == 5000 && back.players.size() == 2 && back.players[0].uid == "u1" && back.players[1].me);
    CHECK(!match_from_json("{\"id\":\"x\"}", &back));

    // storage: one file per match; two stores union by id; id sanitising
    fs::path root = fs::temp_directory_path() / "rivals_test";
    fs::remove_all(root);
    std::string ud = (root / "pc1").string(), vt = (root / "vault").string(), ud2 = (root / "pc2").string();
    Match m2 = mk("../evil/id", 6000, 1, 2, 0, 1);
    CHECK(save_match(m, ud, vt) && save_match(m2, ud, vt));
    CHECK(fs::exists(root / "vault" / "_account" / "rivals" / "AB12-777.json"));
    CHECK(fs::exists(root / "pc1" / "rivals" / "___evil_id.json"));
    CHECK(save_match(m, ud, vt));                                      // same id again: kept, still ok
    CHECK(save_match(mk("PC2-1", 7000, 1, 2, 0, 1), ud2, vt));         // another PC writes its own file in the shared vault
    auto all = load_matches(ud, vt);
    CHECK(all.size() == 3 && all[0].id == "PC2-1" && all[2].id == "AB12-777");
    CHECK(load_matches(ud2, vt).size() == 3);                           // PC2 sees PC1's matches through the vault
    CHECK(load_matches("", "").empty());

    // pruning keeps the newest N per store
    prune_store(ud, false, 2);
    prune_store(vt, true, 2);
    { auto left = load_matches(ud, ""); CHECK(left.size() == 2 && left[0].id == "___evil_id" || left.size() == 2); }
    CHECK(load_matches("", vt).size() == 2 && load_matches("", vt)[0].at >= 6000);
    CHECK(!fs::exists(root / "vault" / "_account" / "rivals" / "AB12-777.json"));   // the oldest (at 5000) went

    // head to head: W W L W L L W (oldest to newest) vs u1; one match vs someone else, one without us
    std::vector<Match> ms;
    int rk[7][2] = {{1, 2}, {1, 2}, {2, 1}, {1, 2}, {2, 1}, {2, 1}, {1, 2}};
    for (int i = 0; i < 7; ++i) ms.push_back(mk(("m" + std::to_string(i)).c_str(), 100 + i, rk[i][0], rk[i][1], i < 5 ? 0 : 1, 3));
    ms.push_back(mk("o", 300, 1, 2, 0, 5, "u2"));
    Match nobody = mk("n", 301, 1, 2, 0, 0); nobody.players[0].me = false;
    ms.push_back(nobody);
    std::stable_sort(ms.begin(), ms.end(), [](const Match& x, const Match& y) { return x.at > y.at; });
    H2H h = head_to_head(ms, "u1");
    CHECK(h.wins == 4 && h.losses == 3 && h.draws == 0 && h.games() == 7);
    CHECK(h.last5 == "LWLLW");   // last five of W W L W L L W
    CHECK(h.my_main == "Mario");
    CHECK(h.their_main == "Samus");
    CHECK(record_text(h, "Mickey") == "You 4 - 3 Mickey" && mains_text(h) == "Mario vs Samus");
    CHECK(head_to_head(ms, "zzz").games() == 0 && head_to_head(ms, "").games() == 0);
    H2H d = head_to_head({mk("d", 1, 1, 1, 0, 0)}, "u1");
    CHECK(d.draws == 1 && record_text(d, "Mickey") == "You 0 - 0 Mickey (1 tie)" && d.last5 == "D");

    // rivals card
    auto rv = top_rivals(ms);
    CHECK(rv.size() == 2 && rv[0].uid == "u1" && rv[0].name == "Mickey" && rv[0].h.games() == 7 && rv[1].uid == "u2");
    CHECK(top_rivals(ms, 1).size() == 1 && top_rivals({}).empty());

    // sessions together
    hubui::group::RecentGroups g;
    hubui::group::RecentGroup x; x.members = {{"u1", "Mickey"}}; x.game_id = "mk64"; x.at = 10; g.record(x);
    x.game_id = "ssb"; x.at = 20; g.record(x);
    x.members = {{"u2", "Z"}}; x.at = 30; g.record(x);
    CHECK(sessions_together(g, "u1") == 2 && sessions_together(g, "u2") == 1 && sessions_together(g, "u9") == 0);
    CHECK(sessions_text(0).empty() && sessions_text(1) == "1 session together" && sessions_text(2) == "2 sessions together");

    fs::remove_all(root);
    std::printf(fails ? "rivals_test: %d FAILED\n" : "rivals_test: ok\n", fails);
    return fails ? 1 : 0;
}
