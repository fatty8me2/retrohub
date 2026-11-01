#pragma once
/* Auto-resume guard (Plan 4, saves-F1). Pure: the caller passes file times (unix seconds, 0 = file missing).
 *
 * retrorun's --resume loads <states>/auto.state at start. When the game's battery save is NEWER than that state (another PC's
 * save arrived through the cloud or a sync folder, or the game was played without writing a state), resuming the old state
 * rolls the progress back and the next in-game save writes over the newer one. So the newer save wins: no --resume, and the
 * old resume point stays available from the game page (state strip, "Resume").
 *
 * At exit retrorun writes the battery file BEFORE auto.state (save_srm, then the state), so a same-session battery file is never
 * newer than the state; kResumeSlackSec only absorbs whole-second mtimes (the cloud stores unix seconds) and file-system rounding.
 * Clock skew between PCs can still hide an older-looking newer save: the state-sidecar check of Plan 4 R1 closes that. */
#include <cstdint>

namespace hubui {

constexpr int64_t kResumeSlackSec = 15;

/* state_time: auto.state mtime. newest_battery_time: newest file in the game's battery folder. */
inline bool resume_allowed(int64_t state_time, int64_t newest_battery_time) {
    if (state_time <= 0 || newest_battery_time <= 0) return true;   // nothing to protect: unchanged behaviour
    return newest_battery_time <= state_time + kResumeSlackSec;
}

}  // namespace hubui
