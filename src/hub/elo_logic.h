#pragma once
/* Friends ladder (Plan 4 P8): an Elo rating per viewer, computed from the rivalry matches this PC stores (rivals_logic.h,
 * at most rivals::kMaxStored). Pure; unit tested in test_bracket.cpp. Nothing is published: every viewer computes their own.
 *
 * Only YOU and your friends are rated (a match against a stranger changes nobody: strangers have no uid). Start 1000, K = 32.
 * A match with more than two rated players is scored as every pair meeting once, each pair with K / (rated players - 1), so one
 * match never moves a rating by more than K. A shared rank (tie) scores 0.5 each. Matches are replayed oldest first. */
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "rivals_logic.h"

namespace hubui {
namespace elo {

constexpr double kStart = 1000.0;
constexpr double kK = 32.0;

inline double expected(double ra, double rb) { return 1.0 / (1.0 + std::pow(10.0, (rb - ra) / 400.0)); }
/* score_a: 1 win, 0.5 draw, 0 loss. */
inline void apply(double* ra, double* rb, double score_a, double k = kK) {
    const double e = expected(*ra, *rb);
    const double da = k * (score_a - e);
    *ra += da;
    *rb -= da;   // zero-sum: E_b = 1 - E_a, so B's change is exactly -da
}

struct Entry {
    std::string uid;       // "" = you
    std::string name;
    bool me = false;
    int rating = 1000;
    int games = 0;
};

/* The ladder, best first (ties by name). `ms` is newest first, as load_matches returns it. Everyone who played at least one
 * rated game appears (you always do, even with no games: 1000). */
inline std::vector<Entry> ladder(const std::vector<rivals::Match>& ms) {
    struct Acc { std::string name; double r = kStart; int games = 0; };
    std::map<std::string, Acc> acc;   // key: uid, "" = me
    acc[""].name = "You";
    for (auto it = ms.rbegin(); it != ms.rend(); ++it) {
        std::vector<const rivals::Player*> rated;
        bool has_me = false;
        for (auto& p : it->players) {
            if (p.me) { if (!has_me) rated.push_back(&p); has_me = true; }
            else if (!p.uid.empty()) rated.push_back(&p);
        }
        if (!has_me || rated.size() < 2) continue;   // nobody to compare with
        const double k = kK / (double)(rated.size() - 1);
        auto key = [](const rivals::Player* p) { return p->me ? std::string() : p->uid; };
        for (size_t i = 0; i < rated.size(); ++i)
            for (size_t j = i + 1; j < rated.size(); ++j) {
                Acc& a = acc[key(rated[i])];
                Acc& b = acc[key(rated[j])];
                if (!rated[i]->me && a.name.empty()) a.name = rated[i]->name;
                if (!rated[j]->me && b.name.empty()) b.name = rated[j]->name;
                if (!rated[i]->me && !rated[i]->name.empty()) a.name = rated[i]->name;   // newest name wins (we walk oldest to newest)
                if (!rated[j]->me && !rated[j]->name.empty()) b.name = rated[j]->name;
                const double s = rated[i]->rank < rated[j]->rank ? 1.0 : rated[i]->rank > rated[j]->rank ? 0.0 : 0.5;
                apply(&a.r, &b.r, s, k);
                ++a.games; ++b.games;
            }
    }
    std::vector<Entry> out;
    for (auto& kv : acc) {
        if (!kv.first.empty() && kv.second.games == 0) continue;
        Entry e;
        e.uid = kv.first; e.name = kv.second.name; e.me = kv.first.empty();
        e.rating = (int)std::lround(kv.second.r);
        e.games = kv.second.games;
        out.push_back(e);
    }
    std::stable_sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        if (a.rating != b.rating) return a.rating > b.rating;
        if (a.me != b.me) return a.me;   // equal rating: you first
        return a.name < b.name;
    });
    return out;
}

/* 1-based place among the rated entries, 0 when the uid ("" = you) is not on the ladder or has no games. */
inline int place_of(const std::vector<Entry>& l, const std::string& uid) {
    for (size_t i = 0; i < l.size(); ++i)
        if (l[i].uid == uid && (l[i].games > 0 || l[i].me)) return (int)i + 1;
    return 0;
}
inline const Entry* find(const std::vector<Entry>& l, const std::string& uid) {
    for (auto& e : l) if (e.uid == uid) return &e;
    return nullptr;
}
/* Rating a seeding should use for this uid: the ladder rating, or the starting 1000 for unknown people. */
inline int rating_of(const std::vector<Entry>& l, const std::string& uid) {
    const Entry* e = find(l, uid);
    return e && (e->games > 0 || e->me) ? e->rating : (int)kStart;
}
/* "1180 · #2 among friends" (separator is plain ASCII " - " when ascii is true). "" when the uid has no rated games. */
inline std::string rating_text(const std::vector<Entry>& l, const std::string& uid, bool with_place_label = true) {
    const Entry* e = find(l, uid);
    if (!e || (e->games == 0 && !(e->me && l.size() > 1))) return "";
    int pl = place_of(l, uid);
    std::string t = std::to_string(e->rating);
    if (pl) t += std::string(" \xC2\xB7 #") + std::to_string(pl) + (with_place_label ? " among friends" : "");
    return t;
}
/* Short form for a friend row: "1180 #2". */
inline std::string rating_short(const std::vector<Entry>& l, const std::string& uid) {
    const Entry* e = find(l, uid);
    if (!e || e->games == 0) return "";
    int pl = place_of(l, uid);
    return std::to_string(e->rating) + (pl ? " #" + std::to_string(pl) : std::string());
}

}  // namespace elo
}  // namespace hubui
