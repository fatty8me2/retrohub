#pragma once
/* Game night (Plan 3 S6): pure logic, no ImGui and no network. Unit tested in test_gamenight.cpp.
 *
 * "Plan a session" sends each invited friend an ordinary direct message through the existing DM inbox. The message ends with a
 * machine-readable tag, so an older RetroHub just shows a readable sentence:
 *
 *   Game night: Mario Kart 64 on Fri 8:00 PM - open RetroHub to join  [RHGN1|<start unix seconds>|<game id>|<title>]
 *
 * The receiving client parses the tag and stores the plan locally (gamenights.json in the user-data dir; no server scheduler or
 * alarms). A reminder shows 10 minutes before the start (in RetroHub while it is open; as a Windows scheduled toast when it is closed, see
 * gamenight_sched.h), and at the start time a Host / Join card appears. Nothing here talks
 * to the Worker, so it costs no REQ_BUDGET beyond the one DM per invited friend. */
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
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
    bool toast_sched = false;            // a Windows scheduled toast is pending for the reminder (gamenight_sched.h)
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
          << ",\"started\":" << (n.started ? "true" : "false") << ",\"dismissed\":" << (n.dismissed ? "true" : "false") << ",\"toast\":" << (n.toast_sched ? "true" : "false") << ",\"invitees\":[";
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
        n.mine = b("mine"); n.reminded = b("reminded"); n.started = b("started"); n.dismissed = b("dismissed"); n.toast_sched = b("toast");
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

/* ---- "Add to calendar": a standard iCalendar (RFC 5545) file for one game night ----
 * Plans are single events (no recurrence in the model), so there is no RRULE. UTC times, CRLF line ends, lines folded at 75 octets
 * (never inside a UTF-8 character), TEXT values escaped. */
constexpr int64_t kIcsDurationMs = 90LL * 60 * 1000;   // same window the Host / Join card stays offered
constexpr int kIcsAlarmMinutes = 15;

/* RFC 5545 TEXT escaping: backslash, semicolon, comma, newline. Other control characters become spaces. */
inline std::string ics_escape(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\') o += "\\\\";
        else if (c == ';') o += "\\;";
        else if (c == ',') o += "\\,";
        else if (c == '\r') { if (i + 1 < s.size() && s[i + 1] == '\n') ++i; o += "\\n"; }
        else if (c == '\n') o += "\\n";
        else if (c < 0x20 || c == 0x7f) o += ' ';
        else o += (char)c;
    }
    return o;
}

/* Folds a content line (no line end) at 75 octets: each continuation starts with one space. Returns CRLF-terminated text. */
inline std::string ics_fold(const std::string& line) {
    std::string o;
    size_t pos = 0, limit = 75;   // octets allowed on this physical line, including the leading space of continuations
    bool first = true;
    while (pos < line.size()) {
        size_t room = first ? limit : limit - 1;
        size_t take = std::min(room, line.size() - pos);
        if (pos + take < line.size())   // do not cut a multi-byte character: back up to a character start
            while (take > 0 && ((unsigned char)line[pos + take] & 0xC0) == 0x80) --take;
        if (take == 0) take = std::min(room, line.size() - pos);   // malformed input: still make progress
        if (!first) o += ' ';
        o.append(line, pos, take);
        o += "\r\n";
        pos += take;
        first = false;
    }
    if (line.empty()) o = "\r\n";
    return o;
}

/* "YYYYMMDDTHHMMSSZ" for unix seconds (proleptic Gregorian, no locale / timezone). */
inline std::string ics_utc(int64_t secs) {
    int64_t days = secs / 86400, rem = secs % 86400;
    if (rem < 0) { rem += 86400; --days; }
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int d = (int)(doy - (153 * mp + 2) / 5 + 1);
    int m = (int)(mp < 10 ? mp + 3 : mp - 9);
    if (m <= 2) ++y;
    char b[32];
    std::snprintf(b, sizeof b, "%04d%02d%02dT%02d%02d%02dZ", (int)y, m, d, (int)(rem / 3600), (int)(rem % 3600 / 60), (int)(rem % 60));
    return b;
}

