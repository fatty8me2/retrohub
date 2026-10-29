/* WsClient::close() must return promptly whatever the server does (offline, 127.0.0.1 only).
 * Regression for the v1.29.2 UI hangs: close() ran WinHttpWebSocketClose on the UI thread, which waits for the server's close frame
 * with an infinite receive timeout. A server that closed first (its frame already consumed by the receive thread) or went silent
 * froze the hub for good on the next reconnect/stop. */
#include "net_internal.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using hubnet::detail::WsClient;
using hubnet::detail::WsState;

static std::string accept_key(const std::string& key) {
    std::string in = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char h[20];
    BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0, (PUCHAR)in.data(), (ULONG)in.size(), h, 20);
    static const char* b = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (int i = 0; i < 20; i += 3) {
        unsigned v = h[i] << 16 | (i + 1 < 20 ? h[i + 1] << 8 : 0) | (i + 2 < 20 ? h[i + 2] : 0);
        o += b[v >> 18 & 63]; o += b[v >> 12 & 63];
        o += i + 1 < 20 ? b[v >> 6 & 63] : '='; o += i + 2 < 20 ? b[v & 63] : '=';
    }
    return o;
}

/* mode 0: server sends a close frame and then ignores everything (never answers ours). mode 1: server goes silent after the upgrade. */
static int run_case(int mode) {
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    bind(ls, (sockaddr*)&a, sizeof a); listen(ls, 1);
    int al = sizeof a; getsockname(ls, (sockaddr*)&a, &al);
    int port = ntohs(a.sin_port);
    std::atomic<bool> done{false};
    std::thread srv([&] {
        SOCKET c = accept(ls, nullptr, nullptr);
        std::string req; char buf[2048];
        while (req.find("\r\n\r\n") == std::string::npos) { int n = recv(c, buf, sizeof buf, 0); if (n <= 0) break; req.append(buf, n); }
        std::string key;
        size_t k = req.find("Sec-WebSocket-Key:");
        if (k == std::string::npos) k = req.find("sec-websocket-key:");
        if (k != std::string::npos) { size_t s = req.find_first_not_of(' ', k + 18), e = req.find("\r\n", s); key = req.substr(s, e - s); }
        std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept_key(key) + "\r\n\r\n";
        send(c, resp.data(), (int)resp.size(), 0);
        if (mode == 0) { const char cf[] = {(char)0x88, 2, 0x03, (char)0xE8}; send(c, cf, 4, 0); }   // close 1000
        while (!done) { u_long av = 0; ioctlsocket(c, FIONREAD, &av); if (av) recv(c, buf, sizeof buf, 0); Sleep(20); }   // read, never answer
        closesocket(c);
    });
    WsClient ws;
    ws.connect("ws://127.0.0.1:" + std::to_string(port) + "/friends");
    auto t0 = std::chrono::steady_clock::now();
    while (ws.state() == WsState::Connecting && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) Sleep(10);
    Sleep(300);   // mode 0: the receive thread has consumed the server's close frame
    auto c0 = std::chrono::steady_clock::now();
    ws.close();
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
    done = true; srv.join(); closesocket(ls);
    std::printf("mode %d: state before close ok, close() took %.2f s\n", mode, secs);
    return secs < 3.0 ? 0 : 1;
}

/* mode 2: close() while the connect is still pending toward a black-holed address must not wait out the connect timeouts. */
static int run_connecting_case() {
    int bad = 0;
    for (int delay : {0, 1, 5, 20, 150}) {   // close() racing every stage of the connect
        WsClient ws;
        ws.connect("ws://10.255.255.1:81/friends");
        if (delay) Sleep(delay);
        auto c0 = std::chrono::steady_clock::now();
        ws.close();
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
        std::printf("connecting: close() after %d ms took %.2f s\n", delay, secs);
        if (secs >= 3.0) ++bad;
    }
    return bad ? 1 : 0;
}

int main() {
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    int fails = 0;
    for (int m = 0; m < 2; ++m) {   // run each case on a watchdog: a hang in close() is the failure being tested
        std::atomic<int> r{-1};
        std::thread t([&] { r = run_case(m); });
        for (int i = 0; i < 150 && r < 0; ++i) Sleep(100);
        if (r < 0) { std::printf("FAIL: mode %d close() hung (> 15 s)\n", m); std::fflush(stdout); TerminateProcess(GetCurrentProcess(), 1); }
        t.join();
        if (r) { std::printf("FAIL: mode %d close() too slow\n", m); ++fails; }
    }
    {
        std::atomic<int> r{-1};
        std::thread t([&] { r = run_connecting_case(); });
        for (int i = 0; i < 150 && r < 0; ++i) Sleep(100);
        if (r < 0) { std::printf("FAIL: close() during connect hung (> 15 s)\n"); std::fflush(stdout); TerminateProcess(GetCurrentProcess(), 1); }
        t.join();
        if (r) { std::printf("FAIL: close() during connect too slow\n"); ++fails; }
    }
    if (!fails) std::printf("PASS\n");
    return fails;
}
