// Console test for retrorun's DS local-wireless packet framing (ds_link.h). Exit code 0 = all passed.
#include "ds_link.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } \
    } while (0)

static std::vector<uint8_t> blob(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; i++) v[i] = (uint8_t)(seed + i * 7);
    return v;
}

int main() {
    using namespace dslink;
    // Small packet: one datagram, header + payload, reassembles to itself.
    {
        auto b = blob(100, 1);
        auto f = split(7, b.data(), b.size());
        CHECK(f.size() == 1 && f[0].size() == kHeader + 100 && f[0][0] == kMagic);
        Reassembler r;
        std::vector<uint8_t> out;
        CHECK(r.add(1, f[0].data(), f[0].size(), &out) && out == b && r.pending() == 0);
    }
    // A full DS frame (~2.3 KB) spans three datagrams, each within the Session limit, any arrival order.
    {
        auto b = blob(2300, 9);
        auto f = split(300, b.data(), b.size());
        CHECK(f.size() == 3);
        for (auto& d : f) CHECK(d.size() <= 1100);
        Reassembler r;
        std::vector<uint8_t> out;
        CHECK(!r.add(2, f[2].data(), f[2].size(), &out));
        CHECK(!r.add(2, f[0].data(), f[0].size(), &out));
        CHECK(r.pending() == 1);
        CHECK(r.add(2, f[1].data(), f[1].size(), &out) && out == b && r.pending() == 0);
        CHECK(!r.add(2, f[1].data(), f[1].size(), &out) || out == b);   // a duplicate never corrupts (re-opens a pending entry)
    }
    // Same message id from two senders does not mix.
    {
        auto a = blob(1500, 3), c = blob(1500, 200);
        auto fa = split(5, a.data(), a.size()), fc = split(5, c.data(), c.size());
        Reassembler r;
        std::vector<uint8_t> out;
        CHECK(!r.add(1, fa[0].data(), fa[0].size(), &out));
        CHECK(!r.add(2, fc[0].data(), fc[0].size(), &out));
        CHECK(r.add(2, fc[1].data(), fc[1].size(), &out) && out == c);
        CHECK(r.add(1, fa[1].data(), fa[1].size(), &out) && out == a);
    }
    // A lost fragment never completes, and stale partial messages are bounded.
    {
        Reassembler r;
        std::vector<uint8_t> out;
        for (uint16_t id = 0; id < 40; id++) {
            auto b = blob(1500, (uint8_t)id);
            auto f = split(id, b.data(), b.size());
            CHECK(!r.add(1, f[0].data(), f[0].size(), &out));   // second fragment "lost"
        }
        CHECK(r.pending() <= 8);
    }
    // Garbage and limits.
    {
        Reassembler r;
        std::vector<uint8_t> out;
        uint8_t junk[3] = {1, 2, 3};
        CHECK(!r.add(0, junk, sizeof junk, &out));
        uint8_t bad[6] = {kMagic, 0, 0, 5, 2, 0};   // index >= count
        CHECK(!r.add(0, bad, sizeof bad, &out));
        CHECK(split(1, nullptr, 10).empty() && split(1, junk, 0).empty());
        std::vector<uint8_t> huge(kMaxMessage + 1);
        CHECK(split(1, huge.data(), huge.size()).empty());
        std::vector<uint8_t> max(kMaxMessage);
        CHECK(split(1, max.data(), max.size()).size() == 32);
    }
    // melonDS DS core MP frame header: u64 timestamp, u8 aid, u8 type.
    {
        uint8_t cmd[12] = {0, 0, 0, 0, 0, 0, 0, 1, 0, 2, 0xAA, 0xBB};
        uint8_t rep[10] = {0, 0, 0, 0, 0, 0, 0, 1, 3, 1};
        CHECK(frame_type(cmd, sizeof cmd) == 2 && frame_type(rep, sizeof rep) == 1 && frame_type(rep, 9) == -1);
    }
    if (g_fail) std::printf("%d check(s) failed\n", g_fail);
    else std::printf("ds_link_test: all passed\n");
    return g_fail ? 1 : 0;
}
