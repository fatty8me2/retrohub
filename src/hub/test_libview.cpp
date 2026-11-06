#include "libview_logic.h"
#include "logic.h"

#include <cstdio>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main() {
    using namespace hubui;
    CHECK(layout_from_key("list") == LibLayout::List && layout_from_key("zzz") == LibLayout::Grid);
    for (int i = 0; i < kLibLayouts; ++i) CHECK(layout_from_key(layout_key((LibLayout)i)) == (LibLayout)i);

    CHECK(letter_of("Zelda") == "Z" && letter_of("zelda") == "Z" && letter_of("1080 Snowboarding") == "#");
    CHECK(letter_of("'Cuda") == "C" && letter_of("") == "" && letter_of("!!!") == "#");

    std::vector<std::string> lab = {"", "A", "A", "B", "B", "B", "D", "A", "C"};
    auto runs = build_runs(lab);
    CHECK(runs.size() == 6 && runs[0].label.empty() && runs[1].first == 1 && runs[2].first == 3 && runs[4].first == 7);
    auto marks = strip_marks(runs);
    CHECK(marks.size() == 4 && marks[0].label == "A" && marks[0].first == 1 && marks[3].label == "C" && marks[3].first == 8);
    CHECK(run_of(runs, 0) == 0 && run_of(runs, 2) == 1 && run_of(runs, 8) == 5 && run_of({}, 0) == -1);
    CHECK(jump_target(runs, 1, 1) == 3);      // A -> B
    CHECK(jump_target(runs, 4, 1) == 6);      // inside B -> D
    CHECK(jump_target(runs, 8, 1) == -1);     // last run
    CHECK(jump_target(runs, 4, -1) == 3);     // inside a run: its start first
    CHECK(jump_target(runs, 3, -1) == 1);     // at a run start: the previous run
    CHECK(jump_target(runs, 1, -1) == -1);    // the unlabelled run is not a target
    CHECK(strip_slot(0, 100, 4) == 0 && strip_slot(99, 100, 4) == 3 && strip_slot(500, 100, 4) == 3 && strip_slot(-5, 100, 4) == 0 && strip_slot(1, 100, 0) == -1);
    {
        auto t = strip_targets(marks);   // A B C D present
        CHECK(t.size() == 27);
        CHECK(marks[t[0]].label == "A" && marks[t[1]].label == "B" && marks[t[2]].label == "C" && marks[t[3]].label == "D");
        CHECK(t[4] == -1 && t[26] == -1);   // E.. and # have nothing at or after
    }

    // list column sort keys
    std::vector<ListRow> rows(6);
    rows[0] = {"k0", "Zelda", "n64", false, false, 4, 100, 5000};
    rows[1] = {"k1", "alpha", "gba", false, false, 1, 0, 0};
    rows[2] = {"k2", "Beta", "n64", false, false, 2, 300, 60};
    rows[3] = {"k3", "Smash", "n64", true, false, 4, 50, 9};
    rows[4] = {"k4", "Friend Game", "gc", false, true, 4, 0, 0};
    rows[5] = {"k5", "Mario", "gc", false, false, 4, 200, 0};
    auto ord = [&](ListCol c, bool d) { return sort_list_rows(rows, nullptr, c, d); };
    {
        auto o = ord(ListCol::Title, false);   // pinned first, case-insensitive A-Z, ghost last
        CHECK((o == std::vector<int>{3, 1, 2, 5, 0, 4}));
        o = ord(ListCol::Title, true);
        CHECK((o == std::vector<int>{3, 0, 5, 2, 1, 4}));
        o = ord(ListCol::Console, false);       // n64 (Beta, Zelda), gc, gba
        CHECK((o == std::vector<int>{3, 2, 0, 5, 1, 4}));
        o = ord(ListCol::Players, true);        // 4 players first (by title), then 2, 1
        CHECK((o == std::vector<int>{3, 5, 0, 2, 1, 4}));
        o = ord(ListCol::Last, true);           // newest first, never played last
        CHECK((o == std::vector<int>{3, 2, 5, 0, 1, 4}));
        o = ord(ListCol::Last, false);          // oldest first, never played STILL last
        CHECK((o == std::vector<int>{3, 0, 5, 2, 1, 4}));
        o = ord(ListCol::Time, true);
        CHECK((o == std::vector<int>{3, 0, 2, 1, 5, 4}));
        o = ord(ListCol::None, false);
        CHECK((o == std::vector<int>{0, 1, 2, 3, 4, 5}));
        std::vector<int> sub = {5, 2};
        CHECK((sort_list_rows(rows, &sub, ListCol::Title, false) == std::vector<int>{2, 5}));
    }
    {
        ListCol c; bool d;
        col_click(ListCol::None, false, ListCol::Last, &c, &d);
        CHECK(c == ListCol::Last && d);
        col_click(ListCol::Last, true, ListCol::Last, &c, &d);
        CHECK(c == ListCol::Last && !d);
        col_click(ListCol::Last, false, ListCol::Title, &c, &d);
        CHECK(c == ListCol::Title && !d);
        CHECK(col_from_key(col_key(ListCol::Time)) == ListCol::Time && col_from_key("") == ListCol::None);
    }
    {
        ListCols wide = list_columns(900, 8), narrow = list_columns(400, 8), tiny = list_columns(200, 8);
        CHECK(wide.console && wide.players && wide.last && wide.time);
        CHECK(!narrow.time && narrow.console);
        CHECK(!tiny.console && !tiny.players && tiny.title_w > 0);
        CHECK(wide.title_x + wide.title_w <= wide.console_x && wide.last_x + wide.last_w <= wide.time_x + 0.01f);
    }
    {   // virtual rows: two sections (5 and 3 items), 4 columns, headers
        VLayout l = build_vlayout({5, 3}, {true, true}, 4, 100.f, 30.f, 10.f, 20.f);
        CHECK(l.rows.size() == 5);   // head, 2 rows, head, 1 row
        CHECK(l.item_row.size() == 8 && l.rows[l.item_row[0]].kind == 1 && l.rows[l.item_row[5]].first == 5);
        int f, la;
        rows_in_range(l, 0, 10, &f, &la);
        CHECK(f == 0 && la == 0);
        rows_in_range(l, 1e6f, 2e6f, &f, &la);
        CHECK(f == -1);
        rows_in_range(l, 0, l.total_h, &f, &la);
        CHECK(f == 0 && la == (int)l.rows.size() - 1);
        CHECK(scroll_to_row(l, 1, 0, 1000, 0, 500) == 0);
        CHECK(scroll_to_row(l, (int)l.rows.size() - 1, 0, 100, 5, 500) > 0);
    }
    {   // settings round trip + defaults for an old file
        Settings s;
        CHECK(s.lib_layout == "cases" && s.lib_cases_default && s.lib_list_col.empty() && !s.lib_list_desc);
        s.lib_layout = "showcase"; s.lib_list_col = "time"; s.lib_list_desc = true;
        Settings m;
        CHECK(settings_from_json(settings_to_json(s), &m));
        CHECK(m.lib_layout == "showcase" && m.lib_list_col == "time" && m.lib_list_desc);
        Settings o;
        CHECK(settings_from_json("{\"name\": \"x\"}", &o) && o.lib_layout == "cases" && o.lib_cases_default);
        Settings g;   // old file: grid, no marker -> cases once; marker present + grid -> grid stays
        CHECK(settings_from_json("{\"lib_layout\": \"grid\"}", &g) && g.lib_layout == "cases" && g.lib_cases_default);
        Settings g2;
        CHECK(settings_from_json("{\"lib_layout\": \"list\"}", &g2) && g2.lib_layout == "list");
        Settings k;
        CHECK(settings_from_json("{\"lib_layout\": \"grid\", \"lib_cases_default\": true}", &k) && k.lib_layout == "grid");
        Settings k2;
        CHECK(settings_from_json(settings_to_json(g), &k2) && k2.lib_layout == "cases");
        g.lib_layout = "grid";
        CHECK(settings_from_json(settings_to_json(g), &k2) && k2.lib_layout == "grid");   // a later Grid pick survives a save/load
    }
    if (fails) { std::printf("%d FAILED\n", fails); return 1; }
    std::printf("hub_libview_test: all ok\n");
    return 0;
}
