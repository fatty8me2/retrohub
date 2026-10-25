#pragma once
/* STUB (agent/cheat-ui): the real cheats_db.h/.cpp (agent/cheat-db + agent/cheat-featured) REPLACE this on merge.
 * Interface the hub UI codes against: cheats_for(GameEntry) -> the known cheats of a game, each flagged featured or not. */
#include <vector>

#include "cheats.h"
#include "library.h"

namespace cheatsdb {

struct Entry {
    cheats::Cheat cheat;     // enabled is ignored (the hub applies the player's choices)
    bool featured = false;   // shown under "Popular"
};

/* Stub: a tiny built-in sample (real data comes from the database agents). */
std::vector<Entry> cheats_for(const hub::GameEntry& e);

}  // namespace cheatsdb
