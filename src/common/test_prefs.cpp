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
    CHECK(m.favorites == V{"k1"} && m.theme == "new");
    CHECK(m.stamp("favorites") == 100 && m.stamp("theme") == 200);
    CHECK(same(merge(a, b), merge(b, a)) && same(merge(m, a), m) && same(merge(m, m), m));
    Prefs c, d;
    set_theme(&c, "aaa", 5); set_theme(&d, "bbb", 5);
    CHECK(same(merge(c, d), merge(d, c)));
    CHECK(same(merge(a, Prefs()), a));
    Prefs e = a; set_favorites(&e, {}, 300);
    CHECK(merge(a, e).favorites.empty());   // a newer deletion wins over an older value
    CHECK(merge_text(serialize(a), "not json").find("k1") != std::string::npos);

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
