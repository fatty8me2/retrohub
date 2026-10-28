/* Console test: desync recovery (rollback.h, state_xfer.h). N peers on a virtual clock over a lossy link, a fake console with a 150 KB state.
 * One peer is corrupted at some frames; the host's state must replace everybody's, play must continue and every later state hash must match.
 * Also: a state that is nondeterministic for good makes the engine give up after max_resyncs; recovery off = old behaviour (desync ends the game). Exit 0 = pass. */
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <vector>

#include "rollback.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const size_t kState = 150000;

struct FakeSim {
    std::vector<uint8_t> mem = std::vector<uint8_t>(kState, 0);   // mostly zeros plus a small hot area: compresses like a console state
    uint64_t x = 1, y = 7, last_sum = 0, pad_now[4] = {0, 0, 0, 0};
    uint32_t frame = 0;
    void step(const uint64_t p[4]) {
        for (int i = 0; i < 4; i++) { x = (x ^ p[i]) * 0x9E3779B97F4A7C15ull; x ^= x >> 31; y += x * (i + 3) + (y >> 5); }
        mem[(x >> 7) % 4096] ^= (uint8_t)y;
        frame++;
        last_sum = x ^ (y * 31);
    }
    uint64_t hash() const { return rb::hash_bytes(mem.data(), mem.size()) ^ x ^ (y << 1) ^ frame; }
};

static int g_corrupt_calls = 0;
static rb::Sim make_sim(FakeSim* s, int port, uint32_t seed, std::function<bool(FakeSim*, uint32_t)> poison) {
    rb::Sim sim;
    sim.state_size = [] { return kState + 20; };
    sim.save = [s](uint8_t* b, size_t) { memcpy(b, s->mem.data(), kState); memcpy(b + kState, &s->x, 8); memcpy(b + kState + 8, &s->y, 8); memcpy(b + kState + 16, &s->frame, 4); return true; };
    sim.load = [s](const uint8_t* b, size_t) { memcpy(s->mem.data(), b, kState); memcpy(&s->x, b + kState, 8); memcpy(&s->y, b + kState + 8, 8); memcpy(&s->frame, b + kState + 16, 4); return true; };
    auto left = std::make_shared<int>(0);   // not part of the saved state, and counted in steps (replays included): a rollback cannot heal it
    sim.step = [s, poison, left](const uint64_t p[4], bool vis) {
        if (vis && poison && poison(s, s->frame)) { g_corrupt_calls++; *left = 24; }
        if (*left > 0) { (*left)--; uint64_t q[4] = {p[0] ^ 5, p[1], p[2], p[3]}; s->step(q); return; }
        s->step(p);
    };
    sim.frame_sum = [s] { return s->last_sum; };
    sim.poll_local = [s, port, seed] {
        uint32_t v = ((s->frame / 3) * 2654435761u) ^ (uint32_t)(port + 1) * 40503u ^ seed;
        v ^= v >> 13; v *= 2246822519u; v ^= v >> 15;
        s->pad_now[port] = v & 0x3FF;
    };
    sim.read_local = [s](int p) { return s->pad_now[p]; };
    sim.pack_state = [](const uint8_t* in, size_t n, std::vector<uint8_t>* out) {   // trivial run-length coder: enough to exercise the compressed path
        out->clear();
        for (size_t i = 0; i < n;) { size_t j = i; while (j < n && j - i < 255 && in[j] == in[i]) j++; out->push_back((uint8_t)(j - i)); out->push_back(in[i]); i = j; }
        return true;
    };
    sim.unpack_state = [](const uint8_t* in, size_t n, uint8_t* out, size_t raw) {
        size_t o = 0;
        for (size_t i = 0; i + 1 < n; i += 2) { if (o + in[i] > raw) return false; memset(out + o, in[i + 1], in[i]); o += in[i]; }
        return o == raw;
    };
    return sim;
}

