#pragma once
/* Party nights history (Plan 4 P1): one small JSON per finished night, so Wrapped (and the Home stats) can count party nights.
 * Pure: no ImGui, no network. Unit tested in test_party.cpp (scratch dirs only).
 *
 * Same storage idea as rivalries (rivals_logic.h): one file per night, named after the night's id, in BOTH the user-data dir and
 * the vault, so two PCs never write the same file and a night both PCs saw has the same id (union by id):
 *     <dir>/party/<id>.json          (vault: <vault>/_account/party/<id>.json)
 * id = "<room code>-<seed>" (a room) or "couch-<seed>" (a couch party). Every PC that sees the night end saves its own copy.
 *
 * Hooks other lanes need (not done here, see the lane report): the cloud engine syncing _account/party (it lists
 * _account/rivals only) and Wrapped reading count_nights()/my_wins(). */
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "group_logic.h"
#include "json.h"
#include "party_logic.h"
#include "rivals_logic.h"

namespace hubui {
namespace party {

struct NightRow {
    std::string name;
    int points = 0, firsts = 0, rank = 0;
    bool me = false;
};

struct Night {
    std::string id;
    int64_t at = 0;                    // ms since epoch when the night ended
    bool couch = false;
    int rounds = 0, played = 0;        // rounds planned / rounds actually played
    std::vector<std::string> games;    // game ids, in play order
    std::vector<NightRow> rows;        // the podium, best first
};

constexpr size_t kMaxNights = 200;    // per store

inline std::string night_id(const std::string& room_code, uint32_t seed, bool couch) {
    return (couch || room_code.empty() ? std::string("couch") : room_code) + "-" + std::to_string(seed);
}

/* `my_slot` marks the row that is this PC's player (-1 = none, e.g. a couch party). */
inline Night night_from_state(const State& s, const std::string& id, int64_t at_ms, int my_slot) {
    Night n;
    n.id = id;
    n.at = at_ms;
    n.couch = s.couch;
    n.rounds = s.rounds;
    n.played = (int)s.played.size();
    n.games = s.played;
    for (const Standing& e : podium(s)) {
        NightRow r;
        r.name = e.name;
        r.points = e.points;
        r.firsts = e.firsts;
        r.rank = e.rank;
        r.me = e.slot == my_slot && my_slot >= 0 && !e.ghost;
        n.rows.push_back(r);
    }
    return n;
}

/* A night worth keeping: at least one round was played and somebody scored or there were at least two players. */
inline bool night_worth_saving(const Night& n) { return !n.id.empty() && n.played > 0 && n.rows.size() >= 2; }

inline std::string night_to_json(const Night& n) {
    json::Value o = json::make_object();
    o.o.push_back({"v", json::make_int(1)});
    o.o.push_back({"id", json::make_string(n.id)});
    o.o.push_back({"at", json::make_int(n.at)});
    o.o.push_back({"couch", json::make_bool(n.couch)});
    o.o.push_back({"rounds", json::make_int(n.rounds)});
    o.o.push_back({"played", json::make_int(n.played)});
    json::Value g = json::make_array();
    for (const auto& x : n.games) g.a.push_back(json::make_string(x));
    o.o.push_back({"games", g});
    json::Value rows = json::make_array();
    for (const auto& r : n.rows) {
        json::Value ro = json::make_object();
        ro.o.push_back({"name", json::make_string(r.name)});
        ro.o.push_back({"pts", json::make_int(r.points)});
        ro.o.push_back({"firsts", json::make_int(r.firsts)});
        ro.o.push_back({"rank", json::make_int(r.rank)});
        ro.o.push_back({"me", json::make_bool(r.me)});
        rows.a.push_back(ro);
    }
    o.o.push_back({"rows", rows});
    return json::stringify(o);
}

inline bool night_from_json(const std::string& text, Night* out) {
    json::Value root;
    if (!json::parse(text, &root) || !root.is_object()) return false;
    Night n;
    n.id = root.str("id");
    n.at = root.i64("at");
    n.couch = root.boolean("couch");
    n.rounds = (int)root.i64("rounds");
    n.played = (int)root.i64("played");
    if (const json::Value* g = root.get("games"); g && g->is_array())
        for (const auto& x : g->a) if (x.is_string() && n.games.size() < 64) n.games.push_back(x.s);
    const json::Value* rows = root.get("rows");
    if (n.id.empty() || n.at <= 0 || !rows || !rows->is_array()) return false;
    for (const auto& ro : rows->a) {
        if (!ro.is_object()) continue;
        NightRow r;
        r.name = ro.str("name");
        r.points = (int)ro.i64("pts");
        r.firsts = (int)ro.i64("firsts");
        r.rank = (int)ro.i64("rank");
        r.me = ro.boolean("me");
        n.rows.push_back(r);
        if (n.rows.size() >= 8) break;
    }
    if (n.rows.size() < 2) return false;
    if (out) *out = n;
    return true;
}

inline std::filesystem::path night_path(const std::string& dir, const std::string& id, bool vault) {
    auto base = std::filesystem::u8path(dir);
    if (vault) base /= "_account";
    return base / "party" / (rivals::safe_id(id) + ".json");
}

inline void prune_nights(const std::string& dir, bool vault, size_t keep = kMaxNights) {
    if (dir.empty()) return;
    std::error_code ec;
    std::vector<std::pair<int64_t, std::filesystem::path>> v;
    for (auto& e : std::filesystem::directory_iterator(night_path(dir, "x", vault).parent_path(), ec)) {
        if (e.path().extension() != ".json") continue;
        Night n;
        v.push_back({night_from_json(group::slurp(e.path()), &n) ? n.at : 0, e.path()});
    }
    if (v.size() <= keep) return;
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = keep; i < v.size(); ++i) std::filesystem::remove(v[i].second, ec);
}

/* Writes the night under the user-data dir and the vault (either may be empty to skip). An existing file is kept. */
inline bool save_night(const Night& n, const std::string& userdata, const std::string& vault) {
    if (!night_worth_saving(n)) return false;
    const std::string j = night_to_json(n);
    bool ok = true;
    std::error_code ec;
    if (!userdata.empty()) { auto p = night_path(userdata, n.id, false); if (!std::filesystem::exists(p, ec)) ok = group::spit(p, j) && ok; }
    if (!vault.empty()) { auto p = night_path(vault, n.id, true); if (!std::filesystem::exists(p, ec)) ok = group::spit(p, j) && ok; }
    prune_nights(userdata, false);
    prune_nights(vault, true);
    return ok;
}

/* Union of both stores by night id, newest first. */
inline std::vector<Night> load_nights(const std::string& userdata, const std::string& vault) {
    std::map<std::string, Night> byid;
    auto scan = [&](const std::filesystem::path& d) {
        std::error_code ec;
        for (auto& e : std::filesystem::directory_iterator(d, ec)) {
            if (e.path().extension() != ".json") continue;
            Night n;
            if (night_from_json(group::slurp(e.path()), &n)) byid.emplace(n.id, n);
        }
    };
    if (!userdata.empty()) scan(night_path(userdata, "x", false).parent_path());
    if (!vault.empty()) scan(night_path(vault, "x", true).parent_path());
    std::vector<Night> v;
    for (auto& kv : byid) v.push_back(kv.second);
    std::stable_sort(v.begin(), v.end(), [](const Night& a, const Night& b) { return a.at > b.at; });
    return v;
}

/* For Wrapped: party nights that ended in [from_s, to_s) (unix seconds), and how many of them this player won (rank 1 on their row). */
inline int count_nights(const std::vector<Night>& nights, int64_t from_s, int64_t to_s) {
    int n = 0;
    for (const auto& x : nights) if (x.at / 1000 >= from_s && x.at / 1000 < to_s) ++n;
    return n;
}
inline int my_wins(const std::vector<Night>& nights, int64_t from_s, int64_t to_s) {
    int n = 0;
    for (const auto& x : nights) {
        if (x.at / 1000 < from_s || x.at / 1000 >= to_s) continue;
        for (const auto& r : x.rows) if (r.me && r.rank == 1) { ++n; break; }
    }
    return n;
}

}  // namespace party
}  // namespace hubui
