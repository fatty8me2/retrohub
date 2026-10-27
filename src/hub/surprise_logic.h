#pragma once
/* Surprise me (Wave 3 L7), pure logic: which owned game to suggest, and the slot-machine reel.
 *
 * Only games not played for kQuietDays (or never) are candidates. Never-played games are the likeliest, favorites count
 * more, and a game that has been waiting longer gets a little extra. If everything was played recently the pick falls back
 * to the whole library (the UI says so). A game picked last time is skipped when there is any other choice. */

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace hubui::surprise {

constexpr int64_t kQuietDays = 14;
constexpr double kNeverPlayed = 4.0;     // weight of a game never played
constexpr double kFavoriteBoost = 2.5;   // multiplier for favorites

struct Cand {
    std::string key;
    int64_t last_played = 0;   // unix seconds, 0 = never
    bool favorite = false;
};

inline bool quiet_enough(const Cand& c, int64_t now) { return c.last_played <= 0 || now - c.last_played >= kQuietDays * 86400; }

/* Weight of a candidate: 0 when it was played too recently. */
inline double weight(const Cand& c, int64_t now) {
    if (!quiet_enough(c, now)) return 0.0;
    double w;
    if (c.last_played <= 0) w = kNeverPlayed;
    else {
        const double extra_days = (double)(now - c.last_played) / 86400.0 - (double)kQuietDays;
        w = 1.0 + std::min(2.0, std::max(0.0, extra_days) / 60.0);   // up to 3x after ~4 months
    }
    if (c.favorite) w *= kFavoriteBoost;
    return w;
}

struct Pick {
    int index = -1;            // into the candidate list, -1 when there is nothing to pick
    bool all_recent = false;   // every game was played lately: this is a pick from the whole library
};

/* u: a uniform random number in [0,1). avoid: the key suggested last time ("" = none). */
inline Pick pick(const std::vector<Cand>& cands, int64_t now, double u, const std::string& avoid = std::string()) {
    Pick r;
    if (cands.empty()) return r;
    auto run = [&](bool honour_recent, bool skip_avoid) -> int {
        std::vector<double> w(cands.size(), 0.0);
        double total = 0;
        for (size_t i = 0; i < cands.size(); ++i) {
            if (skip_avoid && !avoid.empty() && cands[i].key == avoid) continue;
            double x = honour_recent ? weight(cands[i], now) : (cands[i].favorite ? kFavoriteBoost : 1.0);
            w[i] = x;
            total += x;
        }
        if (total <= 0) return -1;
        double t = std::max(0.0, std::min(0.999999999, u)) * total, acc = 0;
        int last = -1;
        for (size_t i = 0; i < cands.size(); ++i) {
            if (w[i] <= 0) continue;
            last = (int)i;
            acc += w[i];
            if (t < acc) return (int)i;
        }
        return last;
    };
    int i = run(true, true);
    if (i < 0) i = run(true, false);                            // the only quiet game is the one just shown
    if (i < 0) { i = run(false, true); r.all_recent = i >= 0; }  // nothing quiet: anything but the last pick
    if (i < 0) { i = run(false, false); r.all_recent = i >= 0; }
    r.index = i;
    return r;
}

/* ---- the reel ---- */

/* A reel of `len` covers (indices into the candidate list) that ends on `target`; no cover repeats right after itself.
 * `seed` makes it repeatable. With one candidate the reel is just that one repeated. */
inline std::vector<int> make_reel(int n_cands, int target, int len, uint32_t seed) {
    std::vector<int> r;
    if (n_cands <= 0 || len <= 0) return r;
    uint32_t s = seed * 2654435761u + 12345u;
    auto rnd = [&]() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; };
    r.reserve((size_t)len);
    target = std::max(0, std::min(n_cands - 1, target));
    for (int i = 0; i < len - 1; ++i) {
        int c = (int)(rnd() % (uint32_t)n_cands);
        const bool last_before_target = i == len - 2;   // the cover just before the target must differ from it too
        for (int tries = 0; n_cands > 1 && tries < n_cands; ++tries) {
            const bool clash = (!r.empty() && c == r.back()) || (last_before_target && c == target);
            if (!clash) break;
            c = (c + 1) % n_cands;
        }
        r.push_back(c);
    }
    r.push_back(target);
    return r;
}

/* Reel position (in cover widths from the start) at time t of a spin lasting `dur` seconds that ends exactly on the last cover.
 * Fast at first, slowing to a stop (ease-out), with a tiny overshoot-free finish. */
inline float spin_pos(double t, double dur, int reel_len) {
    if (reel_len <= 1 || dur <= 0) return 0.f;
    double u = std::max(0.0, std::min(1.0, t / dur));
    double e = 1.0 - (1.0 - u) * (1.0 - u) * (1.0 - u) * (1.0 - u);   // ease-out quartic
    return (float)(e * (double)(reel_len - 1));
}

}  // namespace hubui::surprise
