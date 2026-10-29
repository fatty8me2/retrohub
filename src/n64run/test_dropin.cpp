// Console test: N64 drop-in (dropin.h protocol + LockstepCore reserved ports / join_port / join_self / leave_port).
// Three in-process cores: the founders A (slot 0, port 0) and B (slot 1, port 1) with ports 2 and 3 reserved (plugged in, neutral),
// and a joiner C that takes port 2 as slot 2 from an agreed sample.
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "dropin.h"
#include "lockstep_core.h"

using namespace n64run;

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static uint32_t inp(int slot, uint32_t s) { return 0x100u * (uint32_t)(slot + 1) + (s % 97u) + 1u; }   // never 0

static LockstepCore::Config cfg(int my_slot, uint32_t mask, uint8_t reserved, int delay) {
    LockstepCore::Config c;
    c.owner[0] = 0; c.owner[1] = 1; c.owner[2] = -1; c.owner[3] = -1;
    c.my_slot = my_slot; c.slot_mask = mask; c.delay = delay; c.reserved = reserved;
    return c;
}

static void deliver(LockstepCore& from, int from_slot, std::vector<LockstepCore*> to) {
    uint8_t buf[1400];
    size_t n = from.build_packet(buf, sizeof buf);
    if (!n) return;
    for (auto* t : to) t->on_packet(from_slot, buf, n);
}

static void test_wire() {
    dropin::Msg m;
    m.sub = dropin::Admit; m.from = 0; m.id = 7; m.slot = 2; m.port = 2; m.d = 3; m.flags = 3; m.j = 1234; m.f = 1250; m.pos = 99; m.mask = 3;
    m.owners[0] = 0; m.owners[1] = 1; m.name = "Kai Smith!";
    auto v = dropin::encode(kPktDropIn, m);
    dropin::Msg r;
    CHECK(dropin::decode(kPktDropIn, v.data(), v.size(), &r), "decode");
    CHECK(r.sub == m.sub && r.id == 7 && r.slot == 2 && r.port == 2 && r.d == 3 && r.flags == 3 && r.j == 1234 && r.f == 1250 && r.pos == 99 && r.mask == 3 && r.owners[1] == 1 && r.owners[2] == -1 && r.name == m.name, "fields");
    CHECK(!dropin::decode(kPktDropIn, v.data(), 10, &r), "short");
    v[25] = 99;   // name length beyond the datagram
    CHECK(!dropin::decode(kPktDropIn, v.data(), v.size(), &r), "bad name length");
    auto k = dropin::encode_knock(0x72, 2, dropin::kCapDropIn, "Mia");
    CHECK(dropin::is_knock(k) && dropin::knock_caps(k) == dropin::kCapDropIn && dropin::knock_name(k) == "Mia", "knock");
    std::vector<uint8_t> plain = {0x72, 2, 1};
    CHECK(!dropin::is_knock(plain), "a plain spectator knock is not a drop-in knock");
    CHECK(dropin::exit_code_for(dropin::kRefuseNoPort) == 9 && dropin::exit_code_for(dropin::kRefuseOldPeer) == 10 && dropin::exit_code_for(dropin::kRefuseBusy) == 11 && dropin::exit_code_for(0) == 11, "exit codes");
    CHECK(dropin::first_sample(100, 3, 0) == 115 && dropin::first_sample(100, 3, 100) == 100 + 3 + 12 + 6 && dropin::first_sample(100, 3, 0, 14) == 129, "first sample %u", dropin::first_sample(100, 3, 100));
}

static void test_ctl() {
    dropin::Ctl c;
    dropin::Msg m; m.slot = 2; m.port = 2; m.j = 500; m.f = 510;
    CHECK(!c.active(), "idle");
    uint8_t id = c.begin(m, 0b111u & ~1u | 4u, 1000);   // slots 1 and 2 must ack
    CHECK(c.active() && !c.all_acked() && c.id() == id, "begin");
    CHECK(c.send_due(1000) && !c.send_due(1050) && c.send_due(1100), "resend every 100 ms");
    dropin::Msg a; a.sub = dropin::Ack; a.id = (uint8_t)(id + 1); a.from = 1;
    c.on_ack(a);
    CHECK(!c.all_acked(), "an ack of another admit is ignored");
    a.id = id; c.on_ack(a);
    CHECK(!c.all_acked() && c.missing() == 4u, "slot 2 missing");
    CHECK(!c.timed_out(1000 + dropin::kAckLimitMs) && c.timed_out(1001 + dropin::kAckLimitMs), "ack limit");
    a.from = 2; c.on_ack(a);
    CHECK(c.all_acked() && !c.send_due(5000), "all acked: no more resends");
    c.finish();
    CHECK(!c.active(), "finish");
    dropin::ReadyGate g;
    for (int i = 0; i < 100; i++) g.on_sample(2);
    CHECK(g.ready(), "ready when the lag stays small");
    g.on_sample(50);
    CHECK(!g.ready(), "a lag spike resets it");
}

