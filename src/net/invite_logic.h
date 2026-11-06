#pragma once
/* Account invite codes (docs/ADMIN.md "Invite codes"): pure logic shared by the friends client, the lock screen and the Admin page.
 * No network, no UI. The owner makes a code (XXXX-XXXX); a newcomer waiting for approval types it on the lock screen (or opens
 * retrohub://invite/XXXX-XXXX) and the server approves the account. The server decides everything; this shapes requests and words.
 * Not to be confused with the room invites in friends.h (Event::InviteResult, "invite_result" with a results[] array). */

#include "json.h"

#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace hubnet {

/* ---- code shape: 8 characters from an alphabet without 0 O 1 I L, shown as XXXX-XXXX ---- */
inline bool invite_char_ok(char c)
{
    return (c >= '2' && c <= '9') || (c >= 'A' && c <= 'Z' && c != 'I' && c != 'L' && c != 'O');
}
/* Upper-case, spaces / dashes / anything that is not a letter or digit dropped, at most 8 characters. Characters outside the alphabet are
 * kept (so the field shows what was typed); invite_valid() is what accepts a code. */
inline std::string invite_clean(const std::string& raw)
{
    std::string o;
    for (unsigned char c : raw) {
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) { o += (char)c; if (o.size() >= 8) break; }
    }
    return o;
}
/* "ABCD", "ABCD-E", "ABCD-EFGH": the dash appears as soon as there is a fifth character. */
inline std::string invite_format(const std::string& raw)
{
    const std::string c = invite_clean(raw);
    return c.size() <= 4 ? c : c.substr(0, 4) + "-" + c.substr(4);
}
inline bool invite_valid(const std::string& raw)
{
    const std::string c = invite_clean(raw);
    if (c.size() != 8) return false;
    for (char ch : c) if (!invite_char_ok(ch)) return false;
    return true;
}
/* Finds the first XXXX-XXXX (dash required) inside free text, e.g. a pasted invite message. */
inline bool invite_find_in_text(const std::string& text, std::string* code)
{
    for (size_t i = 0; i + 9 <= text.size(); ++i) {
        std::string w = text.substr(i, 9);
        if (w[4] != '-') continue;
        for (char& c : w) if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        bool ok = true;
        for (size_t k = 0; k < 9 && ok; ++k) if (k != 4) ok = invite_char_ok(w[k]);
        if (!ok) continue;
        if (i > 0 && (std::isalnum((unsigned char)text[i - 1]))) continue;   // inside a longer word
        if (i + 9 < text.size() && std::isalnum((unsigned char)text[i + 9])) continue;
        if (code) *code = w;
        return true;
    }
    return false;
}
/* Whatever the player typed or pasted -> the text the field should show: a pasted link or message is reduced to its code. */
inline std::string invite_field_text(const std::string& typed)
{
    std::string c;
    if (typed.size() > 9 && invite_find_in_text(typed, &c)) return c;
    return invite_format(typed);
}

