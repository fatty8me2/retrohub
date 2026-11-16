#pragma once
/* Library desk (Settings > General > Layout: "Library desk"): the model of the game list in the left column.
 * Pure (no ImGui, no app state): groups, search, open / closed groups, keyboard / controller movement over the rows, and the
 * virtual-row maths. Unit-tested in test_desk.cpp (hub_desk_test). The drawing is desk_view.cpp.
 *
 * Groups, in order: "Recent" (the most recently played games, up to 5), "Favorites" (when there are any), then one group per console
 * in hubui::console_order(), and "other" for a console id that is not in that list. A game can be in Recent, in Favorites and in its
 * console group at once. Hidden games are never listed. A search keeps the games whose title contains the text (any case) and opens
 * every group that has a match; empty groups are left out. */

#include "logic.h"   // hubui::console_order()

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace hubdesk {

/* ---- the setting (hub.json "ui_layout") ---- */
constexpr const char* kTraditional = "traditional";   // the default: a missing or unknown value reads as Traditional
constexpr const char* kDesk = "desk";
inline bool is_desk(const std::string& v) { return v == kDesk; }
inline std::string normalize_layout(const std::string& v) { return v == kDesk ? std::string(kDesk) : std::string(kTraditional); }

/* ---- input ---- */
struct Game {
    std::string key, title, console;   // console: library.h console id (a system short, or "switch2")
    unsigned long long last_played = 0;
    bool favorite = false, hidden = false, pinned = false;   // pinned: the bundled native port, first in its console group
};

constexpr int kMaxRecent = 5;
constexpr int kBigLibrary = 40;   // more games than this: only Recent and the first console group start open

/* ---- open / closed groups (hub.json "desk_groups": "recent=1,n64=0,gc=1") ---- */
using OpenMap = std::map<std::string, bool>;

inline OpenMap parse_open(const std::string& s) {
    OpenMap m;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(',', i);
        if (j == std::string::npos) j = s.size();
        const std::string tok = s.substr(i, j - i);
        const size_t eq = tok.find('=');
        if (eq != std::string::npos && eq > 0 && eq + 2 == tok.size() && (tok[eq + 1] == '0' || tok[eq + 1] == '1'))
            m[tok.substr(0, eq)] = tok[eq + 1] == '1';
        i = j + 1;
    }
    return m;
}
inline std::string format_open(const OpenMap& m) {
    std::string o;
    for (const auto& kv : m) {
        if (kv.first.empty() || kv.first.find_first_of(",=") != std::string::npos) continue;
        if (!o.empty()) o += ',';
        o += kv.first + (kv.second ? "=1" : "=0");
    }
    return o;
}
/* Before the player opened or closed a group: everything open in a small library; in a big one only Recent and the first console. */
inline bool default_open(const std::string& id, const std::string& first_console, int total_games, int big = kBigLibrary) {
    if (total_games <= big) return true;
    return id == "recent" || (!first_console.empty() && id == first_console);
}

/* ---- the model ---- */
enum class GroupKind { Recent, Favorites, Console };
struct Group {
    std::string id;           // "recent", "favs", a console id, or "other"
    GroupKind kind = GroupKind::Console;
    std::vector<int> items;   // indices into the games vector, in display order (the count shown is items.size())
    bool open = true;
};
struct Row {
    int group = -1;
    int game = -1;   // index into the games vector; -1 = the group's header row
    bool header() const { return game < 0; }
};
struct Model {
    std::vector<Group> groups;
    std::vector<Row> rows;        // the visible rows: each group's header, then its games when it is open
    bool searching = false;
    int total = 0;                // games in the library (hidden ones not counted)
    int matches = 0;              // games that match the search (each once); == total without a search
    std::string first_console;    // id of the first console group of the whole library (the default-open one)
};

inline std::string lower_ascii(const std::string& s) {
    std::string o = s;
    for (char& c : o) c = (char)std::tolower((unsigned char)c);
    return o;
}
inline std::string trim_query(const std::string& q) {
    const size_t b = q.find_first_not_of(" \t");
    if (b == std::string::npos) return std::string();
    const size_t e = q.find_last_not_of(" \t");
    return q.substr(b, e - b + 1);
}
inline bool title_matches(const std::string& title, const std::string& lower_query) {
    return lower_query.empty() || lower_ascii(title).find(lower_query) != std::string::npos;
}

/* Which console group a console id goes in: itself when it is a known console, else "other". */
inline std::string console_group_id(const std::string& console) {
    const std::vector<std::string>& o = hubui::console_order();
    return std::find(o.begin(), o.end(), console) != o.end() ? console : std::string("other");
}

