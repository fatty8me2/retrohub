#include "party_logic.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>

#include "logic.h"   // compare_app_versions

/* Party mode pure logic (Plan 4 P1-P3): points, shuffle, the host reducer, polls, standings, host migration, the pt_* wire format.
 * No UI, no files, no network. Tests: test_party.cpp. */

namespace hubui {
namespace party {

namespace {

constexpr int kMaxSlot = 15;       // slots in a State are 0..3 (players) and 4..15 (ghosts)
constexpr size_t kMaxMapEntries = 12;

/* UTF-8 safe cut: never ends in the middle of a character. */
std::string utf8_cut(const std::string& s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    size_t n = max_bytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;   // s[n] is a continuation byte: back off to the lead byte
    return s.substr(0, n);
}

std::string clean_id(const std::string& s) { return s.size() <= kMaxId ? s : std::string(); }

bool valid_slot(int s) { return s >= 0 && s <= kMaxSlot; }

/* xorshift32: tiny, portable, identical on every PC (std::shuffle is not specified to give the same order on every standard library). */
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed * 2654435761u + 0x9E3779B9u) {
        if (!s) s = 0x6D2B79F5u;
        for (int i = 0; i < 4; ++i) next();
    }
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    uint32_t below(uint32_t n) { return n ? next() % n : 0; }
};

const char* kEnds[] = {"", "result", "timer", "closed", "vote", "skip"};
std::string clean_end(const std::string& e) {
    for (const char* k : kEnds) if (e == k) return e;
    return std::string();
}

std::vector<std::string> clean_ids(const std::vector<std::string>& in, size_t cap, bool unique) {
    std::vector<std::string> out;
    for (const std::string& s : in) {
        if (s.empty() || s.size() > kMaxId) continue;
        if (unique && std::find(out.begin(), out.end(), s) != out.end()) continue;
        out.push_back(s);
        if (out.size() >= cap) break;
    }
    return out;
}

template <class V> void clean_map(std::map<int, V>& m) {
    for (auto it = m.begin(); it != m.end();) it = valid_slot(it->first) ? std::next(it) : m.erase(it);
    while (m.size() > kMaxMapEntries) m.erase(std::prev(m.end()));
}

std::vector<int> clean_slots(const std::vector<int>& in) {
    std::set<int> u;
    for (int s : in) if (valid_slot(s)) u.insert(s);
    std::vector<int> out(u.begin(), u.end());
    if (out.size() > 8) out.resize(8);
    return out;
}

/* The State as it goes on the wire / into a reducer result: every field inside its limits. */
State sanitize(const State& in) {
    State s = in;
    s.v = kStateV;
    if (s.rev < 0) s.rev = 0;
    s.rounds = clamp_rounds(s.rounds);
    s.round = std::max(0, std::min(kMaxRounds, s.round));
    s.round_secs = clamp_round_secs(s.round_secs);
    s.list = clean_ids(s.list, kMaxList, false);
    s.cur = clean_id(s.cur);
    clean_map(s.points);
    clean_map(s.firsts);
    clean_map(s.gain);
    clean_map(s.names);
    for (auto& kv : s.names) kv.second = utf8_cut(kv.second, kMaxName);
    clean_map(s.votes);
    for (auto it = s.votes.begin(); it != s.votes.end();) it = (it->second.empty() || it->second.size() > kMaxId) ? s.votes.erase(it) : std::next(it);
    if (s.poll != "next" && s.poll != "winner") { s.poll.clear(); s.cand.clear(); s.votes.clear(); }
    s.cand = clean_ids(s.cand, kMaxCand, true);
    s.players = clean_slots(s.players);
    s.played = clean_ids(s.played, kMaxRounds, false);
    s.last_id = utf8_cut(s.last_id, 64);
    s.end = clean_end(s.end);
    s.pick = clean_id(s.pick);
    s.tied = clean_ids(s.tied, kMaxCand, true);
    return s;
}

json::Value int_map(const std::map<int, int>& m) {
    json::Value o = json::make_object();
    for (auto& kv : m) o.o.push_back({std::to_string(kv.first), json::make_int(kv.second)});
    return o;
}
json::Value str_map(const std::map<int, std::string>& m) {
    json::Value o = json::make_object();
    for (auto& kv : m) o.o.push_back({std::to_string(kv.first), json::make_string(kv.second)});
    return o;
}
json::Value str_list(const std::vector<std::string>& v) {
    json::Value a = json::make_array();
    for (auto& s : v) a.a.push_back(json::make_string(s));
    return a;
}

std::string build_body(const State& s) {
    json::Value o = json::make_object();
    auto put = [&](const char* k, json::Value v) { o.o.push_back({k, std::move(v)}); };
    put("kind", json::make_string("pt_state"));
    put("v", json::make_int(s.v));
    put("rev", json::make_int(s.rev));
    put("ph", json::make_int((int)s.phase));
    put("rd", json::make_int(s.round));
    put("rds", json::make_int(s.rounds));
    put("sh", json::make_int(s.shuffle ? 1 : 0));
    put("seed", json::make_int((int64_t)s.seed));
    put("sc", json::make_int((int)s.scoring));
    put("secs", json::make_int(s.round_secs));
    put("ar", json::make_int(s.auto_ready ? 1 : 0));
    put("cu", json::make_int(s.couch ? 1 : 0));
    put("list", str_list(s.list));
    put("cur", json::make_string(s.cur));
    put("pts", int_map(s.points));
    put("nm", str_map(s.names));
    if (!s.firsts.empty()) put("fs", int_map(s.firsts));
    if (!s.gain.empty()) put("gn", int_map(s.gain));
    if (!s.players.empty()) {
        json::Value a = json::make_array();
        for (int p : s.players) a.a.push_back(json::make_int(p));
        put("pl", a);
    }
    if (!s.poll.empty()) {
        put("poll", json::make_string(s.poll));
        put("cand", str_list(s.cand));
        put("vt", str_map(s.votes));
    }
    if (!s.played.empty()) put("pg", str_list(s.played));
    if (!s.last_id.empty()) put("lid", json::make_string(s.last_id));
    if (!s.end.empty()) put("end", json::make_string(s.end));
    if (!s.pick.empty()) put("pick", json::make_string(s.pick));
    if (!s.tied.empty()) put("tied", str_list(s.tied));
    return json::stringify(o);
}

bool read_int_map(const json::Value* v, std::map<int, int>* out) {
    out->clear();
    if (!v) return true;
    if (!v->is_object()) return false;
    for (auto& kv : v->o) {
        char* end = nullptr;
        long slot = std::strtol(kv.first.c_str(), &end, 10);
        if (!end || *end || kv.first.empty() || !valid_slot((int)slot) || !kv.second.is_number()) return false;
        (*out)[(int)slot] = (int)std::max<int64_t>(-99999, std::min<int64_t>(999999, kv.second.i));
    }
    return true;
}
bool read_str_map(const json::Value* v, std::map<int, std::string>* out) {
    out->clear();
    if (!v) return true;
    if (!v->is_object()) return false;
    for (auto& kv : v->o) {
        char* end = nullptr;
        long slot = std::strtol(kv.first.c_str(), &end, 10);
        if (!end || *end || kv.first.empty() || !valid_slot((int)slot) || !kv.second.is_string()) return false;
        (*out)[(int)slot] = kv.second.s;
    }
    return true;
}
bool read_str_list(const json::Value* v, std::vector<std::string>* out) {
    out->clear();
    if (!v) return true;
    if (!v->is_array()) return false;
    for (auto& e : v->a) {
        if (!e.is_string()) return false;
        out->push_back(e.s);
    }
    return true;
}

/* next game for the round after `s.round`, "" after the last round */
std::string game_after(const State& s) { return s.round < s.rounds ? game_for_round(s, s.round + 1) : std::string(); }

/* Common tail of a finished round (result, timer, closed): standings phase, the finished game goes to `played`, the next one is preselected. */
void finish_round(State* s) {
    s->phase = Phase::Intermission;
    if (!s->cur.empty() && (int)s->played.size() < kMaxRounds) s->played.push_back(s->cur);
    s->cur = game_after(*s);
}

State bump(State s) { s.rev += 1; return s; }

/* Moves slot `from`'s data to `to` (every per-slot map). `to` must be free. */
void move_slot(State* s, int from, int to) {
    auto mv = [&](auto& m) {
        auto it = m.find(from);
        if (it == m.end()) return;
        auto v = std::move(it->second);
        m.erase(it);
        m[to] = std::move(v);
    };
    mv(s->points); mv(s->firsts); mv(s->gain); mv(s->names);
}

bool has_data(const State& s, int slot) { return s.points.count(slot) || s.firsts.count(slot) || s.gain.count(slot); }
bool has_any(const State& s, int slot) { return has_data(s, slot) || s.names.count(slot); }
/* A seat that only has the empty row made for whoever sat down (no points, no round won, nothing gained). */
bool blank_seat(const State& s, int slot) {
    auto p = s.points.find(slot);
    auto f = s.firsts.find(slot);
    auto g = s.gain.find(slot);
    return (p == s.points.end() || p->second == 0) && (f == s.firsts.end() || f->second == 0) && (g == s.gain.end() || g->second == 0);
}

int free_ghost(const State& s) {
    for (int g = kGhostBase; g <= kMaxSlot; ++g) if (!has_any(s, g)) return g;
    return -1;
}

}  // namespace

