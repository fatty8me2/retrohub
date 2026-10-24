// Console test: simulates 2-3 lockstep peers in-process over a lossy, reordering, duplicating
// "network" and checks that every peer sees the identical input sequence. Also checks the BUTTONS
// bit layout constants against the real m64p_plugin.h.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <random>
#include <vector>

#include "buttons.h"
#include "lockstep_core.h"
#include "m64p_plugin.h"

using namespace n64run;

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static uint32_t local_input(int port, uint32_t sample) {
    // deterministic pseudo-random input per (port, sample-at-read-time)
    uint64_t x = (uint64_t)port * 0x9E3779B97F4A7C15ull + sample * 0xD1B54A32D192ED03ull + 12345;
    x ^= x >> 31; x *= 0x7FB5D329728EA185ull; x ^= x >> 27;
    return (uint32_t)x | 1u;   // never 0, so "neutral" is distinguishable
}

static void test_buttons() {
    BUTTONS b; std::memset(&b, 0, sizeof b);
    b.R_DPAD = 1;  CHECK(b.Value == BTN_R_DPAD, "R_DPAD");  b.Value = 0;
    b.L_DPAD = 1;  CHECK(b.Value == BTN_L_DPAD, "L_DPAD");  b.Value = 0;
    b.D_DPAD = 1;  CHECK(b.Value == BTN_D_DPAD, "D_DPAD");  b.Value = 0;
    b.U_DPAD = 1;  CHECK(b.Value == BTN_U_DPAD, "U_DPAD");  b.Value = 0;
    b.START_BUTTON = 1; CHECK(b.Value == BTN_START, "START"); b.Value = 0;
    b.Z_TRIG = 1;  CHECK(b.Value == BTN_Z, "Z");  b.Value = 0;
    b.B_BUTTON = 1; CHECK(b.Value == BTN_B, "B"); b.Value = 0;
    b.A_BUTTON = 1; CHECK(b.Value == BTN_A, "A"); b.Value = 0;
    b.R_CBUTTON = 1; CHECK(b.Value == BTN_C_RIGHT, "CR"); b.Value = 0;
    b.L_CBUTTON = 1; CHECK(b.Value == BTN_C_LEFT, "CL"); b.Value = 0;
    b.D_CBUTTON = 1; CHECK(b.Value == BTN_C_DOWN, "CD"); b.Value = 0;
    b.U_CBUTTON = 1; CHECK(b.Value == BTN_C_UP, "CU"); b.Value = 0;
    b.R_TRIG = 1; CHECK(b.Value == BTN_R, "R"); b.Value = 0;
    b.L_TRIG = 1; CHECK(b.Value == BTN_L, "L"); b.Value = 0;
    b.X_AXIS = -80; b.Y_AXIS = 79;
    CHECK(b.Value == pack_stick(-80, 79), "stick pack %08x vs %08x", b.Value, pack_stick(-80, 79));
}

static uint32_t g_mask = 0;   // 0 = all slots 0..npeers-1 active; else sparse slot set
static bool active(int i) { return g_mask == 0 || (g_mask >> i & 1u); }

struct Packet { int deliver_tick; int to; int from; std::vector<uint8_t> bytes; };

struct Net {
    std::mt19937 rng;
    double loss, dup;
    int max_delay;
    int tick = 0;
    std::vector<Packet> inflight;
    long sent = 0, dropped = 0;
    Net(uint32_t seed, double l, double d, int md) : rng(seed), loss(l), dup(d), max_delay(md) {}
    void send(int from, int to, const uint8_t* d, size_t n, int npeers) {
        for (int t = 0; t < npeers; t++) {
            if (t == from || (to >= 0 && t != to) || !active(t)) continue;
            sent++;
            std::uniform_real_distribution<double> u(0, 1);
            if (u(rng) < loss) { dropped++; continue; }
            int copies = u(rng) < dup ? 2 : 1;
            for (int c = 0; c < copies; c++) {
                int delay = (int)(rng() % (uint32_t)(max_delay + 1));
                inflight.push_back({tick + delay, t, from, std::vector<uint8_t>(d, d + n)});
            }
        }
    }
};

struct Peer {
    std::unique_ptr<LockstepCore> core;
    uint32_t n = 0;             // next sample to complete
    bool scheduled = false;     // inputs for poll n already scheduled/sent
    int last_send_tick = -100;
    std::vector<std::vector<uint32_t>> seen;   // [port][sample]
    std::vector<uint64_t> hashes_sent;
};

