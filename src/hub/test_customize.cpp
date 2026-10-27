// Custom shelves + renames: pure logic (customize_logic.h), rename-aware search (logic.cpp filter_sort), and the prefs round trip.
#include "customize_logic.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

using namespace hubui::cust;

int main() {
    // ---- shelf ops ----
    Shelves v;
    std::string made;
    CHECK(add_shelf(&v, "  Co-op night  ", &made) && made == "Co-op night" && v.size() == 1);
    CHECK(add_shelf(&v, "co-op NIGHT", &made) && made == "co-op NIGHT 2");   // case-insensitive duplicate gets a suffix
    CHECK(add_shelf(&v, "", &made) && made == "New shelf");
    CHECK(v.size() == 3);
    CHECK(find_shelf(v, "NEW SHELF") == 2);
    CHECK(unique_name(v, std::string(100, 'x')).size() == 40);

    CHECK(rename_shelf(&v, 2, "Racing"));
    CHECK(!rename_shelf(&v, 2, "Racing"));            // unchanged
    CHECK(!rename_shelf(&v, 9, "x"));                 // out of range
    CHECK(rename_shelf(&v, 2, "  co-op night "));     // taken by shelf 0 -> suffix
    CHECK(v[2].name == "co-op night 2" || v[2].name == "co-op night 3");
    CHECK(rename_shelf(&v, 2, "Racing"));

    // membership
    CHECK(add_game(&v, 0, "n64:aaa"));
    CHECK(!add_game(&v, 0, "n64:aaa"));               // already there
    CHECK(add_game(&v, 0, "gba:bbb"));
    CHECK(add_game(&v, 2, "n64:aaa"));
    CHECK(!add_game(&v, 0, ""));
    CHECK(!add_game(&v, 7, "n64:aaa"));
    CHECK(has_game(v, 0, "gba:bbb") && !has_game(v, 1, "gba:bbb"));
    CHECK(membership_count(v, "n64:aaa") == 2);
    CHECK(toggle_game(&v, 2, "n64:aaa") && membership_count(v, "n64:aaa") == 1);
    CHECK(toggle_game(&v, 2, "n64:aaa") && membership_count(v, "n64:aaa") == 2);
    CHECK(remove_game(&v, 2, "n64:aaa") && !remove_game(&v, 2, "n64:aaa"));

    // reorder
    CHECK(!move_shelf(&v, 0, -1));                    // already first
    CHECK(!move_shelf(&v, 2, +1));                    // already last
    CHECK(move_shelf(&v, 0, +1));
    CHECK(v[0].name == "co-op NIGHT 2" && v[1].name == "Co-op night");
    CHECK(v[1].keys.size() == 2);                     // keys travel with the shelf
    CHECK(move_shelf(&v, 1, -1) && v[0].name == "Co-op night");

    // delete
    CHECK(delete_shelf(&v, 1) && v.size() == 2 && v[1].name == "Racing");
    CHECK(!delete_shelf(&v, 5));

    // cap
    {
        Shelves big;
        for (int i = 0; i < 24; ++i) CHECK(add_shelf(&big, "s" + std::to_string(i)));
        CHECK(!add_shelf(&big, "one too many"));
    }

    // ---- Home composition: after the built-ins, order kept, unknown keys + empty shelves skipped ----
    {
        std::map<std::string, int> idx = {{"n64:aaa", 4}, {"gba:bbb", 1}};
        Shelves s;
        add_shelf(&s, "A"); add_shelf(&s, "B"); add_shelf(&s, "C");
        add_game(&s, 0, "gba:bbb"); add_game(&s, 0, "gone:zzz"); add_game(&s, 0, "n64:aaa");
        add_game(&s, 2, "gone:zzz");                  // only an unowned game -> shelf C is not shown
        auto out = compose_user_shelves(s, idx);
        CHECK(out.size() == 1);
        CHECK(out[0].id == "user:0" && out[0].title == "A");
        CHECK(out[0].items.size() == 2 && out[0].items[0] == 1 && out[0].items[1] == 4);
        CHECK(compose_user_shelves(Shelves(), idx).empty());
        // the built-in composition is untouched by user shelves
        std::vector<hubui::ShelfGame> g(1);
        g[0].key = "n64:aaa"; g[0].title = "Zelda"; g[0].system = "n64";
        auto b = hubui::compose_shelves(g, 12, hubui::SortMode::Title);
        CHECK(b.size() == 1 && b[0].id == "console:n64");
    }

    // ---- renames ----
    {
        std::map<std::string, std::string> m;
        CHECK(display_title(m, "k", "Orig") == "Orig");
        m = with_rename(m, "k", "Orig", "  My Name ");
        CHECK(m.size() == 1 && display_title(m, "k", "Orig") == "My Name");
        CHECK(with_rename(m, "k", "Orig", "Orig").empty());       // same as the original = reset
        CHECK(with_rename(m, "k", "Orig", "   ").empty());        // blank = reset
        CHECK(display_title({{"k", "  "}}, "k", "Orig") == "Orig");
        CHECK(with_rename({}, "k", "Orig", std::string(200, 'z'))["k"].size() == 80);
    }

    // ---- rename-aware search: matches the new name AND the original ----
    {
        std::vector<hubui::ListItem> items;
        hubui::ListItem a{"n64:1", "Best Game Ever", "n64", false, 0, 0, 0};
        a.alt_title = "Super Mario 64";                           // renamed from this
        hubui::ListItem b{"n64:2", "Mario Kart 64", "n64", false, 0, 0, 0};
        hubui::ListItem c{"n64:3", "Zelda", "n64", false, 0, 0, 0};
        items = {a, b, c};
        auto r = hubui::filter_sort(items, "", "best game");
        CHECK(r.size() == 1 && r[0] == 0);                        // new name
        r = hubui::filter_sort(items, "", "super mario");
        CHECK(r.size() == 1 && r[0] == 0);                        // original name
        r = hubui::filter_sort(items, "", "MARIO");
        CHECK(r.size() == 2);                                     // original of #0 + title of #1
        r = hubui::filter_sort(items, "", "nothing");
        CHECK(r.empty());
        r = hubui::filter_sort(items, "", "");
        CHECK(r.size() == 3);
    }

    // ---- prefs round trip through the real setters/serializer (merge logic is prefs_test's job) ----
    {
        hub::prefs::Prefs p;
        Shelves s;
        add_shelf(&s, "Party"); add_game(&s, 0, "n64:aaa");
        CHECK(hub::prefs::set_user_shelves(&p, s, 1000));
        CHECK(!hub::prefs::set_user_shelves(&p, s, 2000));        // unchanged -> not stamped again
        CHECK(hub::prefs::set_renames(&p, with_rename({}, "n64:aaa", "Orig", "New"), 1500));
        hub::prefs::Prefs q;
        CHECK(hub::prefs::parse(hub::prefs::serialize(p), &q));
        CHECK(q.user_shelves.size() == 1 && q.user_shelves[0].name == "Party" && q.user_shelves[0].keys.size() == 1);
        CHECK(display_title(q.renames, "n64:aaa", "Orig") == "New");
    }

    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("customize_test: all passed\n");
    return 0;
}
