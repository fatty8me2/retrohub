#pragma once
/* Game night (Plan 3 S6): pure logic, no ImGui and no network. Unit tested in test_gamenight.cpp.
 *
 * "Plan a session" sends each invited friend an ordinary direct message through the existing DM inbox. The message ends with a
 * machine-readable tag, so an older RetroHub just shows a readable sentence:
 *
 *   Game night: Mario Kart 64 on Fri 8:00 PM - open RetroHub to join  [RHGN1|<start unix seconds>|<game id>|<title>]
 *
 * The receiving client parses the tag and stores the plan locally (gamenights.json in the user-data dir; no server scheduler or
 * alarms). A reminder shows 10 minutes before the start, and at the start time a Host / Join card appears. Nothing here talks
 * to the Worker, so it costs no REQ_BUDGET beyond the one DM per invited friend. */
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "json.h"

namespace hubui {
namespace night {

constexpr const char* kTag = "RHGN1";
constexpr int64_t kReminderMs = 10LL * 60 * 1000;      // reminder lead time
constexpr int64_t kLiveMs = 90LL * 60 * 1000;          // the Host / Join card stays offered this long after the start
constexpr int64_t kMaxAheadMs = 14LL * 24 * 3600 * 1000;
constexpr size_t kMaxNights = 20;
constexpr size_t kMaxInvitees = 8;                     // fan-out cap per plan (one DM each)
constexpr int64_t kMinLeadMs = 60LL * 1000;            // a plan must start at least a minute from now

struct Night {
    std::string id;                      // "<host uid>@<start seconds>"
    std::string host_uid, host_name;     // who planned it (for our own plans: our uid / "" )
    std::string game_id, game_title;
    int64_t start_ms = 0;
    bool mine = false;                   // we planned it
    std::vector<std::string> invitees;   // mine only: friend uids the plan was sent to
    bool reminded = false;               // reminder shown / put in the bell
    bool started = false;                // start card shown / put in the bell
    bool dismissed = false;
};

enum class Phase { Upcoming, Reminder, Live, Over };

inline Phase phase_of(const Night& n, int64_t now) {
    if (n.dismissed || now >= n.start_ms + kLiveMs) return Phase::Over;
    if (now >= n.start_ms) return Phase::Live;
    if (now >= n.start_ms - kReminderMs) return Phase::Reminder;
    return Phase::Upcoming;
}
/* A reminder is due once (also when the PC was off at that moment and the night has already begun). */
inline bool reminder_due(const Night& n, int64_t now) {
    Phase p = phase_of(n, now);
    return !n.reminded && (p == Phase::Reminder || p == Phase::Live);
}
inline bool start_due(const Night& n, int64_t now) { return !n.started && phase_of(n, now) == Phase::Live; }

/* "in 8 min", "in 2 hours", "now", "started 12 min ago" */
inline std::string when_text(const Night& n, int64_t now) {
    int64_t d = n.start_ms - now;
    auto u = [](int64_t v, const char* w) { return std::to_string(v) + " " + w + (v == 1 ? "" : "s"); };
    if (d <= 0) {
        int64_t m = (-d) / 60000;
        return m < 1 ? "now" : "started " + u(m, "min") + " ago";
    }
    int64_t m = (d + 59999) / 60000;
    if (m < 60) return "in " + u(m, "min");
    if (m < 48 * 60) return "in " + u(m / 60, "hour");
    return "in " + u(m / (24 * 60), "day");
}

/* The game id and title go into a '|' separated tag that ends at ']': keep them plain. */
inline std::string clean(const std::string& s, size_t max) {
    std::string o;
    for (unsigned char c : s) {
        if (c < 0x20 || c == '|' || c == '[' || c == ']') c = ' ';
        o += (char)c;
        if (o.size() >= max) break;
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}
inline std::string clean_id(const std::string& s) {
    std::string o;
    for (char c : s) if (std::isalnum((unsigned char)c) || c == '_' || c == '.' || c == ':' || c == '-') o += c;
    return o.substr(0, 80);
}

/* `when_label` = the start time in the sender's local time, as a readable phrase ("Fri 8:00 PM"). */
inline std::string encode(const Night& n, const std::string& when_label) {
    std::string title = clean(n.game_title, 60);
    return "Game night: " + (title.empty() ? std::string("a session") : title) + " on " + clean(when_label, 30) +
           " - open RetroHub to join  [" + kTag + "|" + std::to_string(n.start_ms / 1000) + "|" + clean_id(n.game_id) + "|" + title + "]";
}

/* Reads the tag at the end of a DM. Fills game/title/start; the caller sets host_uid / host_name / id. */
inline bool parse(const std::string& text, Night* out) {
    size_t a = text.rfind(std::string("[") + kTag + "|");
    if (a == std::string::npos) return false;
    size_t b = text.find(']', a);
    if (b == std::string::npos) return false;
    std::string body = text.substr(a + 1, b - a - 1);
    std::vector<std::string> f;
    size_t pos = 0;
    for (int i = 0; i < 3; ++i) {
        size_t q = body.find('|', pos);
        if (q == std::string::npos) return false;
        f.push_back(body.substr(pos, q - pos));
        pos = q + 1;
    }
    f.push_back(body.substr(pos));
    if (f[1].empty() || f[1].size() > 11) return false;
    int64_t secs = 0;
    for (char c : f[1]) { if (c < '0' || c > '9') return false; secs = secs * 10 + (c - '0'); }
    if (secs <= 0) return false;
    Night n;
    n.start_ms = secs * 1000;
    n.game_id = clean_id(f[2]);
    n.game_title = clean(f[3], 60);
    *out = n;
    return true;
}
/* The conversation shows the readable sentence only. */
inline std::string display_text(const std::string& text) {
    Night n;
    if (!parse(text, &n)) return text;
    size_t a = text.rfind(std::string("[") + kTag + "|");
    std::string s = text.substr(0, a);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

inline std::string make_id(const std::string& host_uid, int64_t start_ms) { return host_uid + "@" + std::to_string(start_ms / 1000); }

/* Validation when planning: a minute or more ahead, at most two weeks, a game, at least one friend (at most kMaxInvitees). */
inline std::string plan_error(const Night& n, int64_t now) {
    if (n.invitees.empty()) return "Pick at least one friend.";
    if (n.invitees.size() > kMaxInvitees) return "Pick up to " + std::to_string(kMaxInvitees) + " friends.";
    if (n.game_id.empty()) return "Pick a game.";
    if (n.start_ms < now + kMinLeadMs) return "Pick a time in the future.";
    if (n.start_ms > now + kMaxAheadMs) return "Pick a time within the next two weeks.";
    return "";
}

struct Plans {
    std::vector<Night> v;   // soonest first

    /* A plan already stored (same host and start) is left alone, so a re-delivered DM never re-triggers the reminder. */
    bool add(const Night& n, int64_t now) {
        if (n.id.empty() || phase_of(n, now) == Phase::Over) return false;
        for (auto& o : v) if (o.id == n.id) return false;
        v.push_back(n);
        sort_trim(now);
        return true;
    }
    void sort_trim(int64_t now) {
        v.erase(std::remove_if(v.begin(), v.end(), [&](const Night& n) { return n.start_ms + kLiveMs + 24LL * 3600 * 1000 < now; }), v.end());
        std::stable_sort(v.begin(), v.end(), [](const Night& a, const Night& b) { return a.start_ms < b.start_ms; });
        if (v.size() > kMaxNights) v.resize(kMaxNights);
    }
    Night* find(const std::string& id) { for (auto& n : v) if (n.id == id) return &n; return nullptr; }
    /* Upcoming or live plans, soonest first. */
    std::vector<const Night*> active(int64_t now) const {
        std::vector<const Night*> r;
        for (auto& n : v) if (phase_of(n, now) != Phase::Over) r.push_back(&n);
        return r;
    }
};

inline std::string q_(const std::string& s) { return "\"" + json::escape(s) + "\""; }

inline std::string to_json(const Plans& p) {
    std::ostringstream o;
    o << "{\"nights\":[";
    bool first = true;
    for (auto& n : p.v) {
        o << (first ? "" : ",") << "{\"id\":" << q_(n.id) << ",\"host_uid\":" << q_(n.host_uid) << ",\"host_name\":" << q_(n.host_name)
          << ",\"game_id\":" << q_(n.game_id) << ",\"title\":" << q_(n.game_title) << ",\"start\":" << n.start_ms
          << ",\"mine\":" << (n.mine ? "true" : "false") << ",\"reminded\":" << (n.reminded ? "true" : "false")
          << ",\"started\":" << (n.started ? "true" : "false") << ",\"dismissed\":" << (n.dismissed ? "true" : "false") << ",\"invitees\":[";
        for (size_t i = 0; i < n.invitees.size(); ++i) o << (i ? "," : "") << q_(n.invitees[i]);
        o << "]}";
        first = false;
    }
    o << "]}";
    return o.str();
}

inline bool from_json(const std::string& text, Plans* out) {
    json::Value j;
    if (!json::parse(text, &j) || j.type != json::Value::Object) return false;
    Plans p;
    if (const json::Value* a = j.get("nights")) if (a->type == json::Value::Array) for (auto& e : a->a) {
        if (e.type != json::Value::Object) continue;
        Night n;
        n.id = e.str("id"); n.host_uid = e.str("host_uid"); n.host_name = e.str("host_name");
        n.game_id = clean_id(e.str("game_id")); n.game_title = clean(e.str("title"), 60);
        n.start_ms = (int64_t)e.num("start", 0);
        auto b = [&](const char* k) { const json::Value* x = e.get(k); return x && x->type == json::Value::Bool && x->b; };
        n.mine = b("mine"); n.reminded = b("reminded"); n.started = b("started"); n.dismissed = b("dismissed");
        if (const json::Value* iv = e.get("invitees")) if (iv->type == json::Value::Array) for (auto& u : iv->a) if (u.type == json::Value::String && n.invitees.size() < kMaxInvitees) n.invitees.push_back(u.s);
        if (n.id.empty() || n.start_ms <= 0) continue;
        p.v.push_back(n);
    }
    std::stable_sort(p.v.begin(), p.v.end(), [](const Night& a, const Night& b) { return a.start_ms < b.start_ms; });
    if (p.v.size() > kMaxNights) p.v.resize(kMaxNights);
    *out = p;
    return true;
}

inline bool load(const std::string& path, Plans* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    return from_json(ss.str(), out);
}
inline bool save(const std::string& path, const Plans& p) {
    std::error_code ec;
    std::filesystem::path pp(path);
    if (pp.has_parent_path()) std::filesystem::create_directories(pp.parent_path(), ec);
    std::string tmp = path + ".tmp";
    { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); if (!f) return false; f << to_json(p); if (!f) return false; }
    std::filesystem::rename(tmp, path, ec);
    if (ec) { std::filesystem::remove(path, ec); std::filesystem::rename(tmp, path, ec); }
    return !ec;
}

}  // namespace night
}  // namespace hubui
