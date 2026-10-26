#include "dsu_server.h"
#include "pad_convert.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
using namespace dsu;

static float getf(const uint8_t* p) { float f; std::memcpy(&f, p, 4); return f; }

int main() {
    const uint8_t nine[] = {'1','2','3','4','5','6','7','8','9'};
    CHECK(crc32(nine, 9) == 0xCBF43926u);

    {   // guest pad -> DSU pad (Wii U A = east = DSU B bit)
        rplay::PadState g{}; g.buttons = rplay::kPadA | rplay::kPadZR | rplay::kPadPlus; g.buttons2 = rplay::kPad2StickR; g.lx = 100; g.accel[2] = 9.80665f;
        Pad d = rplay::to_dsu(g);
        CHECK((d.buttons & kB) && !(d.buttons & kA) && (d.buttons & kStart) && (d.buttons & kR3) && d.rt == 255 && d.lt == 0 && d.lx == 100);
        CHECK(d.accel[2] > 0.99f && d.accel[2] < 1.01f);
        CHECK(rplay::dsu_slot_for_guest(1) == 0 && rplay::dsu_slot_for_guest(3) == 2 && rplay::dsu_slot_for_guest(0) == -1 && rplay::dsu_slot_for_guest(4) == -1);
    }
    auto v = build_version(7);
    uint32_t type = 0, sid = 0; std::vector<uint8_t> pl;
    CHECK(v.size() == 22 && std::memcmp(v.data(), "DSUS", 4) == 0);
    CHECK(parse(v.data(), v.size(), true, &type, &pl, &sid) && type == kMsgVersion && sid == 7 && pl.size() == 2 && pl[0] == (1001 & 0xFF) && pl[1] == (1001 >> 8));
    auto bad = v; bad[20] ^= 1;
    CHECK(!parse(bad.data(), bad.size(), true, &type, &pl));
    CHECK(!parse(v.data(), v.size(), false, &type, &pl));   // wrong magic for a client packet
    CHECK(!parse(v.data(), 10, true, &type, &pl));

    auto inf = build_info(1, 2, true);
    CHECK(inf.size() == kInfoPacketSize);
    CHECK(parse(inf.data(), inf.size(), true, &type, &pl) && type == kMsgInfo && pl.size() == 12 && pl[0] == 2 && pl[1] == 2 && pl[2] == 2 && pl[3] == 2 && pl[10] == 5);
    auto off = build_info(1, 3, false);
    parse(off.data(), off.size(), true, &type, &pl);
    CHECK(pl[0] == 3 && pl[1] == 0);

    Pad p;
    p.buttons = kA | kUp | kStart | kLB;
    p.lx = 32767; p.ly = -32768; p.rx = 0; p.ry = 0;
    p.lt = 200; p.rt = 10;
    p.gyro[0] = 1.5f; p.gyro[1] = -2.f; p.gyro[2] = 3.f;
    p.accel[0] = 0.1f; p.accel[1] = 0.2f; p.accel[2] = 0.97f;
    auto d = build_data(9, 1, true, 42, 123456789ull, p);
    CHECK(d.size() == kDataPacketSize);
    CHECK(parse(d.data(), d.size(), true, &type, &pl) && type == kMsgData && pl.size() == 80);
    CHECK(pl[0] == 1 && pl[1] == 2 && pl[11] == 1);   // slot, connected, connected flag
    CHECK(pl[12] == 42 && pl[13] == 0);                // packet number
    CHECK(pl[16] == (0x10 | 0x08));                    // dpad up + options
    CHECK(pl[17] == (0x20 | 0x04 | 0x01));             // A + L1 + L2 (lt>127)
    CHECK(pl[20] == 255 && pl[21] == 0 && pl[22] == 128 && pl[23] == 128);   // sticks
    CHECK(pl[27] == 255 && pl[28 + 2] == 255);          // analog up, analog A
    CHECK(pl[35] == 200 && pl[34] == 10);               // analog L2 / R2
    CHECK(pl[48] == 0x15 && pl[49] == 0xCD && pl[50] == 0x5B && pl[51] == 0x07);   // timestamp 123456789 us
    CHECK(getf(&pl[56]) == 0.1f && getf(&pl[64]) == 0.97f && getf(&pl[68]) == 1.5f && getf(&pl[76]) == 3.f);

    auto ir = build_info_request(5, {0, 1});
    CHECK(parse(ir.data(), ir.size(), false, &type, &pl) && type == kMsgInfo && pl.size() == 6);
    auto dr = build_data_request(5, -1);
    CHECK(parse(dr.data(), dr.size(), false, &type, &pl) && type == kMsgData && pl.size() == 8 && pl[0] == 0);

    /* Loopback round trip against the real server (ephemeral port). */
    DsuServer srv;
    CHECK(srv.start(0, true) && srv.port() != 0);
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    SOCKET c = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    DWORD to = 1000; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof to);
    sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_port = htons(srv.port()); sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    auto tx = [&](const std::vector<uint8_t>& b) { sendto(c, (const char*)b.data(), (int)b.size(), 0, (sockaddr*)&sa, sizeof sa); };
    auto rx = [&](std::vector<uint8_t>* out) { uint8_t buf[512]; int n = recv(c, (char*)buf, sizeof buf, 0); if (n <= 0) return false; out->assign(buf, buf + n); return true; };
    std::vector<uint8_t> got;
    tx(build_version(1) /* wrong magic: ignored */);
    tx(wrap(1, {0, 0, 0x10, 0}, false));   // version request (type 0x100000)
    CHECK(rx(&got) && parse(got.data(), got.size(), true, &type, &pl) && type == kMsgVersion);
    srv.set_pad(1, p);
    tx(build_info_request(1, {1, 3}));
    CHECK(rx(&got) && parse(got.data(), got.size(), true, &type, &pl) && type == kMsgInfo && pl[0] == 1 && pl[1] == 2);
    CHECK(rx(&got) && parse(got.data(), got.size(), true, &type, &pl) && type == kMsgInfo && pl[0] == 3 && pl[1] == 0);
    tx(build_data_request(1, 1));
    bool data = false;
    for (int i = 0; i < 5 && !data; ++i) data = rx(&got) && parse(got.data(), got.size(), true, &type, &pl) && type == kMsgData && pl[0] == 1;
    CHECK(data && got.size() == kDataPacketSize);
    CHECK(srv.clients() == 1);
    Pad q = p; q.buttons = kB;
    srv.set_pad(1, q);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    bool upd = false;
    for (int i = 0; i < 600 && !upd; ++i) { if (!rx(&got)) break; if (parse(got.data(), got.size(), true, &type, &pl) && pl.size() > 17 && (pl[17] & 0x60) == 0x40) upd = true; }
    CHECK(upd);
    closesocket(c);
    srv.stop();
    std::printf(fails ? "dsu_test: %d FAILED\n" : "dsu_test: all passed\n", fails);
    return fails ? 1 : 0;
}
