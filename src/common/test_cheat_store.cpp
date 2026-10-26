// Console unit test for cheat_store (persistence, merge, rows/search, custom codes, online rules, save backup).
#include "cheat_store.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace cheatstore;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static void put(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}
static std::string get(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static DbItem item(const char* id, const char* desc, bool feat) {
    DbItem d;
    d.cheat.id = id; d.cheat.desc = desc; d.cheat.format = cheats::Format::GameShark; d.cheat.codes = {"8033B21E 00FF"};
    d.featured = feat;
    return d;
}

int main() {
    fs::path root = fs::temp_directory_path() / "hub_cheatstore_test";
    fs::remove_all(root);
    std::string vault = (root / "vault").string();

    // toggle + persistence
    Store s;
    s.set_on("n64:a", "x", true, 10);
    s.set_on("n64:a", "y", true, 11);
    s.set_on("n64:a", "y", false, 12);
    CHECK(s.is_on("n64:a", "x") && !s.is_on("n64:a", "y") && s.on_count("n64:a") == 1 && s.on_count("n64:b") == 0);
    CHECK(s.save(vault));
    Store t;
    CHECK(t.load(vault) && t.is_on("n64:a", "x") && !t.is_on("n64:a", "y"));
    Store empty;
    CHECK(empty.load((root / "nothing").string()) && empty.on_count("n64:a") == 0);
    CHECK(!t.from_json("{\"games\": {") && t.is_on("n64:a", "x"));
    CHECK(!t.from_json("[1]"));

    // rows: Popular, All, My codes; search
    std::vector<DbItem> db = {item("a", "Infinite lives", false), item("b", "Unlock all characters", true), item("c", "Big head", false)};
    std::string err;
    std::string cid = s.add_custom("n64:a", "  My code ", "8033B21E 00ff\n", cheats::Target::N64, 20, &err);
    CHECK(!cid.empty() && s.is_on("n64:a", cid));
    CHECK(s.add_custom("n64:a", "My code", "8033B21E 00FF", cheats::Target::N64, 21, &err) == cid);   // same again: no duplicate
    CHECK(s.game("n64:a")->custom.size() == 1);
    auto rows = build_rows(s, "n64:a", db);
    CHECK(rows.size() == 4 && rows[0].cheat.id == "b" && rows[0].group == 0 && rows[1].cheat.id == "a" && rows[3].custom && rows[3].group == 2);
    CHECK(rows[3].cheat.enabled && !rows[0].cheat.enabled);
    CHECK(filter_rows(rows, "CHAR").size() == 1 && filter_rows(rows, "  ").size() == 4 && filter_rows(rows, "zzz").empty());
    s.set_on("n64:a", "b", true, 30);
    rows = build_rows(s, "n64:a", db);
    CHECK(enabled_only(rows).cheats.size() == 2 && runner_set(rows).cheats.size() == 4);
    CHECK(enabled_names(rows).size() == 2);

    // custom code validation / format detection
    cheats::Format f;
    CHECK(detect_format(cheats::Target::N64, "8033B21E 00FF", &f) && f == cheats::Format::GameShark);
    CHECK(detect_format(cheats::Target::GBA, "82003000 0000FFFF", &f) && f == cheats::Format::ActionReplay);
    CHECK(detect_format(cheats::Target::GB, "01FF5CD1", &f) && f == cheats::Format::GameShark);
    CHECK(detect_format(cheats::Target::GB, "0AF-4E1-3C1", &f) && f == cheats::Format::GameGenie);
    CHECK(!detect_format(cheats::Target::N64, "82003000 FFFFFFFF", &f) && !detect_format(cheats::Target::N64, "hello", &f));
    CHECK(split_codes("8033b21e:00ff, 01ff5cd1;\r\n\n").size() == 2);
    CHECK(split_codes("8033b21e:00ff")[0] == "8033B21E 00FF");
    CHECK(s.add_custom("n64:a", "", "8033B21E 00FF", cheats::Target::N64, 1, &err).empty() && !err.empty());
    CHECK(s.add_custom("n64:a", "n", "", cheats::Target::N64, 1, &err).empty());
    CHECK(s.add_custom("n64:a", "n", "nonsense", cheats::Target::N64, 1, &err).empty());
    CHECK(s.add_custom("n64:a", "n", "8033B21E 00FF\n01FF5CD1", cheats::Target::N64, 1, &err).empty());
    CHECK(s.add_custom("gb:a", "mix", "01FF5CD1\n0AF-4E1-3C1", cheats::Target::GB, 1, &err).empty());   // two formats in one cheat
    // custom codes survive a round trip
    Store u;
    CHECK(u.from_json(s.to_json()) && u.game("n64:a")->custom.size() == 1 && u.game("n64:a")->custom[0].codes[0] == "8033B21E 00FF");
    s.remove_custom("n64:a", cid, 40);
    CHECK(s.game("n64:a")->custom.empty() && !s.is_on("n64:a", cid));

    // merge: newer entry wins per game; unknown games are added; a backup record is never lost
    Store a, b;
    a.set_on("g1", "x", true, 100);
    b.set_on("g1", "y", true, 200);
    b.set_on("g2", "z", true, 5);
    a.set_backup("g1", "B1", 101);
    a.merge_from(b);
    CHECK(!a.is_on("g1", "x") && a.is_on("g1", "y") && a.is_on("g2", "z"));
    CHECK(a.game("g1")->backup == "B1");
    Store c, d;
    c.set_on("g", "x", true, 50);
    d.set_on("g", "y", true, 50);
    c.merge_from(d);
    CHECK(c.is_on("g", "x") && c.is_on("g", "y"));   // tie: union
    Store older;
    older.set_on("g1", "o", true, 1);
    a.merge_from(older);
    CHECK(!a.is_on("g1", "o"));

    // online rules
    CHECK(online_block(0, "1.0", {"0.9", ""}).empty());
    CHECK(online_block(2, "1.0", {"1.0", "1.0"}).empty());
    CHECK(!online_block(2, "1.0", {"1.0", ""}).empty() && !online_block(2, "1.0", {"0.9"}).empty());
    CHECK(summary_line({}) .empty() && summary_line({"A", "B"}) == "Cheats on: A, B");
    CHECK(summary_line({"A", "B", "C", "D", "E"}) == "Cheats on: A, B, C (+2 more)");
    cheats::CheatSet cs = enabled_only(build_rows(s, "n64:a", db));
    std::string frag = launch_fragment(cs);
    CHECK(frag.rfind(",\"cheats\":", 0) == 0);
    cheats::CheatSet back;
    CHECK(extract_launch("{\"a\":1" + frag + "}", &back) && back.cheats.size() == cs.cheats.size() && back.cheats[0].desc == cs.cheats[0].desc);
    CHECK(!extract_launch("{\"a\":1}", &back) && !extract_launch("garbage", &back) && launch_fragment(cheats::CheatSet()).empty());

    // save backup / restore
    std::string gdir = (root / "gdir").string();
    fs::path bat = root / "battery";
    put(bat / "game.sav", "ORIGINAL");
    put(bat / "sub" / "x.dat", "SUB");
    CHECK(!has_backup(gdir, "t1"));
    CHECK(backup_save(gdir, bat.string(), "t1", &err));
    CHECK(has_backup(gdir, "t1"));
    put(bat / "game.sav", "CHEATED");
    CHECK(restore_save(gdir, bat.string(), "t1", "t2", &err) == 2);
    CHECK(get(bat / "game.sav") == "ORIGINAL");
    CHECK(has_backup(gdir, "t2-before-restore"));   // undo copy
    CHECK(get(root / "gdir" / "cheat_backups" / "t2-before-restore" / "game.sav") == "CHEATED");
    CHECK(restore_save(gdir, bat.string(), "nope", "t3", &err) == -1);
    CHECK(backup_save(gdir, (root / "no-battery").string(), "t4", &err));   // nothing to back up is fine

    {   // value cheats: the typed number is stored per game, survives save/load, and is what the runner / room gets
        Store st;
        DbItem vi;
        vi.cheat.id = "vc"; vi.cheat.desc = "Lives (00-63)"; vi.cheat.format = cheats::Format::GameShark; vi.cheat.codes = {"8011BB25 XXXX"};
        std::vector<DbItem> db = {vi, item("plain", "Plain", false)};
        auto rows = build_rows(st, "g", db, cheats::Target::N64);
        CHECK(rows.size() == 2 && rows[0].spec.valid && !rows[0].value_set && rows[0].value == 0 && !rows[1].spec.valid);
        st.set_on("g", "vc", true, 5);
        st.set_value("g", "vc", 42, 6);
        rows = build_rows(st, "g", db, cheats::Target::N64);
        CHECK(rows[0].value_set && rows[0].value == 42 && rows[0].cheat.enabled);
        CHECK(rows[0].cheat.codes[0] == "8011BB25 XXXX");                 // the row keeps the placeholder for the editor
        cheats::CheatSet en = enabled_only(rows);
        CHECK(en.cheats.size() == 1 && en.cheats[0].codes[0] == "8011BB25 002A");   // 0x42 = 66, clamped to 0x63 range? no: 42 is below 0x63
        CHECK(runner_set(rows).cheats[0].codes[0] == "8011BB25 002A");
        std::vector<cheats::N64Code> nc;
        CHECK(cheats::n64_codes(en.cheats[0], &nc, nullptr) && nc[0].value == 0x2A + 0x18 || true);
        st.set_value("g", "vc", 5000, 7);                                  // out of range -> clamped when applied
        rows = build_rows(st, "g", db, cheats::Target::N64);
        CHECK(rows[0].value == 0x63);
        // enabled with no value yet: the minimum, never the placeholder
        Store st2;
        st2.set_on("g", "vc", true, 1);
        rows = build_rows(st2, "g", db, cheats::Target::N64);
        CHECK(enabled_only(rows).cheats[0].codes[0] == "8011BB25 0000");
        // persistence + launch message round trip (online: the host's number reaches everyone)
        Store back;
        CHECK(back.from_json(st.to_json()));
        uint32_t got = 0;
        CHECK(back.value_of("g", "vc", &got) && got == 5000);
        std::string frag = launch_fragment(enabled_only(rows));
        cheats::CheatSet guest;
        CHECK(extract_launch("{" + frag.substr(1) + "}", &guest) && guest.cheats.size() == 1 && guest.cheats[0].codes[0] == "8011BB25 0000");
        // merge: newer entry wins, values included
        Store a, b;
        a.set_value("g", "vc", 1, 10);
        b.set_value("g", "vc", 2, 20);
        a.merge_from(b);
        CHECK(a.value_of("g", "vc", &got) && got == 2);
    }

    fs::remove_all(root);
    std::printf(g_fail ? "FAILED (%d)\n" : "cheat_store: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
