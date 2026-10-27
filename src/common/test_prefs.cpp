#include <cstdio>
#include <filesystem>

#include "library.h"
#include "prefs.h"

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
    std::printf(fails ? "prefs_test FAILED\n" : "prefs_test OK\n");
    return fails ? 1 : 0;
}
