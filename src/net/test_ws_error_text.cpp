/* Offline table test for the WebSocket error -> player text mapping. No network. */
#include "ws_error_text.h"
#include <cstdio>
#include <cstring>

using namespace hubnet;

int main()
{
    struct Row { const char* raw; WsErrorKind kind; const char* text; };
    const char* unreach = "Can't reach the RetroHub server. Check your internet connection and try again.";
    const char* busy = "The server is busy. Wait a minute and try again.";
    const char* unk = "Couldn't connect. Try again in a moment.";
    const Row rows[] = {
        {"couldn't reach example.com", WsErrorKind::Unreachable, unreach},
        {"couldn't connect to example.com (error 12007)", WsErrorKind::Unreachable, unreach},
        {"couldn't connect to example.com (error 12029)", WsErrorKind::Unreachable, unreach},
        {"server answered HTTP 429", WsErrorKind::ServerBusy, busy},
        {"server answered HTTP 500", WsErrorKind::ServerBusy, busy},
        {"server answered HTTP 503", WsErrorKind::ServerBusy, busy},
        {"server answered HTTP 404", WsErrorKind::Unknown, unk},
        {"server answered HTTP 200", WsErrorKind::Unknown, unk},
        {"WebSocket upgrade failed", WsErrorKind::Blocked, "The connection was blocked. A VPN or proxy may be interfering."},
        {"connection to the server was lost", WsErrorKind::Dropped, "The connection dropped."},
        {"server closed the connection", WsErrorKind::Dropped, "The connection dropped."},
        {"server closed the connection: room is full", WsErrorKind::Dropped, "room is full"},
        {"server closed the connection:   padded reason \nsecond line", WsErrorKind::Dropped, "padded reason"},
        {"server closed the connection: ", WsErrorKind::Dropped, "The connection dropped."},
        {"server answered HTTP ", WsErrorKind::Unknown, unk},
        {"something else entirely", WsErrorKind::Unknown, unk},
        {"", WsErrorKind::None, unk},
    };
    int fails = 0;
    for (const Row& r : rows) {
        if (classify_ws_error(r.raw) != r.kind) { std::printf("FAIL kind: '%s'\n", r.raw); ++fails; }
        if (ws_error_player_text(std::string(r.raw)) != r.text) { std::printf("FAIL text: '%s'\n", r.raw); ++fails; }
        std::string t = ws_error_player_text(std::string(r.raw));
        if (t.find("error 1") != std::string::npos || t.find("HTTP") != std::string::npos) { std::printf("FAIL jargon: '%s'\n", r.raw); ++fails; }
    }
    if (ws_error_player_text(std::string("server closed the connection: ") + std::string(300, 'x')).size() != 120) { std::printf("FAIL cap\n"); ++fails; }
    if (!fails) std::printf("PASS\n");
    return fails;
}
