#pragma once
/* Remote play (Wii U couch co-op) room logic. Pure: no UI, no processes, so a console test covers it.
 * Contract with streamrun: docs/REMOTE_PLAY.md (src/stream). Media goes peer-to-peer only; the room server carries signalling. */
#include <string>

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

/* streamrun --host ... ; `capture_pid` is Cemu's PID (streamrun picks its biggest window and captures its audio). */
inline std::string host_args(unsigned long capture_pid, const std::string& net, const std::string& room, int members, bool upnp) {
    return "--host --capture-hwnd " + std::to_string(capture_pid) + " --net " + quote(net) + " --room " + room +
           " --slot 0 --guests " + std::to_string(guest_count(members)) + (upnp ? " --upnp" : "");
}

/* streamrun --guest ... */
inline std::string guest_args(const std::string& net, const std::string& room, int slot, bool fullscreen) {
    return "--guest --net " + quote(net) + " --room " + room + " --slot " + std::to_string(slot) + (fullscreen ? " --fullscreen" : "");
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
