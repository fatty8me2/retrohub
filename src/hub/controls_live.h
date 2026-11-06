#pragma once
/* Live controller changes while a game runs (docs/RUNNER_CTL.md "Live controls"). Pure helpers; the hub glue is in page_controllers.cpp.
 *
 * Built-in runners (retrorun, n64run) re-read their --controls file when sent runctl::Cmd::ReloadControls, so a change on the Controllers
 * page applies on the next frame. The external emulators cannot: none of Dolphin, Cemu, PCSX2, Azahar or PPSSPP re-reads its controller
 * config while running, and the only ways to make them (their settings dialogs, hotkeys) need a window or keystrokes, which RetroHub never
 * sends into a game. They get a freshly generated config at every launch, so the change simply applies at the next start. */
#include <string>

#include "controls.h"
#include "library.h"

namespace hubctlive {

/* The runner needs a reload (and the file a rewrite) only when what it reads changed: the mapping or the pad port order. */
inline bool needs_reload(bool on_disk_valid, const hubctl::Profile& on_disk, const hubctl::Profile& next) {
    return !on_disk_valid || !on_disk.same_mapping(next) || on_disk.port_order != next.port_order;
}

enum class Mode { NoGame, Live, NextLaunch };
/* builtin_ctl: the running game is a built-in runner that was started with a controls file and a control channel. */
inline Mode mode_for(bool game_running, bool builtin_ctl) { return !game_running ? Mode::NoGame : builtin_ctl ? Mode::Live : Mode::NextLaunch; }

inline const char* emulator_name(hub::System s) {
    switch (s) {
        case hub::System::GC: return "Dolphin";
        case hub::System::WiiU: return "Cemu";
        case hub::System::N3DS: return "Azahar";
        case hub::System::PS2: return "PCSX2";
        case hub::System::PSP: return "PPSSPP";
        case hub::System::Switch: return "Lichii";
        default: return "";
    }
}

/* The note under the Controllers page header while a game runs ("" when no game runs). */
inline std::string note(Mode m, const std::string& game_title, const std::string& emulator) {
    switch (m) {
        case Mode::Live: return "Game running: control changes apply right away.";
        case Mode::NextLaunch:
            return "Applies the next time you start " + game_title + " (" + (emulator.empty() ? std::string("this game") : emulator) + " can't change controls mid-game).";
        default: return "";
    }
}

}  // namespace hubctlive
