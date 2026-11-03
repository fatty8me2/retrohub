#pragma once
/* Which controller device each libretro port needs, and which core option switches on the 4-pad adapter, per system. Pure (no SDL, no core): every PC of an
 * online room derives the same plan from the same --ports string, and the rules are unit tested (test_rollback.cpp).
 *
 * Why this exists: a libretro core only sees a pad on a port when the frontend plugged one in (retro_set_controller_port_device) or the core defaults to one;
 * "input_state was called for that port" proves nothing. Found by the players audit (docs/RETRORUN.md, "Players per system"):
 *   NES    Nestopia's ports 2-3 default to "Auto" and stay unplugged even with the Four Score on: pads 3-4 never reached the game.
 *   SNES   snes9x: the multitap is a device on port 2 (libretro port 1); ports 2-3 are then its pads 3-4.
 *   PS1    Beetle: DualShock on every port; ports 2-3 only exist with beetle_psx_enable_multitap_port1.
 *   PS2    LRPS2: only port 0 is plugged by default; the multitap is the pcsx2_multitap option (console port 1).
 *   GB/GBC/GBA/NDS: one pad (link cable / wireless are separate cores/instances, not extra pads).
 */
#include <string>
#include <utility>
#include <vector>

namespace retrorun {

enum class Sys { Other, NES, SNES, PS1, PS2, GB, GBA, NDS };

inline Sys sys_of(const std::string& lowered_system) {
    if (lowered_system == "nes") return Sys::NES;
    if (lowered_system == "snes") return Sys::SNES;
    if (lowered_system == "ps1") return Sys::PS1;
    if (lowered_system == "ps2") return Sys::PS2;
    if (lowered_system == "gb" || lowered_system == "gbc") return Sys::GB;
    if (lowered_system == "gba") return Sys::GBA;
    if (lowered_system == "nds") return Sys::NDS;
    return Sys::Other;
}

constexpr unsigned kDevJoypad = 1;            // RETRO_DEVICE_JOYPAD
constexpr unsigned kDevNesGamepad = 0x101;    // Nestopia "Gamepad" = RETRO_DEVICE_SUBCLASS(JOYPAD, 0)
constexpr unsigned kDevSnesMultitap = 0x101;  // snes9x "Multitap" on port 2 (libretro port 1)
constexpr unsigned kDevPsDualShock = 0x205;   // RETRO_DEVICE_SUBCLASS(ANALOG, 1): Beetle PSX DualShock

struct PortPlanIn {
    Sys sys = Sys::Other;
    int owner[4] = {0, -1, -1, -1};   // --ports: lobby slot owning each libretro port, -1 = nobody
    int players = 1;                  // PCs in the room (PS2: also the offline player count)
    bool multiport = false;           // --multiport: boot with the 4-pad adapter even if only 2 ports are used (drop-in)
    bool want_tap = false;            // --multitap
    bool no_tap = false;              // --no-multitap
};

struct PortPlanOut {
    bool tap = false;                                           // NES Four Score / SNES or PS1 multitap / PS2 multitap is on
    std::vector<std::pair<unsigned, unsigned>> devices;         // retro_set_controller_port_device(port, device), applied after the game loaded
    std::vector<std::pair<std::string, std::string>> options;   // core option pins (before the core loads; the user's --opt/ini applies on top)
};

inline bool port_used(const PortPlanIn& in, int port) { return in.owner[port] >= 0; }

/* The highest used port + 1 (0 when none). */
inline int ports_in_use(const PortPlanIn& in) {
    int n = 0;
    for (int i = 0; i < 4; i++) if (in.owner[i] >= 0) n = i + 1;
    return n;
}

inline PortPlanOut plan_ports(const PortPlanIn& in) {
    PortPlanOut o;
    const bool upper = in.owner[2] >= 0 || in.owner[3] >= 0;   // players 3-4 exist
    switch (in.sys) {
        case Sys::NES:
            o.tap = !in.no_tap && (in.multiport || upper);
            if (o.tap) {
                o.options.push_back({"nestopia_select_adapter", "ntsc"});          // the NES Four Score
                o.devices.push_back({2, kDevNesGamepad});                           // "Auto" leaves pads 3-4 unplugged
                o.devices.push_back({3, kDevNesGamepad});
            }
            break;
        case Sys::SNES:
            o.tap = !in.no_tap && (in.want_tap || in.multiport || upper);
            if (o.tap) o.devices.push_back({1, kDevSnesMultitap});                  // multitap in controller port 2: libretro ports 2-3 = its pads 3-4
            break;
        case Sys::PS1:
            o.tap = !in.no_tap && (in.multiport || upper);
            for (unsigned p = 0; p < 4; p++) o.devices.push_back({p, kDevPsDualShock});
            if (o.tap) o.options.push_back({"beetle_psx_enable_multitap_port1", "enabled"});   // tap in console port 1: libretro ports 0-3 are its four pads
            break;
        case Sys::PS2: {
            o.tap = in.players >= 3 || upper;                                       // the multitap is in console port 1: frontend ports 0-3 are its slots
            const unsigned np = o.tap ? 4u : 2u;                                    // 1-2 players: console ports 1-2
            for (unsigned p = 0; p < np; p++) o.devices.push_back({p, kDevJoypad});
            o.options.push_back({"pcsx2_multitap", o.tap ? "port 1" : "disabled"});
            break;
        }
        default: break;
    }
    return o;
}

/* Offline (no --net, no --ports): how many local ports to drive from the number of connected controllers. At least 1; the keyboard shares port 0 with
 * the first pad. Capped by what the core can take: 4 pads on NES (Four Score), SNES (multitap) and PS1 (multitap), 2 without the adapter, 1 elsewhere. */
inline int offline_players(Sys sys, int connected_pads, bool no_tap) {
    int cap = 1;
    switch (sys) {
        case Sys::NES: case Sys::SNES: case Sys::PS1: cap = no_tap ? 2 : 4; break;
        case Sys::PS2: cap = 4; break;
        default: break;
    }
    if (connected_pads < 1) connected_pads = 1;
    return connected_pads > cap ? cap : connected_pads;
}

}  // namespace retrorun
