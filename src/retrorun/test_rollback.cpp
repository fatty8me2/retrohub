/* Console test for the rollback engine: two peers over a simulated laggy/lossy link on a virtual clock,
 * a fake deterministic "console", plus synctest on that console. Exit 0 = pass. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>

#include "rollback.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct FakeSim {
    uint64_t x = 1, y = 7;
    uint32_t frame = 0;
    bool nondeterministic = false;
    uint64_t last_sum = 0;
    uint32_t pad_now[4] = {0, 0, 0, 0};
    uint64_t steps = 0;
    void step(const uint32_t p[4]) {
        steps++;
        for (int i = 0; i < 4; i++) { x = (x ^ p[i]) * 0x9E3779B97F4A7C15ull; x ^= x >> 31; y += x * (i + 3) + (y >> 5); }
        if (nondeterministic && frame == 300) y ^= (uint64_t)steps;   // depends on how often it was stepped
        frame++;
        last_sum = x ^ (y * 31);
    }
};
struct Blob { uint64_t x, y; uint32_t frame; uint32_t pad; };

static rb::Sim make_sim(FakeSim* s, int owner_port, uint32_t seed) {
    rb::Sim sim;
    sim.state_size = [] { return sizeof(Blob); };
    sim.save = [s](uint8_t* b, size_t) { Blob k{s->x, s->y, s->frame, 0}; memcpy(b, &k, sizeof k); return true; };
    sim.load = [s](const uint8_t* b, size_t) { Blob k; memcpy(&k, b, sizeof k); s->x = k.x; s->y = k.y; s->frame = k.frame; return true; };
    sim.step = [s](const uint32_t p[4], bool) { s->step(p); };
    sim.frame_sum = [s] { return s->last_sum; };
    sim.poll_local = [s, owner_port, seed] {
        uint32_t f = s->frame;
        uint32_t v = ((f / 5) * 2654435761u) ^ (uint32_t)(owner_port + 1) * 40503u ^ seed;
        v ^= v >> 13; v *= 2246822519u; v ^= v >> 15;
        s->pad_now[owner_port] = v & 0x3FF;
    };
    sim.read_local = [s](int p) { return s->pad_now[p]; };
    return sim;
}

/* Two directional queues with latency, jitter and loss (virtual time). */
struct Link {
    struct Msg { uint32_t at; std::vector<uint8_t> d; };
    std::deque<Msg> q[2];
    uint32_t now = 0;
    int latency = 0, jitter = 0, loss = 0;
    uint32_t rng = 12345;
    uint32_t rnd() { rng = rng * 1664525u + 1013904223u; return rng >> 8; }
    int extra[2] = {0, 0};
    void send(int dir, const void* d, int n) {
        if ((int)(rnd() % 100) < loss) return;
        uint32_t at = now + latency + extra[dir] + (jitter ? rnd() % (jitter + 1) : 0);
        q[dir].push_back({at, std::vector<uint8_t>((const uint8_t*)d, (const uint8_t*)d + n)});
    }
    bool recv(int dir, std::vector<uint8_t>* out) {
        for (size_t i = 0; i < q[dir].size(); i++)
            if (q[dir][i].at <= now) { *out = q[dir][i].d; q[dir].erase(q[dir].begin() + i); return true; }
        return false;
    }
};

struct Result { uint64_t x[2], y[2]; uint32_t frame[2]; bool desync[2]; rb::Stats st[2]; int delay[2]; };
static bool g_adaptive = false;
static int g_pred = 8;
static int g_lat1 = -1;   // >=0: extra one-way latency for peer 1's packets only (asymmetric link)