inline void flatten(Model* m) {
    m->rows.clear();
    for (size_t g = 0; g < m->groups.size(); ++g) {
        m->rows.push_back(Row{(int)g, -1});
        if (m->groups[g].open)
            for (int i : m->groups[g].items) m->rows.push_back(Row{(int)g, i});
    }
}

/* search_closed: groups the player closed during the current search (a search opens every group with a match; the caller clears the
 * set when the search text changes). Never persisted. */
inline Model build(const std::vector<Game>& games, const std::string& query, const OpenMap& open,
                   const std::set<std::string>& search_closed = std::set<std::string>(), int max_recent = kMaxRecent, int big = kBigLibrary) {
    Model m;
    const std::string q = lower_ascii(trim_query(query));
    m.searching = !q.empty();
    std::vector<int> keep;   // matching, not hidden
    for (size_t i = 0; i < games.size(); ++i) {
        if (games[i].hidden) continue;
        ++m.total;
        if (title_matches(games[i].title, q)) keep.push_back((int)i);
    }
    m.matches = (int)keep.size();
    auto by_title = [&](int a, int b) {
        if (games[a].pinned != games[b].pinned) return games[a].pinned;
        const std::string x = lower_ascii(games[a].title), y = lower_ascii(games[b].title);
        if (x != y) return x < y;
        return games[a].key < games[b].key;
    };
    {   // the first console group of the WHOLE library (a search must not move the default)
        int best = -1;
        bool other = false;
        const std::vector<std::string>& o = hubui::console_order();
        for (const Game& g : games) {
            if (g.hidden) continue;
            auto it = std::find(o.begin(), o.end(), g.console);
            if (it == o.end()) { other = true; continue; }
            const int r = (int)(it - o.begin());
            if (best < 0 || r < best) best = r;
        }
        m.first_console = best >= 0 ? o[(size_t)best] : (other ? std::string("other") : std::string());
    }
    auto add = [&](const std::string& id, GroupKind kind, std::vector<int> items) {
        if (items.empty()) return;
        Group g;
        g.id = id;
        g.kind = kind;
        g.items = std::move(items);
        if (m.searching) g.open = search_closed.count(id) == 0;
        else {
            auto it = open.find(id);
            g.open = it != open.end() ? it->second : default_open(id, m.first_console, m.total, big);
        }
        m.groups.push_back(std::move(g));
    };
    {   // Recent: newest first
        std::vector<int> r;
        for (int i : keep) if (games[i].last_played > 0) r.push_back(i);
        std::stable_sort(r.begin(), r.end(), [&](int a, int b) {
            if (games[a].last_played != games[b].last_played) return games[a].last_played > games[b].last_played;
            return by_title(a, b);
        });
        if ((int)r.size() > max_recent) r.resize((size_t)std::max(0, max_recent));
        add("recent", GroupKind::Recent, r);
    }
    {
        std::vector<int> f;
        for (int i : keep) if (games[i].favorite) f.push_back(i);
        std::stable_sort(f.begin(), f.end(), by_title);
        add("favs", GroupKind::Favorites, f);
    }
    std::map<std::string, std::vector<int>> by_console;
    for (int i : keep) by_console[console_group_id(games[i].console)].push_back(i);
    for (const std::string& c : hubui::console_order()) {
        auto it = by_console.find(c);
        if (it == by_console.end()) continue;
        std::stable_sort(it->second.begin(), it->second.end(), by_title);
        add(c, GroupKind::Console, it->second);
    }
    if (by_console.count("other")) {
        std::vector<int>& v = by_console["other"];
        std::stable_sort(v.begin(), v.end(), by_title);
        add("other", GroupKind::Console, v);
    }
    flatten(&m);
    return m;
}

