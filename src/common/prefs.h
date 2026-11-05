#pragma once
/* Synced account preferences: <vault>/_account/prefs.json (cloud class "Settings", see src/saves).
 * Typed struct + JSON loader/serializer + a FIELD-WISE merge. Every field carries its own last-modified time (unix ms), and
 * merge() keeps, per field, whichever side changed last, so two PCs editing different fields never overwrite each other.
 * Fields: favorites, user_shelves, hidden, renames, covers, preferred_copy, progress, theme, profile.avatar, profile.fav_game, profile.status.
 * Map-like fields (renames, covers, preferred_copy, progress, user_shelves) merge per KEY (shelves: per name), favorites/hidden per key
 * with tombstones, and top-level fields a newer client added are carried through untouched.
 * Game identities are the library keys (library.h). Pure logic apart from load()/save()/the library mirror. No UI. */
#include <cstdint>
#include <map>
#include <set>
#include <tuple>
#include <string>
#include <utility>
#include <vector>

#include "json.h"

namespace hub { namespace prefs {

/* A smart shelf's rule (Plan 4 L1). Every field is optional; a game must match all that are set. Stored under "rule" in a shelf's
 * JSON; clients that do not know it ignore it and show an empty shelf. */
struct ShelfRule {
    std::vector<std::string> systems;   // library system_short codes; empty = any system
    std::string genre;                  // "" = any
    int min_players = 0;                // players >= N; 0 = any
    std::string status;                 // completion status ("playing" ...); "" = any
    bool favorites_only = false;
    int not_played_days = 0;            // not played in N days (never played counts); 0 = any
    int decade = 0;                     // 1990 = the 1990s; 0 = any
    bool operator==(const ShelfRule& o) const {
        return systems == o.systems && genre == o.genre && min_players == o.min_players && status == o.status &&
               favorites_only == o.favorites_only && not_played_days == o.not_played_days && decade == o.decade;
    }
    bool operator<(const ShelfRule& o) const {
        return std::tie(systems, genre, min_players, status, favorites_only, not_played_days, decade) <
               std::tie(o.systems, o.genre, o.min_players, o.status, o.favorites_only, o.not_played_days, o.decade);
    }
};

struct Shelf {
    std::string name;
    std::vector<std::string> keys;      // hand-picked games (unused by a smart shelf)
    bool smart = false;                 // true = the shelf is `rule`, not `keys`
    ShelfRule rule;
    bool operator==(const Shelf& o) const { return name == o.name && keys == o.keys && smart == o.smart && rule == o.rule; }
    bool operator<(const Shelf& o) const {
        return std::tie(name, keys, smart, rule) < std::tie(o.name, o.keys, o.smart, o.rule);
    }
};

/* Completion status + a short note for one game (Plan 4 L2). status: "" | playing | beaten | complete (100%) | dropped | backlog. */
struct Progress {
    std::string status, note;           // note <= 500 characters
    bool operator==(const Progress& o) const { return status == o.status && note == o.note; }
    bool operator<(const Progress& o) const { return std::tie(status, note) < std::tie(o.status, o.note); }
};

struct Profile {
    std::string avatar, fav_game, status;
};

struct Prefs {
    std::vector<std::string> favorites;            // game keys, kept sorted + unique by set_favorites / mirror
    std::vector<Shelf> user_shelves;               // user-made shelves, in display order
    std::vector<std::string> hidden;               // game keys hidden from the library
    std::map<std::string, std::string> renames;    // game key -> display name
    std::map<std::string, std::string> covers;     // game key -> cover image path
    std::map<std::string, std::string> preferred_copy;   // game key -> path of the duplicate copy to use (library scan)
    std::map<std::string, Progress> progress;      // game key -> completion status + note
    std::string theme;
    Profile profile;
    /* Per-key stamps for the map fields (renames, covers, preferred_copy, progress, user_shelves by shelf name): field -> key -> unix ms,
     * negative = tombstone. merge() resolves each key on its own, so two PCs editing different games both keep their edit. */
    std::map<std::string, std::map<std::string, long long>> map_ts;
    std::set<std::string> legacy_maps;             // map fields parsed from a file with no per-key stamps (older client); cleared by the setters
    /* Top-level fields this client does not know (a newer RetroHub wrote them). Kept through parse / serialize / merge, never dropped. */
    std::vector<std::pair<std::string, json::Value>> extra;
    /* Per-key stamps for favorites and hidden (unix ms). Positive = key present as of that time, negative = tombstone
     * (removed at -value). merge() unions these, newest per key wins, so two PCs favoriting different games both keep them. */
    std::map<std::string, long long> favorites_ts, hidden_ts;
    bool legacy_favorites = false, legacy_hidden = false;   // parsed from a file that had no per-key stamps (older client); cleared by the setters
    std::map<std::string, long long> ts;           // field name -> last modified (unix ms); absent = 0