/* ---- the link: retrohub://invite/XXXX-XXXX (scheme case-insensitive, one optional trailing '/', query / fragment ignored, quotes ok) ---- */
inline bool invite_link_code(const std::string& in, std::string* code)
{
    size_t b = 0, e = in.size();
    auto sp = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (b < e && (sp(in[b]) || in[b] == '"' || in[b] == '\'' || in[b] == '<')) ++b;
    while (e > b && (sp(in[e - 1]) || in[e - 1] == '"' || in[e - 1] == '\'' || in[e - 1] == '>')) --e;
    std::string s = in.substr(b, e - b);
    auto lower = [](std::string x) { for (char& c : x) if (c >= 'A' && c <= 'Z') c = (char)(c + 32); return x; };
    if (lower(s.substr(0, 9)) != "retrohub:") return false;
    std::string rest = s.substr(9);
    const size_t cut = rest.find_first_of("?#");
    if (cut != std::string::npos) rest.erase(cut);
    size_t k = 0;
    while (k < rest.size() && rest[k] == '/') ++k;
    rest = rest.substr(k);
    std::string dec;   // %XX (ASCII) decoded, so %2D works
    for (size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == '%' && i + 2 < rest.size() + 0 && std::isxdigit((unsigned char)rest[i + 1]) && std::isxdigit((unsigned char)rest[i + 2])) {
            const int v = std::stoi(rest.substr(i + 1, 2), nullptr, 16);
            if (v > 0 && v < 128) { dec += (char)v; i += 2; continue; }
        }
        dec += rest[i];
    }
    if (lower(dec.substr(0, 7)) != "invite/") return false;
    dec = dec.substr(7);
    while (!dec.empty() && dec.back() == '/') dec.pop_back();
    for (char c : dec) if (!(std::isalnum((unsigned char)c) || c == '-')) return false;
    if (!invite_valid(dec) || invite_clean(dec).size() != 8) return false;
    if (code) *code = invite_format(dec);
    return true;
}
inline std::string invite_link(const std::string& code) { return "retrohub://invite/" + invite_format(code); }
inline const char* invite_download_url() { return "https://github.com/fatty8me2/retrohub/releases/latest"; }
/* The text "Copy invite message" puts on the clipboard. */
inline std::string invite_share_text(const std::string& code)
{
    const std::string c = invite_format(code);
    return std::string("Join me on RetroHub! Install it from ") + invite_download_url() + " , then enter invite code " + c +
           " on the waiting screen (or open " + invite_link(c) + ")";
}

/* ---- a friend code typed by the owner: "RH-7KQ2-M9XD" in any case, with or without the RH- and dashes ---- */
inline bool friend_code_normalize(const std::string& raw, std::string* out)
{
    std::string c;
    for (unsigned char ch : raw) {
        if (ch >= 'a' && ch <= 'z') ch = (unsigned char)(ch - 32);
        if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) c += (char)ch;
    }
    if (c.size() == 10 && c.compare(0, 2, "RH") == 0) c = c.substr(2);
    if (c.size() != 8) return false;
    if (out) *out = "RH-" + c.substr(0, 4) + "-" + c.substr(4);
    return true;
}
/* The owner's field shows the 8 characters as 7KQ2-M9XD (the RH- is implied; typing it, or pasting a whole code, is fine). */
inline std::string friend_code_field_text(const std::string& typed)
{
    std::string c;
    for (unsigned char ch : typed) {
        if (ch >= 'a' && ch <= 'z') ch = (unsigned char)(ch - 32);
        if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) c += (char)ch;
    }
    const bool prefixed = typed.size() >= 3 && (typed[0] == 'R' || typed[0] == 'r') && (typed[1] == 'H' || typed[1] == 'h') && !std::isalnum((unsigned char)typed[2]);
    if ((prefixed || c.size() > 8) && c.compare(0, 2, "RH") == 0) c = c.substr(2);
    if (c.size() > 8) c.resize(8);
    return c.size() <= 4 ? c : c.substr(0, 4) + "-" + c.substr(4);
}

/* ---- requests ---- */
inline std::string invite_esc(const std::string& s) { return json::escape(s); }
inline std::string redeem_invite_json(const std::string& code) { return "{\"type\":\"redeem_invite\",\"code\":\"" + invite_format(code) + "\"}"; }
inline int invite_clamp_uses(int v) { return v < 1 ? 1 : v > 20 ? 20 : v; }
inline int invite_clamp_hours(int v) { return v < 1 ? 1 : v > 720 ? 720 : v; }
inline std::string invite_create_json(int uses, int hours, const std::string& note)
{
    return "{\"type\":\"admin_invite_create\",\"uses\":" + std::to_string(invite_clamp_uses(uses)) + ",\"hours\":" + std::to_string(invite_clamp_hours(hours)) +
           ",\"note\":\"" + invite_esc(note.substr(0, 60)) + "\"}";
}
inline std::string invite_list_json() { return "{\"type\":\"admin_invite_list\"}"; }
inline std::string invite_revoke_json(const std::string& code) { return "{\"type\":\"admin_invite_revoke\",\"code\":\"" + invite_format(code) + "\"}"; }
inline std::string approve_code_json(const std::string& friend_code) { return "{\"type\":\"admin_approve_code\",\"code\":\"" + invite_esc(friend_code) + "\"}"; }