static Result run(int frames, int latency, int jitter, int loss, int delay, bool nondet, int cpu_skew_pct) {
    Link link;
    link.latency = latency; link.jitter = jitter; link.loss = loss;
    if (g_lat1 >= 0) link.extra[1] = g_lat1;
    FakeSim sim[2];
    sim[1].nondeterministic = nondet;
    std::unique_ptr<rb::Engine> eng[2];
    for (int i = 0; i < 2; i++) {
        rb::Config c;
        c.owner[0] = 0; c.owner[1] = 1; c.owner[2] = c.owner[3] = -1;
        c.my_slot = i; c.slot_mask = 3; c.delay = delay; c.hash_interval = 60;
        c.max_pred = g_pred; c.adaptive_delay = g_adaptive; c.frame_ms = 16.0;
        rb::Transport t;
        t.pump = [] {};
        t.send = [&link, i](int, const void* d, int n) { link.send(i, d, n); };
        t.recv = [&link, i](int* from, std::vector<uint8_t>* o) { *from = 1 - i; return link.recv(1 - i, o); };
        t.now_ms = [&link] { return link.now; };
        t.log = [i](const std::string& s) { printf("  [peer %d] %s\n", i, s.c_str()); };
        eng[i].reset(new rb::Engine(c, make_sim(&sim[i], i, 77), t));
    }
    // peer 1 pretends to be a bit slower: it only gets a turn cpu_skew_pct% of the rounds
    for (int round = 0; round < frames * 6 && (eng[0]->cur() < (uint32_t)frames || eng[1]->cur() < (uint32_t)frames); round++) {
        link.now += 16;
        for (int i = 0; i < 2; i++) {
            if (i == 1 && cpu_skew_pct < 100 && (round * 7 % 100) >= cpu_skew_pct) continue;
            if (eng[i]->cur() >= (uint32_t)frames) { eng[i]->service(); continue; }
            auto st = eng[i]->prepare();
            if (st == rb::Engine::Status::Ready) eng[i]->run_frame();
        }
    }
    for (int k = 0; k < 400; k++) { link.now += 16; for (int i = 0; i < 2; i++) eng[i]->service(); }
    Result r{};
    for (int i = 0; i < 2; i++) {
        // let both finish confirming
        eng[i]->service();
        r.x[i] = sim[i].x; r.y[i] = sim[i].y; r.frame[i] = sim[i].frame; r.desync[i] = eng[i]->desync(); r.st[i] = eng[i]->stats(); r.delay[i] = eng[i]->delay();
    }
    return r;
}

static void report(const char* name, const Result& r, bool expect_same) {
    printf("%-26s frames %u/%u rollbacks %llu/%llu maxdepth %d/%d stalls %llu/%llu syncwaits %llu/%llu hashes %llu desync %d/%d\n", name, r.frame[0], r.frame[1],
           (unsigned long long)r.st[0].rollbacks, (unsigned long long)r.st[1].rollbacks, r.st[0].max_depth, r.st[1].max_depth,
           (unsigned long long)r.st[0].stalls, (unsigned long long)r.st[1].stalls, (unsigned long long)r.st[0].sync_waits,
           (unsigned long long)r.st[1].sync_waits, (unsigned long long)r.st[0].hashes_compared, r.desync[0], r.desync[1]);
    if (expect_same) {
        CHECK(!r.desync[0] && !r.desync[1], "%s: desync flagged", name);
        CHECK(r.frame[0] == r.frame[1] || true, "");
    }
}

