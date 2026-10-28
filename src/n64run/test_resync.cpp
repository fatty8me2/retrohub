// Console test: lockstep desync recovery agreement (resync.h) plus a simulated host -> guest state transfer with the rb::xfer pair.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "lockstep_core.h"
#include "resync.h"
#include "state_xfer.h"

using namespace n64run;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); g_fail++; } } while (0)

static void caps(uint8_t c[8], uint8_t v0, uint8_t v1, uint8_t v2 = 0) { std::memset(c, 0, 8); c[0] = v0; c[1] = v1; c[2] = v2; }

static void test_codec() {
    resync::Msg m; m.sub = resync::Propose; m.from = 0; m.seq = 7; m.s = 123456;
    auto v = resync::encode(kPktResync, m);
    resync::Msg d;
    CHECK(resync::decode(kPktResync, v.data(), v.size(), &d));
    CHECK(d.sub == resync::Propose && d.seq == 7 && d.s == 123456);
    CHECK(!resync::decode(kPktResync, v.data(), 5, &d));
    v[1] = 9;
    CHECK(!resync::decode(kPktResync, v.data(), v.size(), &d));
}

static void test_capability() {
    uint8_t c[8];
    resync::Ctl a;
    caps(c, 1, 1);
    a.configure(0, 0b11, c);
    CHECK(a.enabled());
    caps(c, 1, 0);   // an old guest: no capability bit
    resync::Ctl b;
    b.configure(0, 0b11, c);
    CHECK(!b.enabled());
    CHECK(b.on_mismatch(600, 650) == resync::Act::Fallback);   // old behaviour
    caps(c, 1, 1, 0);
    resync::Ctl t;
    t.configure(0, 0b111, c);   // slot 2 is in the mask but old
    CHECK(!t.enabled());
    resync::Ctl solo;
    caps(c, 1, 0);
    solo.configure(0, 0b1, c);
    CHECK(!solo.enabled());
}

static void test_handshake() {
    uint8_t c[8]; caps(c, 1, 1);
    resync::Ctl h, g;
    h.configure(0, 0b11, c, 90);
    g.configure(1, 0b11, c, 90);
    // guest sees the desync first: asks the host
    CHECK(g.on_mismatch(600, 640) == resync::Act::Reply);
    CHECK(g.req_due(1000));
    CHECK(!g.req_due(1100));   // rate limited
    CHECK(g.req_due(1200));
    resync::Msg req = g.req_msg(600), rep;
    CHECK(h.on_msg(req, 645, &rep) == resync::Act::None);
    CHECK(h.active() && h.target() == 645 + 90);
    CHECK(h.propose_due(2000) && !h.propose_due(2050) && h.propose_due(2100));
    // guest gets the proposal and acks it
    CHECK(g.on_msg(h.propose_msg(), 650, &rep) == resync::Act::Reply);
    CHECK(rep.sub == resync::PropAck && g.active() && g.target() == h.target());
    CHECK(!h.all_acked());
    CHECK(h.on_msg(rep, 660, &rep) == resync::Act::None);
    CHECK(h.all_acked() && !h.propose_due(5000));
    // a repeated Propose is answered again, not restarted
    resync::Msg again;
    CHECK(g.on_msg(h.propose_msg(), 655, &again) == resync::Act::Reply && again.sub == resync::PropAck);
    // the host saves at S - 1 and both stall at S
    CHECK(!h.save_due(h.target() - 2) && h.save_due(h.target() - 1));
    h.mark_saved();
    CHECK(!h.save_due(h.target() - 1));
    CHECK(h.barrier_at(h.target()) && g.barrier_at(g.target()) && !h.barrier_at(h.target() - 1));
    // the host's own detection of the same desync, and the guest's, while it is active: nothing new
    CHECK(h.on_mismatch(600, 700) == resync::Act::None);
    CHECK(g.on_mismatch(600, 700) == resync::Act::None);
    uint32_t S = h.target();
    h.complete(); g.complete();
    CHECK(h.count() == 1 && !h.active() && !g.active());
    CHECK(h.floor() == S && h.stale(S - 1) && !h.stale(S));
    // a late report of the old desync (hash sample before S) is history
    CHECK(h.on_mismatch(600, S + 10) == resync::Act::None && !h.active());
    CHECK(g.on_mismatch(600, S + 10) == resync::Act::None);
}

static void test_limit() {
    uint8_t c[8]; caps(c, 1, 1);
    resync::Ctl h;
    h.configure(0, 0b11, c, 90);
    uint32_t n = 1000;
    for (int i = 0; i < resync::kMaxResyncs; i++) {
        CHECK(h.on_mismatch(n, n + 5) == resync::Act::None && h.active());
        h.complete();
        n += 1200;
    }
    CHECK(h.exhausted());
    CHECK(h.on_mismatch(n, n + 5) == resync::Act::Fallback);   // the 4th: give up, end as before
    CHECK(!h.enabled());
}

