#pragma once
/* PlayStation Portable via PPSSPP (RetroHub's own portable download, Kind::Ppsspp in emulators.h): ppsspp.ini defaults and per-launch
 * values, controls.ini from the RetroHub "psp" profile (XInput pad), launch arguments and the in-game save/load state keys.
 * UI-free; see docs/EMULATORS.md (PSP). */

#include <string>
#include <vector>

#include "controls.h"
#include "emulators.h"
#include "game_settings.h"

namespace hubemu {

/* <install>\memstick\PSP\SYSTEM holds ppsspp.ini and controls.ini (PPSSPP is portable while no installed.txt sits beside the exe). */
std::string ppsspp_system_dir(const std::string& install_dir);

/* ppsspp.ini, pure text transforms. Defaults (once): no first-run / update prompts, no exit confirmation, no pause on focus loss. */
std::string ppsspp_ini_defaults(const std::string& ini);
/* Every launch: [Graphics] FullScreen and InternalResolution (psp.res). */
std::string ppsspp_ini_launch(const std::string& ini, const hub::gs::Values& v);

/* One profile term -> "<device>-<keycode>" for PPSSPP's controls.ini (XInput pad `slot` = device 20 + slot), "" when it has no equivalent.
 * Buttons use Android key codes (A 96, B 97, X 99, Y 100, L1 102, R1 103, Start 108, Select 109, D-pad 19..22); axes 4000 + 2 * axis + (negative ? 1 : 0). */
std::string ppsspp_binding(const hubctl::Term& t, int xinput_slot);
/* Whole controls.ini ([ControlMapping]): face buttons, shoulders, Start/Select, D-pad and the analog stick from the profile (XInput) plus PPSSPP's own
 * keyboard keys; every hotkey (pause, fast-forward, screenshot, rewind ...) is left empty, because PPSSPP erases its defaults for every name when the
 * section exists. Save State / Load State stay unmapped on purpose: PPSSPP then keeps its built-in F2 / F4 accelerators, which RetroHub sends. */
std::string ppsspp_controls_ini(const hubctl::Profile& profile, int xinput_slot);
/* The same mapping merged into an existing controls.ini: only the keys RetroHub manages (the PSP buttons and An.*) are updated in place;
 * every other key, section and comment of the player's file stays. An empty/missing file gives exactly ppsspp_controls_ini. */
std::string ppsspp_controls_merge(const std::string& existing, const hubctl::Profile& profile, int xinput_slot);
/* Windows virtual-key codes of the chord that saves (F2) / loads (F4) the quick-save state. */
std::vector<int> ppsspp_state_keys(bool save);

/* Writes ppsspp.ini and controls.ini. Returns "" or an error. */
std::string prepare_ppsspp(const std::string& install_dir, const hubctl::Profile& profile, int xinput_slot, const hub::gs::Values& v);
/* --fullscreen | --windowed, then "<game>" (the game path last). */
std::string ppsspp_args(const std::string& game_path, bool fullscreen);

}  // namespace hubemu
