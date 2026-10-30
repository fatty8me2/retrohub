#pragma once
/* Friends' games shown greyed out in your library (pure logic, header-only; tests: test_friend_games.cpp).
 * Metadata only: a "ghost" is a game one or more friends own and you do not. Nothing here touches files. */

#include "library_share.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace hubui {
namespace fg {

/* Title with region/revision tags removed, lower case, punctuation folded: "Mario Kart 64 (USA) (Rev 1)" and
 * "Mario Kart 64 (Europe)" both give "mario kart 64". A leading "the " is dropped. */
inline std::string norm_title(const std::string& t)
{
    std::string o;
    int depth = 0;
    for (char ch : t) {
        if (ch == '(' || ch == '[') { ++depth; continue; }
        if (ch == ')' || ch == ']') { if (depth > 0) --depth; continue; }
        if (depth) continue;
        unsigned char c = (unsigned char)ch;
        if (std::isalnum(c)) o += (char)std::tolower(c);
        else if (c >= 0x80) o += ch;
        else if (!o.empty() && o.back() != ' ') o += ' ';
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    if (o.compare(0, 4, "the ") == 0) o.erase(0, 4);
    return o;
}

/* Identity of a game across regions: the catalog id when there is one, else the normalised title; always per system. */
inline std::string identity(const std::string& sys, const std::string& cid, const std::string& title)
{
    if (!cid.empty()) return sys + "|c:" + cid;
    return sys + "|t:" + norm_title(title);
}

struct Owned { std::string sys, key, cid, title; };

struct Ghost {
    std::string id;                    // identity()
    std::string sys, cid, key, title;  // as published by the first friend that owns it
    std::vector<std::string> uids;     // friends that own it (in friend order)
};

constexpr size_t kMaxGhosts = 5000;

/* Friends' games you do not own. `libs` = (friend uid, that friend's list) in friend order (callers drop blocked/removed
 * people). A game is yours when its identity or its key matches one of yours. Result sorted by title (case-insensitive),
 * then identity. Entries with an empty identity title are skipped. */
inline std::vector<Ghost> build_ghosts(const std::vector<Owned>& mine,
                                       const std::vector<std::pair<std::string, const std::vector<hubnet::LibGame>*>>& libs)
{
    std::set<std::string> have_id, have_key;
    for (const Owned& o : mine) { have_id.insert(identity(o.sys, o.cid, o.title)); have_id.insert(identity(o.sys, "", o.title)); have_key.insert(o.sys + "|" + o.key); }
    std::map<std::string, Ghost> by_id;
    for (const auto& fl : libs) {
        if (!fl.second) continue;
        for (const hubnet::LibGame& g : *fl.second) {
            if (g.sys.empty() || g.title.empty()) continue;
            std::string id = identity(g.sys, g.cid, g.title);
            if (have_id.count(id) || have_id.count(identity(g.sys, "", g.title)) || have_key.count(g.sys + "|" + g.key)) continue;
            auto it = by_id.find(id);
            if (it == by_id.end()) {
                if (by_id.size() >= kMaxGhosts) continue;
                Ghost n; n.id = id; n.sys = g.sys; n.cid = g.cid; n.key = g.key; n.title = g.title;
                it = by_id.emplace(id, std::move(n)).first;
            }
            auto& u = it->second.uids;
            if (std::find(u.begin(), u.end(), fl.first) == u.end()) u.push_back(fl.first);
        }
    }
    // a friend with a catalogued copy and another with an uncatalogued one of the same title are the same game: merge into the catalogued one
    for (auto it = by_id.begin(); it != by_id.end();) {
        if (it->second.cid.empty()) {
            Ghost* tgt = nullptr;
            for (auto& kv : by_id)
                if (!kv.second.cid.empty() && kv.second.sys == it->second.sys && norm_title(kv.second.title) == norm_title(it->second.title)) { tgt = &kv.second; break; }
            if (tgt) {
                for (auto& u : it->second.uids) if (std::find(tgt->uids.begin(), tgt->uids.end(), u) == tgt->uids.end()) tgt->uids.push_back(u);
                it = by_id.erase(it);
                continue;
            }
        }
        ++it;
    }
    std::vector<Ghost> out;
    out.reserve(by_id.size());
    for (auto& kv : by_id) out.push_back(std::move(kv.second));
    auto low = [](std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; };
    std::sort(out.begin(), out.end(), [&](const Ghost& a, const Ghost& b) {
        std::string x = low(a.title), y = low(b.title);
        return x != y ? x < y : a.id < b.id;
    });
    return out;
}

/* "Mickey has this", "Mickey, camer have this", "Mickey, camer and 2 others have this". `max_names` names are listed. */
inline std::string badge_text(const std::vector<std::string>& names, size_t max_names = 2)
{
    if (names.empty()) return "In a shared pack";   // a pack-only ghost has no owners
    if (names.size() == 1) return names[0] + " has this";
    std::string o;
    size_t shown = std::min(names.size(), max_names);
    for (size_t i = 0; i < shown; ++i) o += (i ? ", " : "") + names[i];
    if (names.size() > shown) {
        size_t more = names.size() - shown;
        o += " and " + std::to_string(more) + (more == 1 ? " other" : " others");
    }
    return o + " have this";
}

/* Diagnostics line for Settings > Online > Friends: how many friends' shared game lists this PC holds. */
inline std::string lists_held_text(size_t held, size_t friends)
{
    return "Game lists from friends: " + std::to_string(held) + " of " + std::to_string(friends);
}

/* Why the ghost list is empty ("" when it is not). Written to the hub log when the ghosts are rebuilt. */
inline std::string empty_reason(bool setting_on, size_t friends, size_t held, size_t ghosts)
{
    if (ghosts > 0) return "";
    if (!setting_on) return "setting off";
    if (friends == 0) return "no friends";
    if (held == 0) return "no lists held";
    return "friends own nothing you do not have";
}

/* The library snapshot to publish: everything except `skip_key` (the native Smash entry). */
inline std::vector<hubnet::LibGame> publish_list(const std::vector<Owned>& mine, const std::string& skip_key = "")
{
    std::vector<hubnet::LibGame> v;
    for (const Owned& o : mine) {
        if (!skip_key.empty() && o.key == skip_key) continue;
        v.push_back({o.sys, o.key, o.cid, o.title});
    }
    return hubnet::lib_normalize(std::move(v));
}

/* Debounce for publishing: the owned-library signature must stay unchanged for `quiet_ms` before the list is sent, and an
 * unchanged signature never sends twice. */
struct PublishGate {
    std::string sig;           // last observed signature
    long long changed_at = 0;  // when it was first observed
    std::string sent_sig;      // signature already handed to the client
    bool poll(const std::string& now_sig, long long now_ms, long long quiet_ms = 3000)
    {
        if (now_sig != sig) { sig = now_sig; changed_at = now_ms; }
        if (sig == sent_sig || now_ms - changed_at < quiet_ms) return false;
        sent_sig = sig;
        return true;
    }
};

} // namespace fg
} // namespace hubui
