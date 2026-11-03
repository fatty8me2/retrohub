#pragma once
/* Once per version, RetroHub re-saves its own Start Menu / Desktop shortcuts and tells Explorer to reload icons: Windows keeps a stale
 * icon for a shortcut whose exe got a new icon in an update. Pure decision logic (no files, no shell); the work is shell_refresh_icons(). */
#include <string>

namespace hubicon {

/* True when the version running now differs from the one recorded after the last refresh (nothing recorded = never refreshed). */
inline bool refresh_due(const std::string& recorded, const std::string& current)
{
    return !current.empty() && recorded != current;
}

/* One step of the policy. `allowed` = a real installed copy that is not a test / headless / QA run, `busy` = a game or session is running.
 * Runs `refresh` at most once per version and only when idle; *record is set to `current` only after the refresh ran, so a busy start simply
 * tries again later and a failed refresh (returns false) is retried on the next start. Returns true if the refresh ran and succeeded. */
template <class F>
bool step(const std::string& recorded, const std::string& current, bool allowed, bool busy, F&& refresh, std::string* record)
{
    if (!allowed || busy || !refresh_due(recorded, current)) return false;
    if (!refresh()) return false;
    *record = current;
    return true;
}

} // namespace hubicon
