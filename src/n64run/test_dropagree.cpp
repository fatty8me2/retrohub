// Console test: mid-match guest drop agreement (drop_agree.h) and its use by LockstepCore.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "drop_agree.h"
#include "lockstep_core.h"

using namespace n64run;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); g_fail++; } } while (0)

static void test_agreement() {
    // three survivors 0, 2, 3 with proposals 90, 95, 93: cutoff is the max (95) once everyone is heard
    netdrop::Agreement a(1, 0b1101, 0, 90), b(1, 0b1101, 2, 95), c(1, 0b1101, 3, 93);
    CHECK(!a.decided() && a.window_hi() == 90);
    a.on_msg(2, 95, 0b0100);
    CHECK(!a.decided());
    a.on_msg(3, 93, 0b1000);
    CHECK(a.decided() && a.cutoff() == 95 && a.window_hi() == 95);
    CHECK(!a.settled());
    b.on_msg(0, 90, 1); b.on_msg(3, 93, 8);
    c.on_msg(0, 90, 1); c.on_msg(2, 95, 4);
    CHECK(b.cutoff() == 95 && c.cutoff() == 95);
    // everybody reports hearing all three: settled
    a.on_msg(2, 95, 0b1101); a.on_msg(3, 93, 0b1101);
    CHECK(a.settled());
    // a survivor dies before proposing: the rest decide without it
    netdrop::Agreement d(1, 0b1101, 0, 10);
    d.on_msg(2, 12, 0b0101);
    CHECK(!d.decided());
    d.remove_survivor(3);
    CHECK(d.decided() && d.cutoff() == 12);
    // codec
    netdrop::Msg m; m.from = 2; m.dead = 1; m.proposal = 77; m.heard = 5; m.first = 70;
    netdrop::PortWin w; w.port = 1; for (int i = 0; i < 7; i++) w.vals.push_back(0x100000000ull * i + i);
    m.ports.push_back(w);
    uint8_t buf[512];
    size_t n = netdrop::encode(11, m, buf, sizeof buf);
    CHECK(n > 0);
    netdrop::Msg r;
    CHECK(netdrop::decode(11, buf, n, &r));
    CHECK(r.from == 2 && r.dead == 1 && r.proposal == 77 && r.heard == 5 && r.first == 70 && r.ports.size() == 1 && r.ports[0].vals == w.vals);
    CHECK(!netdrop::decode(11, buf, n - 1, &r));
    CHECK(!netdrop::decode(12, buf, n, &r));
}

struct Peer {
    LockstepCore core;
    explicit Peer(const LockstepCore::Config& c) : core(c) {}
};

