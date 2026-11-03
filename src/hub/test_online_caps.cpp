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
    CHECK(std::string(hubcaps::online_unsupported_reason(System::WiiU)).empty() && hubcaps::remote_play_system(System::WiiU) && !hubcaps::remote_play_system(System::GC));   // Wii U = remote play now
    CHECK(hubcaps::default_online_max(System::PS2) == 4);
    /* PS2: catalog games flagged online are capable and up to 4 players (LRPS2 multitap, catalog online_max wins); anything else is offered as "untested". */
    {
        const hub::GameInfo* t5 = hub::find_game("tekken5");
        CHECK(t5 && t5->online && hubcaps::online_capable(System::PS2, t5) && !hubcaps::online_untested(System::PS2, t5));
        const hub::GameInfo* cn = hub::find_game("crashnitro");
        CHECK(cn && cn->max_players == 4 && hub::online_max_for(*cn) == 4);
        const hub::GameInfo* kh = hub::find_game("kh");
        CHECK(kh && hubcaps::online_capable(System::PS2, kh) && hubcaps::online_untested(System::PS2, kh));
        CHECK(hubcaps::online_capable(System::PS2, nullptr) && hubcaps::online_untested(System::PS2, nullptr));
    }
    CHECK(hubcaps::default_online_max(System::PS1) == 4 && hubcaps::default_online_max(System::GC) == 4 && hubcaps::default_online_max(System::GB) == 2);   // PS1: emulated multitap for 3-4 (tagged untested)
    CHECK(hubcaps::default_online_max(System::NDS) == 2 && hubcaps::default_online_max(System::GBC) == 2 && hubcaps::default_online_max(System::N64) == 4);
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

    /* Local seats for a game without a catalog entry: never below what they were (the old room caps), raised for PS1/Wii U; PSP is one pad. */
    for (System s : {System::GB, System::GBC, System::NDS}) CHECK(hubcaps::default_local_max(s) == 2);
    for (System s : {System::GBA, System::N3DS, System::N64, System::GC, System::WiiU, System::PS1, System::PS2}) CHECK(hubcaps::default_local_max(s) == 4);
    CHECK(hubcaps::default_local_max(System::PSP) == 1);
    CHECK(hubcaps::default_local_max(System::NES) == 2 && hubcaps::default_local_max(System::SNES) == 2);

    /* Caps against what each path can deliver. */
    for (const hub::GameInfo& g : hub::catalog()) {
        if (g.engine == hub::Engine::Native) continue;
        System s = hub::system_from_short(g.system.c_str());
        int cap = hub::online_max_for(g);
        CHECK(cap >= 1 && cap <= g.max_players && cap <= 4);
        if (g.online) CHECK(cap >= 2);                                       // "fun with friends online" needs two seats
        if (hub::link_cable_game(g.id)) CHECK(cap == 2);                     // one console per player
        if (s == System::GB || s == System::GBC) CHECK(cap <= 2);
        if (s == System::N3DS) CHECK(cap <= 2);                              // Azahar rooms are the 2-player trade/battle rooms here
        if (s == System::NES || s == System::SNES) {
            /* 3-4 seats need the Four Score / multitap: only these listed games are known to use them */
            static const char* const multi[] = {"nes_gauntlet2", "nes_nwc", "snes_som", "snes_sbomber", "snes_sbomber2", "snes_sbomber3", "snes_sbomber4", "snes_sbomber5", "snes_nbajam", "snes_nbajamte"};
            bool listed = false;
            for (const char* m : multi) if (g.id == m) listed = true;
            if (cap > 2) CHECK(listed);
        }
    }

    /* Buzz! needs USB buzzers a room does not have; the limit note says why a room is small. */
    {
        const hub::GameInfo* bz = hub::find_game("buzz_hollywood");
        CHECK(bz && !hubcaps::online_game_block(System::PS2, bz, bz->title).empty());
        CHECK(!hubcaps::online_game_block(System::PS2, nullptr, "Buzz! The Music Quiz").empty());
        CHECK(hubcaps::online_game_block(System::PS2, nullptr, "Gauntlet - Dark Legacy").empty() && hubcaps::online_game_block(System::GC, nullptr, "Buzz!").empty());
        const hub::GameInfo* t5 = hub::find_game("tekken5");
        CHECK(t5 && hubcaps::online_limit_note(System::PS2, t5, 2).find("at most 2") != std::string::npos);
        CHECK(hubcaps::online_limit_note(System::PS2, t5, 2, true).empty());   // only repeats "(max 2)": not added to that reason
        CHECK(!hubcaps::online_limit_note(System::NES, nullptr, 2, true).empty() && !hubcaps::online_limit_note(System::PS2, hub::find_game("kh"), 1, true).empty());
        CHECK(hubcaps::online_limit_note(System::PS2, hub::find_game("crashnitro"), 4).empty());
        CHECK(hubcaps::online_limit_note(System::PS1, nullptr, 4).empty());
        CHECK(hubcaps::online_limit_note(System::NES, nullptr, 2).find("Four Score") != std::string::npos);
        CHECK(hubcaps::online_limit_note(System::SNES, nullptr, 2).find("multitap") != std::string::npos);
        CHECK(hubcaps::online_limit_note(System::GBA, hub::find_game("pokeemerald"), 2).find("Link-cable") != std::string::npos);
        CHECK(hubcaps::online_limit_note(System::PS2, hub::find_game("kh"), 1).find("single-player") != std::string::npos);
    }

    /* Wii U remote play: catalog couch co-op games are online, 1-4 pads, host-only ownership. */
    for (System s : {System::Unknown, System::N64, System::GC, System::PS2, System::WiiU, System::N3DS})
        CHECK(hubcaps::remote_play_system(s) == (s == System::WiiU));
    const hub::GameInfo* sm = hub::find_game("sm3dw");
    CHECK(sm && sm->online && sm->system == "wiiu" && hub::online_max_for(*sm) == 4);
    CHECK(sm && hubcaps::online_capable(System::WiiU, sm) && !hubcaps::online_untested(System::WiiU, sm) && hubcaps::remote_play_game(System::WiiU, sm));
    CHECK(hubcaps::online_untested(System::WiiU, nullptr) && hubcaps::default_online_max(System::WiiU) == 4);   // host GamePad + 3 guests on DSU slots (hubrp::guest_count)
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
    {   // direct-connection pre-check + host status line
        CHECK(hubrp::probe_host_args("wss://x", "AB-101", 3, true, "r.json").find("--probe --host") == 0 && hubrp::probe_guest_args("wss://x", "AB-101", 2, 3, false).find("--slot 2") != std::string::npos);
        CHECK(hubrp::probe_msg_is(hubrp::probe_msg("AB-101", 3)) && !hubrp::probe_msg_is("{\"kind\":\"launch\"}"));
        auto m = hubrp::parse_probe_slots("{\"ok\":false,\"slots\":{\"1\":12,\"2\":-1}}");
        CHECK(m.size() == 2 && m[1] == 12 && m[2] == -1);
        std::map<int, std::string> names = {{0, "Host"}, {1, "Mickey"}, {2, "Minnie"}};
        std::string why = hubrp::probe_block_reason(m, names);
        CHECK(why.find("Minnie") != std::string::npos && why.find("Mickey") == std::string::npos && why.find("UPnP") != std::string::npos);
        CHECK(hubrp::probe_block_reason(hubrp::parse_probe_slots("{\"ok\":true,\"slots\":{\"1\":5,\"2\":9}}"), names).empty());
        CHECK(hubrp::probe_block_reason({}, names).find("Mickey and Minnie") != std::string::npos);   // no result file = nobody reachable
        std::string st = "{\"t\":1,\"kbps\":12000,\"guests\":[{\"slot\":1,\"rtt_ms\":48,\"loss_permille\":0}]}";
        CHECK(hubrp::status_text(st, names) == "streaming to Mickey · 12 Mbps · 48 ms");
        CHECK(hubrp::status_text("{\"kbps\":5,\"guests\":[]}", names).empty());
        CHECK(hubrp::host_args(0, "wss://x", "R-1", 2, false, "s.json", 0, "p.pid").find("--await-pid-file \"p.pid\"") != std::string::npos && hubrp::host_args(0, "wss://x", "R-1", 2, false, "s.json", 0, "p.pid").find("--capture-hwnd") == std::string::npos);   // streamrun first (DSU up before Cemu), Cemu's PID later
        CHECK(hubrp::host_args(7, "wss://x", "R-1", 2, false, "s.json", 26761).find("--status-file \"s.json\" --dsu-port 26761") != std::string::npos);
    }
    CHECK(hubrp::launch_is_remote("{\"kind\":\"launch\"" + hubrp::launch_fragment(3) + "}") && !hubrp::launch_is_remote("{\"kind\":\"launch\"}"));
    CHECK(hubrp::host_left_text("Ann") == "Ann left - the stream ended." && hubrp::host_left_text("").find("The host") == 0);

    std::printf(fails ? "FAILED (%d)\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
