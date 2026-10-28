// Console test for spectate.h: a spectator follows a host's confirmed-input stream over a lossy, reordering link,
// joins late, catches up, detects a hash mismatch, and learns the end of the session.
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <random>
#include <vector>

#include "spectate.h"

static int g_fail = 0;
#define CHECK(c, msg) do { if (!(c)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); g_fail++; } } while (0)

struct Link {
    struct Pkt { uint32_t due; std::vector<uint8_t> d; };
    std::deque<Pkt> q;
    std::mt19937 rng{7};
    int loss_pct = 0, lat = 20, jitter = 0;
    void send(uint32_t now, const uint8_t* d, int n) {
        if (loss_pct && (int)(rng() % 100) < loss_pct) return;
        q.push_back({now + lat + (jitter ? (uint32_t)(rng() % (jitter + 1)) : 0u), std::vector<uint8_t>(d, d + n)});
    }
    bool pop(uint32_t now, std::vector<uint8_t>* out) {
        for (size_t i = 0; i < q.size(); i++)
            if ((int32_t)(now - q[i].due) >= 0) { *out = q[i].d; q.erase(q.begin() + i); return true; }
        return false;
    }
};

static uint64_t val(uint32_t s, int p) { return (uint64_t)(s * 2654435761u) ^ ((uint64_t)p << 40) ^ (p == 1 ? 0xABCDEF0000000000ull : 0); }

struct Sim {
    spectate::Host host;
    spectate::Client cli;
    Link down, up;
    uint32_t now = 1000;
    uint32_t consumed = 0;
    uint64_t sum_host = 0, sum_cli = 0;
    bool wide;
    Sim(uint8_t mask, bool w) : host(mask, w ? 8 : 4), wide(w) {}
    void push_one() {
        uint64_t v[4];
        for (int p = 0; p < 4; p++) v[p] = wide ? val(host.total(), p) : (uint32_t)val(host.total(), p);
        host.push(v);
        for (int p = 0; p < 4; p++) if (0xB >> p & 1) sum_host = sum_host * 31 + v[p];
        if (host.total() % 600 == 0) host.add_hash(host.total(), 0x1000 + host.total());
    }
    void tick(bool cli_consumes, int per_tick) {
        now++;
        host.service(now, 1u << 2, [&](int, const uint8_t* d, int n) { down.send(now, d, n); });
        std::vector<uint8_t> m;
        while (down.pop(now, &m)) cli.on_data(m.data(), m.size(), now);
        for (int i = 0; cli_consumes && i < per_tick && cli.ready(consumed); i++) {
            for (int p = 0; p < 4; p++) if (0xB >> p & 1) sum_cli = sum_cli * 31 + cli.get(consumed, p);
            if ((consumed + 1) % 600 == 0) cli.add_local_hash(consumed + 1, 0x1000 + consumed + 1);
            consumed++;
            cli.consumed(consumed);
        }
        cli.service(now, 2, [&](int, const uint8_t* d, int n) { up.send(now, d, n); });
        while (up.pop(now, &m)) host.on_ack(2, m.data(), m.size(), now);
    }
};

static void run(bool wide, int loss, int jitter, const char* name) {
    Sim s(0xB, wide);
    s.down.loss_pct = s.up.loss_pct = loss;
    s.down.jitter = s.up.jitter = jitter;
    // 3000 samples of history exist before the spectator is even connected (late join), then the live game continues
    for (int i = 0; i < 3000; i++) s.push_one();
    for (int t = 0; t < 400000 && s.consumed < 6000; t++) {
        if (t % 16 == 0 && s.host.total() < 6000) s.push_one();   // live: ~1 sample per 16 ms
        s.tick(true, 8);                                           // spectator may run up to 8 samples per ms while catching up
    }
    char b[96];
    std::snprintf(b, sizeof b, "%s: spectator consumed all 6000 samples", name);
    CHECK(s.consumed == 6000, b);
    std::snprintf(b, sizeof b, "%s: identical input sums", name);
    CHECK(s.sum_host == s.sum_cli, b);
    std::snprintf(b, sizeof b, "%s: hashes compared and equal", name);
    CHECK(s.cli.hashes_ok() >= 8 && s.cli.hashes_bad() == 0, b);
    for (int t = 0; t < 3000; t++) s.tick(true, 8);
    std::snprintf(b, sizeof b, "%s: host saw the spectator's hashes ok", name);
    CHECK(s.host.hashes_ok() >= 8 && s.host.hashes_bad() == 0, b);
    s.host.finish();
    for (int t = 0; t < 3000; t++) s.tick(true, 8);
    std::snprintf(b, sizeof b, "%s: fin learned, all acked", name);
    CHECK(s.cli.finished(s.consumed) && s.host.all_acked(1u << 2), b);
    std::printf("%s: ok (packets %u)\n", name, s.host.packets_sent());
}

