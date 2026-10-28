/* Console test: drop-in (rollback.h kPktAdmit, Engine::host_admit / join_finish). N peers on a virtual clock over a lossy link, a fake console with a
 * 150 KB state. Two peers play; mid-match a third (then a fourth) takes an empty port: the host's confirmed snapshot is loaded by the joiner, replayed with the
 * confirmed pads up to the live game (what retrorun does with the spectate stream) and the joiner's engine takes over. Everybody must end on the same state and
 * hash chains, nobody may notice a stall, and a joiner that never shows up must not freeze the game for long. Exit 0 = pass. */
#include <algorithm>
#include <array>
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
    std::vector<uint8_t> mem = std::vector<uint8_t>(kState, 0);
    uint64_t x = 1, y = 7, last_sum = 0, pad_now[4] = {0, 0, 0, 0};
    uint32_t frame = 0;
    uint64_t nonzero_local = 0;   // frames this PC itself supplied a non-neutral pad (for the joiner: proves it really plays)
    void step(const uint64_t p[4]) {
        for (int i = 0; i < 4; i++) { x = (x ^ p[i]) * 0x9E3779B97F4A7C15ull; x ^= x >> 31; y += x * (i + 3) + (y >> 5); }
        mem[(x >> 7) % 4096] ^= (uint8_t)y;
        frame++;
        last_sum = x ^ (y * 31);
    }
    uint64_t hash() const { return rb::hash_bytes(mem.data(), mem.size()) ^ x ^ (y << 1) ^ frame; }
};

