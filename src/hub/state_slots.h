#pragma once
/* Save-state slots of one game as the details modal shows them, plus the Home "resume" wording and library sort modes.
 * Pure (std::filesystem + the shared JSON reader), unit-tested by hub_states_test.
 *
 * File contract (owned by Lane 2, src/common/runner_ctl.h; mirrored here so this lane builds on its own):
 *   <state_dir>/slot<N>.state (N = 0-9)  slot<N>.png thumbnail  slot<N>.json sidecar {game_key,time,playtime,disc}
 *   <state_dir>/auto.state               the resume point (+ auto.png / auto.json when the runner writes them)
 * A state without a sidecar still lists (time = file mtime). Launch: `--load-state <path>` (Lane 2's runners). */
#include <cstdint>
#include <string>
#include <vector>

namespace hubui {

struct StateSlot {
    int slot = -1;              // 0-9, or -1 for the auto (resume) state
    bool present = false;       // the .state file exists
    std::string state_path;     // always filled, even when !present
    std::string thumb_path;     // "" when there is no thumbnail file
    uint64_t time = 0;          // unix seconds: sidecar "time", else the state file's mtime
    uint64_t playtime = 0;      // sidecar playtime (seconds), 0 = unknown
    int disc = 0;               // PS1 disc index from the sidecar
    uint64_t bytes = 0;
};

constexpr int kStateSlots = 10;

/* "Auto" first, then slot 0-9: always 11 entries (absent ones have present == false). */
std::vector<StateSlot> list_states(const std::string& state_dir);
int count_states(const std::vector<StateSlot>& v, bool include_auto);
/* Removes the state, thumbnail and sidecar. True when the .state is gone afterwards. */
bool delete_state(const std::string& state_dir, int slot);
std::string state_slot_label(int slot);   // "Auto" / "Slot 3"
/* Systems whose runner supports states at all: false for external emulators (Dolphin, Cemu, Azahar, PCSX2) and the native port. */
bool system_has_states(const std::string& system_short, bool native);
/* Why not, for the details modal ("" when supported). */
std::string no_states_reason(const std::string& system_short, bool native);

/* "3 min ago" "2h ago" "yesterday" "5 days ago" (older: "3 weeks ago"); "never" for 0. now < t reads "just now". */
std::string ago_text(uint64_t now, uint64_t t);
/* The Home hero's line: "Resume <title> - last played 2h ago" (an ASCII hyphen-dot is drawn by the caller; sep is passed in). */
std::string resume_line(const std::string& title, uint64_t now, uint64_t last_played, const std::string& sep);
/* Which launch the hero's primary button does: resume the auto state, or a plain Play. */
bool can_resume(bool has_states_support, bool auto_state_present);
/* "12:05" or "1h 05m" for a state's session playtime; "" when 0. */
std::string playtime_text(uint64_t seconds);

/* ---- library sort ---- */
enum class SortMode { Title = 0, LastPlayed = 1, MostPlayed = 2, RecentlyAdded = 3 };
constexpr int kSortModes = 4;
const char* sort_name(SortMode m);               // "Title" "Last played" "Most played" "Recently added"
const char* sort_key(SortMode m);                // persisted: "title" "last_played" "most_played" "recently_added"
SortMode sort_from_key(const std::string& k);    // unknown -> Title
SortMode next_sort(SortMode m, int dir = 1);     // wraps

}  // namespace hubui
