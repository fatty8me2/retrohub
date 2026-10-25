// Console test: changelog.json parsing, shipped-file sanity and the show-once-per-version popup rule.
#include "changelog.h"

#include <cstdio>
#include <fstream>
#include <sstream>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main(int argc, char** argv) {
    using namespace hubui::changelog;
    std::vector<Entry> v;
    CHECK(!parse("", &v) && v.empty());
    CHECK(!parse("{\"versions\":5}", &v));
    CHECK(!parse("[1,2]", &v));
    const char* j = "{\"versions\":[{\"version\":\"v1.2.0\",\"date\":\"2026-03-09\",\"title\":\"T\",\"new\":[\"a\",\"\",3,\"b\"],\"fixed\":[\"c\"]},"
                    "{\"version\":\"1.1.0\"},{\"title\":\"no version\",\"new\":[\"x\"]},{\"version\":\"1.0.0\",\"improved\":[\"d\"]}]}";
    CHECK(parse(j, &v) && v.size() == 2);   // entries without a version or bullets are dropped
    CHECK(v[0].version == "1.2.0" && v[0].items[New].size() == 2 && v[0].items[Fixed].size() == 1 && v[0].total() == 3);
    CHECK(v[1].version == "1.0.0" && v[1].items[Improved].size() == 1);
    CHECK(find(v, "v1.2.0") == &v[0] && find(v, "1.0.0") == &v[1] && !find(v, "1.1.0"));
    CHECK(pretty_date("2026-03-09") == "Mar 9, 2026" && pretty_date("x") == "x" && pretty_date("2026-13-01") == "2026-13-01");
    CHECK(to_markdown(v[0]) == "## T\n\n**New**\n- a\n- b\n\n**Fixed**\n- c\n");

    // show once per version
    CHECK(popup_action("", "v1.22.0", true, false, false) == Popup::Show);        // upgrade from a build that never recorded a version
    CHECK(popup_action("1.21.0", "v1.22.0", true, false, false) == Popup::Show);
    CHECK(popup_action("1.22.0", "v1.22.0", true, false, false) == Popup::None);   // already shown
    CHECK(popup_action("v1.22.0", "1.22.0", true, false, false) == Popup::None);
    CHECK(popup_action("1.23.0", "1.22.0", true, false, false) == Popup::None);    // downgrade
    CHECK(popup_action("", "1.22.0", true, true, false) == Popup::MarkSeen);       // fresh install: no popup, remember
    CHECK(popup_action("", "1.22.0", true, true, true) == Popup::MarkSeen);
    CHECK(popup_action("1.21.0", "1.22.0", true, false, true) == Popup::None);     // wizard open: wait
    CHECK(popup_action("1.21.0", "1.22.0", false, false, false) == Popup::MarkSeen);   // no notes for this build
    CHECK(popup_action("", "dev", true, false, false) == Popup::None);

    if (argc > 1) {   // the shipped file
        std::ifstream f(argv[1], std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        std::vector<Entry> all;
        CHECK(parse(ss.str(), &all));
        CHECK(all.size() >= 5);
        CHECK(!all.empty() && !all[0].version.empty());   // newest entry; not pinned to a release number
        for (size_t i = 0; i < all.size(); ++i) {
            CHECK(!all[i].date.empty() && !all[i].title.empty());
            if (i) CHECK(all[i].date <= all[i - 1].date);   // newest first
        }
    }
    if (fails) { std::printf("%d failure(s)\n", fails); return 1; }
    std::printf("hub_changelog_test OK\n");
    return 0;
}
