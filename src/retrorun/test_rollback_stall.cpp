/* Console test: rollback engine under long network stalls (a 4-5 s round-trip spike, as seen on a relayed SNES 4-player run), 2 and 4 peers, on a
 * virtual clock. Rules checked: never a desync, the sim never advances more than the ring can roll back, a slow-but-alive peer is not dropped by a
 * stall (even a one-sided one), and a peer that really was dropped learns it (connection_lost) instead of playing on alone. Exit 0 = pass. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <vector>

#include "rollback.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct FakeSim {
    uint64_t x = 1, y = 7, last_sum = 0, pad_now[4] = {0, 0, 0, 0};
    uint32_t frame = 0;
    void step(const uint64_t p[4]) {
        for (int i = 0; i < 4; i++) { x = (x ^ p[i]) * 0x9E3779B97F4A7C15ull; x ^= x >> 31; y += x * (i + 3) + (y >> 5); }
        frame++;
        last_sum = x ^ (y * 31);
    }
};
struct Blob { uint64_t x, y; uint32_t frame, pad; };

static rb::Sim make_sim(FakeSim* s, int port, uint32_t seed) {
    rb::Sim sim;
    sim.state_size = [] { return sizeof(Blob); };
    sim.save = [s](uint8_t* b, size_t) { Blob k{s->x, s->y, s->frame, 0}; memcpy(b, &k, sizeof k); return true; };
    sim.load = [s](const uint8_t* b, size_t) { Blob k; memcpy(&k, b, sizeof k); s->x = k.x; s->y = k.y; s->frame = k.frame; return true; };
    sim.step = [s](const uint64_t p[4], bool) { s->step(p); };
    sim.frame_sum = [s] { return s->last_sum; };
    sim.poll_local = [s, port, seed] {
        uint32_t v = ((s->frame / 3) * 2654435761u) ^ (uint32_t)(port + 1) * 40503u ^ seed;
        v ^= v >> 13; v *= 2246822519u; v ^= v >> 15;
        s->pad_now[port] = v & 0x3FF;
    };
    sim.read_local = [s](int p) { return s->pad_now[p]; };
    return sim;
}

struct Stall { int a, b; uint32_t from, to; bool oneway; bool eat; };   // traffic a<->b (or a->b) sent in [from,to) is held until `to` (eat: lost)

struct Net {
    struct Msg { uint32_t at; int from; std::vector<uint8_t> d; };
    std::deque<Msg> q[4];
    uint32_t now = 0, rng = 777;
    int latency = 40, jitter = 20, loss = 2;
    std::vector<Stall> stalls;
    uint32_t rnd() { rng = rng * 1664525u + 1013904223u; return rng >> 8; }
    void send(int from, int to, const void* d, int n) {
        if ((int)(rnd() % 100) < loss) return;
        uint32_t at = now + latency + (jitter ? rnd() % (jitter + 1) : 0);
        for (auto& s : stalls) {
            bool m = (s.a == from && s.b == to) || (!s.oneway && s.a == to && s.b == from) || (s.b < 0 && (s.a == from || s.a == to));   // b<0: every link of a
            if (s.a < 0) m = true;   // every link
            if (m && now >= s.from && now < s.to) { if (s.eat) return; at = std::max(at, s.to + latency); }
        }
        q[to].push_back({at, from, std::vector<uint8_t>((const uint8_t*)d, (const uint8_t*)d + n)});
    }
    bool recv(int to, int* from, std::vector<uint8_t>* out) {
        for (size_t i = 0; i < q[to].size(); i++)
            if (q[to][i].at <= now) { *from = q[to][i].from; *out = q[to][i].d; q[to].erase(q[to].begin() + i); return true; }
        return false;
    }
};

struct Res { int n; uint32_t frame[4]; uint64_t x[4], y[4]; bool desync[4], lost[4]; uint32_t dropped[4]; rb::Stats st[4]; };

static Res run(int n, int frames, int max_pred, int delay, const std::vector<Stall>& stalls, bool verbose = false) {
    Net net; net.stalls = stalls;
    FakeSim sim[4];
    std::unique_ptr<rb::Engine> eng[4];
    for (int i = 0; i < n; i++) {
        rb::Config c;
        for (int p = 0; p < 4; p++) c.owner[p] = p < n ? p : -1;
        c.my_slot = i; c.slot_mask = (1u << n) - 1; c.delay = delay; c.hash_interval = 60;
        c.max_pred = max_pred; c.adaptive_delay = true; c.frame_ms = 16.0;
        rb::Transport t;
        t.pump = [] {};
        t.send = [&net, i, n](int to, const void* d, int len) { for (int s = 0; s < n; s++) if (s != i && (to < 0 || to == s)) net.send(i, s, d, len); };
        t.recv = [&net, i](int* from, std::vector<uint8_t>* o) { return net.recv(i, from, o); };
        t.now_ms = [&net] { return net.now + 1; };
        t.log = [i, verbose](const std::string& s) { if (verbose || s.find("DESYNC") != std::string::npos || s.find("drop") != std::string::npos) printf("  [peer %d] %s\n", i, s.c_str()); };
        eng[i].reset(new rb::Engine(c, make_sim(&sim[i], i, 91), t));
    }
    bool done[4] = {false, false, false, false};
    for (int round = 0; round < frames * 8; round++) {
        net.now += 16;
        bool all = true;
        for (int i = 0; i < n; i++) {
            if (eng[i]->connection_lost()) { done[i] = true; continue; }
            if (eng[i]->cur() >= (uint32_t)frames) { eng[i]->service(); continue; }
            all = false;
            if (eng[i]->prepare() == rb::Engine::Status::Ready) eng[i]->run_frame();
        }
        if (all) break;
    }
    for (int k = 0; k < 600; k++) { net.now += 16; for (int i = 0; i < n; i++) eng[i]->service(); }
    Res r{}; r.n = n;
    for (int i = 0; i < n; i++) {
        r.frame[i] = sim[i].frame; r.x[i] = sim[i].x; r.y[i] = sim[i].y; r.desync[i] = eng[i]->desync(); r.lost[i] = eng[i]->connection_lost();
        r.dropped[i] = eng[i]->dropped_mask(); r.st[i] = eng[i]->stats();
    }
    return r;
}

static void report(const char* name, const Res& r) {
    printf("%-34s", name);
    for (int i = 0; i < r.n; i++) printf(" [f%u rb%llu d%d st%llu%s%s]", r.frame[i], (unsigned long long)r.st[i].rollbacks, r.st[i].max_depth, (unsigned long long)r.st[i].stalls, r.desync[i] ? " DESYNC" : "", r.lost[i] ? " LOST" : "");
    printf("\n");
}

static void same_state(const Res& r, const char* name, uint32_t skip_mask = 0) {
    int ref = -1;
    for (int i = 0; i < r.n; i++) {
        if (skip_mask >> i & 1u) continue;
        CHECK(!r.desync[i], "%s: peer %d flagged a desync", name, i);
        if (ref < 0) { ref = i; continue; }
        CHECK(r.frame[i] == r.frame[ref] && r.x[i] == r.x[ref] && r.y[i] == r.y[ref], "%s: peer %d state differs from peer %d", name, i, ref);
    }
}

int main() {
    const uint32_t S = 4000, SPIKE = 4600;
    for (int pred : {8, 14}) {
        char nm[64];
        // 2 peers: both directions stall 4.6 s, held (latency spike) then burst; and a version where the spike packets are lost
        for (int eat = 0; eat < 2; eat++) {
            snprintf(nm, sizeof nm, "2P spike%s pred %d", eat ? " (lost)" : "", pred);
            Res r = run(2, 1500, pred, 1, {{0, 1, S, S + SPIKE, false, eat != 0}});
            report(nm, r); same_state(r, nm);
            CHECK(r.frame[0] == 1500 && r.frame[1] == 1500 && !r.lost[0] && !r.lost[1], "%s: did not finish", nm);
        }
        // 2 peers, one-way stall (A hears B, B hears nothing)
        snprintf(nm, sizeof nm, "2P one-way spike pred %d", pred);
        { Res r = run(2, 1500, pred, 1, {{0, 1, S, S + SPIKE, true, false}}); report(nm, r); same_state(r, nm); CHECK(r.frame[0] == 1500 && r.frame[1] == 1500, "%s: unfinished", nm); }
        // 4 peers: whole network spikes (every peer silent to every other)
        snprintf(nm, sizeof nm, "4P whole-net spike pred %d", pred);
        { Res r = run(4, 1500, pred, 1, {{-1, 0, S, S + SPIKE, false, false}}); report(nm, r); same_state(r, nm);
          for (int i = 0; i < 4; i++) CHECK(r.dropped[i] == 0 && !r.lost[i] && r.frame[i] == 1500, "%s: peer %d dropped/lost/unfinished", nm, i); }
        // 4 peers: one guest's links stall (slow but alive): nobody may be dropped
        snprintf(nm, sizeof nm, "4P guest-3 spike pred %d", pred);
        { Res r = run(4, 1500, pred, 1, {{3, -1, S, S + SPIKE, false, false}}); report(nm, r); same_state(r, nm);
          for (int i = 0; i < 4; i++) CHECK(r.dropped[i] == 0 && !r.lost[i] && r.frame[i] == 1500, "%s: peer %d dropped/lost/unfinished", nm, i); }
        // 4 peers: guest 2 cannot hear the others for 4.6 s (one-way)
        snprintf(nm, sizeof nm, "4P guest-2 deaf spike pred %d", pred);
        { std::vector<Stall> st; for (int s = 0; s < 4; s++) if (s != 2) st.push_back({s, 2, S, S + SPIKE, true, false});
          Res r = run(4, 1500, pred, 1, st); report(nm, r); same_state(r, nm);
          for (int i = 0; i < 4; i++) CHECK(r.dropped[i] == 0 && !r.lost[i] && r.frame[i] == 1500, "%s: peer %d dropped/lost/unfinished", nm, i); }
        // 4 peers: two spikes in a row on different guests, with jitter and loss
        snprintf(nm, sizeof nm, "4P rolling spikes pred %d", pred);
        { Res r = run(4, 2400, pred, 1, {{1, -1, 2000, 6600, false, false}, {2, -1, 8000, 12600, false, false}, {3, 0, 16000, 20000, false, true}}); report(nm, r); same_state(r, nm);
          for (int i = 0; i < 4; i++) CHECK(r.dropped[i] == 0 && !r.lost[i] && r.frame[i] == 2400, "%s: peer %d dropped/lost/unfinished", nm, i); }
    }
    // a guest that really is gone for good: survivors agree and carry on, the returning/orphaned peer learns it (no silent solo play)
    {
        Res r = run(4, 1800, 8, 1, {{3, -1, 3000, 400000, false, true}});
        report("4P guest 3 gone", r);
        same_state(r, "4P guest 3 gone", 1u << 3);
        for (int i = 0; i < 3; i++) CHECK(r.dropped[i] >> 3 & 1u, "survivor %d did not drop slot 3", i);
        CHECK(r.frame[0] == 1800, "survivors unfinished");
    }
    // a guest silent 12 s then back: if the others dropped it, it must see connection_lost, never keep playing a diverged game unnoticed
    {
        Res r = run(4, 2400, 8, 1, {{3, -1, 3000, 15000, false, false}});
        report("4P guest 3 back after 12s", r);
        same_state(r, "4P guest 3 back after 12s", 1u << 3);
        if (r.dropped[0] >> 3 & 1u) CHECK(r.lost[3] || r.frame[3] < 2400, "dropped guest kept playing alone");
    }
    printf(g_fail ? "ROLLBACK STALL TEST FAILED (%d)\n" : "ROLLBACK STALL TEST PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
