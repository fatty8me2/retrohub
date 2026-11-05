/* Console test for palpark_logic.h: which DS games get the row, choice ordering, save-file matching, args, stored choice. */
#include <cstdio>
#include <string>

#include "palpark_logic.h"

using namespace hubui::palpark;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

int main() {
    for (const char* id : {"pokediamond", "pokepearl", "pokeplatinum", "pokeheartgold", "pokesoulsilver"}) CHECK(ds_uses_slot2(id));
    CHECK(!ds_uses_slot2("pokeblack"));
    CHECK(!ds_uses_slot2(""));
    CHECK(!ds_uses_slot2("mkds"));

    CHECK(gen3_rank("pokeruby") == 0 && gen3_rank("pokeleafgreen") == 4 && gen3_rank("aria") == -1 && gen3_rank("") == -1);

    std::vector<Gba> lib = {
        {"gba:a1", "Metroid Fusion", "mfusion", "C:\\r\\mf.gba", ""},
        {"gba:b1", "Pokemon LeafGreen", "pokeleafgreen", "C:\\r\\lg.gba", ""},
        {"gba:c1", "Advance Wars", "", "C:\\r\\aw.gba", ""},
        {"gba:d1", "Pokemon Ruby", "pokeruby", "C:\\r\\ruby.gba", ""},
        {"gba:e1", "no path", "", "", ""},
        {"gba:f1", "Pokemon Emerald", "pokeemerald", "C:\\r\\em.zip", "Pokemon Emerald (USA)"},
    };
    auto o = order_choices(lib);
    CHECK(o.size() == 5);
    CHECK(lib[o[0]].catalog_id == "pokeruby" && lib[o[1]].catalog_id == "pokeemerald" && lib[o[2]].catalog_id == "pokeleafgreen");   // Gen 3 first, release order
    CHECK(lib[o[3]].title == "Advance Wars" && lib[o[4]].title == "Metroid Fusion");                                                     // then A-Z
    CHECK(order_choices({}).empty());
    CHECK(find_gba(lib, "gba:d1") && find_gba(lib, "gba:d1")->title == "Pokemon Ruby");
    CHECK(!find_gba(lib, "gba:zz") && !find_gba(lib, ""));

    CHECK(file_stem("C:\\r\\Pokemon Ruby (USA).gba") == "Pokemon Ruby (USA)");
    CHECK(file_stem("/x/y.z/rom") == "rom");
    CHECK(srm_stem(lib[5]) == "Pokemon Emerald (USA)");      // zipped: the inner name
    CHECK(srm_stem(lib[3]) == "ruby");

    CHECK(pick_save_file({"ruby.srm", "other.srm"}, "ruby") == "ruby.srm");
    CHECK(pick_save_file({"RUBY.SRM"}, "ruby") == "RUBY.SRM");
    CHECK(pick_save_file({"renamed.srm"}, "ruby") == "renamed.srm");              // lone save under another name: this game's
    CHECK(pick_save_file({"a.srm", "b.srm"}, "ruby").empty());                    // ambiguous: never guess
    CHECK(pick_save_file({"ruby.rtc", "x.srm.bak"}, "ruby").empty());
    CHECK(pick_save_file({}, "ruby").empty());

    CHECK(build_args("C:\\r\\ruby.gba", "C:\\v\\ruby.srm") == " --slot2-rom \"C:\\r\\ruby.gba\" --slot2-save \"C:\\v\\ruby.srm\"");
    CHECK(build_args("C:\\r\\ruby.gba", "") == " --slot2-rom \"C:\\r\\ruby.gba\"");
    CHECK(build_args("", "C:\\v\\ruby.srm").empty());
    CHECK(build_args("C:\\r\\ru\"by.gba", "").empty());

    Choices c;
    CHECK(get_choice(c, "nds:x").empty());
    set_choice(&c, "nds:x", "gba:d1");
    set_choice(&c, "nds:y", "gba:b1");
    CHECK(get_choice(c, "nds:x") == "gba:d1");
    Choices r = parse_choices(write_choices(c));
    CHECK(r == c);
    set_choice(&r, "nds:x", "");
    CHECK(get_choice(r, "nds:x").empty() && r.size() == 1);
    CHECK(parse_choices("").empty() && parse_choices("not json").empty() && parse_choices("[]").empty());
    CHECK(parse_choices("{\"slot2\":{\"a\":5,\"b\":\"\",\"c\":\"k\"}}").size() == 1);
    CHECK(write_choices(Choices()) == "{\"slot2\":{}}");

    printf(g_fail ? "hub_palpark_test: %d FAILED\n" : "hub_palpark_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
