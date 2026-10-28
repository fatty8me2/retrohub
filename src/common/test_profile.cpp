#include <cstdio>
#include "profile.h"
using namespace hub::profile;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
int main() {
    CHECK(valid_avatar("c:snes") && !valid_avatar("c:bogus") && valid_avatar("g:n64:smash") && !valid_avatar("g:a b") && !valid_avatar("x:y") && !valid_avatar(""));
    CHECK(clean_status("  grinding   Melee \n") == "grinding Melee");
    CHECK(clean_status(std::string(100, 'x')).size() == kStatusMax);
    CHECK(clean_status("a\tb") == "a b");
    CHECK(valid_reaction("fire") && !valid_reaction("<b>") && std::string(reaction_label("gg")) == "GG");
    Tally t;
    CHECK(t.set("a", "fire", true) && !t.set("a", "fire", true) && t.count("fire") == 1 && t.mine("a", "fire"));
    CHECK(t.set("a", "gg", true) && t.count("fire") == 0 && t.count("gg") == 1);   // one reaction per person
    CHECK(t.set("b", "gg", true) && t.count("gg") == 2);
    CHECK(!t.set("a", "fire", false) && t.set("a", "gg", false) && t.count("gg") == 1);
    CHECK(!t.set("a", "nope", true));
    std::printf(fails ? "FAILED\n" : "profile_test ok\n");
    return fails ? 1 : 0;
}
