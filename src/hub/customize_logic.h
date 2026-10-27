#pragma once
/* Make-it-yours logic (pure, no UI): user shelves (create / rename / reorder / delete / membership) and renamed titles.
 * The data lives in synced prefs (src/common/prefs.h: user_shelves, renames, covers); the UI is customize_ui.cpp.
 * All edits work on a copy of the vector and return whether anything changed, so the caller only persists real changes
 * (through prefs::set_user_shelves, which stamps the field and leaves the merge logic alone). */
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "logic.h"
#include "prefs.h"

namespace hubui { namespace cust {

typedef std::vector<hub::prefs::Shelf> Shelves;

inline std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}
inline std::string fold(const std::string& s) {
    std::string o = s;
    for (auto& c : o) c = (char)std::tolower((unsigned char)c);
    return o;
}
inline int find_shelf(const Shelves& v, const std::string& name) {
    const std::string f = fold(trim(name));
    for (size_t i = 0; i < v.size(); ++i) if (fold(v[i].name) == f) return (int)i;
    return -1;
}
/* A usable, unused shelf name: trimmed, capped at 40 bytes, "New shelf" when empty, " 2" / " 3" ... added when taken.
 * `except` = an index whose own name does not count as taken (renaming a shelf to its current name). */
inline std::string unique_name(const Shelves& v, const std::string& wanted, int except = -1) {
    std::string base = trim(wanted);
    if (base.size() > 40) base.resize(40);
    base = trim(base);
    if (base.empty()) base = "New shelf";
    std::string name = base;
    for (int n = 2; n < 1000; ++n) {
        int at = find_shelf(v, name);
        if (at < 0 || at == except) break;
        name = base + " " + std::to_string(n);
    }
    return name;
}
inline bool add_shelf(Shelves* v, const std::string& wanted, std::string* made = nullptr) {
    if (v->size() >= 24) return false;
    std::string n = unique_name(*v, wanted);
    hub::prefs::Shelf s;
    s.name = n;
    v->push_back(s);
    if (made) *made = n;
    return true;
}
inline bool rename_shelf(Shelves* v, int i, const std::string& wanted) {
    if (i < 0 || i >= (int)v->size()) return false;
    std::string n = unique_name(*v, wanted, i);
    if ((*v)[i].name == n) return false;
    (*v)[i].name = n;
    return true;
}
inline bool delete_shelf(Shelves* v, int i) {
    if (i < 0 || i >= (int)v->size()) return false;
    v->erase(v->begin() + i);
    return true;
}
/* dir -1 = up (earlier on Home), +1 = down. False at either end. */
inline bool move_shelf(Shelves* v, int i, int dir) {
    int j = i + (dir < 0 ? -1 : 1);
    if (i < 0 || i >= (int)v->size() || j < 0 || j >= (int)v->size()) return false;
    std::swap((*v)[i], (*v)[j]);
    return true;
}
inline bool has_game(const Shelves& v, int i, const std::string& key) {
    if (i < 0 || i >= (int)v.size()) return false;
    return std::find(v[i].keys.begin(), v[i].keys.end(), key) != v[i].keys.end();
}
inline bool add_game(Shelves* v, int i, const std::string& key) {
    if (key.empty() || i < 0 || i >= (int)v->size() || has_game(*v, i, key)) return false;
    (*v)[i].keys.push_back(key);
    return true;
}
inline bool remove_game(Shelves* v, int i, const std::string& key) {
    if (i < 0 || i >= (int)v->size()) return false;
    auto& k = (*v)[i].keys;
    auto it = std::find(k.begin(), k.end(), key);
    if (it == k.end()) return false;
    k.erase(it);
    return true;
}
inline bool toggle_game(Shelves* v, int i, const std::string& key) {
    return has_game(*v, i, key) ? remove_game(v, i, key) : add_game(v, i, key);
}
/* How many shelves hold this game. */
inline int membership_count(const Shelves& v, const std::string& key) {
    int n = 0;
    for (size_t i = 0; i < v.size(); ++i) n += has_game(v, (int)i, key) ? 1 : 0;
    return n;
}

/* Home shelves for the user's shelves, in their order, after the built-ins: id "user:<index>", the shelf name as title, items =
 * indices into the ShelfGame vector (shelf order kept; keys that are not in the library, and friends' ghost games, are skipped).
 * Empty shelves are left out (the manager still lists them). */
inline std::vector<hubui::Shelf> compose_user_shelves(const Shelves& v, const std::map<std::string, int>& index_of_key) {
    std::vector<hubui::Shelf> out;
    for (size_t i = 0; i < v.size(); ++i) {
        hubui::Shelf s;
        s.id = "user:" + std::to_string(i);
        s.title = v[i].name;
        for (const std::string& k : v[i].keys) {
            auto it = index_of_key.find(k);
            if (it != index_of_key.end()) s.items.push_back(it->second);
        }
        if (!s.items.empty()) out.push_back(s);
    }
    return out;
}

/* The shown title: the user's rename when set and non-blank, else the scanned one. */
inline std::string display_title(const std::map<std::string, std::string>& renames, const std::string& key, const std::string& original) {
    auto it = renames.find(key);
    if (it == renames.end()) return original;
    std::string t = trim(it->second);
    return t.empty() ? original : t;
}
/* Rename that is just the original name again is no rename: returns the new renames map (key dropped when blank or equal). */
inline std::map<std::string, std::string> with_rename(std::map<std::string, std::string> m, const std::string& key, const std::string& original,
                                                      const std::string& wanted) {
    std::string t = trim(wanted);
    if (t.size() > 80) t.resize(80);
    if (t.empty() || t == original) m.erase(key);
    else m[key] = t;
    return m;
}

}}  // namespace hubui::cust
