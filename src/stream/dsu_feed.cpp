/* dsu_feed: runs a DSU server on 127.0.0.1:<port> with scripted pads on slots 0-2 (wiggling stick + A/B presses) for N seconds.
 * Used to check that Cemu connects. Usage: dsu_feed [seconds=30] [port=26760] [logfile] */
#include "dsu_server.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

int main(int argc, char** argv) {
    int secs = argc > 1 ? std::atoi(argv[1]) : 30;
    int port = argc > 2 ? std::atoi(argv[2]) : dsu::kDefaultPort;
    if (argc > 3) std::freopen(argv[3], "w", stdout);
    dsu::DsuServer s;
    if (!s.start((uint16_t)port, true)) { std::printf("dsu_feed: cannot bind %d\n", port); return 1; }
    std::printf("dsu_feed: listening on 127.0.0.1:%d\n", port);
    auto t0 = std::chrono::steady_clock::now();
    int lastc = -1;
    for (;;) {
        double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (t > secs) break;
        for (int sl = 0; sl < 3; ++sl) {
            dsu::Pad p;
            p.lx = (int16_t)(((int)(t * 2 + sl) % 2) ? 20000 : -20000);
            p.buttons = (int)(t) % 2 ? dsu::kA : dsu::kB;
            s.set_pad(sl, p);
        }
        if (s.clients() != lastc) { lastc = s.clients(); std::printf("dsu_feed: t=%.1f clients=%d packets=%llu\n", t, lastc, s.packets_sent()); std::fflush(stdout); }
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    std::printf("dsu_feed: done, packets=%llu\n", s.packets_sent());
    return 0;
}
