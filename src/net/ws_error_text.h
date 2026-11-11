/* Maps the raw low-level WebSocket error (WsClient::error(), technical text kept for the log) to a kind and to plain player text.
 * Pure and header-only so it is unit-tested offline. Classification uses the stable prefixes ws.cpp writes. */
#pragma once
#include <cstdlib>
#include <cstring>
#include <string>

namespace hubnet {

enum class WsErrorKind { None, Unreachable, ServerBusy, Blocked, Dropped, Unknown };

inline WsErrorKind classify_ws_error(const std::string& raw)
{
    if (raw.empty()) return WsErrorKind::None;
    auto starts = [&](const char* p) { return raw.rfind(p, 0) == 0; };
    if (starts("couldn't reach ") || starts("couldn't connect to ")) return WsErrorKind::Unreachable;
    if (starts("server answered HTTP ")) {
        static const char kPre[] = "server answered HTTP ";
        char* end = nullptr;
        long code = std::strtol(raw.c_str() + (sizeof(kPre) - 1), &end, 10);
        if (end == raw.c_str() + (sizeof(kPre) - 1)) return WsErrorKind::Unknown;
        if (code == 429 || (code >= 500 && code <= 599)) return WsErrorKind::ServerBusy;
        return WsErrorKind::Unknown;
    }
    if (starts("WebSocket upgrade failed")) return WsErrorKind::Blocked;
    if (starts("connection to the server was lost") || starts("server closed the connection")) return WsErrorKind::Dropped;
    return WsErrorKind::Unknown;
}

inline const char* ws_error_player_text(WsErrorKind k)
{
    switch (k) {
        case WsErrorKind::Unreachable: return "Can't reach the RetroHub server. Check your internet connection and try again.";
        case WsErrorKind::ServerBusy:  return "The server is busy. Wait a minute and try again.";
        case WsErrorKind::Blocked:     return "The connection was blocked. A VPN or proxy may be interfering.";
        case WsErrorKind::Dropped:     return "The connection dropped.";
        default:                       return "Couldn't connect. Try again in a moment.";
    }
}

/* The reason text the SERVER sent in its close frame ("server closed the connection: <reason>"): trimmed, first line only, capped. Empty if none. */
inline std::string ws_server_reason(const std::string& raw)
{
    static const char kPre[] = "server closed the connection: ";
    if (raw.rfind(kPre, 0) != 0) return std::string();
    std::string r = raw.substr(sizeof(kPre) - 1);
    size_t nl = r.find_first_of("\r\n");
    if (nl != std::string::npos) r.resize(nl);
    size_t a = r.find_first_not_of(" \t");
    if (a == std::string::npos) return std::string();
    size_t b = r.find_last_not_of(" \t");
    r = r.substr(a, b - a + 1);
    if (r.size() > 120) r.resize(120);
    return r;
}

inline std::string ws_error_player_text(const std::string& raw)
{
    std::string reason = ws_server_reason(raw);
    return reason.empty() ? std::string(ws_error_player_text(classify_ws_error(raw))) : reason;
}

} // namespace hubnet
