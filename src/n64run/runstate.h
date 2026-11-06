#pragma once
/* Process-wide state shared between the frontend (main) and the in-exe input plugin. */
#include <memory>
#include <string>

#include "lockstep.h"

namespace n64run {

struct RunState {
    int owner[kPorts] = {0, -1, -1, -1};   // port -> owning slot, -1 = not owned
    uint8_t reserved = 0;                  // drop-in room: ports (bit = port) plugged in from boot that nobody owns yet (lockstep reads them neutral)
    bool rumble = false;                   // attach a Rumble Pak instead (offline, per-game option); wins over mempak
    void (*rumble_cb)(int port, bool on) = nullptr;   // set by the frontend: drive the pad
    bool mempak = false;                   // attach a Controller Pak to present ports (mempak_enabled)
    bool tpak = false;                     // Transfer Pak on controller 1 (port 0) instead of the Controller / Rumble Pak (offline, tpak_enabled)
    std::string tpak_rom, tpak_save;       // the Game Boy cartridge and its save file for that pak
    std::unique_ptr<Lockstep> lockstep;    // set by the frontend before EXECUTE
};

extern RunState g_run;

}  // namespace n64run
