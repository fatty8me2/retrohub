#pragma once
/* Pure logic for "play again / recent groups" and the notification history (no ImGui, no network). Unit tested in test_group.cpp.
 *
 *  - RecentGroups: the last 5 online sessions (members + game + time). Stored as recent_groups.json in the user-data
 *    dir AND the vault, so it follows the user between PCs (load merges both: union by session, newest first).
 *  - NotifHistory: the bell's history (last 50, persisted the same way). Actionable entries stay actionable while
 *    valid (an invite until its room expires, a friend request until answered) and then show an expired/done state. */
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "json.h"

namespace hubui {
namespace group {

constexpr size_t kMaxGroups = 5;
constexpr size_t kMaxNotifs = 50;
constexpr int64_t kDayMs = 24LL * 60 * 60 * 1000;

/* "just now", "5 min ago", "3 hours ago", "yesterday", "2 days ago", "3 weeks ago", "5 months ago". */
inline std::string ago_text(int64_t at, int64_t now) {
    if (at <= 0) return "";
    int64_t d = now - at;
    if (d < 0) d = 0;
    int64_t mins = d / 60000, hours = d / 3600000, days = d / kDayMs;
    auto plural = [](int64_t n, const char* u) { return std::to_string(n) + " " + u + (n == 1 ? "" : "s") + " ago"; };
    if (mins < 1) return "just now";
    if (hours < 1) return std::to_string(mins) + " min ago";
    if (days < 1) return plural(hours, "hour");
    if (days == 1) return "yesterday";
    if (days < 14) return plural(days, "day");
    if (days < 60) return plural(days / 7, "week");
    return plural(days / 30, "month");
}

/* ------------------------------------------------------------------ recent groups */
struct Member { std::string uid, name; };
struct RecentGroup {
    std::vector<Member> members;   // everyone except us, friends only (uid non-empty)
    std::string game_id, game_title;
    int64_t at = 0;                // when the session ended (ms since epoch)
};

inline std::string member_key(const RecentGroup& g) {
    std::vector<std::string> u;
    for (auto& m : g.members) u.push_back(m.uid);
    std::sort(u.begin(), u.end());
    std::string k;
    for (auto& s : u) k += s + ",";
    return k;
}

struct RecentGroups {
    std::vector<RecentGroup> v;   // newest first

