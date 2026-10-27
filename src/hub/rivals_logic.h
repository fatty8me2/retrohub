#pragma once
/* Pure logic for rivalries (Wave 4 S2): no ImGui, no network. Unit tested in test_rivals.cpp.
 *
 * Hand-off: the hub launches Smash Classic with SSB64_RESULT_FILE=<path>. When an online VS match ends, BattleShip writes
 * a small JSON there (parse_result below): {"v":1,"game":"smash64","id":"<room>-<seed>","code":"<room>","players":[
 * {"slot":0,"char":1,"place":0,"stocks":2,"kos":3,"falls":1}, ...]}. place: 0 = winner, larger = knocked out earlier.
 * The hub maps the lobby slots to accounts (friends by uid; everyone else by name only), ranks the players and stores one
 * file per match, named after the match id, in BOTH the user-data dir and the vault:
 *     <dir>/rivals/<id>.json      (vault: <vault>/_account/rivals/<id>.json)
 * One file per match means two PCs never write the same file, and a match both PCs saw simply has the same id (union by id).
 *
 * Other games have no results: only "sessions together", counted from the recent groups. */
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "group_logic.h"
#include "json.h"

namespace hubui {
namespace rivals {

/* ---- raw result as written by the game ---- */
struct RawPlayer { int slot = -1, ch = -1, place = 0, stocks = 0, kos = 0, falls = 0; };
struct RawResult { std::string game, id, code; std::vector<RawPlayer> players; };

/* False for anything malformed or a match with fewer than two players. */
inline bool parse_result(const std::string& text, RawResult* out) {
    json::Value root;
    if (!json::parse(text, &root) || !root.is_object()) return false;
    RawResult r;
    r.game = root.str("game");
    r.id = root.str("id");
    r.code = root.str("code");
    if (r.game.empty() || r.id.empty()) return false;
    const json::Value* ps = root.get("players");
    if (!ps || !ps->is_array()) return false;
    for (auto& p : ps->a) {
        if (!p.is_object()) continue;
        RawPlayer rp;
        rp.slot = (int)p.i64("slot", -1);
        if (rp.slot < 0 || rp.slot > 7) continue;
        rp.ch = (int)p.i64("char", -1);
        rp.place = (int)p.i64("place");
        rp.stocks = (int)p.i64("stocks");
        rp.kos = (int)p.i64("kos");
        rp.falls = (int)p.i64("falls");
        r.players.push_back(rp);
    }
    if (r.players.size() < 2) return false;
    if (out) *out = r;
    return true;
}

/* ---- a stored match ---- */
struct Player {
    std::string uid, name;   // uid empty = not one of our friends (and not us)
    bool me = false;
    int ch = -1;
    int rank = 0;            // 1 = winner; equal rank = tie
    int stocks = 0, kos = 0;
};
struct Match {
    std::string id, game;
    int64_t at = 0;          // ms since epoch
    std::vector<Player> players;
};

/* Who sat in a lobby slot (from the room list + the friends list). */
struct SlotInfo { int slot = -1; std::string uid, name; bool me = false; };

/* Ranks players: place ascending, then stocks left, then KOs, then fewer falls. */
inline Match build_match(const RawResult& r, const std::vector<SlotInfo>& slots, int64_t at) {
    Match m;
    m.id = r.id;
    m.game = r.game;
    m.at = at;
    struct Row { Player p; int place, falls; };
    std::vector<Row> rows;
    for (auto& rp : r.players) {
        Row row;
        for (auto& s : slots) if (s.slot == rp.slot) { row.p.uid = s.uid; row.p.name = s.name; row.p.me = s.me; break; }
        if (row.p.name.empty() && !row.p.me) row.p.name = "Player " + std::to_string(rp.slot + 1);
        row.p.ch = rp.ch; row.p.stocks = rp.stocks; row.p.kos = rp.kos;
        row.place = rp.place; row.falls = rp.falls;
        rows.push_back(row);
    }
    auto better = [](const Row& a, const Row& b) {
        if (a.place != b.place) return a.place < b.place;
        if (a.p.stocks != b.p.stocks) return a.p.stocks > b.p.stocks;
        if (a.p.kos != b.p.kos) return a.p.kos > b.p.kos;
        return a.falls < b.falls;
    };
    auto same = [&](const Row& a, const Row& b) { return !better(a, b) && !better(b, a); };
    std::stable_sort(rows.begin(), rows.end(), better);
    for (size_t i = 0; i < rows.size(); ++i) {
        rows[i].p.rank = (i > 0 && same(rows[i], rows[i - 1])) ? rows[i - 1].p.rank : (int)i + 1;
        m.players.push_back(rows[i].p);
    }
    return m;
}

inline bool has_me(const Match& m) { for (auto& p : m.players) if (p.me) return true; return false; }

/* ---- persistence ---- */
inline std::string match_to_json(const Match& m) {
    json::Value o = json::make_object();
    o.o.push_back({"v", json::make_int(1)});
    o.o.push_back({"id", json::make_string(m.id)});
    o.o.push_back({"game", json::make_string(m.game)});
    o.o.push_back({"at", json::make_int(m.at)});
    json::Value arr = json::make_array();
    for (auto& p : m.players) {
        json::Value po = json::make_object();
        po.o.push_back({"uid", json::make_string(p.uid)});
        po.o.push_back({"name", json::make_string(p.name)});
        po.o.push_back({"me", json::make_bool(p.me)});
        po.o.push_back({"char", json::make_int(p.ch)});
        po.o.push_back({"rank", json::make_int(p.rank)});
        po.o.push_back({"stocks", json::make_int(p.stocks)});
        po.o.push_back({"kos", json::make_int(p.kos)});
        arr.a.push_back(po);
    }
    o.o.push_back({"players", arr});
    return json::stringify(o);
}
inline bool match_from_json(const std::string& text, Match* out) {
    json::Value root;
    if (!json::parse(text, &root) || !root.is_object()) return false;
    Match m;
    m.id = root.str("id");
    m.game = root.str("game");
    m.at = root.i64("at");
    const json::Value* ps = root.get("players");
    if (m.id.empty() || m.at <= 0 || !ps || !ps->is_array()) return false;
    for (auto& p : ps->a) {
        if (!p.is_object()) continue;
        Player pl;
        pl.uid = p.str("uid"); pl.name = p.str("name"); pl.me = p.boolean("me");
        pl.ch = (int)p.i64("char", -1); pl.rank = (int)p.i64("rank", 1);
        pl.stocks = (int)p.i64("stocks"); pl.kos = (int)p.i64("kos");
        m.players.push_back(pl);
    }
    if (m.players.size() < 2) return false;
    if (out) *out = m;
    return true;
}

/* File name for a match id: only [A-Za-z0-9_-] survive, the rest becomes '_'. */
inline std::string safe_id(const std::string& id) {
    std::string s;
    for (char c : id) s += (std::isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_';
    return s.empty() ? std::string("_") : s;
}
inline std::filesystem::path match_path(const std::string& dir, const std::string& id, bool vault) {
    auto base = std::filesystem::u8path(dir);
    if (vault) base /= "_account";
    return base / "rivals" / (safe_id(id) + ".json");
}
/* Writes the match under the user-data dir and the vault (either may be empty to skip). An existing file is kept. */
inline bool save_match(const Match& m, const std::string& userdata, const std::string& vault) {
    std::string j = match_to_json(m);
    bool ok = true;
    std::error_code ec;
    if (!userdata.empty()) { auto p = match_path(userdata, m.id, false); if (!std::filesystem::exists(p, ec)) ok = group::spit(p, j) && ok; }
    if (!vault.empty()) { auto p = match_path(vault, m.id, true); if (!std::filesystem::exists(p, ec)) ok = group::spit(p, j) && ok; }
    return ok;
}
/* Union of both stores by match id, newest first. */
inline std::vector<Match> load_matches(const std::string& userdata, const std::string& vault) {
    std::map<std::string, Match> byid;
    auto scan = [&](const std::filesystem::path& d) {
        std::error_code ec;
        for (auto& e : std::filesystem::directory_iterator(d, ec)) {
            if (e.path().extension() != ".json") continue;
            Match m;
            if (match_from_json(group::slurp(e.path()), &m)) byid.emplace(m.id, m);
        }
    };
    if (!userdata.empty()) scan(match_path(userdata, "x", false).parent_path());
    if (!vault.empty()) scan(match_path(vault, "x", true).parent_path());
    std::vector<Match> v;
    for (auto& kv : byid) v.push_back(kv.second);
    std::stable_sort(v.begin(), v.end(), [](const Match& a, const Match& b) { return a.at > b.at; });
    return v;
}

/* ---- characters ---- */
inline std::string char_name(const std::string& game, int ch) {
    static const char* k64[] = {"Mario", "Fox", "DK", "Samus", "Luigi", "Link", "Yoshi", "Falcon", "Kirby", "Pikachu", "Jigglypuff", "Ness"};
    if (game == "smash64" && ch >= 0 && ch < 12) return k64[ch];
    return ch >= 0 ? "#" + std::to_string(ch) : std::string("?");
}

/* ---- aggregation ---- */
struct H2H {
    int wins = 0, losses = 0, draws = 0;
    std::string my_main, their_main;   // "" = unknown
    std::string last5;                 // oldest to newest, e.g. "WLWWL" (W/L/D), at most 5
    int games() const { return wins + losses + draws; }
};

inline std::string main_char(const std::vector<std::pair<std::string, int>>& newest_first) {   // (game, ch)
    std::map<std::pair<std::string, int>, int> n;
    for (auto& g : newest_first) if (g.second >= 0) ++n[g];
    std::pair<std::string, int> best{"", -1};
    int bn = 0;
    for (auto& g : newest_first) {   // newest first: a tie goes to the more recent pick
        if (g.second < 0) continue;
        if (n[g] > bn) { bn = n[g]; best = g; }
    }
    return bn ? char_name(best.first, best.second) : std::string();
}

/* Head to head between us and `uid` over every stored match (newest first in `ms`). Matches without both are skipped. */
inline H2H head_to_head(const std::vector<Match>& ms, const std::string& uid) {
    H2H h;
    std::vector<std::pair<std::string, int>> mine, theirs;
    std::string newest_first;
    for (auto& m : ms) {
        const Player* me = nullptr; const Player* them = nullptr;
        for (auto& p : m.players) { if (p.me) me = &p; else if (!uid.empty() && p.uid == uid) them = &p; }
        if (!me || !them) continue;
        char r = me->rank < them->rank ? 'W' : me->rank > them->rank ? 'L' : 'D';
        if (r == 'W') ++h.wins; else if (r == 'L') ++h.losses; else ++h.draws;
        if (newest_first.size() < 5) newest_first += r;
        mine.push_back({m.game, me->ch});
        theirs.push_back({m.game, them->ch});
    }
    h.last5.assign(newest_first.rbegin(), newest_first.rend());
    h.my_main = main_char(mine);
    h.their_main = main_char(theirs);
    return h;
}

struct Rival { std::string uid, name; H2H h; };
/* Friends we have played the most (min 1 game), most games first, then most recent. */
inline std::vector<Rival> top_rivals(const std::vector<Match>& ms, size_t max_n = 3) {
    std::vector<std::string> order;   // uids in order of their newest match
    std::map<std::string, std::string> names;
    for (auto& m : ms) {
        if (!has_me(m)) continue;
        for (auto& p : m.players)
            if (!p.me && !p.uid.empty() && !names.count(p.uid)) { names[p.uid] = p.name; order.push_back(p.uid); }
    }
    std::vector<Rival> v;
    for (auto& u : order) { Rival r; r.uid = u; r.name = names[u]; r.h = head_to_head(ms, u); if (r.h.games() > 0) v.push_back(r); }
    std::stable_sort(v.begin(), v.end(), [](const Rival& a, const Rival& b) { return a.h.games() > b.h.games(); });
    if (v.size() > max_n) v.resize(max_n);
    return v;
}

/* "You 12 - 9 Mickey" (draws appended as ", 1 tie"). */
inline std::string record_text(const H2H& h, const std::string& name) {
    std::string t = "You " + std::to_string(h.wins) + " - " + std::to_string(h.losses) + " " + name;
    if (h.draws) t += " (" + std::to_string(h.draws) + (h.draws == 1 ? " tie)" : " ties)");
    return t;
}
/* "Mario vs Fox" or "" when neither main is known. */
inline std::string mains_text(const H2H& h) {
    if (h.my_main.empty() && h.their_main.empty()) return "";
    return (h.my_main.empty() ? "?" : h.my_main) + " vs " + (h.their_main.empty() ? "?" : h.their_main);
}

/* Other games: how many recent groups (sessions) this friend was in. */
inline int sessions_together(const group::RecentGroups& g, const std::string& uid) {
    int n = 0;
    for (auto& x : g.v) for (auto& m : x.members) if (m.uid == uid) { ++n; break; }
    return n;
}
inline std::string sessions_text(int n) {
    return n <= 0 ? std::string() : std::to_string(n) + (n == 1 ? " session together" : " sessions together");
}

}  // namespace rivals
}  // namespace hubui
