#include "online_caps.h"

#include <cstdio>
#include <string>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

using hub::System;

int main() {
    struct Row { System s; bool branch; bool gated; };   // branch: launch_online has a branch; gated: needs the catalog online flag
    const Row table[] = {
        {System::Unknown, false, false}, {System::N64, true, false}, {System::GBA, true, false}, {System::GB, true, false},
        {System::GBC, true, false},      {System::GC, true, true},   {System::N3DS, true, false}, {System::WiiU, false, false},
        {System::NDS, true, true},       {System::PS2, false, false}, {System::PS1, true, true},
        {System::NES, true, false},      {System::SNES, true, false},
    };
    CHECK((int)System::SNES + 1 == (int)(sizeof table / sizeof table[0]));   // a new System enum value must get a row here
    hub::GameInfo on = hub::GameInfo(), off = hub::GameInfo();
    on.online = true; off.online = false;
    for (const Row& r : table) {
        CHECK(hubcaps::system_has_online_launch(r.s) == r.branch);
        CHECK(hubcaps::online_needs_catalog_flag(r.s) == r.gated);
        CHECK(hubcaps::online_capable(r.s, &on) == r.branch);       // every system with a launch branch accepts any game
        CHECK(hubcaps::online_capable(r.s, &off) == r.branch);
        CHECK(hubcaps::online_capable(r.s, nullptr) == r.branch);
        CHECK(!hubcaps::online_untested(r.s, &on));                 // vetted
        CHECK(hubcaps::online_untested(r.s, &off) == (r.branch && r.gated));   // gated systems: unvetted = untested
        CHECK(hubcaps::online_untested(r.s, nullptr) == (r.branch && r.gated));
        CHECK((hubcaps::online_unsupported_reason(r.s)[0] == 0) == r.branch);   // only systems with no branch carry a reason
        CHECK(hubcaps::default_online_max(r.s) >= 2 && hubcaps::default_online_max(r.s) <= 4);
    }
    CHECK(std::string(hubcaps::online_unsupported_reason(System::PS2)) == "PS2 online isn't supported yet");
    CHECK(hubcaps::default_online_max(System::PS1) == 2 && hubcaps::default_online_max(System::GC) == 4 && hubcaps::default_online_max(System::GB) == 2);
    CHECK(hubcaps::default_online_max(System::NES) == 2 && hubcaps::default_online_max(System::SNES) == 2);
    /* Every online catalog game belongs to a system that can launch online, and a PS1 game's cap comes from the catalog. */
    int ps1_online = 0;
    for (const hub::GameInfo& g : hub::catalog()) {
        if (!g.online || g.engine == hub::Engine::Native) continue;
        System s = hub::system_from_short(g.system.c_str());
        CHECK(hubcaps::online_capable(s, &g));
        if (s == System::PS1) { ++ps1_online; CHECK(hub::online_max_for(g) >= 2 && hub::online_max_for(g) <= 4); }
    }
    CHECK(ps1_online >= 6);
    const hub::GameInfo* cb = hub::find_game("crashbash");
    CHECK(cb && hubcaps::online_capable(System::PS1, cb) && hub::online_max_for(*cb) == 4);
    const hub::GameInfo* sp = hub::find_game("spyro");
    CHECK(sp && hubcaps::online_capable(System::PS1, sp) && hubcaps::online_untested(System::PS1, sp));   // single-player: offered, tagged untested
    CHECK(hubcaps::online_capable(System::PS1, nullptr) && hubcaps::online_untested(System::PS1, nullptr));   // unknown disc: offered, untested

    std::printf(fails ? "FAILED (%d)\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
