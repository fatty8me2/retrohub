#include "stream_core.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <random>

using namespace rplay;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static Bytes rnd_blob(size_t n, uint32_t seed)
{
    std::mt19937 r(seed); Bytes b(n);
    for (auto& x : b) x = (uint8_t)r();
    return b;
}

static Bytes run(const std::vector<Bytes>& pk, Reassembler& ra, bool* got)
{
    for (auto& p : pk) ra.feed(p.data(), (int)p.size(), 0);
    uint32_t id; Bytes b; bool k;
    *got = ra.pop(&id, &b, &k);
    return b;
}

int main()
{
    // frame blob round trip
    {
        FrameInfo fi; fi.cap_us = 12345; fi.w = 1280; fi.h = 720; fi.flags = 1; fi.seq = 77;
        Bytes au = rnd_blob(100, 1);
        Bytes b = make_frame_blob(fi, au.data(), au.size());
        FrameInfo o; const uint8_t* p; size_t l;
        CHECK(parse_frame_blob(b, &o, &p, &l) && o.cap_us == 12345 && o.w == 1280 && o.seq == 77 && l == 100 && !memcmp(p, au.data(), 100));
    }
    // packetizer sizes, in-order and shuffled reassembly, all sizes around fragment boundary
    for (size_t sz : {1u, 999u, 1000u, 1001u, 4500u, 12000u, 61234u}) {
        Bytes blob = rnd_blob(sz, (uint32_t)sz);
        for (int G : {0, 1, 4, 8}) {
            auto pk = packetize_frame(5, true, blob, G);
            for (auto& p : pk) CHECK((int)p.size() <= kMaxDatagram);
            Reassembler ra; bool got;
            Bytes out = run(pk, ra, &got);
            CHECK(got && out == blob);
            std::mt19937 r(7); std::shuffle(pk.begin(), pk.end(), r);
            Reassembler rb; out = run(pk, rb, &got);
            CHECK(got && out == blob);
        }
    }
    // FEC: drop exactly one data fragment per group -> recovered without NACK
    {
        Bytes blob = rnd_blob(30000, 3);   // 30 fragments
        auto pk = packetize_frame(9, false, blob, 5);
        std::vector<Bytes> kept; int dropped = 0, di = 0;
        for (auto& p : pk) {
            if (!(p[1] & kVfParity)) { if (di % 5 == 2) { di++; dropped++; continue; } di++; }
            kept.push_back(p);
        }
        CHECK(dropped == 6);
        Reassembler ra; bool got; Bytes out = run(kept, ra, &got);
        CHECK(got && out == blob && ra.recovered_by_fec == 6);
    }
    // FEC cannot fix 2 losses in a group -> NACK -> retransmit from history completes the frame
    {
        Bytes blob = rnd_blob(10000, 4);
        auto pk = packetize_frame(11, false, blob, 8);
        NackHistory hist; hist.store(11, pk);
        std::vector<Bytes> kept; int di = 0;
        for (auto& p : pk) { if (!(p[1] & kVfParity)) { di++; if (di == 2 || di == 3) continue; } kept.push_back(p); }
        Reassembler ra; bool got; run(kept, ra, &got);
        CHECK(!got);
        CHECK(ra.nacks(5, 20, 50, 64).empty());            // too young
        auto m = ra.nacks(30, 20, 50, 64);
        CHECK(m.size() == 2 && m[0].idx == 1 && m[1].idx == 2);
        CHECK(ra.nacks(40, 20, 50, 64).empty());           // retry window
        Bytes nk = make_nack(m); auto m2 = parse_nack(nk.data(), (int)nk.size());
        CHECK(m2.size() == 2 && m2[1].frame_id == 11);
        for (auto& f : m2) { const Bytes* r = hist.find(f.frame_id, f.idx); CHECK(r); if (r) ra.feed(r->data(), (int)r->size(), 31); }
        uint32_t id; Bytes b; bool k;
        CHECK(ra.pop(&id, &b, &k) && b == blob && id == 11);
    }
    // ordering: frame 2 completes before frame 1 -> delivered 1 then 2; hopeless frame expires and requests a key
    {
        Reassembler ra;
        Bytes b1 = rnd_blob(3000, 1), b2 = rnd_blob(2000, 2), b3 = rnd_blob(500, 3);
        auto p1 = packetize_frame(1, true, b1, 0), p2 = packetize_frame(2, false, b2, 0), p3 = packetize_frame(3, false, b3, 0);
        uint32_t id; Bytes b; bool k;
        for (auto& p : packetize_frame(0, true, rnd_blob(10, 9), 0)) ra.feed(p.data(), (int)p.size(), 0);
        CHECK(ra.pop(&id, &b, &k) && id == 0);
        for (auto& p : p2) ra.feed(p.data(), (int)p.size(), 0);
        CHECK(!ra.pop(&id, &b, &k));
        for (auto& p : p1) ra.feed(p.data(), (int)p.size(), 1);
        CHECK(ra.pop(&id, &b, &k) && id == 1 && k && b == b1);
        CHECK(ra.pop(&id, &b, &k) && id == 2 && b == b2);
        CHECK(!ra.need_key);
        // frame 3 loses a fragment forever, frame 4 arrives whole
        Bytes b3big = rnd_blob(3000, 5), b4 = rnd_blob(100, 6);
        auto q3 = packetize_frame(3, false, b3big, 0), q4 = packetize_frame(4, false, b4, 0);
        ra.feed(q3[0].data(), (int)q3[0].size(), 10);
        for (auto& p : q4) ra.feed(p.data(), (int)p.size(), 11);
        CHECK(!ra.pop(&id, &b, &k));
        CHECK(!ra.expire(50, 100));
        CHECK(ra.expire(200, 100) && ra.need_key);
        CHECK(ra.pop(&id, &b, &k) && id == 4 && b == b4);
        // late duplicate of old frame is ignored
        ra.feed(q3[1].data(), (int)q3[1].size(), 210);
        CHECK(!ra.pop(&id, &b, &k));
    }
    // random loss soak: 2% iid loss with FEC group 4 + one NACK round recovers (nearly) every frame
    {
        std::mt19937 r(42); Reassembler ra; NackHistory hist(200); int done = 0, N = 300; uint32_t now = 0;
        for (int f = 1; f <= N; f++) {
            Bytes blob = rnd_blob(6000 + (f % 7) * 1000, f);
            auto pk = packetize_frame(f, f == 1, blob, 4); hist.store(f, pk);
            for (auto& p : pk) if (r() % 100 >= 2) ra.feed(p.data(), (int)p.size(), now);
            now += 16;
            for (auto& m : ra.nacks(now + 30, 20, 50, 64)) { const Bytes* p = hist.find(m.frame_id, m.idx); if (p) ra.feed(p->data(), (int)p->size(), now + 30); }
            uint32_t id; Bytes b; bool k;
            while (ra.pop(&id, &b, &k)) { done++; CHECK(b.size() >= 6000); }
        }
        CHECK(done == N);
        printf("soak: %d/%d frames, fec recovered %llu\n", done, N, (unsigned long long)ra.recovered_by_fec);
    }
    // jitter buffer: reorder, loss concealment, duplicates, late
    {
        JitterBuffer jb(3, 16);
        auto mk = [](uint32_t s) { AudioPkt a; a.codec = 1; a.seq = s; a.ts_ms = s * 20; a.data = {(uint8_t)s}; return a; };
        AudioPkt o; bool lost;
        jb.push(mk(10)); jb.push(mk(12)); CHECK(!jb.pop(&o, &lost));   // priming
        jb.push(mk(11));
        CHECK(jb.pop(&o, &lost) && !lost && o.seq == 10);
        CHECK(jb.pop(&o, &lost) && !lost && o.seq == 11);
        CHECK(jb.pop(&o, &lost) && !lost && o.seq == 12);
        CHECK(!jb.pop(&o, &lost));
        jb.push(mk(12)); CHECK(jb.late == 1);
        jb.push(mk(14)); jb.push(mk(15)); jb.push(mk(16));          // 13 missing
        CHECK(jb.pop(&o, &lost) && lost && o.seq == 13 && jb.lost_total == 1);
        CHECK(jb.pop(&o, &lost) && !lost && o.seq == 14);
        jb.push(mk(15)); CHECK(jb.dups == 1);
        Bytes b = make_audio_pkt(kAudAac, 99, 1234, (const uint8_t*)"abc", 3); AudioPkt pa;
        CHECK(parse_audio_pkt(b.data(), (int)b.size(), &pa) && pa.seq == 99 && pa.ts_ms == 1234 && pa.data.size() == 3);
        JitterBuffer ov(2, 4);
        for (uint32_t s = 0; s < 20; s += 2) ov.push(mk(s));
        CHECK(ov.depth() <= 4 && ov.overflow > 0);
    }
    // input + stats codecs
    {
        PadState p{}; p.buttons = kPadA | kPadZR; p.lx = -1234; p.ry = 32767; p.lt = 200; p.gyro[1] = 1.5f; p.slot = 2;
        Bytes b = make_input_pkt(65535, p); uint16_t seq; PadState q{};
        CHECK(parse_input_pkt(b.data(), (int)b.size(), &seq, &q) && seq == 65535 && !memcmp(&p, &q, sizeof p));
        CHECK(!parse_input_pkt(b.data(), (int)b.size() - 1, &seq, &q));
        StatsReport s; s.loss_permille = 33; s.recv_kbps = 9000; s.jitter_ms = 4; s.fps = 60; s.rtt_ms = 12;
        Bytes sb = make_stats_pkt(s); StatsReport t;
        CHECK(parse_stats_pkt(sb.data(), (int)sb.size(), &t) && t.loss_permille == 33 && t.rtt_ms == 12 && t.fps == 60);
    }
    // bitrate controller
    {
        BitrateController bc(8000); StatsReport bad; bad.loss_permille = 50;
        CHECK(bc.update(bad) == 6400);
        for (int i = 0; i < 40; i++) bc.update(bad);
        CHECK(bc.kbps() == 3000);
        StatsReport ok;
        int k = 0; for (int i = 0; i < 200; i++) k = bc.update(ok);
        CHECK(k == 15000);
        CHECK(bc.fec_group() == 8);
    }
    printf(g_fail ? "stream_core_test: %d FAILED\n" : "stream_core_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
