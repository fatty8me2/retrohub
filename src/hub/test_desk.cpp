/* hub_desk_test: the Library desk list model (desk_logic.h) and the "ui_layout" / "desk_groups" settings round-trip. */
#include "desk_logic.h"
#include "logic.h"

#include <cstdio>
#include <string>
#include <vector>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

using namespace hubdesk;

static Game G(const char* key, const char* title, const char* console, unsigned long long played = 0, bool fav = false) {
    Game g;
    g.key = key; g.title = title; g.console = console; g.last_played = played; g.favorite = fav;
    return g;
}
static std::vector<std::string> ids(const Model& m) {
    std::vector<std::string> v;
    for (const Group& g : m.groups) v.push_back(g.id);
    return v;
}
static std::string row_text(const Model& m, const std::vector<Game>& games, int r) {
    const Row& row = m.rows[(size_t)r];
    return row.header() ? "[" + m.groups[(size_t)row.group].id + "]" : games[(size_t)row.game].key;
}

int main() {
    /* ---- setting values ---- */
    CHECK(normalize_layout("desk") == "desk" && normalize_layout("traditional") == "traditional");
    CHECK(normalize_layout("") == "traditional" && normalize_layout("Desk") == "traditional" && normalize_layout("grid") == "traditional");
    CHECK(is_desk("desk") && !is_desk("traditional") && !is_desk(""));
    {
        hubui::Settings d;
        CHECK(d.ui_layout == "traditional" && d.desk_groups.empty());
        hubui::Settings old;   // a file from before the setting existed: Traditional
        CHECK(hubui::settings_from_json("{\"name\": \"x\"}", &old) && old.ui_layout == "traditional");
        hubui::Settings bad;
        CHECK(hubui::settings_from_json("{\"ui_layout\": \"sideways\"}", &bad) && bad.ui_layout == "traditional");
        hubui::Settings s;
        s.ui_layout = "desk";
        s.desk_groups = "n64=0,recent=1";
        hubui::Settings r;
        CHECK(hubui::settings_from_json(hubui::settings_to_json(s), &r) && r.ui_layout == "desk" && r.desk_groups == "n64=0,recent=1");
        s.ui_layout = "traditional";
        CHECK(hubui::settings_from_json(hubui::settings_to_json(s), &r) && r.ui_layout == "traditional");
    }

    /* ---- open / closed groups text ---- */
    {
        OpenMap m = parse_open("recent=1,n64=0,,bad,gc=2,=1,wiiu=1");
        CHECK(m.size() == 3 && m["recent"] && !m["n64"] && m["wiiu"]);
        CHECK(format_open(m) == "n64=0,recent=1,wiiu=1");
        CHECK(parse_open(format_open(m)) == m);
        CHECK(parse_open("").empty());
        CHECK(default_open("gc", "n64", 10) && default_open("favs", "n64", 40));
        CHECK(default_open("recent", "n64", 41) && default_open("n64", "n64", 41) && !default_open("gc", "n64", 41) && !default_open("favs", "n64", 41));
    }

    /* ---- groups ---- */
    std::vector<Game> games = {
        G("a", "Zelda", "n64", 500), G("b", "banjo", "n64", 0, true), G("c", "Wind Waker", "gc", 900, true), G("d", "Pokemon Emerald", "gba", 100),
        G("e", "Mario Kart World", "switch2", 300), G("f", "Weird", "zz64"), G("g", "Smash", "n64"), G("h", "Hidden Game", "gc", 999),
        G("i", "Metroid", "gc", 50), G("j", "Kirby", "gba", 20),
    };
    games[6].pinned = true;
    games[7].hidden = true;
    {
        Model m = build(games, "", OpenMap());
        CHECK(m.total == 9 && m.matches == 9 && !m.searching && m.first_console == "n64");
        CHECK((ids(m) == std::vector<std::string>{"recent", "favs", "n64", "gc", "switch2", "gba", "other"}));
        const Group& rec = m.groups[0];
        CHECK(rec.items.size() == 5);
        CHECK(games[(size_t)rec.items[0]].key == "c" && games[(size_t)rec.items[1]].key == "a" && games[(size_t)rec.items[2]].key == "e" && games[(size_t)rec.items[3]].key == "d" && games[(size_t)rec.items[4]].key == "i");   // newest first, 5 at most
        const Group& fav = m.groups[1];
        CHECK(fav.items.size() == 2 && games[(size_t)fav.items[0]].key == "b" && games[(size_t)fav.items[1]].key == "c");   // A-Z, any case
        const Group& n64 = m.groups[2];
        CHECK(n64.items.size() == 3 && games[(size_t)n64.items[0]].key == "g" && games[(size_t)n64.items[1]].key == "b" && games[(size_t)n64.items[2]].key == "a");   // the pinned port first
        CHECK(m.groups[3].items.size() == 2);   // the hidden GameCube game is not listed (nor in Recent)
        CHECK(m.groups[6].id == "other" && m.groups[6].items.size() == 1);
        for (const Group& g : m.groups) CHECK(g.open);   // a small library: every group open
        CHECK(m.rows.size() == 7 + 5 + 2 + 3 + 2 + 1 + 2 + 1);
        CHECK(row_text(m, games, 0) == "[recent]" && row_text(m, games, 1) == "c" && row_text(m, games, 6) == "[favs]");
    }
    {   // no history, no favourites: those groups are left out
        std::vector<Game> plain = {G("x", "One", "gba"), G("y", "Two", "n64")};
        Model m = build(plain, "", OpenMap());
        CHECK((ids(m) == std::vector<std::string>{"n64", "gba"}) && m.first_console == "n64");
    }
    {   // a big library: Recent and the first console open, the rest closed; the player's choices win
        std::vector<Game> big;
        for (int i = 0; i < 45; ++i) {
            static const char* cons[] = {"gc", "gba", "ps1"};
            big.push_back(G(("k" + std::to_string(i)).c_str(), ("Game " + std::to_string(100 + i)).c_str(), cons[i % 3], i < 3 ? 100 + i : 0, i == 7));
        }
        Model m = build(big, "", OpenMap());
        CHECK((ids(m) == std::vector<std::string>{"recent", "favs", "gc", "ps1", "gba"}) && m.first_console == "gc");
        CHECK(m.groups[0].open && !m.groups[1].open && m.groups[2].open && !m.groups[3].open && !m.groups[4].open);
        CHECK(m.rows.size() == 5 + 3 + 15);
        OpenMap om = parse_open("gc=0,gba=1");
        Model m2 = build(big, "", om);
        CHECK(!m2.groups[2].open && m2.groups[4].open && m2.groups[0].open);
        /* a search opens every group with a match (even ones the player closed), and leaves the empty ones out */
        Model s = build(big, "game 12", om);
        CHECK(s.searching && s.matches == 10 && s.total == 45);   // Game 112, 120-129
        for (const Group& g : s.groups) CHECK(g.open && !g.items.empty());
        CHECK((ids(s) == std::vector<std::string>{"gc", "ps1", "gba"}));
        Model s2 = build(big, "  GAME 112 ", om);
        CHECK(s2.matches == 1 && s2.groups.size() == 1 && s2.rows.size() == 2);
        std::set<std::string> closed = {"gc"};
        Model s3 = build(big, "game 12", om, closed);
        CHECK(!s3.groups[0].open && s3.groups[1].open);
        Model none = build(big, "nothing like this", om);
        CHECK(none.groups.empty() && none.rows.empty() && none.matches == 0 && none.first_console == "gc");
    }

    /* ---- identity across rebuilds ---- */
    {
        Model m = build(games, "", OpenMap());
        const int row_a_n64 = 6 + 3 + 3;   // [recent] 5 rows, [favs] 2 rows, [n64] g b a
        CHECK(row_text(m, games, row_a_n64) == "a");
        RowRef ref = ref_of(m, games, row_a_n64);
        CHECK(ref.group == "n64" && ref.key == "a");
        CHECK(find_row(m, games, ref) == row_a_n64);
        CHECK(row_of_key(m, games, "a") == 2);   // its Recent row comes first
        std::vector<Game> g2 = games;
        g2.insert(g2.begin(), G("new", "Aaa", "n64"));   // a rescan adds a game above it
        Model m2 = build(g2, "", OpenMap());
        CHECK(row_text(m2, g2, find_row(m2, g2, ref)) == "a" && m2.groups[(size_t)m2.rows[(size_t)find_row(m2, g2, ref)].group].id == "n64");
        OpenMap closed = parse_open("n64=0");
        Model m3 = build(games, "", closed);   // its group closed: the game's Recent row
        CHECK(find_row(m3, games, ref) == 2);
        RowRef gone{"n64", "zzz"};
        CHECK(row_text(m3, games, find_row(m3, games, gone)) == "[n64]");   // the game is gone: its group header
        CHECK(find_row(m3, games, RowRef{"nope", "zzz"}) == -1 && find_row(m3, games, RowRef()) == -1);
        CHECK(has_game(m3, games, "b") && !has_game(m3, games, "h") && !has_game(m3, games, "nope"));
        CHECK(first_game_row(m) == 1 && first_game_row(Model()) == -1);
    }

    /* ---- movement ---- */
    {
        std::vector<Game> g = {G("a", "Alpha", "n64"), G("b", "Beta", "n64"), G("c", "Gamma", "gc"), G("d", "Delta", "gba")};
        Model m = build(g, "", OpenMap());   // [n64] a b [gc] c [gba] d
        CHECK(m.rows.size() == 7);
        CHECK(step(m, 0, Key::Up, 5).act == Act::Edge);
        CHECK(step(m, 6, Key::Down, 5).act == Act::Edge);
        Step s = step(m, 2, Key::Down, 5);
        CHECK(s.act == Act::Move && s.row == 3);   // crosses into the next group's header
        s = step(m, 2, Key::Left, 5);
        CHECK(s.act == Act::Move && s.row == 0);   // a game: to its header
        s = step(m, 0, Key::Left, 5);
        CHECK(s.act == Act::Close && s.group == 0);
        s = step(m, 4, Key::Right, 5);
        CHECK(s.act == Act::EnterPane);
        s = step(m, 3, Key::Right, 5);
        CHECK(s.act == Act::Move && s.row == 4);   // an open header: its first game
        s = step(m, 1, Key::Activate, 5);
        CHECK(s.act == Act::Play && s.row == 1);
        s = step(m, 3, Key::Activate, 5);
        CHECK(s.act == Act::Close && s.group == 1);
        CHECK(step(m, 1, Key::PageDown, 3).row == 4 && step(m, 5, Key::PageDown, 3).row == 6 && step(m, 6, Key::PageDown, 3).act == Act::None);
        CHECK(step(m, 5, Key::PageUp, 3).row == 2 && step(m, 1, Key::PageUp, 3).row == 0 && step(m, 0, Key::PageUp, 3).act == Act::None);
        CHECK(step(m, 0, Key::PageDown, 3).row == 4);   // a page that lands on an open group's header goes on to its first game
        CHECK(step(m, 6, Key::PageUp, 3).row == 4);     // 6 - 3 = [gc]: its game
        CHECK(step(m, 2, Key::PageUp, 1).row == 1);
        CHECK(step(m, 4, Key::Home, 3).row == 0 && step(m, 1, Key::End, 3).row == 6 && step(m, 6, Key::End, 3).act == Act::None);
        CHECK(step(m, 99, Key::Down, 3).act == Act::Move && step(m, 99, Key::Down, 3).row == 6 && step(m, -1, Key::Up, 3).row == 0);
        CHECK(step(Model(), 0, Key::Down, 3).act == Act::None);
        Model c = build(g, "", parse_open("n64=0"));   // [n64] [gc] c [gba] d
        CHECK(c.rows.size() == 5);
        s = step(c, 0, Key::Right, 5);
        CHECK(s.act == Act::Open && s.group == 0);
        s = step(c, 0, Key::Left, 5);
        CHECK(s.act == Act::Leave);
        s = step(c, 0, Key::Activate, 5);
        CHECK(s.act == Act::Open);
    }

    /* ---- virtual rows ---- */
    {
        std::vector<Game> g = {G("a", "Alpha", "n64"), G("b", "Beta", "n64"), G("c", "Gamma", "gc")};
        Model m = build(g, "", OpenMap());   // [n64] a b [gc] c
        Metrics mt{30.f, 40.f, 10.f};
        Layout l = layout_rows(m.rows, mt);
        CHECK(l.top.size() == 5 && l.top[0] == 0 && l.top[1] == 30 && l.top[2] == 70 && l.top[3] == 120 && l.top[4] == 150 && l.total == 190);
        int f = -2, la = -2;
        visible_rows(l, m.rows, mt, 0, 60, &f, &la);
        CHECK(f == 0 && la == 1);
        visible_rows(l, m.rows, mt, 75, 40, &f, &la);   // 75..115: row 2 (70..110) only; 110..120 is the gap
        CHECK(f == 2 && la == 2);
        visible_rows(l, m.rows, mt, 112, 5, &f, &la);   // entirely inside the gap
        CHECK(f == -1 && la == -1);
        visible_rows(l, m.rows, mt, 100, 1000, &f, &la);
        CHECK(f == 2 && la == 4);
        visible_rows(l, m.rows, mt, 500, 100, &f, &la);
        CHECK(f == -1 && la == -1);
        visible_rows(Layout(), m.rows, mt, 0, 100, &f, &la);
        CHECK(f == -1);
        CHECK(row_at(l, m.rows, mt, 0) == 0 && row_at(l, m.rows, mt, 69.9f) == 1 && row_at(l, m.rows, mt, 115) == -1 && row_at(l, m.rows, mt, 160) == 4 &&
              row_at(l, m.rows, mt, 190) == -1 && row_at(l, m.rows, mt, -1) == -1);
        CHECK(reveal(l, m.rows, mt, 4, 0, 100, 0) == 90);     // row 4 ends at 190: scroll so it ends at the bottom
        CHECK(reveal(l, m.rows, mt, 1, 90, 100, 5) == 25);    // row 1 starts at 30: 5 px of room above
        CHECK(reveal(l, m.rows, mt, 2, 40, 100, 0) == 40);    // already in view: unchanged
        CHECK(reveal(l, m.rows, mt, 0, 50, 100, 8) == 0);     // clamped at the top
        CHECK(clamp_scroll(500, 190, 100) == 90 && clamp_scroll(-3, 190, 100) == 0 && clamp_scroll(20, 50, 100) == 0);
        CHECK(page_rows(400, 40) == 9 && page_rows(30, 40) == 1 && page_rows(100, 0) == 1);
        Thumb t = thumb(0, 190, 100, 100, 20);
        CHECK(t.shown && t.y0 == 0 && std::fabs(t.y1 - 100.f * 100.f / 190.f) < 0.01f);
        t = thumb(90, 190, 100, 100, 20);
        CHECK(std::fabs(t.y1 - 100.f) < 0.01f);
        CHECK(!thumb(0, 90, 100, 100, 20).shown);
        Thumb tiny = thumb(0, 100000, 100, 100, 20);
        CHECK(tiny.y1 - tiny.y0 == 20);
        const float th = t.y1 - t.y0;
        CHECK(std::fabs(scroll_for_thumb(100 - th, th, 190, 100, 100) - 90) < 0.01f && scroll_for_thumb(0, th, 190, 100, 100) == 0 && scroll_for_thumb(5, 100, 190, 100, 100) == 0);
    }
    {   // the column: a quarter of the content, clamped
        CHECK(column_width(888, 8) == 240);    // 1024 px window: the minimum
        CHECK(column_width(1200, 8) == 300);
        CHECK(column_width(1800, 8) == 384);   // 1920 px: the maximum
        CHECK(column_width(400, 8) == 200);    // never more than half of a very narrow content area
        CHECK(column_width(1076, 12) == 360);  // UI size 150%
    }

    if (fails) { std::printf("%d check(s) failed\n", fails); return 1; }
    std::printf("hub_desk_test: all checks passed\n");
    return 0;
}
