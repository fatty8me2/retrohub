/* Console test: drop-in (rollback.h kPktAdmit, Engine::host_admit / join_finish) and the controller hand-off (kPktHandoff, Engine::host_handoff). N peers on a
 * virtual clock over a lossy link, a fake console with a 150 KB state. Two peers play; mid-match a third (then a fourth) takes an empty port: the host's confirmed snapshot is loaded by the joiner, replayed with the
 * confirmed pads up to the live game (what retrorun does with the spectate stream) and the joiner's engine takes over. Everybody must end on the same state and
 * hash chains, nobody may notice a stall, and a joiner that never shows up must not freeze the game for long. Exit 0 = pass. */
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <vector>

#include <climits>

#include "rollback.h"
#include "spectate.h"
#include "joinfp.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const size_t kState = 150000;

struct FakeSim {
    std::vector<uint8_t> mem = std::vector<uint8_t>(kState, 0);
    uint64_t x = 1, y = 7, last_sum = 0, pad_now[4] = {0, 0, 0, 0};
    uint32_t frame = 0;
    uint64_t nonzero_local = 0;   // frames this PC itself supplied a non-neutral pad (for the joiner: proves it really plays)
    uint32_t drive = 0;           // the ports this PC's pads drive (bit p): its own port, plus every port a hand-off gave it
    int poison_left = 0;          // QA: steps (replays included, outside the saved state: a rollback cannot heal it) that run with a wrong pad
    void step(const uint64_t p[4]) {
        for (int i = 0; i < 4; i++) { x = (x ^ p[i]) * 0x9E3779B97F4A7C15ull; x ^= x >> 31; y += x * (i + 3) + (y >> 5); }
        mem[(x >> 7) % 4096] ^= (uint8_t)y;
        frame++;
        last_sum = x ^ (y * 31);
    }
    uint64_t hash() const { return rb::hash_bytes(mem.data(), mem.size()) ^ x ^ (y << 1) ^ frame; }
};

/* The pad slot `slot` presses on `port` when its sim is at `frame`: what a test expects the confirmed history to hold. Never neutral. */
static uint64_t gen_pad(int slot, int port, uint32_t frame) {
    uint32_t v = ((frame / 3) * 2654435761u) ^ (uint32_t)(port + 1) * 40503u ^ (uint32_t)(91 + slot);
    v ^= v >> 13; v *= 2246822519u; v ^= v >> 15;
    return (v & 0x3FF) | 0x001;
}

