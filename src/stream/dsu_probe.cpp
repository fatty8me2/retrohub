/* dsu_probe: a tiny cemuhook client (QA). Subscribes to every slot of a DSU server and prints what Cemu would see.
 * usage: dsu_probe [seconds=20] [port=26760] [logfile]
 * Lines: "t=3.1 slot=0 connected=1 b1=0x00 b2=0x40 lx=200 ly=128"  (b1/b2 = DSU button bytes, sticks 0..255), then a summary per slot. */
#include "dsu_server.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>

int main(int argc, char** argv) {
    int secs = argc > 1 ? std::atoi(argv[1]) : 20;
    int port = argc > 2 ? std::atoi(argv[2]) : dsu::kDefaultPort;
    if (argc > 3) std::freopen(argv[3], "w", stdout);
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in to{}; to.sin_family = AF_INET; to.sin_port = htons((u_short)port); inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
    DWORD tmo = 50; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof tmo);
    struct Seen { bool conn = false, ever = false, was_conn_then_off = false; unsigned long pk = 0; int b1 = -1, b2 = -1; unsigned long conn_pk = 0; };
    std::map<int, Seen> seen;
    auto t0 = std::chrono::steady_clock::now(); double last_req = -10;
    std::printf("dsu_probe: server 127.0.0.1:%d\n", port); std::fflush(stdout);
    for (;;) {
        double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (t > secs) break;
        if (t - last_req >= 1.0) {
            last_req = t;
            auto r = dsu::build_data_request(0xC11E, -1);
            sendto(s, (const char*)r.data(), (int)r.size(), 0, (sockaddr*)&to, sizeof to);
        }
        uint8_t buf[256];
        int n = recvfrom(s, (char*)buf, sizeof buf, 0, nullptr, nullptr);
        if (n <= 0) continue;
        uint32_t type; std::vector<uint8_t> pl;
        if (!dsu::parse(buf, (size_t)n, true, &type, &pl) || type != dsu::kMsgData || pl.size() < 40) continue;
        int slot = pl[0]; bool conn = pl[1] == 2 && pl[11] == 1;
        Seen& sn = seen[slot];
        sn.pk++;
        int b1 = pl[16], b2 = pl[17];
        if (conn != sn.conn || b1 != sn.b1 || b2 != sn.b2) {
            std::printf("t=%.1f slot=%d connected=%d b1=0x%02x b2=0x%02x lx=%d ly=%d\n", t, slot, conn ? 1 : 0, b1, b2, pl[20], pl[21]);
            std::fflush(stdout);
        }
        if (sn.conn && !conn) sn.was_conn_then_off = true;
        if (conn) { sn.ever = true; sn.conn_pk++; }
        sn.conn = conn; sn.b1 = b1; sn.b2 = b2;
    }
    for (auto& kv : seen)
        std::printf("summary slot=%d packets=%lu connected_packets=%lu ever_connected=%d disconnected_after=%d final_connected=%d\n", kv.first, kv.second.pk, kv.second.conn_pk,
                    kv.second.ever, kv.second.was_conn_then_off, kv.second.conn);
    std::printf("dsu_probe: done\n");
    return 0;
}