struct Net {
    struct Msg { uint32_t at; int from; std::vector<uint8_t> d; };
    std::deque<Msg> q[4];
    uint32_t now = 1000, rng = 4242;
    int latency = 30, jitter = 10, loss = 3;
    uint32_t rnd() { rng = rng * 1664525u + 1013904223u; return rng >> 8; }
    void send(int from, int to, int n_peers, const void* d, int n) {
        for (int t = 0; t < n_peers; t++) {
            if (t == from || (to >= 0 && t != to)) continue;
            if ((int)(rnd() % 100) < loss) continue;
            q[t].push_back({now + latency + (jitter ? rnd() % (jitter + 1) : 0), from, std::vector<uint8_t>((const uint8_t*)d, (const uint8_t*)d + n)});
        }
    }
    bool recv(int me, int* from, std::vector<uint8_t>* out) {
        for (size_t i = 0; i < q[me].size(); i++)
            if (q[me][i].at <= now) { *from = q[me][i].from; *out = q[me][i].d; q[me].erase(q[me].begin() + i); return true; }
        return false;
    }
};

struct Run {
    int peers = 2, frames = 900, max_resyncs = 3, loss = 3;
    bool relayed = false;
    std::function<bool(int peer, FakeSim*, uint32_t frame)> poison;
    // results
    bool desync[4] = {}; int resyncs[4] = {}; uint32_t frame[4] = {}; uint64_t h[4] = {}; int notices[4] = {};
    uint64_t chain_out[4] = {};
};

static void run(Run& r) {
    Net net; net.loss = r.loss;
    FakeSim sim[4];
    std::unique_ptr<rb::Engine> eng[4];
    for (int i = 0; i < r.peers; i++) {
        rb::Config c;
        for (int p = 0; p < 4; p++) c.owner[p] = p < r.peers ? p : -1;
        c.my_slot = i; c.slot_mask = (1u << r.peers) - 1; c.delay = 1; c.hash_interval = 60; c.max_pred = 8; c.frame_ms = 16.0;
        c.max_resyncs = r.max_resyncs; c.use_state_hash = false;
        rb::Transport t;
        int n = r.peers;
        t.pump = [] {};
        t.send = [&net, i, n](int to, const void* d, int len) { net.send(i, to, n, d, len); };
        t.recv = [&net, i](int* from, std::vector<uint8_t>* o) { return net.recv(i, from, o); };
        t.now_ms = [&net] { return net.now; };
        t.log = [i](const std::string& s) { if (getenv("RESYNC_VERBOSE")) printf("  [peer %d] %s\n", i, s.c_str()); };
        t.notice = [&r, i](const std::string& s) { if (s == "Resynced") r.notices[i]++; };
        t.relayed = [&r] { return r.relayed; };
        int pi = i;
        auto poison = [&r, pi](FakeSim* s, uint32_t f) { return r.poison && r.poison(pi, s, f); };
        eng[i].reset(new rb::Engine(c, make_sim(&sim[i], i, 91 + i, poison), t));
    }
    for (int round = 0; round < r.frames * 12; round++) {
        bool all = true;
        for (int i = 0; i < r.peers; i++) if (eng[i]->cur() < (uint32_t)r.frames && !eng[i]->desync()) all = false;
        if (all) break;
        net.now += 16;
        for (int i = 0; i < r.peers; i++) {
            if (eng[i]->desync()) continue;
            if (eng[i]->cur() >= (uint32_t)r.frames) { eng[i]->service(); continue; }
            if (eng[i]->prepare() == rb::Engine::Status::Ready) eng[i]->run_frame();
        }
    }
    for (int k = 0; k < 300; k++) { net.now += 16; for (int i = 0; i < r.peers; i++) if (!eng[i]->desync()) eng[i]->service(); }
    for (int i = 0; i < r.peers; i++) {
        r.desync[i] = eng[i]->desync(); r.resyncs[i] = eng[i]->resyncs(); r.frame[i] = eng[i]->cur(); r.h[i] = sim[i].hash();
        r.chain_out[i] = eng[i]->confirmed_out_hash();
    }
}