/* ------------------------------------------------------------------ scoring */

std::map<int, int> points_for(const std::vector<std::vector<int>>& ranks) {
    static const int kPts[] = {3, 2, 1, 0};
    std::map<int, int> out;
    int placed = 0;
    for (const auto& grp : ranks) {
        if (grp.empty()) continue;
        const int p = placed < 4 ? kPts[placed] : 0;
        for (int slot : grp) out.emplace(slot, p);   // a slot listed twice keeps its first group
        placed += (int)grp.size();
    }
    return out;
}

int clamp_rounds(int r) { return std::max(1, std::min(kMaxRounds, r)); }
int clamp_round_secs(int secs) { return std::max(0, std::min(3600, secs)); }

std::string phase_name(Phase p) {
    switch (p) {
        case Phase::Off: return "off";
        case Phase::Lobby: return "lobby";
        case Phase::Playing: return "playing";
        case Phase::Intermission: return "intermission";
        case Phase::Done: return "done";
    }
    return "off";
}
std::string scoring_name(Scoring s) { return s == Scoring::Vote ? "vote" : s == Scoring::None ? "none" : "auto"; }

/* ------------------------------------------------------------------ shuffle */

std::vector<std::string> shuffled(const std::vector<std::string>& pool_in, uint32_t seed, int count) {
    std::vector<std::string> out;
    std::vector<std::string> pool;
    for (const std::string& g : pool_in) if (!g.empty() && std::find(pool.begin(), pool.end(), g) == pool.end()) pool.push_back(g);
    std::sort(pool.begin(), pool.end());   // the result does not depend on the order the pool was collected in
    if (pool.empty() || count <= 0) return out;
    count = std::min(count, 64);
    Rng rng(seed);
    while ((int)out.size() < count) {
        std::vector<std::string> perm = pool;
        for (size_t i = perm.size(); i > 1; --i) std::swap(perm[i - 1], perm[rng.below((uint32_t)i)]);   // Fisher-Yates
        if (!out.empty() && perm.size() > 1 && perm[0] == out.back()) std::swap(perm[0], perm[1]);        // no game twice in a row across a pass
        for (const std::string& g : perm) {
            if ((int)out.size() >= count) break;
            out.push_back(g);
        }
    }
    return out;
}