/* ---- rows: identity across rebuilds (a rescan, a search, a favourite toggled) ---- */
struct RowRef {
    std::string group, key;   // key "" = the group's header
    bool empty() const { return group.empty(); }
};
inline RowRef ref_of(const Model& m, const std::vector<Game>& games, int row) {
    if (row < 0 || row >= (int)m.rows.size()) return RowRef();
    const Row& r = m.rows[(size_t)row];
    RowRef ref;
    ref.group = m.groups[(size_t)r.group].id;
    if (!r.header()) ref.key = games[(size_t)r.game].key;
    return ref;
}
/* First visible row of a game (any group); -1 when it is not visible. */
inline int row_of_key(const Model& m, const std::vector<Game>& games, const std::string& key) {
    if (key.empty()) return -1;
    for (size_t i = 0; i < m.rows.size(); ++i)
        if (!m.rows[i].header() && games[(size_t)m.rows[i].game].key == key) return (int)i;
    return -1;
}
inline int header_row(const Model& m, const std::string& group) {
    for (size_t i = 0; i < m.rows.size(); ++i)
        if (m.rows[i].header() && m.groups[(size_t)m.rows[i].group].id == group) return (int)i;
    return -1;
}
/* The same row again: the same game in the same group; else that game in any group; else the group's header; else -1. */
inline int find_row(const Model& m, const std::vector<Game>& games, const RowRef& ref) {
    if (ref.empty()) return -1;
    for (size_t i = 0; i < m.rows.size(); ++i) {
        const Row& r = m.rows[i];
        if (m.groups[(size_t)r.group].id != ref.group) continue;
        if (ref.key.empty() ? r.header() : (!r.header() && games[(size_t)r.game].key == ref.key)) return (int)i;
    }
    if (!ref.key.empty()) {
        const int k = row_of_key(m, games, ref.key);
        if (k >= 0) return k;
    }
    return header_row(m, ref.group);
}
/* Is the game listed in any group (open or closed)? */
inline bool has_game(const Model& m, const std::vector<Game>& games, const std::string& key) {
    for (const Group& g : m.groups)
        for (int i : g.items) if (games[(size_t)i].key == key) return true;
    return false;
}
inline int first_game_row(const Model& m) {
    for (size_t i = 0; i < m.rows.size(); ++i) if (!m.rows[i].header()) return (int)i;
    return -1;
}

/* ---- keyboard / controller movement over the visible rows ---- */
enum class Key { Up, Down, Left, Right, PageUp, PageDown, Home, End, Activate };
enum class Act {
    None,
    Move,        // the cursor goes to `row`
    Open,        // open `group` (the cursor stays on its header)
    Close,       // close `group` (the cursor stays on its header)
    EnterPane,   // Right on a game: the focus goes to the game pane
    Leave,       // Left on a closed header: the focus may leave the list (to the sidebar)
    Edge,        // Up on the first row / Down on the last one: the focus may leave the list (search box / Add games)
    Play         // Activate on a game
};
struct Step {
    Act act = Act::None;
    int row = -1;
    int group = -1;
};
inline Step step(const Model& m, int cur, Key k, int page_rows) {
    Step s;
    const int n = (int)m.rows.size();
    if (n == 0) return s;
    if (cur < 0 || cur >= n) { s.act = Act::Move; s.row = cur < 0 ? 0 : n - 1; return s; }
    const Row& r = m.rows[(size_t)cur];
    const int pg = std::max(1, page_rows);
    auto move = [&](int to) { to = std::max(0, std::min(n - 1, to)); if (to != cur) { s.act = Act::Move; s.row = to; } return s; };
    switch (k) {
    case Key::Up:   if (cur == 0) { s.act = Act::Edge; s.row = cur; return s; } return move(cur - 1);
    case Key::Down: if (cur == n - 1) { s.act = Act::Edge; s.row = cur; return s; } return move(cur + 1);
    case Key::PageUp: case Key::PageDown: {   // a page lands on a game: an open group's header gives way to its first game
        int to = std::max(0, std::min(n - 1, cur + (k == Key::PageUp ? -pg : pg)));
        const int adj = to + 1;
        if (m.rows[(size_t)to].header() && m.groups[(size_t)m.rows[(size_t)to].group].open && adj < n && (k == Key::PageUp ? adj < cur : adj > cur)) to = adj;
        return move(to);
    }
    case Key::Home: return move(0);
    case Key::End: return move(n - 1);
    case Key::Left:
        if (!r.header()) { s.act = Act::Move; s.row = header_row(m, m.groups[(size_t)r.group].id); return s; }
        if (m.groups[(size_t)r.group].open) { s.act = Act::Close; s.group = r.group; s.row = cur; return s; }
        s.act = Act::Leave; s.row = cur; return s;
    case Key::Right:
        if (r.header()) {
            if (!m.groups[(size_t)r.group].open) { s.act = Act::Open; s.group = r.group; s.row = cur; return s; }
            return move(cur + 1);   // an open group always has a game right below its header
        }
        s.act = Act::EnterPane; s.row = cur; return s;
    case Key::Activate:
        if (r.header()) { s.act = m.groups[(size_t)r.group].open ? Act::Close : Act::Open; s.group = r.group; s.row = cur; return s; }
        s.act = Act::Play; s.row = cur; return s;
    }
    return s;
}