static void run_sim(const char* name, int npeers, const int owner[kPorts], int delay, double loss, double dup, int max_delay,
                    uint32_t samples, bool corrupt_hash, uint32_t seed, const std::vector<std::pair<uint32_t, int>>* orders = nullptr) {
    Net net(seed, loss, dup, max_delay);
    std::vector<Peer> peers(npeers);
    for (int i = 0; i < npeers; i++) {
        if (!active(i)) continue;
        LockstepCore::Config cfg;
        for (int p = 0; p < kPorts; p++) cfg.owner[p] = owner[p];
        cfg.my_slot = i; cfg.num_slots = npeers; cfg.slot_mask = g_mask; cfg.delay = delay;
        peers[i].core.reset(new LockstepCore(cfg));
        peers[i].seen.assign(kPorts, {});
    }
    int mismatches = 0;
    long ticks = 0;
    const long max_ticks = (long)samples * 200 + 100000;
    while (ticks < max_ticks) {
        net.tick = (int)ticks;
        // deliver (random order within a tick thanks to shuffled scan)
        for (size_t i = 0; i < net.inflight.size();) {
            if (net.inflight[i].deliver_tick <= net.tick) {
                Packet pk = std::move(net.inflight[i]);
                net.inflight[i] = std::move(net.inflight.back());
                net.inflight.pop_back();
                peers[pk.to].core->on_packet(pk.from, pk.bytes.data(), pk.bytes.size());
            } else i++;
        }
        bool all_done = true;
        std::vector<int> order(npeers);
        for (int i = 0; i < npeers; i++) order[i] = i;
        std::shuffle(order.begin(), order.end(), net.rng);
        for (int i : order) {
            if (!active(i)) continue;
            Peer& pe = peers[i];
            uint8_t buf[1100];
            if (pe.n >= samples) {   // finished peers keep re-announcing (as a live peer would until exit)
                if ((int)ticks - pe.last_send_tick >= 4) {
                    pe.core->set_progress(pe.n);
                    size_t len = pe.core->build_packet(buf, sizeof buf);
                    if (len) net.send(i, -1, buf, len, npeers);
                    pe.last_send_tick = (int)ticks;
                }
                continue;
            }
            all_done = false;
            if (!pe.scheduled) {
                uint32_t btn[kPorts] = {0, 0, 0, 0};
                for (int p = 0; p < kPorts; p++) if (pe.core->port_local(p)) btn[p] = local_input(p, pe.n);
                if (pe.n > 0 && pe.n % 600 == 0) {
                    uint64_t h = 0xABCDEF + pe.n;
                    if (corrupt_hash && i == (g_mask ? 2 : 1) && pe.n == 1200) h ^= 1;
                    pe.core->add_local_hash(pe.n, h);
                }
                if (orders && i == 0) for (auto& o : *orders) if (o.first == pe.n) pe.core->order_delay(o.second);
                pe.core->schedule_local(pe.n, btn);
                pe.scheduled = true;
                pe.core->set_progress(pe.n);
                size_t len = pe.core->build_packet(buf, sizeof buf);
                if (len) net.send(i, -1, buf, len, npeers);
                pe.last_send_tick = (int)ticks;
            }
            if (pe.core->ready(pe.n)) {
                for (int p = 0; p < kPorts; p++)
                    if (pe.core->port_present(p)) pe.seen[p].push_back(pe.core->get(pe.n, p));
                pe.n++;
                pe.scheduled = false;
            } else if ((int)ticks - pe.last_send_tick >= 4) {   // blocked: retransmit newest packet
                pe.core->set_progress(pe.n);
                size_t len = pe.core->build_packet(buf, sizeof buf);
                if (len) net.send(i, -1, buf, len, npeers);
                pe.last_send_tick = (int)ticks;
            }
            LockstepCore::Mismatch mm;
            while (pe.core->poll_mismatch(&mm)) mismatches++;
        }
        ticks++;
        if (all_done) break;
    }
    for (int i = 0; i < npeers; i++) if (active(i)) CHECK(peers[i].n == samples, "%s: peer %d stalled at %u/%u (deadlock)", name, i, peers[i].n, samples);
    // Identical sequences + equal to the expected function.
    long compared = 0;
    for (int p = 0; p < kPorts; p++) {
        if (owner[p] < 0) continue;
        for (uint32_t s = 0; s < samples; s++) {
            uint32_t expect = s < (uint32_t)delay ? 0u : local_input(p, s - delay);
            int ref = -1;
            for (int i = 0; i < npeers; i++) {
                if (!active(i) || s >= peers[i].seen[p].size()) continue;
                compared++;
                if (orders) {   // adaptive delay: all PCs must agree, and the value must be a recent input of the owner (hold/skip semantics)
                    if (ref < 0) {
                        ref = i;
                        bool okv = s < (uint32_t)delay;
                        if (s < (uint32_t)delay) okv = peers[i].seen[p][s] == 0u;
                        else for (uint32_t k = (s >= (uint32_t)kMaxDelay ? s - kMaxDelay : 0); k <= s && !okv; k++) okv = peers[i].seen[p][s] == local_input(p, k);
                        if (!okv) { CHECK(false, "%s: port %d sample %u value is not a recent input of its owner", name, p, s); goto next_port; }
                    } else if (peers[i].seen[p][s] != peers[ref].seen[p][s]) {
                        CHECK(false, "%s: peers %d and %d disagree on port %d sample %u", name, ref, i, p, s);
                        goto next_port;
                    }
                    continue;
                }
                if (peers[i].seen[p][s] != expect) {
                    CHECK(false, "%s: peer %d port %d sample %u got %08x want %08x", name, i, p, s, peers[i].seen[p][s], expect);
                    goto next_port;
                }
            }
        }
    next_port:;
    }
    int first_active = 0; while (!active(first_active)) first_active++;
    uint32_t hashes = peers[first_active].core->hashes_compared();
    if (corrupt_hash) CHECK(mismatches >= 1, "%s: hash corruption not detected", name);
    else CHECK(mismatches == 0, "%s: false hash mismatch (%d)", name, mismatches);
    std::printf("%-34s peers=%d D=%d loss=%2.0f%% dup=%2.0f%% reorder<=%d : %u samples, %ld values checked, %ld pkts (%ld dropped), %ld ticks, hashes compared=%u mism=%d  OK=%s\n",
                name, npeers, delay, loss * 100, dup * 100, max_delay, samples, compared, net.sent, net.dropped, ticks, hashes, mismatches,
                g_fail ? "NO" : "yes");
}