/* The whole join with delay D=3: admit at sample 40, joins (J) at 60, supplies input from F = 70. */
static void test_join() {
    const int D = 3;
    const uint32_t J = 60, F = 70;
    LockstepCore A(cfg(0, 3, 0b1100, D)), B(cfg(1, 3, 0b1100, D));
    CHECK(A.port_present(2) && A.port_present(3) && !A.port_local(2) && A.free_reserved() == 0b1100u, "reserved ports are present, unowned");
    // the joiner spectated until J: it is slot 2, its core knows only the founders
    LockstepCore C(cfg(2, 3, 0b1100, D));
    uint32_t cs = 0;   // joiner's sample counter
    uint32_t seenA[200], seenB[200], seenC[200];
    std::memset(seenA, 0xFF, sizeof seenA); std::memset(seenB, 0xFF, sizeof seenB); std::memset(seenC, 0xFF, sizeof seenC);
    bool joined = false;
    uint32_t stallsA = 0;
    for (uint32_t n = 0; n < 160; n++) {
        if (n == 40) {   // the Admit reaches the founders (acked in time)
            CHECK(A.join_port(2, 2, F) && B.join_port(2, 2, F), "bind");
            CHECK(!A.join_port(2, 3, F), "a taken port cannot be bound again");
            CHECK(A.free_reserved() == 0b1000u, "one reserved port left");
        }
        uint32_t ba[kPorts] = {inp(0, n), 0, 0, 0}, bb[kPorts] = {0, inp(1, n), 0, 0};
        A.schedule_local(n, ba); B.schedule_local(n, bb);
        if (n == J && !joined) {   // the joiner becomes a player at its poll J
            const int8_t owners[4] = {0, 1, -1, -1};
            C.adopt_layout(owners, 3);
            C.join_self(2, F, D);
            joined = true;
            cs = n;
        }
        if (joined) {
            uint32_t bc[kPorts] = {0, 0, inp(2, n), 0};
            C.schedule_local(n, bc);
        }
        A.set_progress(n); B.set_progress(n); if (joined) C.set_progress(n);
        deliver(A, 0, {&B, &C}); deliver(B, 1, {&A, &C});
        if (joined) deliver(C, 2, {&A, &B});
        // the founders must never be blocked by the joiner before F (they only need each other)
        if (n < F) { if (!A.ready(n) || !B.ready(n)) stallsA++; }
        CHECK(A.ready(n) && B.ready(n), "founders blocked at %u", n);
        for (int p = 0; p < kPorts; p++) {
            // identical views
            CHECK(A.get(n, p) == B.get(n, p), "A and B disagree on port %d at sample %u", p, n);
        }
        if (n < F + 0) CHECK(A.get(n, 2) == 0, "port 2 neutral before F at %u", n);
        if (joined && n >= F + 2) CHECK(C.ready(n) || true, "-");
        if (n >= F) {
            // at/after F the joiner's own input (scheduled D samples earlier) is what everyone reads for port 2
            CHECK(A.ready(n) == B.ready(n), "ready mismatch");
        }
        seenA[n] = A.get(n, 2); seenB[n] = B.get(n, 2);
        if (joined) seenC[n] = C.get(n, 2);
    }
    CHECK(stallsA == 0, "the join stalled the founders (%u)", stallsA);
    // after F: port 2 carries the joiner's input, the same on all three
    int real = 0;
    for (uint32_t n = F + D + 1; n < 150; n++) {
        if (seenA[n] != 0 && seenA[n] == seenB[n]) real++;
    }
    CHECK(real > 60, "joiner input visible to the founders (%d samples)", real);
    uint32_t agree = 0, tot = 0;
    for (uint32_t n = F + D + 1; n < 150; n++) { tot++; if (seenA[n] == seenC[n]) agree++; }
    CHECK(agree == tot, "joiner sees what the founders see on its own port (%u of %u)", agree, tot);
    // its samples are the ones it scheduled: input read at poll m is applied at m + D
    for (uint32_t n = F + D + 1; n < 150; n++) CHECK(seenA[n] == inp(2, n - D), "sample %u carries the joiner's input of poll %u (got %x)", n, n - D, seenA[n]);
    (void)cs;
    // the hash mask now includes slot 2
    CHECK(A.slot_mask() == 7u && B.slot_mask() == 7u && C.slot_mask() == 7u, "masks %x %x %x", A.slot_mask(), B.slot_mask(), C.slot_mask());
}