/* ---- virtual rows ---- */
struct Metrics {
    float head_h = 0, row_h = 0, gap = 0;   // gap: space above every header but the first
};
struct Layout {
    std::vector<float> top;   // y of each row (content coordinates, 0 = top of the list)
    float total = 0;          // content height
};
inline float row_height(const Row& r, const Metrics& mt) { return r.header() ? mt.head_h : mt.row_h; }
inline Layout layout_rows(const std::vector<Row>& rows, const Metrics& mt) {
    Layout l;
    l.top.reserve(rows.size());
    float y = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].header() && i > 0) y += mt.gap;
        l.top.push_back(y);
        y += row_height(rows[i], mt);
    }
    l.total = y;
    return l;
}
inline float max_scroll(float total, float view_h) { return std::max(0.f, total - view_h); }
inline float clamp_scroll(float s, float total, float view_h) { return std::max(0.f, std::min(max_scroll(total, view_h), s)); }
/* Rows that intersect [scroll, scroll + view_h): first / last index, -1 / -1 when none. */
inline void visible_rows(const Layout& l, const std::vector<Row>& rows, const Metrics& mt, float scroll, float view_h, int* first, int* last) {
    *first = *last = -1;
    const int n = (int)l.top.size();
    if (n == 0 || view_h <= 0) return;
    const float y0 = scroll, y1 = scroll + view_h;
    int lo = (int)(std::upper_bound(l.top.begin(), l.top.end(), y0) - l.top.begin()) - 1;   // last row starting at or above y0
    if (lo < 0) lo = 0;
    if (l.top[(size_t)lo] + row_height(rows[(size_t)lo], mt) <= y0) ++lo;   // it ends above the view (or y0 sits in a gap)
    if (lo >= n || l.top[(size_t)lo] >= y1) return;
    int hi = (int)(std::lower_bound(l.top.begin(), l.top.end(), y1) - l.top.begin()) - 1;   // last row starting above y1
    *first = lo;
    *last = std::max(lo, hi);
}
/* The row under content y; -1 in a gap or outside the list. */
inline int row_at(const Layout& l, const std::vector<Row>& rows, const Metrics& mt, float y) {
    if (l.top.empty() || y < 0) return -1;
    int i = (int)(std::upper_bound(l.top.begin(), l.top.end(), y) - l.top.begin()) - 1;
    if (i < 0) return -1;
    return y < l.top[(size_t)i] + row_height(rows[(size_t)i], mt) ? i : -1;
}
/* Scroll that shows the whole row with `margin` around it (the current scroll when it already does), clamped. */
inline float reveal(const Layout& l, const std::vector<Row>& rows, const Metrics& mt, int row, float scroll, float view_h, float margin) {
    if (row < 0 || row >= (int)l.top.size()) return clamp_scroll(scroll, l.total, view_h);
    const float t = l.top[(size_t)row], b = t + row_height(rows[(size_t)row], mt);
    float s = scroll;
    if (t - margin < s) s = t - margin;
    else if (b + margin > s + view_h) s = b + margin - view_h;
    return clamp_scroll(s, l.total, view_h);
}
/* Rows in a page (PageUp / PageDown, LT / RT): what fits in the view, at least 1. */
inline int page_rows(float view_h, float row_h) { return row_h > 0 ? std::max(1, (int)std::floor(view_h / row_h) - 1) : 1; }

/* ---- the scrollbar ---- */
struct Thumb { float y0 = 0, y1 = 0; bool shown = false; };
/* Thumb inside a track of track_h px (0 at the top of the track); hidden when everything fits. */
inline Thumb thumb(float scroll, float total, float view_h, float track_h, float min_thumb) {
    Thumb t;
    if (total <= view_h + 0.5f || track_h <= 0) return t;
    t.shown = true;
    const float h = std::max(std::min(min_thumb, track_h), track_h * view_h / total);
    const float ms = max_scroll(total, view_h);
    const float y = ms > 0 ? (track_h - h) * (clamp_scroll(scroll, total, view_h) / ms) : 0.f;
    t.y0 = y;
    t.y1 = y + h;
    return t;
}
/* Scroll for a thumb whose top is at thumb_y0 (dragging), clamped. */
inline float scroll_for_thumb(float thumb_y0, float thumb_h, float total, float view_h, float track_h) {
    const float room = track_h - thumb_h;
    if (room <= 0) return 0.f;
    return clamp_scroll(thumb_y0 / room * max_scroll(total, view_h), total, view_h);
}

/* ---- the column width: about a quarter of the content, clamped to 30..48 units (unit = sp(1), the 8 px step at the current scale) ---- */
inline float column_width(float content_w, float unit) {
    const float w = content_w * 0.25f, lo = 30.f * unit, hi = 48.f * unit;
    return std::max(std::min(lo, content_w * 0.5f), std::min(hi, w));
}

}  // namespace hubdesk
