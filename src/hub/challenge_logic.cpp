#include "challenge_logic.h"

#include "json.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <random>

namespace hub::challenge {

namespace {

const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string b64_encode(const std::string& in) {
    std::string o;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8) | (unsigned char)in[i + 2];
        o += kB64[v >> 18]; o += kB64[(v >> 12) & 63]; o += kB64[(v >> 6) & 63]; o += kB64[v & 63];
    }
    if (i + 1 == in.size()) {
        unsigned v = (unsigned char)in[i] << 16;
        o += kB64[v >> 18]; o += kB64[(v >> 12) & 63];
    } else if (i + 2 == in.size()) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8);
        o += kB64[v >> 18]; o += kB64[(v >> 12) & 63]; o += kB64[(v >> 6) & 63];
    }
    return o;
}
bool b64_decode(const std::string& in, std::string* out) {
    out->clear();
    unsigned acc = 0; int bits = 0;
    for (char c : in) {
        const char* p = c ? std::char_traits<char>::find(kB64, 64, c) : nullptr;
        if (!p) return false;
        acc = (acc << 6) | (unsigned)(p - kB64);
        bits += 6;
        if (bits >= 8) { bits -= 8; out->push_back((char)((acc >> bits) & 0xFF)); }
    }
    return in.size() % 4 != 1;
}

/* Cut to at most n code points (never inside a UTF-8 sequence); also drops control characters. */
std::string cut(const std::string& s, size_t n) {
    std::string o;
    size_t chars = 0, i = 0;
    while (i < s.size() && chars < n) {
        unsigned char c = s[i];
        size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (i + len > s.size()) break;
        if (c >= 32 && c != 127) o.append(s, i, len);
        i += len; chars++;
    }
    return o;
}
std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::string att_json(const hubnet::Attach& a, size_t name_max) {
    return std::string("{\"i\":") + json::quote(a.id) + ",\"n\":" + json::quote(cut(a.name, name_max)) + ",\"h\":" + json::quote(a.sha)
         + ",\"z\":" + std::to_string(a.size) + ",\"x\":" + std::to_string(a.expires) + "}";
}
bool att_from(const json::Value* v, const char* kind, hubnet::Attach* a) {
    if (!v || !v->is_object()) return false;
    a->id = v->str("i"); a->name = cut(v->str("n"), 64); a->sha = v->str("h"); a->kind = kind;
    a->size = v->i64("z"); a->expires = v->i64("x");
    if (a->id.size() != 32 || a->size < 0 || a->size > 64 * 1024 * 1024) return false;
    for (char c : a->id) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    if (!a->sha.empty() && a->sha.size() != 64) return false;
    return true;
}

std::string build(const Card& c, size_t title_max, size_t note_max, size_t name_max, bool with_shot) {
    std::string j = "{\"v\":1,\"k\":\"";
    j += c.kind == CardKind::Challenge ? "c" : "r";
    j += "\",\"i\":" + json::quote(cut(c.id, 32));
    if (c.kind == CardKind::Challenge) {
        j += ",\"g\":" + json::quote(cut(c.game_key, 64)) + ",\"t\":" + json::quote(cut(c.game_title, title_max))
           + ",\"s\":" + json::quote(cut(c.system, 16)) + ",\"r\":" + json::quote(cut(c.rom_key, 64));
        if (!c.note.empty() && note_max) j += ",\"n\":" + json::quote(cut(c.note, note_max));
        j += ",\"f\":" + json::quote(cut(c.sender, name_max));
        j += ",\"gk\":" + std::to_string((int)c.goal.kind) + ",\"gt\":" + std::to_string(c.goal.target);
        j += ",\"st\":" + att_json(c.state, 40);
        if (with_shot && c.has_shot) j += ",\"sh\":" + att_json(c.shot, 40);
    } else {
        j += ",\"p\":" + json::quote(cut(c.player, name_max)) + ",\"o\":" + std::to_string(c.outcome == Outcome::Done ? 0 : 1)
           + ",\"tm\":" + std::to_string(c.time_ms) + ",\"sc\":" + std::to_string(c.score) + ",\"a\":" + std::to_string(c.at);
    }
    j += "}";
    return std::string(kCardPrefix) + b64_encode(j);
}

} // namespace

bool looks_like_card(const std::string& t) { return t.size() > 5 && t.compare(0, 5, kCardPrefix) == 0; }

std::string encode_card(const Card& c) {
    struct Try { size_t title, note, name; bool shot; };
    const Try tries[] = {{64, 80, 24, true}, {40, 60, 20, true}, {24, 30, 16, true}, {24, 0, 16, true}, {16, 0, 12, false}};
    for (const Try& t : tries) {
        std::string s = build(c, t.title, t.note, t.name, t.shot);
        if (s.size() <= hubnet::kCardMax) return s;
    }
    return "";
}