int main() {
    run(false, 0, 0, "clean");
    run(false, 20, 30, "20% loss, jitter");
    run(true, 35, 60, "wide 35% loss, heavy jitter");

    {   // flow control: a stalled spectator never gets more than kWindow samples ahead of what it consumed
        Sim s(0xB, false);
        for (int i = 0; i < 5000; i++) s.push_one();
        for (int t = 0; t < 4000; t++) s.tick(false, 0);
        CHECK(s.cli.total() == 5000, "total known");
        CHECK(s.host.acked_of(2) <= (int64_t)spectate::kWindow + 1, "window bounds the stream");
        for (int t = 0; t < 4000; t++) s.tick(true, 2);
        CHECK(s.consumed == 5000, "resumes after the stall");
    }
    {   // hash mismatch is detected on both ends
        Sim s(0x3, false);
        for (int i = 0; i < 1300; i++) s.push_one();
        for (int t = 0; t < 4000; t++) {
            s.now++;
            s.host.service(s.now, 1u << 1, [&](int, const uint8_t* d, int n) { s.down.send(s.now, d, n); });
            std::vector<uint8_t> m;
            while (s.down.pop(s.now, &m)) s.cli.on_data(m.data(), m.size(), s.now);
            for (int i = 0; i < 8 && s.cli.ready(s.consumed); i++) {
                if ((s.consumed + 1) % 600 == 0) s.cli.add_local_hash(s.consumed + 1, 0xDEAD);   // wrong
                s.consumed++; s.cli.consumed(s.consumed);
            }
            s.cli.service(s.now, 1, [&](int, const uint8_t* d, int n) { s.up.send(s.now, d, n); });
            while (s.up.pop(s.now, &m)) s.host.on_ack(1, m.data(), m.size(), s.now);
        }
        uint32_t sm; uint64_t a, b;
        CHECK(s.cli.hashes_bad() >= 1 && s.cli.poll_mismatch(&sm, &a, &b) && a == 0xDEAD, "spectator sees the mismatch");
        CHECK(s.host.hashes_bad() >= 1, "host sees the mismatch");
    }
    {   // garbage never crashes or advances
        spectate::Client c;
        uint8_t junk[40] = {spectate::kPktData, 0, 0, 0, 0, 0, 200, 0xF, 4};
        CHECK(!c.on_data(junk, sizeof junk, 5), "truncated data rejected");
        CHECK(!c.on_data(junk, 3, 5), "short rejected");
        spectate::Host h(0x1, 4);
        uint8_t ack[5] = {spectate::kPktAck, 1, 0, 0, 0};
        CHECK(!h.on_ack(1, ack, sizeof ack, 5), "short ack rejected");
    }
    {   // live join: host holds a new spectator, starts it at the snapshot frame; it buffers from F over a lossy link and catches up
        for (int loss : {0, 15}) {
            spectate::Host host(0x3, 8);
            host.set_hold_ms(spectate::kHoldMs);
            spectate::Client cli;
            Link down, up;
            down.loss_pct = up.loss_pct = loss; down.jitter = up.jitter = 10;
            uint32_t now = 1000;
            auto produce = [&]() {
                uint64_t v[4] = {0, 0, 0, 0};
                for (int p = 0; p < 2; p++) v[p] = val(host.total(), p);
                host.push(v);
                if (host.total() % 60 == 0) host.add_hash(host.total(), 0x5000 + host.total());
            };
            for (int i = 0; i < 9000; i++) produce();   // 2.5 minutes of match already played
            // held: nothing streams before the join is decided
            int sent = 0;
            for (int t = 0; t < 500; t++) { now++; host.service(now, 1u << 2, [&](int, const uint8_t*, int) { sent++; }); }
            CHECK(sent == 0 && host.held(2), "held spectator gets nothing");
            CHECK(spectate::decide_join(true, host.total(), 0, false, false) == spectate::JoinDecision::Snapshot, "snapshot decided");
            const uint32_t F = host.total() - 3;   // the confirmed frame of the snapshot (a little behind the newest)
            CHECK(host.start_peer_at(2, F, now), "start at F");
            CHECK(!host.start_peer_at(2, host.total() + 1, now), "frame past the history refused");
            CHECK(host.start_peer_at(2, F, now), "start at F again");
            cli.start_at(F);
            CHECK(cli.base() == F && !cli.ready(F), "client starts empty at F");
            // the spectator downloads for 800 ms while the match goes on (60 fps), buffering inputs, then plays unpaced until live
            uint32_t f = F, t_load = now + 800, live_at = 0;
            spectate::CatchUp cu;
            bool good = true;
            double next_prod = now;
            int hash_ok_before = 0;
            for (int t = 0; t < 8000; t++) {
                now++;
                while (now >= next_prod) { produce(); next_prod += 1000.0 / 60.0; }
                host.service(now, 1u << 2, [&](int, const uint8_t* d, int n) { down.send(now, d, n); });
                std::vector<uint8_t> m;
                while (down.pop(now, &m)) cli.on_data(m.data(), m.size(), now);
                if (now >= t_load) {
                    // unpaced: up to 20 frames per ms (~20x real time) while catching, real time once live
                    int budget = cu.catching() || !cu.live_once() ? 20 : 0;
                    if (!budget && cli.lag() > 2) budget = 1;
                    for (int i = 0; i < budget && cli.ready(f); i++) {
                        if (cli.get(f, 0) != val(f, 0) || cli.get(f, 1) != val(f, 1)) good = false;
                        f++;
                        cli.consumed(f);
                        if (f % 60 == 0) cli.add_local_hash(f, 0x5000 + f);
                    }
                    cu.update(cli.lag(), now);
                    if (cu.live_once() && !live_at) live_at = now;
                }
                cli.service(now, 2, [&](int, const uint8_t* d, int n) { up.send(now, d, n); });
                while (up.pop(now, &m)) host.on_ack(2, m.data(), m.size(), now);
            }
            CHECK(good, "live join: every frame from F has the host's inputs");
            CHECK(live_at && live_at - t_load < 1500, "live join: caught up within 1.5 s of loading");
            CHECK(cli.lag() <= spectate::kLiveLagSamples + 2, "live join: stays live");
            CHECK(f > F + 300, "live join: kept playing");
            CHECK(cli.hashes_ok() > 3 && cli.hashes_bad() == 0, "live join: hashes from F on agree");
            CHECK(host.hashes_ok() > 0 && host.hashes_bad() == 0, "live join: host sees agreeing hashes");
            (void)hash_ok_before;
        }
    }
    {   // hold: an old spectator (acks from 0 without a Join) is released at once and streamed from 0; nobody: after kHoldMs
        spectate::Host host(0x1, 4);
        host.set_hold_ms(spectate::kHoldMs);
        uint64_t v[4] = {7, 0, 0, 0};
        for (int i = 0; i < 100; i++) host.push(v);
        int sent = 0;
        host.service(10, 1u << 1, [&](int, const uint8_t*, int) { sent++; });
        CHECK(sent == 0, "held at first");
        spectate::Client c;
        std::vector<uint8_t> ack;
        c.service(20, 1, [&](int, const uint8_t* d, int n) { ack.assign(d, d + n); });
        CHECK(!ack.empty() && host.on_ack(1, ack.data(), ack.size(), 20), "legacy ack accepted");
        CHECK(!host.held(1), "legacy ack releases the hold");
        uint32_t first = 0xFFFFFFFF;
        host.service(30, 1u << 1, [&](int, const uint8_t* d, int) { uint32_t x; std::memcpy(&x, d + 2, 4); first = std::min(first, x); });
        CHECK(first == 0, "legacy spectator streamed from 0");
        sent = 0;
        host.service(40, 1u << 2, [&](int, const uint8_t*, int) { sent++; });
        CHECK(sent == 0, "silent spectator held");
        host.service(40 + spectate::kHoldMs, 1u << 2, [&](int, const uint8_t*, int) { sent++; });
        CHECK(sent > 0, "hold times out: stream from 0");
        host.service(50 + spectate::kHoldMs, 1u << 3, [&](int, const uint8_t*, int) {});
        host.release(3);
        CHECK(!host.held(3), "release ends the hold");
        {   // extend_hold keeps a spectator held past kHoldMs while its snapshot is prepared
            spectate::Host hh(0x1, 4);
            hh.set_hold_ms(spectate::kHoldMs);
            for (int i = 0; i < 400; i++) hh.push(v);
            int n2 = 0;
            hh.service(100, 1u << 1, [&](int, const uint8_t*, int) { n2++; });
            hh.extend_hold(1, 100 + spectate::kHoldMs - 10);
            hh.service(100 + spectate::kHoldMs + 5, 1u << 1, [&](int, const uint8_t*, int) { n2++; });
            CHECK(n2 == 0 && hh.held(1), "extended hold");
            // the spectator leaves (mask 0) and a new one takes the slot: it starts held again, not at the old position
            hh.start_peer_at(1, 350, 200);
            hh.service(300, 0, [&](int, const uint8_t*, int) {});
            n2 = 0;
            hh.service(400, 1u << 1, [&](int, const uint8_t*, int) { n2++; });
            CHECK(n2 == 0 && hh.held(1), "a departed spectator is forgotten");
        }
        spectate::Host legacy(0x1, 4);   // hold 0 (N64 lockstep): streams at once
        for (int i = 0; i < 10; i++) legacy.push(v);
        sent = 0;
        legacy.service(5, 1u << 1, [&](int, const uint8_t*, int) { sent++; });
        CHECK(sent > 0, "no hold: streams at once");
    }
    {   // join decisions and the spectator's state machine
        using spectate::JoinDecision;
        CHECK(spectate::decide_join(false, 99999, 0, false, false) == JoinDecision::NoLiveUnsupported, "unsupported core");
        CHECK(spectate::decide_join(true, spectate::kMinLiveFrames - 1, 0, false, false) == JoinDecision::NoLiveEarly, "young match: from 0");
        CHECK(spectate::decide_join(true, 99999, spectate::kMaxSnapshotsInFlight, false, false) == JoinDecision::Busy, "cap on snapshots");
        CHECK(spectate::decide_join(true, 99999, 0, false, true) == JoinDecision::Busy, "engine resyncing: later");
        CHECK(spectate::decide_join(true, 99999, 1, true, false) == JoinDecision::Ignore, "repeated knock ignored");
        spectate::JoinClient j;
        CHECK(j.want_knock(100) && !j.want_knock(150) && j.want_knock(100 + spectate::kKnockMs), "knock pacing");
        CHECK(!j.acks_allowed() && !j.playing(), "no acks while knocking");
        j.on_begin(5000, 400);
        CHECK(j.mode() == spectate::JoinClient::Mode::Download && j.frame() == 5000 && j.acks_allowed(), "download");
        CHECK(!j.want_knock(1000), "no knocks while downloading");
        CHECK(!j.download_stuck(5000, 4000) && j.download_stuck(30000, 4000), "stuck download detected");
        j.on_nolive(1);
        CHECK(j.mode() == spectate::JoinClient::Mode::Download, "late NoLive ignored");
        j.on_loaded();
        CHECK(j.playing() && j.live(), "playing live");
        spectate::JoinClient k;
        k.want_knock(1);
        k.on_nolive(spectate::kRefuseEarly);
        CHECK(k.playing() && !k.live() && k.refuse_reason() == spectate::kRefuseEarly, "refused: from 0");
        spectate::JoinClient l;
        l.on_legacy_data();
        CHECK(l.playing() && l.refuse_reason() == 0xFF, "old host: from 0");
        spectate::JoinClient n(false);
        CHECK(n.playing() && !n.want_knock(1), "live join off");
        spectate::CatchUp cu;
        CHECK(cu.update(500, 10) && !cu.live_once(), "behind: unpaced");
        CHECK(!cu.update(spectate::kLiveLagSamples, 20) && cu.live_once() && cu.live_at() == 20, "live");
        CHECK(cu.update(100, 30) && cu.live_at() == 20, "falls behind again: unpaced, first live time kept");
    }
    {   // n64run lockstep live join: snapshot requested during poll n = state before n + 1; spectator resumes at S; hashes agree
        using spectate::JoinDecision;
        CHECK(spectate::lockstep_snapshot_sample(858) == 859 && spectate::lockstep_snapshot_sample(0) == 1, "snapshot sample is the next poll");
        CHECK(spectate::decide_join(true, 2000, 1, false, true) == JoinDecision::Busy, "n64: one core save job at a time");
        spectate::Host h(0x3, 4);
        spectate::Client c;
        Link dn, up;
        dn.loss_pct = 10; dn.jitter = 30; up.loss_pct = 10;
        h.set_hold_ms(spectate::kHoldMs);
        auto step = [](uint64_t st, uint32_t n) { return (st ^ val(n, 0) ^ (val(n, 1) << 1)) * 0x9E3779B97F4A7C15ull; };   // the "emulator"
        uint64_t hstate = 0, snap = 0;
        uint32_t S = 0, now = 5000;
        const uint32_t join_at = 1500, total = 4000;
        // the spectator buffers from S while the state downloads (start_at at Begin)
        c.start_at(spectate::lockstep_snapshot_sample(join_at));
        uint64_t sst = 0;
        uint32_t n = 0;
        bool loaded = false;
        auto pump = [&]() {
            std::vector<uint8_t> m;
            while (dn.pop(now, &m)) c.on_data(m.data(), m.size(), now);
            while (up.pop(now, &m)) h.on_ack(2, m.data(), m.size(), now);
            h.service(now, 1u << 2, [&](int, const uint8_t* d, int k) { dn.send(now, d, k); });
            c.service(now, 2, [&](int, const uint8_t* d, int k) { up.send(now, d, k); });
        };
        auto spec_run = [&](uint32_t limit) {
            while (loaded && n < limit && c.ready(n)) {
                if (n % 100 == 0) c.add_local_hash(n, sst);
                CHECK(c.get(n, 0) == (uint32_t)val(n, 0) && c.get(n, 1) == (uint32_t)val(n, 1), "input at the right sample (4-byte values)");
                sst = step(sst, n);
                n++;
                c.consumed(n);
            }
        };
        for (uint32_t p = 0; p < total; p++) {   // host polls at 60 Hz; the spectator runs whatever it has (unthrottled)
            if (p % 100 == 0 && p) h.add_hash(p, hstate);   // the lockstep's hash point: before sample p is delivered
            if (p == join_at) S = spectate::lockstep_snapshot_sample(p);
            uint64_t v[4] = {val(p, 0), val(p, 1), 0, 0};
            hstate = step(hstate, p);   // poll p consumed
            if (p == join_at) snap = hstate;   // the core's save job runs after the poll: the state before S
            h.push(v);
            if (p == join_at + 3) CHECK(h.start_peer_at(2, S, now), "stream from S once recorded");
            if (p == join_at + 120) { sst = snap; n = S; loaded = true; }   // the state arrived and the core loaded it
            for (int i = 0; i < 8; i++, now += 2) { pump(); spec_run(total); }
        }
        for (int i = 0; i < 3000 && n < total; i++, now += 2) { pump(); spec_run(total); }
        CHECK(n == total, "spectator reached the end");
        CHECK(sst == hstate, "same final state as the host");
        for (int i = 0; i < 400; i++, now += 5) pump();   // let the last hashes travel
        CHECK(c.hashes_ok() >= 5 && c.hashes_bad() == 0, "hashes agree after the live join");
        CHECK(h.hashes_bad() == 0 && h.hashes_ok() > 0, "host sees the same game");
        std::printf("n64 live join: S=%u, spectator hashes ok %u, host %u\n", S, c.hashes_ok(), h.hashes_ok());
    }
    if (g_fail) { std::printf("%d FAILED\n", g_fail); return 1; }
    std::printf("spectate test: all ok\n");
    return 0;
}