static rb::Sim make_sim(FakeSim* s, int port, uint32_t seed) {
    s->drive |= 1u << port;
    rb::Sim sim;
    sim.state_size = [] { return kState + 20; };
    sim.save = [s](uint8_t* b, size_t) { memcpy(b, s->mem.data(), kState); memcpy(b + kState, &s->x, 8); memcpy(b + kState + 8, &s->y, 8); memcpy(b + kState + 16, &s->frame, 4); return true; };
    sim.load = [s](const uint8_t* b, size_t) { memcpy(s->mem.data(), b, kState); memcpy(&s->x, b + kState, 8); memcpy(&s->y, b + kState + 8, 8); memcpy(&s->frame, b + kState + 16, 4); return true; };
    sim.step = [s](const uint64_t p[4], bool) {
        if (s->poison_left > 0) { s->poison_left--; uint64_t q[4] = {p[0] ^ 5, p[1], p[2], p[3]}; s->step(q); return; }
        s->step(p);
    };
    sim.frame_sum = [s] { return s->last_sum; };
    sim.poll_local = [s, seed] {
        for (int p = 0; p < 4; p++) {
            if (!(s->drive >> p & 1u)) continue;
            uint32_t v = ((s->frame / 3) * 2654435761u) ^ (uint32_t)(p + 1) * 40503u ^ seed;
            v ^= v >> 13; v *= 2246822519u; v ^= v >> 15;
            s->pad_now[p] = (v & 0x3FF) | 0x001;   // never neutral: a joiner that plays shows up in everybody's state
        }
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
    bool deaf[4] = {false, false, false, false};   // never receives an Admit
    bool dead[4] = {false, false, false, false};   // a crashed PC: nothing goes in or out
    bool deaf_ho[4] = {false, false, false, false};   // never receives a Handoff
    uint32_t ho_deaf_until[4] = {0, 0, 0, 0};          // receives no Handoff before this time (a late decree)
    uint32_t rnd() { rng = rng * 1664525u + 1013904223u; return rng >> 8; }
    void send(int from, int to, const void* d, int n) {
        if (dead[from]) return;
        for (int t = 0; t < 4; t++) {
            if (t == from || (to >= 0 && t != to) || dead[t]) continue;
            if (deaf[t] && n > 0 && ((const uint8_t*)d)[0] == rb::kPktAdmit) continue;
            if (deaf_ho[t] && n > 0 && ((const uint8_t*)d)[0] == rb::kPktHandoff) continue;
            if (now < ho_deaf_until[t] && n > 0 && ((const uint8_t*)d)[0] == rb::kPktHandoff) continue;
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
    int fp_case = 0;            // the joiner's knock fingerprint: 0 not modelled, 1 matches, 2 other game, 3 other core, 4 no fingerprint (older RetroHub), 5 other netcode
    bool refused_fp = false; uint8_t fp_code = 0;   // the host refused the joiner for its fingerprint (kPktNoLive reason)
    int join_at[2] = {300, -1}; // host frame at which the next joiner (slot 2, then slot 3) is admitted; -1 none
    bool joiner_crashes = false;
    int kill_j0_at = -1;        // host frame at which the first joiner dies after it played (its port must become free again)
    bool deaf1 = false;         // slot 1 never receives an Admit (never acks)
    int j_hold_rounds = 0;      // the joiner waits this many rounds after the Admit before it takes over
    int admit_lead = INT_MIN;   // INT_MIN: the engine default; otherwise Engine::set_admit_lead_for_test
    // controller hand-off
    struct HoEv { int at, port, to, by = -1; bool retry = true; bool fired = false, ok = false; int reason = 0; };   // at: host frame; by: this guest asks (-1: the host decides)
    std::vector<HoEv> ho;
    bool handoff = false;       // every peer advertises kCapHandoff
    int old_ho = -1;            // ... except this slot
    int resyncs = 3;            // Config::max_resyncs (0: a desync is fatal and visible)
    bool deaf_ho[4] = {};       // that slot never receives a Handoff
    int ho_deaf_ms[4] = {};     // that slot hears no Handoff for this long after the first decree/request (it learns of it late)
    int kill_at[4] = {-1, -1, -1, -1};   // host frame at which that slot crashes
    int ho_lead = INT_MIN;      // INT_MIN: the engine default; otherwise Engine::set_handoff_lead_for_test (a late decree)
    int hash_iv = 60;           // Config::hash_interval
    int corrupt_at = -1;        // host frame at which slot 1's state is damaged (the hash check must notice and the host's state heal it)
    // results
    bool desync[4] = {}; uint32_t frame[4] = {}; uint64_t h[4] = {}, chain_out[4] = {}, chain_in[4] = {}; uint64_t stalled_ms[4] = {}, stalls[4] = {};
    bool aborted = false; int port_after_abort = -2; int second_port = -2; bool admitted[2] = {}; int notices[4] = {}; bool refused_old = false; uint32_t dropped_mask[4] = {};
    uint64_t j_local[4] = {}; uint32_t takeover[4] = {}; uint32_t admit_from[4] = {};
    int max_gap_frames = 0;     // longest run of rounds the host did not advance (stall seen by an existing player)
    int ho_n[4] = {}, ho_rev[4] = {}, ho_refused[4] = {}, owner_end[4][4] = {}; uint32_t resyncs_done[4] = {};
    std::vector<std::pair<uint32_t, int>> tl[4];            // per port, host's view: (first frame, new owner)
    std::map<uint32_t, std::array<uint64_t, 4>> conf[4];    // per engine: the confirmed pads of every frame it settled
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
        c.my_slot = i; c.slot_mask = (1u << r.players) - 1; c.delay = 1; c.hash_interval = r.hash_iv; c.max_pred = 8; c.frame_ms = 16.0;
        c.max_resyncs = r.resyncs; c.use_state_hash = false;
        c.joiner = joiner;
        c.caps = (uint8_t)(rb::kCapDropIn | (r.handoff && r.old_ho != i ? rb::kCapHandoff : 0));
        if (r.old_peer && i == 1) c.caps = 0;
        rb::Transport t;
        t.pump = [] {};
        t.send = [&net, i](int to, const void* d, int len) { net.send(i, to, d, len); };
        t.recv = [&net, i](int* from, std::vector<uint8_t>* o) { return net.recv(i, from, o); };
        t.now_ms = [&net] { return net.now; };
        t.log = [i](const std::string& s) { if (getenv("DROPIN_VERBOSE")) printf("  [peer %d] %s\n", i, s.c_str()); };
        t.notice = [&r, i](const std::string& s) { if (s.find(" joined") != std::string::npos) r.notices[i]++; };
        t.relayed = [&r] { return r.relayed; };
        t.handoff = [&r, &sim, i](int port, int, int to, uint32_t first) {
            r.ho_n[i]++;
            if (to == i) sim[i].drive |= 1u << port;
            if (i == 0) r.tl[port].push_back({first, to});
        };
        t.handoff_reverted = [&r, i](int port) { r.ho_rev[i]++; if (i == 0 && !r.tl[port].empty()) r.tl[port].pop_back(); };
        t.handoff_refused = [&r, i](int, int reason) { r.ho_refused[i] = reason; };
        eng[i].reset(new rb::Engine(c, make_sim(&sim[i], i, 91 + i), t));
        eng[i]->set_confirmed_sink([&r, i](uint32_t f, const uint64_t pads[4]) { r.conf[i][f] = {pads[0], pads[1], pads[2], pads[3]}; });
        if (r.ho_lead != INT_MIN) eng[i]->set_handoff_lead_for_test(r.ho_lead);
        net.deaf_ho[i] = r.deaf_ho[i];
    }
    for (int i = 0; i < r.players; i++) { uint8_t rd[3] = {rb::kPktReady, (uint8_t)i, (uint8_t)(r.old_peer && i == 1 ? 0 : rb::kCapDropIn)}; net.send(i, -1, rd, 3); }   // the start barrier's caps exchange
    net.deaf[1] = r.deaf1;
    eng[0]->set_confirmed_sink([&hist, &r](uint32_t f, const uint64_t pads[4]) { if (f == hist.size()) hist.push_back({pads[0], pads[1], pads[2], pads[3]}); r.conf[0][f] = {pads[0], pads[1], pads[2], pads[3]}; });

    struct J { bool started = false, loaded = false, done = false; uint32_t f = 0; uint64_t ro = 0, ri = 0; int idx = 0; int rounds = 0; } js[2];
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
        bool want_more = (r.join_at[0] >= 0 && !js[0].started) || (r.join_at[1] >= 0 && !js[1].started);
        for (auto& ev : r.ho) if (!ev.fired) want_more = true;
        if (all && !want_more) break;
        net.now += 16;
        // host-side trigger: snapshot + admit
        for (int k = 0; k < 2; k++) {
            const int slot = r.players + k;
            if (r.join_at[k] < 0 || js[k].started || (k == 1 && !js[0].done && !r.joiner_crashes) || eng[0]->cur() < (uint32_t)r.join_at[k]) continue;
            if (k == 1 && eng[0]->free_port() < 0) continue;   // wait until the first joiner's port was given up by the drop logic
            if (r.fp_case) {   // the host's knock gate (Frontend::join_gate): the same bytes the joiner puts on the wire
                joinfp::Fp host; host.content = 0xA1B2C3D4u; host.core = 0x11223344u;
                joinfp::Fp mine = host;
                if (r.fp_case == 2) mine.content ^= 1; else if (r.fp_case == 3) mine.core ^= 1; else if (r.fp_case == 5) mine.proto++;
                std::vector<uint8_t> kn = {spectate::kPktJoin, (uint8_t)slot, 2, rb::kCapDropIn, 0};
                if (r.fp_case != 4) joinfp::append(kn, mine);
                joinfp::Gate gt = joinfp::gate(host, kn);   // the host's whole decision (Frontend::join_gate calls exactly this)
                joinfp::Verdict v = gt.v;
                if (v != joinfp::Verdict::Ok) { r.refused_fp = true; r.fp_code = gt.reason; js[k].started = true; js[k].done = true; r.join_at[k] = -1; continue; }
            }
            if (!eng[0]->dropin_capable()) { if (r.old_peer) { r.refused_old = true; js[k].started = true; js[k].done = true; r.join_at[k] = -1; } continue; }
            std::vector<uint8_t> raw; uint32_t F; uint64_t ro, ri;
            if (!eng[0]->snapshot_confirmed(&raw, &F, &ro, &ri)) continue;
            js[k].started = true;
            js[k].idx = slot;
            rb::Sim js_sim = make_sim(&sim[slot], slot, 91 + slot);
            js_sim.load(raw.data(), raw.size());
            js[k].f = F; js[k].ro = ro; js[k].ri = ri; js[k].loaded = true;
            const int port = eng[0]->free_port();
            r.second_port = k == 1 ? port : r.second_port;
            CHECK(port == (r.kill_j0_at > 0 && k == 1 ? 2 : slot), "free port %d, expected %d", port, slot);
            if (r.admit_lead != INT_MIN) eng[0]->set_admit_lead_for_test(r.admit_lead);
            r.admitted[k] = eng[0]->host_admit(slot, port, "Kai");
            CHECK(r.admitted[k], "host_admit refused");
            r.admit_from[k] = eng[slot]->join_port() >= 0 ? eng[slot]->join_from() : 0;
            if (r.joiner_crashes) net.dead[slot] = true;
        }
        for (auto& ev : r.ho) {   // controller hand-offs: the host decides (or a guest asks) once the host reaches the frame
            if (ev.fired || eng[0]->cur() < (uint32_t)ev.at) continue;
            const int who = ev.by >= 0 ? ev.by : 0;
            if (net.dead[who]) { ev.fired = true; continue; }
            int reason = 0;
            ev.ok = ev.by >= 0 ? eng[who]->request_handoff(ev.port, ev.to, &reason) : eng[0]->host_handoff(ev.port, ev.to, &reason);
            ev.reason = reason;
            if (ev.ok) for (int k = 0; k < 4; k++) if (r.ho_deaf_ms[k] && !net.ho_deaf_until[k]) net.ho_deaf_until[k] = net.now + (uint32_t)r.ho_deaf_ms[k];
            if (ev.ok || !ev.retry || reason != rb::kHoBusy) ev.fired = true;
        }
        for (int k = 0; k < 4; k++) if (r.kill_at[k] >= 0 && eng[0]->cur() >= (uint32_t)r.kill_at[k]) net.dead[k] = true;
        if (r.corrupt_at >= 0 && eng[0]->cur() >= (uint32_t)r.corrupt_at) { r.corrupt_at = -1; sim[1].poison_left = 24; }
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
                if (eng[i]->join_aborted()) { j.done = true; r.aborted = true; r.port_after_abort = eng[0]->free_port(); continue; }
                if (eng[i]->join_admitted()) j.rounds++;
                if (eng[i]->join_admitted() && j.rounds >= r.j_hold_rounds && hist.size() - j.f <= 3 && j.f > 0) {
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
        if (r.kill_j0_at > 0 && js[0].done && eng[0]->cur() >= (uint32_t)r.kill_j0_at) net.dead[r.players] = true;
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
        for (int q = 0; q < 4; q++) r.owner_end[i][q] = eng[i]->owner_at(q, 1 << 30);
        r.resyncs_done[i] = (uint32_t)eng[i]->resyncs();
    }
}

/* The confirmed history of every settled frame must be the same on every PC AND the pads the right owners pressed: before a hand-off's first frame the old owner's,
 * from it on the new owner's (port q is slot q's at the start). Returns the number of bad frames; exact = false only compares the PCs with each other. */
static int check_pads(const Run& r, int ports, bool exact, int* compared) {
    int bad = 0;
    *compared = 0;
    for (int i = 0; i < 4; i++) {
        for (const auto& kv : r.conf[i]) {
            const uint32_t f = kv.first;
            for (int j = 0; j < i; j++) {
                auto it = r.conf[j].find(f);
                if (it == r.conf[j].end()) continue;
                (*compared)++;
                if (it->second != kv.second) { if (!bad) printf("  pads of frame %u differ between PC %d and PC %d\n", f, j, i); bad++; }
            }
            if (!exact) continue;
            for (int q = 0; q < ports; q++) {
                int owner = q;
                for (const auto& sg : r.tl[q]) if (sg.first <= f) owner = sg.second;
                const uint64_t want = f >= 1 ? gen_pad(owner, q, f - 1) : 0;
                if (kv.second[q] != want) { if (!bad) printf("  PC %d frame %u port %d: pad %llx, want %llx (slot %d)\n", i, f, q, (unsigned long long)kv.second[q], (unsigned long long)want, owner); bad++; }
            }
        }
    }
    return bad;
}

int main() {
    if (!getenv("DROPIN_ONLY_HANDOFF")) {   // (set it to run just the controller hand-off cases below)
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
    // 5b) the joiner's fingerprint (netcore-H4): a matching one is admitted; another game / core is refused with the mismatch reason, no fingerprint or another netcode
    //     version with the old-peer reason; the room plays on untouched every time
    for (int fc = 1; fc <= 5; fc++) {
        Run r; r.fp_case = fc; r.frames = 900; run(r);
        printf("fingerprint case %d: refused %d code %u admitted %d\n", fc, r.refused_fp, (unsigned)r.fp_code, r.admitted[0]);
        if (fc == 1) { CHECK(!r.refused_fp && r.admitted[0], "a matching fingerprint must be admitted"); continue; }
        const uint8_t want = fc == 2 ? joinfp::kRefuseMismatch : fc == 3 ? joinfp::kRefuseCore : joinfp::kRefuseOldPeer;
        CHECK(r.refused_fp && r.fp_code == want && !r.admitted[0], "fingerprint case %d: expected refusal %u, got %d/%u", fc, (unsigned)want, r.refused_fp, (unsigned)r.fp_code);
        CHECK(!r.desync[0] && !r.desync[1] && r.h[0] == r.h[1] && r.frame[0] == 900, "fingerprint case %d disturbed the match", fc);
    }
    {   // wire format: drop-in knock (after the name) and plain spectator knock (offset 3); content 0 = unknown matches anything; index files carry no game identity
        joinfp::Fp a; a.content = 0xDEADBEEFu; a.core = 0x01020304u; a.proto = 7;
        std::vector<uint8_t> k = {spectate::kPktJoin, 2, 2, rb::kCapDropIn, 3, 'a', 'b', 'c'};
        joinfp::append(k, a);
        joinfp::Fp g;
        CHECK(joinfp::parse(k.data(), k.size(), 8, &g) && g.content == a.content && g.core == a.core && g.proto == 7, "drop-in trailer round trip");
        CHECK(!joinfp::parse(k.data(), 8, 8, &g) && !joinfp::parse(k.data(), k.size() - 1, 8, &g) && !joinfp::parse(k.data(), k.size(), 5, &g), "short / misplaced trailer");
        std::vector<uint8_t> sp = {spectate::kPktJoin, 4, 1};
        joinfp::append(sp, a);
        CHECK(joinfp::parse(sp.data(), sp.size(), 3, &g) && g.core == a.core, "spectator trailer round trip");
        std::vector<uint8_t> plain = {spectate::kPktJoin, 4, 1};
        CHECK(!joinfp::parse(plain.data(), plain.size(), 3, &g), "an older spectator has no trailer");
        joinfp::Fp unk = a; unk.content = 0;
        CHECK(joinfp::check(a, true, unk) == joinfp::Verdict::Rom && joinfp::check(unk, true, a) == joinfp::Verdict::Rom && joinfp::check(unk, true, unk) == joinfp::Verdict::Ok, "an unknown game id only matches another unknown one");
        CHECK(joinfp::trailer_offset(k) == 8 && joinfp::trailer_offset(sp) == 3 && joinfp::trailer_offset({spectate::kPktJoin, 4, 9}) == 3, "trailer offsets");
        CHECK(joinfp::gate(a, k).pass && joinfp::gate(a, sp).pass, "gate: both knock layouts with a matching trailer");
        CHECK(joinfp::gate(a, plain).pass && joinfp::gate(a, plain).reason == 0, "gate: a spectator with no trailer (a v1.39 spectator) is admitted and streamed to");
        std::vector<uint8_t> dnt = {spectate::kPktJoin, 2, 2, rb::kCapDropIn, 3, 'a', 'b', 'c'};   // a v1.39 drop-in knock: no trailer
        CHECK(!joinfp::gate(a, dnt).pass && joinfp::gate(a, dnt).reason == joinfp::kRefuseOldPeer, "gate: a drop-in joiner with no trailer gets the old-version refusal");
        joinfp::Fp ob = a; ob.content ^= 1;
        std::vector<uint8_t> spb = {spectate::kPktJoin, 4, 1}; joinfp::append(spb, ob);
        CHECK(!joinfp::gate(a, spb).pass && joinfp::gate(a, spb).reason == joinfp::kRefuseMismatch, "gate: a spectator with another game is refused");
        joinfp::Fp op = a; op.proto++;
        std::vector<uint8_t> spp = {spectate::kPktJoin, 4, 1}; joinfp::append(spp, op);
        CHECK(!joinfp::gate(a, spp).pass && joinfp::gate(a, spp).reason == joinfp::kRefuseOldPeer, "gate: a spectator on another netcode version is refused");
        std::vector<uint8_t> nine = {spectate::kPktJoin, 4, 9};
        CHECK(!joinfp::gate(a, nine).pass && joinfp::gate(a, nine).reason == 0 && !joinfp::gate(a, {spectate::kPktJoin}).pass, "gate: short / drop-in-done knocks are dropped");
        joinfp::Fp oc = a; oc.core ^= 1;
        std::vector<uint8_t> kc = {spectate::kPktJoin, 4, 1}; joinfp::append(kc, oc);
        CHECK(joinfp::gate(a, kc).reason == joinfp::kRefuseCore, "core mismatch has its own reason");
        CHECK(joinfp::check(a, false, a) == joinfp::Verdict::Missing && joinfp::refuse_code(joinfp::Verdict::Missing) == joinfp::kRefuseOldPeer, "missing -> old peer");
        joinfp::Fp o = a; o.core ^= 5;
        CHECK(joinfp::check(a, true, o) == joinfp::Verdict::Core, "other core");
        o = a; o.content ^= 5;
        CHECK(joinfp::check(a, true, o) == joinfp::Verdict::Rom && joinfp::refuse_code(joinfp::Verdict::Rom) == joinfp::kRefuseMismatch, "other game");
        CHECK(joinfp::is_index_file("C:\g\Game.CUE") && joinfp::is_index_file("a.m3u") && !joinfp::is_index_file("a.chd") && !joinfp::is_index_file("nodot"), "index files");
        CHECK(joinfp::core_id("Beetle", "1.0") != joinfp::core_id("Beetle", "1.1") && joinfp::core_id("Beetle", "1.0") == joinfp::core_id("Beetle", "1.0"), "core id");
    }
    // 4b) a LATE admit (the admit frame is already behind every peer when the message arrives) and a very lossy link: still one game
    for (int lead : {0, -20}) {
        Run r; r.admit_lead = lead; r.frames = 1500; r.loss = lead ? 30 : 3; run(r);
        printf("late admit lead %d loss %d%%: frames %u/%u/%u desync %d%d%d\n", lead, r.loss, r.frame[0], r.frame[1], r.frame[2], r.desync[0], r.desync[1], r.desync[2]);
        bool ok = r.admitted[0];
        for (int i = 0; i < 3; i++) ok = ok && !r.desync[i] && r.frame[i] == 1500 && r.h[i] == r.h[0] && r.chain_out[i] == r.chain_out[0] && r.chain_in[i] == r.chain_in[0];
        CHECK(ok, "late admit (lead %d) did not converge", lead);
    }
    // 4c) a spectator joining after a drop-in reads port 2 from the stream; an old host's stream (port mask 0x03) reads 0 for it
    {
        spectate::Host old_host(0x03, 8), new_host(0x0F, 8);
        uint64_t v[4] = {5, 6, 7, 8};
        old_host.push(v); new_host.push(v);
        std::vector<uint8_t> got;
        spectate::Client co, cn;
        auto grab = [&](spectate::Host& h, spectate::Client& c) {
            h.service(1000, 1u << 1, [&](int, const uint8_t* d, int n) { c.on_data(d, (size_t)n, 1000); });
            h.service(1300, 1u << 1, [&](int, const uint8_t* d, int n) { c.on_data(d, (size_t)n, 1300); });
        };
        grab(old_host, co); grab(new_host, cn);
        CHECK(co.ready(0) && co.get(0, 0) == 5 && co.get(0, 1) == 6 && co.get(0, 2) == 0 && co.get(0, 3) == 0, "old host stream: absent ports read 0 (%llu %llu)", (unsigned long long)co.get(0, 2), (unsigned long long)co.get(0, 3));
        CHECK(cn.ready(0) && cn.get(0, 2) == 7 && cn.get(0, 3) == 8, "new host stream carries all four ports");
    }
    // 4d) a peer never acknowledges the Admit: the host cancels it after ~5 s for everybody, the joiner is told, the match goes on untouched, the port is free again
    {
        Run r; r.deaf1 = true; r.j_hold_rounds = 600; r.frames = 1500; run(r);
        printf("peer never acks: aborted %d, free port after %d, frames %u/%u desync %d%d\n", r.aborted, r.port_after_abort, r.frame[0], r.frame[1], r.desync[0], r.desync[1]);
        CHECK(r.aborted, "the joiner was not told the admit was cancelled");
        CHECK(r.port_after_abort == 2, "the port is not free again (%d)", r.port_after_abort);
        CHECK(!r.desync[0] && !r.desync[1] && r.frame[0] == 1500 && r.frame[1] == 1500 && r.h[0] == r.h[1] && r.chain_out[0] == r.chain_out[1], "the match diverged after a cancelled admit");
    }
    // 4e) a joiner plays and disconnects: the drop logic frees its port, and somebody else drops into the SAME port; everybody converges
    {
        Run r; r.kill_j0_at = 700; r.join_at[0] = 300; r.join_at[1] = 1500; r.frames = 2600; run(r);
        printf("joiner leaves, another drops into its port: second joiner's port %d, frames %u/%u/%u desync %d%d%d dropped(host) 0x%x\n", r.second_port, r.frame[0], r.frame[1], r.frame[3], r.desync[0], r.desync[1], r.desync[3], r.dropped_mask[0]);
        CHECK(r.second_port == 2, "second joiner did not get the freed port (%d)", r.second_port);
        CHECK(r.admitted[0] && r.admitted[1], "admits");
        CHECK(!r.desync[0] && !r.desync[1] && !r.desync[3] && r.frame[0] == 2600 && r.frame[1] == 2600 && r.frame[3] == 2600, "frames / desync");
        CHECK(r.h[0] == r.h[1] && r.h[1] == r.h[3] && r.chain_out[0] == r.chain_out[3] && r.chain_in[0] == r.chain_in[3], "state / chains differ");
    }
    // 6) the joiner crashes right after the admit: the others stall only briefly, then play on without it
    {
        Run r; r.joiner_crashes = true; r.frames = 1500; run(r);
        printf("joiner crash: frames %u/%u desync %d%d dropped 0x%x gap %d rounds (%d ms)\n", r.frame[0], r.frame[1], r.desync[0], r.desync[1], r.dropped_mask[0], r.max_gap_frames, r.max_gap_frames * 16);
        CHECK(!r.desync[0] && !r.desync[1] && r.frame[0] == 1500 && r.frame[1] == 1500 && r.h[0] == r.h[1], "survivors did not finish together");
        CHECK(r.max_gap_frames * 16 < 9000, "survivors froze for %d ms", r.max_gap_frames * 16);
    }

    }
    // ======================= controller hand-off (kPktHandoff, Engine::host_handoff / request_handoff) =======================
    auto converged = [](const Run& r, std::vector<int> peers, int frames) {
        for (int i : peers)
            if (r.desync[i] || r.frame[i] != (uint32_t)frames || r.h[i] != r.h[peers[0]] || r.chain_out[i] != r.chain_out[peers[0]] || r.chain_in[i] != r.chain_in[peers[0]]) return false;
        return true;
    };
    /* what a spectator sees: the confirmed pads of frames 0..frames-1 stepped through a fresh console must end in the players' final state */
    auto replay_ok = [](const Run& r, int frames) {
        FakeSim fs;
        for (int f = 0; f < frames; f++) {
            auto it = r.conf[0].find((uint32_t)f);
            if (it == r.conf[0].end()) return false;
            uint64_t p4[4] = {it->second[0], it->second[1], it->second[2], it->second[3]};
            fs.step(p4);
        }
        return fs.hash() == r.h[0];
    };
    // H1) 2 players: the host passes port 0 to slot 1, later slot 1 asks to give it back. Loss / relay matrix. The confirmed history has the old owner's pad before each
    //     first frame and the new owner's from it on (exact), every PC holds the same history, a spectator replaying it ends in the players' state, nobody resyncs.
    for (int loss : {0, 5}) {
        for (bool relay : {false, true}) {
            Run r; r.join_at[0] = -1; r.handoff = true; r.resyncs = 0; r.loss = loss; r.relayed = relay; r.frames = 1500;
            r.ho = {{300, 0, 1}, {800, 0, 0, 1}};
            run(r);
            int cmp = 0; const int bad = check_pads(r, 2, true, &cmp);
            printf("hand-off 0->1 and back, loss %d%% relay %d: frames %u/%u applied %d/%d (first %u), pads compared %d bad %d, state %s\n", loss, relay, r.frame[0], r.frame[1], r.ho_n[0], r.ho_n[1],
                   r.tl[0].empty() ? 0u : r.tl[0][0].first, cmp, bad, converged(r, {0, 1}, 1500) ? "equal" : "DIFFERENT");
            CHECK(r.ho[0].ok && r.ho[1].ok, "decrees refused (%d, %d)", r.ho[0].reason, r.ho[1].reason);
            CHECK(converged(r, {0, 1}, 1500), "did not converge");
            CHECK(r.ho_n[0] == 2 && r.ho_n[1] == 2 && r.tl[0].size() == 2, "applied %d/%d, host timeline %zu", r.ho_n[0], r.ho_n[1], r.tl[0].size());
            CHECK(bad == 0 && cmp > 1000, "confirmed pads wrong (%d bad of %d)", bad, cmp);
            CHECK(r.owner_end[0][0] == 0 && r.owner_end[1][0] == 0 && r.owner_end[0][1] == 1, "owners at the end: %d %d", r.owner_end[0][0], r.owner_end[1][0]);
            CHECK(r.resyncs_done[0] == 0 && r.resyncs_done[1] == 0, "needed a resync");
            CHECK(replay_ok(r, 1500), "a spectator replaying the confirmed stream ends in another state");
            Run ctl; ctl.join_at[0] = -1; ctl.handoff = true; ctl.resyncs = 0; ctl.loss = loss; ctl.relayed = relay; ctl.frames = 1500; run(ctl);
            CHECK(r.h[0] != ctl.h[0], "the hand-off never changed the game");
        }
    }
    // H2) 3 players, a chain of hand-offs between guests and the host (port 1 -> slot 2 and back asked by the guests, then port 0 -> slot 2 decided by the host)
    {
        Run r; r.join_at[0] = -1; r.players = 3; r.handoff = true; r.resyncs = 0; r.loss = 3; r.frames = 1800;
        r.ho = {{400, 1, 2, 1}, {800, 1, 1, 2}, {1000, 0, 2}};
        run(r);
        int cmp = 0; const int bad = check_pads(r, 3, true, &cmp);
        printf("3P chain of hand-offs: frames %u/%u/%u applied %d/%d/%d, pads compared %d bad %d, owners %d %d %d\n", r.frame[0], r.frame[1], r.frame[2], r.ho_n[0], r.ho_n[1], r.ho_n[2], cmp, bad,
               r.owner_end[0][0], r.owner_end[0][1], r.owner_end[0][2]);
        CHECK(r.ho[0].ok && r.ho[1].ok && r.ho[2].ok, "decrees refused (%d %d %d)", r.ho[0].reason, r.ho[1].reason, r.ho[2].reason);
        CHECK(converged(r, {0, 1, 2}, 1800) && bad == 0, "3P chain did not converge exactly");
        CHECK(r.ho_n[0] == 3 && r.ho_n[1] == 3 && r.ho_n[2] == 3, "not every PC applied every decree");
        CHECK(r.owner_end[0][0] == 2 && r.owner_end[0][1] == 1 && r.owner_end[1][0] == 2 && r.owner_end[2][1] == 1, "owners at the end");
        CHECK(r.resyncs_done[1] == 0 && replay_ok(r, 1800), "resync / spectator replay");
    }
    // H3) a hand-off, then a friend drops in (the drop-in waits for the hand-off to settle and joins into the free port); then the joiner takes slot 1's port and gives it back
    {
        Run r; r.join_at[0] = -1; r.handoff = true; r.resyncs = 0; r.frames = 2000; r.join_at[0] = 700;
        r.ho = {{300, 0, 1}, {1200, 1, 2}, {1600, 1, 1, 2}};
        run(r);
        int cmp = 0; const int bad = check_pads(r, 2, true, &cmp);
        printf("hand-off, drop-in, hand-off to the joiner and back: frames %u/%u/%u admitted %d takeover %u, hand-offs %d/%d/%d, pads bad %d of %d\n", r.frame[0], r.frame[1], r.frame[2], r.admitted[0],
               r.takeover[2], r.ho_n[0], r.ho_n[1], r.ho_n[2], bad, cmp);
        CHECK(r.admitted[0] && r.ho[0].ok && r.ho[1].ok && r.ho[2].ok, "admit %d, decrees %d %d %d (%d)", r.admitted[0], r.ho[0].ok, r.ho[1].ok, r.ho[2].ok, r.ho[1].reason);
        CHECK(converged(r, {0, 1, 2}, 2000), "did not converge");
        CHECK(bad == 0, "confirmed pads wrong after the drop-in");
        CHECK(r.owner_end[2][0] == 1 && r.owner_end[2][1] == 1 && r.owner_end[0][2] == 2, "owners at the end (joiner sees %d %d)", r.owner_end[2][0], r.owner_end[2][1]);
    }
    // H4) LATE decrees. (a) a tight lead: the guests hear of it after its first frame; (b) a guest is out of reach for 1.2 s and hears it way after the first frame (the
    //     game stalls at the prediction limit meanwhile, then it rolls back to the first frame and claims the port); (c) the same for the NEW owner of a guest-to-guest hand-off.
    for (int lead : {3, 8}) {
        Run r; r.join_at[0] = -1; r.handoff = true; r.resyncs = 0; r.frames = 1500; r.loss = 3; r.ho_lead = lead;
        r.ho = {{300, 0, 1}};
        run(r);
        int cmp = 0; const int bad = check_pads(r, 2, false, &cmp);
        printf("tight hand-off (lead %d frames): frames %u/%u applied %d/%d desync %d%d, pads compared %d bad %d\n", lead, r.frame[0], r.frame[1], r.ho_n[0], r.ho_n[1], r.desync[0], r.desync[1], cmp, bad);
        CHECK(r.ho[0].ok && converged(r, {0, 1}, 1500) && bad == 0 && r.ho_n[1] == 1, "tight decree (lead %d) did not converge", lead);
    }
    {
        Run r; r.join_at[0] = -1; r.handoff = true; r.resyncs = 0; r.frames = 1500; r.ho_deaf_ms[1] = 1200;
        r.ho = {{300, 0, 1}};
        run(r);
        int cmp = 0; const int bad = check_pads(r, 2, false, &cmp);
        printf("guest hears the decree 1.2 s late: frames %u/%u applied %d/%d desync %d%d, stalled ms %llu/%llu, pads bad %d of %d\n", r.frame[0], r.frame[1], r.ho_n[0], r.ho_n[1], r.desync[0], r.desync[1],
               (unsigned long long)r.stalled_ms[0], (unsigned long long)r.stalled_ms[1], bad, cmp);
        CHECK(r.ho[0].ok && converged(r, {0, 1}, 1500) && bad == 0 && r.ho_n[1] == 1 && r.owner_end[1][0] == 1, "late guest (1.2 s) did not converge");
    }
    {
        Run r; r.join_at[0] = -1; r.players = 3; r.handoff = true; r.resyncs = 0; r.frames = 1500; r.ho_deaf_ms[2] = 1200;
        r.ho = {{300, 1, 2}};
        run(r);
        int cmp = 0; const int bad = check_pads(r, 3, false, &cmp);
        printf("new owner hears the decree 1.2 s late (3P): frames %u/%u/%u desync %d%d%d, pads bad %d of %d\n", r.frame[0], r.frame[1], r.frame[2], r.desync[0], r.desync[1], r.desync[2], bad, cmp);
        CHECK(r.ho[0].ok && converged(r, {0, 1, 2}, 1500) && bad == 0 && r.owner_end[2][1] == 2, "late new owner did not converge");
    }
    {   // the OLD owner (a guest) hears it late: it keeps supplying frames the others no longer accept; the hash check notices and the host's state heals it (a resync, by design)
        Run r; r.join_at[0] = -1; r.players = 3; r.handoff = true; r.resyncs = 3; r.frames = 1800; r.ho_deaf_ms[1] = 1200;
        r.ho = {{300, 1, 2}};
        run(r);
        printf("old owner hears the decree 1.2 s late (3P): frames %u/%u/%u desync %d%d%d resyncs %u/%u/%u\n", r.frame[0], r.frame[1], r.frame[2], r.desync[0], r.desync[1], r.desync[2], r.resyncs_done[0], r.resyncs_done[1], r.resyncs_done[2]);
        CHECK(r.ho[0].ok && converged(r, {0, 1, 2}, 1800) && r.owner_end[1][1] == 2, "late old owner did not recover");
    }
    // H5) a guest never hears the decree: the host cancels it after ~3 s for everybody, the port is slot 0's again, the game goes on (a short stall, no desync)
    {
        Run r; r.join_at[0] = -1; r.handoff = true; r.resyncs = 0; r.frames = 1500; r.deaf_ho[1] = true;
        r.ho = {{300, 0, 1}};
        run(r);
        printf("decree nobody acknowledges: frames %u/%u cancelled %d/%d owner %d/%d, stalled ms %llu/%llu desync %d%d\n", r.frame[0], r.frame[1], r.ho_rev[0], r.ho_rev[1], r.owner_end[0][0], r.owner_end[1][0],
               (unsigned long long)r.stalled_ms[0], (unsigned long long)r.stalled_ms[1], r.desync[0], r.desync[1]);
        CHECK(r.ho[0].ok && r.ho_rev[0] == 1 && r.owner_end[0][0] == 0 && r.owner_end[1][0] == 0, "the decree was not cancelled everywhere");
        CHECK(converged(r, {0, 1}, 1500), "the game diverged after a cancelled hand-off");
        CHECK(r.stalled_ms[0] < 5000 && r.stalled_ms[1] < 5000, "stalled %llu / %llu ms", (unsigned long long)r.stalled_ms[0], (unsigned long long)r.stalled_ms[1]);
    }
    // H6) refusals
    {
        Run r; r.join_at[0] = -1; r.handoff = true; r.resyncs = 0; r.frames = 900;
        r.ho = {{150, 0, 0, -1, false}, {160, 3, 1, -1, false}, {170, 0, 5, -1, false}, {180, 0, 1, 1, false}, {300, 0, 1, -1, false}, {301, 1, 0, -1, false}};
        run(r);
        printf("refusals: to==from %d, no such port %d, no such player %d, not the owner %d, ok %d, busy %d (reasons %d %d %d %d %d)\n", r.ho[0].ok, r.ho[1].ok, r.ho[2].ok, r.ho[3].ok, r.ho[4].ok, r.ho[5].ok,
               r.ho[0].reason, r.ho[1].reason, r.ho[2].reason, r.ho[3].reason, r.ho[5].reason);
        CHECK(!r.ho[0].ok && r.ho[0].reason == rb::kHoBadTarget, "to == from");
        CHECK(!r.ho[1].ok && r.ho[1].reason == rb::kHoBadPort, "port 3 does not exist in a 2P game");
        CHECK(!r.ho[2].ok && r.ho[2].reason == rb::kHoBadTarget, "slot 5 is not a player");
        CHECK(!r.ho[3].ok && r.ho[3].reason == rb::kHoNotOwner, "slot 1 does not own port 0");
        CHECK(r.ho[4].ok, "the valid one (reason %d)", r.ho[4].reason);
        CHECK(!r.ho[5].ok && r.ho[5].reason == rb::kHoBusy, "a second decree while one is in flight");
        CHECK(converged(r, {0, 1}, 900) && r.ho_n[0] == 1 && r.ho_n[1] == 1, "refused requests disturbed the game");
    }
    // H7) a player without the capability (an old RetroHub) or a host with the feature off: refused up front, the game is untouched
    {
        Run a; a.join_at[0] = -1; a.handoff = true; a.old_ho = 1; a.resyncs = 0; a.frames = 700; a.ho = {{300, 0, 1, -1, false}};
        run(a);
        Run b; b.join_at[0] = -1; b.handoff = false; b.resyncs = 0; b.frames = 700; b.ho = {{300, 0, 1, -1, false}};
        run(b);
        printf("no capability: old peer -> reason %d, feature off -> reason %d\n", a.ho[0].reason, b.ho[0].reason);
        CHECK(!a.ho[0].ok && a.ho[0].reason == rb::kHoOldPeer && a.ho_n[0] == 0 && converged(a, {0, 1}, 700), "old peer");
        CHECK(!b.ho[0].ok && b.ho[0].reason == rb::kHoDisabled && b.ho_n[0] == 0 && converged(b, {0, 1}, 700), "feature off");
    }
    // H8) the player that passed its port crashes later: the survivors play on (the dead slot's cutoff changes no frame); then the NEW owner crashes: its port is neutral from the cutoff
    {
        Run r; r.join_at[0] = -1; r.players = 3; r.handoff = true; r.resyncs = 0; r.frames = 1600; r.ho = {{300, 2, 1}}; r.kill_at[2] = 700;
        run(r);
        int cmp = 0; const int bad = check_pads(r, 3, true, &cmp);
        printf("old owner crashes after passing its port: frames %u/%u desync %d%d dropped 0x%x/0x%x pads bad %d of %d\n", r.frame[0], r.frame[1], r.desync[0], r.desync[1], r.dropped_mask[0], r.dropped_mask[1], bad, cmp);
        CHECK(r.ho[0].ok && converged(r, {0, 1}, 1600) && bad == 0 && (r.dropped_mask[0] >> 2 & 1u), "survivors after the old owner crashed");
        Run q; q.join_at[0] = -1; q.players = 3; q.handoff = true; q.resyncs = 0; q.frames = 1600; q.ho = {{300, 0, 2}}; q.kill_at[2] = 700;
        run(q);
        int cmp2 = 0; const int bad2 = check_pads(q, 3, false, &cmp2);
        printf("new owner crashes: frames %u/%u desync %d%d dropped 0x%x/0x%x pads bad %d of %d\n", q.frame[0], q.frame[1], q.desync[0], q.desync[1], q.dropped_mask[0], q.dropped_mask[1], bad2, cmp2);
        CHECK(q.ho[0].ok && converged(q, {0, 1}, 1600) && bad2 == 0 && (q.dropped_mask[0] >> 2 & 1u), "survivors after the new owner crashed");
    }
    // H9) desync recovery with hand-offs in the history and in flight: a damaged guest state is healed by the host's, the ownership stays
    for (int corrupt : {500, 305}) {
        Run r; r.join_at[0] = -1; r.handoff = true; r.resyncs = 3; r.frames = 1600; r.corrupt_at = corrupt; r.hash_iv = corrupt == 305 ? 8 : 60; r.ho = {{300, 0, 1}, {900, 0, 0, 1}};
        run(r);
        int cmp = 0; const int bad = check_pads(r, 2, true, &cmp);
        printf("damaged state at frame %d around hand-offs: resyncs %u/%u, frames %u/%u, owners %d/%d, pads bad %d of %d\n", corrupt, r.resyncs_done[0], r.resyncs_done[1], r.frame[0], r.frame[1], r.owner_end[0][0], r.owner_end[1][0], bad, cmp);
        CHECK(r.ho[0].ok && r.ho[1].ok && r.resyncs_done[1] >= 1 && converged(r, {0, 1}, 1600), "no recovery (corrupt at %d)", corrupt);
        CHECK(r.owner_end[0][0] == 0 && r.owner_end[1][0] == 0 && r.ho_n[1] == 2, "ownership lost in the resync");
    }
    printf(g_fail ? "DROPIN TEST FAILED (%d)\n" : "DROPIN TEST PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
