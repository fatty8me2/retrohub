#include "logic.h"
#include "state_slots.h"
#include "state_tombstones.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static void put(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary);
    f << s;
}

int main() {
    using namespace hubui;

    /* ---- state listing ---- */
    fs::path d = fs::temp_directory_path() / "hub_states_test";
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    {
        auto v = list_states(d.string());
        CHECK(v.size() == 11 && v[0].slot == -1 && v[1].slot == 0 && v[10].slot == 9);
        CHECK(count_states(v, true) == 0);
    }
    put(d / "slot0.state", "aaaa");
    put(d / "slot0.png", "png");
    put(d / "slot0.json", "{\"game_key\":\"gba:x\",\"time\":1700000000,\"playtime\":3725,\"disc\":1}");
    put(d / "slot3.state", "bbbbbb");                 // no sidecar, no thumbnail: mtime fallback
    put(d / "auto.state", "cc");
    put(d / "slot5.json", "{\"time\":5}");            // sidecar without a state: not listed
    put(d / "slot7.state", "d");
    put(d / "slot7.json", "not json");                // broken sidecar: still lists
    {
        auto v = list_states(d.string());
        CHECK(v[1].present && v[1].time == 1700000000 && v[1].playtime == 3725 && v[1].disc == 1 && v[1].bytes == 4);
        CHECK(!v[1].thumb_path.empty());
        CHECK(v[4].present && v[4].time > 1600000000 && v[4].playtime == 0 && v[4].thumb_path.empty());
        CHECK(v[0].present && v[0].slot == -1);
        CHECK(!v[6].present);
        CHECK(v[8].present && v[8].time > 0);
        CHECK(count_states(v, true) == 4 && count_states(v, false) == 3);
    }
    CHECK(delete_state(d.string(), 0));
    CHECK(!fs::exists(d / "slot0.png") && !fs::exists(d / "slot0.json") && !fs::exists(d / "slot0.state"));
    CHECK(delete_state(d.string(), 9));   // nothing there: still "gone"
    CHECK(delete_state(d.string(), -1) && !fs::exists(d / "auto.state"));
    CHECK(count_states(list_states(d.string()), true) == 2);
    fs::remove_all(d, ec);
    CHECK(count_states(list_states((d / "missing").string()), true) == 0);

    {   // saves-F15: a delete inside a vault game folder is remembered by WHAT was deleted (size + SHA-256) so the cloud sync does not bring that file back
        namespace tb = hub::saves::tomb;
        fs::path vault = d.parent_path() / "tomb_vault";
        fs::remove_all(vault, ec);
        fs::path sd = vault / "gba" / "gba-abc" / "states";
        fs::create_directories(sd, ec);
        put(sd / "slot2.state", "x"); put(sd / "slot2.png", "p"); put(sd / "auto.state", "a");
        CHECK(delete_state(sd.string(), 2));
        auto m = tb::load(vault.string());
        CHECK(m.size() == 2 && m.count("gba/gba-abc/states/slot2.state") && m.count("gba/gba-abc/states/slot2.png"));   // slot2.json never existed: nothing to identify
        CHECK(m["gba/gba-abc/states/slot2.state"].size == 1 && m["gba/gba-abc/states/slot2.state"].sha == "2d711642b726b04401627ca9fbac32f5c8530fb1903cc4db02258717921a4881");   // SHA-256("x")
        CHECK(!m.count("gba/gba-abc/states/auto.state") && fs::exists(sd / "auto.state"));
        CHECK(delete_state(sd.string(), -1));
        CHECK(tb::load(vault.string()).count("gba/gba-abc/states/auto.state"));
        tb::clear(vault.string(), "gba/gba-abc/states/auto.state");
        CHECK(!tb::load(vault.string()).count("gba/gba-abc/states/auto.state"));
        // SHA-256 of a longer input (two blocks) matches the known value
        put(sd / "long.bin", std::string(1000, 'a'));
        tb::Entry e;
        CHECK(tb::identity_of_file((sd / "long.bin").string(), &e) && e.size == 1000 && e.sha == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
        std::string v2, k2;
        CHECK(!tb::key_for_state_file("C:\\tmp\\states", "a.state", &v2, &k2));   // not vault shaped: nothing remembered
        CHECK(tb::key_for_state_file("C:\\v\\n64\\k\\states\\", "a.state", &v2, &k2) && v2 == "C:/v" && k2 == "n64/k/states/a.state");
        fs::remove_all(vault, ec);
    }

    CHECK(state_slot_label(-1) == "Auto" && state_slot_label(4) == "Slot 4");
    CHECK(system_has_states("gba", false) && system_has_states("ps1", false) && !system_has_states("gc", false));
    CHECK(system_has_states("n64", false));
    CHECK(!system_has_states("n64", true) && !no_states_reason("gc", false).empty() && no_states_reason("gba", false).empty());

    /* ---- resume wording ---- */
    CHECK(ago_text(1000, 0) == "never" && ago_text(1000, 990) == "just now" && ago_text(1000, 2000) == "just now");
    CHECK(ago_text(10000, 10000 - 300) == "5 min ago" && ago_text(100000, 100000 - 7300) == "2h ago");
    CHECK(ago_text(1000000, 1000000 - 90000) == "yesterday" && ago_text(1000000, 1000000 - 86400 * 5) == "5 days ago");
    CHECK(ago_text(10000000, 10000000 - 86400 * 21) == "3 weeks ago");
    CHECK(resume_line("Pokemon", 100000, 100000 - 7300, " - ") == "Resume Pokemon - last played 2h ago");
    CHECK(resume_line("Pokemon", 100000, 0, " - ") == "Play Pokemon");
    CHECK(can_resume(true, true) && !can_resume(true, false) && !can_resume(false, true));
    CHECK(playtime_text(0).empty() && playtime_text(125) == "2:05" && playtime_text(3900) == "1h 05m");

    /* ---- sort modes ---- */
    CHECK(sort_from_key("most_played") == SortMode::MostPlayed && sort_from_key("zzz") == SortMode::Title);
    for (int i = 0; i < kSortModes; ++i) CHECK(sort_from_key(sort_key((SortMode)i)) == (SortMode)i);
    CHECK(next_sort(SortMode::Title) == SortMode::LastPlayed && next_sort(SortMode::Console) == SortMode::Title);
    CHECK(next_sort(SortMode::Title, -1) == SortMode::Console && next_sort(SortMode::RecentlyAdded) == SortMode::ReleaseYear);
    CHECK(sort_from_key("release_year") == SortMode::ReleaseYear && sort_from_key("console") == SortMode::Console && sort_from_key("some_future_mode") == SortMode::Title);

    std::vector<ListItem> it = {
        {"a", "Alpha", "n64", false, 100, 50, 7},
        {"b", "Bravo", "n64", false, 300, 10, 5},
        {"c", "Charlie", "n64", false, 0, 0, 9},
        {"d", "Delta", "n64", false, 200, 900, 1},
        {"n", "Zed Native", "n64", true, 0, 0, 0},
    };
    auto names = [&](const std::vector<int>& v) { std::string s; for (int i : v) s += it[i].key; return s; };
    CHECK(names(filter_sort(it, "", "")) == "nabcd");
    CHECK(names(filter_sort(it, "", "", SortMode::LastPlayed)) == "nbdac");   // pinned, 300, 200, 100, never
    CHECK(names(filter_sort(it, "", "", SortMode::MostPlayed)) == "ndabc");
    CHECK(names(filter_sort(it, "", "", SortMode::RecentlyAdded)) == "ncabd");
    CHECK(names(filter_sort(it, "", "a", SortMode::LastPlayed)) == "nbdac");   // alpha/bravo/charlie/delta all contain "a"; zed native has "a"
    it[2].last_played = 0; it[0].last_played = 0; it[1].last_played = 0; it[3].last_played = 0;
    CHECK(names(filter_sort(it, "", "", SortMode::LastPlayed)) == "nabcd");   // all tied: title order
    auto secs = group_by_console(it, nullptr, SortMode::MostPlayed);
    CHECK(secs.size() == 1 && secs[0].items.size() == 5 && it[secs[0].items[1]].key == "d");

    /* ---- shelves honour the sort; "recent" shelf is always newest-first ---- */
    std::vector<ShelfGame> g(3);
    g[0].key = "a"; g[0].title = "Alpha"; g[0].system = "gba"; g[0].last_played = 10; g[0].play_seconds = 5;
    g[1].key = "b"; g[1].title = "Bravo"; g[1].system = "gba"; g[1].last_played = 50; g[1].play_seconds = 1;
    g[2].key = "c"; g[2].title = "Charlie"; g[2].system = "gba"; g[2].play_seconds = 99;
    auto sh = compose_shelves(g, 12, SortMode::MostPlayed);
    CHECK(sh.size() == 2 && sh[0].id == "recent" && sh[0].items[0] == 1);
    CHECK(sh[1].id == "console:gba" && sh[1].items[0] == 2 && sh[1].items[1] == 0 && sh[1].items[2] == 1);
    CHECK(compose_shelves(g)[1].items[0] == 0);

    /* ---- settings persist the sort and the cloud-states switch ---- */
    Settings s, t;
    CHECK(s.cloud_states && s.library_sort == "title");
    s.library_sort = "most_played";
    s.cloud_states = false;
    CHECK(settings_from_json(settings_to_json(s), &t) && t.library_sort == "most_played" && !t.cloud_states);
    Settings u;
    CHECK(settings_from_json("{\"name\":\"x\"}", &u) && u.cloud_states && u.library_sort == "title");

    /* ---- car-nav: release year, console, favorites first, reverse ---- */
    {
        std::vector<ListItem> v = {
            {"a", "Alpha", "gba", false, 0, 0, 0}, {"b", "bravo", "n64", false, 0, 0, 0}, {"c", "Charlie", "gc", false, 0, 0, 0},
            {"d", "Delta", "n64", false, 0, 0, 0}, {"e", "Echo", "gba", false, 0, 0, 0}, {"n", "Zed Native", "n64", true, 0, 0, 0},
        };
        v[0].year = 2001; v[1].year = 1998; v[2].year = 0; v[3].year = 1998; v[4].year = 2010;
        v[2].favorite = true; v[4].favorite = true;
        auto nm = [&](const std::vector<int>& o) { std::string r; for (int i : o) r += v[i].key; return r; };
        CHECK(nm(filter_sort(v, "", "", SortSpec(SortMode::ReleaseYear))) == "nbdaec");                  // pinned, 1998 (title tie-break b < d), 2001, 2010, unknown last
        CHECK(nm(filter_sort(v, "", "", SortSpec(SortMode::ReleaseYear, true))) == "neabdc");             // newest first: unknown still last, ties still by title
        CHECK(nm(filter_sort(v, "", "", SortSpec(SortMode::Title))) == "nabcde");
        CHECK(nm(filter_sort(v, "", "", SortSpec(SortMode::Title, true))) == "nedcba");
        CHECK(nm(filter_sort(v, "", "", SortSpec(SortMode::Console))) == "nbdcae");                       // n64, gc, gba (the library's console order), titles inside
        CHECK(nm(filter_sort(v, "", "", SortSpec(SortMode::Console, true))) == "naecbd");
        CHECK(nm(filter_sort(v, "", "", SortSpec(SortMode::Title, false, true))) == "nceabd");            // favorites first, then the order
        CHECK(nm(filter_sort(v, "", "", SortSpec(SortMode::ReleaseYear, false, true))) == "necbda");
        v.push_back({"g", "Ghost", "n64", false, 0, 0, 0}); v.back().ghost = true; v.back().year = 1980;
        CHECK(nm(filter_sort(v, "", "", SortSpec(SortMode::ReleaseYear, false, true))).back() == 'g');     // friends' games stay last whatever the sort
        CHECK(sort_label(SortSpec(SortMode::Title)) == "Title A-Z" && sort_label(SortSpec(SortMode::Title, true)) == "Title Z-A");
        CHECK(sort_label(SortSpec(SortMode::ReleaseYear, false, true)) == "Favorites, Year (oldest)");
        SortSpec fromMode = SortMode::MostPlayed;   // implicit: old call sites keep compiling
        CHECK(fromMode.mode == SortMode::MostPlayed && !fromMode.reverse && !fromMode.favs_first);
        {   // Console + Reverse in the grouped layouts: the last console first, "Other" still last
            std::vector<ListItem> g = {{"a", "Alpha", "gba", false, 0, 0, 0}, {"b", "Bravo", "n64", false, 0, 0, 0}, {"c", "Charlie", "gc", false, 0, 0, 0}, {"x", "Xeno", "zzz", false, 0, 0, 0}};
            auto order = [&](SortSpec sp) { std::string r; for (auto& c : group_by_console(g, nullptr, sp)) r += c.system + ","; return r; };
            CHECK(order(SortSpec(SortMode::Console)) == "n64,gc,gba,other,");
            CHECK(order(SortSpec(SortMode::Console, true)) == "gba,gc,n64,other,");
            CHECK(order(SortSpec(SortMode::Title, true)) == "n64,gc,gba,other,");   // other sorts keep the library's console order
            std::vector<ShelfGame> sg(3);
            sg[0].key = "a"; sg[0].title = "Alpha"; sg[0].system = "gba"; sg[1].key = "b"; sg[1].title = "Bravo"; sg[1].system = "n64"; sg[2].key = "c"; sg[2].title = "Charlie"; sg[2].system = "gc";
            auto sh = compose_shelves(sg, 12, SortSpec(SortMode::Console, true));
            CHECK(sh.size() == 3 && sh[0].id == "console:gba" && sh[2].id == "console:n64");
        }
        {   // Title sort: digits, symbols and non-ASCII letters form ONE "#" block ahead of A (behind Z when reversed)
            std::vector<ListItem> h = {{"1", "1942", "n64", false, 0, 0, 0}, {"e", "Ãtoile", "n64", false, 0, 0, 0}, {"z", "Zed", "n64", false, 0, 0, 0},
                                       {"s", "!!!", "n64", false, 0, 0, 0}, {"a", "Alpha", "n64", false, 0, 0, 0}};
            auto hn = [&](SortSpec sp) { std::string r; for (int i : filter_sort(h, "", "", sp)) r += h[i].key; return r; };
            for (auto& it : h) prepare_sort_key(it);   // precomputed keys give the same order
            const std::string a1 = hn(SortSpec(SortMode::Title));
            CHECK(a1.size() == 5 && a1.substr(3) == "az" && a1.find('1') < 3 && a1.find('e') < 3 && a1.find('s') < 3);   // the three "#" games first, then Alpha, Zed
            const std::string r1 = hn(SortSpec(SortMode::Title, true));
            CHECK(r1.size() == 5 && r1.substr(0, 2) == "za" && r1.find('1') > 1 && r1.find('e') > 1 && r1.find('s') > 1);
        }
        Settings s0, s1;
        s0.library_sort = "release_year"; s0.library_sort_rev = true; s0.library_favs_first = true;
        CHECK(settings_from_json(settings_to_json(s0), &s1) && s1.library_sort == "release_year" && s1.library_sort_rev && s1.library_favs_first);
        Settings s2;
        CHECK(settings_from_json("{\"library_sort\":\"from_the_future\"}", &s2) && sort_from_key(s2.library_sort) == SortMode::Title && !s2.library_sort_rev && !s2.library_favs_first);
    }

    std::printf(fails ? "hub_states_test: %d FAILED\n" : "hub_states_test: all passed\n", fails);
    return fails ? 1 : 0;
}