std::string game_for_round(const State& s, int round) {
    if (s.list.empty() || round < 1) return std::string();
    return s.list[(size_t)(round - 1) % s.list.size()];
}

/* ------------------------------------------------------------------ wire format */

std::string encode_state(const State& in) {
    State s = sanitize(in);
    std::string body = build_body(s);
    /* A pathological State (every id at the limit) must still fit one frame: drop the least important parts first. */
    while (body.size() > kMaxBody) {
        if (!s.tied.empty()) s.tied.clear();
        else if (!s.played.empty()) s.played.erase(s.played.begin());
        else if (!s.pick.empty()) s.pick.clear();
        else if (s.cand.size() > 2) s.cand.pop_back();
        else if (s.list.size() > 1) s.list.pop_back();
        else if (!s.votes.empty()) s.votes.erase(s.votes.begin());
        else break;
        body = build_body(s);
    }
    return body;
}

bool decode_state(const json::Value& j, State* out) {
    if (!j.is_object() || j.str("kind") != "pt_state") return false;
    const int64_t v = j.i64("v", 0);
    if (v < 1 || v > kStateV) return false;   // missing or newer than this build understands
    State s;
    s.v = (int)v;
    s.rev = (int)std::max<int64_t>(0, std::min<int64_t>(2000000000, j.i64("rev", 0)));
    const int64_t ph = j.i64("ph", -1);
    if (ph < 0 || ph > (int64_t)Phase::Done) return false;
    s.phase = (Phase)ph;
    s.round = (int)j.i64("rd", 0);
    s.rounds = (int)j.i64("rds", 3);
    s.shuffle = j.i64("sh", 0) != 0;
    const int64_t seed = j.i64("seed", 0);
    if (seed < 0 || seed > 0xFFFFFFFFll) return false;
    s.seed = (uint32_t)seed;
    const int64_t sc = j.i64("sc", 0);
    if (sc < 0 || sc > (int64_t)Scoring::None) return false;
    s.scoring = (Scoring)sc;
    s.round_secs = (int)j.i64("secs", 0);
    s.auto_ready = j.i64("ar", 1) != 0;
    s.couch = j.i64("cu", 0) != 0;
    s.cur = j.str("cur");
    s.poll = j.str("poll");
    s.last_id = j.str("lid");
    s.end = j.str("end");
    s.pick = j.str("pick");
    if (!read_str_list(j.get("list"), &s.list) || !read_str_list(j.get("cand"), &s.cand) || !read_str_list(j.get("pg"), &s.played) ||
        !read_str_list(j.get("tied"), &s.tied))
        return false;
    if (!read_int_map(j.get("pts"), &s.points) || !read_int_map(j.get("fs"), &s.firsts) || !read_int_map(j.get("gn"), &s.gain) ||
        !read_str_map(j.get("nm"), &s.names) || !read_str_map(j.get("vt"), &s.votes))
        return false;
    if (const json::Value* pl = j.get("pl")) {
        if (!pl->is_array()) return false;
        for (auto& e : pl->a) {
            if (!e.is_number()) return false;
            s.players.push_back((int)e.i);
        }
    }
    if (out) *out = sanitize(s);
    return true;
}

