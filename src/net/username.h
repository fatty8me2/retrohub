#pragma once
/* RetroHub usernames: pure validation / normalization / suggestions (header-only, no I/O). See docs/FRIENDS.md "Usernames".
 *
 * A username is the account. It is unique case-insensitively (the "key" is the lower-case form) but displayed as typed.
 * Rules (the server enforces the same ones plus its own reserved list): 3-16 characters, letters, digits and underscore,
 * starts with a letter or digit, does not end in an underscore, no double underscore. */

#include <cstdint>
#include <string>
#include <vector>

namespace hubnet {

constexpr int kUsernameMin = 3;
constexpr int kUsernameMax = 16;
constexpr int kUsernameHoldDays = 30;   // after a change the old name stays reserved for you; nobody else can take it meanwhile

enum class NameCheck { Ok, Empty, TooShort, TooLong, BadChars, BadEdge, Reserved };

/* Trims surrounding spaces / tabs (what the user typed -> what is validated and sent). */
inline std::string username_trim(const std::string& in)
{
    size_t a = 0, b = in.size();
    while (a < b && (in[a] == ' ' || in[a] == '\t')) a++;
    while (b > a && (in[b - 1] == ' ' || in[b - 1] == '\t')) b--;
    return in.substr(a, b - a);
}

/* Comparison key: ASCII lower case of the trimmed name. "Kai_64" and "kai_64" are the same username. */
inline std::string username_key(const std::string& in)
{
    std::string s = username_trim(in);
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

inline bool username_reserved(const std::string& key)
{
    static const char* kWords[] = {"admin", "administrator", "moderator", "mod", "system", "retrohub", "support", "staff", "owner",
                                   "root", "server", "null", "undefined", "player", "guest", "official", "everyone", "anonymous"};
    for (const char* w : kWords) if (key == w) return true;
    return false;
}

inline NameCheck username_validate(const std::string& in)
{
    const std::string s = username_trim(in);
    if (s.empty()) return NameCheck::Empty;
    for (unsigned char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return NameCheck::BadChars;
    }
    if (s.size() < (size_t)kUsernameMin) return NameCheck::TooShort;
    if (s.size() > (size_t)kUsernameMax) return NameCheck::TooLong;
    if (s.front() == '_' || s.back() == '_' || s.find("__") != std::string::npos) return NameCheck::BadEdge;
    if (username_reserved(username_key(s))) return NameCheck::Reserved;
    return NameCheck::Ok;
}

inline bool username_ok(const std::string& s) { return username_validate(s) == NameCheck::Ok; }

/* One line for the UI under the text box. "" for Ok and Empty. */
inline std::string username_problem_text(NameCheck c)
{
    switch (c) {
        case NameCheck::TooShort: return "Too short: use at least 3 characters.";
        case NameCheck::TooLong: return "Too long: 16 characters at most.";
        case NameCheck::BadChars: return "Use letters, numbers and _ only (no spaces).";
        case NameCheck::BadEdge: return "Start with a letter or number, and don't end with _ or use __.";
        case NameCheck::Reserved: return "That name is not allowed.";
        default: return "";
    }
}

/* Turns anything (a Windows account name, a rejected attempt) into something that passes username_validate: drops disallowed
 * characters, fixes the edges, pads a too-short result with "Player", cuts to `max_len`. */
inline std::string username_sanitize(const std::string& in, size_t max_len = (size_t)kUsernameMax)
{
    std::string o;
    for (unsigned char c : in) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) o += (char)c;
        else if (c == '_' || c == ' ' || c == '-' || c == '.') { if (!o.empty() && o.back() != '_') o += '_'; }
    }
    while (!o.empty() && o.back() == '_') o.pop_back();
    if (o.size() > max_len) o.resize(max_len);
    while (!o.empty() && o.back() == '_') o.pop_back();
    if (o.size() < (size_t)kUsernameMin) o = o.empty() ? "Player" : o + "Player";
    if (o.size() > max_len) o.resize(max_len);
    return o;
}

/* "name2", "name_64"-style alternatives for a taken / unusable name. Deterministic for a given seed. Never returns the
 * name itself; every entry passes username_validate. */
inline std::vector<std::string> username_suggestions(const std::string& wanted, uint32_t seed = 0, size_t count = 3)
{
    const std::string base = username_sanitize(wanted, 12);
    std::vector<std::string> out;
    auto add = [&](std::string s) {
        if (s.size() > (size_t)kUsernameMax) s.resize(kUsernameMax);
        if (!username_ok(s) || username_key(s) == username_key(wanted)) return;
        for (const auto& e : out) if (username_key(e) == username_key(s)) return;
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

/* "RH-7KQ2-M9XD" / "7kq2m9xd" shaped input is a friend code, not a username (the add-friend box takes both). */
inline bool username_looks_like_code(const std::string& in)
{
    if (in.find('-') == std::string::npos) return false;   // bare letters are a username; a code is written with its dash
    std::string s;
    for (unsigned char c : in) if (c != '-' && c != ' ') s += (char)((c >= 'a' && c <= 'z') ? c - 32 : c);
    if (s.compare(0, 2, "RH") == 0) s = s.substr(2);
    if (s.size() != 8) return false;
    for (char c : s) if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
    return true;
}

/* First letter, upper case, for avatars (names may start with a digit). */
inline char username_initial(const std::string& n)
{
    if (n.empty()) return '?';
    char c = n[0];
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

} // namespace hubnet