static void test_core_drop(bool wide) {
    LockstepCore::Config cfg;
    cfg.owner[0] = 0; cfg.owner[1] = 1; cfg.owner[2] = 2;
    cfg.num_slots = 3; cfg.delay = 2; cfg.wide = wide;
    LockstepCore::Config c0 = cfg, c1 = cfg, c2 = cfg;
    c0.my_slot = 0; c1.my_slot = 1; c2.my_slot = 2;
    LockstepCore p0(c0), p1(c1), p2(c2);
    const int K = 60;
    uint8_t early[1100]; size_t early_n = 0;
    for (int n = 0; n < K; n++) {
        uint32_t b[kPorts] = {(uint32_t)(0x100 + n), (uint32_t)(0x200 + n), (uint32_t)(0x300 + n), 0};
        uint32_t ax[kPorts] = {(uint32_t)n, (uint32_t)(n * 3), (uint32_t)(n * 5), 0};
        p1.schedule_local(n, b, ax);
        uint8_t pk[1100];
        size_t pn = p1.build_packet(pk, sizeof pk);
        p0.on_packet(1, pk, pn);
        if (n <= K - 6) { p2.on_packet(1, pk, pn); early_n = pn; std::memcpy(early, pk, pn); }   // the last packet slot 2 ever receives
    }
    uint8_t late[1100];
    size_t late_n = p1.build_packet(late, sizeof late);   // slot 0 receives everything
    CHECK(early_n && late_n);
    uint32_t pr0 = p0.begin_drop(1), pr2 = p2.begin_drop(1);
    CHECK(pr0 > pr2);
    netdrop::Agreement a0(1, 0b101, 0, pr0), a2(1, 0b101, 2, pr2);
    for (int round = 0; round < 2; round++) {
        uint8_t buf[1100];
        netdrop::Msg m, d;
        m.from = 0; m.dead = 1; m.proposal = a0.my_proposal(); m.heard = a0.heard_mask();
        p0.fill_drop_window(1, a0.window_hi(), &m);
        size_t n = netdrop::encode(kPktDrop, m, buf, sizeof buf);
        CHECK(netdrop::decode(kPktDrop, buf, n, &d));
        p2.apply_drop_window(d); a2.on_msg(0, d.proposal, d.heard);
        netdrop::Msg m2;
        m2.from = 2; m2.dead = 1; m2.proposal = a2.my_proposal(); m2.heard = a2.heard_mask();
        p2.fill_drop_window(1, a2.window_hi(), &m2);
        n = netdrop::encode(kPktDrop, m2, buf, sizeof buf);
        CHECK(netdrop::decode(kPktDrop, buf, n, &d));
        p0.apply_drop_window(d); a0.on_msg(2, d.proposal, d.heard);
    }
    CHECK(a0.decided() && a2.decided() && a0.cutoff() == a2.cutoff() && a0.cutoff() == pr0);
    p0.finish_drop(1, a0.cutoff());
    p2.finish_drop(1, a2.cutoff());
    uint32_t cut = a0.cutoff();
    CHECK(cut == (uint32_t)K + 2);   // slot 1 scheduled samples up to K-1+D
    for (uint32_t n = 2; n < cut + 20; n++) {
        CHECK(p0.get(n, 1) == p2.get(n, 1));
        CHECK(p0.get_aux(n, 1) == p2.get_aux(n, 1));
        if (n >= cut) CHECK(p0.get(n, 1) == 0 && p2.get(n, 1) == 0);
        else CHECK(p2.get(n, 1) == 0x200u + (n - 2));
    }
    CHECK(p2.drop_done(1) && !(p2.slot_mask() >> 1 & 1u));
    p2.on_packet(1, late, late_n);   // a straggler from the dropped slot changes nothing
    CHECK(p2.get(cut + 1, 1) == 0);
}

// --- clean quit and kick policy (netcore-M7, netcore-M2) ---
static void test_policy() {
    // 2 players: the other one quitting still ends the match
    CHECK(!bye_becomes_drop(1, 0, 0b11, 0));
    CHECK(!bye_becomes_drop(0, 1, 0b11, 0));
    // 3 players: a guest quits -> drop; the host quits -> match ends
    CHECK(bye_becomes_drop(1, 0, 0b111, 0));
    CHECK(bye_becomes_drop(2, 1, 0b111, 0));
    CHECK(!bye_becomes_drop(0, 1, 0b111, 0));
    CHECK(!bye_becomes_drop(0, 2, 0b111, 0));
    // 3 players, both guests quit: the second one leaves the host alone, so the match ends
    CHECK(!bye_becomes_drop(2, 0, 0b111, 0b010));
    // 4 players, two guests quit at once: both drop, play goes on with host + one guest
    CHECK(bye_becomes_drop(1, 0, 0b1111, 0));
    CHECK(bye_becomes_drop(2, 0, 0b1111, 0b0010));
    CHECK(!bye_becomes_drop(3, 0, 0b1111, 0b0110));   // the last guest
    // a bye from a player already being dropped changes nothing
    CHECK(bye_becomes_drop(1, 0, 0b111, 0b010));
    // never about myself
    CHECK(!bye_becomes_drop(1, 1, 0b111, 0));
    // kick: host's request starts a drop; a guest's only joins one already begun
    CHECK(drop_msg_allowed(0, false, 0, 3000));        // host kick works even if I hear the victim
    CHECK(drop_msg_allowed(0, false, 10, 3000));
    CHECK(!drop_msg_allowed(2, false, 0, 3000));       // guest kick of someone I never lost: ignored
    CHECK(!drop_msg_allowed(2, false, 100, 3000));     // ... or hear fine
    CHECK(!drop_msg_allowed(2, false, 1499, 3000));
    CHECK(drop_msg_allowed(2, false, 1500, 3000));     // I have not heard the victim for half the limit: join the drop
    CHECK(drop_msg_allowed(2, true, 0, 3000));         // a drop I already began
    CHECK(!drop_msg_allowed(2, false, 5000, 0));       // silence drops switched off: a guest cannot start one
    // forged Ready: the slot in the packet must be the sender's
    const uint8_t rd[4] = {kPktReady, 2, 0, 0};
    CHECK(ready_slot_ok(2, rd, 4));
    CHECK(!ready_slot_ok(1, rd, 4));
    CHECK(!ready_slot_ok(-1, rd, 4));
    CHECK(!ready_slot_ok(2, rd, 1));
}