static rb::Sim make_sim(FakeSim* s, int port, uint32_t seed) {
    rb::Sim sim;
    sim.state_size = [] { return kState + 20; };
    sim.save = [s](uint8_t* b, size_t) { memcpy(b, s->mem.data(), kState); memcpy(b + kState, &s->x, 8); memcpy(b + kState + 8, &s->y, 8); memcpy(b + kState + 16, &s->frame, 4); return true; };
    sim.load = [s](const uint8_t* b, size_t) { memcpy(s->mem.data(), b, kState); memcpy(&s->x, b + kState, 8); memcpy(&s->y, b + kState + 8, 8); memcpy(&s->frame, b + kState + 16, 4); return true; };
    sim.step = [s](const uint64_t p[4], bool) { s->step(p); };
    sim.frame_sum = [s] { return s->last_sum; };
    sim.poll_local = [s, port, seed] {
        uint32_t v = ((s->frame / 3) * 2654435761u) ^ (uint32_t)(port + 1) * 40503u ^ seed;
        v ^= v >> 13; v *= 2246822519u; v ^= v >> 15;
        s->pad_now[port] = (v & 0x3FF) | 0x001;   // never neutral: a joiner that plays shows up in everybody's state
        s->nonzero_local++;
    };
    sim.read_local = [s](int p) { return s->pad_now[p]; };
    sim.pack_state = [](const uint8_t* in, size_t n, std::vector<uint8_t>* out) {
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
    bool dead[4] = {false, false, false, false};   // a crashed PC: nothing goes in or out
    uint32_t rnd() { rng = rng * 1664525u + 1013904223u; return rng >> 8; }
    void send(int from, int to, const void* d, int n) {
        if (dead[from]) return;
        for (int t = 0; t < 4; t++) {
            if (t == from || (to >= 0 && t != to) || dead[t]) continue;
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
    int players = 2;            // present at the start
    int frames = 1500;
    int loss = 3;
    bool relayed = false;
    bool old_peer = false;      // slot 1 does not advertise drop-in
    int join_at[2] = {300, -1}; // host frame at which the next joiner (slot 2, then slot 3) is admitted; -1 none
    bool joiner_crashes = false;
    // results
    bool desync[4] = {}; uint32_t frame[4] = {}; uint64_t h[4] = {}, chain_out[4] = {}, chain_in[4] = {}; uint64_t stalled_ms[4] = {}, stalls[4] = {};
    bool admitted[2] = {}; int notices[4] = {}; bool refused_old = false; uint32_t dropped_mask[4] = {};
    uint64_t j_local[4] = {}; uint32_t takeover[4] = {}; uint32_t admit_from[4] = {};
    int max_gap_frames = 0;     // longest run of rounds the host did not advance (stall seen by an existing player)
};

static void run(Run& r) {
    Net net; net.loss = r.loss;
    FakeSim sim[4];
    std::unique_ptr<rb::Engine> eng[4];
    std::vector<std::array<uint64_t, 4>> hist;   // confirmed pads, from the host (the spectate stream)
    for (int i = 0; i < 4; i++) {
        rb::Config c;
        const bool joiner = i >= r.players;
        for (int p = 0; p < 4; p++) c.owner[p] = p < r.players ? p : -1;
        c.my_slot = i; c.slot_mask = (1u << r.players) - 1; c.delay = 1; c.hash_interval = 60; c.max_pred = 8; c.frame_ms = 16.0;
        c.max_resyncs = 3; c.use_state_hash = false;
        c.joiner = joiner;
        if (r.old_peer && i == 1) c.caps = 0;
        rb::Transport t;
        t.pump = [] {};
        t.send = [&net, i](int to, const void* d, int len) { net.send(i, to, d, len); };
        t.recv = [&net, i](int* from, std::vector<uint8_t>* o) { return net.recv(i, from, o); };
        t.now_ms = [&net] { return net.now; };
        t.log = [i](const std::string& s) { if (getenv("DROPIN_VERBOSE")) printf("  [peer %d] %s\n", i, s.c_str()); };
        t.notice = [&r, i](const std::string& s) { if (s.find(" joined") != std::string::npos) r.notices[i]++; };
        t.relayed = [&r] { return r.relayed; };
        eng[i].reset(new rb::Engine(c, make_sim(&sim[i], i, 91 + i), t));
    }
    for (int i = 0; i < r.players; i++) { uint8_t rd[3] = {rb::kPktReady, (uint8_t)i, (uint8_t)(r.old_peer && i == 1 ? 0 : rb::kCapDropIn)}; net.send(i, -1, rd, 3); }   // the start barrier's caps exchange
    eng[0]->set_confirmed_sink([&hist](uint32_t f, const uint64_t pads[4]) { if (f == hist.size()) hist.push_back({pads[0], pads[1], pads[2], pads[3]}); });

    struct J { bool started = false, loaded = false, done = false; uint32_t f = 0; uint64_t ro = 0, ri = 0; int idx = 0; } js[2];
    const uint64_t kPrime = 0x100000001B3ull;
    int host_gap = 0;
    uint32_t last_host_cur = 0;
    for (int round = 0; round < r.frames * 14; round++) {
        bool all = true;
        for (int i = 0; i < 4; i++) {
            const bool exists = i < r.players || (i - r.players < 2 && js[i - r.players].started);
            if (!exists || net.dead[i]) continue;
            if (eng[i]->cur() < (uint32_t)r.frames && !eng[i]->desync()) all = false;
            if (i >= r.players && !js[i - r.players].done) all = false;
        }
        const bool want_more = (r.join_at[0] >= 0 && !js[0].started) || (r.join_at[1] >= 0 && !js[1].started);
        if (all && !want_more) break;
        net.now += 16;
        // host-side trigger: snapshot + admit
        for (int k = 0; k < 2; k++) {
            const int slot = r.players + k;
            if (r.join_at[k] < 0 || js[k].started || (k == 1 && !js[0].done && !r.joiner_crashes) || eng[0]->cur() < (uint32_t)r.join_at[k]) continue;
            if (!eng[0]->dropin_capable()) { if (r.old_peer) { r.refused_old = true; js[k].started = true; js[k].done = true; r.join_at[k] = -1; } continue; }
            std::vector<uint8_t> raw; uint32_t F; uint64_t ro, ri;
            if (!eng[0]->snapshot_confirmed(&raw, &F, &ro, &ri)) continue;
            js[k].started = true;
            js[k].idx = slot;
            rb::Sim js_sim = make_sim(&sim[slot], slot, 91 + slot);
            js_sim.load(raw.data(), raw.size());
            js[k].f = F; js[k].ro = ro; js[k].ri = ri; js[k].loaded = true;
            const int port = eng[0]->free_port();
            CHECK(port == slot, "free port %d, expected %d", port, slot);
            r.admitted[k] = eng[0]->host_admit(slot, port, "Kai");
            CHECK(r.admitted[k], "host_admit refused");
            r.admit_from[k] = eng[slot]->join_port() >= 0 ? eng[slot]->join_from() : 0;
            if (r.joiner_crashes) net.dead[slot] = true;
        }
        for (int i = 0; i < 4; i++) {
            const bool is_j = i >= r.players;
            if (is_j && !js[i - r.players].started) { eng[i]->service(); continue; }   // listening already (its Session is up)
            if (net.dead[i]) continue;
            if (eng[i]->desync()) continue;
            if (is_j && !js[i - r.players].done) {
                J& j = js[i - r.players];
                eng[i]->service();
                // fast-forward with the confirmed pads (like run_rb_spectator), unpaced
                int steps = 0;
                rb::Sim s = make_sim(&sim[i], i, 91 + i);
                while (j.f < hist.size() && steps < 12) {
                    const auto& pads = hist[j.f];
                    uint64_t p4[4] = {pads[0], pads[1], pads[2], pads[3]};
                    s.step(p4, false);
                    j.ro = rb::roll_step(j.ro, s.frame_sum());
                    for (int q = 0; q < 4; q++) j.ri = (j.ri ^ p4[q]) * kPrime;
                    j.f++; steps++;
                }
                if (eng[i]->join_admitted() && hist.size() - j.f <= 3 && j.f > 0) {
                    const auto& lp = hist[j.f - 1];
                    uint64_t p4[4] = {lp[0], lp[1], lp[2], lp[3]};
                    eng[i]->join_finish(j.f, j.ro, j.ri, p4);
                    eng[0]->note_join_active(i);
                    j.done = true;
                    r.takeover[i] = j.f;
                }
                continue;
            }
            if (eng[i]->cur() >= (uint32_t)r.frames) { eng[i]->service(); continue; }
            if (eng[i]->prepare() == rb::Engine::Status::Ready) eng[i]->run_frame();
        }
        // stall seen by the host: rounds in a row without a new frame (a round is one 16 ms frame time)
        if (eng[0]->cur() == last_host_cur && eng[0]->cur() < (uint32_t)r.frames) { host_gap++; r.max_gap_frames = std::max(r.max_gap_frames, host_gap); } else host_gap = 0;
        last_host_cur = eng[0]->cur();
    }
    for (int k = 0; k < 400; k++) { net.now += 16; for (int i = 0; i < 4; i++) if (!net.dead[i] && !eng[i]->desync() && !(i >= r.players && !js[i - r.players].started)) eng[i]->service(); }
    for (int i = 0; i < 4; i++) {
        r.desync[i] = eng[i]->desync(); r.frame[i] = eng[i]->cur(); r.h[i] = sim[i].hash();
        r.chain_out[i] = eng[i]->confirmed_out_hash(); r.chain_in[i] = eng[i]->confirmed_in_hash();
        r.stalled_ms[i] = eng[i]->stats().stalled_ms; r.stalls[i] = eng[i]->stats().stalls; r.dropped_mask[i] = eng[i]->dropped_mask();
        r.j_local[i] = sim[i].nonzero_local;
    }
}

int main() {
    // 1) 2 players, a 3rd drops in at frame ~300: loss / relay matrix
    for (int loss : {0, 5}) {
        for (bool relay : {false, true}) {
            Run r; r.loss = loss; r.relayed = relay;
            run(r);
            printf("2P + 1 drop-in, loss %d%% relay %d: frames %u/%u/%u takeover %u admit-from %u max host gap %d frames, stalled ms %llu/%llu, notices %d/%d, desync %d%d%d\n", loss, relay, r.frame[0], r.frame[1], r.frame[2],
                   r.takeover[2], r.admit_from[2], r.max_gap_frames, (unsigned long long)r.stalled_ms[0], (unsigned long long)r.stalled_ms[1], r.notices[0], r.notices[1], r.desync[0], r.desync[1], r.desync[2]);
            CHECK(r.admitted[0], "not admitted");
            CHECK(!r.desync[0] && !r.desync[1] && !r.desync[2], "desync");
            CHECK(r.frame[0] == 1500 && r.frame[1] == 1500 && r.frame[2] == 1500, "frames reached");
            CHECK(r.h[0] == r.h[1] && r.h[1] == r.h[2], "final state differs");
            CHECK(r.chain_out[0] == r.chain_out[1] && r.chain_out[1] == r.chain_out[2], "output chains differ");
            CHECK(r.chain_in[0] == r.chain_in[1] && r.chain_in[1] == r.chain_in[2], "input chains differ");
            CHECK(r.notices[0] == 1 && r.notices[1] == 1, "'joined' notice (host %d guest %d)", r.notices[0], r.notices[1]);
            CHECK(r.takeover[2] > 0 && r.takeover[2] < 700, "joiner took over late (frame %u)", r.takeover[2]);
            CHECK(r.max_gap_frames <= 12, "existing players stalled %d frames", r.max_gap_frames);   // 12 rounds = 190 ms
        }
    }
    // 2) a joiner's own input really reaches everybody: the state differs from the same match without a 3rd player
    {
        Run a; a.join_at[0] = 300; run(a);
        Run b; b.join_at[0] = -1; run(b);
        CHECK(a.h[0] != b.h[0], "the joiner's input never changed the game");
    }
    // 3) 3 players at launch (ports 0-2), a 4th drops in
    {
        Run r; r.players = 3; r.join_at[0] = 400; r.frames = 1800; run(r);
        printf("3P + 1 drop-in: frames %u/%u/%u/%u desync %d%d%d%d gap %d\n", r.frame[0], r.frame[1], r.frame[2], r.frame[3], r.desync[0], r.desync[1], r.desync[2], r.desync[3], r.max_gap_frames);
        bool ok = r.admitted[0];
        for (int i = 0; i < 4; i++) ok = ok && !r.desync[i] && r.frame[i] == 1800 && r.h[i] == r.h[0] && r.chain_out[i] == r.chain_out[0];
        CHECK(ok, "3P + drop-in did not converge");
        CHECK(r.notices[0] == 1 && r.notices[1] == 1 && r.notices[2] == 1, "notices");
    }
    // 4) two drop-ins in a row (3rd, then 4th)
    {
        Run r; r.join_at[0] = 300; r.join_at[1] = 700; r.frames = 2000; run(r);
        printf("2P + 2 drop-ins: frames %u/%u/%u/%u desync %d%d%d%d\n", r.frame[0], r.frame[1], r.frame[2], r.frame[3], r.desync[0], r.desync[1], r.desync[2], r.desync[3]);
        bool ok = r.admitted[0] && r.admitted[1];
        for (int i = 0; i < 4; i++) ok = ok && !r.desync[i] && r.frame[i] == 2000 && r.h[i] == r.h[0] && r.chain_out[i] == r.chain_out[0];
        CHECK(ok, "two drop-ins did not converge");
    }
    // 5) an old peer (no drop-in capability): the host refuses cleanly, the match is untouched
    {
        Run r; r.old_peer = true; r.frames = 900; run(r);
        CHECK(r.refused_old, "host should refuse with an old peer");
        CHECK(!r.desync[0] && !r.desync[1] && r.h[0] == r.h[1] && r.frame[0] == 900, "match disturbed by the refused join");
    }
    // 6) the joiner crashes right after the admit: the others stall only briefly, then play on without it
    {
        Run r; r.joiner_crashes = true; r.frames = 1500; run(r);
        printf("joiner crash: frames %u/%u desync %d%d dropped 0x%x gap %d rounds (%d ms)\n", r.frame[0], r.frame[1], r.desync[0], r.desync[1], r.dropped_mask[0], r.max_gap_frames, r.max_gap_frames * 16);
        CHECK(!r.desync[0] && !r.desync[1] && r.frame[0] == 1500 && r.frame[1] == 1500 && r.h[0] == r.h[1], "survivors did not finish together");
        CHECK(r.max_gap_frames * 16 < 9000, "survivors froze for %d ms", r.max_gap_frames * 16);
    }
    printf(g_fail ? "DROPIN TEST FAILED (%d)\n" : "DROPIN TEST PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
