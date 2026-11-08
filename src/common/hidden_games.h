#pragma once
/* Hidden games (library-12): the pure parts of Hide / Unhide / "Show hidden", with no UI. Hidden keys live in the synced
 * prefs (Prefs::hidden); the "Show hidden" switch is a per-PC flag stored next to the other per-PC files. */
#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "prefs.h"

namespace hub {
namespace hidden {

/* The hidden list after hiding / unhiding one game (no duplicates, order kept). */
inline std::vector<std::string> with_key(const std::vector<std::string>& cur, const std::string& key) {
    std::vector<std::string> out = cur;
    if (!key.empty() && std::find(out.begin(), out.end(), key) == out.end()) out.push_back(key);
    return out;
}
inline std::vector<std::string> without_key(const std::vector<std::string>& cur, const std::string& key) {
    std::vector<std::string> out;
    for (const std::string& k : cur) if (k != key) out.push_back(k);
    return out;
}

/* Hide / unhide through the synced prefs. True when the prefs changed (and so need saving). `t` is the stamp (0 = now). */
inline bool hide(prefs::Prefs* p, const std::string& key, long long t = 0) { return prefs::set_hidden(p, with_key(p->hidden, key), t); }
inline bool unhide(prefs::Prefs* p, const std::string& key, long long t = 0) { return prefs::set_hidden(p, without_key(p->hidden, key), t); }

/* Takes the hidden games out of `v` and returns them (in their old order). `keep_key` (the bundled Smash entry) is never hidden.
 * `v` is the library every other page reads (Home rows, Discover, room lists), so a hidden game must not stay in it. */
template <class T>
std::vector<T> take_hidden(std::vector<T>* v, const std::set<std::string>& hid, const std::string& keep_key = std::string()) {
    std::vector<T> gone, kept;
    if (hid.empty()) return gone;
    for (T& g : *v) {
        if (g.key != keep_key && hid.count(g.key)) gone.push_back(std::move(g));
        else kept.push_back(std::move(g));
    }
    *v = std::move(kept);
    return gone;
}

/* "Show hidden" chip text; empty = do not draw the chip (nothing hidden and the switch is off, so it would only add clutter). */
inline std::string chip_label(bool show_hidden, size_t hidden_count) {
    if (hidden_count == 0 && !show_hidden) return std::string();
    return "Show hidden  " + std::to_string(hidden_count);
}

}  // namespace hidden
}  // namespace hub
