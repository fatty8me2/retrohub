#pragma once
/* Proof of the account for a game-room socket (the server's strict_rooms switch: every seat sends {type:"ident"} after the welcome).
 *
 * A process that opens /hub by itself (n64run, retrorun, streamrun, ...) is handed the account through the INHERITED ENVIRONMENT, never the
 * command line (other programs can read a command line). The hub sets RETROHUB_IDENT only around the CreateProcess of its own runners and
 * clears it again (launch.cpp spawn). The value binds the secret to ONE server, so a runner pointed anywhere else sends nothing:
 *     RETROHUB_IDENT = "<server url>|<uid>|<secret>"
 * The secret is never logged. Game rooms are proof only: nopair is always true. */
#include <mutex>
#include <string>

namespace hubnet {

constexpr const char* kIdentEnv = "RETROHUB_IDENT";

inline std::string ident_norm_server(std::string s)
{
    while (!s.empty() && s.back() == '/') s.pop_back();
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}
inline bool ident_field_ok(const std::string& s, size_t n)
{
    if (s.size() != n) return false;
    for (char c : s) if (!((c >= 'a' && c <= 'z') || (c >= '2' && c <= '7'))) return false;   // base32: also means nothing here can break the JSON
    return true;
}
inline std::string ident_env_value(const std::string& server, const std::string& uid, const std::string& secret)
{
    return server + "|" + uid + "|" + secret;
}
/* The frame to send to `cfg_server`, or "" (no value, malformed, or the value is bound to another server). */
inline std::string ident_frame_from_env(const std::string& cfg_server, const char* env_value)
{
    if (!env_value || !*env_value) return "";
    const std::string v = env_value;
    const size_t a = v.find('|'), b = a == std::string::npos ? a : v.find('|', a + 1);
    if (b == std::string::npos) return "";
    const std::string server = v.substr(0, a), uid = v.substr(a + 1, b - a - 1), secret = v.substr(b + 1);
    if (server.empty() || ident_norm_server(server) != ident_norm_server(cfg_server)) return "";
    if (!ident_field_ok(uid, 26) || !ident_field_ok(secret, 52)) return "";
    return "{\"type\":\"ident\",\"uid\":\"" + uid + "\",\"secret\":\"" + secret + "\",\"nopair\":true}";
}

/* In-process sessions (the hub's own 3DS / GameCube forwarders): the hub registers the account once; used only for the same server. */
struct ProcessIdent { std::mutex mu; std::string server, uid, secret; };
inline ProcessIdent& process_ident_store() { static ProcessIdent p; return p; }
inline void set_process_ident(const std::string& server, const std::string& uid, const std::string& secret)   // empty uid clears it
{
    ProcessIdent& p = process_ident_store();
    std::lock_guard<std::mutex> lk(p.mu);
    p.server = server; p.uid = uid; p.secret = secret;
}
inline std::string process_ident_frame(const std::string& cfg_server)
{
    ProcessIdent& p = process_ident_store();
    std::lock_guard<std::mutex> lk(p.mu);
    if (p.uid.empty()) return "";
    return ident_frame_from_env(cfg_server, ident_env_value(p.server, p.uid, p.secret).c_str());
}

} // namespace hubnet