static void test_auto_delay_and_masks() {
    // D = ceil((rtt/2 + 8) / 16.67) + 1, clamped 1..10
    CHECK(auto_delay_from_rtt(0, 16.67) == 2, "rtt 0 -> %d", auto_delay_from_rtt(0, 16.67));
    CHECK(auto_delay_from_rtt(20, 16.67) == 3, "rtt 20 -> %d", auto_delay_from_rtt(20, 16.67));
    CHECK(auto_delay_from_rtt(60, 16.67) == 4, "rtt 60 -> %d", auto_delay_from_rtt(60, 16.67));
    CHECK(auto_delay_from_rtt(100, 16.67) == 5, "rtt 100 -> %d", auto_delay_from_rtt(100, 16.67));
    CHECK(auto_delay_from_rtt(160, 16.67) == 7, "rtt 160 -> %d", auto_delay_from_rtt(160, 16.67));
    CHECK(auto_delay_from_rtt(160, 16.67, true) == 8, "relay bump");
    CHECK(auto_delay_from_rtt(2000, 16.67) == 10, "clamp high");
    CHECK(auto_delay_from_rtt(-50, 16.67) >= 1, "clamp low");
    CHECK(auto_delay_from_rtt(100, 20.0) == 4, "pal 50fps rtt 100 -> %d", auto_delay_from_rtt(100, 20.0));
    CHECK(effective_rtt({}) == 0 && effective_rtt({40}) == 40 && effective_rtt({40, 90, 10}) == 130, "effective_rtt");
    std::printf("auto delay: rtt 20/60/100/160/relay160 -> %d/%d/%d/%d/%d\n", auto_delay_from_rtt(20, 16.67), auto_delay_from_rtt(60, 16.67),
                auto_delay_from_rtt(100, 16.67), auto_delay_from_rtt(160, 16.67), auto_delay_from_rtt(160, 16.67, true));

    std::string err;
    const int a[kPorts] = {0, 2, -1, -1};
    CHECK(derive_slot_mask(a, 2, 2, &err) == 0x5u, "sparse {0,2}: %s", err.c_str());
    CHECK(derive_slot_mask(a, 0, 2, &err) == 0x5u, "sparse {0,2} as host");
    CHECK(derive_slot_mask(a, 2, 1, &err) == 0 && !err.empty(), "players < PCs rejected");
    const int b[kPorts] = {1, 3, -1, -1};   // host owns no port
    CHECK(derive_slot_mask(b, 3, 3, &err) == 0xBu, "host without port + {1,3}: %x", derive_slot_mask(b, 3, 3, &err));
    const int c[kPorts] = {0, 1, -1, -1};
    CHECK(derive_slot_mask(c, 0, 3, &err) == 0x7u, "port-less spectator filled in");
    CHECK(derive_slot_mask(c, 4, 2, &err) == 0, "slot 4 rejected");
    const int d[kPorts] = {0, 5, -1, -1};
    CHECK(derive_slot_mask(d, 0, 2, &err) == 0, "port owner 5 rejected");
}