bool decode_card(const std::string& text, Card* out) {
    if (!out || !looks_like_card(text) || text.size() > hubnet::kCardMax) return false;
    std::string js;
    if (!b64_decode(text.substr(5), &js)) return false;
    json::Value v;
    json::ParseOptions opt; opt.max_depth = 6;
    if (!json::parse(js, &v, opt) || !v.is_object()) return false;
    if (v.i64("v", 0) != 1) return false;
    Card c;
    std::string k = v.str("k");
    c.id = v.str("i");
    if (c.id.empty() || c.id.size() > 32) return false;
    for (char ch : c.id) if (!std::isalnum((unsigned char)ch)) return false;
    if (k == "c") {
        c.kind = CardKind::Challenge;
        c.game_key = cut(v.str("g"), 64); c.game_title = cut(v.str("t"), 64); c.system = cut(v.str("s"), 16); c.rom_key = cut(v.str("r"), 64);
        c.note = cut(v.str("n"), 80); c.sender = cut(v.str("f"), 24);
        int64_t gk = v.i64("gk", 1);
        if (gk < 0 || gk > 2) return false;
        c.goal.kind = (GoalKind)gk;
        c.goal.target = std::max<int64_t>(0, v.i64("gt", 0));
        if (!att_from(v.get("st"), "state", &c.state)) return false;
        c.has_shot = att_from(v.get("sh"), "shot", &c.shot);
        if (c.game_key.empty()) return false;
    } else if (k == "r") {
        c.kind = CardKind::Result;
        c.player = cut(v.str("p"), 24);
        c.outcome = v.i64("o", 0) == 1 ? Outcome::GaveUp : Outcome::Done;
        c.time_ms = v.i64("tm", -1); c.score = v.i64("sc", -1); c.at = v.i64("a", 0);
        if (c.time_ms < -1 || c.score < -1) return false;
    } else return false;
    *out = c;
    return true;
}

std::string goal_text(const Goal& g) {
    switch (g.kind) {
        case GoalKind::FastestTime: return "Fastest time";
        case GoalKind::Score: return g.target > 0 ? "Score " + format_score(g.target) : "Score";
        default: return "Just beat it";
    }
}

std::string card_fallback_text(const Card& c) {
    if (c.kind == CardKind::Challenge) return "Challenge: " + goal_text(c.goal) + " in " + (c.game_title.empty() ? c.game_key : c.game_title);
    Goal g;
    g.kind = c.time_ms >= 0 ? GoalKind::FastestTime : c.score >= 0 ? GoalKind::Score : GoalKind::JustBeatIt;
    return "Result: " + result_text(c, g);
}

std::string new_challenge_id() {
    static std::random_device rd;
    std::mt19937_64 g(((uint64_t)rd() << 32) ^ rd());
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)g());
    return buf;
}

std::string format_time(int64_t ms) {
    if (ms < 0) ms = 0;
    int64_t cs = (ms / 10) % 100, s = (ms / 1000) % 60, m = (ms / 60000) % 60, h = ms / 3600000;
    char buf[48];
    if (h > 0) std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld.%02lld", (long long)h, (long long)m, (long long)s, (long long)cs);
    else std::snprintf(buf, sizeof buf, "%lld:%02lld.%02lld", (long long)m, (long long)s, (long long)cs);
    return buf;
}

std::string format_score(int64_t score) {
    if (score < 0) score = 0;
    std::string d = std::to_string(score), o;
    for (size_t i = 0; i < d.size(); i++) {
        if (i && (d.size() - i) % 3 == 0) o += ',';
        o += d[i];
    }
    return o;
}

std::string osd_timer_text(int64_t ms) {
    if (ms < 0) ms = 0;
    int64_t t = (ms / 100) % 10, s = (ms / 1000) % 60, m = (ms / 60000) % 60, h = ms / 3600000;
    char buf[48];
    if (h > 0) std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", (long long)h, (long long)m, (long long)s);
    else std::snprintf(buf, sizeof buf, "%lld:%02lld.%lld", (long long)m, (long long)s, (long long)t);
    return buf;
}

std::string result_text(const Card& r, const Goal& goal) {
    if (r.outcome == Outcome::GaveUp) return "Gave up";
    if (goal.kind == GoalKind::FastestTime && r.time_ms >= 0) return format_time(r.time_ms);
    if (goal.kind == GoalKind::Score && r.score >= 0) return format_score(r.score);
    if (r.time_ms >= 0 && goal.kind != GoalKind::Score) return format_time(r.time_ms);
    if (r.score >= 0) return format_score(r.score);
    return "Done";
}

