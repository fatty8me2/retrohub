#include "picker_logic.h"

#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

using namespace hubpick;

static Item mk(const std::string& id, const std::string& title, const char* sys, const char* sname, bool usable = true) {
    Item i; i.id = id; i.title = title; i.sys = sys; i.sys_name = sname;
    i.selectable = usable;
    return i;
}
static const Section* find(const std::vector<Section>& v, const std::string& key) {
    for (auto& s : v) if (s.key == key) return &s;
    return nullptr;
}

int main() {
    CHECK(players_text(1, 4) == "1-4 players" && players_text(1, 1) == "1 player" && players_text(2, 2) == "2 players" && players_text(0, 0) == "1 player");

    std::vector<Item> v;
    v.push_back(mk("a", "Mario Kart 64", "n64", "Nintendo 64"));
    v.push_back(mk("b", "Super Smash Bros.", "n64", "Nintendo 64"));
    v.back().pinned = true; v.back().favorite = true; v.back().last_played = 50;
    v.push_back(mk("c", "Metroid Prime", "gc", "GameCube", false));   // someone lacks it
    v.back().reason = "Mia doesn't have this game";
    v.push_back(mk("d", "Crash Bash", "ps1", "PlayStation"));
    v.back().untested = true; v.back().last_played = 90;
    v.push_back(mk("e", "God of War", "ps2", "PlayStation 2", false));
    v.back().supported = false; v.back().favorite = true;
    v.push_back(mk("f", "Zelda", "n64", "Nintendo 64"));
    v.back().last_played = 70;

    CHECK(matches_query(v[0], "kart") && matches_query(v[0], "64 mario") && !matches_query(v[0], "zelda") && matches_query(v[0], ""));
    CHECK(matches_query(v[3], "playstation") && matches_query(v[3], "PS1"));

    /* sections: Recently played (usable only, newest first), Favorites (supported only), Everyone has it, then consoles */
    auto s = build_sections(v, Filter{});
    CHECK(s[0].key == "recent" && s[0].items.size() == 3 && s[0].items[0] == 3 && s[0].items[1] == 5 && s[0].items[2] == 1);
    const Section* fav = find(s, "favorites");
    CHECK(fav && fav->items.size() == 1 && fav->items[0] == 1);   // the unsupported favourite is not offered as a pick
    const Section* ev = find(s, "everyone");
    CHECK(ev && ev->items.size() == 4);                           // a, b, d, f (c lacks, e unsupported)
    CHECK(ev && ev->items[0] == 1);                               // native/pinned first
    const Section* n64 = find(s, "sys:n64");
    CHECK(n64 && n64->items.size() == 3 && v[n64->items[0]].pinned);
    CHECK(find(s, "sys:gc") && find(s, "sys:gc")->items.size() == 1);   // blocked games stay visible in their console section
    CHECK(find(s, "sys:ps2") && find(s, "sys:ps2")->items.size() == 1);
    for (size_t i = 1; i < s.size(); ++i) CHECK(!s[i].items.empty());

    /* console chips */
    auto ch = chips(v, "");
    CHECK(ch.size() == 4 && ch[0].name == "GameCube" && ch[0].count == 1);
    int n64count = 0;
    for (auto& c : ch) if (c.sys == "n64") n64count = c.count;
    CHECK(n64count == 3);
    CHECK(chips(v, "kart").size() == 1);
    /* chip filter narrows every section */
    Filter fc; fc.sys = "n64";
    auto s2 = build_sections(v, fc);
    for (auto& sec : s2) for (int i : sec.items) CHECK(v[i].sys == "n64");
    CHECK(!find(s2, "sys:gc") && find(s2, "sys:n64"));
    /* search = one flat Results section, usable first */
    Filter fq; fq.query = "o";
    auto s3 = build_sections(v, fq);
    CHECK(s3.size() == 1 && s3[0].key == "results");
    bool seen_unusable = false, order_ok = true;
    for (int i : s3[0].items) { if (!v[i].usable()) seen_unusable = true; else if (seen_unusable) order_ok = false; }
    CHECK(order_ok && !s3[0].items.empty());
    Filter fn; fn.query = "nonexistent";
    CHECK(build_sections(v, fn).empty());
    /* recently played is capped */
    std::vector<Item> big;
    for (int i = 0; i < 20; ++i) { Item it = mk(std::to_string(i), "G" + std::to_string(i), "gb", "Game Boy"); it.last_played = 100 + i; big.push_back(it); }
    auto s4 = build_sections(big, Filter{});
    CHECK(s4[0].key == "recent" && s4[0].items.size() == 8 && s4[0].items[0] == 19);

    /* layout + virtualisation over a 600-game library */
    std::vector<Item> lib;
    for (int i = 0; i < 600; ++i) lib.push_back(mk(std::to_string(i), "Game " + std::to_string(i), i % 2 ? "gba" : "n64", i % 2 ? "Game Boy Advance" : "Nintendo 64"));
    auto sl = build_sections(lib, Filter{});
    CHECK(sl.size() == 3);   // everyone + 2 consoles
    Layout L = build_layout(sl, 5, 40.f, 200.f);
    int tiles = 0;
    for (auto& r : L.rows) if (!r.header) tiles += r.count;
    CHECK(tiles == 1200);
    CHECK(L.y.size() == L.rows.size() + 1 && L.total == L.y.back());
    CHECK(L.rows.size() == 3 + 120 + 60 + 60);
    int a, b;
    visible_rows(L, 0, 600, 1, &a, &b);
    CHECK(a == 0 && b - a < 12);                 // only a handful of rows are ever submitted
    visible_rows(L, L.total - 600, 600, 1, &a, &b);
    CHECK(b == (int)L.rows.size() && b - a < 12);
    visible_rows(L, 5000, 600, 0, &a, &b);
    CHECK(L.y[a] <= 5000 && L.y[b - 1] <= 5600 && L.y[b] > 5600 - 1);
    CHECK(row_of(L, 0, 0) == 1 && row_of(L, 0, 5) == 2 && row_of(L, 0, 9999) == -1);
    Layout E = build_layout({}, 3, 40, 100);
    visible_rows(E, 0, 500, 2, &a, &b);
    CHECK(a == 0 && b == 0 && E.total == 0);

    std::printf(fails ? "FAILED (%d)\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
