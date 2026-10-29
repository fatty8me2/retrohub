#pragma once
/* RetroHub usernames: pure validation / normalization / suggestions (header-only, no I/O). See docs/FRIENDS.md "Usernames"
 * and docs/ACCOUNTS.md. The server (BattleShip/server/cloudflare/src/usernames.js) enforces the same rules plus its own
 * reserved / profanity lists; keep the two in step.
 *
 * A username is the account. It is displayed as typed but unique by its "fold key" (username_fold_key).
 * Rules: 3-20 characters (Unicode code points, names are UTF-8 here). Allowed: printable ASCII (letters, digits, space and
 * symbols) except  " \ < > ` /  (they break JSON, command lines, markup and paths), plus the Latin-1 letters U+00C0-U+00FF
 * except x (U+00D7) and / (U+00F7). Nothing else (the hub font only has Basic Latin + Latin-1). Leading/trailing spaces are
 * trimmed and runs of spaces collapse to one; tabs and other whitespace are not allowed. At least 3 letters/digits must
 * survive the fold. Never shaped like a friend code (RH-XXXX-XXXX). */

#include <cstdint>
#include <string>
#include <vector>

namespace hubnet {

constexpr int kUsernameMin = 3;
constexpr int kUsernameMax = 20;
constexpr int kUsernameHoldDays = 7;    // after a change the old name stays reserved for you; nobody else can take it meanwhile

enum class NameCheck { Ok, Empty, TooShort, TooLong, BadChars, BadEdge /* fewer than 3 letters/digits */, Reserved };

/* ---- UTF-8 helpers ---- */

/* Strict decode (no overlongs, no surrogates). False on invalid UTF-8; `out` is then partial. */
inline bool username_decode(const std::string& s, std::vector<uint32_t>& out)
{
    out.clear();
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        uint32_t cp; size_t n;
        if (c < 0x80) { cp = c; n = 1; }
        else if (c >= 0xC2 && c <= 0xDF) { cp = c & 0x1F; n = 2; }
        else if (c >= 0xE0 && c <= 0xEF) { cp = c & 0x0F; n = 3; }
        else if (c >= 0xF0 && c <= 0xF4) { cp = c & 0x07; n = 4; }
        else return false;
        if (i + n > s.size()) return false;
        for (size_t k = 1; k < n; k++) {
            const unsigned char d = (unsigned char)s[i + k];
            if ((d & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (d & 0x3F);
        }
        if ((n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        out.push_back(cp);
        i += n;
    }
    return true;
}
inline void username_append(std::string& s, uint32_t cp)
{
    if (cp < 0x80) s += (char)cp;
    else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
    else { s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
}
/* Code points in the allowed set (see the header comment). */
inline bool username_cp_allowed(uint32_t cp)
{
    if (cp >= 0x20 && cp <= 0x7E) return cp != '"' && cp != '\\' && cp != '<' && cp != '>' && cp != '`' && cp != '/';
    return cp >= 0xC0 && cp <= 0xFF && cp != 0xD7 && cp != 0xF7;
}
/* Number of characters (code points). Invalid UTF-8 counts bytes, which is only ever used for a name that gets rejected anyway. */
inline size_t username_length(const std::string& s)
{
    std::vector<uint32_t> v;
    return username_decode(s, v) ? v.size() : s.size();
}
/* First `n` characters, never splitting a UTF-8 sequence. */
inline std::string username_cut(const std::string& s, size_t n)
{
    std::vector<uint32_t> v;
    if (!username_decode(s, v)) return s.substr(0, n);
    if (v.size() <= n) return s;
    std::string o;
    for (size_t i = 0; i < n; i++) username_append(o, v[i]);
    return o;
}

/* Trims surrounding spaces / tabs and collapses runs of spaces to one (what the user typed -> what is validated and sent). */
inline std::string username_trim(const std::string& in)
{
    size_t a = 0, b = in.size();
    while (a < b && (in[a] == ' ' || in[a] == '\t')) a++;
    while (b > a && (in[b - 1] == ' ' || in[b - 1] == '\t')) b--;
    std::string o;
    for (size_t i = a; i < b; i++) if (!(in[i] == ' ' && !o.empty() && o.back() == ' ')) o += in[i];
    return o;
}

/* Base letters for U+00C0-U+00FF lower-cased ("" for the two non-letters). Index = cp - 0xC0. */
inline const char* username_latin1_base(uint32_t cp)
{
    static const char* kT[64] = {
        "a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i",
        "d","n","o","o","o","o","o","",  "o","u","u","u","u","y","th","ss",
        "a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i",
        "d","n","o","o","o","o","o","",  "o","u","u","u","u","y","th","y"};
    return cp >= 0xC0 && cp <= 0xFF ? kT[cp - 0xC0] : "";
}

/* ASCII-only rendering of any text, for places that cannot show Latin-1 (the OSD font, the 3DS username, emulator nicknames):
 * Latin-1 letters become their base letters (case kept: E -> E, ae -> ae, Thorn -> Th), other characters outside printable ASCII are
 * dropped. Works on arbitrary bytes (invalid UTF-8 keeps only its ASCII bytes). */
inline std::string username_ascii(const std::string& in)
{
    std::vector<uint32_t> v;
    std::string o;
    if (!username_decode(in, v)) {
        for (unsigned char c : in) if (c >= 0x20 && c <= 0x7E) o += (char)c;
        return o;
    }
    for (uint32_t cp : v) {
        if (cp >= 0x20 && cp <= 0x7E) o += (char)cp;
        else if (cp >= 0xC0 && cp <= 0xFF) {
            const bool upper = cp <= 0xDE && cp != 0xD7;
            std::string b = username_latin1_base(upper ? cp + 32 : cp);
            if (upper && !b.empty()) b[0] = (char)(b[0] - 32);
            o += b;
        }
    }
    return o;
}

/* Display-level comparison key: the trimmed name in lower case (ASCII and Latin-1). "Kai_64" and "kai_64" are the same
 * name text; this is NOT the uniqueness key (see username_fold_key). */
inline std::string username_key(const std::string& in)
{
    const std::string s = username_trim(in);
    std::vector<uint32_t> v;
    if (!username_decode(s, v)) {
        std::string o = s;
        for (char& c : o) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        return o;
    }
    std::string o;
    for (uint32_t cp : v) {
        if (cp >= 'A' && cp <= 'Z') cp += 32;
        else if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) cp += 32;
        username_append(o, cp);
    }
    return o;
}

/* The server's uniqueness key: lower case, Latin-1 accents -> base letters, every character that is not [a-z0-9] dropped,
 * then 0->o 1->l 5->s. For every name valid under the old rules (A-Za-z0-9 _ . -) this equals the old key. */
inline std::string username_fold_key(const std::string& in)
{
    std::vector<uint32_t> v;
    std::string o;
    if (!username_decode(in, v)) return o;
    for (uint32_t cp : v) {
        if (cp >= 'A' && cp <= 'Z') cp += 32;
        if (cp >= 0xC0) { o += username_latin1_base(cp >= 0xC0 && cp <= 0xDE ? cp + 32 : cp); continue; }
        if (cp >= 'a' && cp <= 'z') o += (char)cp;
        else if (cp == '0') o += 'o';
        else if (cp == '1') o += 'l';
        else if (cp == '5') o += 's';
        else if (cp >= '0' && cp <= '9') o += (char)cp;
    }
    return o;
}

/* Fold key with l -> i as well: the form reserved words are matched in (so "Adm1n" is "admin"). */
inline std::string username_strong_key(const std::string& in)
{
    std::string k = username_fold_key(in);
    for (char& c : k) if (c == 'l') c = 'i';
    return k;
}

/* Mirrors RESERVED_WORDS / BAD_WORDS in the server's usernames.js (keep identical; test_username.cpp runs the shared parity corpus). */
inline bool username_reserved(const std::string& name)
{
    static const char* kWords[] = {"admin", "administrator", "retrohub", "retro", "hub", "mod", "moderator", "system", "staff", "support", "official",
                                   "smashclassic", "smash", "root", "owner", "server", "everyone", "here", "help", "anonymous", "guest", "player",
                                   "null", "undefined", "nintendo", "sony", "microsoft", "cloudflare", "anthropic", "claude", "bot", "dev", "developer",
                                   "operator", "sysop", "team", "announcement", "news", "me", "you", "unknown", "deleted", "removed"};
    const std::string k = username_strong_key(name);
    for (const char* w : kWords) if (k == username_strong_key(w)) return true;
    return false;
}
/* Substring match on the strong fold ("Sh1tlord", "FUCK"). */
inline bool username_profane(const std::string& name)
{
    static const char* kBad[] = {"fuck", "shit", "cunt", "nigger", "nigga", "faggot", "bitch", "whore", "rapist", "nazi", "hitler", "kike", "retard"};
    const std::string k = username_strong_key(name);
    for (const char* w : kBad) if (k.find(username_strong_key(w)) != std::string::npos) return true;
    return false;
}
/* "qa-", "qa_", "qa." or "qa " prefix: reserved for the QA/test accounts (the server refuses it for real accounts). */
inline bool username_qa_prefix(const std::string& s)
{
    return s.size() >= 3 && (s[0] == 'q' || s[0] == 'Q') && (s[1] == 'a' || s[1] == 'A') && (s[2] == '-' || s[2] == '_' || s[2] == '.' || s[2] == ' ');
}

/* RH-XXXX-XXXX / RH XXXX XXXX / RHXXXXXXXX: would be mistaken for a friend code, so it can't be a username. Same shape as the
 * server's CODE_LIKE: RH, optional - or space, 4 of [A-Z2-7], optional - or space, 4 of [A-Z2-7], case-insensitive. */
inline bool username_looks_like_code_shape(const std::string& s)
{
    size_t i = 0;
    auto up = [&](size_t k) { char c = s[k]; return (char)((c >= 'a' && c <= 'z') ? c - 32 : c); };
    auto sep = [&] { if (i < s.size() && (s[i] == '-' || s[i] == ' ')) i++; };
    auto grp = [&] {
        if (i + 4 > s.size()) return false;
        for (size_t k = 0; k < 4; k++) { char c = up(i + k); if (!((c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7'))) return false; }
        i += 4;
        return true;
    };
    if (s.size() < 10 || up(0) != 'R' || up(1) != 'H') return false;
    i = 2;
    sep();
    if (!grp()) return false;
    sep();
    return grp() && i == s.size();
}

inline NameCheck username_validate(const std::string& in)
{
    const std::string s = username_trim(in);
    if (s.empty()) return NameCheck::Empty;
    std::vector<uint32_t> v;
    if (!username_decode(s, v)) return NameCheck::BadChars;
    for (uint32_t cp : v) if (!username_cp_allowed(cp)) return NameCheck::BadChars;
    if (v.size() < (size_t)kUsernameMin) return NameCheck::TooShort;
    if (v.size() > (size_t)kUsernameMax) return NameCheck::TooLong;
    /* Same order as the server's normalizeUsername: qa prefix, code shape, key length, reserved, profanity. */
    if (username_qa_prefix(s)) return NameCheck::Reserved;
    if (username_looks_like_code_shape(s)) return NameCheck::Reserved;
    if (username_fold_key(s).size() < (size_t)kUsernameMin) return NameCheck::BadEdge;
    if (username_reserved(s) || username_profane(s)) return NameCheck::Reserved;
    return NameCheck::Ok;
}

inline bool username_ok(const std::string& s) { return username_validate(s) == NameCheck::Ok; }

/* One line for the UI under the text box. "" for Ok and Empty. */
inline std::string username_problem_text(NameCheck c)
{
    switch (c) {
        case NameCheck::TooShort: return "Too short: use at least 3 characters.";
        case NameCheck::TooLong: return "Too long: 20 characters at most.";
        case NameCheck::BadChars: return "Use letters, numbers, spaces and common symbols (no emoji, and not \" \\ / < > `).";
        case NameCheck::BadEdge: return "Include at least 3 letters or numbers.";
        case NameCheck::Reserved: return "That name is not allowed.";
        default: return "";
    }
}

/* Turns anything (a Windows account name, a rejected attempt) into something that passes username_validate: drops disallowed
 * characters, collapses/trims spaces, pads a name with fewer than 3 letters/digits with "Player", cuts to `max_len` characters. */
inline std::string username_sanitize(const std::string& in, size_t max_len = (size_t)kUsernameMax)
{
    std::vector<uint32_t> v;
    std::string o;
    if (username_decode(in, v)) {
        for (uint32_t cp : v) {
            if (cp == '\t') cp = ' ';
            if (!username_cp_allowed(cp)) continue;
            if (cp == ' ' && (o.empty() || o.back() == ' ')) continue;
            username_append(o, cp);
        }
    } else {   // not UTF-8 (ANSI leftovers): keep the ASCII subset
        for (unsigned char c : in) {
            if (c >= 0x80 || !username_cp_allowed(c)) continue;
            if (c == ' ' && (o.empty() || o.back() == ' ')) continue;
            o += (char)c;
        }
    }
    auto trim_tail = [&] { while (!o.empty() && o.back() == ' ') o.pop_back(); };
    trim_tail();
    o = username_cut(o, max_len);
    trim_tail();
    if (username_fold_key(o).size() < (size_t)kUsernameMin) {
        o = username_fold_key(o).empty() ? "Player" : o + " Player";
        o = username_cut(o, max_len);
        trim_tail();
    }
    return o;
}

/* "name2", "name_64"-style alternatives for a taken / unusable name. Deterministic for a given seed. Never returns the
 * name itself; every entry passes username_validate. */
inline std::vector<std::string> username_suggestions(const std::string& wanted, uint32_t seed = 0, size_t count = 3)
{
    const std::string base = username_sanitize(wanted, 14);
    std::vector<std::string> out;
    auto add = [&](std::string s) {
        s = username_cut(s, kUsernameMax);
        if (!username_ok(s) || username_fold_key(s) == username_fold_key(wanted)) return;
        for (const auto& e : out) if (username_fold_key(e) == username_fold_key(s)) return;
        if (out.size() < count) out.push_back(s);
    };
    uint32_t x = seed * 2654435761u + 12345u;
    auto num = [&](int digits) {
        x = x * 1664525u + 1013904223u;
        uint32_t m = 1;
        for (int i = 0; i < digits; i++) m *= 10;
        uint32_t v = (x >> 8) % m;
        std::string d = std::to_string(v);
        while ((int)d.size() < digits) d = "0" + d;
        return d;
    };
    add(base + "2");
    add(base + "_" + num(2));
    add(base + num(3));
    add(base + "_" + num(2));
    add(base + num(2));
    return out;
}

/* "RH-7KQ2-M9XD" / "7kq2m9xd" shaped input is a friend code, not a username (the add-friend box takes both). Only letters,
 * digits, dashes and spaces can be part of one (a name with other symbols is a name). */
inline bool username_looks_like_code(const std::string& in)
{
    if (in.find('-') == std::string::npos) return false;   // bare letters are a username; a code is written with its dash
    std::string s;
    for (unsigned char c : in) {
        if (c == '-' || c == ' ') continue;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;   // other symbols mean a name, not a code
        s += (char)((c >= 'a' && c <= 'z') ? c - 32 : c);
    }
    if (s.compare(0, 2, "RH") == 0) s = s.substr(2);
    return s.size() == 8;
}

/* First letter or digit, upper case and without accent, for avatars (names may start with a symbol, a digit or an accented letter). */
inline char username_initial(const std::string& n)
{
    std::vector<uint32_t> v;
    if (!username_decode(n, v)) return '?';
    for (uint32_t cp : v) {
        if (cp >= 'a' && cp <= 'z') return (char)(cp - 32);
        if (cp >= 'A' && cp <= 'Z') return (char)cp;
        if (cp >= '0' && cp <= '9') return (char)cp;
        if (cp >= 0xC0) { std::string b = username_latin1_base(cp >= 0xC0 && cp <= 0xDE ? cp + 32 : cp); if (!b.empty()) return (char)(b[0] - 32); }
    }
    return '?';
}

} // namespace hubnet
