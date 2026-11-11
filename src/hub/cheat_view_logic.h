#pragma once
/* Cheats list, display side only (pure, unit tested). Cheat databases often list the same code twice. This decides which rows the
 * player sees and what each one is called. It never changes which cheats get applied: the store and the runner still see every row. */
#include <cctype>
#include <map>
#include <string>
#include <vector>

namespace hubcheatview {

struct In {
    std::string desc;
    std::vector<std::string> codes;
    bool enabled = false;
    bool master = false;       // an enable/master code other cheats depend on
    int format = 0;            // any number that tells code types apart
};

struct Out {
    int src = -1;              // index into the input
    std::string label;         // name to show
    bool master = false;
};

inline std::string norm_text(const std::string& s) {
    std::string o;
    bool sp = false;
    for (unsigned char c : s) {
        if (std::isspace(c)) { sp = !o.empty(); continue; }
        if (sp) { o += ' '; sp = false; }
        o += (char)std::tolower(c);
    }
    return o;
}

inline std::string code_key(const std::vector<std::string>& codes) {
    std::string k;
    for (auto& c : codes) { k += norm_text(c); k += '\n'; }
    return k;
}

/* One entry per distinct (name, code content). Identical name AND codes: shown once (an enabled copy wins, so a switched-on cheat never
 * hides). Same name, different codes: all shown, the later ones get " (version 2)", " (version 3)". Order follows the input. */
inline std::vector<Out> dedupe(const std::vector<In>& in) {
    std::map<std::string, int> keep;       // name + codes -> index kept
    for (size_t i = 0; i < in.size(); ++i) {
        const std::string k = norm_text(in[i].desc) + "\x01" + code_key(in[i].codes);
        auto it = keep.find(k);
        if (it == keep.end()) keep[k] = (int)i;
        else if (in[i].enabled && !in[(size_t)it->second].enabled) it->second = (int)i;
    }
    std::vector<Out> out;
    std::map<std::string, int> seen_name;
    for (size_t i = 0; i < in.size(); ++i) {
        const std::string k = norm_text(in[i].desc) + "\x01" + code_key(in[i].codes);
        if (keep[k] != (int)i) continue;
        const int n = ++seen_name[norm_text(in[i].desc)];
        Out o;
        o.src = (int)i;
        o.label = n > 1 ? in[i].desc + " (version " + std::to_string(n) + ")" : in[i].desc;
        o.master = in[i].master;
        out.push_back(o);
    }
    return out;
}

/* The code type ("GameShark") only helps when the list mixes types; with a single type it is just jargon. */
inline bool show_code_type(const std::vector<In>& in) {
    for (auto& r : in) if (r.format != in.front().format) return true;
    return false;
}

inline const char* master_note() { return "Needed for the other cheats to work"; }

}  // namespace hubcheatview