// A guest proposes a huge cutoff for a victim an honest peer is already dropping: it is clamped.
static void test_clamp() {
    netdrop::Agreement a(1, 0b101, 0, 100);
    a.on_msg(2, 0xFFFFFFFFu, 0b101);
    CHECK(a.decided() && a.cutoff() == 100 + netdrop::kMaxAhead);
    netdrop::Agreement b(1, 0b101, 0, 100);   // an honest proposal inside the bound is untouched
    b.on_msg(2, 120, 0b101);
    CHECK(b.cutoff() == 120);
    netdrop::Agreement c(1, 0b101, 0, 0xFFFFFFF0u);   // no wrap-around near the top
    c.on_msg(2, 0xFFFFFFFFu, 0b101);
    CHECK(c.cutoff() == 0xFFFFFFFFu);
}

// "One survivor still hears the victim": guest 2 lost slot 1, host 0 still hears it. The host starts nothing on
// its own; it must accept guest 2's request once its own silence timer is half-way, and then both agree one cutoff.
static void test_partial_loss() {
    const uint32_t silence = 3000;
    CHECK(!drop_msg_allowed(2, false, 40, silence));      // host hears slot 1 right now: a kick is refused
    CHECK(drop_msg_allowed(2, false, 1600, silence));     // slot 1 really went quiet for the host too: join
    LockstepCore::Config cfg;
    cfg.owner[0] = 0; cfg.owner[1] = 1; cfg.owner[2] = 2; cfg.num_slots = 3; cfg.delay = 2;
    LockstepCore::Config c0 = cfg, c2 = cfg; c0.my_slot = 0; c2.my_slot = 2;
    LockstepCore p0(c0), p2(c2);
    uint32_t pr2 = p2.begin_drop(1);   // guest 2 began first
    netdrop::Agreement a2(1, 0b101, 2, pr2);
    uint32_t pr0 = p0.begin_drop(1);   // host joined on guest 2's message
    netdrop::Agreement a0(1, 0b101, 0, pr0);
    a0.on_msg(2, a2.my_proposal(), a2.heard_mask());
    a2.on_msg(0, a0.my_proposal(), a0.heard_mask());
    CHECK(a0.decided() && a2.decided() && a0.cutoff() == a2.cutoff());
}

