#pragma once
/* Command palette / type-to-search: fuzzy scoring and ranking. Pure. */
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace hubpal {

enum class Kind { Action = 0, Setting, Game, Friend };

struct Item {
    Kind kind = Kind::Action;
    std::string title, sub, id;
};

inline std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }

/* 0 = no match. Higher = better: exact > prefix > word prefix > substring > subsequence (gaps cost). */
inline int score(const std::string& query, const std::string& text) {
    std::string q = lower(query), t = lower(text);
    while (!q.empty() && q.front() == ' ') q.erase(q.begin());
    while (!q.empty() && q.back() == ' ') q.pop_back();
    if (q.empty()) return 1;
    if (t == q) return 1000;
    if (t.compare(0, q.size(), q) == 0) return 800 - (int)std::min<size_t>(t.size() - q.size(), 100);
    size_t pos = t.find(q);
    if (pos != std::string::npos) {
        bool word = pos == 0 || t[pos - 1] == ' ' || t[pos - 1] == '-' || t[pos - 1] == ':';
        return (word ? 600 : 400) - (int)std::min<size_t>(pos, 100);
    }
    size_t ti = 0; int gaps = 0, last = -1;   // subsequence
    for (char c : q) {
        if (c == ' ') continue;
        size_t f = t.find(c, ti);
        if (f == std::string::npos) return 0;
        if (last >= 0 && (int)f != last + 1) ++gaps;
        last = (int)f; ti = f + 1;
    }
    return std::max(1, 200 - gaps * 15 - (int)std::min<size_t>(t.size(), 60));
}

inline int kind_bias(Kind k) { return k == Kind::Action ? 30 : k == Kind::Setting ? 15 : k == Kind::Friend ? 5 : 0; }

/* Indices into `items`, best first, at most `limit`. Empty query keeps actions then settings (the list a first Ctrl+K shows). */
inline std::vector<int> rank(const std::string& query, const std::vector<Item>& items, size_t limit = 40) {
    std::vector<std::pair<int, int>> v;
    for (size_t i = 0; i < items.size(); ++i) {
        int s = score(query, items[i].title);
        if (s == 0 && !items[i].sub.empty()) s = score(query, items[i].sub) / 4;
        if (s == 0) continue;
        v.push_back({s + kind_bias(items[i].kind), (int)i});
    }
    std::stable_sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<int> out;
    for (auto& p : v) { if (out.size() >= limit) break; out.push_back(p.second); }
    return out;
}

/* Typed characters that start a library search from Home / All Games. F, I and S stay the Favorite / Details / Shelf shortcuts. */
inline bool starts_search(unsigned c) {
    if (c < 33 || c > 126) return false;
    return !(c == 'f' || c == 'F' || c == 'i' || c == 'I' || c == 's' || c == 'S');
}

}  // namespace hubpal
