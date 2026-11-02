#pragma once
/* Discord Rich Presence over the local IPC pipe: frame encoding, handshake / SET_ACTIVITY JSON and the opt-in gate. Pure.
 * No SDK: frames are  u32 opcode (LE) | u32 length (LE) | UTF-8 JSON.  Transport lives in discord_ipc.cpp. */
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>

namespace hubdiscord {

/* OWNER ACTION: create an application at https://discord.com/developers/applications, paste its Application ID here
 * (digits only) and upload the art assets named in console_asset(). While this is the placeholder nothing is ever sent. */
constexpr const char* kClientId = "REPLACE_WITH_DISCORD_APPLICATION_ID";

enum Op : uint32_t { Handshake = 0, Frame = 1, Close = 2, Ping = 3, Pong = 4 };

inline bool client_id_valid(const std::string& id) {
    if (id.size() < 17 || id.size() > 20) return false;
    for (char c : id) if (c < '0' || c > '9') return false;
    return true;
}

inline std::string esc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}

inline std::string encode_frame(uint32_t op, const std::string& json) {
    std::string f(8, '\0');
    uint32_t n = (uint32_t)json.size();
    for (int i = 0; i < 4; ++i) { f[i] = (char)((op >> (8 * i)) & 0xFF); f[4 + i] = (char)((n >> (8 * i)) & 0xFF); }
    return f + json;
}
/* Reads one frame from the front of `buf`; returns bytes consumed (0 = incomplete, -1 = malformed). */
inline int decode_frame(const std::string& buf, uint32_t* op, std::string* json) {
    if (buf.size() < 8) return 0;
    uint32_t o = 0, n = 0;
    for (int i = 0; i < 4; ++i) { o |= (uint32_t)(unsigned char)buf[i] << (8 * i); n |= (uint32_t)(unsigned char)buf[4 + i] << (8 * i); }
    if (n > (1u << 20) || o > 4) return -1;
    if (buf.size() < 8 + (size_t)n) return 0;
    *op = o; *json = buf.substr(8, n);
    return (int)(8 + n);
}

inline std::string handshake_json(const std::string& client_id) { return "{\"v\":1,\"client_id\":\"" + esc(client_id) + "\"}"; }

struct Presence {
    bool playing = false, in_room = false;
    std::string title, console, room_code, invite_url, image_url, console_key;
    int party_size = 0, party_max = 0;
    int64_t start_unix = 0;
};

struct Opts { bool enabled = false; bool join = false; };

/* Asset key uploaded to the Discord application for each console (fallback "retrohub"). */
inline std::string console_asset(const std::string& console_key) {
    std::string k;
    for (char c : console_key) k += (char)std::tolower((unsigned char)c);
    return k.empty() ? "retrohub" : "console_" + k;
}

/* Opt-in gate. Presence is sent only when the user turned it on, an application id is configured, and there is something to show. */
inline bool may_publish(const Opts& o, const Presence& p, const std::string& client_id = kClientId) {
    return o.enabled && client_id_valid(client_id) && (p.playing || p.in_room);
}
/* The Join button shares the room code with Discord friends: its own toggle, and only for a real room with an https invite link. */
inline bool may_join_button(const Opts& o, const Presence& p) {
    return o.enabled && o.join && p.in_room && !p.room_code.empty() && p.invite_url.rfind("https://", 0) == 0;
}

inline std::string clear_json(int pid, const std::string& nonce) {
    return "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(pid) + "},\"nonce\":\"" + esc(nonce) + "\"}";
}

/* Empty string when the gate says no. Never contains the room code or link unless may_join_button(). */
inline std::string activity_json(const Opts& o, const Presence& p, int pid, const std::string& nonce, const std::string& client_id = kClientId) {
    if (!may_publish(o, p, client_id)) return "";
    std::string a = "{";
    a += "\"details\":\"" + esc(p.playing ? p.title : std::string("In a RetroHub room")) + "\"";
    std::string state = p.playing ? p.console : std::string("Waiting to play");
    if (!state.empty()) a += ",\"state\":\"" + esc(state) + "\"";
    if (p.playing && p.start_unix > 0) a += ",\"timestamps\":{\"start\":" + std::to_string(p.start_unix) + "}";
    std::string large = p.image_url.rfind("https://", 0) == 0 ? p.image_url : console_asset(p.console_key);
    a += ",\"assets\":{\"large_image\":\"" + esc(large) + "\",\"large_text\":\"" + esc(p.playing ? p.title : std::string("RetroHub")) + "\"";
    if (large != console_asset(p.console_key)) a += ",\"small_image\":\"" + esc(console_asset(p.console_key)) + "\",\"small_text\":\"" + esc(p.console) + "\"";
    a += "}";
    if (p.in_room && p.party_size > 0 && p.party_max >= p.party_size)
        a += ",\"party\":{\"id\":\"retrohub-party\",\"size\":[" + std::to_string(p.party_size) + "," + std::to_string(p.party_max) + "]}";
    if (may_join_button(o, p)) a += ",\"buttons\":[{\"label\":\"Join room\",\"url\":\"" + esc(p.invite_url) + "\"}]";
    a += "}";
    return "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(pid) + ",\"activity\":" + a + "},\"nonce\":\"" + esc(nonce) + "\"}";
}

}  // namespace hubdiscord
