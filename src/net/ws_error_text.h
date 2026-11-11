/* Maps the raw low-level WebSocket error (WsClient::error(), technical text kept for the log) to a kind and to plain player text.
 * Pure and header-only so it is unit-tested offline. Classification uses the stable prefixes ws.cpp writes. */
#pragma once
#include <cstdlib>
#include <string>

namespace hubnet {

enum class WsErrorKind { None, Unreachable, ServerBusy, Blocked, Dropped, Unknown };

inline WsErrorKind classify_ws_error(const std::string& raw)
{
    if (raw.empty()) return WsErrorKind::None;
    auto starts = [&](const char* p) { return raw.rfind(p, 0) == 0; };
    if (starts("couldn't reach ") || starts("couldn't connect to ")) return WsErrorKind::Unreachable;
    if (starts("server answered HTTP ")) {
        int code = std::atoi(raw.c_str() + 20);
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

inline std::string ws_error_player_text(const std::string& raw) { return ws_error_player_text(classify_ws_error(raw)); }

} // namespace hubnet
