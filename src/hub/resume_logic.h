#pragma once
/* Auto-resume guard (Plan 4, saves-F1). Pure: the caller passes file times (unix seconds, 0 = file missing).
 *
 * retrorun's --resume loads <states>/auto.state at start. When the game's battery save is NEWER than that state (another PC's
 * save arrived through the cloud or a sync folder, or the game was played without writing a state), resuming the old state
 * rolls the progress back and the next in-game save writes over the newer one. So the newer save wins: no --resume, and the
 * old resume point stays available from the game page (state strip, "Resume").
 *
 * The runner writes the battery file and auto.state around the same exit, in either order, so differences up to
 * kResumeSlackSec count as the same session. */
#include <cstdint>

namespace hubui {

constexpr int64_t kResumeSlackSec = 120;

/* state_time: auto.state mtime. newest_battery_time: newest file in the game's battery folder. */
inline bool resume_allowed(int64_t state_time, int64_t newest_battery_time) {
    if (state_time <= 0 || newest_battery_time <= 0) return true;   // nothing to protect: unchanged behaviour
    return newest_battery_time <= state_time + kResumeSlackSec;
}

}  // namespace hubui
