#pragma once
/* Social profile + reactions: pure logic shared by the hub UI and tests (header-only).
 * Avatar ids: "c:<console>" (a pixel console icon) or "g:<game key>" (that game's cover as a frame).
 * Reaction codes are a fixed list, so a reaction is a few bytes on the wire. */
#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace hub { namespace profile {

inline const char* const kConsoles[] = {"nes", "snes", "n64", "gc", "wiiu", "gb", "gbc", "gba", "ds", "3ds", "ps1", "ps2"};
inline const char* const kReactions[] = {"heart", "up", "lol", "fire", "gg", "sad"};
inline const char* const kReactionLabels[] = {"<3", "+1", "LOL", "FIRE", "GG", ":("};
constexpr size_t kStatusMax = 32;

inline bool is_console(const std::string& c) { for (auto k : kConsoles) if (c == k) return true; return false; }
inline bool valid_reaction(const std::string& r) { for (auto k : kReactions) if (r == k) return true; return false; }
inline const char* reaction_label(const std::string& r) {
    static const char* const lab[] = {"<3", "+1", "LOL", "FIRE", "GG", ":("};
    for (size_t i = 0; i < 6; i++) if (r == kReactions[i]) return lab[i];
    return "?";
}

/* Valid avatar id: "c:<known console>" or "g:<up to 36 safe chars>". */
inline bool valid_avatar(const std::string& a) {
    if (a.size() < 3 || a[1] != ':') return false;
    std::string rest = a.substr(2);
    if (a[0] == 'c') return is_console(rest);
    if (a[0] != 'g' || rest.size() > 36) return false;
    for (unsigned char ch : rest) if (!(std::isalnum(ch) || ch == '_' || ch == '.' || ch == ':' || ch == '-')) return false;
    return true;
}

/* Status line: collapse whitespace, drop control bytes, cap at kStatusMax characters (UTF-8 aware). */
inline std::string clean_status(const std::string& s) {
    std::string o; bool sp = true; size_t chars = 0;
    for (size_t i = 0; i < s.size() && chars < kStatusMax;) {
        unsigned char c = (unsigned char)s[i];
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (i + n > s.size()) break;
        if (c < 0x20 || c == 0x7f || std::isspace(c)) { if (!sp) { o += ' '; sp = true; chars++; } i++; continue; }
        o.append(s, i, n); i += n; chars++; sp = false;
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}

struct Profile { std::string avatar, fav_game, status; bool empty() const { return avatar.empty() && fav_game.empty() && status.empty(); } };

/* Reaction tally for one chat/DM line: reactor id -> reaction code (one reaction per person; same code again clears it). */
struct Tally {
    std::map<std::string, std::string> by;
    bool set(const std::string& who, const std::string& code, bool on) {
        if (!valid_reaction(code) || who.empty()) return false;
        auto it = by.find(who);
        if (!on) { if (it != by.end() && it->second == code) { by.erase(it); return true; } return false; }
        if (it != by.end() && it->second == code) return false;
        by[who] = code; return true;
    }
    int count(const std::string& code) const { int n = 0; for (auto& kv : by) if (kv.second == code) n++; return n; }
    bool mine(const std::string& who, const std::string& code) const { auto it = by.find(who); return it != by.end() && it->second == code; }
};

}}  // namespace hub::profile