static void test_delay_adapter() {
    // Single stall is ignored; a second one within 3 s raises; quiet 12 s lowers one step toward the floor.
    DelayAdapter a(2);
    uint32_t t = 1000;
    for (int i = 0; i < 100; i++) { a.on_poll(t, 0); t += 16; }
    CHECK(a.want(3, t) == 3, "no stalls keeps D");
    a.on_poll(t, 40); t += 16;
    CHECK(a.want(3, t) == 3, "single stall ignored");
    for (int i = 0; i < 10; i++) { a.on_poll(t, 0); t += 16; }
    a.on_poll(t, 33); t += 16;
    int w = a.want(3, t);
    CHECK(w >= 4 && w <= 6, "two stalls raise (got %d)", w);
    CHECK(a.want(3, t + 100) == w, "raise request persists until it took effect");
    CHECK(a.want(w, t + 100) == w, "no change once in effect");
    int wants[4] = {2, 5, 3, 2};
    DelayAdapter h(2);
    CHECK(h.decide(3, wants, 0xF, 10000) == 5, "host orders the largest ask");
    int w2[4] = {2, 2, 2, 2};
    CHECK(h.decide(5, w2, 0xF, 10100) == 5, "host rate-limits changes");
    CHECK(h.decide(5, w2, 0xF, 20000) == 4, "everybody wants lower: one step down");
    int w3[4] = {4, 2, 2, 4};
    CHECK(h.decide(4, w3, 0xF, 40000) == 4, "one holdout keeps D");
    CHECK(h.decide(2, w2, 0xF, 60000) == 2, "never below floor");
    DelayAdapter q(2);
    t = 1000;
    for (int i = 0; i < 800; i++) { q.on_poll(t, 0); t += 16; }   // ~12.8 s quiet
    CHECK(q.want(5, t) == 4, "quiet lowers one step");
    {   // regression: long stalls (loads/hiccups) must not ratchet D; D stays under the cap and comes back down
        DelayAdapter r(3, DelayAdapter::cap_for_floor(3));
        CHECK(DelayAdapter::cap_for_floor(3) == 6 && DelayAdapter::cap_for_floor(10) == 10, "cap bound");
        uint32_t tt = 1000; int cur = 3;
        for (int i = 0; i < 40; i++) { r.on_poll(tt, 900); tt += 950; cur = r.want(cur, tt); }
        CHECK(cur == 3, "long stalls ignored (got %d)", cur);
        for (int round = 0; round < 60; round++) {   // constant jitter: may climb but never above the cap
            r.on_poll(tt, 30); tt += 50; r.on_poll(tt, 30); tt += 50; cur = r.want(cur, tt);
            tt += 1500;
        }
        CHECK(cur <= 6, "capped (got %d)", cur);
        for (int i = 0; i < 6 && cur > 3; i++) { for (int k = 0; k < 300; k++) { r.on_poll(tt, 0); tt += 16; } cur = r.want(cur, tt); }
        CHECK(cur == 3, "returns to floor after quiet (got %d)", cur);
    }
    std::printf("delay adapter tests done\n");
}

static void test_bye() {
    // A peer that said bye no longer gates progress.
    LockstepCore::Config c0, c1;
    for (int i = 0; i < kPorts; i++) c0.owner[i] = c1.owner[i] = i < 2 ? i : -1;
    c0.my_slot = 0; c1.my_slot = 1; c0.num_slots = c1.num_slots = 2; c0.delay = c1.delay = 2;
    LockstepCore a(c0), b(c1);
    uint32_t btn[kPorts] = {7, 9, 0, 0};
    uint8_t buf[1100];
    for (uint32_t n = 0; n < 50; n++) {
        a.schedule_local(n, btn); b.schedule_local(n, btn);
        a.set_progress(n); b.set_progress(n);
        size_t l = b.build_packet(buf, sizeof buf); a.on_packet(1, buf, l);
        l = a.build_packet(buf, sizeof buf); b.on_packet(0, buf, l);
    }
    CHECK(a.ready(40), "ready while peer alive");
    CHECK(!a.ready(200), "not ready beyond peer data");
    // a spectator-like gate: after bye the progress gate is gone, but missing data still blocks
    a.peer_left(1);
    CHECK(!a.ready(200), "missing data of a left peer still blocks");
    std::printf("bye tests done\n");
}

