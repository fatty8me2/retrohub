#pragma once
/* One rule for "may a game start now?" (pure logic, unit tested by hub_launchset_test).
 * A game must never start while ANY session is up: an offline game (A.proc), but also GameCube NetPlay, a 3DS room, Wii U remote play,
 * Kanto Together or an emulator job that still holds processes. Several of those never set A.proc, so checking A.proc alone let a
 * second Dolphin open on the same user folder and memory card. The caller passes session_active() (window_guard.cpp). */
#include <string>

namespace hubui {

inline bool launch_allowed(bool a_proc, bool any_session) { return !a_proc && !any_session; }

/* Short reason shown on a disabled Play / Play Online button and in the notice when a launch is refused. */
inline const char* launch_busy_reason() { return "A game is already running."; }

/* A button is enabled when the page wants it AND a launch is allowed. */
inline bool launch_button_enabled(bool wanted, bool a_proc, bool any_session) { return wanted && launch_allowed(a_proc, any_session); }

/* runners-M6: a Cheats panel opened from the game menu / View+Y hands focus back to the game when it closes (same path as closing the
 * game menu: focus_game()). One opened from the library never does. */
inline bool cheats_return_focus(bool opened_from_game, bool game_running) { return opened_from_game && game_running; }

}  // namespace hubui
