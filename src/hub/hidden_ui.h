#pragma once
/* Hide game / Show hidden (library-12). Implemented in libcopies_ui.cpp next to the Hidden games list; pure logic is in common/hidden_games.h. */
#include <string>
#include <vector>

#include "library.h"

/* One press: hides the game on all your PCs (the synced prefs), takes it out of the library and shows the Undo card. */
void hide_game_now(const std::string& key);
/* Undo / Unhide: brings it back (a rescan puts it in the library again). */
void unhide_game_now(const std::string& key);

/* apply_scan hands over the hidden games it took out of the library, so the "Show hidden" strip can list them. */
void hidden_stash_set(std::vector<hub::GameEntry> hidden);
size_t hidden_stash_count();

/* "Show hidden": off by default, remembered on this PC. */
bool show_hidden_on();
void show_hidden_toggle();

/* The library's dimmed list of hidden games with an Unhide button each (empty unless "Show hidden" is on). */
void hidden_strip_draw();
