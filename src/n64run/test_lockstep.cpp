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
            if (t == from || (to >= 0 && t != to)) continue;
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
                    uint32_t samples, bool corrupt_hash, uint32_t seed) {
    Net net(seed, loss, dup, max_delay);
    std::vector<Peer> peers(npeers);
    for (int i = 0; i < npeers; i++) {
        LockstepCore::Config cfg;
        for (int p = 0; p < kPorts; p++) cfg.owner[p] = owner[p];
        cfg.my_slot = i; cfg.num_slots = npeers; cfg.delay = delay;
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
                    if (corrupt_hash && i == 1 && pe.n == 1200) h ^= 1;
                    pe.core->add_local_hash(pe.n, h);
                }
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
    for (int i = 0; i < npeers; i++) CHECK(peers[i].n == samples, "%s: peer %d stalled at %u/%u (deadlock)", name, i, peers[i].n, samples);
    // Identical sequences + equal to the expected function.
    long compared = 0;
    for (int p = 0; p < kPorts; p++) {
        if (owner[p] < 0) continue;
        for (uint32_t s = 0; s < samples; s++) {
            uint32_t expect = s < (uint32_t)delay ? 0u : local_input(p, s - delay);
            for (int i = 0; i < npeers; i++) {
                if (s >= peers[i].seen[p].size()) continue;
                compared++;
                if (peers[i].seen[p][s] != expect) {
                    CHECK(false, "%s: peer %d port %d sample %u got %08x want %08x", name, i, p, s, peers[i].seen[p][s], expect);
                    goto next_port;
                }
            }
        }
    next_port:;
    }
    uint32_t hashes = peers[0].core->hashes_compared();
    if (corrupt_hash) CHECK(mismatches >= 1, "%s: hash corruption not detected", name);
    else CHECK(mismatches == 0, "%s: false hash mismatch (%d)", name, mismatches);
    std::printf("%-34s peers=%d D=%d loss=%2.0f%% dup=%2.0f%% reorder<=%d : %u samples, %ld values checked, %ld pkts (%ld dropped), %ld ticks, hashes compared=%u mism=%d  OK=%s\n",
                name, npeers, delay, loss * 100, dup * 100, max_delay, samples, compared, net.sent, net.dropped, ticks, hashes, mismatches,
                g_fail ? "NO" : "yes");
}

int main() {
    test_buttons();
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
    std::printf(g_fail ? "FAILED (%d)\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
