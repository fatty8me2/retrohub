// Console test for tex_cache_logic.h: thumbnail caches must never free a texture queued for drawing this frame.
#include "tex_cache_logic.h"
#include <cstdio>
#include <map>
#include <set>
using namespace hubtex;
static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } else ++g_pass; } while (0)

struct Tex { int id = 0; };
static std::map<int, int> g_destroyed;   // id -> times destroyed
static std::set<int> g_destroyed_this_frame;
static void kill(Tex& t) { ++g_destroyed[t.id]; g_destroyed_this_frame.insert(t.id); }

int main() {
    {   // 100 inserts in one frame, capacity 40: nothing touched this frame is destroyed in that frame
        g_destroyed.clear(); g_destroyed_this_frame.clear();
        TexCache<Tex> c(40, kill);
        c.begin_frame();
        g_destroyed_this_frame.clear();
        for (int i = 0; i < 100; ++i) { Tex t; t.id = i; c.insert("k" + std::to_string(i), t); }
        CHECK(g_destroyed_this_frame.empty());
        CHECK(g_destroyed.empty());
        CHECK(c.size() == 100);                 // soft cap: all were used this frame
        for (int i = 0; i < 100; ++i) CHECK(c.find("k" + std::to_string(i)) != nullptr);
        // next frame: graveyard empty, then eviction down to capacity (evicted ones wait one more frame)
        c.begin_frame();
        CHECK(g_destroyed.empty());
        CHECK(c.size() == 40);
        CHECK(c.graveyard_size() == 60);
        // LRU: the 40 newest survive
        for (int i = 60; i < 100; ++i) CHECK(c.find("k" + std::to_string(i)) != nullptr);
        c.begin_frame();
        CHECK(g_destroyed.size() == 60);
        for (auto& kv : g_destroyed) CHECK(kv.second == 1 && kv.first < 60);
        CHECK(c.graveyard_size() == 0);
        c.begin_frame(); c.begin_frame();
        for (auto& kv : g_destroyed) CHECK(kv.second == 1);   // exactly once
    }
    {   // LRU order with touches, capacity respected across frames
        g_destroyed.clear();
        TexCache<Tex> c(3, kill);
        c.begin_frame();
        for (int i = 1; i <= 3; ++i) { Tex t; t.id = i; c.insert("k" + std::to_string(i), t); }
        c.begin_frame();
        c.touch("k1");                          // k1 most recent, k2 now the oldest
        Tex t4; t4.id = 4; c.insert("k4", t4);  // over cap: evict LRU not used this frame = k2
        CHECK(c.find("k2") == nullptr);
        CHECK(c.size() == 3);
        CHECK(c.graveyard_size() == 1 && g_destroyed.empty());
        c.begin_frame();
        CHECK(g_destroyed[2] == 1);             // destroyed at the next begin_frame, once
    }
    {   // exact graveyard timing
        g_destroyed.clear();
        TexCache<Tex> c(1, kill);
        c.begin_frame();
        Tex a; a.id = 1; c.insert("a", a);
        c.begin_frame();
        Tex b; b.id = 2; c.insert("b", b);      // a is evictable (not used this frame): buried
        CHECK(c.find("a") == nullptr);
        CHECK(g_destroyed.empty());             // not destroyed while this frame may still draw it
        c.begin_frame();
        CHECK(g_destroyed[1] == 1);             // destroyed at the start of the next frame
        CHECK(c.size() == 1);
        // replacing a key buries the old value instead of destroying it
        Tex b2; b2.id = 3; c.insert("b", b2);
        CHECK(g_destroyed[2] == 0);
        c.begin_frame();
        CHECK(g_destroyed[2] == 1);
        // clear_deferred
        c.clear_deferred();
        CHECK(c.size() == 0 && g_destroyed[3] == 0);
        c.begin_frame();
        CHECK(g_destroyed[3] == 1);
        // capacity respected across frames: churn 50 frames with 5 new keys per frame, cap 8
        TexCache<Tex> d(8, kill);
        int id = 100;
        for (int f = 0; f < 50; ++f) {
            d.begin_frame();
            CHECK(d.size() <= 8);
            for (int k = 0; k < 5; ++k) { Tex t; t.id = id; d.insert("c" + std::to_string(id), t); ++id; }
        }
        d.destroy_all();
        CHECK(d.size() == 0 && d.graveyard_size() == 0);
        for (auto& kv : g_destroyed) CHECK(kv.second == 1);
    }
    {   // decode budget
        DecodeBudget b(2);
        int done = 0;
        for (int i = 0; i < 60; ++i) if (b.spend()) ++done;
        CHECK(done == 2);
        b.begin_frame();
        CHECK(b.used() == 0);
        CHECK(b.spend() && b.spend() && !b.spend());
        // 60 gallery items at 2 per frame take 30 frames
        int frames = 0, left = 60;
        while (left > 0) { b.begin_frame(); while (left > 0 && b.spend()) --left; ++frames; }
        CHECK(frames == 30);
    }
    std::printf("tex_cache: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
