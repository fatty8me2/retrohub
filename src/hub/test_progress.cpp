// Completion status + smart-shelf rule evaluation: pure logic (progress_logic.h).
#include "progress_logic.h"

#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

using namespace hubui::prog;
using hub::prefs::ShelfRule;

static Facts game(const char* key, const char* sys, const char* genre, int players, int year, bool fav, unsigned long long last, const char* status = "") {
    Facts f;
    f.key = key; f.system = sys; f.genre = genre; f.players = players; f.year = year; f.favorite = fav; f.last_played = last; f.status = status;
    return f;
}

int main() {
    const unsigned long long now = 1800000000ull, day = 86400ull;

    // ---- statuses ----
    CHECK(valid_status("") && valid_status("beaten") && !valid_status("nonsense"));
    CHECK(status_label("complete") == "100%" && status_badge("complete") == "100%" && status_badge("") == "" && status_badge("future") == "");
    CHECK(status_badge("playing") == "PLAYING" && status_label("") == "No status");

    // ---- notes ----
    CHECK(clean_note("  hi\x01 there \n") == "hi there");
    CHECK(clean_note(std::string(700, 'a')).size() == 500);
    std::string u;
    for (int i = 0; i < 300; ++i) u += "\xC3\xA9";   // 2-byte chars: 600 bytes
    std::string cu = clean_note(u);
    CHECK(cu.size() == 500 && ((unsigned char)cu.back() & 0xC0) != 0xC0);

    // ---- progress map edits ----
    std::map<std::string, hub::prefs::Progress> m;
    m = with_status(m, "k", "beaten");
    CHECK(m["k"].status == "beaten");
    m = with_note(m, "k", "good one");
    CHECK(m["k"].status == "beaten" && m["k"].note == "good one");
    m = with_status(m, "k", "");
    CHECK(m.count("k") && m["k"].note == "good one");   // the note keeps the entry alive
    m = with_note(m, "k", "");
    CHECK(m.empty());
    m = with_status(m, "k", "bogus");
    CHECK(m.empty());

    // ---- rule evaluation ----
    ShelfRule empty;
    CHECK(rule_is_empty(empty) && rule_summary(empty) == "Everything");
    Facts a = game("a", "n64", "Action", 4, 1999, true, now - 10 * day, "playing");
    Facts b = game("b", "gba", "RPG", 1, 2003, false, 0, "backlog");
    Facts c = game("c", "n64", "Racing", 2, 1996, false, now - 400 * day, "");
    Facts d = game("d", "gc", "Action", 4, 2005, true, now - 100 * day, "beaten");
    std::vector<Facts> all = {a, b, c, d};

    ShelfRule r;
    r.systems = {"n64"};
    CHECK(rule_select(r, all, now) == (std::vector<int>{0, 2}));
    r.systems = {"n64", "gc"};
    CHECK(rule_select(r, all, now) == (std::vector<int>{0, 2, 3}));
    r = ShelfRule(); r.genre = "action";   // case-insensitive
    CHECK(rule_select(r, all, now) == (std::vector<int>{0, 3}));
    r = ShelfRule(); r.min_players = 2;
    CHECK(rule_select(r, all, now) == (std::vector<int>{0, 2, 3}));
    r = ShelfRule(); r.min_players = 4;
    CHECK(rule_select(r, all, now) == (std::vector<int>{0, 3}));
    r = ShelfRule(); r.status = "backlog";
    CHECK(rule_select(r, all, now) == (std::vector<int>{1}));
    r = ShelfRule(); r.favorites_only = true;
    CHECK(rule_select(r, all, now) == (std::vector<int>{0, 3}));
    r = ShelfRule(); r.not_played_days = 90;   // never played counts; 10 days ago does not; 100 and 400 days ago do
    CHECK(rule_select(r, all, now) == (std::vector<int>{1, 2, 3}));
    r = ShelfRule(); r.not_played_days = 365;
    CHECK(rule_select(r, all, now) == (std::vector<int>{1, 2}));
    r = ShelfRule(); r.decade = 1990;
    CHECK(rule_select(r, all, now) == (std::vector<int>{0, 2}));
    r = ShelfRule(); r.decade = 2000;
    CHECK(rule_select(r, all, now) == (std::vector<int>{1, 3}));
    // every set field must match
    r = ShelfRule(); r.systems = {"n64"}; r.favorites_only = true; r.min_players = 4; r.decade = 1990; r.status = "playing";
    CHECK(rule_select(r, all, now) == (std::vector<int>{0}));
    r.status = "beaten";
    CHECK(rule_select(r, all, now).empty());
    // unknown year / players never match those fields
    Facts u0 = game("u", "n64", "Other", 0, 0, false, 0);
    r = ShelfRule(); r.decade = 1990;
    CHECK(!rule_matches(r, u0, now));
    r = ShelfRule(); r.min_players = 2;
    CHECK(!rule_matches(r, u0, now));
    // clock skew: a last-played time in the future counts as recent
    Facts fut = game("f", "n64", "Other", 1, 2000, false, now + 5 * day);
    r = ShelfRule(); r.not_played_days = 30;
    CHECK(!rule_matches(r, fut, now));

    CHECK(decade_of(1999) == 1990 && decade_of(2000) == 2000 && decade_of(0) == 0 && decade_of(12) == 0);

    // ---- summary + steppers ----
    r = ShelfRule(); r.systems = {"n64", "gc"}; r.min_players = 2; r.status = "backlog"; r.favorites_only = true; r.not_played_days = 90; r.decade = 1990;
    CHECK(rule_summary(r) == "n64 or gc, 2+ players, Backlog, favorites, not played in 90 days, the 1990s");
    CHECK(next_players(0) == 2 && next_players(2) == 3 && next_players(4) == 0);
    CHECK(next_days(0) == 30 && next_days(365) == 0 && next_days(90) == 180);
    CHECK(next_decade(0) == 1980 && next_decade(1990) == 2000 && next_decade(2020) == 0);
    CHECK(next_status("") == "playing" && next_status("backlog") == "" && next_status("beaten") == "complete");
    ShelfRule t;
    toggle_system(&t, "n64"); toggle_system(&t, "gba");
    CHECK(t.systems.size() == 2);
    toggle_system(&t, "n64");
    CHECK(t.systems == (std::vector<std::string>{"gba"}));

    // ---- smart shelf edits + resolving ----
    hubui::cust::Shelves sh;
    std::string made;
    CHECK(!add_smart_shelf(&sh, "Empty", ShelfRule()));   // an empty rule would match everything: refused
    ShelfRule favs; favs.favorites_only = true;
    CHECK(add_smart_shelf(&sh, "Favs", favs, &made) && made == "Favs" && sh.size() == 1 && sh[0].smart);
    CHECK(add_smart_shelf(&sh, "favs", favs, &made) && made == "favs 2");
    ShelfRule np; np.systems = {"n64"};
    CHECK(edit_smart_shelf(&sh, 0, "Favs", np) && sh[0].rule.systems.size() == 1 && !sh[0].rule.favorites_only);
    CHECK(!edit_smart_shelf(&sh, 0, "Favs", np));   // unchanged
    CHECK(!edit_smart_shelf(&sh, 5, "x", np) && !edit_smart_shelf(&sh, 0, "x", ShelfRule()));
    std::map<std::string, std::string> titles = {{"a", "Zelda"}, {"b", "Banjo"}, {"c", "agent"}, {"d", "Mario"}};
    hubui::cust::Shelves res = resolve_smart(sh, all, titles, now);
    CHECK(res[0].keys == (std::vector<std::string>{"c", "a"}));   // n64 games, by title ("agent" < "Zelda"), case-insensitive
    hub::prefs::Shelf hand; hand.name = "Hand"; hand.keys = {"d"};
    sh.push_back(hand);
    res = resolve_smart(sh, all, titles, now);
    CHECK(res[2].keys == (std::vector<std::string>{"d"}));   // hand-picked shelves are untouched
    CHECK(resolve_smart(sh, all, titles, now, 1)[0].keys.size() == 1);

    std::printf(g_fail ? "hub_progress_test FAILED\n" : "hub_progress_test OK\n");
    return g_fail ? 1 : 0;
}
