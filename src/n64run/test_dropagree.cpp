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

int main() {
    test_agreement();
    test_core_drop(false);
    test_core_drop(true);
    if (g_fail) { std::printf("dropagree: %d FAILED\n", g_fail); return 1; }
    std::printf("dropagree: all passed\n");
    return 0;
}