/* ---- the newcomer's answer: {type:"invite_result", ok, reason?} ---- */
struct RedeemResult { bool ok = false; std::string reason; };
/* False when the message is a ROOM invite result (it has a results[] array) or not an object. */
inline bool parse_redeem_result(const json::Value& j, RedeemResult* r)
{
    if (j.type != json::Value::Object || j.get("results")) return false;
    const json::Value* ok = j.get("ok");
    if (!ok || ok->type != json::Value::Bool) return false;
    if (r) { r->ok = ok->b; r->reason = j.str("reason"); }
    return true;
}
inline std::string redeem_result_text(bool ok, const std::string& reason)
{
    if (ok) return "Code accepted. Unlocking RetroHub...";
    if (reason == "invalid") return "That code isn't right. Check it and try again.";
    if (reason == "expired") return "That invite has expired. Ask for a new one.";
    if (reason == "used-up") return "That invite has already been used up. Ask for a new one.";
    if (reason == "rate-limited") return "Too many tries. Wait a few minutes and try again.";
    if (reason == "busy") return "The server is busy. Try again in a moment.";
    return "That didn't work. Try again.";
}
inline const char* redeem_local_problem() { return "Invite codes look like ABCD-EFGH: 8 letters and numbers."; }

/* ---- the owner's list: {type:"admin_invites", invites:[{code,note,uses,used,exp,created,by_names}], created?:{...}} ---- */
struct AdminInvite {
    std::string code, note;
    int uses = 1, used = 0;
    int64_t exp = 0, created = 0;           // ms since the epoch (a value below 1e11 is taken as seconds)
    std::vector<std::string> by_names;      // who used it
    bool spent() const { return used >= uses; }
};
struct AdminInvites {
    bool valid = false;
    bool ok = true;                         // false: the create / revoke the reply answers failed (reason: invite-cap | not-found)
    std::string reason;
    std::vector<AdminInvite> list;
    bool has_created = false;               // this reply carried a freshly made code
    AdminInvite created;
};
inline int64_t invite_time_ms(int64_t t) { return t > 0 && t < 100000000000LL ? t * 1000 : t; }
inline AdminInvite parse_admin_invite(const json::Value& j)
{
    AdminInvite a;
    a.code = invite_format(j.str("code"));
    a.note = j.str("note");
    a.uses = (int)j.i64("uses", 1);
    a.used = (int)j.i64("used", 0);
    a.exp = invite_time_ms(j.i64("exp"));
    a.created = invite_time_ms(j.i64("created"));
    if (const json::Value* b = j.get("by_names"))
        for (const json::Value& x : b->a) if (x.type == json::Value::String && a.by_names.size() < 20) a.by_names.push_back(x.s);
    return a;
}
inline bool parse_admin_invites(const json::Value& j, AdminInvites* out)
{
    if (j.type != json::Value::Object || j.str("type") != "admin_invites") return false;
    AdminInvites r;
    r.valid = true;
    if (const json::Value* o = j.get("ok")) if (o->type == json::Value::Bool) r.ok = o->b;
    r.reason = j.str("reason");
    if (const json::Value* a = j.get("invites"))
        for (const json::Value& x : a->a) {
            if (x.type != json::Value::Object) continue;
            AdminInvite v = parse_admin_invite(x);
            if (!invite_valid(v.code) || r.list.size() >= 100) continue;
            r.list.push_back(std::move(v));
        }
    if (const json::Value* c = j.get("created"))
        if (c->type == json::Value::Object) {
            AdminInvite v = parse_admin_invite(*c);
            if (invite_valid(v.code)) { r.has_created = true; r.created = std::move(v); }
        }
    if (out) *out = std::move(r);
    return true;
}
/* Newest first, spent / expired last. */
inline void invite_sort(std::vector<AdminInvite>* v, int64_t now_ms)
{
    auto live = [&](const AdminInvite& a) { return !a.spent() && a.exp > now_ms; };
    for (size_t i = 1; i < v->size(); ++i)
        for (size_t k = i; k > 0; --k) {
            const AdminInvite &p = (*v)[k - 1], &q = (*v)[k];
            const bool swap = (live(q) && !live(p)) || (live(q) == live(p) && q.created > p.created);
            if (!swap) break;
            std::swap((*v)[k - 1], (*v)[k]);
        }
}
/* "6 days left", "5 hours left", "40 min left", "expired" */
inline std::string invite_left_text(int64_t exp_ms, int64_t now_ms)
{
    if (exp_ms <= now_ms) return "expired";
    const int64_t s = (exp_ms - now_ms) / 1000;
    if (s < 3600) return std::to_string(s / 60 < 1 ? 1 : s / 60) + " min left";
    if (s < 48 * 3600) return std::to_string(s / 3600) + (s / 3600 == 1 ? " hour left" : " hours left");
    return std::to_string(s / 86400) + " days left";
}
inline std::string invite_used_text(const AdminInvite& a)
{
    std::string t = std::to_string(a.used) + "/" + std::to_string(a.uses) + " used";
    if (!a.by_names.empty()) {
        t += ": ";
        for (size_t i = 0; i < a.by_names.size(); ++i) { if (i) t += ", "; t += a.by_names[i]; }
    }
    return t;
}
inline std::string invite_options_text(int uses, int hours)
{
    return std::to_string(uses) + (uses == 1 ? " person" : " people") + ", " + (hours % 24 == 0 ? std::to_string(hours / 24) + (hours == 24 ? " day" : " days") : std::to_string(hours) + (hours == 1 ? " hour" : " hours"));
}