// Simulated mesh: N players play K samples; the `leavers` stop (their last packet reaches everybody); the others
// start a drop for each as the policy says, trade drop messages, and must agree on one cutoff and stay in step.
static void sim_leave(int n, uint32_t leavers) {
    LockstepCore::Config cfg;
    for (int i = 0; i < n; i++) cfg.owner[i] = i;
    cfg.num_slots = n; cfg.delay = 2;
    std::vector<LockstepCore*> p;
    for (int i = 0; i < n; i++) { LockstepCore::Config c = cfg; c.my_slot = i; p.push_back(new LockstepCore(c)); }
    const int K = 40;
    for (int s = 0; s < K; s++) {
        for (int i = 0; i < n; i++) {
            uint32_t b[kPorts] = {0, 0, 0, 0}, ax[kPorts] = {0, 0, 0, 0};
            b[i] = 0x100u * (i + 1) + s;
            p[i]->schedule_local(s, b, ax);
            uint8_t pk[1100];
            size_t pn = p[i]->build_packet(pk, sizeof pk);
            for (int j = 0; j < n; j++) if (j != i) p[j]->on_packet(i, pk, pn);
        }
    }
    uint32_t all = (1u << n) - 1;
    std::vector<int> surv;
    for (int i = 0; i < n; i++) if (!(leavers >> i & 1u)) surv.push_back(i);
    // every survivor processes the byes (in slot order) through the policy
    std::vector<std::vector<netdrop::Agreement>> ag(n);
    std::vector<std::vector<int>> dead_of(n);
    bool ends = false;
    for (int i : surv) {
        uint32_t g = 0;
        for (int l = 0; l < n; l++) {
            if (!(leavers >> l & 1u)) continue;
            if (!bye_becomes_drop(l, i, all, g)) { ends = true; continue; }
            uint32_t pr = p[i]->begin_drop(l);
            uint32_t sv = all & ~leavers;
            ag[i].push_back(netdrop::Agreement(l, sv, i, pr));
            dead_of[i].push_back(l);
            g |= 1u << l;
        }
    }
    bool host_left = leavers & 1u;
    CHECK(ends == (host_left || surv.size() < 2));
    if (ends) return;   // the match ends (host left / nobody else): the old behaviour, checked by test_policy
    for (int round = 0; round < 4; round++)
        for (int i : surv)
            for (size_t k = 0; k < ag[i].size(); k++) {
                netdrop::Msg m; m.from = (uint8_t)i; m.dead = (uint8_t)ag[i][k].dead();
                m.proposal = ag[i][k].my_proposal(); m.heard = ag[i][k].heard_mask();
                p[i]->fill_drop_window(m.dead, ag[i][k].window_hi(), &m);
                uint8_t buf[1100];
                size_t len = netdrop::encode(kPktDrop, m, buf, sizeof buf);
                for (int j : surv) {
                    if (j == i) continue;
                    netdrop::Msg d;
                    CHECK(netdrop::decode(kPktDrop, buf, len, &d));
                    for (size_t q = 0; q < ag[j].size(); q++) if (ag[j][q].dead() == (int)d.dead) { p[j]->apply_drop_window(d); ag[j][q].on_msg(i, d.proposal, d.heard); }
                }
            }
    for (int i : surv)
        for (size_t k = 0; k < ag[i].size(); k++) {
            CHECK(ag[i][k].decided());
            p[i]->finish_drop(ag[i][k].dead(), ag[i][k].cutoff());
        }
    int a = surv[0];
    for (int i : surv) {
        for (int l = 0; l < n; l++) if (leavers >> l & 1u) {
            CHECK(p[i]->drop_done(l));
            for (uint32_t s = 2; s < (uint32_t)K + 10; s++) CHECK(p[i]->get(s, l) == p[a]->get(s, l));
            CHECK(p[i]->get(K + 5, l) == 0);
        }
        // survivors still see each other's ports unchanged
        for (int j : surv) CHECK(p[i]->get(10, j) == 0x100u * (j + 1) + 8);
    }
    for (auto* c : p) delete c;
}

static void test_sims() {
    sim_leave(3, 0b100);    // 3 players: a guest quits
    sim_leave(3, 0b010);
    sim_leave(4, 0b0100);   // 4 players: a guest quits
    sim_leave(4, 0b1010);   // two guests quit at once
    sim_leave(3, 0b001);    // 3 players: the host quits -> ends (no drop)
    sim_leave(4, 0b0001);   // 4 players: the host quits -> ends
    sim_leave(2, 0b10);     // 2 players: the other quits -> ends
}

int main() {
    test_policy();
    test_clamp();
    test_partial_loss();
    test_sims();
    test_agreement();
    test_core_drop(false);
    test_core_drop(true);
    if (g_fail) { std::printf("dropagree: %d FAILED\n", g_fail); return 1; }
    std::printf("dropagree: all passed\n");
    return 0;
}