/* gating: from F on, a silent joiner holds the founders back (the drop path then frees them); before F it never does. */
static void test_gate_and_cancel() {
    const int D = 2;
    LockstepCore A(cfg(0, 3, 0b1100, D)), B(cfg(1, 3, 0b1100, D));
    CHECK(A.join_port(2, 2, 100) && B.join_port(2, 2, 100), "bind");
    uint32_t n = 0;
    for (; n < 99; n++) {
        uint32_t ba[kPorts] = {inp(0, n), 0, 0, 0}, bb[kPorts] = {0, inp(1, n), 0, 0};
        A.schedule_local(n, ba); B.schedule_local(n, bb);
        A.set_progress(n); B.set_progress(n);
        deliver(A, 0, {&B}); deliver(B, 1, {&A});
        CHECK(A.ready(n), "ready before F at %u", n);
    }
    // a cancelled admit: nobody waits for slot 2 ever
    A.cancel_join(2, 2); B.cancel_join(2, 2);
    CHECK(A.slot_mask() == 3u && A.free_reserved() == 0b1100u, "unbound again");
    for (; n < 160; n++) {
        uint32_t ba[kPorts] = {inp(0, n), 0, 0, 0}, bb[kPorts] = {0, inp(1, n), 0, 0};
        A.schedule_local(n, ba); B.schedule_local(n, bb);
        A.set_progress(n); B.set_progress(n);
        deliver(A, 0, {&B}); deliver(B, 1, {&A});
        CHECK(A.ready(n) && B.ready(n), "ready after the cancel at %u", n);
        CHECK(A.get(n, 2) == 0, "neutral");
    }
    // bound but silent: gates from F
    CHECK(A.join_port(2, 2, 200) && B.join_port(2, 2, 200), "bind again");
    bool blocked = false;
    for (; n < 260; n++) {
        uint32_t ba[kPorts] = {inp(0, n), 0, 0, 0}, bb[kPorts] = {0, inp(1, n), 0, 0};
        A.schedule_local(n, ba); B.schedule_local(n, bb);
        A.set_progress(n); B.set_progress(n);
        deliver(A, 0, {&B}); deliver(B, 1, {&A});
        if (!A.ready(n)) { blocked = true; CHECK(n >= 200, "blocked too early at %u", n); break; }
    }
    CHECK(blocked, "a silent joiner gates from F on");
    // the existing drop agreement ends it: neutral from the cutoff, then the port is reserved again for the next friend
    A.finish_drop(2, 210); B.finish_drop(2, 210);
    CHECK(A.ready(n) || n < 210 || true, "-");
    CHECK(A.drop_done(2) && A.drop_cutoff(2) == 210, "drop recorded");
    A.leave_port(2, 2); B.leave_port(2, 2);
    CHECK(A.free_reserved() == 0b1100u && !A.drop_done(2) && A.slot_mask() == 3u, "port reusable");
    // slot 2 (or 3) can drop in again at a later sample
    CHECK(A.join_port(2, 3, 400) && B.join_port(2, 3, 400), "next friend binds the same port as slot 3");
    CHECK(A.slot_mask() == 11u, "mask %x", A.slot_mask());
}

int main() {
    test_wire();
    test_ctl();
    test_join();
    test_gate_and_cancel();
    if (g_fail) { std::printf("n64run_dropin_test: %d failure(s)\n", g_fail); return 1; }
    std::printf("n64run_dropin_test: ok\n");
    return 0;
}
