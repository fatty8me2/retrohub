#pragma once
/* Completion status + notes (L2) and smart-shelf facts (L1): the small surface the other hub files use. Include after app.h. */
#include <string>

#include "progress_logic.h"

namespace hub { struct GameEntry; }

/* The tile badge text for a game ("" = no status set, so no badge) and its hover text. */
std::string progress_tag(const std::string& key, std::string* tip = nullptr);
/* What a smart-shelf rule is matched against (system, genre, players, year, favorite, last played, status). */
hubui::prog::Facts progress_facts(const hub::GameEntry& e);
/* Genres present in the library, sorted ("Other" last): the smart-shelf editor's genre choices. */
std::vector<std::string> progress_genres();

/* Synced prefs access (customize_ui.cpp owns the cache and the writes). custom_progress: null when the game has no status and no note. */
const hub::prefs::Progress* custom_progress(const std::string& key);
bool custom_set_status(const std::string& key, const std::string& status);   // "" clears it
bool custom_set_note(const std::string& key, const std::string& note);       // cleaned + capped at 500 characters
