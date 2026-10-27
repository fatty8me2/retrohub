/* Console test: rewind ring (sizes, limits, exactness), policy tables, and the scripted rewind / run-ahead checks against a
 * deterministic fake core. No SDL, no real core. */
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "features.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* A tiny deterministic "console": 4 KB of RAM, a frame counter and a constant input; run() mutates a few words. */
struct Fake {
    std::vector<uint8_t> ram = std::vector<uint8_t>(4096 + 8, 0);
    uint32_t input = 0;
    uint64_t vid = 0;   // the "framebuffer": written by run(), NOT restored by load (like a real core's last picture)
    int runs = 0, spec_runs = 0, loads = 0;
    bool spec = false;
    uint64_t frame() const { uint64_t f; std::memcpy(&f, ram.data() + 4096, 8); return f; }
    void run() {
        uint64_t f = frame() + 1;
        std::memcpy(ram.data() + 4096, &f, 8);
        uint32_t x = (uint32_t)f * 2654435761u ^ input * 40503u;
        for (int k = 0; k < 6; k++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; std::memcpy(ram.data() + (x % 1000) * 4, &x, 4); }
        runs++;
        vid = hash();
        if (spec) spec_runs++;
    }
    uint64_t hash() const { uint64_t h = 1469598103934665603ull; for (uint8_t b : ram) h = (h ^ b) * 1099511628211ull; return h; }
};

static retrofeat::Hooks hooks(Fake& c, std::vector<std::string>* msgs = nullptr) {
    retrofeat::Hooks h;
    h.run = [&c] { c.run(); };
    h.state_size = [&c] { return c.ram.size(); };
    h.save = [&c](void* b, size_t n) { std::memcpy(b, c.ram.data(), n); return true; };
    h.load = [&c](const void* b, size_t n) { std::memcpy(c.ram.data(), b, n); c.loads++; return true; };
    h.video_hash = [&c] { return c.vid; };
    h.speculative = [&c](bool s) { c.spec = s; };
    if (msgs) h.notify = [msgs](const std::string& s, retrofeat::Note) { msgs->push_back(s); };
    return h;
}

static void test_ring() {
    const size_t N = 4096;
    rwd::Ring r(N, 30, rwd::kMaxBytes);
    std::vector<std::vector<uint8_t>> hist;
    std::vector<uint8_t> s(N, 0);
    uint32_t x = 12345;
    for (int i = 0; i < 100; i++) {
        for (int k = 0; k < 20; k++) { x = x * 1664525u + 1013904223u; s[x % N] = (uint8_t)(x >> 24); }
        r.push(s.data());
        hist.push_back(s);
    }
    CHECK(r.frames() == 30);                       // frame limit
    CHECK(r.bytes() < N + 29 * 400);               // sparse deltas, not 30 whole states
    std::vector<uint8_t> out;
    bool exact = true;
    for (int back = 1; back <= 29; back++) {       // 99 is the newest; step back must give 98, 97, ...
        if (!r.step_back(&out) || out != hist[99 - back]) exact = false;
    }
    CHECK(exact);
    CHECK(r.frames() == 1);
    CHECK(!r.step_back(&out));                     // history exhausted
    CHECK(r.newest(&out) && out == hist[70]);

    // byte limit: random (incompressible) states, 64 KB each, 400 KB cap
    rwd::Ring rb(65536, 1000, 400 * 1024);
    std::vector<uint8_t> big(65536);
    for (int i = 0; i < 40; i++) {
        for (auto& b : big) { x = x * 1664525u + 1013904223u; b = (uint8_t)(x >> 24); }
        rb.push(big.data());
    }
    CHECK(rb.bytes() <= 400 * 1024);
    CHECK(rb.frames() >= 2 && rb.frames() < 10);

    // odd sizes and identical states
    rwd::Ring ro(13, 5, rwd::kMaxBytes);
    uint8_t a[13] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13}, b[13];
    std::memcpy(b, a, 13); b[12] = 99;
    ro.push(a); ro.push(a); ro.push(b);
    CHECK(ro.frames() == 3);
    CHECK(ro.step_back(&out) && out.size() == 13 && std::memcmp(out.data(), a, 13) == 0);
    rwd::Ring rc(8, 0, 10);                     // max_frames 0 is clamped to 1
    rc.push(a); rc.push(b);
    CHECK(rc.frames() == 1 && rc.max_frames() == 1);
}

