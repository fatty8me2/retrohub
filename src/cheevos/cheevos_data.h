#pragma once
/* RetroAchievements data shared by the hub and retrorun: pure logic, no network, no rcheevos.
 *   userdata\retroachievements.json   {"enabled":bool,"hardcore":bool,"username":"..","token":".."}   (the password is NEVER stored)
 *   userdata\cheevos\game_<key>.json  per-game cache the runner writes (title, progress, achievement list)
 *   userdata\cheevos\recent.json      newest unlocks across games (Home "Recent unlocks")
 * The hub only reads the caches; the runner writes them. */
#include <string>
#include <vector>

namespace cheevos {

struct Config {
    bool enabled = false;     // master switch; also needs a token
    bool hardcore = false;    // hardcore: no state load, rewind, cheats, fast-forward or slow motion
    std::string username, token;
    bool signed_in() const { return !username.empty() && !token.empty(); }
};
std::string config_to_json(const Config& c);
bool config_from_json(const std::string& text, Config* out);
bool load_config(const std::string& path, Config* out);
bool save_config(const std::string& path, const Config& c);

struct Ach {
    unsigned id = 0;
    std::string title, desc, badge;
    unsigned points = 0;
    long long unlock_time = 0;   // 0 = locked
    bool hardcore = false;       // unlocked in hardcore
};
struct GameCache {
    std::string game_key, title;
    unsigned game_id = 0;
    unsigned total = 0, unlocked = 0, points_total = 0, points_unlocked = 0;
    long long updated = 0;
    std::vector<Ach> achs;
};
struct Recent {
    std::string game_key, game_title, title, desc;
    unsigned points = 0;
    long long time = 0;
    bool hardcore = false;
};

std::string cache_to_json(const GameCache& g);
bool cache_from_json(const std::string& text, GameCache* out);
std::string recent_to_json(const std::vector<Recent>& r);
bool recent_from_json(const std::string& text, std::vector<Recent>* out);
/* Newest first, same achievement of same game replaced, capped at `cap`. */
void recent_push(std::vector<Recent>* r, const Recent& n, size_t cap = 12);

std::string safe_key(const std::string& game_key);   // file-name safe
std::string cache_path(const std::string& dir, const std::string& game_key);
std::string recent_path(const std::string& dir);
bool save_cache(const std::string& dir, const GameCache& g);
bool load_cache(const std::string& dir, const std::string& game_key, GameCache* out);
bool save_recent(const std::string& dir, const std::vector<Recent>& r);
std::vector<Recent> load_recent(const std::string& dir);

/* "3 of 12 unlocked (25%)" */
std::string progress_text(const GameCache& g);

/* Hardcore rules. */
enum class Feature { LoadState, Rewind, Cheats, FastForward, SlowMotion };
bool hardcore_blocks(bool hardcore, Feature f);
const char* block_message(Feature f);
/* Speed multiplier allowed in hardcore (anything below 1x is not; fast-forward is its own Feature). */
bool speed_allowed(bool hardcore, double speed);

}  // namespace cheevos
