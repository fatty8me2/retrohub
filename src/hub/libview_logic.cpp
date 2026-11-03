#include "libview_logic.h"
#include "logic.h"

#include <algorithm>
#include <cctype>

namespace hubui {

const char* layout_key(LibLayout l) {
    switch (l) {
        case LibLayout::List: return "list";
        case LibLayout::Compact: return "compact";
        case LibLayout::Showcase: return "showcase";
        case LibLayout::Shelf: return "shelf";
        default: return "grid";
    }
}
const char* layout_name(LibLayout l) {
    switch (l) {
        case LibLayout::List: return "List";
        case LibLayout::Compact: return "Compact";
        case LibLayout::Showcase: return "Showcase";
        case LibLayout::Shelf: return "Shelf";
        default: return "Grid";
    }
}
LibLayout layout_from_key(const std::string& k) {
    for (int i = 0; i < kLibLayouts; ++i) if (k == layout_key((LibLayout)i)) return (LibLayout)i;
    return LibLayout::Grid;
}

std::string letter_of(const std::string& title) {
    for (unsigned char c : title) {
        if (c >= 0x80) return "#";
        if (std::isalpha(c)) return std::string(1, (char)std::toupper(c));
        if (std::isdigit(c)) return "#";
    }
    return title.empty() ? std::string() : std::string("#");
}

std::vector<JumpRun> build_runs(const std::vector<std::string>& labels) {
    std::vector<JumpRun> r;
    for (size_t i = 0; i < labels.size(); ++i)
        if (r.empty() || r.back().label != labels[i]) r.push_back({labels[i], (int)i});
    return r;
}

std::vector<JumpRun> strip_marks(const std::vector<JumpRun>& runs) {
    std::vector<JumpRun> m;
    for (const JumpRun& r : runs) {
        if (r.label.empty()) continue;
        bool seen = false;
        for (const JumpRun& x : m) if (x.label == r.label) { seen = true; break; }
        if (!seen) m.push_back(r);
    }
    return m;
}

int run_of(const std::vector<JumpRun>& runs, int idx) {
    int best = -1;
    for (size_t i = 0; i < runs.size(); ++i) {
        if (runs[i].first <= idx) best = (int)i;
        else break;
    }
    return best;
}

int jump_target(const std::vector<JumpRun>& runs, int cur, int dir) {
    int r = run_of(runs, cur);
    if (runs.empty()) return -1;
    if (dir > 0) {
        for (size_t i = (size_t)(r + 1); i < runs.size(); ++i) if (!runs[i].label.empty()) return runs[i].first;
        return -1;
    }
    if (r >= 0 && !runs[r].label.empty() && cur > runs[r].first) return runs[r].first;
    for (int i = r - 1; i >= 0; --i) if (!runs[i].label.empty()) return runs[i].first;
    return -1;
}

int strip_slot(float y, float h, int n) {
    if (n <= 0 || h <= 0.f) return -1;
    int s = (int)(y / h * (float)n);
    return std::max(0, std::min(n - 1, s));
}

std::vector<int> strip_targets(const std::vector<JumpRun>& marks) {
    static const char* az = "ABCDEFGHIJKLMNOPQRSTUVWXYZ#";
    std::vector<int> t;
    for (const char* p = az; *p; ++p) {
        std::string l(1, *p);
        int hit = -1;
        for (size_t i = 0; i < marks.size(); ++i) {
            // marks sort by first appearance; the first mark whose label is >= this letter (# last)
            std::string m = marks[i].label;
            auto key = [](const std::string& s) { return s == "#" ? std::string("~") : s; };
            if (key(m) >= key(l)) { if (hit < 0 || key(marks[i].label) < key(marks[hit].label)) hit = (int)i; }
        }
        t.push_back(hit);
    }
    return t;
}

const char* col_key(ListCol c) {
    switch (c) {
        case ListCol::Title: return "title";
        case ListCol::Console: return "console";
        case ListCol::Players: return "players";
        case ListCol::Last: return "last";
        case ListCol::Time: return "time";
        default: return "";
    }
}
ListCol col_from_key(const std::string& k) {
    for (int i = 1; i <= 5; ++i) if (k == col_key((ListCol)i)) return (ListCol)i;
    return ListCol::None;
}
const char* col_name(ListCol c) {
    switch (c) {
        case ListCol::Title: return "Title";
        case ListCol::Console: return "Console";
        case ListCol::Players: return "Players";
        case ListCol::Last: return "Last played";
        case ListCol::Time: return "Play time";
        default: return "";
    }
}
bool col_default_desc(ListCol c) { return c == ListCol::Players || c == ListCol::Last || c == ListCol::Time; }
void col_click(ListCol cur, bool cur_desc, ListCol clicked, ListCol* out, bool* out_desc) {
    if (clicked == cur && cur != ListCol::None) { *out = cur; *out_desc = !cur_desc; }
    else { *out = clicked; *out_desc = col_default_desc(clicked); }
}

static int console_rank(const std::string& s) {
    const std::vector<std::string>& o = console_order();
    auto it = std::find(o.begin(), o.end(), s);
    return it == o.end() ? (int)o.size() : (int)(it - o.begin());
}
static std::string lc(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

std::vector<int> sort_list_rows(const std::vector<ListRow>& rows, const std::vector<int>* idx, ListCol col, bool desc) {
    std::vector<int> v;
    if (idx) v = *idx;
    else for (size_t i = 0; i < rows.size(); ++i) v.push_back((int)i);
    if (col == ListCol::None) return v;
    std::stable_sort(v.begin(), v.end(), [&](int a, int b) {
        const ListRow &x = rows[a], &y = rows[b];
        if (x.pinned != y.pinned) return x.pinned;
        if (x.ghost != y.ghost) return y.ghost;
        auto by_title = [&](bool d) {
            std::string lx = lc(x.title), ly = lc(y.title);
            if (lx != ly) return d ? lx > ly : lx < ly;
            return x.key < y.key;
        };
        auto num = [&](unsigned long long p, unsigned long long q) -> int {   // -1 x first, 1 y first, 0 tie; zero always last
            if (p == q) return 0;
            if (p == 0) return 1;
            if (q == 0) return -1;
            return (desc ? p > q : p < q) ? -1 : 1;
        };
        int r = 0;
        switch (col) {
            case ListCol::Title: return by_title(desc);
            case ListCol::Console: {
                int ra = console_rank(x.system), rb = console_rank(y.system);
                if (ra != rb) return desc ? ra > rb : ra < rb;
                return by_title(false);
            }
            case ListCol::Players:
                if (x.players != y.players) return desc ? x.players > y.players : x.players < y.players;
                return by_title(false);
            case ListCol::Last: r = num(x.last_played, y.last_played); break;
            case ListCol::Time: r = num(x.play_seconds, y.play_seconds); break;
            default: break;
        }
        if (r != 0) return r < 0;
        return by_title(false);
    });
    return v;
}

ListCols list_columns(float w, float u) {
    ListCols c;
    c.icons_w = 7.f * u;
    c.console_w = 8.f * u;
    c.players_w = 6.f * u;
    c.last_w = 10.f * u;
    c.time_w = 8.f * u;
    const float thumb = 5.f * u, min_title = 16.f * u;
    auto fits = [&]() {
        float need = thumb + min_title + c.icons_w + (c.console ? c.console_w : 0) + (c.players ? c.players_w : 0) +
                     (c.last ? c.last_w : 0) + (c.time ? c.time_w : 0);
        return need <= w;
    };
    if (!fits()) c.time = false;
    if (!fits()) c.last = false;
    if (!fits()) c.players = false;
    if (!fits()) c.console = false;
    float x = w - c.icons_w;
    c.icons_x = x;
    if (c.time) { x -= c.time_w; c.time_x = x; }
    if (c.last) { x -= c.last_w; c.last_x = x; }
    if (c.players) { x -= c.players_w; c.players_x = x; }
    if (c.console) { x -= c.console_w; c.console_x = x; }
    c.thumb_x = 0;
    c.title_x = thumb;
    c.title_w = std::max(0.f, x - thumb - u);
    return c;
}

VLayout build_vlayout(const std::vector<int>& sec_sizes, const std::vector<bool>& has_head, int cols, float item_h, float head_h, float gap, float sec_gap) {
    VLayout l;
    l.cols = std::max(1, cols);
    float y = 0;
    int pos = 0;
    for (size_t s = 0; s < sec_sizes.size(); ++s) {
        int n = sec_sizes[s];
        if (n <= 0) continue;
        if (s < has_head.size() && has_head[s]) {
            l.rows.push_back({0, y, head_h, (int)s, 0});
            y += head_h;
        }
        for (int i = 0; i < n; i += l.cols) {
            int cnt = std::min(l.cols, n - i);
            l.rows.push_back({1, y, item_h, pos + i, cnt});
            for (int k = 0; k < cnt; ++k) l.item_row.push_back((int)l.rows.size() - 1);
            y += item_h + gap;
        }
        y += sec_gap;
        pos += n;
    }
    l.total_h = y;
    return l;
}

void rows_in_range(const VLayout& l, float y0, float y1, int* first, int* last) {
    *first = *last = -1;
    if (l.rows.empty()) return;
    int lo = 0, hi = (int)l.rows.size();
    while (lo < hi) {   // first row whose bottom is below y0
        int m = (lo + hi) / 2;
        if (l.rows[m].y + l.rows[m].h < y0) lo = m + 1; else hi = m;
    }
    if (lo >= (int)l.rows.size() || l.rows[lo].y > y1) return;
    int e = lo;
    while (e + 1 < (int)l.rows.size() && l.rows[e + 1].y <= y1) ++e;
    *first = lo;
    *last = e;
}

float scroll_to_row(const VLayout& l, int r, float cur, float view_h, float margin, float max_scroll) {
    if (r < 0 || r >= (int)l.rows.size()) return cur;
    float top = l.rows[r].y, bot = top + l.rows[r].h;
    float s = cur;
    if (top - margin < s) s = top - margin;
    else if (bot + margin > s + view_h) s = bot + margin - view_h;
    return std::max(0.f, std::min(max_scroll, s));
}

}  // namespace hubui