static void test_policy() {
    for (const char* s : {"nes", "snes", "gb", "gbc", "gba"}) {
        CHECK(rwd::auto_seconds(s) == 20);
        CHECK(runahead::auto_frames(s) == 1);
    }
    for (const char* s : {"ps1", "nds", "ps2", ""}) {
        CHECK(rwd::auto_seconds(s) == 0);
        CHECK(runahead::auto_frames(s) == 0);
        CHECK(rwd::forced_seconds(s) > 0);
    }
    int m = -9;
    CHECK(runahead::parse("auto", &m) && m == runahead::kAuto);
    CHECK(runahead::parse("off", &m) && m == 0 && runahead::parse("2", &m) && m == 2 && !runahead::parse("3", &m));
    CHECK(runahead::effective(runahead::kAuto, "gba") == 1 && runahead::effective(runahead::kAuto, "ps1") == 0 && runahead::effective(2, "ps1") == 2);
    CHECK(rwd::parse_mode("on", &m) && m == 1 && !rwd::parse_mode("maybe", &m));
}

static void test_rewind_features() {
    Fake c;
    retrofeat::Settings s; s.system = "gba"; s.measure = false;
    std::string rep;
    retrofeat::Features f(hooks(c), s);
    CHECK(f.rewind_active());
    CHECK(f.selftest_rewind(300, 100, &rep)); std::printf("%s\n", rep.c_str());
    CHECK(f.ring() && f.ring()->frames() <= 20 * 60);

    // History limit: a 20 s budget keeps 1200 states, then the oldest fall off.
    Fake c2; retrofeat::Features f2(hooks(c2), s);
    for (int i = 0; i < 3000; i++) f2.frame();
    CHECK(f2.ring()->frames() == 1200);

    // Rewind past the start of history stops and holds; held rewind while off is refused.
    Fake c3; std::vector<std::string> msgs;
    retrofeat::Features f3(hooks(c3, &msgs), s);
    for (int i = 0; i < 10; i++) f3.frame();
    CHECK(f3.set_latched(true));
    for (int i = 0; i < 30; i++) f3.frame();
    CHECK(!f3.rewinding());
    CHECK(!msgs.empty());
    retrofeat::Settings sp = s; sp.system = "ps1";
    Fake c4; std::vector<std::string> m4;
    retrofeat::Features f4(hooks(c4, &m4), sp);
    CHECK(!f4.rewind_active() && !f4.set_latched(true) && m4.size() == 1);   // PS1: off by default
    retrofeat::Settings sf = sp; sf.rewind = 1;
    Fake c5; retrofeat::Features f5(hooks(c5), sf);
    CHECK(f5.rewind_active());
    retrofeat::Settings so = s; so.rewind = 2;
    Fake c6; retrofeat::Features f6(hooks(c6), so);
    CHECK(!f6.rewind_active());
    retrofeat::Settings sn = s; sn.offline = false; sn.runahead = 2;
    Fake c7; retrofeat::Features f7(hooks(c7), sn);
    CHECK(!f7.rewind_active() && f7.runahead_frames() == 0);   // refused online
    f7.frame();
    CHECK(c7.runs == 1);
}

static void test_runahead() {
    for (int n : {1, 2}) {
        Fake c;
        retrofeat::Settings s; s.system = "nes"; s.measure = false; s.rewind = 2; s.runahead = n;
        retrofeat::Features f(hooks(c), s);
        CHECK(f.runahead_frames() == n);
        c.input = 5;
        std::string rep;
        CHECK(f.selftest_runahead(200, &rep)); std::printf("%s\n", rep.c_str());
        CHECK(c.spec_runs == n * 200);                    // speculative frames were flagged (audio muted)
        CHECK(!c.spec);
    }
    // Real state is never left ahead: after K frames the core is on frame K (+ the baseline frames the test ran first).
    Fake c; retrofeat::Settings s; s.system = "snes"; s.measure = false; s.rewind = 2; s.runahead = 2;
    retrofeat::Features f(hooks(c), s);
    for (int i = 0; i < 50; i++) f.frame();
    CHECK(c.frame() == 50);
    // Auto on GBA = 1, off on PS1.
    retrofeat::Settings sa; sa.system = "gba"; sa.runahead = runahead::kAuto; sa.rewind = 2; sa.measure = false;
    Fake ca; retrofeat::Features fa(hooks(ca), sa);
    CHECK(fa.runahead_frames() == 1);
    sa.system = "ps1";
    Fake cb; retrofeat::Features fb(hooks(cb), sa);
    CHECK(fb.runahead_frames() == 0);
    // Speed verdict: a slow clock turns run-ahead off with a message.
    Fake cs; std::vector<std::string> msgs;
    uint64_t t = 0;
    retrofeat::Hooks h = hooks(cs, &msgs);
    h.now_us = [&t] { return t += 9000; };               // every now() call costs 9 ms of fake time
    retrofeat::Settings ss; ss.system = "gba"; ss.rewind = 2; ss.runahead = 1; ss.measure = true;
    retrofeat::Features fs(h, ss);
    for (int i = 0; i < 200; i++) fs.frame();
    CHECK(fs.runahead_frames() == 0 && !msgs.empty());
}

int main() {
    test_ring();
    test_policy();
    test_rewind_features();
    test_runahead();
    if (fails) { std::printf("%d failures\n", fails); return 1; }
    std::printf("rewind_test: ok\n");
    return 0;
}
