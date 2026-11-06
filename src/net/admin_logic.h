#pragma once
/* Owner dashboard and account access: pure logic shared by the friends client and the Admin page (docs/ADMIN.md).
 * No network, no UI. The server decides everything; this only shapes requests and words. */

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace hubnet {

/* ---- account access (a refused hello) ---- */
enum class Access { Ok, Pending, Unavailable };

/* The server's refusal reasons: "pending-approval" (waiting for the owner) and "account-unavailable" (blocked; deliberately vague). */
inline Access access_from_reason(const std::string& r)
{
    if (r == "pending-approval") return Access::Pending;
    if (r == "account-unavailable") return Access::Unavailable;
    return Access::Ok;
}
inline std::string access_title(Access a)
{
    return a == Access::Pending ? "Waiting for approval" : a == Access::Unavailable ? "Online is unavailable for this account" : "";
}
inline std::string access_message(Access a)
{
    if (a == Access::Pending)
        return "Waiting for the owner to approve this account. RetroHub unlocks as soon as it is approved.";
    if (a == Access::Unavailable)
        return "This account can't use RetroHub.";
    return "";
}

/* ---- owner dashboard data ---- */
struct AdminAccount {
    std::string uid, code, name, username, status, by;   // status: ok | pending | banned | denied
    int64_t created = 0, first_seen = 0, last_seen = 0;   // ms since the epoch (0 = unknown)
    int pcs = 0;
    bool friend_of_you = false;
    bool test = false;          // a test-flagged account (hidden unless the "test accounts" filter is on)
    bool unapplied = false;     // the verdict has not reached the account yet (a failed push): the server keeps retrying
    bool unreachable = false;   // the server gave up pushing (about 10 tries / 24 h); it is still enforced at the account's next hello
    std::string req;                                      // "in" | "out" | "" : a request between you and this account
    bool blocked() const { return status == "banned" || status == "denied"; }
    bool pending() const { return status == "pending"; }
    std::string shown_name() const { return username.empty() ? name : username; }
};

struct AdminSettings {
    bool approval = true;     // new accounts wait for the owner (ON by default on the server)
    bool vouch = false;       // a friend adding a waiting account by its code approves it (OFF by default)
    bool autofriend = true;   // every new account is added to the owner's friends (ON by default = the old behaviour)
    bool strict_rooms = false; // a room seat must prove an approved account (OFF by default)
    int64_t cutoff = 0;       // when approval was switched on: older accounts are untouched
};
struct AdminCounts { int all = 0, ok = 0, pending = 0, banned = 0; };

struct AdminList {
    bool valid = false;
    std::vector<AdminAccount> rows;
    int total = 0, offset = 0, tracked = 0;
    AdminCounts counts;
    AdminSettings settings;
};

struct AdminDetail {
    bool valid = false, found = false, has_peek = false;
    std::string uid, code, rel, how;            // rel: friend | requested_you | you_requested | none; how: auto | request | room | unknown | ""
    int64_t how_at = 0, friend_since = 0;
    AdminAccount row;
    std::vector<std::string> friends;           // names of their friends (at most 30)
    int nfriends = 0;
};

struct AdminQuery {
    std::string q;
    bool by_first = false;     // sort by first seen instead of last seen
    bool asc = false;
    std::string status = "all";   // all | pending | banned | ok
    int offset = 0, limit = 25;
    bool show_test = false;    // include test-flagged accounts
};

/* ---- requests (JSON text for the socket) ---- */
inline std::string admin_esc(const std::string& s)
{
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}
inline std::string admin_list_json(const AdminQuery& q)
{
    return "{\"type\":\"admin_list\",\"q\":\"" + admin_esc(q.q.substr(0, 40)) + "\",\"sort\":\"" + (q.by_first ? "first" : "last") + "\",\"order\":\"" +
           (q.asc ? "asc" : "desc") + "\",\"status\":\"" + admin_esc(q.status) + "\",\"offset\":" + std::to_string(q.offset < 0 ? 0 : q.offset) +
           ",\"limit\":" + std::to_string(q.limit < 1 ? 25 : q.limit > 50 ? 50 : q.limit) + ",\"tests\":" + (q.show_test ? "true" : "false") + "}";
}
/* op: ban | unban | approve | deny | detail */
inline std::string admin_uid_json(const std::string& op, const std::string& uid)
{
    return "{\"type\":\"admin_" + admin_esc(op) + "\",\"uid\":\"" + admin_esc(uid) + "\"}";
}
/* -1 = leave as it is. Only the fields that change are sent. */
inline std::string admin_settings_json(int approval = -1, int vouch = -1, int autofriend = -1, int strict_rooms = -1)
{
    std::string s;
    auto add = [&](const char* k, int v) { if (v < 0) return; if (!s.empty()) s += ","; s += std::string("\"") + k + "\":" + (v ? "true" : "false"); };
    add("approval", approval); add("vouch", vouch); add("autofriend", autofriend); add("strict_rooms", strict_rooms);
    return s.empty() ? std::string("{\"type\":\"admin_settings\"}") : "{\"type\":\"admin_settings\",\"set\":{" + s + "}}";
}

/* ---- words ---- */
inline std::string admin_ago(int64_t now_ms, int64_t then_ms)
{
    if (then_ms <= 0) return "never";
    int64_t s = (now_ms - then_ms) / 1000;
    if (s < 0) s = 0;
    if (s < 60) return "just now";
    if (s < 3600) return std::to_string(s / 60) + " min ago";
    if (s < 86400) return std::to_string(s / 3600) + (s / 3600 == 1 ? " hour ago" : " hours ago");
    int64_t d = s / 86400;
    if (d < 60) return std::to_string(d) + (d == 1 ? " day ago" : " days ago");
    return std::to_string(d / 30) + " months ago";
}
inline std::string admin_how_text(const std::string& how, const std::string& rel)
{
    if (how == "auto") return "Added automatically when they installed RetroHub";
    if (how == "request") return "Friend request (one of you asked, the other accepted)";
    if (how == "room") return "Met in an online room and became friends";
    if (how == "unknown" || how == "migrated") return "Older friend (before this was tracked)";
    if (rel == "requested_you") return "Has sent you a friend request";
    if (rel == "you_requested") return "You sent them a friend request";
    return "Not on your friend list";
}
inline std::string admin_status_label(const AdminAccount& a)
{
    if (a.status == "pending") return "Waiting";
    if (a.status == "banned") return "Banned";
    if (a.status == "denied") return "Denied";
    return "Active";
}
inline std::string admin_ban_confirm(const std::string& name)
{
    return "Ban " + name + "? They are removed from your friends and everyone's friend requests, and can no longer use online play, friends, "
           "rooms, invites, cloud saves or account links. RetroHub locks for them on their next connection, offline play included. You can unban them later.";
}
inline std::string admin_deny_confirm(const std::string& name)
{
    return "Deny " + name + "? They are refused online and can ask again only if you approve or unban them later.";
}
inline std::string admin_approval_help(bool on)
{
    return std::string("New accounts wait in a Pending list until you approve them. Your current friends are not affected: every account that already existed keeps working. ") +
           (on ? "Waiting or banned accounts can't use RetroHub at all, not even offline." : "Off: everyone can join, as before.");
}
inline std::string admin_vouch_help()
{
    return "When on, a new waiting account is approved automatically if one of your approved friends adds it by friend code (up to 5 each). Needs approval mode.";
}
inline std::string admin_autofriend_help()
{
    return "On: every new account is added to your friends, as it always was. Off: new accounts are not added to your list. Your current friends are not affected.";
}
inline std::string admin_strict_rooms_help()
{
    return "On: a room seat that does not prove an approved RetroHub account within 15 seconds is closed. Friends on versions older than 1.31.11 can't join rooms while this is on, "
           "so switch it on after they have updated. Off by default.";
}
inline std::string admin_result_text(const std::string& op, bool ok, const std::string& name, const std::string& reason, bool applied_known, bool applied)
{
    if (!ok) {
        if (reason == "ban-cap") return "The ban list is full (unban someone first).";
        if (reason == "cap") return "The account list is full.";
        if (reason == "invite-cap") return "You have too many active invites. Revoke one first.";
        if (reason == "not-found") return op == "invite" ? "That invite is already gone." : "No waiting account has that friend code.";
        if (reason == "not-pending") return "That account isn't waiting for approval.";
        return "That did not work. Try again.";
    }
    std::string who = name.empty() ? std::string("that account") : name;
    std::string t = op == "ban" ? "Banned " + who + "." : op == "unban" ? "Unbanned " + who + "." : op == "approve" ? "Approved " + who + "." : "Denied " + who + ".";
    if (applied_known && !applied) t += " Not applied yet - retrying.";
    return t;
}

} // namespace hubnet