int main() {
    // 1) one corrupted guest: recovered, play continues, final states and hash chains identical
    for (int loss : {0, 5}) {
        for (bool relay : {false, true}) {
            Run r; r.loss = loss; r.relayed = relay;
            bool done = false;
            r.poison = [&done](int peer, FakeSim*, uint32_t f) { if (peer == 1 && f == 200 && !done) { done = true; return true; } return false; };
            run(r);
            printf("guest corrupted loss %d%% relay %d: frames %u/%u resyncs %d/%d notices %d/%d desync %d/%d\n", loss, relay, r.frame[0], r.frame[1], r.resyncs[0], r.resyncs[1], r.notices[0], r.notices[1], r.desync[0], r.desync[1]);
            CHECK(!r.desync[0] && !r.desync[1], "gave up");
            CHECK(r.resyncs[0] == 1 && r.resyncs[1] == 1, "resync count");
            CHECK(r.notices[0] == 1 && r.notices[1] == 1, "Resynced notice");
            CHECK(r.frame[0] == 900 && r.frame[1] == 900, "frames reached");
            CHECK(r.h[0] == r.h[1], "final state differs after resync");
            CHECK(r.chain_out[0] == r.chain_out[1], "confirmed output chains differ after resync");
        }
    }
    // 2) the host itself corrupted: its own confirmed state is still the authority
    {
        Run r; bool done = false;
        r.poison = [&done](int peer, FakeSim*, uint32_t f) { if (peer == 0 && f == 333 && !done) { done = true; return true; } return false; };
        run(r);
        CHECK(!r.desync[0] && !r.desync[1] && r.resyncs[0] == 1 && r.h[0] == r.h[1] && r.frame[0] == 900, "host-side corruption (desync %d/%d resyncs %d)", r.desync[0], r.desync[1], r.resyncs[0]);
    }
    // 3) three recoveries in one session, 4 players: all converge
    {
        Run r; r.peers = 4; r.frames = 1500;
        bool d1 = false, d2 = false, d3 = false;
        r.poison = [&](int peer, FakeSim*, uint32_t f) {
            if (peer == 2 && f == 200 && !d1) { d1 = true; return true; }
            if (peer == 3 && f == 600 && !d2) { d2 = true; return true; }
            if (peer == 1 && f == 1000 && !d3) { d3 = true; return true; }
            return false; };
        run(r);
        printf("4 players, 3 corruptions: resyncs %d %d %d %d\n", r.resyncs[0], r.resyncs[1], r.resyncs[2], r.resyncs[3]);
        bool ok = true;
        for (int i = 0; i < 4; i++) { ok = ok && !r.desync[i] && r.resyncs[i] == 3 && r.h[i] == r.h[0] && r.frame[i] == 1500; }
        CHECK(ok, "4P three resyncs");
    }
    // 4) a 4th desync exceeds the budget: the game ends as before
    {
        Run r; r.frames = 2400;
        int hits = 0;
        r.poison = [&hits](int peer, FakeSim*, uint32_t f) { static const uint32_t at[4] = {200, 700, 1200, 1700}; if (peer != 1) return false; for (int k = 0; k < 4; k++) if (f == at[k] && hits == k) { hits++; return true; } return false; };
        run(r);
        printf("4 corruptions, budget 3: resyncs %d/%d desync %d/%d\n", r.resyncs[0], r.resyncs[1], r.desync[0], r.desync[1]);
        CHECK(r.resyncs[0] == 3 && r.resyncs[1] == 3 && (r.desync[0] || r.desync[1]), "should give up after 3");
    }
    // 5) recovery off (default): a desync ends the game
    {
        Run r; r.max_resyncs = 0; bool done = false;
        r.poison = [&done](int peer, FakeSim*, uint32_t f) { if (peer == 1 && f == 200 && !done) { done = true; return true; } return false; };
        run(r);
        CHECK(r.desync[0] || r.desync[1], "recovery off must still flag the desync");
        CHECK(r.resyncs[0] == 0 && r.resyncs[1] == 0, "no resync when off");
    }
    printf(g_fail ? "RESYNC TEST FAILED (%d)\n" : "RESYNC TEST PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