/* SHA-256 (lower-case hex). Used so the calendar file carries no account id: the UID and file name only hold a hash. */
inline std::string sha256_hex(const std::string& msg) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string m = msg;
    const uint64_t bits = (uint64_t)msg.size() * 8;
    m += (char)0x80;
    while (m.size() % 64 != 56) m += (char)0;
    for (int i = 7; i >= 0; --i) m += (char)((bits >> (i * 8)) & 0xff);
    auto rr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = ((uint32_t)(unsigned char)m[off + i * 4] << 24) | ((uint32_t)(unsigned char)m[off + i * 4 + 1] << 16) | ((uint32_t)(unsigned char)m[off + i * 4 + 2] << 8) | (uint32_t)(unsigned char)m[off + i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rr(w[i - 15], 7) ^ rr(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = rr(w[i - 2], 17) ^ rr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t t1 = hh + (rr(e, 6) ^ rr(e, 11) ^ rr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            uint32_t t2 = (rr(a, 2) ^ rr(a, 13) ^ rr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    char buf[65];
    for (int i = 0; i < 8; ++i) std::snprintf(buf + i * 8, 9, "%08x", (unsigned)h[i]);
    return buf;
}

/* A stable id with no personal data: a hash of the night's id (which holds the host's account id), so importing the same night twice
 * updates the calendar entry instead of duplicating it, and the file never shows the account id. */
inline std::string night_hash(const Night& n) {
    return sha256_hex("retrohub-gamenight:" + (n.id.empty() ? std::to_string(n.start_ms / 1000) : n.id));
}
inline std::string ics_uid(const Night& n) { return "gamenight-" + night_hash(n).substr(0, 32) + "@retrohub"; }

/* now_ms only feeds DTSTAMP. */
inline std::string make_ics(const Night& n, int64_t now_ms) {
    const std::string title = n.game_title.empty() ? std::string("a session") : n.game_title;
    std::string desc = "Game night in RetroHub: " + title + ".";
    if (!n.mine && !n.host_name.empty()) desc += "\nPlanned by " + n.host_name + ".";
    desc += "\nOpen RetroHub a few minutes before the start to host or join.";
    std::string o;
    auto L = [&](const std::string& l) { o += ics_fold(l); };
    L("BEGIN:VCALENDAR");
    L("VERSION:2.0");
    L("PRODID:-//RetroHub//Game night//EN");
    L("CALSCALE:GREGORIAN");
    L("BEGIN:VEVENT");
    L("UID:" + ics_uid(n));
    L("DTSTAMP:" + ics_utc(now_ms / 1000));
    L("DTSTART:" + ics_utc(n.start_ms / 1000));
    L("DTEND:" + ics_utc((n.start_ms + kIcsDurationMs) / 1000));
    L("SUMMARY:" + ics_escape("Game night: " + title));
    L("DESCRIPTION:" + ics_escape(desc));
    L("BEGIN:VALARM");
    L("ACTION:DISPLAY");
    L("TRIGGER:-PT" + std::to_string(kIcsAlarmMinutes) + "M");
    L("DESCRIPTION:" + ics_escape("Game night starts in " + std::to_string(kIcsAlarmMinutes) + " minutes: " + title));
    L("END:VALARM");
    L("END:VEVENT");
    L("END:VCALENDAR");
    return o;
}

/* "game-night-20261112-2000-1a2b3c4d.ics": UTC start plus a short hash, so two nights (even at the same minute) never share a name. */
inline std::string ics_file_name(const Night& n) {
    const std::string t = ics_utc(n.start_ms / 1000);   // YYYYMMDDTHHMMSSZ
    return "game-night-" + t.substr(0, 8) + "-" + t.substr(9, 4) + "-" + night_hash(n).substr(0, 8) + ".ics";
}

}  // namespace night
}  // namespace hubui
