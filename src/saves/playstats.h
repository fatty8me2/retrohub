#pragma once
/* Play-time stats (Wave 2 H). Every finished session is appended to the game's vault folder as one line
 *   <game dir>/playlog/<this PC>.log      "<start unix>,<seconds>\n"
 * One file per PC, so two PCs never conflict and the cloud saves carry them to every PC.
 * Aggregation is pure (tests feed it sessions). */

#include <cstdint>
#include <string>
#include <vector>

namespace hub::saves {

struct Session { std::string system, key; int64_t start = 0, secs = 0; };
struct GameTotal { std::string system, key; int64_t secs = 0; };   // lifetime play time (library.json play_seconds)

struct StatsTop { std::string system, key; int64_t secs = 0; };
struct Stats {
    int64_t total_secs = 0;
    int64_t week_secs = 0;          // last 7 days (rolling)
    int sessions = 0;
    Session longest;                // secs == 0 when no sessions
    std::vector<StatsTop> top;      // up to 5, most played first
    std::vector<std::pair<std::string, int64_t>> systems;   // system short -> secs, biggest first
    int streak_days = 0;            // consecutive local days with play, ending today (or yesterday)
};

/* totals: lifetime numbers per game (they win for total/top/systems); sessions: logged sessions (for week, longest, streak).
 * tz_offset_s = local time minus UTC. */
Stats compute_stats(const std::vector<GameTotal>& totals, const std::vector<Session>& sessions, int64_t now, int tz_offset_s, size_t top_n = 5);

std::string playlog_line(int64_t start, int64_t secs);
bool parse_playlog_line(const std::string& line, int64_t* start, int64_t* secs);

/* I/O */
void playlog_append(const std::string& system, const std::string& key, int64_t start, int64_t secs);   // this PC's file in the game folder
std::vector<Session> playlog_read_all();   // every game in the vault, every PC's file

} // namespace hub::saves
