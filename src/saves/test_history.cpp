/* Console test: local save history (prune / restore / undo / merge / export) and play stats aggregation. */
#include <windows.h>
#include "history.h"
#include "playstats.h"
#include "savestore.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using namespace hub::saves;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { g_fail++; std::printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static void put(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    o << s;
}
static std::string get(const fs::path& p) { std::ifstream i(p, std::ios::binary); std::stringstream s; s << i.rdbuf(); return s.str(); }
static std::string U(const fs::path& p) { auto u = p.u8string(); return std::string(u.begin(), u.end()); }

int main() {
    fs::path root = fs::temp_directory_path() / ("rh_hist_test_" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(root);
    fs::path bat = root / "battery", hist = root / "hist";
    std::string B = U(bat), H = U(hist);

    CHECK(!history_snapshot(H, B, 100, "PC-A"));              // empty battery: nothing
    put(bat / "a.sra", "v1");
    CHECK(history_snapshot(H, B, 100, "PC-A"));
    CHECK(!history_snapshot(H, B, 101, "PC-A"));              // unchanged: no duplicate
    put(bat / "big.card", std::string(600 * 1024, 'x'));      // too big: never kept
    CHECK(!history_snapshot(H, B, 102, "PC-A"));
    fs::remove_all(hist);
    for (int i = 1; i <= 8; ++i) { put(bat / "a.sra", "v" + std::to_string(i)); CHECK(history_snapshot(H, B, 1000 + i, "PC-A")); }
    auto l = history_list(H);
    CHECK(l.size() == 5);                                      // pruned to 5
    CHECK(l[0].ts == 1008 && l[4].ts == 1004);
    CHECK(l[0].files == 1 && l[0].size == 2 && l[0].device == "PC-A");
    CHECK(!fs::exists(hist / l[0].id / "big.card"));

    // restore v5 -> undo copy holds v8
    std::string err;
    CHECK(history_restore(H, B, l[3].id, 2000, &err));
    CHECK(get(bat / "a.sra") == "v5");
    HistEntry u;
    CHECK(history_undo_info(H, &u) && u.ts == 2000);
    CHECK(history_list(H).size() == 5);                        // undo copy is not listed
    CHECK(history_undo(H, B, &err));
    CHECK(get(bat / "a.sra") == "v8");
    CHECK(!history_undo_info(H, &u));
    CHECK(!history_undo(H, B, &err));
    CHECK(!history_restore(H, B, "../x", 1, &err));
    CHECK(get(bat / "big.card").size() == 600 * 1024);          // untouched

    // merge
    std::vector<CloudVer> cv = {{"battery/a.sra", 3, 1006, 2, "PC-B", true}, {"battery/a.sra", 2, 900, 2, "PC-B", false}};
    auto m = history_merge(history_list(H), cv);
    CHECK(m.size() == 7);
    CHECK(m[0].ts == 1008 && !m[0].cloud);
    CHECK(!m[2].cloud && m[3].cloud && m[3].current && m[3].ts == 1006);
    CHECK(m.back().cloud && m.back().ts == 900);

    // export
    fs::path g = root / "game";
    put(g / "battery" / "a.sra", "zz");
    put(g / "states" / "slot0.state", "st");
    put(g / "meta.json", "{}");
    CHECK(export_game_zip(U(g), U(root / "o.zip"), false, &err));
    CHECK(fs::file_size(root / "o.zip") > 20);
    CHECK(!export_game_zip(U(root / "none"), U(root / "n.zip"), false, &err));

    // stats
    CHECK(playlog_line(5, 60) == "5,60\n");
    int64_t a = 0, b = 0;
    CHECK(parse_playlog_line("1700000000,1800", &a, &b) && a == 1700000000 && b == 1800);
    CHECK(!parse_playlog_line("x", &a, &b) && !parse_playlog_line("5,-1", &a, &b));
    const int64_t now = 1700000000 + 10 * 86400 + 3600;
    std::vector<GameTotal> tot;
    for (int i = 0; i < 7; ++i) tot.push_back({i % 2 ? "n64" : "gba", "g" + std::to_string(i), (i + 1) * 3600});
    std::vector<Session> ss = {{"n64", "g1", now - 3600, 1800}, {"gba", "g0", now - 86400, 5400}, {"gba", "g0", now - 2 * 86400, 600},
                               {"gba", "g0", now - 9 * 86400, 7200}};
    Stats s = compute_stats(tot, ss, now, 0);
    CHECK(s.total_secs == 28 * 3600);
    CHECK(s.top.size() == 5 && s.top[0].key == "g6" && s.top[0].secs == 7 * 3600);
    CHECK(s.week_secs == 1800 + 5400 + 600);
    CHECK(s.longest.secs == 7200 && s.sessions == 4);
    CHECK(s.streak_days == 3);
    CHECK(s.systems.size() == 2 && s.systems[0].second >= s.systems[1].second);
    Stats e = compute_stats({}, {}, now, 0);
    CHECK(e.total_secs == 0 && e.streak_days == 0 && e.longest.secs == 0 && e.top.empty());
    // streak ending yesterday still counts; a gap breaks it
    Stats y = compute_stats({}, {{"n64", "g", now - 86400, 60}, {"n64", "g", now - 3 * 86400, 60}}, now, 0);
    CHECK(y.streak_days == 1);

    // play log reader: playlog_read_game / playlog_read_all / playlog_append against a throwaway vault
    {
        fs::path vault = root / "vault";
        set_vault_dir(U(vault));
        const std::string key = "playkey_1";                    // nothing sanitize_component would change
        fs::path pl = vault / "n64" / key / "playlog";
        put(pl / "PC-A.log", "1700000000,100\nnot a line\n1700000500,200\n");
        put(pl / "pc-b.log", "1700001000,300\n5,-1\n");
        put(pl / "notes.txt", "1700009000,999\n");                // not a .log: ignored
        put(pl / "PC-A.log.tmp", "1700009100,998\n");             // a sync tool's temp copy: ignored
        put(pl / "x.log" / "inner.log", "1700009200,997\n");      // a folder named x.log: not a regular file, not read
        std::vector<Session> g = playlog_read_game("n64", key);
        CHECK(g.size() == 3);
        int na = 0, nb = 0;
        int64_t sum_a = 0, sum_b = 0;
        bool meta_ok = true;
        for (const Session& s : g) {
            if (s.system != "n64" || s.key != key) meta_ok = false;
            if (s.device == "PC-A") { ++na; sum_a += s.secs; }
            else if (s.device == "pc-b") { ++nb; sum_b += s.secs; if (s.start != 1700001000) meta_ok = false; }
            else meta_ok = false;
        }
        CHECK(meta_ok);                                           // device = the file name without ".log" (case kept), system/key = the arguments
        CHECK(na == 2 && sum_a == 300);                           // the malformed line is skipped
        CHECK(nb == 1 && sum_b == 300);

        // a key with ':' lives in a folder with '_' on disk, found by the unsanitized key
        const std::string gp = game_dir_path("n64", "n64:abc");
        CHECK(gp.size() >= 12 && gp.substr(gp.size() - 12) == "n64\\n64_abc\\");
        put(vault / "n64" / "n64_abc" / "playlog" / "PC-A.log", "1700002000,60\n");
        std::vector<Session> c = playlog_read_game("n64", "n64:abc");
        CHECK(c.size() == 1 && c[0].key == "n64:abc" && c[0].system == "n64" && c[0].device == "PC-A" && c[0].start == 1700002000 && c[0].secs == 60);

        // read-only: no folder, no sessions, nothing created
        CHECK(playlog_read_game("n64", "no_such_game").empty());
        CHECK(!fs::exists(vault / "n64" / "no_such_game"));
        CHECK(playlog_read_game("gba", "no_such_game").empty() && !fs::exists(vault / "gba"));
        CHECK(playlog_read_game("", key).empty() && playlog_read_game("n64", "").empty() && playlog_read_game("", "").empty());

        // every game, every PC (device filled)
        std::vector<Session> all = playlog_read_all();
        CHECK(all.size() == 4);
        bool dev_ok = true;
        for (const Session& s : all) if (s.device.empty()) dev_ok = false;
        CHECK(dev_ok);

        // append, then read it back: this PC's file
        playlog_append("gba", "rt", 1700003000, 90);
        std::vector<Session> rt = playlog_read_game("gba", "rt");
        CHECK(rt.size() == 1 && rt[0].start == 1700003000 && rt[0].secs == 90 && rt[0].device == playlog_device() && rt[0].system == "gba" && rt[0].key == "rt");
        playlog_append("gba", "rt", 1700004000, 10);
        CHECK(playlog_read_game("gba", "rt").size() == 2);

        set_vault_dir("");
    }

    fs::remove_all(root);
    std::printf(g_fail ? "history_test: %d FAILED\n" : "history_test: all ok\n", g_fail);
    return g_fail ? 1 : 0;
}
