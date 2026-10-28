/* Dolphin speed over DSU: pure logic + a loopback round trip (offline, no Dolphin needed). */
#include "dolphin_speed_logic.h"
#include "dolphin_speed.h"
#include "dsu_server.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <thread>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using hubqm::DolphinKey;

int main() {
    /* hotkey / ini text */
    CHECK(hubds::server_entries(26761) == "RHSpeed:127.0.0.1:26761;");
    CHECK(hubds::hotkey_expr(DolphinKey::Increase) == "`DSUClient/0/RHSpeed:Options` | @(Ctrl+Shift+F13)");
    CHECK(hubds::hotkey_expr(DolphinKey::Decrease) == "`DSUClient/0/RHSpeed:Share` | @(Ctrl+Shift+F14)");
    CHECK(hubds::kPort != dsu::kDefaultPort);
    CHECK(hubds::key_from_scan(0x64) == DolphinKey::Increase && hubds::key_from_scan(0x65) == DolphinKey::Decrease);
    CHECK(hubds::key_from_scan(hubqm::dolphin_scan(hubqm::dolphin_vk(DolphinKey::Decrease))) == DolphinKey::Decrease);

    /* sequencing: alternating press / release, ends released */
    auto st = hubds::press_steps({DolphinKey::Increase, DolphinKey::Decrease, DolphinKey::Decrease}, 30, 20);
    CHECK(st.size() == 6);
    CHECK(st[0].buttons == hubds::kBtnStart && st[0].ms == 30 && st[1].buttons == 0 && st[1].ms == 20);
    CHECK(st[2].buttons == hubds::kBtnBack && st[4].buttons == hubds::kBtnBack && st[5].buttons == 0);
    CHECK(hubds::press_steps({}).empty());
    CHECK(hubds::burst_duration_ms(90) == 7200);

    /* guest-timebase speed (the live test's readback) */
    CHECK(std::fabs(hubds::tb_speed_percent(1000, 1000 + 40500000u, 1.0) - 100.0) < 1e-6);
    CHECK(std::fabs(hubds::tb_speed_percent(0xFFFFFF00u, 0xFFFFFF00u + 20250000u, 1.0) - 50.0) < 1e-6);   /* across the 32-bit wrap */
    CHECK(std::fabs(hubds::tb_speed_percent(0, 81000000u, 2.0) - 100.0) < 1e-6);
    CHECK(hubds::tb_speed_percent(0, 1, 0) < 0);
    CHECK(hubds::speed_matches(5, 50) && hubds::speed_matches(5, 55) && !hubds::speed_matches(5, 100) && !hubds::speed_matches(5, 20));
    CHECK(hubds::speed_matches(1, 12) && !hubds::speed_matches(1, 30));

    /* loopback: a DSU client registers, 3 increase presses come out as exactly 3 rising edges of Options and none of Share */
    CHECK(!dolphin_speed_running());
    CHECK(dolphin_speed_start() && dolphin_speed_running() && dolphin_speed_start());
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)hubds::kPort); inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    DWORD to = 20; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof to);
    auto reg = dsu::build_data_request(0xC11E, hubds::kSlot);
    sendto(s, (const char*)reg.data(), (int)reg.size(), 0, (sockaddr*)&a, sizeof a);

    std::atomic<int> done{-99};
    std::vector<int> keys = {0x64, 0x64, 0x64};
    std::thread th;
    bool started = false;
    int rises_opt = 0, rises_share = 0, frames = 0;
    bool prev_opt = false, prev_share = false;
    auto t0 = std::chrono::steady_clock::now();
    auto last_reg = t0;
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(6)) {
        auto now = std::chrono::steady_clock::now();
        if (now - last_reg > std::chrono::milliseconds(500)) { sendto(s, (const char*)reg.data(), (int)reg.size(), 0, (sockaddr*)&a, sizeof a); last_reg = now; }
        if (!started && dolphin_speed_connected()) { started = true; dolphin_speed_send(keys, [&](int d) { done = d; }); }
        uint8_t buf[256];
        int n = recvfrom(s, (char*)buf, sizeof buf, 0, nullptr, nullptr);
        if (n > 0) {
            uint32_t type; std::vector<uint8_t> pl;
            if (dsu::parse(buf, (size_t)n, true, &type, &pl) && type == dsu::kMsgData && pl.size() > 17) {
                ++frames;
                bool opt = pl[16] & 0x08, shr = pl[16] & 0x01;
                if (opt && !prev_opt) ++rises_opt;
                if (shr && !prev_share) ++rises_share;
                prev_opt = opt; prev_share = shr;
            }
        }
        if (done != -99) break;
    }
    Sleep(100);
    CHECK(started);
    CHECK(done == 3);
    CHECK(rises_opt == 3);
    CHECK(rises_share == 0);
    CHECK(frames > 20);
    std::printf("loopback: done=%d options_edges=%d share_edges=%d frames=%d\n", done.load(), rises_opt, rises_share, frames);

    /* with nothing registered a burst reports -1 only after the wait; with the server stopped it reports -1 immediately */
    closesocket(s);
    dolphin_speed_stop();
    CHECK(!dolphin_speed_running());
    std::atomic<int> d2{-99};
    dolphin_speed_send({0x64}, [&](int d) { d2 = d; });
    CHECK(d2 == -1);

    std::printf(g_fail ? "FAILED (%d)\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
