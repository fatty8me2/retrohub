#include "nettest.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <thread>

namespace hubnt {

static bool resolve(const std::string& hp, sockaddr_in* out) {
    size_t c = hp.rfind(':');
    if (c == std::string::npos) return false;
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(hp.substr(0, c).c_str(), hp.substr(c + 1).c_str(), &hints, &res) != 0 || !res) return false;
    *out = *(sockaddr_in*)res->ai_addr;
    freeaddrinfo(res);
    return true;
}

Mapped stun_query(unsigned long long sockv, const std::string& hp, int tries, int timeout_ms) {
    SOCKET s = (SOCKET)sockv;
    Mapped none;
    sockaddr_in to;
    if (!resolve(hp, &to)) return none;
    uint8_t tid[12], req[20];
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    for (int i = 0; i < 12; ++i) tid[i] = (uint8_t)((q.QuadPart >> (i % 8 * 4)) + i * 37 + GetCurrentThreadId());
    build_request(tid, req);
    DWORD to_ms = (DWORD)timeout_ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to_ms, sizeof to_ms);
    for (int t = 0; t < tries; ++t) {
        sendto(s, (const char*)req, 20, 0, (sockaddr*)&to, sizeof to);
        uint8_t buf[512];
        sockaddr_in from; int fl = sizeof from;
        int n = recvfrom(s, (char*)buf, sizeof buf, 0, (sockaddr*)&from, &fl);
        if (n > 0) { Mapped m = parse_response(buf, n, tid); if (m.ok) return m; }
    }
    return none;
}

static int ping_lobby(std::string url, int samples) {
    // ws:// -> http://, wss:// -> https://
    if (url.rfind("wss://", 0) == 0) url = "https://" + url.substr(6);
    else if (url.rfind("ws://", 0) == 0) url = "http://" + url.substr(5);
    bool tls = url.rfind("https://", 0) == 0;
    if (!tls && url.rfind("http://", 0) != 0) return -1;
    std::string rest = url.substr(tls ? 8 : 7);
    std::string host = rest.substr(0, rest.find('/'));
    int port = tls ? 443 : 80;
    size_t c = host.find(':');
    if (c != std::string::npos) { port = std::atoi(host.c_str() + c + 1); host = host.substr(0, c); }
    std::wstring wh(host.begin(), host.end());
    HINTERNET ses = WinHttpOpen(L"RetroHub-connection-test", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!ses) return -1;
    WinHttpSetTimeouts(ses, 4000, 4000, 4000, 4000);
    int best = -1;
    HINTERNET con = WinHttpConnect(ses, wh.c_str(), (INTERNET_PORT)port, 0);
    for (int i = 0; con && i < samples; ++i) {
        HINTERNET rq = WinHttpOpenRequest(con, L"HEAD", L"/", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, tls ? WINHTTP_FLAG_SECURE : 0);
        if (!rq) break;
        auto t0 = std::chrono::steady_clock::now();
        bool ok = WinHttpSendRequest(rq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(rq, nullptr);
        int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        WinHttpCloseHandle(rq);
        if (ok && (best < 0 || ms < best)) best = ms;   // first sample includes connect + TLS; the best one is the steady round trip
    }
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return best;
}

Report run_test(const std::string& s1, const std::string& s2, const std::string& lobby) {
    Report r;
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s != INVALID_SOCKET) {
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s, (sockaddr*)&a, sizeof a) == 0) {
            r.a = stun_query(s, s1);
            r.b = stun_query(s, s2);
        }
        closesocket(s);
    }
    WSACleanup();
    r.verdict = verdict(r.a, r.b);
    r.lobby = lobby;
    if (!lobby.empty()) r.ping_ms = ping_lobby(lobby, 3);
    r.done = true;
    return r;
}

void Job::start(const std::string& lobby_url) {
    if (running_.exchange(true)) return;
    { std::lock_guard<std::mutex> g(mu_); have_ = false; }
    std::thread([this, lobby_url] {
        Report r = run_test("stun.cloudflare.com:3478", "stun.l.google.com:19302", lobby_url);
        { std::lock_guard<std::mutex> g(mu_); rep_ = r; have_ = true; }
        running_ = false;
    }).detach();
}
bool Job::has_report() { std::lock_guard<std::mutex> g(mu_); return have_; }
Report Job::report() { std::lock_guard<std::mutex> g(mu_); return rep_; }

}  // namespace hubnt
