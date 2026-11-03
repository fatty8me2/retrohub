/* Console test for port_plan.h: which libretro device each port needs per system, when the 4-pad adapter is on, and how many local ports an offline run drives. */
#include <cstdio>
#include <string>

#include "port_plan.h"

using namespace retrorun;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static PortPlanIn in(Sys s, int o0, int o1, int o2, int o3, int players) {
    PortPlanIn p;
    p.sys = s;
    p.owner[0] = o0; p.owner[1] = o1; p.owner[2] = o2; p.owner[3] = o3;
    p.players = players;
    return p;
}
static bool has_dev(const PortPlanOut& o, unsigned port, unsigned dev) {
    for (auto& d : o.devices) if (d.first == port && d.second == dev) return true;
    return false;
}
static bool has_dev_port(const PortPlanOut& o, unsigned port) {
    for (auto& d : o.devices) if (d.first == port) return true;
    return false;
}
static std::string opt(const PortPlanOut& o, const std::string& k) {
    for (auto& v : o.options) if (v.first == k) return v.second;
    return "";
}

int main() {
    CHECK(sys_of("nes") == Sys::NES && sys_of("snes") == Sys::SNES && sys_of("ps1") == Sys::PS1 && sys_of("ps2") == Sys::PS2);
    CHECK(sys_of("gb") == Sys::GB && sys_of("gbc") == Sys::GB && sys_of("gba") == Sys::GBA && sys_of("nds") == Sys::NDS && sys_of("") == Sys::Other);

    /* ---- NES: Four Score from the third port; pads 3-4 must be plugged (Nestopia leaves them on "Auto" = nobody) ---- */
    {
        PortPlanOut o = plan_ports(in(Sys::NES, 0, 1, -1, -1, 2));
        CHECK(!o.tap && o.devices.empty() && o.options.empty());   // 2 players: nothing changes, zapper/arkanoid auto-detect on port 2 stays
        o = plan_ports(in(Sys::NES, 0, 1, 2, -1, 3));
        CHECK(o.tap && opt(o, "nestopia_select_adapter") == "ntsc");
        CHECK(has_dev(o, 2, kDevNesGamepad) && has_dev(o, 3, kDevNesGamepad) && !has_dev_port(o, 0) && !has_dev_port(o, 1));
        o = plan_ports(in(Sys::NES, -1, -1, 2, -1, 1));   // a room where only port 2 is taken still needs the adapter
        CHECK(o.tap);
        PortPlanIn d = in(Sys::NES, 0, 1, -1, -1, 2);
        d.multiport = true;   // drop-in room: the adapter must be on from boot
        CHECK(plan_ports(d).tap && has_dev(plan_ports(d), 3, kDevNesGamepad));
        d.no_tap = true;
        CHECK(!plan_ports(d).tap);
    }
    /* ---- SNES: multitap is a device on controller port 2 (libretro port 1) ---- */
    {
        PortPlanOut o = plan_ports(in(Sys::SNES, 0, 1, -1, -1, 2));
        CHECK(!o.tap && o.devices.empty());
        o = plan_ports(in(Sys::SNES, 0, 1, 2, -1, 3));
        CHECK(o.tap && o.devices.size() == 1 && has_dev(o, 1, kDevSnesMultitap));
        o = plan_ports(in(Sys::SNES, 0, 1, 2, 3, 4));
        CHECK(o.tap && has_dev(o, 1, kDevSnesMultitap));
        PortPlanIn f = in(Sys::SNES, 0, 1, -1, -1, 2);
        f.want_tap = true;
        CHECK(plan_ports(f).tap);
        f = in(Sys::SNES, 0, 1, 2, 3, 4);
        f.no_tap = true;
        CHECK(!plan_ports(f).tap && plan_ports(f).devices.empty());
    }
    /* ---- PS1: a DualShock everywhere, the multitap option from the third port ---- */
    {
        PortPlanOut o = plan_ports(in(Sys::PS1, 0, 1, -1, -1, 2));
        CHECK(!o.tap && opt(o, "beetle_psx_enable_multitap_port1").empty());
        for (unsigned p = 0; p < 4; p++) CHECK(has_dev(o, p, kDevPsDualShock));
        o = plan_ports(in(Sys::PS1, 0, 0, 1, 1, 2));   // two PCs with two pads each: 4 ports, so the multitap
        CHECK(o.tap && opt(o, "beetle_psx_enable_multitap_port1") == "enabled");
        o = plan_ports(in(Sys::PS1, -1, -1, -1, 3, 1));
        CHECK(o.tap);
        PortPlanIn f = in(Sys::PS1, 0, 1, 2, 3, 4);
        f.no_tap = true;
        CHECK(!plan_ports(f).tap && opt(plan_ports(f), "beetle_psx_enable_multitap_port1").empty());
    }
    /* ---- PS2: pads on both console ports, the four multitap slots with 3-4 players (or any pad on port 3/4) ---- */
    {
        PortPlanOut o = plan_ports(in(Sys::PS2, 0, 1, -1, -1, 2));
        CHECK(!o.tap && o.devices.size() == 2 && has_dev(o, 0, kDevJoypad) && has_dev(o, 1, kDevJoypad) && opt(o, "pcsx2_multitap") == "disabled");
        o = plan_ports(in(Sys::PS2, 0, -1, -1, -1, 1));
        CHECK(!o.tap && o.devices.size() == 2);   // player 2 may join the room later: console port 2 is plugged
        o = plan_ports(in(Sys::PS2, 0, 1, 2, -1, 3));
        CHECK(o.tap && o.devices.size() == 4 && opt(o, "pcsx2_multitap") == "port 1");
        for (unsigned p = 0; p < 4; p++) CHECK(has_dev(o, p, kDevJoypad));
        o = plan_ports(in(Sys::PS2, 0, 0, 1, -1, 2));   // 2 PCs but 3 pads: the third pad needs the tap too
        CHECK(o.tap && o.devices.size() == 4);
        CHECK(plan_ports(in(Sys::PS2, 0, 1, 2, 3, 4)).tap);
    }
    /* ---- one-pad systems: nothing to plug ---- */
    for (Sys s : {Sys::GB, Sys::GBA, Sys::NDS, Sys::Other}) {
        PortPlanOut o = plan_ports(in(s, 0, 1, 2, 3, 4));
        CHECK(!o.tap && o.devices.empty() && o.options.empty());
    }
    CHECK(ports_in_use(in(Sys::NES, 0, 1, -1, -1, 2)) == 2 && ports_in_use(in(Sys::NES, -1, -1, -1, 3, 1)) == 4 && ports_in_use(in(Sys::NES, -1, -1, -1, -1, 0)) == 0);

    /* ---- offline: one local port per connected pad, capped by what the core takes ---- */
    CHECK(offline_players(Sys::SNES, 0, false) == 1 && offline_players(Sys::SNES, 1, false) == 1);
    CHECK(offline_players(Sys::SNES, 2, false) == 2 && offline_players(Sys::SNES, 3, false) == 3 && offline_players(Sys::SNES, 4, false) == 4);
    CHECK(offline_players(Sys::SNES, 8, false) == 4 && offline_players(Sys::NES, 6, false) == 4 && offline_players(Sys::PS1, 5, false) == 4);
    CHECK(offline_players(Sys::SNES, 4, true) == 2 && offline_players(Sys::PS1, 3, true) == 2);
    CHECK(offline_players(Sys::GBA, 3, false) == 1 && offline_players(Sys::GB, 2, false) == 1 && offline_players(Sys::NDS, 4, false) == 1);

    /* ---- the game's seat cap (--max-local): never more local players than seats, so no adapter for a 1-2 player game ---- */
    CHECK(offline_players(Sys::SNES, 4, false, 2) == 2 && offline_players(Sys::NES, 4, false, 1) == 1 && offline_players(Sys::PS1, 3, false, 2) == 2);
    CHECK(offline_players(Sys::SNES, 4, false, 4) == 4 && offline_players(Sys::SNES, 2, false, 4) == 2 && offline_players(Sys::NES, 3, false, 3) == 3);
    CHECK(offline_players(Sys::SNES, 4, false, 0) == 4);   // 0 = no cap given (a launcher that predates --max-local)
    {   // the adapter follows the players actually driven: 4 pads on a 2-seat game -> 2 players -> no adapter; on a 4-seat game -> adapter
        for (Sys s : {Sys::NES, Sys::SNES, Sys::PS1}) {
            int n = offline_players(s, 4, false, 2);
            PortPlanIn p; p.sys = s; p.players = n;
            for (int i = 0; i < 4; i++) p.owner[i] = i < n ? 0 : -1;
            CHECK(!plan_ports(p).tap);
            n = offline_players(s, 4, false, 4);
            for (int i = 0; i < 4; i++) p.owner[i] = i < n ? 0 : -1;
            CHECK(plan_ports(p).tap);
        }
    }
    /* ---- ghost pads: virtual / duplicate entries are not players ---- */
    {
        auto pad = [](const char* name, const char* guid, const char* serial, unsigned vid = 0, unsigned pid = 0) { PadDesc d; d.name = name; d.guid = guid; d.serial = serial; d.vendor = vid; d.product = pid; return d; };
        CHECK(count_real_pads({}) == 0);
        CHECK(count_real_pads({pad("Xbox 360 Controller", "g1", "")}) == 1);
        CHECK(count_real_pads({pad("Xbox 360 Controller", "g1", ""), pad("Xbox 360 Controller", "g1", "")}) == 2);   // two identical XInput pads (no serial) are two players
        CHECK(count_real_pads({pad("DualSense", "g2", "AB12"), pad("DualSense", "g2", "AB12")}) == 1);                // one device listed twice
        CHECK(count_real_pads({pad("DualSense", "g2", "AB12"), pad("DualSense", "g2", "CD34")}) == 2);                // two real DualSenses
        CHECK(count_real_pads({pad("Xbox 360 Controller", "g1", ""), pad("Steam Virtual Gamepad", "gs", "", 0x28DE, 0x11FF)}) == 1);
        CHECK(count_real_pads({pad("Xbox 360 Controller", "g1", ""), pad("Xbox 360 Controller", "gx", "", 0x28DE, 0x11FF)}) == 1);   // by VID/PID although it calls itself an Xbox pad
        CHECK(count_real_pads({pad("Steam Virtual Gamepad", "gs", "", 0x28DE, 0x11FF), pad("Steam Virtual Gamepad", "gs", "", 0x28DE, 0x11FF)}) == 2);   // Steam Input only: they ARE the pads
        PadDesc sv = pad("Some Pad", "g3", ""); sv.sdl_virtual = true;
        CHECK(count_real_pads({pad("Xbox 360 Controller", "g1", ""), sv}) == 1);
        CHECK(ci_contains("Steam VIRTUAL Gamepad", "virtual") && !ci_contains("Xbox", "virtual"));
        /* 1 real pad + 1 ghost on a 1-player game (cap 1): still one player and no adapter, whatever the pad list says */
        CHECK(offline_players(Sys::NES, count_real_pads({pad("A", "g", "S1"), pad("A", "g", "S1"), pad("B", "h", "", 0x28DE, 0x11FF)}), false, 1) == 1);
    }
    /* ---- the port plan stored with a save state ---- */
    {
        PortPlanOut two = plan_ports(in(Sys::SNES, 0, 1, -1, -1, 2)), four = plan_ports(in(Sys::SNES, 0, 1, 2, 3, 4));
        CHECK(plan_signature(two) != plan_signature(four) && plan_signature(two) == plan_signature(plan_ports(in(Sys::SNES, 0, -1, -1, -1, 1))));   // 1 or 2 players: same plan
        CHECK(plan_signature(plan_ports(in(Sys::SNES, 0, 1, 2, -1, 3))) == plan_signature(four));   // 3 or 4: same adapter
        CHECK(plan_signature(four).find("tap=1") != std::string::npos && plan_signature(two).find("tap=0") != std::string::npos);
        CHECK(plan_signature(plan_ports(in(Sys::NES, 0, 1, -1, -1, 2))) != plan_signature(plan_ports(in(Sys::NES, 0, 1, 2, -1, 3))));
        CHECK(plan_signature(plan_ports(in(Sys::GBA, 0, -1, -1, -1, 1))) == plan_signature(plan_ports(in(Sys::GBA, 0, 1, 2, 3, 4))));
    }

    if (g_fail) { printf("%d FAILED\n", g_fail); return 1; }
    printf("port_plan_test: all passed\n");
    return 0;
}
