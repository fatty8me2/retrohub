// Console test: cheats list display rules (de-duplication, version suffix, enabled copy wins, code type shown only when mixed).
#include <cstdio>

#include "cheat_view_logic.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

using namespace hubcheatview;

static In mk(const char* d, std::vector<std::string> c, bool en = false, bool master = false, int fmt = 0) {
    In r; r.desc = d; r.codes = std::move(c); r.enabled = en; r.master = master; r.format = fmt; return r;
}

int main() {
    {   // the same name and the same codes: once
        auto o = dedupe({mk("Enable Code (Must Be On)", {"EE000000 0000", "FF000000 0000"}, false, true),
                         mk("Infinite Health", {"8033B21E 00FF"}),
                         mk("enable  code (must be on)", {"ee000000 0000", "FF000000   0000"}, false, true)});
        CHECK(o.size() == 2);
        CHECK(o[0].src == 0 && o[0].label == "Enable Code (Must Be On)" && o[0].master);
        CHECK(o[1].src == 1 && !o[1].master);
    }
    {   // the same name, different codes: both stay, the second says which
        auto o = dedupe({mk("Master Code", {"AAAA 0001"}), mk("Master Code", {"BBBB 0002"}), mk("Master Code", {"CCCC 0003"})});
        CHECK(o.size() == 3);
        CHECK(o[0].label == "Master Code");
        CHECK(o[1].label == "Master Code (version 2)");
        CHECK(o[2].label == "Master Code (version 3)");
    }
    {   // a switched-on copy is the one shown
        auto o = dedupe({mk("Moon Jump", {"1"}), mk("Moon Jump", {"1"}, true)});
        CHECK(o.size() == 1 && o[0].src == 1);
    }
    {   // a duplicate of a version keeps the numbering stable
        auto o = dedupe({mk("X", {"1"}), mk("X", {"2"}), mk("X", {"2"})});
        CHECK(o.size() == 2 && o[1].label == "X (version 2)");
    }
    CHECK(dedupe({}).empty());
    // code type: only when mixed
    CHECK(!show_code_type({mk("a", {"1"}, false, false, 1), mk("b", {"2"}, false, false, 1)}));
    CHECK(show_code_type({mk("a", {"1"}, false, false, 1), mk("b", {"2"}, false, false, 2)}));
    CHECK(!show_code_type({}));
    if (fails == 0) std::printf("cheat view: all passed\n");
    return fails ? 1 : 0;
}
