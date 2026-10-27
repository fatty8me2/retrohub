#pragma once
/* Synced account preferences: <vault>/_account/prefs.json (cloud class "Settings", see src/saves).
 * Typed struct + JSON loader/serializer + a FIELD-WISE merge. Every field carries its own last-modified time (unix ms), and
 * merge() keeps, per field, whichever side changed last, so two PCs editing different fields never overwrite each other.
 * Fields: favorites, user_shelves, hidden, renames, covers, theme, profile.avatar, profile.fav_game, profile.status.
 * Game identities are the library keys (library.h). Pure logic apart from load()/save()/the library mirror. No UI. */
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace hub { namespace prefs {

struct Shelf {
    std::string name;
    std::vector<std::string> keys;
    bool operator==(const Shelf& o) const { return name == o.name && keys == o.keys; }
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
    std::string theme;
    Profile profile;
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
void mirror_favorite(const std::string& key, bool fav);          // called from hub::set_favorite
/* After prefs were merged from the cloud: make library.json's favorites equal prefs.favorites. Returns how many changed.
 * Does nothing when prefs has no favorites stamp (a fresh prefs never wipes the library). Does not echo back into prefs. */
int apply_favorites_to_library(const Prefs& p);
/* One-time/seeding: copy every favorite in library.json into prefs (stamped now) when prefs has no favorites stamp yet. */
void seed_from_library();

}}  // namespace hub::prefs
