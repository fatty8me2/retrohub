/* WsClient::close() must return promptly whatever the server does (offline, 127.0.0.1 only).
 * Regression for the v1.29.2 UI hangs: close() ran WinHttpWebSocketClose on the UI thread, which waits for the server's close frame
 * with an infinite receive timeout. A server that closed first (its frame already consumed by the receive thread) or went silent
 * froze the hub for good on the next reconnect/stop. */
#include "plat/plat.h"
#include "net_internal.h"
#if RH_WIN
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

#else   // ------------------------------------------------------------------------------------------------ macOS

/* macOS: the same close() cases against an in-process loopback server (BSD sockets, plat_crypto for the accept key), plus the
 * WsClient behaviour checks (echo, Waker, server close code, drop_abruptly, refused / unresolvable host). Offline. */
#include "plat/plat_crypto.h"
#include "plat/plat_sock.h"
#include "plat/plat_wake.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace plat;
using hubnet::detail::WsClient;
using hubnet::detail::WsState;
using hubnet::WsErrorKind;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static std::string b64(const std::string& in) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8) | (unsigned char)in[i + 2];
        o += T[v >> 18]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += T[v & 63];
    }
    if (i + 1 == in.size()) { unsigned v = (unsigned char)in[i] << 16; o += T[v >> 18]; o += T[(v >> 12) & 63]; o += "=="; }
    else if (i + 2 == in.size()) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8);
        o += T[v >> 18]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += '=';
    }
    return o;
}

static std::atomic<bool> g_done{false};

static bool read_exact(socket_t s, void* buf, size_t n) {
    char* p = (char*)buf;
    while (n) {
        if (g_done) return false;
        pollfd pf{s, POLLIN, 0};
        int r = poll(&pf, 1, 50);
        if (r == 0) continue;
        if (r < 0) return false;
        ssize_t k = recv(s, p, n, 0);
        if (k <= 0) return false;
        p += k; n -= (size_t)k;
    }
    return true;
}
static void send_all(socket_t s, const std::string& d) {
    size_t off = 0;
    while (off < d.size()) { ssize_t k = send(s, d.data() + off, d.size() - off, 0); if (k <= 0) return; off += (size_t)k; }
}
static std::string frame(int op, const std::string& pl) {
    std::string f;
    f += (char)(0x80 | op);
    if (pl.size() < 126) f += (char)pl.size();
    else { f += (char)126; f += (char)(pl.size() >> 8); f += (char)(pl.size() & 255); }
    return f + pl;
}

/* mode 0: send close 1000 then ignore everything. 1: silent after the upgrade. 2: echo; text "bye" -> close 4001. */
static void serve_one(socket_t c, int mode) {
    std::string head;
    char ch;
    while (head.size() < 65536 && (head.size() < 4 || head.compare(head.size() - 4, 4, "\r\n\r\n") != 0)) {
        if (!read_exact(c, &ch, 1)) { sock_close(c); return; }
        head += ch;
    }
    std::string low = head;
    for (auto& x : low) x = (char)tolower((unsigned char)x);
    size_t k = low.find("\r\nsec-websocket-key:");
    std::string key;
    if (k != std::string::npos) { k += 20; size_t e = head.find("\r\n", k); key = head.substr(k, e - k); while (!key.empty() && key[0] == ' ') key.erase(0, 1); }
    std::string acc = b64(hash(HashAlg::Sha1, key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
    send_all(c, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + acc + "\r\n\r\n");
    if (mode == 0) send_all(c, frame(8, std::string("\x03\xe8", 2)));
    while (!g_done) {
        unsigned char h[2];
        if (!read_exact(c, h, 2)) break;
        int op = h[0] & 15;
        size_t len = h[1] & 127;
        if (len == 126) { unsigned char e[2]; if (!read_exact(c, e, 2)) break; len = (size_t)((e[0] << 8) | e[1]); }
        unsigned char mask[4] = {0, 0, 0, 0};
        if ((h[1] & 128) && !read_exact(c, mask, 4)) break;
        std::string pl(len, '\0');
        if (len && !read_exact(c, &pl[0], len)) break;
        for (size_t i = 0; i < pl.size(); ++i) pl[i] ^= mask[i & 3];
        if (mode != 2) continue;   // read, never answer
        if (op == 1 && pl == "bye") { send_all(c, frame(8, std::string("\x0f\xa1", 2))); continue; }
        if (op == 1 || op == 2) send_all(c, frame(op, pl));
        else if (op == 8) { send_all(c, frame(8, pl)); break; }
    }
    sock_close(c);
}

struct Server {
    socket_t ls = kBadSocket;
    int port = 0;
    std::thread th;
    explicit Server(int mode) {
        ls = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
        bind(ls, (sockaddr*)&a, sizeof a); listen(ls, 4);
        socklen sl = sizeof a; getsockname(ls, (sockaddr*)&a, &sl);
        port = ntohs(a.sin_port);
        th = std::thread([this, mode] {
            std::vector<std::thread> conns;
            while (!g_done) {
                pollfd pf{ls, POLLIN, 0};
                if (poll(&pf, 1, 50) <= 0) continue;
                socket_t c = accept(ls, nullptr, nullptr);
                if (c != kBadSocket) conns.emplace_back(serve_one, c, mode);
            }
            for (auto& t : conns) t.join();
        });
    }
    std::string url() const { return "ws://127.0.0.1:" + std::to_string(port) + "/friends"; }
    ~Server() { g_done = true; th.join(); sock_close(ls); g_done = false; }
};

static bool wait_state(WsClient& ws, WsState want, int ms) {
    auto t0 = std::chrono::steady_clock::now();
    while (ws.state() != want && std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(ms)) sleep_ms(10);
    return ws.state() == want;
}
static bool wait_text(WsClient& ws, std::string* out, int ms) {
    auto t0 = std::chrono::steady_clock::now();
    while (!ws.poll_text(out)) { if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(ms)) return false; sleep_ms(5); }
    return true;
}
static bool wait_bin(WsClient& ws, std::vector<uint8_t>* out, int ms) {
    auto t0 = std::chrono::steady_clock::now();
    while (!ws.poll_binary(out)) { if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(ms)) return false; sleep_ms(5); }
    return true;
}

