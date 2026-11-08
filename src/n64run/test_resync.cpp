// Console test: lockstep desync recovery agreement (resync.h) plus a simulated host -> guest state transfer with the rb::xfer pair.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "lockstep_core.h"
#include "fbmask.h"
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

static uint64_t plain_hash(const void* data, size_t bytes) {   // = hash_block in lockstep.cpp
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < bytes / 8; i++) { uint64_t w; std::memcpy(&w, p + i * 8, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    return h;
}

static void test_fbmask() {
    const uint32_t kRam = 8u << 20;
    std::vector<uint8_t> ram(kRam);
    uint32_t x = 99;
    for (auto& b : ram) { x = x * 1664525u + 1013904223u; b = (uint8_t)(x >> 24); }
    fbmask::Tracker t;
    CHECK(fbmask::hash(ram.data(), ram.size(), t.ranges(kRam)) == plain_hash(ram.data(), ram.size()));   // nothing seen: the plain hash
    // MK64: 320x240 16 bit, three buffers
    const uint32_t o[3] = {0x31f780, 0x345f80, 0x36b780};
    for (int i = 0; i < 9; i++) t.sample(100 + i, 0x3002, o[i % 3], 320);
    CHECK(t.count() == 3);
    t.sample(110, 0, 0x100000, 320);          // blank screen: ignored
    t.sample(110, 0x3002, 0x200000, 0);       // no width: ignored
    CHECK(t.count() == 3);
    auto r = t.ranges(kRam);
    CHECK(r.size() == 1 && r[0].lo <= 0x31f780 && r[0].hi >= 0x36b780 + 320 * 240 * 2);   // adjacent buffers merge into one range
    uint64_t base = fbmask::hash(ram.data(), ram.size(), r);
    ram[0x330000] ^= 0xFF; ram[o[2] + 1000] ^= 0x5A;   // inside a framebuffer: not a desync
    CHECK(fbmask::hash(ram.data(), ram.size(), r) == base);
    ram[0x7F0000] ^= 1;                                // outside: still detected
    CHECK(fbmask::hash(ram.data(), ram.size(), r) != base);
    ram[0x7F0000] ^= 1;
    ram[0x31f780 - 0x900] ^= 1;                        // just below the margin
    CHECK(fbmask::hash(ram.data(), ram.size(), r) != base);
    // order independent set, 4 kept, the oldest dropped
    fbmask::Tracker a, b;
    for (int i : {0, 1, 2, 0, 1, 2}) a.sample(200, 0x3002, o[i], 320);
    for (int i : {2, 1, 0, 2, 1, 0}) b.sample(200, 0x3002, o[i], 320);
    CHECK(a.ranges(kRam).size() == b.ranges(kRam).size() && a.ranges(kRam)[0].lo == b.ranges(kRam)[0].lo && a.ranges(kRam)[0].hi == b.ranges(kRam)[0].hi);
    fbmask::Tracker c;
    for (uint32_t i = 0; i < 6; i++) c.sample(300 + i, 0x3002, 0x100000 + i * 0x100000, 320);
    CHECK(c.count() == fbmask::kKeep);
    // a late joiner (saw only the last 20 polls) and the host (saw everything) agree once the stale origin expired
    fbmask::Tracker host, joiner;
    for (uint32_t n = 0; n < 400; n++) host.sample(n, 0x3002, n < 50 ? 0x500000u : o[n % 3], 320);
    for (uint32_t n = 380; n < 400; n++) joiner.sample(n, 0x3002, o[n % 3], 320);
    CHECK(host.count() == 3 && joiner.count() == 3 && host.ranges(kRam)[0].lo == joiner.ranges(kRam)[0].lo);
    c.reset();
    CHECK(c.count() == 0 && c.ranges(kRam).empty());
}

static void test_status_poll_and_drop() {
    uint8_t c[8]; caps(c, 1, 1, 1);
    resync::Ctl h, g;
    h.configure(0, 0b111, c); g.configure(1, 0b111, c);
    h.on_mismatch(600, 700);
    resync::Msg rep;
    g.on_msg(h.propose_msg(), 701, &rep);
    // the host aborted (guest 2 refused) and every Abort to guest 1 was lost: the guest's status poll (a repeated PropAck) gets an Abort
    resync::Msg ab; ab.sub = resync::Abort; ab.from = 2; ab.seq = h.seq();
    CHECK(h.on_msg(ab, 705, &rep) == resync::Act::Fallback && !h.active());
    resync::Msg poll = g.ack_msg(), ans;
    CHECK(h.on_msg(poll, 706, &ans) == resync::Act::Reply && ans.sub == resync::Abort && ans.seq == poll.seq);
    CHECK(g.on_msg(ans, 707, &rep) == resync::Act::Fallback && !g.active());
    // a late duplicate status poll after a completed recovery must not hurt the guest that already moved on
    resync::Ctl h2, g2;
    h2.configure(0, 0b11, c); g2.configure(1, 0b11, c);
    h2.on_mismatch(600, 700); g2.on_msg(h2.propose_msg(), 701, &rep);
    resync::Msg stale = g2.ack_msg();
    h2.complete(); g2.complete();
    CHECK(h2.on_msg(stale, 800, &ans) == resync::Act::Reply && ans.sub == resync::Abort);
    CHECK(g2.on_msg(ans, 801, &rep) == resync::Act::None && g2.enabled());
    // a dropped guest is no longer waited for
    resync::Ctl h3; h3.configure(0, 0b111, c); h3.on_mismatch(600, 650);
    resync::Msg a1; a1.sub = resync::PropAck; a1.from = 1; a1.seq = h3.seq();
    h3.on_msg(a1, 651, &rep);
    CHECK(!h3.all_acked());
    h3.drop(2);
    CHECK(h3.all_acked());
}

// netcore-M2: a guest repeating Req cannot burn the host's recoveries; the first request works at once.
static void test_req_throttle() {
    uint8_t c[8]; caps(c, 1, 1, 1);
    resync::Ctl h;
    h.configure(0, 0b111, c, 90);
    resync::Msg req = h.req_msg(600), rep;
    req.from = 1;
    CHECK(h.on_msg(req, 650, &rep, 1000) == resync::Act::None && h.active());   // first request: honoured immediately
    CHECK(!h.take_throttle_note());
    h.complete();                                                                // recovery 1 done
    req.s = 1200;
    CHECK(h.on_msg(req, 1300, &rep, 2000) == resync::Act::None && !h.active());  // 1 s later from the same guest: refused
    CHECK(h.take_throttle_note() && !h.take_throttle_note());                    // logged once ...
    h.on_msg(req, 1301, &rep, 2100);
    CHECK(!h.take_throttle_note() && h.count() == 1 && !h.failed());             // ... not again
    resync::Msg req2 = req; req2.from = 2;
    CHECK(h.on_msg(req2, 1302, &rep, 2200) == resync::Act::None && h.active());   // another guest is not affected
    h.complete();
    req.s = 5000;
    CHECK(h.on_msg(req, 1500, &rep, 1000 + resync::kReqWindowMs) == resync::Act::None && h.active());   // window over: honoured again
    h.complete();
    CHECK(h.count() == 3);
    // spam after the limit cannot be used to switch recovery off before the window passes either
    resync::Ctl k; k.configure(0, 0b11, c, 90);
    k.on_msg(req, 650, &rep, 10000); k.complete();
    k.on_msg(req, 1300, &rep, 10100);
    CHECK(!k.failed() && k.count() == 1);
}

// netcore-M5: the whole recovery in a 3-4 player room over a lossy, delayed virtual network. One guest's state diverged; every peer runs the
// real agreement (resync::Ctl) and the real shared state transfer (one rb::xfer Sender, one Receiver per guest, as Lockstep::rs_host_barrier does).
// Optionally one guest dies before the proposal is acknowledged: the host drops it and the others still recover. Checks: all live peers end on
// the host's state, exactly one recovery ran, nobody fell back, and every live peer plays on past the barrier.
static void test_room(int peers, double loss, uint32_t one_way_ms, int dead_slot) {
    uint8_t cp[8]; caps(cp, 1, 1, 1);
    for (int i = 3; i < peers; i++) cp[i] = 1;
    uint32_t mask = (1u << peers) - 1;
    auto make_state = [](uint32_t seed) {
        std::vector<uint8_t> v(300000);
        uint32_t x = seed;
        for (auto& b : v) { x = x * 1664525u + 1013904223u; b = (uint8_t)(x >> 24); }
        return v;
    };
    const std::vector<uint8_t> good = make_state(11);
    struct Peer {
        resync::Ctl ctl; rb::xfer::Receiver rcv;
        std::vector<uint8_t> state; uint32_t sample = 500; bool stalled = false, saw_mismatch = false;
        int done_ep = -1; uint32_t last_status = 0;
    };
    std::vector<Peer> P(peers);
    for (int i = 0; i < peers; i++) { P[i].ctl.configure(i, mask, cp, 90); P[i].state = good; }
    P[1].state = make_state(99);   // guest 1 diverged
    CHECK(P[0].ctl.enabled() && P[1].ctl.enabled());
    struct Pkt { uint32_t at; int to; bool is_xfer; std::vector<uint8_t> d; };
    std::vector<Pkt> net;
    srand(900 + peers);
    auto lossy = [&] { return (double)rand() / RAND_MAX < loss; };
    auto send = [&](uint32_t now, int from, int to, bool x, std::vector<uint8_t> d) {
        if (from == dead_slot || to == dead_slot) return;   // a dead peer neither sends nor receives
        if (!lossy()) net.push_back({now + one_way_ms, to, x, std::move(d)});
    };
    rb::xfer::Sender snd;
    uint32_t host_s = 0;
    const uint8_t kType = 0x77;
    bool dropped = dead_slot < 0;
    uint32_t finished = 0, now = 1000;
    for (; now < 90000; now++) {
        for (size_t i = 0; i < net.size();) {   // deliver
            if (net[i].at > now) { i++; continue; }
            Pkt p = std::move(net[i]); net.erase(net.begin() + i);
            Peer& me = P[p.to];
            const uint8_t* d = p.d.data();
            if (!p.is_xfer) {
                resync::Msg m, rep;
                if (!resync::decode(kType, d, p.d.size(), &m)) continue;
                resync::Act a = me.ctl.on_msg(m, me.sample, &rep, now);
                if (a == resync::Act::Reply) send(now, p.to, p.to == 0 ? m.from : 0, false, resync::encode(kType, rep));
            } else if (p.to == 0) {
                int from = d[2];
                if (d[1] == rb::xfer::Ack) snd.on_ack(from, rb::xfer::g16(d + 4), rb::xfer::g64(d + 6));
                else if (d[1] == rb::xfer::Done) snd.on_done(from);
            } else if (d[1] == rb::xfer::Begin || d[1] == rb::xfer::Chunk) {
                if (me.done_ep == d[3]) { auto v = rb::xfer::header(rb::xfer::Done, p.to, d[3]); v.push_back(1); send(now, p.to, 0, true, v); }   // loaded already: our Done was lost
                else if (d[1] == rb::xfer::Begin) { rb::xfer::Meta mt; if (rb::xfer::dec_begin(d, p.d.size(), &mt)) me.rcv.on_begin(d[3], mt); }
                else me.rcv.on_chunk(d[3], rb::xfer::g16(d + 4), d + 6, p.d.size() - 6);
            }
        }
        for (int i = 0; i < peers; i++) {
            if (i == dead_slot) continue;
            Peer& me = P[i];
            if (i == 1 && me.sample == 600 && !me.saw_mismatch) { me.saw_mismatch = true; me.ctl.on_mismatch(600, me.sample); }
            if (i >= 2 && !me.saw_mismatch && me.sample == 612) { me.saw_mismatch = true; me.ctl.on_mismatch(600, me.sample); }   // they compare against guest 1 too
            if (i == 0 && me.sample == 700 && !me.saw_mismatch) { me.saw_mismatch = true; me.ctl.on_mismatch(600, me.sample); }
            if (i > 0 && me.ctl.req_due(now)) send(now, i, 0, false, resync::encode(kType, me.ctl.req_msg(600)));
            if (i == 0 && me.ctl.propose_due(now)) for (int g = 1; g < peers; g++) send(now, 0, g, false, resync::encode(kType, me.ctl.propose_msg()));
            if (me.ctl.active() && me.sample >= me.ctl.target()) me.stalled = true;
            if (!me.stalled) me.sample++;
            if (i == 0) {
                if (me.ctl.save_due(me.sample)) { me.ctl.mark_saved(); host_s = me.ctl.target(); }
                if (me.stalled && host_s && me.ctl.all_acked() && !snd.active()) {
                    rb::xfer::Meta m;
                    m.frame = host_s; m.raw = m.comp = (uint32_t)good.size(); m.chunks = (uint16_t)((good.size() + rb::xfer::kChunk - 1) / rb::xfer::kChunk); m.hash = 5;
                    snd.start((uint8_t)(0xC0 | (me.ctl.seq() & 0x3F)), m, good, me.ctl.guests(), now, 1500.0);
                }
                if (snd.active()) {
                    snd.service(now, 0, [&](const uint8_t* d, size_t n) { for (int g = 1; g < peers; g++) send(now, 0, g, true, std::vector<uint8_t>(d, d + n)); });
                    if (snd.all_done()) { snd.stop(); me.ctl.complete(); me.stalled = false; host_s = 0; if (!finished) finished = now; }
                }
                // the host notices the dead peer (the drop agreement of the real game) a while after the proposal went out
                if (!dropped && me.ctl.active() && now > 3000) { me.ctl.drop(dead_slot); snd.remove(dead_slot); dropped = true; }
            } else {
                if (me.stalled && now - me.last_status >= 250) { me.last_status = now; send(now, i, 0, false, resync::encode(kType, me.ctl.ack_msg())); }
                uint16_t nx; uint64_t bits;
                if (me.rcv.active() && me.rcv.ack_due(now, &nx, &bits)) send(now, i, 0, true, rb::xfer::enc_ack(i, me.rcv.epoch(), nx, bits));
                if (me.stalled && me.rcv.complete()) {
                    me.state = me.rcv.payload();
                    me.done_ep = me.rcv.epoch();
                    auto v = rb::xfer::header(rb::xfer::Done, i, (uint8_t)me.done_ep); v.push_back(1);
                    send(now, i, 0, true, v);
                    me.rcv.reset(); me.ctl.complete(); me.stalled = false;
                }
            }
        }
        if (finished && now > finished + 400) break;   // everyone had time to run on
    }
    CHECK(finished != 0);
    CHECK(P[0].ctl.count() == 1 && !P[0].ctl.failed() && !P[0].ctl.active());
    uint32_t target = P[0].ctl.floor();
    for (int i = 1; i < peers; i++) {
        if (i == dead_slot) continue;
        CHECK(P[i].state == good);                           // the diverged guest was repaired, the others still match
        CHECK(P[i].ctl.count() == 1 && !P[i].ctl.failed());  // nobody fell back to the old behaviour
        CHECK(P[i].sample > target);                         // and they play on past the barrier
    }
    CHECK(P[0].sample > target);
    std::printf("  room of %d%s, loss %.0f%%: recovered in %u ms\n", peers, dead_slot >= 0 ? " (one peer died)" : "", loss * 100, finished ? finished - 1000 : 0);
}

int main() {
    test_req_throttle();
    test_fbmask();
    test_status_poll_and_drop();
    test_codec();
    test_capability();
    test_handshake();
    test_limit();
    test_guest_too_late();
    test_abort_from_host();
    test_three_players();
    test_transfer(0.0, 20);
    test_transfer(0.10, 60);
    test_room(3, 0.0, 20, -1);
    test_room(4, 0.0, 20, -1);
    test_room(3, 0.05, 40, -1);
    test_room(4, 0.05, 40, -1);
    test_room(4, 0.05, 40, 3);   // a guest dies mid-recovery: the host drops it and the other two are repaired
    test_room(3, 0.0, 20, 2);
    if (g_fail) { std::printf("%d FAILED\n", g_fail); return 1; }
    std::printf("n64run_resync_test: all passed\n");
    return 0;
}