bool decode_state(const std::string& body, State* out) {
    json::Value j;
    if (body.size() > 8192 || !json::parse(body, &j)) return false;
    return decode_state(j, out);
}

std::string encode_vote(const VoteMsg& v) {
    json::Value o = json::make_object();
    o.o.push_back({"kind", json::make_string("pt_vote")});
    o.o.push_back({"round", json::make_int(v.round)});
    o.o.push_back({"what", json::make_string(v.what)});
    o.o.push_back({"pick", json::make_string(v.pick)});
    return json::stringify(o);
}

bool decode_vote(const json::Value& j, VoteMsg* out) {
    if (!j.is_object() || j.str("kind") != "pt_vote") return false;
    VoteMsg v;
    v.round = (int)j.i64("round", -1);
    v.what = j.str("what");
    v.pick = j.str("pick");
    if (v.round < 0 || v.round > 99 || (v.what != "next" && v.what != "winner") || v.pick.empty() || v.pick.size() > kMaxId) return false;
    if (out) *out = v;
    return true;
}

bool accept(const State& cur, const State& in) {
    if (in.v > kStateV) return false;
    if (cur.phase == Phase::Off && cur.rev == 0 && cur.seed == 0 && cur.list.empty()) return true;   // nothing known yet
    if (in.seed != cur.seed) return true;                                                           // a different night
    return in.rev > cur.rev;
}

bool accept_state_from(int from_slot, bool i_am_host, const State& cur, const State& incoming) {
    if (from_slot != 0 || i_am_host) return false;   // only the host speaks for the party, and the host never takes its own State from the room
    return accept(cur, incoming);
}

bool accept_vote_from(int from_slot, bool i_am_host, const std::vector<int>& member_slots) {
    if (!i_am_host || !valid_slot(from_slot)) return false;   // votes are for the host to fold in
    return std::find(member_slots.begin(), member_slots.end(), from_slot) != member_slots.end();
}

State rebroadcast(const State& last) {
    State s = last;
    s.rev += kRevLeap;
    return s;
}

/* ------------------------------------------------------------------ reducer */

State on_result(const State& s, const results::RoundResult& r) { return on_result(s, r, std::map<int, std::string>()); }

State on_result(const State& s, const results::RoundResult& r, const std::map<int, std::string>& names) {
    if (s.phase != Phase::Playing || s.scoring != Scoring::Auto) return s;   // Vote: a person decides; None: no points
    if (r.ranks.empty() || r.ranks[0].empty()) return s;
    if (!r.id.empty() && r.id == s.last_id) return s;                        // a match counts once
    if (r.local != s.couch) return s;                                        // couch parties count local matches, rooms online ones
    State ns = s;
    ns.gain.clear();
    ns.poll.clear();
    ns.cand.clear();
    ns.votes.clear();
    ns.pick.clear();
    ns.tied.clear();
    const std::map<int, int> pts = points_for(r.ranks);
    std::vector<int> in_round;
    for (const auto& kv : pts) {
        if (!valid_slot(kv.first)) continue;
        ns.points[kv.first] += kv.second;
        ns.gain[kv.first] = kv.second;
        in_round.push_back(kv.first);
        auto nm = names.find(kv.first);
        if (nm != names.end() && !nm->second.empty()) ns.names[kv.first] = nm->second;
    }
    for (int slot : r.ranks[0]) if (valid_slot(slot)) ns.firsts[slot] += 1;
    ns.players = clean_slots(in_round);
    ns.last_id = r.id;
    ns.end = "result";
    finish_round(&ns);
    return bump(sanitize(ns));
}

State on_round_end(const State& s) { return on_round_end(s, "closed"); }

