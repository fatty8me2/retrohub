#include "online_caps.h"
#include "logic.h"
#include "remote_play.h"

#include <cstdio>
#include <string>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

using hub::System;

int main() {
    struct Row { System s; bool branch; bool gated; };   // branch: launch_online has a branch; gated: needs the catalog online flag
    const Row table[] = {
        {System::Unknown, false, false}, {System::N64, true, false}, {System::GBA, true, false}, {System::GB, true, false},
        {System::GBC, true, false},      {System::GC, true, true},   {System::N3DS, true, false}, {System::WiiU, true, true},
        {System::NDS, true, true},       {System::PS2, true, true} , {System::PS1, true, true},
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
    CHECK(std::string(hubcaps::online_unsupported_reason(System::WiiU)) == "Wii U online isn't supported yet");
    CHECK(hubcaps::default_online_max(System::PS2) == 2);
    /* PS2: catalog games flagged online are capable and capped at 2 players (LRPS2 has no multitap); anything else is offered as "untested". */
    {
        const hub::GameInfo* t5 = hub::find_game("tekken5");
        CHECK(t5 && t5->online && hubcaps::online_capable(System::PS2, t5) && !hubcaps::online_untested(System::PS2, t5));
        const hub::GameInfo* cn = hub::find_game("crashnitro");
        CHECK(cn && cn->max_players == 4 && hub::online_max_for(*cn) == 2);
        const hub::GameInfo* kh = hub::find_game("kh");
        CHECK(kh && hubcaps::online_capable(System::PS2, kh) && hubcaps::online_untested(System::PS2, kh));
        CHECK(hubcaps::online_capable(System::PS2, nullptr) && hubcaps::online_untested(System::PS2, nullptr));
    }
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

    /* Wii U remote play: catalog couch co-op games are online, 1-4 pads, host-only ownership. */
    for (System s : {System::Unknown, System::N64, System::GC, System::PS2, System::WiiU, System::N3DS})
        CHECK(hubcaps::remote_play_system(s) == (s == System::WiiU));
    const hub::GameInfo* sm = hub::find_game("sm3dw");
    CHECK(sm && sm->online && sm->system == "wiiu" && hub::online_max_for(*sm) == 4);
    CHECK(sm && hubcaps::online_capable(System::WiiU, sm) && !hubcaps::online_untested(System::WiiU, sm) && hubcaps::remote_play_game(System::WiiU, sm));
    CHECK(hubcaps::online_untested(System::WiiU, nullptr) && hubcaps::default_online_max(System::WiiU) == 2);
    for (const char* id : {"mk8", "nsmbu", "captaintoad", "smash4wiiu", "nland", "pikmin3"}) {
        const hub::GameInfo* g = hub::find_game(id);
        CHECK(g && g->online && g->system == "wiiu" && hub::online_max_for(*g) >= 2);
    }
    {   /* room logic: guests without the game may still be selected for a remote-play game */
        hubui::PeerInfo h{0, "Host", true, {}, {"wiiu:sm3dw"}}, g1{1, "Ann", true, {}, {}}, g2{2, "Bo", true, {}, {}};
        CHECK(!hubui::game_selectable("wiiu:sm3dw", 4, {h, g1, g2}, 0).ok);
        CHECK(hubui::game_selectable("wiiu:sm3dw", 4, {h, g1, g2}, 0, true).ok);
        hubui::PeerInfo nh{0, "Host", true, {}, {}};
        CHECK(!hubui::game_selectable("wiiu:sm3dw", 4, {nh, g1}, 0, true).ok);   // the host must own it
        CHECK(!hubui::game_selectable("wiiu:sm3dw", 2, {h, g1, g2}, 0, true).ok);   // cap still applies
    }

    /* remote_play.h: args per docs/REMOTE_PLAY.md, exit texts, attach timing, launch fragment */
    CHECK(hubrp::room_name("ABCD", 3) == "ABCD-3");
    CHECK(hubrp::guest_count(1) == 0 && hubrp::guest_count(2) == 1 && hubrp::guest_count(4) == 3 && hubrp::guest_count(9) == 3);
    CHECK(hubrp::host_args(1234, "wss://x", "ABCD-3", 3, false) == "--host --capture-hwnd 1234 --net \"wss://x\" --room ABCD-3 --slot 0 --guests 2");
    CHECK(hubrp::host_args(1, "wss://x", "R-1", 2, true).find(" --upnp") != std::string::npos);
    CHECK(hubrp::guest_args("wss://x", "ABCD-3", 2, true) == "--guest --net \"wss://x\" --room ABCD-3 --slot 2 --fullscreen");
    CHECK(hubrp::exit_text(3, false) == hubrp::kNoDirectText && hubrp::exit_text(3, true) == hubrp::kNoDirectText);
    CHECK(hubrp::exit_text(4, false).find("host's game ended") != std::string::npos && hubrp::exit_text(5, true).find("capture") != std::string::npos);
    CHECK(hubrp::attach_decision(true, false, 1000) == hubrp::Attach::Wait && hubrp::attach_decision(true, true, 1000) == hubrp::Attach::Now);
    CHECK(hubrp::attach_decision(false, true, 0) == hubrp::Attach::GiveUp && hubrp::attach_decision(true, false, 91000) == hubrp::Attach::GiveUp);
    CHECK(hubrp::launch_is_remote("{\"kind\":\"launch\"" + hubrp::launch_fragment(3) + "}") && !hubrp::launch_is_remote("{\"kind\":\"launch\"}"));
    CHECK(hubrp::host_left_text("Ann") == "Ann left - the stream ended." && hubrp::host_left_text("").find("The host") == 0);

    std::printf(fails ? "FAILED (%d)\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