int main() {
    test_buttons();
    test_auto_delay_and_masks();
    const int own2[kPorts] = {0, 1, -1, -1};
    const int own3[kPorts] = {0, 1, 2, -1};
    const int own3b[kPorts] = {0, 0, 1, 2};   // slot 0 owns two ports
    const int own3c[kPorts] = {-1, 2, -1, 0}; // lead port owned by slot 2
    const uint32_t N = 10000;
    run_sim("2 peers, clean", 2, own2, 2, 0.0, 0.0, 0, N, false, 1);
    run_sim("2 peers, lossy+reorder", 2, own2, 2, 0.30, 0.10, 6, N, false, 2);
    run_sim("2 peers, delay 0", 2, own2, 0, 0.20, 0.05, 3, N, false, 3);
    run_sim("2 peers, delay 5 heavy loss", 2, own2, 5, 0.50, 0.10, 10, N, false, 4);
    run_sim("3 peers, lossy+reorder", 3, own3, 2, 0.30, 0.10, 6, N, false, 5);
    run_sim("3 peers, multi-port owner", 3, own3b, 3, 0.30, 0.10, 8, N, false, 6);
    run_sim("3 peers, lead port on slot 2", 3, own3c, 2, 0.25, 0.10, 5, N, false, 7);
    run_sim("3 peers, hash corruption", 3, own3, 2, 0.30, 0.10, 6, N, true, 8);
    const int own1[kPorts] = {0, 0, -1, -1};
    run_sim("1 peer (offline)", 1, own1, 0, 0.0, 0.0, 0, 2000, false, 9);
    /* Stress: full 4-PC sessions (every N64 port owned by a different PC). */
    const int own4[kPorts] = {0, 1, 2, 3};
    const int own4b[kPorts] = {0, 1, 2, 3};
    run_sim("4 peers, 15% loss, reorder", 4, own4, 2, 0.15, 0.05, 6, N, false, 10);
    run_sim("4 peers, 15% loss, 50k samples", 4, own4b, 2, 0.15, 0.05, 6, 50000, false, 11);
    run_sim("4 peers, D=8, 15% loss, reorder 12", 4, own4, 8, 0.15, 0.05, 12, N, false, 12);
    run_sim("4 peers, D=0, 15% loss, 50k", 4, own4, 0, 0.15, 0.10, 4, 50000, false, 13);
    run_sim("4 peers, 40% loss (stress)", 4, own4, 3, 0.40, 0.10, 8, N, false, 14);
    run_sim("4 peers, hash corruption", 4, own4, 2, 0.15, 0.05, 6, N, true, 15);
    /* Sparse slot sets: lobby slot 1 left, members {0,2} / {0,3} / {1,2,3}. */
    const int sp02[kPorts] = {0, 2, -1, -1};
    g_mask = 0x5u;
    run_sim("sparse {0,2}, lossy+reorder", 3, sp02, 2, 0.30, 0.10, 6, N, false, 20);
    run_sim("sparse {0,2}, hash corruption", 3, sp02, 2, 0.20, 0.05, 6, N, true, 21);   // peer 1 inactive: slot 1 corrupts nothing -> uses 0 and 2
    const int sp123[kPorts] = {1, 2, 3, -1};
    g_mask = 0xEu;
    run_sim("sparse {1,2,3}, no host port", 4, sp123, 2, 0.20, 0.05, 6, N, false, 22);
    const int sp03[kPorts] = {0, -1, -1, 3};
    g_mask = 0x9u;
    run_sim("sparse {0,3}, lead on 0", 4, sp03, 3, 0.20, 0.05, 6, N, false, 23);
    g_mask = 0;
    {   // adaptive delay: the host orders D changes mid-run (up, down, up by a lot) over a lossy reordering net
        std::vector<std::pair<uint32_t, int>> ord = {{500, 6}, {1500, 3}, {2500, 14}, {4000, 2}, {6000, 20}, {7000, 1}};
        run_sim("2 peers, D changes", 2, own2, 2, 0.20, 0.05, 6, N, false, 30, &ord);
        run_sim("4 peers, D changes", 4, own4, 3, 0.15, 0.05, 6, N, false, 31, &ord);
        run_sim("4 peers, D changes, heavy loss", 4, own4, 3, 0.40, 0.10, 8, N, false, 32, &ord);
    }
    test_delay_adapter();
    test_bye();
    std::printf(g_fail ? "FAILED (%d)\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
