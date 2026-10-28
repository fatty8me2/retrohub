// Console test for spectate.h: a spectator follows a host's confirmed-input stream over a lossy, reordering link,
// joins late, catches up, detects a hash mismatch, and learns the end of the session.
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>
#include <vector>

#include "spectate.h"

static int g_fail = 0;
#define CHECK(c, msg) do { if (!(c)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); g_fail++; } } while (0)

struct Link {
    struct Pkt { uint32_t due; std::vector<uint8_t> d; };
    std::deque<Pkt> q;
    std::mt19937 rng{7};
    int loss_pct = 0, lat = 20, jitter = 0;
    void send(uint32_t now, const uint8_t* d, int n) {
        if (loss_pct && (int)(rng() % 100) < loss_pct) return;
        q.push_back({now + lat + (jitter ? (uint32_t)(rng() % (jitter + 1)) : 0u), std::vector<uint8_t>(d, d + n)});
    }
    bool pop(uint32_t now, std::vector<uint8_t>* out) {
        for (size_t i = 0; i < q.size(); i++)
            if ((int32_t)(now - q[i].due) >= 0) { *out = q[i].d; q.erase(q.begin() + i); return true; }
        return false;
    }
};

static uint64_t val(uint32_t s, int p) { return (uint64_t)(s * 2654435761u) ^ ((uint64_t)p << 40) ^ (p == 1 ? 0xABCDEF0000000000ull : 0); }

struct Sim {
    spectate::Host host;
    spectate::Client cli;
    Link down, up;
    uint32_t now = 1000;
    uint32_t consumed = 0;
    uint64_t sum_host = 0, sum_cli = 0;
    bool wide;
    Sim(uint8_t mask, bool w) : host(mask, w ? 8 : 4), wide(w) {}
    void push_one() {
        uint64_t v[4];
        for (int p = 0; p < 4; p++) v[p] = wide ? val(host.total(), p) : (uint32_t)val(host.total(), p);
        host.push(v);
        for (int p = 0; p < 4; p++) if (0xB >> p & 1) sum_host = sum_host * 31 + v[p];
        if (host.total() % 600 == 0) host.add_hash(host.total(), 0x1000 + host.total());
    }
    void tick(bool cli_consumes, int per_tick) {
        now++;
        host.service(now, 1u << 2, [&](int, const uint8_t* d, int n) { down.send(now, d, n); });
        std::vector<uint8_t> m;
        while (down.pop(now, &m)) cli.on_data(m.data(), m.size(), now);
        for (int i = 0; cli_consumes && i < per_tick && cli.ready(consumed); i++) {
            for (int p = 0; p < 4; p++) if (0xB >> p & 1) sum_cli = sum_cli * 31 + cli.get(consumed, p);
            if ((consumed + 1) % 600 == 0) cli.add_local_hash(consumed + 1, 0x1000 + consumed + 1);
            consumed++;
            cli.consumed(consumed);
        }
        cli.service(now, 2, [&](int, const uint8_t* d, int n) { up.send(now, d, n); });
        while (up.pop(now, &m)) host.on_ack(2, m.data(), m.size(), now);
    }
};

static void run(bool wide, int loss, int jitter, const char* name) {
    Sim s(0xB, wide);
    s.down.loss_pct = s.up.loss_pct = loss;
    s.down.jitter = s.up.jitter = jitter;
    // 3000 samples of history exist before the spectator is even connected (late join), then the live game continues
    for (int i = 0; i < 3000; i++) s.push_one();
    for (int t = 0; t < 400000 && s.consumed < 6000; t++) {
        if (t % 16 == 0 && s.host.total() < 6000) s.push_one();   // live: ~1 sample per 16 ms
        s.tick(true, 8);                                           // spectator may run up to 8 samples per ms while catching up
    }
    char b[96];
    std::snprintf(b, sizeof b, "%s: spectator consumed all 6000 samples", name);
    CHECK(s.consumed == 6000, b);
    std::snprintf(b, sizeof b, "%s: identical input sums", name);
    CHECK(s.sum_host == s.sum_cli, b);
    std::snprintf(b, sizeof b, "%s: hashes compared and equal", name);
    CHECK(s.cli.hashes_ok() >= 8 && s.cli.hashes_bad() == 0, b);
    for (int t = 0; t < 3000; t++) s.tick(true, 8);
    std::snprintf(b, sizeof b, "%s: host saw the spectator's hashes ok", name);
    CHECK(s.host.hashes_ok() >= 8 && s.host.hashes_bad() == 0, b);
    s.host.finish();
    for (int t = 0; t < 3000; t++) s.tick(true, 8);
    std::snprintf(b, sizeof b, "%s: fin learned, all acked", name);
    CHECK(s.cli.finished(s.consumed) && s.host.all_acked(1u << 2), b);
    std::printf("%s: ok (packets %u)\n", name, s.host.packets_sent());
}

int main() {
    run(false, 0, 0, "clean");
    run(false, 20, 30, "20% loss, jitter");
    run(true, 35, 60, "wide 35% loss, heavy jitter");

    {   // flow control: a stalled spectator never gets more than kWindow samples ahead of what it consumed
        Sim s(0xB, false);
        for (int i = 0; i < 5000; i++) s.push_one();
        for (int t = 0; t < 4000; t++) s.tick(false, 0);
        CHECK(s.cli.total() == 5000, "total known");
        CHECK(s.host.acked_of(2) <= (int64_t)spectate::kWindow + 1, "window bounds the stream");
        for (int t = 0; t < 4000; t++) s.tick(true, 2);
        CHECK(s.consumed == 5000, "resumes after the stall");
    }
    {   // hash mismatch is detected on both ends
        Sim s(0x3, false);
        for (int i = 0; i < 1300; i++) s.push_one();
        for (int t = 0; t < 4000; t++) {
            s.now++;
            s.host.service(s.now, 1u << 1, [&](int, const uint8_t* d, int n) { s.down.send(s.now, d, n); });
            std::vector<uint8_t> m;
            while (s.down.pop(s.now, &m)) s.cli.on_data(m.data(), m.size(), s.now);
            for (int i = 0; i < 8 && s.cli.ready(s.consumed); i++) {
                if ((s.consumed + 1) % 600 == 0) s.cli.add_local_hash(s.consumed + 1, 0xDEAD);   // wrong
                s.consumed++; s.cli.consumed(s.consumed);
            }
            s.cli.service(s.now, 1, [&](int, const uint8_t* d, int n) { s.up.send(s.now, d, n); });
            while (s.up.pop(s.now, &m)) s.host.on_ack(1, m.data(), m.size(), s.now);
        }
        uint32_t sm; uint64_t a, b;
        CHECK(s.cli.hashes_bad() >= 1 && s.cli.poll_mismatch(&sm, &a, &b) && a == 0xDEAD, "spectator sees the mismatch");
        CHECK(s.host.hashes_bad() >= 1, "host sees the mismatch");
    }
    {   // garbage never crashes or advances
        spectate::Client c;
        uint8_t junk[40] = {spectate::kPktData, 0, 0, 0, 0, 0, 200, 0xF, 4};
        CHECK(!c.on_data(junk, sizeof junk, 5), "truncated data rejected");
        CHECK(!c.on_data(junk, 3, 5), "short rejected");
        spectate::Host h(0x1, 4);
        uint8_t ack[5] = {spectate::kPktAck, 1, 0, 0, 0};
        CHECK(!h.on_ack(1, ack, sizeof ack, 5), "short ack rejected");
    }
    if (g_fail) { std::printf("%d FAILED\n", g_fail); return 1; }
    std::printf("spectate test: all ok\n");
    return 0;
}
