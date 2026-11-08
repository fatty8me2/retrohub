#include <cstdio>
#include <filesystem>

#include "json.h"
#include "library.h"
#include "prefs.h"
#include "hidden_games.h"

using namespace hub::prefs;
namespace fs = std::filesystem;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static bool same(const Prefs& a, const Prefs& b) { return serialize(a) == serialize(b); }
using V = std::vector<std::string>;

int main() {
    Prefs p;
    set_favorite(&p, "n64:b", true, 10);
    set_favorite(&p, "n64:a", true, 11);
    set_favorite(&p, "n64:a", true, 12);   // unchanged: stamp stays 11
    CHECK(p.favorites.size() == 2 && p.favorites[0] == "n64:a" && p.stamp("favorites") == 11);
    set_user_shelves(&p, {{"Co-op", {"n64:a", "n64:a", "gba:x"}}}, 20);
    set_hidden(&p, {"gb:h", "gb:h"}, 21);
    set_renames(&p, {{"n64:a", "Smash \"64\""}}, 22);
    set_covers(&p, {{"n64:a", "C:\\covers\\a.png"}}, 23);
    set_preferred_copy(&p, {{"n64:a", "D:/roms/a.z64"}}, 28);
    set_theme(&p, "midnight", 24);
    set_profile_avatar(&p, "fox", 25);
    set_profile_fav_game(&p, "n64:a", 26);
    set_profile_status(&p, "afk", 27);
    CHECK(p.user_shelves[0].keys.size() == 2 && p.hidden.size() == 1);
    Prefs q;
    CHECK(parse(serialize(p), &q) && same(p, q) && q.renames["n64:a"] == "Smash \"64\"" && q.profile.status == "afk" && q.stamp("theme") == 24);
    CHECK(q.preferred_copy["n64:a"] == "D:/roms/a.z64" && q.stamp("preferred_copy") == 28);
    CHECK(!parse("garbage", &q) && q.favorites.empty());
    CHECK(parse("{\"theme\":\"x\",\"unknown\":[1,2]}", &q) && q.theme == "x");

    // field-wise merge: A changed favorites later, B changed theme later; both survive
    Prefs a, b;
    set_favorites(&a, {"k1"}, 100); set_theme(&a, "old", 50);
    set_favorites(&b, {"k2"}, 90);  set_theme(&b, "new", 200);
    Prefs m = merge(a, b);
    CHECK(m.favorites == (V{"k1", "k2"}) && m.theme == "new");   // favorites union per key
    CHECK(m.stamp("favorites") == 100 && m.stamp("theme") == 200);
    CHECK(same(merge(a, b), merge(b, a)) && same(merge(m, a), m) && same(merge(m, m), m));
    Prefs c, d;
    set_theme(&c, "aaa", 5); set_theme(&d, "bbb", 5);
    CHECK(same(merge(c, d), merge(d, c)));
    CHECK(same(merge(a, Prefs()), a));
    Prefs e = a; set_favorites(&e, {}, 300);
    CHECK(merge(a, e).favorites.empty());   // a newer tombstone wins over an older value
    Prefs e2 = e; set_favorite(&e2, "k1", true, 400);
    CHECK(merge(e, e2).favorites == V{"k1"} && same(merge(e, e2), merge(e2, e)));   // re-favorited after the tombstone

    // two PCs, offline, favorite/hide different games, then sync both ways
    {
        Prefs base; set_favorites(&base, {"n64:shared", "gb:old"}, 1000); set_hidden(&base, {"x:h"}, 1000);
        Prefs pc1 = base, pc2 = base;
        set_favorite(&pc1, "n64:one", true, 2000);
        set_favorite(&pc1, "gb:old", false, 2100);
        set_favorite(&pc2, "snes:two", true, 2050);
        set_hidden(&pc2, {"x:h", "x:h2"}, 2060);
        set_hidden(&pc1, {}, 2200);   // pc1 unhides x:h
        Prefs m1 = merge(pc1, pc2), m2 = merge(pc2, pc1);
        CHECK(same(m1, m2));
        CHECK(m1.favorites == (V{"n64:one", "n64:shared", "snes:two"}));
        CHECK(m1.hidden == V{"x:h2"});
        std::string txt = merge_text(serialize(pc1), serialize(pc2));
        Prefs rt; CHECK(parse(txt, &rt) && same(rt, m1));
        CHECK(same(merge(m1, pc1), m1) && same(merge(m1, pc2), m1));
        Prefs legacy; CHECK(parse("{\"favorites\":[\"a\"],\"ts\":{\"favorites\":50}}", &legacy));
        Prefs lm = merge(legacy, pc2); CHECK(lm.favorites.size() == 4);
    }
    CHECK(merge_text(serialize(a), "not json").find("k1") != std::string::npos);

    // legacy (no per-key stamps) removal must not resurrect; a newer key still survives
    {
        Prefs newer, older;
        CHECK(parse("{\"favorites\":[],\"ts\":{\"favorites\":500}}", &newer) && newer.legacy_favorites);
        set_favorites(&older, {"k"}, 100);
        CHECK(merge(newer, older).favorites.empty() && merge(older, newer).favorites.empty());
        Prefs newer_key; set_favorites(&newer_key, {"k"}, 900);
        CHECK(merge(newer, newer_key).favorites == V{"k"});
        // two new-format PCs: the one that never saw "k" must not tombstone it
        Prefs x, y; set_favorites(&x, {"a"}, 500); set_favorites(&y, {"k"}, 100);
        CHECK(merge(x, y).favorites == (V{"a", "k"}));
    }
    // tombstones older than 90 days are dropped on save, fresh ones stay
    {
        Prefs g; long long now = 200ll * 24 * 3600 * 1000;
        set_favorites(&g, {"old", "new"}, now - 120ll * 24 * 3600 * 1000);
        set_favorite(&g, "old", false, now - 100ll * 24 * 3600 * 1000);
        set_favorite(&g, "new", false, now - 1000);
        gc_tombstones(&g, now);
        CHECK(!g.favorites_ts.count("old") && g.favorites_ts.count("new") && g.favorites_ts["new"] < 0);
    }

    fs::path root = fs::temp_directory_path() / "rh_prefs_test";
    std::error_code ec; fs::remove_all(root, ec);
    std::string r = root.string();
    Prefs l;
    CHECK(!load(r, &l));
    CHECK(save(r, a) && load(r, &l) && same(l, a));
    Prefs mm = merge_into_file(r, b);
    Prefs back; load(r, &back);
    CHECK(same(mm, back) && back.theme == "new" && back.favorites[0] == "k1");

    // library.json favorites mirrored into prefs (scratch library dir)
    fs::path ld = fs::temp_directory_path() / "rh_prefs_test_lib";
    fs::remove_all(ld, ec); fs::create_directories(ld, ec);
    hub::library_set_data_dir(ld.string());
    fs::remove_all(root, ec);
    set_vault_root(r);
    hub::set_favorite("n64:zz", true);
    Prefs lp; CHECK(load(r, &lp) && lp.favorites == V{"n64:zz"} && lp.stamp("favorites") > 0);
    hub::set_favorite("gba:yy", true);
    hub::set_favorite("n64:zz", false);
    load(r, &lp);
    CHECK(lp.favorites == V{"gba:yy"});
    CHECK(hub::game_state("gba:yy").favorite && !hub::game_state("n64:zz").favorite);
    Prefs pulled = lp; set_favorites(&pulled, {"snes:q"}, lp.stamp("favorites") + 1000);
    CHECK(apply_favorites_to_library(pulled) == 2);
    CHECK(hub::game_state("snes:q").favorite && !hub::game_state("gba:yy").favorite);
    Prefs after; load(r, &after);
    CHECK(after.stamp("favorites") == lp.stamp("favorites"));   // no echo back into prefs
    CHECK(apply_favorites_to_library(Prefs()) == 0 && hub::game_state("snes:q").favorite);
    fs::remove_all(root, ec);
    seed_from_library();
    Prefs seeded; CHECK(load(r, &seeded) && seeded.favorites == V{"snes:q"});

    set_vault_root("");
    hub::library_set_data_dir("");
    fs::remove_all(root, ec); fs::remove_all(ld, ec);

    // ---- Plan 4 Wave 3: per-key map merge, unknown-field preservation, progress, smart shelves ----
    {
        // two PCs rename different games while offline: both renames survive, in either merge order
        Prefs base; set_renames(&base, {{"n64:a", "A"}}, 100);
        Prefs pc1 = base, pc2 = base;
        set_renames(&pc1, {{"n64:a", "A"}, {"n64:b", "B1"}}, 200);
        set_renames(&pc2, {{"n64:a", "A"}, {"n64:c", "C2"}}, 300);
        Prefs m1 = merge(pc1, pc2), m2 = merge(pc2, pc1);
        CHECK(m1.renames.size() == 3 && m1.renames["n64:b"] == "B1" && m1.renames["n64:c"] == "C2" && m1.renames["n64:a"] == "A");
        CHECK(same(m1, m2) && same(merge(m1, pc1), m1) && same(merge(m1, m1), m1));
        // one PC deletes a rename (newer), the other edits another game
        Prefs pc3 = base; set_renames(&pc3, {}, 400);
        Prefs m3 = merge(pc3, pc2);
        CHECK(m3.renames.count("n64:a") == 0 && m3.renames["n64:c"] == "C2" && same(m3, merge(pc2, pc3)));
        // the same game renamed on both: the newer stamp wins
        Prefs x = base, y = base;
        set_renames(&x, {{"n64:a", "X"}}, 500); set_renames(&y, {{"n64:a", "Y"}}, 600);
        CHECK(merge(x, y).renames["n64:a"] == "Y" && same(merge(x, y), merge(y, x)));
        // survives a JSON round trip (stamps and tombstones are stored)
        Prefs rt; CHECK(parse(serialize(m3), &rt) && same(rt, m3));
        Prefs again = merge(rt, pc2);
        CHECK(again.renames.count("n64:a") == 0);   // the tombstone came back through the file
    }
    {
        // completion + notes: per game, field-wise
        Prefs pc1, pc2;
        set_progress(&pc1, {{"n64:a", {"beaten", "took 12 hours"}}}, 100);
        set_progress(&pc2, {{"gba:b", {"playing", ""}}}, 110);
        Prefs m = merge(pc1, pc2);
        CHECK(m.progress.size() == 2 && m.progress["n64:a"].status == "beaten" && m.progress["gba:b"].note.empty());
        CHECK(same(m, merge(pc2, pc1)));
        Prefs pc3 = m; set_progress(&pc3, {{"n64:a", {"complete", "took 12 hours"}}, {"gba:b", {"playing", ""}}}, 200);
        CHECK(merge(m, pc3).progress["n64:a"].status == "complete");
        std::map<std::string, Progress> pm;
        pm["k1"] = {"beaten", std::string(600, 'x')};
        pm["k2"] = {"", ""};   // empty entries are dropped
        Prefs big; set_progress(&big, pm, 5);
        CHECK(big.progress.size() == 1 && big.progress["k1"].note.size() == 500);
        Prefs rt; CHECK(parse(serialize(big), &rt) && same(rt, big) && rt.progress["k1"].status == "beaten");
        CHECK(!set_progress(&big, big.progress, 6) && big.stamp("progress") == 5);   // unchanged: no stamp
    }
    {
        // status on PC A, note on PC B, same game: both survive, in either merge order and either time order
        Prefs base;
        Prefs pa = base, pb = base;
        set_progress(&pa, {{"n64:g", {"beaten", ""}}}, 100);
        set_progress(&pb, {{"n64:g", {"", "needs the red key"}}}, 200);
        Prefs m1 = merge(pa, pb), m2 = merge(pb, pa);
        CHECK(m1.progress.size() == 1 && m1.progress["n64:g"].status == "beaten" && m1.progress["n64:g"].note == "needs the red key");
        CHECK(same(m1, m2) && same(merge(m1, pa), m1) && same(merge(m1, m1), m1));
        Prefs pc = base, pd = base;   // the note is the OLDER edit this time
        set_progress(&pc, {{"n64:g", {"", "old note"}}}, 100);
        set_progress(&pd, {{"n64:g", {"complete", ""}}}, 200);
        Prefs m3 = merge(pc, pd);
        CHECK(m3.progress["n64:g"].status == "complete" && m3.progress["n64:g"].note == "old note" && same(m3, merge(pd, pc)));
        // survives the JSON file and a later edit of one half on one PC
        Prefs rt; CHECK(parse(serialize(m1), &rt) && same(rt, m1));
        Prefs pe = rt; set_progress(&pe, {{"n64:g", {"beaten", "new note"}}}, 300);
        Prefs m4 = merge(pe, pa);
        CHECK(m4.progress["n64:g"].note == "new note" && m4.progress["n64:g"].status == "beaten" && same(m4, merge(pa, pe)));
        // the same half edited on both: the newer stamp wins; changing only the note never touches the status stamp
        Prefs pf = rt, pg = rt;
        set_progress(&pf, {{"n64:g", {"dropped", "needs the red key"}}}, 400);
        set_progress(&pg, {{"n64:g", {"complete", "changed my mind"}}}, 500);
        Prefs m5 = merge(pf, pg);
        CHECK(m5.progress["n64:g"].status == "complete" && m5.progress["n64:g"].note == "changed my mind" && same(m5, merge(pg, pf)));
        Prefs ph = rt; set_progress(&ph, {{"n64:g", {"beaten", "only the note"}}}, 600);
        Prefs pi = rt; set_progress(&pi, {{"n64:g", {"playing", "needs the red key"}}}, 550);
        Prefs m6 = merge(ph, pi);
        CHECK(m6.progress["n64:g"].status == "playing" && m6.progress["n64:g"].note == "only the note");
        // a file an older client wrote (one stamp per game, no progress_note) merges with a new one, both ways
        const std::string old_text = "{\"version\":1,\"progress\":{\"n64:g\":{\"s\":\"playing\",\"n\":\"from old\"}},\"map_ts\":{\"progress\":{\"n64:g\":50}},\"ts\":{\"progress\":50}}";
        Prefs po; CHECK(parse(old_text, &po) && po.progress["n64:g"].note == "from old" && !po.map_ts.count("progress_note"));
        CHECK(serialize(po).find("progress_note") == std::string::npos);   // an untouched legacy file keeps its shape
        Prefs pn = base; set_progress(&pn, {{"n64:g", {"", "newer note"}}}, 60);
        Prefs m7 = merge(po, pn);
        CHECK(m7.progress["n64:g"].status == "playing" && m7.progress["n64:g"].note == "newer note" && same(m7, merge(pn, po)));
        Prefs pp = po; set_progress(&pp, {{"n64:g", {"playing", "edited here"}}}, 70);   // editing a legacy file converts it
        CHECK(pp.map_ts.count("progress_note") && merge(pp, po).progress["n64:g"].note == "edited here");
    }
    {
        // tombstones: clearing a game removes both halves; a newer edit on the other PC revives only its own half
        Prefs base; set_progress(&base, {{"n64:t", {"beaten", "note"}}}, 100);
        Prefs pa = base, pb = base;
        set_progress(&pa, {}, 200);   // cleared on A
        CHECK(pa.progress.empty());
        Prefs m1 = merge(pa, base), m2 = merge(base, pa);
        CHECK(m1.progress.empty() && same(m1, m2));
        CHECK(m1.map_ts["progress"]["n64:t"] < 0 && m1.map_ts["progress_note"]["n64:t"] < 0);
        Prefs rt; CHECK(parse(serialize(m1), &rt) && same(rt, m1) && rt.progress.empty());
        CHECK(merge(rt, base).progress.empty());   // the tombstone came back through the file and still beats the old entry
        set_progress(&pb, {{"n64:t", {"beaten", "newer note"}}}, 300);   // B types a newer note after A's clear
        Prefs m3 = merge(pa, pb);
        CHECK(m3.progress.size() == 1 && m3.progress["n64:t"].note == "newer note" && m3.progress["n64:t"].status.empty() && same(m3, merge(pb, pa)));
        Prefs pc = base; set_progress(&pc, {{"n64:t", {"complete", "note"}}}, 150);   // B's edit is OLDER than A's clear
        CHECK(merge(pa, pc).progress.empty() && same(merge(pa, pc), merge(pc, pa)));
        Prefs again = pa; set_progress(&again, {{"n64:t", {"", "fresh"}}}, 400);   // a game cleared and then given a note-only entry
        CHECK(again.progress["n64:t"].note == "fresh" && merge(again, base).progress["n64:t"].status.empty());
    }
    {
        // an OLD-format file (no map_ts, no progress, no rule) and a NEW-format file merge both ways
        const std::string old_text =
            "{\"version\":1,\"favorites\":[\"n64:f\"],\"user_shelves\":[{\"name\":\"Co-op\",\"keys\":[\"n64:a\"]}],\"hidden\":[],"
            "\"renames\":{\"n64:a\":\"Old A\",\"n64:o\":\"Old O\"},\"covers\":{},\"theme\":\"dark\","
            "\"profile\":{\"avatar\":\"\",\"fav_game\":\"\",\"status\":\"\"},"
            "\"ts\":{\"favorites\":50,\"user_shelves\":50,\"renames\":60,\"theme\":70}}";
        Prefs old_p; CHECK(parse(old_text, &old_p) && old_p.legacy_maps.count("renames") && old_p.user_shelves.size() == 1 && !old_p.user_shelves[0].smart);
        Prefs new_p = old_p;
        set_renames(&new_p, {{"n64:a", "Old A"}, {"n64:o", "Old O"}, {"n64:n", "New N"}}, 1000);
        set_progress(&new_p, {{"n64:a", {"beaten", "great"}}}, 1001);
        Shelf sm; sm.name = "Backlog"; sm.smart = true; sm.rule.status = "backlog"; sm.rule.min_players = 2; sm.rule.systems = {"n64", "gc"};
        std::vector<Shelf> shs = new_p.user_shelves; shs.push_back(sm);
        set_user_shelves(&new_p, shs, 1002);
        Prefs n1 = merge(old_p, new_p), n2 = merge(new_p, old_p);
        CHECK(same(n1, n2));
        CHECK(n1.renames.size() == 3 && n1.renames["n64:n"] == "New N" && n1.progress["n64:a"].note == "great");
        CHECK(n1.user_shelves.size() == 2 && n1.user_shelves[1].smart && n1.user_shelves[1].rule.systems == (V{"n64", "gc"}));
        CHECK(n1.theme == "dark" && n1.favorites == V{"n64:f"});
        // the old client re-writes its own file later with fewer renames: a whole-map write by a legacy client is last-writer-wins
        Prefs old2 = old_p; old2.renames.erase("n64:o"); old2.map_ts.clear(); old2.ts["renames"] = 2000;
        Prefs n3 = merge(new_p, old2);
        CHECK(n3.renames.count("n64:n") == 1 && same(n3, merge(old2, new_p)));   // a key the old client never knew is never wiped
        // an old client re-saving the shelf list drops the rule (it keeps name + empty keys): the merge puts the rule back
        Prefs old3 = new_p; old3.map_ts.clear(); old3.legacy_maps.insert("user_shelves"); old3.ts["user_shelves"] = 3000;
        old3.user_shelves[1].smart = false; old3.user_shelves[1].rule = ShelfRule();
        Prefs n5 = merge(new_p, old3);
        CHECK(n5.user_shelves.size() == 2 && n5.user_shelves[1].smart && n5.user_shelves[1].rule.min_players == 2 && same(n5, merge(old3, new_p)));
        // an old client's untouched file (older stamps) never deletes the new client's keys
        Prefs n4 = merge(new_p, old_p);
        CHECK(n4.renames.count("n64:n") == 1);
        // a new-format file parses back identical; the smart rule round trips
        Prefs rt; CHECK(parse(serialize(n1), &rt) && same(rt, n1) && rt.user_shelves[1].rule.min_players == 2 && rt.user_shelves[1].rule.status == "backlog");
        // an old client sees a shelf with a rule as a plain empty shelf: it reads name + keys only (keys stays an empty array)
        json::Value jv; json::parse(serialize(n1), &jv);
        const json::Value* us = jv.get("user_shelves");
        CHECK(us && us->a.size() == 2 && us->a[1].str("name") == "Backlog" && us->a[1].get("keys") && us->a[1].get("keys")->a.empty());
    }
    {
        // unknown top-level fields written by a newer client survive parse -> serialize -> merge with an older one
        const std::string newer = "{\"version\":1,\"theme\":\"x\",\"future_thing\":{\"a\":[1,2,3],\"b\":\"hi\"},\"future_n\":7,\"ts\":{\"theme\":10,\"future_thing\":20}}";
        Prefs fp; CHECK(parse(newer, &fp) && fp.extra.size() == 2);
        std::string out = serialize(fp);
        CHECK(out.find("\"future_thing\":{\"a\":[1,2,3],\"b\":\"hi\"}") != std::string::npos && out.find("\"future_n\":7") != std::string::npos);
        Prefs plain; set_theme(&plain, "y", 30);
        Prefs m1 = merge(plain, fp), m2 = merge(fp, plain);
        CHECK(same(m1, m2) && m1.extra.size() == 2 && m1.theme == "y" && m1.stamp("future_thing") == 20);
        CHECK(serialize(m1).find("future_thing") != std::string::npos);
        // text-level merge (what the cloud conflict resolver calls) keeps them too
        CHECK(merge_text(serialize(plain), newer).find("future_n") != std::string::npos);
        // both sides carry the field: the newer "ts" stamp wins, order independent
        Prefs g1, g2;
        parse("{\"zz\":1,\"ts\":{\"zz\":5}}", &g1); parse("{\"zz\":2,\"ts\":{\"zz\":9}}", &g2);
        CHECK(merge(g1, g2).extra[0].second.i == 2 && same(merge(g1, g2), merge(g2, g1)));
    }
    {
        // smart shelves merge by name; the order follows the newer shelf-list stamp
        Prefs pc1, pc2;
        Shelf s1; s1.name = "Backlog"; s1.smart = true; s1.rule.status = "backlog";
        Shelf s2; s2.name = "Old favourites"; s2.smart = true; s2.rule.favorites_only = true; s2.rule.not_played_days = 90;
        Shelf s3; s3.name = "Co-op"; s3.keys = {"n64:a"};
        set_user_shelves(&pc1, {s3, s1}, 100);
        set_user_shelves(&pc2, {s3, s2}, 200);
        Prefs m = merge(pc1, pc2);
        CHECK(m.user_shelves.size() == 3 && same(m, merge(pc2, pc1)));
        CHECK(m.user_shelves[0].name == "Co-op" && m.user_shelves[1].name == "Old favourites" && m.user_shelves[2].name == "Backlog");
        // edit the rule of one shelf on one PC, add a game to Co-op on the other: both edits survive
        Prefs e1 = m, e2 = m;
        Shelf s1b = s1; s1b.rule.min_players = 3;
        set_user_shelves(&e1, {m.user_shelves[0], m.user_shelves[1], s1b}, 300);
        Shelf s3b = s3; s3b.keys = {"n64:a", "gb:z"};
        set_user_shelves(&e2, {s3b, m.user_shelves[1], m.user_shelves[2]}, 310);
        Prefs mm = merge(e1, e2);
        const Shelf* cb = nullptr; const Shelf* bk = nullptr;
        for (const auto& s : mm.user_shelves) { if (s.name == "Co-op") cb = &s; if (s.name == "Backlog") bk = &s; }
        CHECK(cb && bk && cb->keys.size() == 2 && bk->rule.min_players == 3 && same(mm, merge(e2, e1)));
        // delete a shelf on one PC (newer): it stays deleted
        Prefs d1 = m; set_user_shelves(&d1, {m.user_shelves[0], m.user_shelves[1]}, 400);
        Prefs md = merge(d1, m);
        CHECK(md.user_shelves.size() == 2 && same(md, merge(m, d1)));
    }

    {   // library-12: Hide / Unhide round-trip through the prefs file, and the hidden-versus-shown split
        Prefs h;
        CHECK(hub::hidden::hide(&h, "n64:a", 100) && h.hidden.size() == 1);
        CHECK(!hub::hidden::hide(&h, "n64:a", 101));                     // hiding twice changes nothing
        Prefs back;
        CHECK(parse(serialize(h), &back) && back.hidden.size() == 1 && back.hidden[0] == "n64:a");   // survives save + load
        CHECK(hub::hidden::hide(&back, "gb:b", 110) && back.hidden.size() == 2);
        Prefs other = h;                                                  // another PC still has only n64:a
        Prefs m = merge(back, other);
        CHECK(m.hidden.size() == 2);                                      // the newer hide reaches the other PC
        CHECK(hub::hidden::unhide(&m, "n64:a", 200) && m.hidden.size() == 1 && m.hidden[0] == "gb:b");
        CHECK(!hub::hidden::unhide(&m, "nope:x", 201));
        Prefs m2 = merge(m, back);                                        // the unhide is newer than the old hide: it sticks
        CHECK(m2.hidden.size() == 1 && m2.hidden[0] == "gb:b");
        struct G { std::string key; };
        std::vector<G> lib{{"n64:a"}, {"gb:b"}, {"smash"}, {"gc:c"}};
        std::vector<G> gone = hub::hidden::take_hidden(&lib, {"gb:b", "smash", "gc:c"}, "smash");
        CHECK(lib.size() == 2 && lib[0].key == "n64:a" && lib[1].key == "smash");   // the bundled game is never hidden
        CHECK(gone.size() == 2 && gone[0].key == "gb:b" && gone[1].key == "gc:c");
        std::vector<G> none = hub::hidden::take_hidden(&lib, {});
        CHECK(none.empty() && lib.size() == 2);
        CHECK(hub::hidden::chip_label(false, 0).empty());                 // nothing hidden: no chip
        CHECK(hub::hidden::chip_label(false, 3) == "Show hidden  3" && hub::hidden::chip_label(true, 0) == "Show hidden  0");
    }

    std::printf(fails ? "prefs_test FAILED\n" : "prefs_test OK\n");
    return fails ? 1 : 0;
}
