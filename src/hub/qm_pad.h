#pragma once
/* Pure logic for the quick menu's "How do I play" card and in-game chat (quickmenu.cpp). Header-only; covered by hub_qm_test. */
#include <map>
#include <string>
#include <vector>

#include "controls.h"

namespace hubqm {

/* ---- "How do I play": which game control each physical pad button triggers, from the live profile ----
 * Keys: a b x y lb rb lt rt ls rs (stick, incl. its click) dpup dpdown dpleft dpright start back. Anything else (guide, paddles ...) is
 * not drawn. A button with no control is simply missing from the map. */
inline std::string pad_key_for_term(const hubctl::Term& t) {
    if (t.axis) {
        std::string n = hubctl::axis_name(t.idx);
        if (n == "lefttrigger") return "lt";
        if (n == "righttrigger") return "rt";
        if (n == "leftx" || n == "lefty") return "ls";
        if (n == "rightx" || n == "righty") return "rs";
        return "";
    }
    std::string n = hubctl::button_name(t.idx);
    if (n == "a" || n == "b" || n == "x" || n == "y" || n == "start") return n;
    if (n == "back") return "back";
    if (n == "leftshoulder") return "lb";
    if (n == "rightshoulder") return "rb";
    if (n == "leftstick") return "ls";
    if (n == "rightstick") return "rs";
    if (n == "dpup" || n == "dpdown" || n == "dpleft" || n == "dpright") return n;
    return "";
}

inline std::map<std::string, std::vector<std::string>> pad_callouts(const hubctl::Profile& p) {
    std::map<std::string, std::vector<std::string>> out;
    auto add = [&](const std::string& key, const std::string& label) {
        if (key.empty()) return;
        auto& v = out[key];
        for (auto& e : v) if (e == label) return;
        v.push_back(label);
    };
    for (const auto& ci : hubctl::controls_for(p.system)) {
        if (ci.analog) {
            auto it = p.analogs.find(ci.name);
            std::string ax = it != p.analogs.end() ? it->second.x : std::string();
            std::string key = ax.compare(0, 5, "right") == 0 ? "rs" : "ls";
            if (ax.empty() && (ci.name.find("right") != std::string::npos || ci.name == "c_stick")) key = "rs";
            add(key, ci.label);
            continue;
        }
        auto it = p.buttons.find(ci.name);
        if (it == p.buttons.end()) continue;
        std::vector<hubctl::Term> terms;
        if (!hubctl::parse_expr(it->second, p.axis_threshold, &terms)) continue;
        for (const auto& t : terms) add(pad_key_for_term(t), ci.label);
    }
    return out;
}

/* "A / B" for a callout (max `max_parts` names, then "+N"). */
inline std::string join_labels(const std::vector<std::string>& v, size_t max_parts = 3) {
    std::string r;
    for (size_t i = 0; i < v.size() && i < max_parts; ++i) r += (i ? " / " : "") + v[i];
    if (v.size() > max_parts) r += " +" + std::to_string(v.size() - max_parts);
    return r;
}

/* ---- in-game chat on the runner OSD ---- */
constexpr int kChatOff = 0, kChatRoom = 1, kChatAll = 2;
inline bool chat_osd_wanted(int mode, bool is_dm) { return mode == kChatAll || (mode == kChatRoom && !is_dm); }

/* "Name: text" as the runner OSD can draw it: printable ASCII only (a run of other bytes becomes one '?'), whitespace collapsed,
 * at most 118 bytes (runner_ctl text limit is 120). "" when there is nothing to show. */
inline std::string chat_osd_text(const std::string& who, const std::string& text) {
    std::string raw = (who.empty() ? std::string() : who + ": ") + text, out;
    bool bad = false, sp = true;
    for (unsigned char c : raw) {
        if (c >= 0x80 || (c < 32 && c != '\t' && c != '\n' && c != '\r')) { if (!bad) out += '?'; bad = true; sp = false; continue; }
        bad = false;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { if (!sp) out += ' '; sp = true; continue; }
        out += (char)c; sp = false;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (out.size() > 118) out = out.substr(0, 115) + "...";
    if (out.find_first_not_of("? ") == std::string::npos) return "";
    return out;
}

/* At most `max` messages per `window_ms`, so a flood cannot bury the game. */
struct ChatOsdLimiter {
    long long t[8] = {0};
    int n = 0;
    bool allow(long long now_ms, int max = 5, long long window_ms = 6000) {
        if (max > 8) max = 8;
        int k = 0;
        for (int i = 0; i < n; ++i) if (now_ms - t[i] < window_ms) t[k++] = t[i];
        n = k;
        if (n >= max) return false;
        t[n++] = now_ms;
        return true;
    }
};

inline const std::vector<std::string>& quick_phrases() {
    static const std::vector<std::string> v = {"GG", "One more?", "BRB", "Lag?"};
    return v;
}

}  // namespace hubqm
