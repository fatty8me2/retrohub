// Console test: every system has a full and a short on-screen name, and they agree with the canonical system table.
#include "console_names.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main() {
    using namespace hub;
    std::set<std::string> fulls, shorts;
    int n = 0;
    for (int i = (int)System::Unknown + 1; i <= (int)System::Switch; ++i) {
        const System s = (System)i;
        const std::string id = system_short(s);
        CHECK(!id.empty());
        CHECK(*full_name(s) != 0);
        CHECK(*short_name(s) != 0);
        CHECK(std::strcmp(full_name(s), system_name(s)) == 0);
        CHECK(std::strcmp(full_name_id(id), full_name(s)) == 0);
        CHECK(std::strcmp(short_name_id(id), short_name(s)) == 0);
        CHECK(std::strlen(short_name(s)) <= std::strlen(full_name(s)) + 4);
        CHECK(fulls.insert(full_name(s)).second);    // no two consoles share a name
        CHECK(shorts.insert(short_name(s)).second);
        ++n;
    }
    CHECK(n == 14);
    CHECK(std::strcmp(full_name(System::N64), "Nintendo 64") == 0);
    CHECK(std::strcmp(short_name(System::PS1), "PS1") == 0);
    CHECK(std::strcmp(full_name(System::PS1), "PlayStation") == 0);
    CHECK(std::strcmp(full_name(System::GBA), "Game Boy Advance") == 0);
    CHECK(std::strcmp(full_name(System::Switch, true), "Nintendo Switch 2") == 0);
    CHECK(std::strcmp(short_name(System::Switch, true), "Switch 2") == 0);
    CHECK(std::strcmp(full_name_id(std::string("switch2")), "Nintendo Switch 2") == 0);
    CHECK(std::strcmp(short_name_id(std::string("switch2")), "Switch 2") == 0);
    CHECK(*full_name(System::Unknown) == 0 && *short_name(System::Unknown) == 0);
    CHECK(*full_name_id(std::string("nope")) == 0);
    if (fails) { std::printf("%d failure(s)\n", fails); return 1; }
    std::printf("console names tests passed\n");
    return 0;
}