static void test_guest_too_late() {
    uint8_t c[8]; caps(c, 1, 1);
    resync::Ctl h, g;
    h.configure(0, 0b11, c, 90);
    g.configure(1, 0b11, c, 90);
    h.on_mismatch(600, 700);
    resync::Msg rep;
    // the guest is already past S (a very long delay): it refuses and the host abandons the attempt
    CHECK(g.on_msg(h.propose_msg(), h.target() + 1, &rep) == resync::Act::Reply);
    CHECK(rep.sub == resync::Abort && !g.active() && g.failed());
    CHECK(h.on_msg(rep, 705, &rep) == resync::Act::Fallback);
    CHECK(!h.active() && h.failed() && !h.enabled());
}

static void test_abort_from_host() {
    uint8_t c[8]; caps(c, 1, 1);
    resync::Ctl h, g;
    h.configure(0, 0b11, c); g.configure(1, 0b11, c);
    h.on_mismatch(600, 700);
    resync::Msg rep;
    g.on_msg(h.propose_msg(), 701, &rep);
    resync::Msg ab; ab.sub = resync::Abort; ab.from = 0; ab.seq = h.seq();
    CHECK(g.on_msg(ab, 710, &rep) == resync::Act::Fallback);
    CHECK(!g.active() && !g.enabled());
}

static void test_three_players() {
    uint8_t c[8]; caps(c, 1, 1, 1);
    resync::Ctl h;
    h.configure(0, 0b111, c);
    h.on_mismatch(600, 650);
    resync::Msg a1; a1.sub = resync::PropAck; a1.from = 1; a1.seq = h.seq();
    resync::Msg rep;
    h.on_msg(a1, 651, &rep);
    CHECK(!h.all_acked());
    a1.from = 2;
    h.on_msg(a1, 652, &rep);
    CHECK(h.all_acked());
    a1.from = 1; a1.seq = (uint8_t)(h.seq() + 1);   // a stale seq is ignored
    resync::Ctl h2; h2.configure(0, 0b111, c); h2.on_mismatch(600, 650);
    h2.on_msg(a1, 651, &rep);
    CHECK(!h2.all_acked());
}

// host -> guest state transfer over a lossy, delayed virtual link, driven the way Lockstep drives it
static void test_transfer(double loss, uint32_t one_way_ms) {
    std::vector<uint8_t> state(520000);
    uint32_t x = 12345;
    for (auto& b : state) { x = x * 1664525u + 1013904223u; b = (uint8_t)(x >> 24); }
    rb::xfer::Meta m;
    m.frame = 4242; m.raw = m.comp = (uint32_t)state.size(); m.chunks = (uint16_t)((state.size() + rb::xfer::kChunk - 1) / rb::xfer::kChunk); m.hash = 77;
    rb::xfer::Sender snd;
    rb::xfer::Receiver rcv;
    const uint8_t ep = 0xC0 | 5;
    snd.start(ep, m, state, 1u << 1, 1000, 1500.0);
    struct Pkt { uint32_t at; std::vector<uint8_t> d; bool to_host; };
    std::vector<Pkt> link;
    srand(5);
    auto lossy = [&] { return (double)rand() / RAND_MAX < loss; };
    bool done_sent = false;
    uint32_t now = 1000, finished = 0;
    for (; now < 120000 && !finished; now++) {
        snd.service(now, 0, [&](const uint8_t* d, size_t n) { if (!lossy()) link.push_back({now + one_way_ms, std::vector<uint8_t>(d, d + n), false}); });
        for (size_t i = 0; i < link.size();) {
            if (link[i].at > now) { i++; continue; }
            Pkt p = link[i]; link.erase(link.begin() + i);
            const uint8_t* d = p.d.data();
            if (p.to_host) {
                if (d[1] == rb::xfer::Ack) snd.on_ack(1, rb::xfer::g16(d + 4), rb::xfer::g64(d + 6));
                else if (d[1] == rb::xfer::Done) snd.on_done(1);
            } else if (d[1] == rb::xfer::Begin) {
                rb::xfer::Meta mt;
                if (rb::xfer::dec_begin(d, p.d.size(), &mt)) rcv.on_begin(d[3], mt);
            } else if (d[1] == rb::xfer::Chunk) rcv.on_chunk(d[3], rb::xfer::g16(d + 4), d + 6, p.d.size() - 6);
        }
        uint16_t nx; uint64_t bits;
        if (rcv.active() && rcv.ack_due(now, &nx, &bits)) { auto v = rb::xfer::enc_ack(1, ep, nx, bits); if (!lossy()) link.push_back({now + one_way_ms, v, true}); }
        if (rcv.complete() && now % 100 == 0) {   // Done repeated until the host stops
            auto v = rb::xfer::header(rb::xfer::Done, 1, ep); v.push_back(1);
            if (!lossy()) link.push_back({now + one_way_ms, v, true});
            done_sent = true;
        }
        if (snd.all_done()) finished = now;
    }
    CHECK(finished && done_sent);
    CHECK(rcv.payload() == state);
    std::printf("  transfer loss %.0f%% rtt %u ms: %u ms for %zu bytes\n", loss * 100, one_way_ms * 2, finished - 1000, state.size());
}

int main() {
    test_codec();
    test_capability();
    test_handshake();
    test_limit();
    test_guest_too_late();
    test_abort_from_host();
    test_three_players();
    test_transfer(0.0, 20);
    test_transfer(0.10, 60);
    if (g_fail) { std::printf("%d FAILED\n", g_fail); return 1; }
    std::printf("n64run_resync_test: all passed\n");
    return 0;
}
