#pragma once
/* Remote play (Wii U couch co-op) room logic. Pure: no UI, no processes, so a console test covers it.
 * Contract with streamrun: docs/REMOTE_PLAY.md (src/stream). Media goes peer-to-peer only; the room server carries signalling. */
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace hubrp {

inline const char* kNoDirectText = "Couldn't connect directly to the host - remote play needs a direct connection (try another network / enable UPnP)";

/* Room name streamrun joins for signalling: CODE-N (the Worker only accepts that shape). */
inline std::string room_name(const std::string& code, int launch_no) { return code + "-" + std::to_string(launch_no); }

inline std::string quote(const std::string& s) {
    std::string o = "\"";
    for (char c : s) if (c != '"') o += c;
    return o + "\"";
}

/* Guests = everyone but the host (the host plays on its own pad); Cemu has 4 pad slots so at most 3 guests. */
inline int guest_count(int members) { return members <= 1 ? 0 : (members - 1 > 3 ? 3 : members - 1); }

/* streamrun --host ... ; `capture_pid` is Cemu's PID (streamrun picks its biggest window and captures its audio). Guests' pads reach
 * Cemu through the DSU server embedded in streamrun (127.0.0.1:dsu_port, players 2-4); status_file feeds the room's "streaming to" line. */
inline std::string host_args(unsigned long capture_pid, const std::string& net, const std::string& room, int members, bool upnp,
                             const std::string& status_file = "", int dsu_port = 0, const std::string& await_file = "") {
    return "--host " + (await_file.empty() ? "--capture-hwnd " + std::to_string(capture_pid) : "--await-pid-file " + quote(await_file)) + " --net " + quote(net) + " --room " + room +
           " --slot 0 --guests " + std::to_string(guest_count(members)) + (upnp ? " --upnp" : "") +
           (status_file.empty() ? "" : " --status-file " + quote(status_file)) + (dsu_port > 0 ? " --dsu-port " + std::to_string(dsu_port) : "");
}

/* streamrun --guest ... */
inline std::string guest_args(const std::string& net, const std::string& room, int slot, bool fullscreen) {
    return "--guest --net " + quote(net) + " --room " + room + " --slot " + std::to_string(slot) + (fullscreen ? " --fullscreen" : "");
}

/* ---- direct-connection pre-check (streamrun --probe): host + every guest join a throwaway session room and ping each other over
 * hole-punched UDP with the relay disabled. Runs before Launch; a guest that cannot be reached blocks the launch with a reason. ---- */
inline std::string probe_room(const std::string& code, int n) { return code + "-" + std::to_string(100 + n % 800); }
inline std::string probe_msg(const std::string& room, int members) { return "{\"kind\":\"rplay_probe\",\"room\":\"" + room + "\",\"pcs\":" + std::to_string(members) + "}"; }
inline bool probe_msg_is(const std::string& body) { return body.find("\"rplay_probe\"") != std::string::npos; }
inline std::string probe_host_args(const std::string& net, const std::string& room, int members, bool upnp, const std::string& result_file, int ms = 9000) {
    return "--probe --host --net " + quote(net) + " --room " + room + " --slot 0 --pcs " + std::to_string(members) + " --probe-ms " + std::to_string(ms) +
           (upnp ? " --upnp" : "") + " --stats-file " + quote(result_file);
}
inline std::string probe_guest_args(const std::string& net, const std::string& room, int slot, int members, bool upnp, int ms = 9000) {
    return "--probe --guest --net " + quote(net) + " --room " + room + " --slot " + std::to_string(slot) + " --pcs " + std::to_string(members) +
           " --probe-ms " + std::to_string(ms) + (upnp ? " --upnp" : "");
}
/* The probe result file is {"ok":bool,"slots":{"1":rtt,"2":-1}}: slot -> rtt ms, -1 = no direct path. */
inline std::map<int, int> parse_probe_slots(const std::string& j) {
    std::map<int, int> m;
    size_t p = j.find("\"slots\"");
    if (p == std::string::npos) return m;
    p = j.find('{', p);
    size_t e = j.find('}', p == std::string::npos ? 0 : p);
    if (p == std::string::npos || e == std::string::npos) return m;
    std::string body = j.substr(p + 1, e - p - 1);
    size_t i = 0;
    while ((i = body.find('"', i)) != std::string::npos) {
        size_t q = body.find('"', i + 1);
        if (q == std::string::npos) break;
        int slot = std::atoi(body.substr(i + 1, q - i - 1).c_str());
        size_t c = body.find(':', q);
        if (c == std::string::npos) break;
        m[slot] = std::atoi(body.c_str() + c + 1);
        i = body.find(',', c);
        if (i == std::string::npos) break;
    }
    return m;
}
/* "" = every guest reachable. `names`: slot -> player name; a slot the host expected but the file lacks counts as unreachable. */
inline std::string probe_block_reason(const std::map<int, int>& rtt, const std::map<int, std::string>& names) {
    std::vector<std::string> v;
    for (auto& kv : names) {
        if (kv.first == 0) continue;
        auto it = rtt.find(kv.first);
        if (it != rtt.end() && it->second >= 0) continue;
        v.push_back(kv.second.empty() ? std::string("Player ") + std::to_string(kv.first + 1) : kv.second);
    }
    if (v.empty()) return "";
    std::string bad;
    for (size_t n = 0; n < v.size(); n++) bad += (n ? (n + 1 == v.size() ? " and " : ", ") : "") + v[n];
    return std::string("Can't reach ") + bad + " directly (no peer-to-peer path, and remote play never goes through the relay). Try enabling UPnP on the router, another network, or a wired connection.";
}

/* Host status line: {"kbps":N,"guests":[{"slot":1,"rtt_ms":48,...}]} + names -> "streaming to Mickey · 12 Mbps · 48 ms" ("" = nobody). */
inline std::string status_text(const std::string& j, const std::map<int, std::string>& names) {
    std::vector<std::string> who;
    int worst = -1;
    size_t g = j.find("\"guests\"");
    if (g == std::string::npos) return "";
    size_t i = g;
    while ((i = j.find("\"slot\":", i)) != std::string::npos) {
        int slot = std::atoi(j.c_str() + i + 7);
        size_t r = j.find("\"rtt_ms\":", i);
        size_t end = j.find('}', i);
        int rtt = (r != std::string::npos && (end == std::string::npos || r < end)) ? std::atoi(j.c_str() + r + 9) : -1;
        auto it = names.find(slot);
        who.push_back(it != names.end() && !it->second.empty() ? it->second : "Player " + std::to_string(slot + 1));
        if (rtt > worst) worst = rtt;
        i = end == std::string::npos ? j.size() : end;
    }
    if (who.empty()) return "";
    size_t k = j.find("\"kbps\":");
    int kbps = k == std::string::npos ? 0 : std::atoi(j.c_str() + k + 7);
    std::string w;
    for (size_t n = 0; n < who.size(); n++) w += (n ? (n + 1 == who.size() ? " and " : ", ") : "") + who[n];
    char b[32];
    std::snprintf(b, sizeof b, "%.0f Mbps", kbps / 1000.0);
    return "streaming to " + w + " · " + b + (worst >= 0 ? " · " + std::to_string(worst) + " ms" : "");
}

/* What a streamrun exit code means for the player (docs/REMOTE_PLAY.md: 0 ok, 2 usage, 3 no direct path, 4 ended, 5 capture/decode). */
inline std::string exit_text(unsigned long code, bool host) {
    switch (code) {
        case 0: return host ? "Remote play ended." : "The stream ended.";
        case 2: return "Remote play couldn't start (bad settings) - update RetroHub and try again.";
        case 3: return kNoDirectText;
        case 4: return host ? "A remote-play guest connection failed." : "The host's game ended - back to the room.";
        case 5: return host ? "Couldn't capture or encode the Cemu window." : "Couldn't decode the stream on this PC.";
        default: return "Remote play exited with code " + std::to_string(code) + ".";
    }
}

/* Host: when to attach streamrun to Cemu. The game window shows up some seconds after Cemu starts. */
enum class Attach { Wait, Now, GiveUp };
inline Attach attach_decision(bool cemu_alive, bool window_found, unsigned long long waited_ms, unsigned long long timeout_ms = 90000) {
    if (!cemu_alive) return Attach::GiveUp;
    if (window_found) return Attach::Now;
    return waited_ms > timeout_ms ? Attach::GiveUp : Attach::Wait;
}

/* Launch message fragment: tells guests this is a remote-play launch (they stream; no game files needed). */
inline std::string launch_fragment(int members) { return ",\"np_rplay\":{\"v\":1,\"guests\":" + std::to_string(guest_count(members)) + "}"; }
inline bool launch_is_remote(const std::string& body) { return body.find("\"np_rplay\"") != std::string::npos; }

/* A player leaving a guest slot mid-game; the host's Cemu keeps running. A host leaving ends every guest's stream. */
inline std::string guest_left_text(const std::string& name) { return name + " left - their controller was disconnected."; }
inline std::string host_left_text(const std::string& name) { return (name.empty() ? std::string("The host") : name) + " left - the stream ended."; }

}  // namespace hubrp
