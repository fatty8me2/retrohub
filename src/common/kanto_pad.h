#pragma once
/* Xbox / SDL game controller bindings for the Kanto Together mGBA (the package's own standalone Qt mGBA).
 *
 * mGBA (SDL2 builds) reads gamepad bindings from config.ini section [gba.input.SDLC] (SDLC = SDL GameController: the numbers are
 * SDL_GameControllerButton / SDL_GameControllerAxis indices, same order as hubctl button_index / axis_index):
 *     keyA=0  keyB=1  keySelect=4  keyStart=6  keyRight=14 keyLeft=13 keyUp=11 keyDown=12 keyR=10 keyL=9
 *     axisRightValue=16384 axisRightAxis=+0   (axis<Key>Value = dead zone, axis<Key>Axis = sign + axis index)
 * mGBA only binds its built-in pad defaults when config.ini has no keyboard section, so a config.ini that already holds the
 * keyboard bindings (the package ships one) leaves the pad completely unbound. RetroHub therefore writes the section itself.
 * The built-in "XInput Controller #n" profile (Nintendo-swapped joystick indices) is overridden with the same bindings by
 * [gba.input-profile.XInput Controller #n].
 *
 * Source of the bindings: RetroHub's "retro" controller profile. Its built-in default (libretro A on Xbox B) is replaced by the
 * plain Xbox layout A=A, B=B; any edit made on the Controllers page is carried over (first button term of each control, plus the
 * stick/trigger axis terms). Keyboard bindings are never touched. */

#include <string>
#include <utility>
#include <vector>

#include "controls.h"

namespace kanto {

/* key/value lines (without the section header) in write order: key*, then axis*Value / axis*Axis. */
std::vector<std::pair<std::string, std::string>> pad_bindings(const hubctl::Profile& retro);
/* Pure text transform: returns `ini` with the SDLC section and the XInput profile sections set to the bindings of `retro`.
 * Other keys and sections (the keyboard, tilt settings, the user's own gamepad profiles) are kept as they are. */
std::string mgba_pad_config_patch(const std::string& ini, const hubctl::Profile& retro);
/* True when `ini` already has the [gba.input.SDLC] section. */
bool has_pad_section(const std::string& ini);

/* Writes <install_dir>\runtime\mgba\config.ini when missing / the profile changed / the section is gone (package reinstalled).
 * The marker runtime\mgba\retrohub-pad.txt holds the profile hash, later edits made in mGBA survive until the profile changes.
 * Returns true when it (re)wrote the config. */
bool prepare_pad(const std::string& install_dir, const hubctl::Profile& retro);

}  // namespace kanto