State on_round_end(const State& s, const std::string& how) {
    if (s.phase != Phase::Playing) return s;
    State ns = s;
    ns.gain.clear();
    ns.poll.clear();
    ns.cand.clear();
    ns.votes.clear();
    ns.pick.clear();
    ns.tied.clear();
    ns.end = how == "timer" ? "timer" : "closed";
    finish_round(&ns);
    if (ns.scoring != Scoring::None) {   // nobody reported a winner: ask the room
        std::vector<int> who = ns.players;
        if (who.empty()) for (const auto& kv : ns.names) if (kv.first < kGhostBase) who.push_back(kv.first);
        if (who.size() >= 2) {
            ns.poll = "winner";
            ns.cand.clear();
            for (int p : who) ns.cand.push_back(std::to_string(p));
        }
    }
    return bump(sanitize(ns));
}

State next_round(const State& s) { return next_round(s, std::vector<int>()); }

State next_round(const State& s, const std::vector<int>& players) {
    if (s.phase != Phase::Intermission) return s;
    State ns = s;
    ns.poll.clear();   // an unanswered "who won?" counts as skipped
    ns.cand.clear();
    ns.votes.clear();
    ns.pick.clear();
    ns.tied.clear();
    ns.gain.clear();
    ns.end.clear();
    if (ns.round >= ns.rounds) {
        ns.phase = Phase::Done;
        ns.cur.clear();
        return bump(ns);
    }
    ns.round += 1;
    ns.phase = Phase::Playing;
    if (ns.cur.empty()) ns.cur = game_for_round(ns, ns.round);
    if (!players.empty()) ns.players = clean_slots(players);
    return bump(sanitize(ns));
}

State set_next(const State& s, const std::string& game) {
    if (s.phase != Phase::Intermission || game.empty() || game.size() > kMaxId || s.round >= s.rounds || game == s.cur) return s;
    State ns = s;
    ns.cur = game;
    if (!ns.list.empty()) ns.list[(size_t)s.round % ns.list.size()] = game;
    return bump(ns);
}

/* ------------------------------------------------------------------ setup */

State make_lobby(uint32_t seed, int rev) {
    State s;
    s.phase = Phase::Lobby;
    s.seed = seed;
    s.rev = rev + 1;
    s.rounds = 5;
    s.shuffle = true;
    return s;
}

State configure(const State& s, int rounds, Scoring scoring, int round_secs, bool auto_ready) {
    if (s.phase != Phase::Lobby) return s;
    State ns = s;
    ns.rounds = clamp_rounds(rounds);
    ns.scoring = scoring;
    ns.round_secs = clamp_round_secs(round_secs);
    ns.auto_ready = auto_ready;
    if (ns.rounds == s.rounds && ns.scoring == s.scoring && ns.round_secs == s.round_secs && ns.auto_ready == s.auto_ready) return s;
    return bump(ns);
}

State set_playlist(const State& s, const std::vector<std::string>& list, bool shuffle, uint32_t seed) {
    if (s.phase != Phase::Lobby) return s;
    State ns = s;
    ns.list = clean_ids(list, kMaxList, false);
    ns.shuffle = shuffle;
    ns.seed = seed;
    if (ns.list == s.list && ns.shuffle == s.shuffle && ns.seed == s.seed) return s;
    return bump(ns);
}

State start_party(const State& s, const std::vector<int>& players, const std::map<int, std::string>& names) {
    if (s.phase != Phase::Lobby || s.list.empty()) return s;
    State ns = s;
    ns.phase = Phase::Intermission;
    ns.round = 0;
    ns.points.clear();
    ns.firsts.clear();
    ns.gain.clear();
    ns.names.clear();
    ns.played.clear();
    ns.votes.clear();
    ns.cand.clear();
    ns.poll.clear();
    ns.last_id.clear();
    ns.end.clear();
    ns.pick.clear();
    ns.tied.clear();
    ns.players = clean_slots(players);
    for (int p : ns.players) {
        ns.points[p] = 0;
        auto nm = names.find(p);
        if (nm != names.end()) ns.names[p] = nm->second;
    }
    ns.cur = game_for_round(ns, 1);
    return bump(sanitize(ns));
}

State end_party(const State& s) {
    if (s.phase == Phase::Off && s.poll.empty()) return s;
    State ns;
    ns.rev = s.rev + 1;
    ns.seed = s.seed;
    ns.couch = s.couch;
    ns.phase = Phase::Off;
    return ns;
}

/* ------------------------------------------------------------------ polls */

State open_poll(const State& s, const std::string& what, const std::vector<std::string>& cand_in) {
    if (what != "next" && what != "winner") return s;
    if (s.phase == Phase::Playing || s.phase == Phase::Done) return s;
    if (what == "winner" && s.phase != Phase::Intermission) return s;
    std::vector<std::string> cand = clean_ids(cand_in, kMaxCand, true);
    if (cand.empty()) return s;
    State ns = s;
    ns.poll = what;
    ns.cand = cand;
    ns.votes.clear();
    ns.pick.clear();
    ns.tied.clear();
    return bump(ns);
}

