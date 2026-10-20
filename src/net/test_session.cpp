/* hubnet_test: N sessions in one process against a /hub server.
 *   hubnet_test [ws://127.0.0.1:8799] [players=3] [count=1000]
 * HUBNET_FORCE_RELAY=1 disables UDP punching (tests the WebSocket relay path). */
#include "log.h"
#include "session.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <thread>

using namespace hubnet;

static bool lobby_check(const std::string& server)
{
    std::string code = new_room_code();
    LobbyClient a, b;
    a.start(server, code, true, "alice");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    b.start(server, code, false, "bob");
    bool sent = false, got = false;
    for (uint32_t t0 = now_ms(); now_ms() - t0 < 5000 && !got;) {
        a.update(); b.update();
        if (!sent && a.connected() && b.connected() && a.members().size() == 2) {
            a.send_msg("{\"kind\":\"chat\",\"text\":\"hi \\\"there\\\"\"}");
            sent = true;
        }
        int from; std::string body;
        if (b.poll_msg(&from, &body)) {
            got = from == 0 && body.find("hi") != std::string::npos;
            printf("lobby: bob got from=%d body=%s\n", from, body.c_str());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    printf("lobby check: %s (alice slot %d, bob slot %d, members %zu)\n", got ? "ok" : "FAILED", a.my_slot(), b.my_slot(), a.members().size());
    return got;
}

int main(int argc, char** argv)
{
    std::string server = argc > 1 ? argv[1] : "ws://127.0.0.1:8799";
    int n = argc > 2 ? atoi(argv[2]) : 3;
    int count = argc > 3 ? atoi(argv[3]) : 1000;
    if (n < 2 || n > kMaxSlots) n = 3;
    const char* fr = getenv("HUBNET_FORCE_RELAY");
    bool relay = fr && fr[0] && fr[0] != '0';
    net_set_log_file("hubnet_test.log");

    if (!lobby_check(server)) return 1;
    std::string room = new_room_code() + "-1";
    printf("server=%s room=%s players=%d count=%d mode=%s\n", server.c_str(), room.c_str(), n, count,
           relay ? "FORCED RELAY" : "direct UDP (relay fallback)");

    std::vector<std::unique_ptr<Session>> s;
    for (int i = 0; i < n; i++) {
        s.emplace_back(new Session());
        SessionConfig c;
        c.server = server;
        c.room = room;
        c.slot = i;
        c.expected = n;
        if (!s[i]->start(c)) { printf("FAIL: session %d start\n", i); return 1; }
        // host must be in the room before clients try (clients retry anyway, but be deterministic)
        if (i == 0) {
            uint32_t t0 = now_ms();
            while (now_ms() - t0 < 300) { s[0]->update(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        }
    }

    auto pump = [&]() { for (auto& x : s) x->update(); };
    uint32_t t0 = now_ms();
    for (;;) {
        pump();
        bool all = true, any_failed = false;
        for (auto& x : s) { all = all && x->ready(); any_failed = any_failed || x->failed(); }
        if (all) break;
        if (any_failed || now_ms() - t0 > 25000) {
            for (int i = 0; i < n; i++) printf("  slot %d: ready=%d failed=%d status=%s\n", i, s[i]->ready(), s[i]->failed(), s[i]->status().c_str());
            printf("FAIL: not ready\n");
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    printf("all ready after %u ms\n", now_ms() - t0);
    for (uint32_t w = now_ms(); now_ms() - w < 800;) { pump(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            if (i != j && (i == 0 || j == 0)) printf("  %d->%d relayed=%d ping=%dms\n", i, j, s[i]->is_relayed(j), s[i]->ping_ms(j));

    // received[receiver][sender] = set of seq numbers
    std::vector<std::vector<std::set<uint32_t>>> got(n, std::vector<std::set<uint32_t>>(n));
    int bad = 0;
    auto drain = [&]() {
        for (int i = 0; i < n; i++) {
            int from;
            std::vector<uint8_t> d;
            while (s[i]->recv(&from, &d)) {
                if (d.size() != 8 || from < 0 || from >= n || d[4] != from || d[5] != 0xAB) { bad++; continue; }
                uint32_t seq;
                memcpy(&seq, d.data(), 4);
                got[i][from].insert(seq);
            }
        }
    };

    for (int seq = 0; seq < count; seq++) {
        for (int i = 0; i < n; i++) {
            uint8_t d[8] = {};
            memcpy(d, &seq, 4);
            d[4] = (uint8_t)i;
            d[5] = 0xAB;
            s[i]->send(-1, d, sizeof(d));
        }
        pump();
        drain();
        // Relay mode: pace to ~50 datagrams/s per sender. `wrangler dev` drops WebSocket frames
        // when pushed above a few hundred frames/s in total (observed with plain node clients too).
        if (relay) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        else if (seq % 4 == 3) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    // drain until everything arrived or nothing new has shown up for 2 s (max 20 s)
    uint32_t t1 = now_ms(), last_new = t1;
    auto total_got = [&]() { size_t t = 0; for (auto& a : got) for (auto& b : a) t += b.size(); return t; };
    size_t prev = total_got();
    while (now_ms() - t1 < 20000 && now_ms() - last_new < 2000) {
        pump(); drain();
        size_t cur = total_got();
        if (cur != prev) { prev = cur; last_new = now_ms(); }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // directed send: client 1 -> client 2 (via host forwarding) one datagram
    bool pass = bad == 0;
    long total = 0, expected = 0;
    for (int r = 0; r < n; r++)
        for (int f = 0; f < n; f++) {
            if (r == f) continue;
            int c = (int)got[r][f].size();
            total += c;
            expected += count;
            printf("  receiver %d from %d: %d/%d\n", r, f, c, count);
            if (c * 100 < count * 95) pass = false;
            if (c < count && getenv("HUBNET_VERBOSE")) {
                printf("    missing:");
                int shown = 0;
                for (int q = 0; q < count && shown < 40; q++)
                    if (!got[r][f].count((uint32_t)q)) { printf(" %d", q); shown++; }
                printf("\n");
            }
        }
    printf("delivered %ld/%ld (%.1f%%), malformed=%d, failed sessions: ", total, expected, 100.0 * total / expected, bad);
    for (auto& x : s) if (x->failed()) { pass = false; printf("[%s] ", x->status().c_str()); }
    printf("\n%s\n", pass ? "PASS" : "FAIL");
    for (auto& x : s) x->stop();
    return pass ? 0 : 1;
}