static int run_close_case(int mode) {
    Server srv(mode);
    WsClient ws;
    ws.connect(srv.url());
    wait_state(ws, WsState::Open, 5000);
    if (mode == 0) sleep_ms(300);   // the receive thread has consumed the server's close frame
    auto c0 = std::chrono::steady_clock::now();
    ws.close();
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
    std::printf("mode %d: close() took %.2f s, state %d\n", mode, secs, (int)ws.state());
    CHECK(ws.state() == WsState::Closed);
    return secs < 3.0 ? 0 : 1;
}

static int run_connecting_case() {
    int bad = 0;
    for (int delay : {0, 1, 5, 20, 150}) {   // close() racing every stage of the connect (black-holed address)
        WsClient ws;
        ws.connect("ws://10.255.255.1:81/friends");
        if (delay) sleep_ms(delay);
        auto c0 = std::chrono::steady_clock::now();
        ws.close();
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
        std::printf("connecting: close() after %d ms took %.2f s\n", delay, secs);
        if (secs >= 3.0) ++bad;
    }
    return bad;
}

static void run_behaviour() {
    {   // echo, Waker, close code, drop_abruptly
        Server srv(2);
        Waker waker;
        WsClient ws;
        ws.set_wake(&waker);
        CHECK(ws.connect(srv.url()));
        CHECK(wait_state(ws, WsState::Open, 5000));
        CHECK(ws.send_text("hello ws"));
        std::string t;
        CHECK(wait_text(ws, &t, 3000) && t == "hello ws");
        CHECK(wait_readable(waker, {}, 1000));   // signalled when the frame was queued
        std::string bin;
        for (int i = 0; i < 300; ++i) bin += (char)(i * 7);
        CHECK(ws.send_binary(bin.data(), (int)bin.size()));
        std::vector<uint8_t> b;
        CHECK(wait_bin(ws, &b, 3000) && std::string(b.begin(), b.end()) == bin);
        CHECK(wait_readable(waker, {}, 1000));
        CHECK(!wait_readable(waker, {}, 50));    // drained: nothing pending
        CHECK(ws.close_code() == 0 && ws.error().empty());

        CHECK(ws.send_text("bye"));              // server answers with close 4001
        CHECK(wait_state(ws, WsState::Failed, 3000));
        CHECK(ws.close_code() == 4001);
        CHECK(ws.error_kind() == WsErrorKind::Dropped);
        CHECK(!ws.send_text("x"));
        ws.close();
        CHECK(ws.state() == WsState::Closed);

        CHECK(ws.connect(srv.url()));            // reconnect on the same object, then drop without a close frame
        CHECK(wait_state(ws, WsState::Open, 5000));
        CHECK(ws.close_code() == 0);
        ws.drop_abruptly();
        CHECK(wait_state(ws, WsState::Failed, 3000));
        CHECK(ws.error_kind() == WsErrorKind::Dropped);
        ws.close();
        CHECK(ws.state() == WsState::Closed);
    }
    {   // refused port: what Windows reports as "couldn't connect to ..." (Unreachable)
        socket_t ls = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(ls, (sockaddr*)&a, sizeof a);
        socklen sl = sizeof a; getsockname(ls, (sockaddr*)&a, &sl);
        int port = ntohs(a.sin_port);
        sock_close(ls);
        WsClient ws;
        ws.connect("ws://127.0.0.1:" + std::to_string(port) + "/friends");
        CHECK(wait_state(ws, WsState::Failed, 8000));
        std::printf("refused: \"%s\"\n", ws.error().c_str());
        CHECK(ws.error_kind() == WsErrorKind::Unreachable);
        CHECK(!ws.send_text("x"));
        ws.close();
    }
    {   // unresolvable host
        WsClient ws;
        ws.connect("ws://no-such-host.invalid/friends");
        CHECK(wait_state(ws, WsState::Failed, 8000));
        std::printf("dns: \"%s\"\n", ws.error().c_str());
        CHECK(ws.error_kind() == WsErrorKind::Unreachable);
    }
    {   // bad scheme
        WsClient ws;
        ws.connect("http://127.0.0.1/");
        CHECK(wait_state(ws, WsState::Failed, 2000));
    }
}

template <class F> static void watchdog(const char* what, F f) {
    std::atomic<int> r{-1};
    std::thread t([&] { r = f(); });
    for (int i = 0; i < 300 && r < 0; ++i) sleep_ms(100);
    if (r < 0) { std::printf("FAIL: %s hung (> 30 s)\n", what); std::fflush(stdout); std::_Exit(1); }
    t.join();
    if (r) { std::printf("FAIL: %s\n", what); ++g_fail; }
}

int main() {
    sock_init();
    watchdog("mode 0 close()", [] { return run_close_case(0); });
    watchdog("mode 1 close()", [] { return run_close_case(1); });
    watchdog("close() during connect", [] { return run_connecting_case(); });
    watchdog("behaviour checks", [] { run_behaviour(); return 0; });
    if (!g_fail) std::printf("PASS\n");
    return g_fail ? 1 : 0;
}
#endif
