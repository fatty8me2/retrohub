#pragma once
/* Pure helpers that wire per-game settings (src/common/game_settings.h) into launches. Header-only, no UI, no processes;
 * covered by hub_launchset_test. */
#include <string>
#include <vector>

#include "game_settings.h"
#include "logic.h"

namespace hubui {

/* One command-line argument, quoted for CreateProcess when needed (spaces, quotes, empty). */
inline std::string cmd_quote(const std::string& s) {
    if (!s.empty() && s.find_first_of(" \t\"") == std::string::npos) return s;
    std::string o = "\"";
    size_t bs = 0;
    for (char c : s) {
        if (c == '\\') { ++bs; continue; }
        if (c == '"') { o.append(bs * 2 + 1, '\\'); o += '"'; }
        else { o.append(bs, '\\'); o += c; }
        bs = 0;
    }
    o.append(bs * 2, '\\');
    return o + "\"";
}

inline std::string join_cmd(const std::vector<std::string>& a) {
    std::string o;
    for (auto& s : a) o += " " + cmd_quote(s);
    return o;
}

/* Settings of one game: option defaults <- system defaults <- game override. The hub's global "Start games fullscreen"
 * is the default for every system: it applies unless the system or the game set "fullscreen" themselves. */
inline hub::gs::Values effective_values(const hub::gs::Store& st, hub::System s, const std::string& game_key, bool global_fullscreen, int global_volume = 100) {
    hub::gs::Values v = st.resolve(s, game_key);
    if (st.source(s, game_key, "fullscreen") == 0) v["fullscreen"] = global_fullscreen ? "true" : "false";
    if (st.source(s, game_key, "volume") == 0) v["volume"] = std::to_string(global_volume < 0 ? 0 : global_volume > 100 ? 100 : global_volume);   // global volume is the default
    return v;
}

/* Values for an online session. Host: its own Sync values go out; guests: `host_launch_body` (the raw launch message)
 * supplies them. Locked options are forced to their defaults for everyone. */
inline hub::gs::Values online_values(hub::System s, const hub::gs::Values& local, bool is_host, const std::string& host_launch_body) {
    hub::gs::Values host = is_host ? hub::gs::sync_subset(local) : hub::gs::extract_sync(host_launch_body);
    return hub::gs::online_effective(s, local, host);
}

/* Extra n64run arguments: --gfx pairs (+ --fullscreen). */
inline std::string n64_extra_args(const hub::gs::Values& v) {
    std::string a = join_cmd(hub::gs::n64_args(v));
    if (hub::gs::is_fullscreen(v)) a += " --fullscreen";
    if (hub::gs::volume_of(v) != 100) a += " --volume " + std::to_string(hub::gs::volume_of(v));
    return a;
}

/* Extra retrorun arguments: --opt / --filter / --integer-scale (+ --fullscreen). */
inline std::string retro_extra_args(hub::System s, const hub::gs::Values& v) {
    std::string a = join_cmd(hub::gs::retrorun_args(s, v));
    if (hub::gs::is_fullscreen(v)) a += " --fullscreen";
    if (hub::gs::volume_of(v) != 100) a += " --volume " + std::to_string(hub::gs::volume_of(v));
    return a;
}

/* ",\"gs\":{...}" for launch_json's extra_json: the host's Sync values (empty when there are none). */
inline std::string host_launch_fragment(const hub::gs::Values& resolved) { return hub::gs::sync_fragment(resolved); }

}  // namespace hubui
