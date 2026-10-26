#pragma once
/* Room game picker: pure list logic (no ImGui, no App state) so test_picker.cpp can check search, console chips, sections, ordering,
 * the virtualised grid layout and the labels. The UI (page_online.cpp) builds Items from the library + the room members' hello info. */
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace hubpick {

struct Item {
    std::string id;         // room id (catalog id or library key)
    std::string title;
    std::string sys;        // system short ("n64", "gc", ...)
    std::string sys_name;   // "Nintendo 64" (console section title / chip label)
    int min_players = 1, max_players = 4;   // local range shown on the tile ("1-4 players")
    bool supported = true;  // false: the system cannot play online at all (PS2, Wii U): listed greyed with `reason`
    bool untested = false;  // capable, but not in the catalog's vetted online list
    bool selectable = true; // supported AND every room member owns this exact game AND it fits the room
    bool remote = false;    // Wii U remote play: the host streams it; only the host needs the game
    bool pinned = false;    // the native Smash port: sorts first
    bool favorite = false;
    uint64_t last_played = 0;
    std::string reason;     // why it is not selectable / supported ("Mia doesn't have this game")
    bool usable() const { return supported && selectable; }
};

struct Filter {
    std::string query;   // instant search: every word must appear in the title / console name / short name
    std::string sys;     // console chip ("" = all)
};

struct Section {
    std::string key;     // "recent" | "favorites" | "everyone" | "results" | "sys:<short>"
    std::string title;
    std::vector<int> items;   // indexes into the Item vector
};

inline std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

/* "1 player", "1-4 players" (a 1-1 range reads as one player). */
inline std::string players_text(int mn, int mx) {
    if (mx < 1) mx = 1;
    if (mn < 1) mn = 1;
    if (mn > mx) mn = mx;
    if (mx == 1) return "1 player";
    if (mn == mx) return std::to_string(mx) + " players";
    return std::to_string(mn) + "-" + std::to_string(mx) + " players";
}

inline bool matches_query(const Item& it, const std::string& query) {
    std::string hay = lower(it.title + " " + it.sys_name + " " + it.sys);
    size_t i = 0;
    while (i < query.size()) {
        while (i < query.size() && std::isspace((unsigned char)query[i])) ++i;
        size_t j = i;
        while (j < query.size() && !std::isspace((unsigned char)query[j])) ++j;
        if (j > i && hay.find(lower(query.substr(i, j - i))) == std::string::npos) return false;
        i = j;
    }
    return true;
}

inline bool passes(const Item& it, const Filter& f) {
    if (!f.sys.empty() && it.sys != f.sys) return false;
    return f.query.empty() || matches_query(it, f.query);
}

/* Title order: the native port first, then case-insensitive title, then id (stable for duplicate titles). */
inline bool title_less(const Item& a, const Item& b) {
    if (a.pinned != b.pinned) return a.pinned;
    std::string x = lower(a.title), y = lower(b.title);
    if (x != y) return x < y;
    return a.id < b.id;
}

struct Chip { std::string sys, name; int count = 0; };

/* Console filter chips: every console in the library (counts honour the search text, not the chip), sorted by name. */
inline std::vector<Chip> chips(const std::vector<Item>& items, const std::string& query) {
    std::vector<Chip> out;
    for (const Item& it : items) {
        if (!query.empty() && !matches_query(it, query)) continue;
        auto f = std::find_if(out.begin(), out.end(), [&](const Chip& c) { return c.sys == it.sys; });
        if (f == out.end()) { out.push_back({it.sys, it.sys_name, 1}); }
        else ++f->count;
    }
    std::sort(out.begin(), out.end(), [](const Chip& a, const Chip& b) { return lower(a.name) != lower(b.name) ? lower(a.name) < lower(b.name) : a.sys < b.sys; });
    return out;
}

/* Sections for the grid.
 * Searching: one "Results" section (usable games first, then A-Z) - the shelves would only repeat each game.
 * Otherwise: Recently played (usable, newest first, capped), Favorites (supported), Everyone has it (usable), then one section per
 * console with every game (usable or not). The console chip narrows all of them. Empty sections are dropped. */
inline std::vector<Section> build_sections(const std::vector<Item>& items, const Filter& f, size_t recent_cap = 8) {
    std::vector<int> idx;
    for (size_t i = 0; i < items.size(); ++i) if (passes(items[i], f)) idx.push_back((int)i);
    std::sort(idx.begin(), idx.end(), [&](int a, int b) { return title_less(items[a], items[b]); });
    std::vector<Section> out;
    if (idx.empty()) return out;
    if (!f.query.empty()) {
        Section r{"results", "Results", {}};
        for (int i : idx) if (items[i].usable()) r.items.push_back(i);
        for (int i : idx) if (!items[i].usable()) r.items.push_back(i);
        out.push_back(r);
        return out;
    }
    Section rec{"recent", "Recently played", {}}, fav{"favorites", "Favorites", {}}, every{"everyone", "Everyone has it", {}};
    for (int i : idx) {
        const Item& it = items[i];
        if (it.usable()) every.items.push_back(i);
        if (it.supported && it.favorite) fav.items.push_back(i);
        if (it.usable() && it.last_played) rec.items.push_back(i);
    }
    std::stable_sort(rec.items.begin(), rec.items.end(), [&](int a, int b) { return items[a].last_played > items[b].last_played; });
    if (rec.items.size() > recent_cap) rec.items.resize(recent_cap);
    for (Section* s : {&rec, &fav, &every}) if (!s->items.empty()) out.push_back(*s);
    std::vector<Chip> cs = chips(items, f.query);
    for (const Chip& c : cs) {
        if (!f.sys.empty() && c.sys != f.sys) continue;
        Section s{"sys:" + c.sys, c.name, {}};
        for (int i : idx) if (items[i].sys == c.sys) s.items.push_back(i);
        if (!s.items.empty()) out.push_back(s);
    }
    return out;
}

/* ---- virtualised layout: a flat list of header rows and tile rows with prefix heights ---- */
struct Row {
    int section = 0;
    bool header = false;
    int first = 0, count = 0;   // tile rows: indexes into Section::items [first, first+count)
};
struct Layout {
    std::vector<Row> rows;
    std::vector<float> y;   // top of each row; y.back() == total height
    float total = 0;
};

inline Layout build_layout(const std::vector<Section>& secs, int cols, float header_h, float tile_row_h) {
    Layout L;
    if (cols < 1) cols = 1;
    float yy = 0;
    for (size_t s = 0; s < secs.size(); ++s) {
        L.rows.push_back({(int)s, true, 0, 0});
        L.y.push_back(yy);
        yy += header_h;
        int n = (int)secs[s].items.size();
        for (int a = 0; a < n; a += cols) {
            L.rows.push_back({(int)s, false, a, std::min(cols, n - a)});
            L.y.push_back(yy);
            yy += tile_row_h;
        }
    }
    L.y.push_back(yy);
    L.total = yy;
    return L;
}

/* Rows intersecting [scroll, scroll+view) widened by `margin` rows each side (so pad navigation always has a neighbour submitted).
 * Returns [first, last) row indexes. */
inline void visible_rows(const Layout& L, float scroll, float view, int margin, int* first, int* last) {
    int n = (int)L.rows.size();
    if (n == 0) { *first = *last = 0; return; }
    auto at = [&](float yv) {   // row containing yv
        int lo = 0, hi = n - 1;
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;
            if (L.y[mid] <= yv) lo = mid; else hi = mid - 1;
        }
        return lo;
    };
    int a = at(std::max(0.f, scroll)), b = at(std::max(0.f, scroll + view));
    *first = std::max(0, a - margin);
    *last = std::min(n, b + 1 + margin);
}

/* Row index (in the layout) of item `item_pos` of section `sec`'s tile rows, or -1. */
inline int row_of(const Layout& L, int sec, int item_pos) {
    for (size_t r = 0; r < L.rows.size(); ++r)
        if (L.rows[r].section == sec && !L.rows[r].header && item_pos >= L.rows[r].first && item_pos < L.rows[r].first + L.rows[r].count) return (int)r;
    return -1;
}

}  // namespace hubpick