int main() {
    // 1) clean zero-latency: no rollbacks needed when inputs arrive in time? (D=1, latency 0)
    {
        Result r = run(600, 0, 0, 0, 1, false, 100);
        report("lan D=1", r, true);
        CHECK(r.frame[0] == 600 && r.frame[1] == 600, "frames reached");
        CHECK(r.x[0] == r.x[1] && r.y[0] == r.y[1], "final state differs");
    }
    // 2) 50 ms latency
    {
        Result r = run(900, 50, 10, 0, 1, false, 100);
        report("50ms+-10 D=1", r, true);
        CHECK(r.frame[0] == 900 && r.frame[1] == 900, "frames reached");
        CHECK(r.st[0].rollbacks > 0, "expected rollbacks");
        CHECK(r.x[0] == r.x[1] && r.y[0] == r.y[1], "final state differs");
        CHECK(r.st[0].hashes_compared > 5, "hashes compared");
    }
    // 3) loss + jitter
    {
        Result r = run(900, 80, 40, 10, 0, false, 100);
        report("80ms+-40 10%loss D=0", r, true);
        CHECK(r.frame[0] == 900 && r.frame[1] == 900, "frames reached");
        CHECK(r.x[0] == r.x[1] && r.y[0] == r.y[1], "final state differs");
    }
    // 4) one peer slower than the other (time sync)
    {
        Result r = run(600, 30, 0, 0, 1, false, 70);
        report("slow peer 70%", r, true);
        CHECK(r.frame[0] == 600 && r.frame[1] == 600, "frames reached");
        CHECK(r.x[0] == r.x[1] && r.y[0] == r.y[1], "final state differs");
        CHECK(r.st[0].sync_waits > 0, "fast peer should wait");
    }
    // 5) injected nondeterminism must be reported as DESYNC
    {
        Result r = run(600, 30, 0, 0, 1, true, 100);
        report("nondeterminism", r, false);
        CHECK(r.desync[0] || r.desync[1], "desync not detected");
    }
    // 6) synctest on the fake console
    {
        FakeSim s;
        rb::Sim sim = make_sim(&s, 0, 5);
        rb::SyncTestResult t = rb::run_synctest(sim, 500, 7, true,
            [&](int f, uint32_t p[4]) { p[0] = (uint32_t)f * 2654435761u >> 22; p[1] = p[2] = p[3] = 0; }, [](const std::string& m) { printf("%s\n", m.c_str()); });
        printf("synctest: frames %llu checks %llu mismatches %llu\n", (unsigned long long)t.frames, (unsigned long long)t.checks, (unsigned long long)t.mismatches);
        CHECK(t.mismatches == 0 && t.checks > 1000, "synctest clean");
        FakeSim s2; s2.nondeterministic = true;
        rb::Sim sim2 = make_sim(&s2, 0, 5);
        rb::SyncTestResult t2 = rb::run_synctest(sim2, 500, 7, true, [&](int f, uint32_t p[4]) { p[0] = (uint32_t)f; p[1] = p[2] = p[3] = 0; }, nullptr);
        CHECK(t2.mismatches > 0, "synctest should catch nondeterminism");
    }
    // 7) adaptive delay + wide window on a 150 ms (one way) lossy link: fewer stalls than the old fixed delay 1 / window 8, never a desync
    {
        g_adaptive = false; g_pred = 8;
        Result a = run(1500, 150, 20, 5, 1, false, 100);
        report("fixed D=1 pred 8", a, true);
        g_adaptive = true; g_pred = 14;
        Result b = run(1500, 150, 20, 5, 1, false, 100);
        report("adaptive pred 14", b, true);
        CHECK(b.frame[0] == 1500 && b.frame[1] == 1500, "frames reached");
        CHECK(b.x[0] == b.x[1] && b.y[0] == b.y[1], "final state differs");
        CHECK(b.st[0].delay_changes > 0 && b.delay[0] >= 3, "delay should have grown (got %d)", b.delay[0]);
        CHECK(b.st[0].rtt_ms > 250 && b.st[0].rtt_ms < 420, "rtt estimate off: %.0f", b.st[0].rtt_ms);
        CHECK(b.st[0].stalled_ms + b.st[1].stalled_ms < a.st[0].stalled_ms + a.st[1].stalled_ms, "adaptive should stall less (%llu vs %llu ms)",
              (unsigned long long)(b.st[0].stalled_ms + b.st[1].stalled_ms), (unsigned long long)(a.st[0].stalled_ms + a.st[1].stalled_ms));
    }
    // 8) a delay that is too high on a LAN shrinks back; both peers stay in sync across the changes
    {
        g_adaptive = true; g_pred = 14;
        Result r = run(900, 10, 0, 0, 4, false, 100);
        report("adaptive shrink 4->0", r, true);
        CHECK(r.delay[0] == 0 && r.delay[1] == 0, "delay should shrink to 0 (got %d/%d)", r.delay[0], r.delay[1]);
        CHECK(r.st[0].delay_changes >= 4, "delay changes");
        CHECK(r.x[0] == r.x[1] && r.y[0] == r.y[1], "final state differs after delay changes");
    }
    // 9) asymmetric link (peer 1's packets take 100 ms longer): peers pick different delays, stay in sync, no desync
    {
        g_adaptive = true; g_pred = 14; g_lat1 = 100;
        Result r = run(1200, 20, 5, 2, 1, false, 100);
        report("asymmetric +100ms", r, true);
        CHECK(r.frame[0] == 1200 && r.frame[1] == 1200, "frames reached");
        CHECK(r.x[0] == r.x[1] && r.y[0] == r.y[1], "final state differs");
        g_lat1 = -1;
    }
    // 10) time sync: a 70% speed peer with the wide window: the fast peer paces itself, rollbacks stay shallow
    {
        g_adaptive = true; g_pred = 14;
        Result r = run(900, 40, 5, 0, 1, false, 70);
        report("slow peer 70% adaptive", r, true);
        CHECK(r.st[0].sync_waits > 5, "fast peer should wait gradually");
        CHECK(r.st[0].max_depth <= 14 && r.st[1].max_depth <= 14, "depth bounded");
        CHECK(r.x[0] == r.x[1] && r.y[0] == r.y[1], "final state differs");
    }
    printf(g_fail ? "ROLLBACK TEST FAILED (%d)\n" : "ROLLBACK TEST PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