    long long stamp(const std::string& field) const;
};

/* Field names used in Prefs::ts. */
extern const char* const kFields[];                // null-terminated list
extern const int kFieldCount;

long long now_ms();                                // wall clock, unix ms

/* ---- setters: change the field and stamp it with `t` (default: now). Return false (and do nothing) if the value is unchanged. */
bool set_favorites(Prefs* p, std::vector<std::string> keys, long long t = 0);
bool set_favorite(Prefs* p, const std::string& key, bool fav, long long t = 0);
bool set_user_shelves(Prefs* p, std::vector<Shelf> shelves, long long t = 0);
bool set_hidden(Prefs* p, std::vector<std::string> keys, long long t = 0);
bool set_renames(Prefs* p, std::map<std::string, std::string> m, long long t = 0);
bool set_covers(Prefs* p, std::map<std::string, std::string> m, long long t = 0);
bool set_preferred_copy(Prefs* p, std::map<std::string, std::string> m, long long t = 0);
bool set_progress(Prefs* p, std::map<std::string, Progress> m, long long t = 0);   // entries with no status and no note are dropped
bool set_theme(Prefs* p, const std::string& theme, long long t = 0);
bool set_profile_avatar(Prefs* p, const std::string& v, long long t = 0);
bool set_profile_fav_game(Prefs* p, const std::string& v, long long t = 0);
bool set_profile_status(Prefs* p, const std::string& v, long long t = 0);

/* ---- JSON. parse() tolerates a missing/garbage file (returns false, *out = empty) and unknown fields. */
bool parse(const std::string& text, Prefs* out);
std::string serialize(const Prefs& p);

/* Field-wise merge: per field the newer stamp wins; equal stamps with different values pick deterministically (the
 * lexicographically larger serialization), so merge(a,b) == merge(b,a) and merge is idempotent. */
Prefs merge(const Prefs& a, const Prefs& b);
/* Drops tombstones (favorites / hidden) older than 90 days relative to `now` (default: the wall clock). Called when saving, never inside merge(). */
void gc_tombstones(Prefs* p, long long now = 0);
/* Text convenience for a sync conflict resolver: parse both, merge, serialize. A side that fails to parse is treated as empty. */
std::string merge_text(const std::string& local, const std::string& remote);

/* ---- file. `vault_root` is the folder that holds _account/ (hub::saves vault root). */
std::string prefs_path(const std::string& vault_root);   // <vault_root>/_account/prefs.json
bool load(const std::string& vault_root, Prefs* out);    // false if missing/corrupt (*out empty)
bool save(const std::string& vault_root, const Prefs& p);// atomic write, creates _account/
/* Load, merge with `incoming` (e.g. a pulled copy), save if the result differs; returns the merged prefs. */
Prefs merge_into_file(const std::string& vault_root, const Prefs& incoming);

/* ---- library.json favorites <-> prefs.
 * library.json stays the source of truth for the library UI. set_vault_root() arms the mirror: after that, every
 * hub::set_favorite() also records the change here (stamped now). "" disarms it (the default). */
void set_vault_root(const std::string& vault_root);
/* Called (outside any lock) after a local change was written to prefs.json: the cloud engine uses it to queue a push. */
void set_change_hook(void (*fn)());
void mirror_favorite(const std::string& key, bool fav);          // called from hub::set_favorite
/* After prefs were merged from the cloud: make library.json's favorites equal prefs.favorites. Returns how many changed.
 * Does nothing when prefs has no favorites stamp (a fresh prefs never wipes the library). Does not echo back into prefs. */
int apply_favorites_to_library(const Prefs& p);
/* One-time/seeding: copy every favorite in library.json into prefs (stamped now) when prefs has no favorites stamp yet. */
void seed_from_library();

}}  // namespace hub::prefs