State cancel_poll(const State& s) {
    if (s.poll.empty()) return s;
    State ns = s;
    ns.poll.clear();
    ns.cand.clear();
    ns.votes.clear();
    return bump(ns);
}

State on_vote(const State& s, int from_slot, const VoteMsg& v) { return on_vote(s, from_slot, v.round, v.what, v.pick); }

State on_vote(const State& s, int from_slot, int round, const std::string& what, const std::string& pick) {
    if (s.poll.empty() || what != s.poll || round != s.round) return s;   // a late vote for an earlier round / another kind of poll
    if (pick.empty() || pick.size() > kMaxId || !valid_slot(from_slot)) return s;
    State ns = s;
    const bool known = std::find(s.cand.begin(), s.cand.end(), pick) != s.cand.end();
    if (!known) {
        if (s.poll != "next" || (int)s.cand.size() >= kMaxCand) return s;   // members may add any game everyone can play to a "next" poll
        ns.cand.push_back(pick);
    }
    auto it = ns.votes.find(from_slot);
    if (known && it != ns.votes.end() && it->second == pick) return s;     // same vote again: nothing changed
    ns.votes[from_slot] = pick;
    return bump(ns);
}

Tally tally(const State& s) {
    Tally t;
    std::map<std::string, int> counts;
    for (const std::string& c : s.cand) counts[c] = 0;
    for (const auto& kv : s.votes) {
        auto it = counts.find(kv.second);
        if (it == counts.end()) continue;
        it->second += 1;
        t.voters += 1;
    }
    for (const std::string& c : s.cand) t.rows.push_back({c, counts[c]});
    std::stable_sort(t.rows.begin(), t.rows.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    if (!t.rows.empty() && t.rows[0].second > 0)
        for (const auto& r : t.rows) if (r.second == t.rows[0].second) t.leaders.push_back(r.first);
    return t;
}

bool all_voted(const State& s, const std::vector<int>& member_slots) {
    if (s.poll.empty() || member_slots.empty()) return false;
    for (int slot : member_slots) {
        auto it = s.votes.find(slot);
        if (it == s.votes.end() || std::find(s.cand.begin(), s.cand.end(), it->second) == s.cand.end()) return false;
    }
    return true;
}

std::string leading_pick(const State& s, uint32_t tiebreak, bool* tied) {
    if (tied) *tied = false;
    Tally t = tally(s);
    if (t.leaders.empty()) return std::string();
    if (t.leaders.size() == 1) return t.leaders[0];
    if (tied) *tied = true;
    return t.leaders[tiebreak % t.leaders.size()];
}

State resolve_next(const State& s, uint32_t tiebreak) {
    if (s.poll != "next") return s;
    bool tied = false;
    const std::string pick = leading_pick(s, tiebreak, &tied);
    Tally t = tally(s);
    State ns = s;
    ns.poll.clear();
    ns.cand.clear();
    ns.votes.clear();
    ns.pick = pick;
    ns.tied = tied ? t.leaders : std::vector<std::string>();
    if (!pick.empty() && ns.phase == Phase::Intermission && ns.round < ns.rounds) {
        ns.cur = pick;
        if (!ns.list.empty()) ns.list[(size_t)ns.round % ns.list.size()] = pick;
    }
    return bump(sanitize(ns));
}

State resolve_winner(const State& s, const std::vector<int>& winners) { return resolve_winner(s, winners, std::map<int, std::string>()); }

State resolve_winner(const State& s, const std::vector<int>& winners_in, const std::map<int, std::string>& names) {
    if (s.poll != "winner") return s;
    State ns = s;
    ns.poll.clear();
    ns.cand.clear();
    ns.votes.clear();
    std::vector<int> winners;
    for (int w : winners_in) if (valid_slot(w) && std::find(winners.begin(), winners.end(), w) == winners.end()) winners.push_back(w);
    ns.gain.clear();
    if (winners.empty()) {
        ns.end = "skip";
        return bump(ns);
    }
    const std::map<int, int> pts = points_for({winners});
    for (const auto& kv : pts) {
        ns.points[kv.first] += kv.second;
        ns.gain[kv.first] = kv.second;
        ns.firsts[kv.first] += 1;
        auto nm = names.find(kv.first);
        if (nm != names.end() && !nm->second.empty()) ns.names[kv.first] = nm->second;
    }
    ns.end = "vote";
    return bump(sanitize(ns));
}

/* ------------------------------------------------------------------ standings */

std::vector<Standing> standings(const State& s) {
    std::set<int> slots;
    for (const auto& kv : s.points) slots.insert(kv.first);
    for (const auto& kv : s.firsts) slots.insert(kv.first);
    for (const auto& kv : s.gain) slots.insert(kv.first);
    std::vector<Standing> v;
    for (int slot : slots) {
        Standing st;
        st.slot = slot;
        auto nm = s.names.find(slot);
        st.name = nm != s.names.end() && !nm->second.empty() ? nm->second : "Player " + std::to_string(slot + 1);
        auto p = s.points.find(slot);
        st.points = p != s.points.end() ? p->second : 0;
        auto f = s.firsts.find(slot);
        st.firsts = f != s.firsts.end() ? f->second : 0;
        auto g = s.gain.find(slot);
        st.gain = g != s.gain.end() ? g->second : 0;
        st.ghost = slot >= kGhostBase;
        v.push_back(st);
    }
    std::stable_sort(v.begin(), v.end(), [](const Standing& a, const Standing& b) {
        if (a.points != b.points) return a.points > b.points;
        if (a.firsts != b.firsts) return a.firsts > b.firsts;
        return false;   // stable: lower slot first
    });
    for (size_t i = 0; i < v.size(); ++i)
        v[i].rank = (i > 0 && v[i].points == v[i - 1].points && v[i].firsts == v[i - 1].firsts) ? v[i - 1].rank : (int)i + 1;
    return v;
}

std::vector<Standing> podium(const State& s) { return standings(s); }

std::string clock_text(int secs) {
    if (secs < 0) secs = 0;
    char b[24];
    std::snprintf(b, sizeof b, "%d:%02d", secs / 60, secs % 60);
    return b;
}

std::string osd_timer(int secs_left, int round, int rounds) {
    std::string head = round > 0 ? "R" + std::to_string(round) + "/" + std::to_string(rounds) + "  " : std::string();
    if (secs_left <= 0) return head + "time is up!";
    if (secs_left >= 90) return head + std::to_string((secs_left + 59) / 60) + " min left";
    return head + std::to_string(((secs_left + 4) / 5) * 5) + " s left";   // counted in fives, so the text (and the runner write) changes 18 times, not 90
}

std::string osd_standings(const State& s, size_t max_len) {
    const std::vector<Standing> st = standings(s);
    if (st.empty()) return std::string();
    const std::string head = s.round > 0 ? "R" + std::to_string(s.round) + "/" + std::to_string(s.rounds) + " " : std::string();
    auto build = [&](size_t name_cap, bool with_head, size_t max_items) {
        std::string out = with_head ? head : std::string();
        size_t n = 0;
        for (const Standing& e : st) {
            if (n >= max_items) break;
            if (n) out += "  ";
            out += std::to_string(e.rank) + " " + utf8_cut(e.name, name_cap) + " " + std::to_string(e.points);
            ++n;
        }
        return out;
    };
    for (size_t cap : {size_t(12), size_t(8), size_t(6), size_t(4)})
        for (bool with_head : {true, false}) {
            std::string out = build(cap, with_head, st.size());
            if (out.size() <= max_len) return out;
        }
    for (size_t n = st.size(); n > 1; --n) {   // even short names do not fit: as many leaders as fit
        std::string out = build(4, false, n);
        if (out.size() <= max_len) return out;
    }
    return utf8_cut(build(4, false, 1), max_len);
}

std::string osd_round_over(const State& s) {
    std::string out = "Round " + std::to_string(s.round) + " over!";
    int best = 0;
    for (const auto& kv : s.gain) best = std::max(best, kv.second);
    std::vector<std::string> top;
    if (best > 0) for (const auto& kv : s.gain) if (kv.second == best) {
        auto nm = s.names.find(kv.first);
        top.push_back(utf8_cut(nm != s.names.end() && !nm->second.empty() ? nm->second : "Player " + std::to_string(kv.first + 1), 16));
    }
    if (top.size() == 1) out += " " + top[0] + " wins (+" + std::to_string(best) + ")";
    else if (top.size() > 1) {
        out += " ";
        for (size_t i = 0; i < top.size(); ++i) out += (i ? (i + 1 == top.size() ? " and " : ", ") : "") + top[i];
        out += " tie (+" + std::to_string(best) + ")";
    } else if (s.poll == "winner") out += " Who won? Vote in RetroHub";
    return utf8_cut(out, 118);
}

/* ------------------------------------------------------------------ room membership / host migration */

State reconcile(const State& s, const std::map<int, std::string>& members) {
    if (s.phase == Phase::Off || s.phase == Phase::Lobby) return s;
    State ns = s;
    /* 1. a seat whose data belongs to somebody else now: that data moves to a ghost slot, keeping its name */
    std::vector<int> seats;
    for (const auto& kv : ns.names) if (kv.first < kGhostBase) seats.push_back(kv.first);
    for (const auto& kv : ns.points) if (kv.first < kGhostBase && !ns.names.count(kv.first)) seats.push_back(kv.first);
    std::sort(seats.begin(), seats.end());
    seats.erase(std::unique(seats.begin(), seats.end()), seats.end());
    for (int seat : seats) {
        auto m = members.find(seat);
        auto nm = ns.names.find(seat);
        const std::string stored = nm != ns.names.end() ? nm->second : std::string();
        if (m != members.end() && (stored.empty() || stored == m->second)) {
            if (stored.empty()) ns.names[seat] = m->second;
            continue;   // still the same person
        }
        if (!has_any(ns, seat)) continue;
        if (blank_seat(ns, seat)) {   // left before scoring anything: nothing to remember
            ns.points.erase(seat); ns.firsts.erase(seat); ns.gain.erase(seat); ns.names.erase(seat);
            ns.votes.erase(seat);
            continue;
        }
        const int g = free_ghost(ns);
        if (g < 0) continue;
        move_slot(&ns, seat, g);
        ns.votes.erase(seat);
    }
    /* 2. a player who is back (or sits in another seat now): their ghost data moves to their seat when it is free */
    for (int g = kGhostBase; g <= kMaxSlot; ++g) {
        auto nm = ns.names.find(g);
        if (nm == ns.names.end()) continue;
        const std::string who = nm->second;
        for (const auto& m : members) {
            if (m.second != who || m.first >= kGhostBase) continue;
            if (has_any(ns, m.first) && !blank_seat(ns, m.first)) continue;   // that seat already holds a real score
            ns.points.erase(m.first); ns.firsts.erase(m.first); ns.gain.erase(m.first); ns.names.erase(m.first);   // an empty row made for the newcomer
            move_slot(&ns, g, m.first);
            break;
        }
    }
    /* 3. everyone present has a row from the first round on */
    if (ns.phase != Phase::Off && ns.phase != Phase::Lobby)
        for (const auto& m : members) {
            if (m.first >= kGhostBase) continue;
            if (!ns.points.count(m.first)) ns.points[m.first] = 0;
            if (!ns.names.count(m.first)) ns.names[m.first] = utf8_cut(m.second, kMaxName);
        }
    if (ns.points == s.points && ns.names == s.names && ns.firsts == s.firsts && ns.gain == s.gain && ns.votes == s.votes) return s;
    return bump(sanitize(ns));
}

State on_host_change(const State& s, int from) {
    if (from <= 0 || from >= kGhostBase) return s;
    State ns = s;
    /* the old host (slot 0) is gone: its points stay in the standings under a ghost slot */
    if (has_data(ns, 0)) {
        const int g = free_ghost(ns);
        if (g >= 0) move_slot(&ns, 0, g);
    }
    ns.points.erase(0); ns.firsts.erase(0); ns.gain.erase(0); ns.names.erase(0);   // whatever is left of the old host's seat
    ns.votes.erase(0);
    move_slot(&ns, from, 0);
    std::map<int, std::string> nv;   // voters move with their seat; in a "who won?" poll the picks are seats too
    for (const auto& kv : ns.votes) {
        std::string pick = kv.second;
        if (ns.poll == "winner") {
            if (pick == "0") continue;                          // a vote for the player who left
            if (pick == std::to_string(from)) pick = "0";
        }
        nv[kv.first == from ? 0 : kv.first] = pick;
    }
    ns.votes = nv;
    std::vector<int> pl;
    for (int p : ns.players) { if (p == 0) continue; pl.push_back(p == from ? 0 : p); }
    ns.players = clean_slots(pl);
    if (ns.poll == "winner") {
        std::vector<std::string> c;
        for (const std::string& x : ns.cand) {
            if (x == "0") continue;
            c.push_back(x == std::to_string(from) ? "0" : x);
        }
        ns.cand = c;
    }
    return ns;   // not bumped: every PC applies this at the same HostChange event; the new host's rebroadcast() carries the new rev
}

/* ------------------------------------------------------------------ mixed versions */

bool peer_can_party(const std::string& peer_app, const std::string& my_app) {
    if (peer_app.empty()) return false;   // a build that sends no version at all is older than Party mode
    std::string floor = kMinApp;
    if (!my_app.empty() && hubui::compare_app_versions(my_app, kMinApp) == -1) floor = my_app;   // a development build before the release bump
    const int c = hubui::compare_app_versions(peer_app, floor);
    return c == 0 || c == 1;
}

std::string needs_update_text(const std::vector<std::string>& names) {
    if (names.empty()) return std::string();
    return join_names(names) + (names.size() == 1 ? " needs" : " need") + " to update for Party mode";
}

}  // namespace party
}  // namespace hubui
