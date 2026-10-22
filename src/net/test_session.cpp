/* hubnet_test: N sessions in one process against a /hub server.
 *   hubnet_test [ws://127.0.0.1:8799] [players=3] [count=1000]
 * HUBNET_FORCE_RELAY=1 disables UDP punching (tests the WebSocket relay path). */
#include "log.h"
#include "session.h"

#include <algorithm>
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
        LobbyEvent ev;
        if (b.poll_event(&ev) && ev.type == LobbyEvent::Msg) {
            got = ev.from_slot == 0 && ev.body.find("hi") != std::string::npos;
            printf("lobby: bob got from=%d body=%s\n", ev.from_slot, ev.body.c_str());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    printf("lobby check: %s (alice slot %d, bob slot %d, members %zu)\n", got ? "ok" : "FAILED", a.my_slot(), b.my_slot(), a.members().size());
    return got;
}

/* Host leaves a lobby room: the lowest remaining member becomes slot 0, nobody is dropped, the code keeps working. */
static bool migration_check(const std::string& server)
{
    std::string code = new_room_code();
    auto pump = [](std::vector<LobbyClient*> v, uint32_t ms, bool (*done)(void*), void* ctx) {
        for (uint32_t t0 = now_ms(); now_ms() - t0 < ms;) {
            for (auto* c : v) c->update();
            if (done && done(ctx)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };
    LobbyClient a, b, c, d;
    a.start(server, code, true, "alice");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    struct Ctx { LobbyClient *a, *b, *c, *d; } cx{&a, &b, &c, &d};
    b.start(server, code, false, "bob");   // join in order so bob gets slot 1 (concurrent joins race on a real server)
    pump({&a, &b}, 5000, [](void* p) { return ((Ctx*)p)->b->connected(); }, &cx);
    c.start(server, code, false, "carol");
    pump({&a, &b, &c}, 5000, [](void* p) { auto* x = (Ctx*)p; return x->c->connected() && x->a->members().size() == 3; }, &cx);
    bool ok = b.my_slot() == 1 && c.my_slot() == 2;
    a.stop();   // host leaves
    LobbyEvent hb, hc;
    bool gotb = false, gotc = false;
    for (uint32_t t0 = now_ms(); now_ms() - t0 < 20000 && !(gotb && gotc);) {
        b.update(); c.update();
        LobbyEvent h;
        while (!gotb && b.poll_event(&h)) if (h.type == LobbyEvent::HostChange) { hb = h; gotb = true; }
        while (!gotc && c.poll_event(&h)) if (h.type == LobbyEvent::HostChange) { hc = h; gotc = true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    pump({&b, &c}, 500, nullptr, nullptr);
    ok = ok && gotb && gotc && hb.from_slot == 1 && hb.name == "bob" && hc.from_slot == 1;
    ok = ok && b.my_slot() == 0 && c.my_slot() == 2 && b.connected() && c.connected() && !b.failed() && !c.failed();
    ok = ok && b.members().size() == 2 && b.members()[0].slot == 0 && b.members()[0].name == "bob" && c.members().size() == 2;
    // the code still works: a new player joins and takes the free slot; messages from the new host arrive as slot 0
    d.start(server, code, false, "dave");
    pump({&b, &c, &d}, 5000, [](void* p) { return ((Ctx*)p)->d->connected(); }, &cx);
    ok = ok && d.connected() && d.my_slot() == 1;
    b.send_msg("{\"kind\":\"select\",\"game\":\"mk64\"}");
    int from = -1; std::string body;
    for (uint32_t t0 = now_ms(); now_ms() - t0 < 5000 && from < 0;) {
        b.update(); c.update(); d.update();
        LobbyEvent ev;
        while (c.poll_event(&ev)) if (ev.type == LobbyEvent::Msg) { from = ev.from_slot; body = ev.body; }
        if (from >= 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ok = ok && from == 0 && body.find("mk64") != std::string::npos;
    printf("migration check: %s (bob slot %d, carol slot %d, dave slot %d, from=%d)\n", ok ? "ok" : "FAILED", b.my_slot(), c.my_slot(), d.my_slot(), from);
    return ok;
}

/* Events must come out in server arrival order and my_slot() must flip only when the HostChange is consumed. */
static bool event_order_check()
{
    LobbyClient c;
    c.inject_server_text(R"({"type":"welcome","room":"ABC234","slot":1})");
    c.inject_server_text(R"({"type":"msg","from":0,"body":{"kind":"launch"}})");
    c.inject_server_text(R"({"type":"msg","from":1,"body":{"kind":"ready","ready":true}})");
    c.inject_server_text(R"({"type":"host_changed","from_slot":1,"slot":0,"name":"me"})");
    c.inject_server_text(R"({"type":"msg","from":0,"body":{"kind":"select","game":"mk64"}})");
    LobbyEvent e;
    bool ok = c.my_slot() == 1;
    ok = ok && c.poll_event(&e) && e.type == LobbyEvent::Msg && e.from_slot == 0 && e.body.find("launch") != std::string::npos && c.my_slot() == 1;
    ok = ok && c.poll_event(&e) && e.type == LobbyEvent::Msg && e.from_slot == 1 && c.my_slot() == 1;
    ok = ok && c.poll_event(&e) && e.type == LobbyEvent::HostChange && e.from_slot == 1 && e.name == "me" && c.my_slot() == 0;
    ok = ok && c.poll_event(&e) && e.type == LobbyEvent::Msg && e.from_slot == 0 && e.body.find("mk64") != std::string::npos;
    ok = ok && !c.poll_event(&e);
    printf("event order check: %s\n", ok ? "ok" : "FAILED");
    return ok;
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

    if (!event_order_check()) return 1;
    if (!lobby_check(server)) return 1;
    if (!migration_check(server)) return 1;
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
    // With HUBNET_SIM_LOSS_PCT=L a client<->client datagram crosses two lossy hops: allow 100 - 2L - 4 %.
    int sim_loss = getenv("HUBNET_SIM_LOSS_PCT") ? atoi(getenv("HUBNET_SIM_LOSS_PCT")) : 0;
    int thr_pct = sim_loss > 0 ? std::max(0, 100 - 2 * sim_loss - 4) : 95;
    long total = 0, expected = 0;
    for (int r = 0; r < n; r++)
        for (int f = 0; f < n; f++) {
            if (r == f) continue;
            int c = (int)got[r][f].size();
            total += c;
            expected += count;
            printf("  receiver %d from %d: %d/%d\n", r, f, c, count);
            if (c * 100 < count * thr_pct) pass = false;
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
