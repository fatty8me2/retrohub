/* Online test (LOCAL server): GameCube forwarder delay. A UDP echo stands in for the host's Dolphin NetPlay server; the "guest Dolphin"
 * sends 60 pings a second to the guest forwarder's local port and measures the round trip through guest forwarder -> session -> host
 * forwarder -> echo -> back. The test thread mimics the hub: it calls update() on a ~60 Hz UI tick only.
 * Threaded forwarder (default): median RTT must stay well under one UI frame. RETROHUB_GCFW_NOTHREAD=1 shows the old per-frame pump.
 * usage: hub_gcfw_latency [server]   ($RETROHUB_TEST_SERVER) */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "netplay_3ds_run.h"
#include "session.h"

static SOCKET udp_bind(int port) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons((u_short)port);
    if (bind(s, (sockaddr*)&a, sizeof a) != 0) { closesocket(s); return INVALID_SOCKET; }
    return s;
}
/* Wait up to ms for a datagram (select, not SO_RCVTIMEO: a timed-out recv leaves a Winsock socket in an indeterminate state). */
static int recv_wait(SOCKET s, char* b, int cap, int ms, sockaddr_in* from = nullptr) {
    fd_set fs; FD_ZERO(&fs); FD_SET(s, &fs);
    timeval tv{0, ms * 1000};
    if (select(0, &fs, nullptr, nullptr, &tv) <= 0) return 0;
    int fl = sizeof(sockaddr_in);
    return recvfrom(s, b, cap, 0, (sockaddr*)from, from ? &fl : nullptr);
}

int main(int argc, char** argv) {
    std::string server = hubnet::test_server_or_exit(argc, argv);
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    timeBeginPeriod(1);
    const int port = 27311;
    SOCKET echo = udp_bind(port);
    if (echo == INVALID_SOCKET) { printf("FAIL echo bind\n"); return 1; }
    std::atomic<bool> quit{false};
    std::atomic<int> echoed{0}, echo_err{0};
    std::thread echo_th([&] {
        char b[2048];
        while (!quit) { sockaddr_in f{}; int n = recv_wait(echo, b, sizeof b, 20, &f); if (n > 0) { sendto(echo, b, n, 0, (sockaddr*)&f, sizeof f); ++echoed; } else if (n < 0) ++echo_err; }
    });
    hubnp3ds::Plan plan; plan.port = port; plan.expected = 2;
    hubnp3ds::Runner host, guest;
    std::string err, code = hubnet::new_room_code();
    bool ok = host.start_host_forward(server, code, 1, plan, "gc", &err) && guest.start_guest(server, code, 1, plan, 1, &err, "gc");
    if (!ok) { printf("FAIL start: %s\n", err.c_str()); quit = true; echo_th.join(); return 1; }
    for (int i = 0; i < 1500 && !(host.ready() && guest.ready()); ++i) {
        host.update(); guest.update(); Sleep(16);
        if (host.failed() || guest.failed()) break;
    }
    if (!host.ready() || !guest.ready()) { printf("FAIL connect: %s / %s\n", host.status().c_str(), guest.status().c_str()); quit = true; echo_th.join(); return 1; }
    Sleep(1500);   // pings settle
    printf("connected: relayed=%d ping=%d ms\n", (int)guest.any_relayed(), guest.worst_ping_ms());

    SOCKET dol = udp_bind(0);
    sockaddr_in fw{}; fw.sin_family = AF_INET; fw.sin_addr.s_addr = htonl(INADDR_LOOPBACK); fw.sin_port = htons((u_short)guest.connect_port());
    std::vector<double> rtt;
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    std::atomic<bool> ui_quit{false};
    std::thread ui([&] { while (!ui_quit) { host.update(); guest.update(); Sleep(16); } });   // the hub's UI tick
    for (int i = 0; i < 180; ++i) {
        LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
        char msg[16]; int n = snprintf(msg, sizeof msg, "p%d", i);
        sendto(dol, msg, n, 0, (sockaddr*)&fw, sizeof fw);
        char b[64];
        for (;;) {
            int r = recv_wait(dol, b, sizeof b, 20);
            LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
            double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart;
            if (r == n && memcmp(b, msg, n) == 0) { rtt.push_back(ms); break; }
            if (ms > 300) break;   // lost
        }
        Sleep(16);
    }
    ui_quit = true; ui.join();
    quit = true; echo_th.join();
    const bool relayed = guest.any_relayed();
    printf("datagrams: guest out %llu in %llu, host out %llu in %llu; echo replied %d (recv errors %d)\n", guest.datagrams_out(), guest.datagrams_in(), host.datagrams_out(), host.datagrams_in(), echoed.load(), echo_err.load());
    host.stop(); guest.stop();
    closesocket(dol); closesocket(echo);
    timeEndPeriod(1);
    if (rtt.size() < 150) { printf("FAIL only %zu/180 echoes came back\n", rtt.size()); return 1; }
    std::sort(rtt.begin(), rtt.end());
    double med = rtt[rtt.size() / 2], p95 = rtt[rtt.size() * 95 / 100];
    const bool threaded = !std::getenv("RETROHUB_GCFW_NOTHREAD");
    printf("forwarder RTT (%s): median %.1f ms, p95 %.1f ms, n=%zu\n", threaded ? "threaded" : "UI tick", med, p95, rtt.size());
    const double lim = relayed ? 25.0 : 10.0;   // relay: + the local Worker hop
    if (threaded && med > lim) { printf("FAIL median RTT above %.0f ms on loopback\n", lim); return 1; }
    printf("PASS\n");
    return 0;
}
