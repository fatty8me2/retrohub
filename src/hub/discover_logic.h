#pragma once
/* Discover page: curated "recommended games" per console (data/recommended.json, shipped next to RetroHub.exe in data\).
 * Metadata only: titles and facts, never a download link. Whether a recommendation is already in the library, installable from a
 * shared GamePack, or has to be added by the player is decided by classify() (tests: test_discover.cpp). */

#include "friend_games.h"
#include "gamepack.h"

#include <string>
#include <vector>

namespace discover {

struct Rec {
    std::string title, sys, genre, why;
    std::string cid;     // games.h catalog id when the game is in the catalog ("" = not catalogued)
    std::string also;    // a second catalog id that counts as owning it (Smash 64: the ROM and the native port)
    int year = 0;
    int players = 1;     // most local players
    bool friends = false;   // "Great with friends"
};

/* Pure: parse the file text. Every game needs title, console, year, genre, players, why; a game that lacks one is dropped and
 * counted in *bad (optional). False when the text is not {"games":[...]}. */
bool parse(const std::string& text, std::vector<Rec>* out, int* bad = nullptr);

/* The shipped list (<exe dir>\data\recommended.json), read once. Empty when the file is missing. */
const std::vector<Rec>& all();

/* Console short codes in the order the picker shows them. */
const std::vector<std::string>& consoles();

/* Pure: the recommendations of one console in file order; friends_only keeps the "Great with friends" ones. */
std::vector<const Rec*> for_console(const std::vector<Rec>& recs, const std::string& sys, bool friends_only);

enum class Status { Owned, InPack, Missing };

struct Match {
    Status st = Status::Missing;
    std::string key;          // Owned: the library entry's key
    std::string pack_sys;     // InPack: the pack's console folder (differs from the rec's for Game Boy / Color twins)
    std::string pack_title;   // InPack: the title to hand to the GamePack installer
    std::string pack_hint;    // InPack: the Drive entry name without extension (exact art-name hint)
};

/* Pure: where a recommendation stands. A library game matches by catalog id first, then by title key (hubgp::pack_key, the same
 * rule the installer uses, GB / GBC twins included). A pack game matches by pack_pick on the console's pack index. */
Match classify(const Rec& r, const std::vector<hubui::fg::Owned>& owned, const std::vector<hubgp::PackGame>& pack);

/* Pure: "1 player" / "1-4 players". */
std::string players_text(int players);

}  // namespace discover