    /* Records a finished session. Sessions without other friends are ignored. The same people + game replace the older entry. */
    bool record(const RecentGroup& g) {
        if (g.members.empty() || g.at <= 0) return false;
        const std::string k = member_key(g);
        v.erase(std::remove_if(v.begin(), v.end(), [&](const RecentGroup& o) { return member_key(o) == k && o.game_id == g.game_id; }), v.end());
        v.insert(v.begin(), g);
        sort_trim();
        return true;
    }
    void sort_trim() {
        std::stable_sort(v.begin(), v.end(), [](const RecentGroup& a, const RecentGroup& b) { return a.at > b.at; });
        if (v.size() > kMaxGroups) v.resize(kMaxGroups);
    }
    const RecentGroup* last() const { return v.empty() ? nullptr : &v[0]; }
    /* The newest session this friend was part of. */
    const RecentGroup* last_with(const std::string& uid) const {
        for (auto& g : v) for (auto& m : g.members) if (m.uid == uid) return &g;
        return nullptr;
    }
    /* Union of two stores (this PC + the vault copy), newest first, capped. */
    void merge(const RecentGroups& o) {
        for (auto& g : o.v) {
            bool have = false;
            for (auto& x : v) if (x.at == g.at && member_key(x) == member_key(g) && x.game_id == g.game_id) { have = true; break; }
            if (!have) v.push_back(g);
        }
        // same people + game: keep only the newest
        std::stable_sort(v.begin(), v.end(), [](const RecentGroup& a, const RecentGroup& b) { return a.at > b.at; });
        std::vector<RecentGroup> out;
        for (auto& g : v) {
            bool dup = false;
            for (auto& x : out) if (member_key(x) == member_key(g) && x.game_id == g.game_id) { dup = true; break; }
            if (!dup) out.push_back(g);
        }
        v = out;
        sort_trim();
    }
};

/* Who in the room is one of our friends, by ACCOUNT uid (names are only display text).
 * A friend whose presence says they are in this room (`lobby_code` == `room_code`) is a member. Presence can lag at the
 * end of a session, so a room member that no friend's presence explains is matched by name, but only when exactly one
 * friend has that name (usernames are unique per account, old display names may not be) and that friend is not
 * already matched. */
struct FriendRef { std::string uid, name, lobby_code; };
inline std::vector<Member> match_friends(const std::vector<std::string>& other_member_names, const std::vector<FriendRef>& friends, const std::string& room_code) {
    auto low = [](std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; };
    std::vector<Member> out;
    auto have = [&](const std::string& uid) { for (auto& m : out) if (m.uid == uid) return true; return false; };
    if (!room_code.empty())
        for (auto& f : friends) if (!f.uid.empty() && f.lobby_code == room_code && !have(f.uid)) out.push_back({f.uid, f.name});
    for (auto& n : other_member_names) {
        bool explained = false;
        for (auto& m : out) if (low(m.name) == low(n)) { explained = true; break; }
        if (explained) continue;
        const FriendRef* hit = nullptr; int cnt = 0;
        for (auto& f : friends) if (!f.uid.empty() && low(f.name) == low(n)) { hit = &f; ++cnt; }
        if (cnt == 1 && !have(hit->uid)) out.push_back({hit->uid, hit->name});
    }
    return out;
}

/* "Last played together: Mario Kart 64, 2 days ago" ("" when the friend never shared a session with us). */
inline std::string together_text(const RecentGroup* g, int64_t now) {
    if (!g) return "";
    std::string t = "Last played together: ";
    t += g->game_title.empty() ? "a game" : g->game_title;
    std::string a = ago_text(g->at, now);
    if (!a.empty()) t += ", " + a;
    return t;
}
/* "Invite last group (Mia, Jake)" label pieces: the names, shortened to "Mia, Jake +2". */
inline std::string group_names(const RecentGroup& g, size_t max_names = 3) {
    std::string s;
    for (size_t i = 0; i < g.members.size() && i < max_names; ++i) s += (i ? ", " : "") + g.members[i].name;
    if (g.members.size() > max_names) s += " +" + std::to_string(g.members.size() - max_names);
    return s;
}
/* Who from the group may be invited now: still friends (in `friend_uids`) and not already in the room (`in_room_uids`). */
inline std::vector<std::string> invitable(const RecentGroup& g, const std::vector<std::string>& friend_uids, const std::vector<std::string>& in_room_uids) {
    std::vector<std::string> out;
    for (auto& m : g.members) {
        if (std::find(friend_uids.begin(), friend_uids.end(), m.uid) == friend_uids.end()) continue;
        if (std::find(in_room_uids.begin(), in_room_uids.end(), m.uid) != in_room_uids.end()) continue;
        out.push_back(m.uid);
    }
    return out;
}

inline std::string groups_to_json(const RecentGroups& r) {
    json::Value root = json::make_object();
    json::Value arr = json::make_array();
    for (auto& g : r.v) {
        json::Value o = json::make_object();
        o.o.push_back({"game_id", json::make_string(g.game_id)});
        o.o.push_back({"game_title", json::make_string(g.game_title)});
        o.o.push_back({"at", json::make_int(g.at)});
        json::Value ms = json::make_array();
        for (auto& m : g.members) {
            json::Value mo = json::make_object();
            mo.o.push_back({"uid", json::make_string(m.uid)});
            mo.o.push_back({"name", json::make_string(m.name)});
            ms.a.push_back(mo);
        }
        o.o.push_back({"members", ms});
        arr.a.push_back(o);
    }
    root.o.push_back({"version", json::make_int(1)});
    root.o.push_back({"groups", arr});
    return json::stringify(root);
}
inline bool groups_from_json(const std::string& text, RecentGroups* out) {
    json::Value root;
    if (!json::parse(text, &root) || !root.is_object()) return false;
    const json::Value* arr = root.get("groups");
    out->v.clear();
    if (!arr || !arr->is_array()) return true;
    for (auto& o : arr->a) {
        if (!o.is_object()) continue;
        RecentGroup g;
        g.game_id = o.str("game_id");
        g.game_title = o.str("game_title");
        g.at = o.i64("at");
        if (const json::Value* ms = o.get("members"); ms && ms->is_array())
            for (auto& m : ms->a) if (m.is_object() && !m.str("uid").empty()) g.members.push_back({m.str("uid"), m.str("name")});
        if (!g.members.empty() && g.at > 0) out->v.push_back(g);
    }
    out->sort_trim();
    return true;
}

/* ------------------------------------------------------------------ "Notify me when they come online" (per friend, off by default)
 * The setting is a comma separated list of friend uids (Settings::watch_online), at most kWatchMax (the server's cap too). */
constexpr size_t kWatchMax = 50;
inline std::vector<std::string> watch_split(const std::string& csv) {
    std::vector<std::string> v;
    size_t i = 0;
    while (i <= csv.size()) {
        size_t j = csv.find(',', i);
        if (j == std::string::npos) j = csv.size();
        std::string u = csv.substr(i, j - i);
        if (!u.empty() && std::find(v.begin(), v.end(), u) == v.end()) v.push_back(u);
        i = j + 1;
    }
    return v;
}
inline std::string watch_join(const std::vector<std::string>& v) {
    std::string o;
    for (auto& u : v) o += (o.empty() ? "" : ",") + u;
    return o;
}
inline bool watch_has(const std::string& csv, const std::string& uid) {
    auto v = watch_split(csv);
    return std::find(v.begin(), v.end(), uid) != v.end();
}
/* Turns the toggle for `uid` on/off. Turning on at the cap is refused (returns false, list unchanged). `keep` (when non-null) lists the
 * uids that are still friends: anyone else is dropped from the list first, so removed friends never fill the 50 slots. */
inline bool watch_set(std::string* csv, const std::string& uid, bool on, const std::vector<std::string>* keep = nullptr) {
    auto v = watch_split(*csv);
    if (keep) v.erase(std::remove_if(v.begin(), v.end(), [&](const std::string& u) { return u != uid && std::find(keep->begin(), keep->end(), u) == keep->end(); }), v.end());
    auto it = std::find(v.begin(), v.end(), uid);
    if (on) {
        if (it == v.end()) {
            if (v.size() >= kWatchMax) { *csv = watch_join(v); return false; }
            v.push_back(uid);
        }
    } else if (it != v.end()) v.erase(it);
    *csv = watch_join(v);
    return true;
}

/* ------------------------------------------------------------------ notification history */
enum class Kind { Invite, FriendRequest, AskJoin, FriendOnline, CloudConflict, Update, Info };
inline const char* kind_name(Kind k) {
    switch (k) {
        case Kind::Invite: return "invite"; case Kind::FriendRequest: return "request"; case Kind::AskJoin: return "ask_join";
        case Kind::FriendOnline: return "online"; case Kind::CloudConflict: return "conflict"; case Kind::Update: return "update";
        default: return "info";
    }
}
inline Kind kind_from(const std::string& s) {
    for (Kind k : {Kind::Invite, Kind::FriendRequest, Kind::AskJoin, Kind::FriendOnline, Kind::CloudConflict, Kind::Update})
        if (s == kind_name(k)) return k;
    return Kind::Info;
}

struct Entry {
    Kind kind = Kind::Info;
    std::string uid, name;                 // who it is about
    std::string game_id, title, code;      // invite / ask-to-join: the game and room code
    std::string text;                      // Info / Update / CloudConflict: the line to show
    int64_t at = 0;                        // arrival (ms since epoch)
    int64_t expires_at = 0;                // invite / ask-to-join: when the room offer lapses (0 = never)
    bool read = false;
    bool done = false;                     // answered here (joined, accepted, dismissed): shown as history only
};

enum class State { Active, Expired, Done, Info };
/* The facts about the world that decide whether an entry is still actionable. */
struct World {
    int64_t now = 0;
    std::vector<std::string> pending_request_uids;   // incoming friend requests still waiting
    bool update_available = false;
    int conflicts = 0;                               // cloud save conflicts still unresolved
};

inline State state_of(const Entry& e, const World& w) {
    switch (e.kind) {
        case Kind::Invite: case Kind::AskJoin:
            if (e.done) return State::Done;
            return e.expires_at > 0 && w.now >= e.expires_at ? State::Expired : State::Active;
        case Kind::FriendRequest:
            if (e.done) return State::Done;
            return std::find(w.pending_request_uids.begin(), w.pending_request_uids.end(), e.uid) != w.pending_request_uids.end() ? State::Active : State::Expired;
        case Kind::CloudConflict: return w.conflicts > 0 && !e.done ? State::Active : State::Expired;
        case Kind::Update: return w.update_available && !e.done ? State::Active : State::Expired;
        default: return State::Info;
    }
}
inline const char* state_label(State s) {
    switch (s) { case State::Expired: return "Expired"; case State::Done: return "Handled"; default: return ""; }
}
/* What the primary button says ("" = none). */
inline std::string action_label(const Entry& e, State s) {
    if (s != State::Active) return "";
    switch (e.kind) {
        case Kind::Invite: return "Join";
        case Kind::AskJoin: return "Let in";
        case Kind::FriendRequest: return "Accept";
        case Kind::CloudConflict: return "Open Saves";
        case Kind::Update: return "Update";
        default: return "";
    }
}
inline std::string headline(const Entry& e) {
    const std::string who = e.name.empty() ? "A friend" : e.name;
    const std::string game = e.title.empty() ? "a game" : e.title;
    switch (e.kind) {
        case Kind::Invite: return who + " invited you to play " + game;
        case Kind::AskJoin: return who + " asked to join your game";
        case Kind::FriendRequest: return who + " wants to be friends";
        case Kind::FriendOnline: return who + " came online";
        default: return e.text;
    }
}

struct NotifHistory {
    std::vector<Entry> v;   // newest first
    bool dirty = false;