/* ---- pushes to the owner ---- */
/* admin_pending: {pending:N, latest:{uid,name,code,created}|null} */
struct PendingLatest { bool valid = false; std::string uid, name, code; int64_t created = 0; };
inline PendingLatest parse_pending_latest(const json::Value& j)
{
    PendingLatest p;
    const json::Value* l = j.get("latest");
    if (!l || l->type != json::Value::Object) return p;
    p.uid = l->str("uid"); p.name = l->str("name"); p.code = l->str("code"); p.created = invite_time_ms(l->i64("created"));
    p.valid = !p.uid.empty();
    return p;
}
/* A waiting account worth a toast: it started waiting in the last 15 minutes (older ones only reach the bell). */
inline bool waiting_is_fresh(int64_t created_ms, int64_t now_ms) { return created_ms <= 0 || (now_ms - created_ms < 15 * 60 * 1000 && now_ms - created_ms > -60000); }
/* admin_invite_used: {code, name, uid} */
struct InviteUsed { bool valid = false; std::string code, name, uid; };
inline InviteUsed parse_invite_used(const json::Value& j)
{
    InviteUsed u;
    u.code = invite_format(j.str("code")); u.name = j.str("name"); u.uid = j.str("uid");
    u.valid = !u.code.empty();
    return u;
}
inline std::string waiting_bell_text(const std::string& name, const std::string& code)
{
    return (name.empty() ? std::string("Someone") : name) + (code.empty() ? std::string() : " (" + code + ")") + " wants to join";
}
inline std::string joined_bell_text(const std::string& name, const std::string& code)
{
    return (name.empty() ? std::string("Someone") : name) + " joined with invite " + invite_format(code);
}

} // namespace hubnet
