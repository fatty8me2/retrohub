#pragma once
/* Completion status + smart-shelf rules (Plan 4 Wave 3, L1 + L2). Pure logic, no UI: progress_ui.cpp and customize_ui.cpp draw it.
 * The data lives in synced prefs (src/common/prefs.h: progress[key], user_shelves[i].rule). */
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "customize_logic.h"
#include "prefs.h"

namespace hubui { namespace prog {

/* ---- completion status ---- */
struct StatusInfo { const char* id; const char* label; const char* badge; };
inline const std::vector<StatusInfo>& statuses() {
    static const std::vector<StatusInfo> v = {
        {"playing", "Playing", "PLAYING"}, {"beaten", "Beaten", "BEATEN"}, {"complete", "100%", "100%"},
        {"dropped", "Dropped", "DROPPED"}, {"backlog", "Backlog", "BACKLOG"},
    };
    return v;
}
inline const StatusInfo* find_status(const std::string& id) {
    for (const auto& s : statuses()) if (id == s.id) return &s;
    return nullptr;
}
inline bool valid_status(const std::string& id) { return id.empty() || find_status(id) != nullptr; }
inline std::string status_label(const std::string& id) { const StatusInfo* s = find_status(id); return s ? s->label : "No status"; }
/* The tile badge: "" when the game has no status (an unknown id from a newer client shows nothing, too). */
inline std::string status_badge(const std::string& id) { const StatusInfo* s = find_status(id); return s ? s->badge : ""; }

constexpr size_t kNoteMax = 500;
/* The note as stored: control characters (except newline) dropped, trimmed, at most 500 bytes without splitting a UTF-8 character. */
inline std::string clean_note(const std::string& in) {
    std::string o;
    for (unsigned char c : in) if (c >= 32 || c == '\n' || c >= 0x80) o += (char)c;
    size_t b = o.find_first_not_of(" \t\r\n"), e = o.find_last_not_of(" \t\r\n");
    o = b == std::string::npos ? std::string() : o.substr(b, e - b + 1);
    if (o.size() > kNoteMax) {   // cut at a character boundary
        size_t n = kNoteMax;
        while (n > 0 && ((unsigned char)o[n] & 0xC0) == 0x80) --n;
        o.resize(n);
    }
    return o;
}

/* New progress map after setting one game's status (keeps its note). Empty status + empty note removes the game. */
inline std::map<std::string, hub::prefs::Progress> with_status(std::map<std::string, hub::prefs::Progress> m, const std::string& key, const std::string& status) {
    if (key.empty() || !valid_status(status)) return m;
    hub::prefs::Progress p = m.count(key) ? m[key] : hub::prefs::Progress();
    p.status = status;
    if (p.status.empty() && p.note.empty()) m.erase(key); else m[key] = p;
    return m;
}
inline std::map<std::string, hub::prefs::Progress> with_note(std::map<std::string, hub::prefs::Progress> m, const std::string& key, const std::string& note) {
    if (key.empty()) return m;
    hub::prefs::Progress p = m.count(key) ? m[key] : hub::prefs::Progress();
    p.note = clean_note(note);
    if (p.status.empty() && p.note.empty()) m.erase(key); else m[key] = p;
    return m;
}

/* ---- smart shelf rules ---- */
struct Facts {
    std::string key, system, genre;     // system = library short code ("n64"); genre = "Other" when unknown
    int players = 0;                    // most players the game takes (0 = unknown)
    int year = 0;                       // release year (0 = unknown)
    bool favorite = false;
    unsigned long long last_played = 0; // unix seconds, 0 = never
    std::string status;                 // completion status ("" = none)
};

inline std::string fold(const std::string& s) {
    std::string o = s;
    for (auto& c : o) c = (char)std::tolower((unsigned char)c);
    return o;
}
inline int decade_of(int year) { return year >= 1950 ? year / 10 * 10 : 0; }

/* True when no field is set: such a rule would match the whole library, so the editor does not let you save it. */
inline bool rule_is_empty(const hub::prefs::ShelfRule& r) {
    return r.systems.empty() && r.genre.empty() && r.min_players <= 0 && r.status.empty() && !r.favorites_only && r.not_played_days <= 0 && r.decade <= 0;
}

/* Does this game match every field that is set? `now` = unix seconds. A game never played counts as "not played in N days".
 * A game with an unknown year or player count does not match a decade / players rule. */
inline bool rule_matches(const hub::prefs::ShelfRule& r, const Facts& f, unsigned long long now) {
    if (!r.systems.empty() && std::find(r.systems.begin(), r.systems.end(), f.system) == r.systems.end()) return false;
    if (!r.genre.empty() && fold(r.genre) != fold(f.genre)) return false;
    if (r.min_players > 0 && f.players < r.min_players) return false;
    if (!r.status.empty() && r.status != f.status) return false;
    if (r.favorites_only && !f.favorite) return false;
    if (r.not_played_days > 0 && f.last_played != 0) {
        unsigned long long cutoff = (unsigned long long)r.not_played_days * 86400ull;
        if (now > f.last_played && now - f.last_played < cutoff) return false;   // played within the window
        if (now <= f.last_played) return false;                                   // played "in the future" (clock skew): recent
    }
    if (r.decade > 0 && decade_of(f.year) != r.decade) return false;
    return true;
}

/* Indices of the matching games, ordered by title is the caller's job (it owns the titles); here: input order. */
inline std::vector<int> rule_select(const hub::prefs::ShelfRule& r, const std::vector<Facts>& games, unsigned long long now) {
    std::vector<int> out;
    for (size_t i = 0; i < games.size(); ++i) if (rule_matches(r, games[i], now)) out.push_back((int)i);
    return out;
}

/* "N64 or GameCube, 2+ players, Backlog" for the manager row and the editor. */
inline std::string rule_summary(const hub::prefs::ShelfRule& r) {
    std::vector<std::string> parts;
    if (!r.systems.empty()) {
        std::string s;
        for (size_t i = 0; i < r.systems.size(); ++i) { if (i) s += i + 1 == r.systems.size() ? " or " : ", "; s += r.systems[i]; }
        parts.push_back(s);
    }
    if (!r.genre.empty()) parts.push_back(r.genre);
    if (r.min_players > 0) parts.push_back(std::to_string(r.min_players) + "+ players");
    if (!r.status.empty()) parts.push_back(status_label(r.status));
    if (r.favorites_only) parts.push_back("favorites");
    if (r.not_played_days > 0) parts.push_back("not played in " + std::to_string(r.not_played_days) + " days");
    if (r.decade > 0) parts.push_back("the " + std::to_string(r.decade) + "s");
    if (parts.empty()) return "Everything";
    std::string o;
    for (size_t i = 0; i < parts.size(); ++i) { if (i) o += ", "; o += parts[i]; }
    return o;
}

/* Editor steppers. Each returns the next value in a short cycle, so a pad user needs only one button per field. */
inline int next_players(int v) { return v <= 0 ? 2 : v >= 4 ? 0 : v + 1; }                      // any, 2+, 3+, 4+
inline int next_days(int v) { return v <= 0 ? 30 : v == 30 ? 90 : v == 90 ? 180 : v == 180 ? 365 : 0; }
inline int next_decade(int v) { return v <= 0 ? 1980 : v >= 2020 ? 0 : v + 10; }                // any, 1980s ... 2020s
inline std::string next_status(const std::string& s) {
    if (s.empty()) return statuses().front().id;
    for (size_t i = 0; i < statuses().size(); ++i) if (s == statuses()[i].id) return i + 1 < statuses().size() ? statuses()[i + 1].id : "";
    return "";
}
inline void toggle_system(hub::prefs::ShelfRule* r, const std::string& sys) {
    auto it = std::find(r->systems.begin(), r->systems.end(), sys);
    if (it == r->systems.end()) r->systems.push_back(sys); else r->systems.erase(it);
}

/* Smart shelf edits on the shelves vector (same contract as customize_logic.h: true = something changed). */
inline bool add_smart_shelf(hubui::cust::Shelves* v, const std::string& wanted, const hub::prefs::ShelfRule& rule, std::string* made = nullptr) {
    if (v->size() >= 24 || rule_is_empty(rule)) return false;
    hub::prefs::Shelf s;
    s.name = hubui::cust::unique_name(*v, wanted.empty() ? std::string("Smart shelf") : wanted);
    s.smart = true;
    s.rule = rule;
    v->push_back(s);
    if (made) *made = s.name;
    return true;
}
/* Replace the rule (and name) of shelf i; it becomes / stays smart. */
inline bool edit_smart_shelf(hubui::cust::Shelves* v, int i, const std::string& wanted, const hub::prefs::ShelfRule& rule) {
    if (i < 0 || i >= (int)v->size() || rule_is_empty(rule)) return false;
    hub::prefs::Shelf s = (*v)[i];
    s.name = hubui::cust::unique_name(*v, wanted.empty() ? s.name : wanted, i);
    s.smart = true;
    s.rule = rule;
    s.keys.clear();
    if (s == (*v)[i]) return false;
    (*v)[i] = s;
    return true;
}
/* A copy of the shelves where every smart shelf holds the keys its rule picks (titles map key -> title decides the order, case-insensitive),
 * so the Home composer treats it like a hand-made one. */
inline hubui::cust::Shelves resolve_smart(const hubui::cust::Shelves& v, const std::vector<Facts>& games, const std::map<std::string, std::string>& titles,
                                          unsigned long long now, size_t max_items = 200) {
    hubui::cust::Shelves out = v;
    for (auto& s : out) {
        if (!s.smart) continue;
        std::vector<std::pair<std::string, std::string>> hit;   // folded title, key
        for (int i : rule_select(s.rule, games, now)) {
            auto t = titles.find(games[i].key);
            hit.push_back({fold(t == titles.end() ? games[i].key : t->second), games[i].key});
        }
        std::sort(hit.begin(), hit.end());
        s.keys.clear();
        for (size_t k = 0; k < hit.size() && k < max_items; ++k) s.keys.push_back(hit[k].second);
    }
    return out;
}

}}  // namespace hubui::prog