    /* Adds an entry. An older live entry for the same thing (same kind + person + room) is replaced. */
    void add(Entry e) {
        v.erase(std::remove_if(v.begin(), v.end(), [&](const Entry& o) {
            if (o.kind != e.kind) return false;
            switch (e.kind) {
                case Kind::Invite: case Kind::AskJoin: return o.uid == e.uid && o.code == e.code;
                case Kind::FriendRequest: case Kind::FriendOnline: return o.uid == e.uid && (e.kind == Kind::FriendRequest || o.at / 60000 == e.at / 60000);
                case Kind::CloudConflict: case Kind::Update: return o.text == e.text;
                default: return o.text == e.text && o.at == e.at;
            }
        }), v.end());
        v.insert(v.begin(), e);
        std::stable_sort(v.begin(), v.end(), [](const Entry& a, const Entry& b) { return a.at > b.at; });
        if (v.size() > kMaxNotifs) v.resize(kMaxNotifs);
        dirty = true;
    }
    int unread() const { int n = 0; for (auto& e : v) if (!e.read) ++n; return n; }
    /* Actionable and still valid: the number the bell badge adds on top of unread info. */
    int active(const World& w) const { int n = 0; for (auto& e : v) if (state_of(e, w) == State::Active) ++n; return n; }
    void mark_all_read() { for (auto& e : v) if (!e.read) { e.read = true; dirty = true; } }
    void clear() { if (!v.empty()) dirty = true; v.clear(); }
    /* Marks entries for (kind, uid) answered, e.g. after Accept. */
    void resolve(Kind k, const std::string& uid, const std::string& code = std::string()) {
        for (auto& e : v)
            if (e.kind == k && e.uid == uid && (code.empty() || e.code == code) && !e.done) { e.done = true; e.read = true; dirty = true; }
    }
    /* Union with another copy (other PC / vault): same (kind, uid, code, at) = same entry; flags OR together. */
    void merge(const NotifHistory& o) {
        for (auto& g : o.v) {
            bool have = false;
            for (auto& x : v)
                if (x.kind == g.kind && x.uid == g.uid && x.code == g.code && x.at == g.at && x.text == g.text) { x.read |= g.read; x.done |= g.done; have = true; break; }
            if (!have) v.push_back(g);
        }
        std::stable_sort(v.begin(), v.end(), [](const Entry& a, const Entry& b) { return a.at > b.at; });
        if (v.size() > kMaxNotifs) v.resize(kMaxNotifs);
    }
};

inline std::string history_to_json(const NotifHistory& h) {
    json::Value root = json::make_object();
    json::Value arr = json::make_array();
    for (auto& e : h.v) {
        json::Value o = json::make_object();
        o.o.push_back({"kind", json::make_string(kind_name(e.kind))});
        o.o.push_back({"uid", json::make_string(e.uid)});
        o.o.push_back({"name", json::make_string(e.name)});
        o.o.push_back({"game_id", json::make_string(e.game_id)});
        o.o.push_back({"title", json::make_string(e.title)});
        o.o.push_back({"code", json::make_string(e.code)});
        o.o.push_back({"text", json::make_string(e.text)});
        o.o.push_back({"at", json::make_int(e.at)});
        o.o.push_back({"expires_at", json::make_int(e.expires_at)});
        o.o.push_back({"read", json::make_bool(e.read)});
        o.o.push_back({"done", json::make_bool(e.done)});
        arr.a.push_back(o);
    }
    root.o.push_back({"version", json::make_int(1)});
    root.o.push_back({"items", arr});
    return json::stringify(root);
}
inline bool history_from_json(const std::string& text, NotifHistory* out) {
    json::Value root;
    if (!json::parse(text, &root) || !root.is_object()) return false;
    out->v.clear();
    const json::Value* arr = root.get("items");
    if (!arr || !arr->is_array()) return true;
    for (auto& o : arr->a) {
        if (!o.is_object() || o.i64("at") <= 0) continue;
        Entry e;
        e.kind = kind_from(o.str("kind"));
        e.uid = o.str("uid"); e.name = o.str("name"); e.game_id = o.str("game_id"); e.title = o.str("title");
        e.code = o.str("code"); e.text = o.str("text");
        e.at = o.i64("at"); e.expires_at = o.i64("expires_at");
        e.read = o.boolean("read"); e.done = o.boolean("done");
        out->v.push_back(e);
    }
    std::stable_sort(out->v.begin(), out->v.end(), [](const Entry& a, const Entry& b) { return a.at > b.at; });
    if (out->v.size() > kMaxNotifs) out->v.resize(kMaxNotifs);
    return true;
}

/* ------------------------------------------------------------------ files */
inline std::string slurp(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
inline bool spit(const std::filesystem::path& p, const std::string& t) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << t;
    return (bool)f;
}
inline std::filesystem::path groups_path(const std::string& dir) { return std::filesystem::u8path(dir) / "recent_groups.json"; }
inline std::filesystem::path history_path(const std::string& dir) { return std::filesystem::u8path(dir) / "notif_history.json"; }

/* Load = union of the user-data copy and the vault copy (either may be missing). */
inline RecentGroups load_groups(const std::string& userdata, const std::string& vault) {
    RecentGroups a, b;
    if (!userdata.empty()) groups_from_json(slurp(groups_path(userdata)), &a);
    if (!vault.empty()) groups_from_json(slurp(groups_path(vault)), &b);
    a.merge(b);
    return a;
}
inline bool save_groups(const RecentGroups& r, const std::string& userdata, const std::string& vault) {
    std::string j = groups_to_json(r);
    bool ok = true;
    if (!userdata.empty()) ok = spit(groups_path(userdata), j) && ok;
    if (!vault.empty()) ok = spit(groups_path(vault), j) && ok;
    return ok;
}
inline NotifHistory load_history(const std::string& userdata, const std::string& vault) {
    NotifHistory a, b;
    if (!userdata.empty()) history_from_json(slurp(history_path(userdata)), &a);
    if (!vault.empty()) history_from_json(slurp(history_path(vault)), &b);
    a.merge(b);
    return a;
}
inline bool save_history(const NotifHistory& h, const std::string& userdata, const std::string& vault) {
    std::string j = history_to_json(h);
    bool ok = true;
    if (!userdata.empty()) ok = spit(history_path(userdata), j) && ok;
    if (!vault.empty()) ok = spit(history_path(vault), j) && ok;
    return ok;
}

/* ------------------------------------------------------------------ play again banner */
/* After an online session, back in the room: the banner's button reads PLAY AGAIN (host: relaunch the same game and
 * settings once everyone is ready; guest: ready up) instead of LAUNCH / READY. `again` = a session just ended in this room. */
inline std::string play_again_label(const std::string& label, bool is_launch, bool ready_on, bool button_enabled, bool again) {
    if (!again || ready_on) return label;
    if (is_launch && !button_enabled) return label;   // not everyone back yet: keep the explanatory state
    return "PLAY AGAIN";
}
inline bool play_again_applies(bool session_ended_here, bool in_game_now, bool game_changed) {
    return session_ended_here && !in_game_now && !game_changed;
}

}  // namespace group
}  // namespace hubui
