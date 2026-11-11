#pragma once
/* Controllers page: the name shown for a connected pad (pure, unit tested). Windows reports names with stray spaces, and an Xbox pad
 * already says "Xbox" in its own name, so adding "(Xbox)" after it only repeats the word. */
#include <cctype>
#include <string>

namespace hubpadname {

inline std::string tidy(const std::string& s) {
    std::string o;
    bool sp = false;
    for (unsigned char c : s) {
        if (std::isspace(c)) { sp = !o.empty(); continue; }
        if (sp) { o += ' '; sp = false; }
        o += (char)c;
    }
    return o;
}

inline bool has_word(const std::string& hay, const char* needle) {
    std::string h, n;
    for (unsigned char c : hay) h += (char)std::tolower(c);
    for (const char* p = needle; *p; ++p) n += (char)std::tolower((unsigned char)*p);
    return h.find(n) != std::string::npos;
}

/* name = what the system reports; xinput = it is read through XInput (an Xbox-style pad). */
inline std::string label(const std::string& name, bool xinput) {
    std::string n = tidy(name);
    if (n.empty()) n = "Controller";
    if (xinput && !has_word(n, "xbox")) n += " (Xbox)";
    return n;
}

}  // namespace hubpadname
