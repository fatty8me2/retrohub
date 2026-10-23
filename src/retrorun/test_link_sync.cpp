/* Console test for the link save exchange: 2-4 simulated PCs, lossy/duplicating/reordering links. */
#include <cstdio>
#include <deque>
#include <random>

#include "link_sync.h"

using namespace rlink;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

struct Net {
    struct Pkt { uint32_t due; int from, to; std::vector<uint8_t> d; };
    std::deque<Pkt> q;
    std::mt19937 rng{1234};
    int loss_pct = 0;
};

static bool run(int n, int loss_pct, size_t blob_size) {
    std::mt19937 rng(77);
    std::vector<std::vector<uint8_t>> blobs(n);
    uint32_t mask = 0;
    for (int i = 0; i < n; i++) {
        blobs[i].resize(blob_size + (size_t)i * 13);
        for (auto& b : blobs[i]) b = (uint8_t)rng();
        mask |= 1u << i;
    }
    std::vector<LinkExchange*> ex;
    for (int i = 0; i < n; i++) ex.push_back(new LinkExchange(i, mask, blobs[i]));
    Net net;
    net.loss_pct = loss_pct;
    std::vector<bool> done(n, false);
    bool ok = false;
    for (uint32_t t = 1; t < 60000; t += 2) {
        for (int i = 0; i < n; i++) {
            ex[i]->tick(t, [&](const std::vector<uint8_t>& d) {
                for (int j = 0; j < n; j++) {
                    if (j == i || (int)(net.rng() % 100) < net.loss_pct) continue;
                    net.q.push_back({t + 20 + (uint32_t)(net.rng() % 40), i, j, d});   // jitter => reordering
                    if (net.rng() % 20 == 0) net.q.push_back({t + 30, i, j, d});        // duplicate
                }
            });
            if (ex[i]->finished(t)) done[i] = true;
        }
        for (size_t k = 0; k < net.q.size();) {
            if (net.q[k].due <= t) { ex[net.q[k].to]->on_packet(net.q[k].from, net.q[k].d.data(), net.q[k].d.size()); net.q.erase(net.q.begin() + (long)k); }
            else k++;
        }
        bool all = true;
        for (int i = 0; i < n; i++) all = all && done[i];
        if (all) { ok = true; break; }
    }
    for (int i = 0; ok && i < n; i++)
        for (int j = 0; j < n; j++) ok = ok && ex[i]->have(j) && ex[i]->blob(j) == blobs[j];
    for (auto* e : ex) delete e;
    return ok;
}

int main() {
    CHECK(run(2, 0, 32768));
    CHECK(run(2, 30, 32768));
    CHECK(run(2, 0, 0));        // nobody has a save
    CHECK(run(3, 20, 5000));
    CHECK(run(4, 10, 131072));  // GBA-size saves
    CHECK(blob_hash((const uint8_t*)"ab", 2) != blob_hash((const uint8_t*)"ba", 2));
    printf("%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