std::vector<Row> leaderboard(const Goal& goal, const std::string& id, const std::vector<Card>& results, size_t cap) {
    auto has_value = [&](const Card& c) {
        return goal.kind == GoalKind::FastestTime ? c.time_ms >= 0 : goal.kind == GoalKind::Score ? c.score >= 0 : true;
    };
    // better(a, b): a is a better finish than b (both Done)
    auto better = [&](const Card& a, const Card& b) {
        bool va = has_value(a), vb = has_value(b);
        if (va != vb) return va;
        if (goal.kind == GoalKind::FastestTime && va && a.time_ms != b.time_ms) return a.time_ms < b.time_ms;
        if (goal.kind == GoalKind::Score && va && a.score != b.score) return a.score > b.score;
        if (a.at != b.at) return a.at < b.at;
        return false;
    };
    std::vector<Card> best;   // one per player
    for (const Card& c : results) {
        if (c.kind != CardKind::Result || c.id != id) continue;
        std::string key = lower(trim(c.player));
        auto it = std::find_if(best.begin(), best.end(), [&](const Card& b) { return lower(trim(b.player)) == key; });
        if (it == best.end()) { best.push_back(c); continue; }
        bool replace;
        if (it->outcome != c.outcome) replace = c.outcome == Outcome::Done;
        else if (c.outcome == Outcome::GaveUp) replace = c.at > it->at;
        else replace = better(c, *it);
        if (replace) *it = c;
    }
    std::vector<Card> done, gave;
    for (const Card& c : best) (c.outcome == Outcome::Done ? done : gave).push_back(c);
    std::stable_sort(done.begin(), done.end(), [&](const Card& a, const Card& b) {
        if (better(a, b)) return true;
        if (better(b, a)) return false;
        return lower(a.player) < lower(b.player);
    });
    std::stable_sort(gave.begin(), gave.end(), [](const Card& a, const Card& b) { return a.at < b.at; });
    std::vector<Row> rows;
    for (size_t i = 0; i < done.size(); i++) {
        Row r;
        r.player = done[i].player; r.text = result_text(done[i], goal); r.at = done[i].at;
        bool tie = false;
        if (i > 0 && goal.kind != GoalKind::JustBeatIt && has_value(done[i]) && has_value(done[i - 1]))
            tie = goal.kind == GoalKind::FastestTime ? done[i].time_ms == done[i - 1].time_ms : done[i].score == done[i - 1].score;
        r.rank = tie ? rows[i - 1].rank : (int)i + 1;
        rows.push_back(r);
    }
    for (const Card& c : gave) {
        Row r; r.rank = 0; r.player = c.player; r.text = "Gave up"; r.gave_up = true; r.at = c.at;
        rows.push_back(r);
    }
    if (rows.size() > cap) rows.resize(cap);
    return rows;
}

RomCheck rom_check(const std::string& mine, const std::string& theirs) {
    if (mine.empty() || theirs.empty()) return RomCheck::Different;
    return lower(mine) == lower(theirs) ? RomCheck::Same : RomCheck::Different;
}
std::string rom_check_message(const std::string& t) {
    return "You need the same game (" + (t.empty() ? std::string("this one") : t) + ") to take this challenge.";
}

bool runner_supports_challenge(const std::string& system) {
    static const char* no[] = {"gc", "gcn", "ngc", "wii", "wiiu", "3ds", "n3ds", "ps2", "pc", "native", "smash", ""};
    std::string s = lower(system);
    for (const char* n : no) if (s == n) return false;
    return true;
}

bool challenge_state_ok(int64_t bytes, std::string* why) {
    if (bytes <= 0) { if (why) *why = "The game didn't save a state."; return false; }
    if (bytes > kMaxStateBytes) {
        if (why) *why = "This game's save state is too big to send (limit 8 MB). Challenges don't work with DS and PS1 games.";
        return false;
    }
    return true;
}

bool challenge_eligible(const std::string& system, int64_t bytes, std::string* why) {
    if (!runner_supports_challenge(system)) { if (why) *why = "Challenges aren't available for this console."; return false; }
    if (bytes > 0 || bytes < 0) return challenge_state_ok(bytes, why);
    return true;   // size unknown yet (0): checked after the state is saved
}

std::string hardcore_note() { return "RetroAchievements hardcore is turned off for this run (loading a save state isn't allowed in hardcore)."; }
std::string offline_only_note() { return "Challenges are played offline, on your own."; }

namespace {
std::string safe_id(const std::string& id) {
    std::string o;
    for (char c : id) if (std::isalnum((unsigned char)c)) o += (char)std::tolower((unsigned char)c);
    return o.empty() ? "x" : o.substr(0, 32);
}
}

std::string scratch_dir(const std::string& d) {
    std::string b = d;
    while (!b.empty() && (b.back() == '\\' || b.back() == '/')) b.pop_back();
    return b + "\\challenge\\";
}
std::string scratch_state_name(const std::string& id, bool received) { return "ch-" + safe_id(id) + (received ? "-recv.state" : "-send.state"); }
std::string scratch_shot_name(const std::string& id) { return "ch-" + safe_id(id) + ".png"; }
std::string scratch_path(const std::string& d, const std::string& name) {
    std::string f;
    for (char c : name) if (std::isalnum((unsigned char)c) || c == '-' || c == '.' || c == '_') f += c;
    for (size_t p = f.find(".."); p != std::string::npos; p = f.find("..")) f.replace(p, 2, "_");
    if (f.empty() || f[0] == '.') f = "x" + f;
    return scratch_dir(d) + f;
}

} // namespace hub::challenge
